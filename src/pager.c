#include "pager.h"

static void check_and_recover_journal(Pager* pager) {
  struct stat st;
  if (stat(pager->journal_filename, &st) == 0 && st.st_size > 0) {
    int jfd = open(pager->journal_filename, O_RDONLY | O_BINARY);
    if (jfd != -1) {
      uint32_t pnum;
      uint8_t page_buf[PAGE_SIZE];
      while (read(jfd, &pnum, 4) == 4) {
        if (read(jfd, page_buf, PAGE_SIZE) == PAGE_SIZE) {
          lseek(pager->file_descriptor, (off_t)pnum * PAGE_SIZE, SEEK_SET);
          ssize_t w = write(pager->file_descriptor, page_buf, PAGE_SIZE);
          if (w != (ssize_t)PAGE_SIZE) {
            fprintf(stderr, "[RECOVERY] Warning: failed to write recovered page %u\n", pnum);
          }
        }
      }
      close(jfd);
      unlink(pager->journal_filename);
      printf("[RECOVERY] Recovered database from journal '%s'.\n", pager->journal_filename);
    }
  }
}

Pager* pager_open(const char* filename) {
  Pager* pager = malloc(sizeof(Pager));
  memset(pager, 0, sizeof(Pager));

  if (strcmp(filename, ":memory:") == 0) {
    pager->is_memory = true;
    pager->file_descriptor = -1;
    strncpy(pager->main_filename, ":memory:", sizeof(pager->main_filename) - 1);
    pager->max_pages = 32;
    pager->pages = malloc(sizeof(void*) * pager->max_pages);
    for (uint32_t i = 0; i < pager->max_pages; i++) {
      pager->pages[i] = NULL;
    }
    pager->reserved_catalog_pages = 33;
    return pager;
  }

  int fd = open(filename, O_RDWR | O_CREAT | O_BINARY, S_IWUSR | S_IRUSR);
  if (fd == -1) {
    fprintf(stderr, "Unable to open file '%s': %s\n", filename, strerror(errno));
    exit(EXIT_FAILURE);
  }

  pager->file_descriptor = fd;

  strncpy(pager->main_filename, filename, sizeof(pager->main_filename) - 1);
  snprintf(pager->journal_filename, sizeof(pager->journal_filename), "%s-journal", filename);
  snprintf(pager->wal_filename, sizeof(pager->wal_filename), "%s-wal", filename);
  pager->wal_fd = -1;
  pager->use_wal = false;
  pager->wal_frame_size = WAL_FRAME_SIZE_V2;
  pager->wal_lsn = 0;

  struct stat wal_st;
  if (stat(pager->wal_filename, &wal_st) == 0 && wal_st.st_size > 0) {
    pager->use_wal = true;
    pager->wal_fd = open(pager->wal_filename, O_RDWR | O_BINARY, S_IWUSR | S_IRUSR);
    if (pager->wal_fd != -1) {
      uint32_t magic = 0;
      lseek(pager->wal_fd, 0, SEEK_SET);
      if (read(pager->wal_fd, &magic, 4) == 4 && magic == WAL_MAGIC) {
        pager->wal_frame_size = WAL_FRAME_SIZE_V2;
        off_t wal_len = lseek(pager->wal_fd, 0, SEEK_END);
        off_t num_frames = (wal_len >= 4) ? (wal_len - 4) / WAL_FRAME_SIZE_V2 : 0;
        uint64_t max_lsn = 0;
        for (off_t f = 0; f < num_frames; f++) {
          off_t offset = 4 + f * WAL_FRAME_SIZE_V2;
          uint64_t f_lsn = 0;
          lseek(pager->wal_fd, offset + 4 + 4 + 8, SEEK_SET);
          if (read(pager->wal_fd, &f_lsn, 8) == 8) {
            if (f_lsn > max_lsn) max_lsn = f_lsn;
          }
        }
        pager->wal_lsn = max_lsn;
      } else {
        pager->wal_frame_size = WAL_FRAME_SIZE_V1;
        pager->wal_lsn = 0;
      }
    }
  }

  /* Check for uncommitted journal from crash and recover */
  check_and_recover_journal(pager);

  off_t file_length = lseek(fd, 0, SEEK_END);
  if (file_length % PAGE_SIZE != 0) {
    fprintf(stderr, "DB file is not a whole number of pages. Corrupt file.\n");
    exit(EXIT_FAILURE);
  }

  pager->file_length = (uint32_t)file_length;
  pager->num_pages   = (uint32_t)(file_length / PAGE_SIZE);

  /* Initialize page list with resizable space */
  pager->max_pages = 32;
  pager->pages = malloc(sizeof(void*) * pager->max_pages);
  for (uint32_t i = 0; i < pager->max_pages; i++) {
    pager->pages[i] = NULL;
  }

  pager->in_transaction = false;
  pager->journal_fd = -1;
  pager->page_is_journaled = NULL;

  for (int t = 0; t < MAX_TABLES; t++) {
    pthread_rwlock_init(&pager->table_rwlocks[t], NULL);
  }

  return pager;
}

void pager_reset_stats(Pager* pager) {
  if (pager) {
    pager->disk_reads = 0;
    pager->cache_hits = 0;
  }
}

/* IEEE 802.3 32-bit CRC Checksum for WAL Frame Integrity */
static uint32_t calculate_crc32(const uint8_t *data, size_t length) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < length; i++) {
    crc ^= data[i];
    for (int j = 0; j < 8; j++) {
      if (crc & 1) crc = (crc >> 1) ^ 0xEDB88320;
      else crc >>= 1;
    }
  }
  return ~crc;
}

#ifdef _WIN32
static ssize_t pread(int fd, void* buf, size_t count, off_t offset) {
  off_t orig = _lseek(fd, 0, SEEK_CUR);
  if (_lseek(fd, offset, SEEK_SET) == -1) return -1;
  ssize_t res = _read(fd, buf, (unsigned int)count);
  _lseek(fd, orig, SEEK_SET);
  return res;
}

static int lock_file_byte(int fd, off_t offset, short lock_type, bool wait) {
  (void)wait;
  HANDLE h = (HANDLE)_get_osfhandle(fd);
  if (h == INVALID_HANDLE_VALUE) return -1;
  OVERLAPPED ov;
  memset(&ov, 0, sizeof(ov));
  ov.Offset = (DWORD)offset;
  ov.OffsetHigh = (DWORD)((uint64_t)offset >> 32);
  if (lock_type == F_UNLCK) {
    UnlockFileEx(h, 0, 1, 0, &ov);
    return 0;
  } else {
    DWORD flags = (lock_type == F_WRLCK) ? LOCKFILE_EXCLUSIVE_LOCK : 0;
    if (!wait) flags |= LOCKFILE_FAIL_IMMEDIATELY;
    return LockFileEx(h, flags, 0, 1, 0, &ov) ? 0 : -1;
  }
}

static bool is_byte_locked(int fd, off_t offset, short lock_type) {
  (void)lock_type;
  HANDLE h = (HANDLE)_get_osfhandle(fd);
  if (h == INVALID_HANDLE_VALUE) return false;
  OVERLAPPED ov;
  memset(&ov, 0, sizeof(ov));
  ov.Offset = (DWORD)offset;
  ov.OffsetHigh = (DWORD)((uint64_t)offset >> 32);
  if (!LockFileEx(h, LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) return true;
  UnlockFileEx(h, 0, 1, 0, &ov);
  return false;
}
#else
static int lock_file_byte(int fd, off_t offset, short lock_type, bool wait) {
  struct flock lock;
  memset(&lock, 0, sizeof(lock));
  lock.l_type = lock_type;
  lock.l_whence = SEEK_SET;
  lock.l_start = offset;
  lock.l_len = 1;
  
  int cmd = wait ? F_SETLKW : F_SETLK;
  return fcntl(fd, cmd, &lock);
}

static bool is_byte_locked(int fd, off_t offset, short lock_type) {
  struct flock lock;
  memset(&lock, 0, sizeof(lock));
  lock.l_type = lock_type;
  lock.l_whence = SEEK_SET;
  lock.l_start = offset;
  lock.l_len = 1;
  
  if (fcntl(fd, F_GETLK, &lock) == -1) return true;
  return (lock.l_type != F_UNLCK);
}
#endif

#define PENDING_BYTE  0x40000000
#define RESERVED_BYTE 0x40000001
#define SHARED_BYTE   0x40000002

bool pager_lock(Pager* pager, PagerLockState lock_type) {
  if (pager->is_memory) return true;
  if (pager->lock_state >= lock_type) {
    return true; /* Already holds lock at or above requested level */
  }

  /* Transition to SHARED */
  if (pager->lock_state == NO_LOCK && lock_type >= SHARED_LOCK) {
    /* 1. Check if PENDING byte is locked by another process */
    if (is_byte_locked(pager->file_descriptor, PENDING_BYTE, F_WRLCK)) {
      return false; /* Pending lock active, block new readers */
    }
    /* 2. Acquire shared read lock on SHARED byte */
    if (lock_file_byte(pager->file_descriptor, SHARED_BYTE, F_RDLCK, false) == -1) {
      return false;
    }
    pager->lock_state = SHARED_LOCK;
  }

  /* Transition to RESERVED */
  if (pager->lock_state == SHARED_LOCK && lock_type >= RESERVED_LOCK) {
    /* Acquire write lock on RESERVED byte */
    if (lock_file_byte(pager->file_descriptor, RESERVED_BYTE, F_WRLCK, false) == -1) {
      return false;
    }
    pager->lock_state = RESERVED_LOCK;
  }

  /* Transition to PENDING */
  if (pager->lock_state == RESERVED_LOCK && lock_type >= PENDING_LOCK) {
    /* Acquire write lock on PENDING byte */
    if (lock_file_byte(pager->file_descriptor, PENDING_BYTE, F_WRLCK, false) == -1) {
      return false;
    }
    pager->lock_state = PENDING_LOCK;
  }

  /* Transition to EXCLUSIVE */
  if (pager->lock_state == PENDING_LOCK && lock_type >= EXCLUSIVE_LOCK) {
    /* Upgrade SHARED byte to write lock. Only succeeds if there are no other readers. */
    if (lock_file_byte(pager->file_descriptor, SHARED_BYTE, F_WRLCK, false) == -1) {
      return false;
    }
    pager->lock_state = EXCLUSIVE_LOCK;
  }

  return true;
}

void pager_unlock(Pager* pager) {
  if (pager->lock_state == NO_LOCK) return;

  /* Release locks on all bytes */
  lock_file_byte(pager->file_descriptor, SHARED_BYTE, F_UNLCK, false);
  lock_file_byte(pager->file_descriptor, RESERVED_BYTE, F_UNLCK, false);
  lock_file_byte(pager->file_descriptor, PENDING_BYTE, F_UNLCK, false);
  
  pager->lock_state = NO_LOCK;
}

void pager_begin_transaction(Pager* pager) {
  if (pager->in_transaction) {
    return;
  }

  /* Retry up to 500ms to acquire RESERVED lock */
  int retries = 50;
  while (!pager_lock(pager, RESERVED_LOCK)) {
    usleep(10000);
    if (--retries <= 0) {
      /* Could not acquire lock — signal error, do NOT start transaction.
       * This is the safe behaviour: refuse the operation rather than
       * risk concurrent corruption (important for financial data). */
      pager->lock_error = true;
      fprintf(stderr, "Error: Database is locked by another process.\n");
      return;
    }
  }
  pager->lock_error = false;

  /* Flush all cached pages to disk so on-disk state is clean baseline */
  for (uint32_t i = 0; i < pager->max_pages; i++) {
    if (pager->pages[i] != NULL) {
      pager_flush(pager, i);
    }
  }
  pager->num_pages_at_tx_start = pager->num_pages;
  pager->file_length = (uint32_t)lseek(pager->file_descriptor, 0, SEEK_END);

  pager->journal_fd = open(pager->journal_filename, O_RDWR | O_CREAT | O_TRUNC | O_BINARY, S_IWUSR | S_IRUSR);
  if (pager->journal_fd == -1) {
    /* Journal open failed — continue without journaling (no crash) */
    pager->in_transaction = true;
    uint32_t track_size = pager->max_pages + 1024;
    pager->page_is_journaled = calloc(track_size, sizeof(bool));
    return;
  }

  pager->in_transaction = true;
  uint32_t track_size = pager->max_pages + 1024;
  pager->page_is_journaled = calloc(track_size, sizeof(bool));
}

void pager_journal_page(Pager* pager, uint32_t page_num) {
  if (!pager->in_transaction || pager->journal_fd == -1) return;
  if (pager->num_savepoints == 0 && pager->page_is_journaled && page_num < pager->max_pages + 1024 && pager->page_is_journaled[page_num]) return;

  /* Snapshot current pre-mutation in-memory page content to journal in ONE single 4100-byte write */
  uint8_t journal_entry[4 + PAGE_SIZE];
  memcpy(journal_entry, &page_num, 4);

  if (page_num < pager->max_pages && pager->pages[page_num] != NULL) {
    memcpy(journal_entry + 4, pager->pages[page_num], PAGE_SIZE);
  } else {
    off_t file_pages = pager->file_length / PAGE_SIZE;
    if (page_num < (uint32_t)file_pages) {
      ssize_t bytes = pread(pager->file_descriptor, journal_entry + 4, PAGE_SIZE, (off_t)page_num * PAGE_SIZE);
      if (bytes != PAGE_SIZE) memset(journal_entry + 4, 0, PAGE_SIZE);
    } else {
      memset(journal_entry + 4, 0, PAGE_SIZE);
    }
  }

  ssize_t w = write(pager->journal_fd, journal_entry, 4 + PAGE_SIZE);
  if (w != (ssize_t)(4 + PAGE_SIZE)) {
    fprintf(stderr, "pager_journal_page: warning: write to journal failed\n");
  }

  if (pager->page_is_journaled && page_num < pager->max_pages + 1024) {
    pager->page_is_journaled[page_num] = true;
  }
}

void* get_page(Pager* pager, uint32_t page_num) {
  if (!pager->is_memory) {
    /* Retry up to 500ms to acquire shared read lock */
    int retries = 50;
    bool got_lock = false;
    while (!(got_lock = pager_lock(pager, SHARED_LOCK))) {
      usleep(10000);
      if (--retries <= 0) break;
    }
    if (!got_lock) {
      /* Could not acquire read lock — return NULL so callers propagate error
       * rather than reading potentially inconsistent data. */
      pager->lock_error = true;
      fprintf(stderr, "Error: Database is locked (read lock unavailable).\n");
      return NULL;
    }
    pager->lock_error = false;
  }

  /* Dynamic page array resize */
  if (page_num >= pager->max_pages) {
    uint32_t old_max = pager->max_pages;
    while (page_num >= pager->max_pages) {
      pager->max_pages *= 2;
    }
    pager->pages = realloc(pager->pages, sizeof(void*) * pager->max_pages);
    for (uint32_t i = old_max; i < pager->max_pages; i++) {
      pager->pages[i] = NULL;
    }
    if (pager->in_transaction && pager->page_is_journaled) {
      pager->page_is_journaled = realloc(pager->page_is_journaled, sizeof(bool) * (pager->max_pages + 1024));
      for (uint32_t i = old_max + 1024; i < pager->max_pages + 1024; i++) {
        pager->page_is_journaled[i] = false;
      }
    }
    if (pager->is_dirty) {
      pager->is_dirty = realloc(pager->is_dirty, sizeof(bool) * (pager->max_pages + 1024));
      for (uint32_t i = old_max + 1024; i < pager->max_pages + 1024; i++) {
        pager->is_dirty[i] = false;
      }
    }
  }

  if (pager->pages[page_num] == NULL) {
    pager->disk_reads++;
    void* page = malloc(PAGE_SIZE);
    memset(page, 0, PAGE_SIZE);

    bool read_from_wal = false;
    if (pager->use_wal && pager->wal_fd != -1) {
      off_t wal_len = lseek(pager->wal_fd, 0, SEEK_END);
      off_t frame_size = pager->wal_frame_size ? pager->wal_frame_size : WAL_FRAME_SIZE_V1;
      off_t header_offset = (frame_size == WAL_FRAME_SIZE_V2) ? 4 : 0;
      if (wal_len >= header_offset) {
        off_t num_frames = (wal_len - header_offset) / frame_size;
        for (off_t f = num_frames - 1; f >= 0; f--) {
          uint32_t f_pnum, stored_crc;
          off_t offset = header_offset + f * frame_size;
          lseek(pager->wal_fd, offset, SEEK_SET);
          if (read(pager->wal_fd, &f_pnum, 4) == 4 && read(pager->wal_fd, &stored_crc, 4) == 4 && f_pnum == page_num) {
            if (frame_size == WAL_FRAME_SIZE_V2) {
              lseek(pager->wal_fd, 8 + 8, SEEK_CUR); /* skip commit_ts and lsn */
            }
            if (read(pager->wal_fd, page, PAGE_SIZE) == PAGE_SIZE) {
              /* Verify CRC32 checksum to reject corrupt or torn frame writes */
              uint32_t computed_crc = calculate_crc32((const uint8_t*)page, PAGE_SIZE);
              if (computed_crc == stored_crc) {
                read_from_wal = true;
                break;
              }
            }
          }
        }
      }
    }

    if (!read_from_wal) {
      uint32_t file_pages = pager->file_length / PAGE_SIZE;
      if (page_num < file_pages) {
        lseek(pager->file_descriptor, page_num * PAGE_SIZE, SEEK_SET);
        ssize_t bytes_read = read(pager->file_descriptor, page, PAGE_SIZE);
        if (bytes_read < 0) {
          memset(page, 0, PAGE_SIZE);
        }
      }
    }
    pager->pages[page_num] = page;
    if (page_num >= pager->num_pages) pager->num_pages = page_num + 1;
  } else {
    pager->cache_hits++;
  }

  /* Mark dirty if transaction is active */
  if (pager->in_transaction) {
    if (!pager->is_dirty) {
      pager->is_dirty = calloc(pager->max_pages + 1024, sizeof(bool));
    }
    if (page_num < pager->max_pages + 1024) {
      pager->is_dirty[page_num] = true;
    }
  }

  return pager->pages[page_num];
}

void pager_flush(Pager* pager, uint32_t page_num) {
  if (pager->is_memory || pager->file_descriptor == -1) return;
  if (page_num >= pager->max_pages || pager->pages[page_num] == NULL) return;

  if (pager->use_wal) {
    if (pager->wal_fd == -1) {
      pager->wal_fd = open(pager->wal_filename, O_RDWR | O_CREAT | O_BINARY, S_IWUSR | S_IRUSR);
    }
    if (pager->wal_fd != -1) {
      off_t wal_len = lseek(pager->wal_fd, 0, SEEK_END);
      if (wal_len == 0) {
        uint32_t magic = WAL_MAGIC;
        if (write(pager->wal_fd, &magic, 4) != 4) {
          fprintf(stderr, "pager_flush: warning: write WAL magic failed\n");
        }
        pager->wal_frame_size = WAL_FRAME_SIZE_V2;
        pager->wal_lsn = 0;
      } else if (pager->wal_frame_size == 0) {
        uint32_t magic = 0;
        lseek(pager->wal_fd, 0, SEEK_SET);
        if (read(pager->wal_fd, &magic, 4) == 4 && magic == WAL_MAGIC) {
          pager->wal_frame_size = WAL_FRAME_SIZE_V2;
        } else {
          pager->wal_frame_size = WAL_FRAME_SIZE_V1;
        }
        lseek(pager->wal_fd, 0, SEEK_END);
      }

      uint32_t crc = calculate_crc32((const uint8_t*)pager->pages[page_num], PAGE_SIZE);
      lseek(pager->wal_fd, 0, SEEK_END);

      if (pager->wal_frame_size == WAL_FRAME_SIZE_V2) {
        uint64_t commit_ts = pager->current_commit_ts ? pager->current_commit_ts : (uint64_t)time(NULL);
        uint64_t lsn = ++pager->wal_lsn;
        uint8_t frame_buf[WAL_FRAME_SIZE_V2];
        memcpy(frame_buf, &page_num, 4);
        memcpy(frame_buf + 4, &crc, 4);
        memcpy(frame_buf + 8, &commit_ts, 8);
        memcpy(frame_buf + 16, &lsn, 8);
        memcpy(frame_buf + 24, pager->pages[page_num], PAGE_SIZE);
        ssize_t w = write(pager->wal_fd, frame_buf, sizeof(frame_buf));
        if (w != (ssize_t)sizeof(frame_buf)) {
          fprintf(stderr, "pager_flush: warning: write to WAL failed\n");
        }
      } else {
        uint8_t frame_buf[WAL_FRAME_SIZE_V1];
        memcpy(frame_buf, &page_num, 4);
        memcpy(frame_buf + 4, &crc, 4);
        memcpy(frame_buf + 8, pager->pages[page_num], PAGE_SIZE);
        ssize_t w = write(pager->wal_fd, frame_buf, sizeof(frame_buf));
        if (w != (ssize_t)sizeof(frame_buf)) {
          fprintf(stderr, "pager_flush: warning: write to WAL failed\n");
        }
      }
      return;
    }
  }

  ssize_t bytes = pwrite(pager->file_descriptor, pager->pages[page_num], PAGE_SIZE, (off_t)page_num * PAGE_SIZE);
  if (bytes != (ssize_t)PAGE_SIZE) {
    fprintf(stderr, "pager_flush: warning: pwrite failed for page %u\n", page_num);
    return;
  }
  if ((page_num + 1) * PAGE_SIZE > pager->file_length) {
    pager->file_length = (page_num + 1) * PAGE_SIZE;
  }
  if (pager->is_dirty && page_num < pager->max_pages + 1024) {
    pager->is_dirty[page_num] = false;
  }
}

uint32_t get_unused_page_num(Pager* pager) {
  if (pager->freelist_head != 0 && pager->freelist_head != INVALID_PAGE_NUM && pager->freelist_head >= pager->reserved_catalog_pages) {
    uint32_t reused_page = pager->freelist_head;
    void* free_page_buf = get_page(pager, reused_page);
    uint32_t next_free = 0;
    memcpy(&next_free, free_page_buf, 4);
    pager->freelist_head = next_free;
    memset(free_page_buf, 0, PAGE_SIZE);
    
    void* p0 = get_page(pager, 0);
    memcpy((uint8_t*)p0 + 4092, &pager->freelist_head, 4);
    return reused_page;
  }
  if (pager->num_pages < pager->reserved_catalog_pages) {
    return pager->reserved_catalog_pages;
  }
  return pager->num_pages;
}

void pager_free_page(Pager* pager, uint32_t page_num) {
  if (page_num < pager->reserved_catalog_pages || page_num == 0 || page_num == INVALID_PAGE_NUM) {
    return;
  }
  void* page_buf = get_page(pager, page_num);
  memset(page_buf, 0, PAGE_SIZE);
  memcpy(page_buf, &pager->freelist_head, 4);
  pager->freelist_head = page_num;

  void* p0 = get_page(pager, 0);
  memcpy((uint8_t*)p0 + 4092, &pager->freelist_head, 4);
}

void pager_commit(Pager* pager) {
  if (!pager->in_transaction) {
    printf("No active transaction to commit.\n");
    return;
  }

  /* Upgrade lock to EXCLUSIVE before flushing dirty pages to disk */
  int retries = 50;
  while (!pager_lock(pager, EXCLUSIVE_LOCK)) {
    usleep(10000);
    if (--retries <= 0) {
      fprintf(stderr, "Error: Database is locked (exclusive lock timeout for commit).\n");
      exit(1);
    }
  }

  if (pager->journal_fd != -1) {
    fdatasync(pager->journal_fd);
  }

  /* Flush ONLY dirty cached pages to disk */
  pager->current_commit_ts = (uint64_t)time(NULL);
  for (uint32_t i = 0; i < pager->num_pages; i++) {
    if (i < pager->max_pages && pager->pages[i]) {
      if (pager->is_dirty == NULL || pager->is_dirty[i]) {
        pager_flush(pager, i);
      }
    }
  }
  pager->current_commit_ts = 0;

  if (pager->use_wal && pager->wal_fd != -1) {
    fdatasync(pager->wal_fd);
  } else if (pager->file_descriptor != -1) {
    fdatasync(pager->file_descriptor);
  }

  if (pager->journal_fd != -1) {
    close(pager->journal_fd);
    pager->journal_fd = -1;
  }
  unlink(pager->journal_filename);

  if (pager->page_is_journaled) {
    free(pager->page_is_journaled);
    pager->page_is_journaled = NULL;
  }
  if (pager->is_dirty) {
    free(pager->is_dirty);
    pager->is_dirty = NULL;
  }

  pager->in_transaction = false;
  pager_unlock(pager);
  printf("Transaction committed.\n");
}

void pager_rollback(Pager* pager) {
  if (!pager->in_transaction) {
    printf("No active transaction to rollback.\n");
    return;
  }

  /* Upgrade lock to EXCLUSIVE before restoring baseline pages */
  int retries = 50;
  while (!pager_lock(pager, EXCLUSIVE_LOCK)) {
    usleep(10000);
    if (--retries <= 0) {
      fprintf(stderr, "Error: Database is locked (exclusive lock timeout for rollback).\n");
      exit(1);
    }
  }

  if (pager->journal_fd != -1) {
    close(pager->journal_fd);
    pager->journal_fd = -1;
  }

  int jfd = open(pager->journal_filename, O_RDONLY | O_BINARY);
  if (jfd != -1) {
    uint32_t pnum;
    uint8_t page_buf[PAGE_SIZE];
    while (read(jfd, &pnum, 4) == 4) {
      if (read(jfd, page_buf, PAGE_SIZE) == PAGE_SIZE) {
        if (pnum < pager->max_pages && pager->pages[pnum]) {
          memcpy(pager->pages[pnum], page_buf, PAGE_SIZE);
        }
        lseek(pager->file_descriptor, (off_t)pnum * PAGE_SIZE, SEEK_SET);
        ssize_t w = write(pager->file_descriptor, page_buf, PAGE_SIZE);
        if (w != (ssize_t)PAGE_SIZE) {
          fprintf(stderr, "pager_rollback: warning: write failed for page %u\n", pnum);
        }
      }
    }
    close(jfd);
    unlink(pager->journal_filename);
  }

  if (pager->page_is_journaled) {
    free(pager->page_is_journaled);
    pager->page_is_journaled = NULL;
  }
  if (pager->is_dirty) {
    free(pager->is_dirty);
    pager->is_dirty = NULL;
  }

  /* Discard any newly allocated in-memory pages beyond baseline */
  if (pager->num_pages_at_tx_start > 0 && pager->num_pages > pager->num_pages_at_tx_start) {
    for (uint32_t p = pager->num_pages_at_tx_start; p < pager->num_pages; p++) {
      if (p < pager->max_pages && pager->pages[p]) {
        free(pager->pages[p]);
        pager->pages[p] = NULL;
      }
    }
    pager->num_pages = pager->num_pages_at_tx_start;
    pager->file_length = pager->num_pages * PAGE_SIZE;
    if (pager->file_descriptor != -1 && !pager->is_memory) {
      if (ftruncate(pager->file_descriptor, (off_t)pager->file_length) != 0) {
        fprintf(stderr, "pager_rollback: warning: ftruncate failed\n");
      }
    }
  }

  pager->in_transaction = false;
  pager->num_savepoints = 0;
  pager_unlock(pager);
  printf("Transaction rolled back.\n");
}

void pager_savepoint(Pager* pager, const char* name) {
  if (!pager->in_transaction) {
    pager_begin_transaction(pager);
  }
  if (pager->num_savepoints >= 16) {
    printf("Error: Savepoint limit reached (max 16).\n");
    return;
  }
  uint32_t idx = pager->num_savepoints++;
  strncpy(pager->savepoints[idx].name, name, sizeof(pager->savepoints[idx].name) - 1);
  pager->savepoints[idx].journal_offset = (pager->journal_fd != -1) ? lseek(pager->journal_fd, 0, SEEK_END) : 0;
  pager->savepoints[idx].num_pages_at_savepoint = pager->num_pages;
  printf("Savepoint '%s' created.\n", name);
}

void pager_rollback_to_savepoint(Pager* pager, const char* name) {
  if (!pager->in_transaction) {
    printf("No active transaction for savepoint rollback.\n");
    return;
  }
  int idx = -1;
  for (int i = (int)pager->num_savepoints - 1; i >= 0; i--) {
    if (strcmp(pager->savepoints[i].name, name) == 0) {
      idx = i;
      break;
    }
  }
  if (idx == -1) {
    printf("Error: Savepoint '%s' not found.\n", name);
    return;
  }

  off_t target_offset = pager->savepoints[idx].journal_offset;
  int jfd = open(pager->journal_filename, O_RDWR | O_BINARY);
  if (jfd != -1) {
    off_t current_len = lseek(jfd, 0, SEEK_END);
    if (current_len > target_offset) {
      off_t entry_size = 4 + PAGE_SIZE;
      off_t pos = current_len;

      while (pos > target_offset) {
        pos -= entry_size;
        lseek(jfd, pos, SEEK_SET);
        uint32_t pnum;
        uint8_t page_buf[PAGE_SIZE];
        if (read(jfd, &pnum, 4) == 4 && read(jfd, page_buf, PAGE_SIZE) == PAGE_SIZE) {
          if (pnum < pager->max_pages && pager->pages[pnum]) {
            memcpy(pager->pages[pnum], page_buf, PAGE_SIZE);
          }
          lseek(pager->file_descriptor, (off_t)pnum * PAGE_SIZE, SEEK_SET);
          ssize_t w = write(pager->file_descriptor, page_buf, PAGE_SIZE);
          if (w != (ssize_t)PAGE_SIZE) {
            fprintf(stderr, "pager_rollback_to_savepoint: warning: write failed for page %u\n", pnum);
          }
        }
      }
      if (ftruncate(jfd, target_offset) != 0) {
        fprintf(stderr, "pager_rollback_to_savepoint: warning: journal ftruncate failed\n");
      }
    }
    close(jfd);
  }

  uint32_t target_pages = pager->savepoints[idx].num_pages_at_savepoint;
  if (target_pages > 0 && pager->num_pages > target_pages) {
    for (uint32_t p = target_pages; p < pager->num_pages; p++) {
      if (p < pager->max_pages && pager->pages[p]) {
        free(pager->pages[p]);
        pager->pages[p] = NULL;
      }
    }
    pager->num_pages = target_pages;
    pager->file_length = pager->num_pages * PAGE_SIZE;
    if (pager->file_descriptor != -1 && !pager->is_memory) {
      if (ftruncate(pager->file_descriptor, (off_t)pager->file_length) != 0) {
        fprintf(stderr, "pager_rollback_to_savepoint: warning: ftruncate failed\n");
      }
    }
  } else {
    pager->num_pages = target_pages;
  }
  pager->num_savepoints = idx + 1;
  printf("Rolled back to savepoint '%s'.\n", name);
}

void pager_release_savepoint(Pager* pager, const char* name) {
  if (!pager->in_transaction) {
    printf("No active transaction for savepoint release.\n");
    return;
  }
  int idx = -1;
  for (int i = (int)pager->num_savepoints - 1; i >= 0; i--) {
    if (strcmp(pager->savepoints[i].name, name) == 0) {
      idx = i;
      break;
    }
  }
  if (idx == -1) {
    printf("Error: Savepoint '%s' not found.\n", name);
    return;
  }
  pager->num_savepoints = idx;
  printf("Savepoint '%s' released.\n", name);
}

void pager_close(Pager* pager) {
  if (pager->is_memory) {
    for (int t = 0; t < MAX_TABLES; t++) {
      pthread_rwlock_destroy(&pager->table_rwlocks[t]);
    }
    for (uint32_t i = 0; i < pager->max_pages; i++) {
      if (pager->pages[i]) {
        free(pager->pages[i]);
        pager->pages[i] = NULL;
      }
    }
    free(pager->pages);
    free(pager);
    return;
  }
  if (pager->in_transaction) {
    pager_commit(pager);
  }

  for (uint32_t i = 0; i < pager->max_pages; i++) {
    if (pager->pages[i] != NULL) {
      if (!pager->use_wal) {
        if (pager->is_dirty == NULL || pager->is_dirty[i]) {
          pager_flush(pager, i);
        }
      } else {
        if (pager->is_dirty && pager->is_dirty[i]) {
          pager_flush(pager, i);
        }
      }
      free(pager->pages[i]);
      pager->pages[i] = NULL;
    }
  }
  fdatasync(pager->file_descriptor);
  pager_unlock(pager);
  for (int t = 0; t < MAX_TABLES; t++) {
    pthread_rwlock_destroy(&pager->table_rwlocks[t]);
  }
  if (pager->page_is_journaled) {
    free(pager->page_is_journaled);
    pager->page_is_journaled = NULL;
  }
  if (pager->is_dirty) {
    free(pager->is_dirty);
    pager->is_dirty = NULL;
  }
  if (pager->wal_fd != -1) {
    close(pager->wal_fd);
  }
  close(pager->file_descriptor);
  if (pager->pages) {
    free(pager->pages);
  }
  free(pager);
}

void pager_set_wal_mode(Pager* pager, bool enable_wal) {
  if (enable_wal) {
    if (pager->use_wal) return;
    pager->use_wal = true;
    if (pager->wal_fd == -1) {
      pager->wal_fd = open(pager->wal_filename, O_RDWR | O_CREAT | O_BINARY, S_IWUSR | S_IRUSR);
    }
    printf("[WAL] Journal mode set to WAL mode.\n");
  } else {
    if (pager->use_wal) {
      pager_checkpoint(pager);
    }
    if (pager->wal_fd != -1) {
      close(pager->wal_fd);
      pager->wal_fd = -1;
    }
    unlink(pager->wal_filename);
    pager->use_wal = false;
    printf("[WAL] Journal mode set to Rollback (DELETE) mode.\n");
  }
}

void pager_checkpoint(Pager* pager) {
  if (pager->wal_fd == -1) {
    pager->wal_fd = open(pager->wal_filename, O_RDWR | O_CREAT | O_BINARY, S_IWUSR | S_IRUSR);
  }
  if (pager->wal_fd == -1) return;

  off_t wal_len = lseek(pager->wal_fd, 0, SEEK_END);
  off_t frame_size = pager->wal_frame_size ? pager->wal_frame_size : WAL_FRAME_SIZE_V1;
  off_t header_offset = (frame_size == WAL_FRAME_SIZE_V2) ? 4 : 0;
  off_t num_frames = (wal_len >= header_offset) ? (wal_len - header_offset) / frame_size : 0;

  if (num_frames > 0) {
    uint8_t page_buf[PAGE_SIZE];
    for (off_t f = 0; f < num_frames; f++) {
      uint32_t f_pnum, stored_crc;
      off_t offset = header_offset + f * frame_size;
      lseek(pager->wal_fd, offset, SEEK_SET);
      if (read(pager->wal_fd, &f_pnum, 4) == 4 && read(pager->wal_fd, &stored_crc, 4) == 4) {
        if (frame_size == WAL_FRAME_SIZE_V2) {
          lseek(pager->wal_fd, 8 + 8, SEEK_CUR); /* skip commit_ts and lsn */
        }
        if (read(pager->wal_fd, page_buf, PAGE_SIZE) == PAGE_SIZE) {
          uint32_t computed_crc = calculate_crc32(page_buf, PAGE_SIZE);
          if (computed_crc == stored_crc) {
            ssize_t bytes = pwrite(pager->file_descriptor, page_buf, PAGE_SIZE, (off_t)f_pnum * PAGE_SIZE);
            if (bytes != (ssize_t)PAGE_SIZE) {
              fprintf(stderr, "pager_checkpoint: warning: pwrite failed for frame page %u\n", f_pnum);
            }
            if ((f_pnum + 1) * PAGE_SIZE > pager->file_length) {
              pager->file_length = (f_pnum + 1) * PAGE_SIZE;
            }
            if (f_pnum >= pager->num_pages) {
              pager->num_pages = f_pnum + 1;
            }
            if (f_pnum < pager->max_pages && pager->pages[f_pnum]) {
              memcpy(pager->pages[f_pnum], page_buf, PAGE_SIZE);
            }
          }
        }
      }
    }
    fdatasync(pager->file_descriptor);
    if (ftruncate(pager->wal_fd, 0) != 0) {
      fprintf(stderr, "pager_checkpoint: warning: WAL ftruncate failed\n");
    }
    lseek(pager->wal_fd, 0, SEEK_SET);
    fsync(pager->wal_fd);
    pager->wal_lsn = 0;
    pager->wal_frame_size = WAL_FRAME_SIZE_V2;
  }

  for (uint32_t i = 0; i < pager->num_pages; i++) {
    if (i < pager->max_pages && pager->pages[i]) {
      free(pager->pages[i]);
      pager->pages[i] = NULL;
    }
  }

  printf("[WAL] Checkpoint completed. All frames written to main database file.\n");
}

void pager_backup(Pager* pager, const char* dest_filename) {
  if (pager->is_memory) {
    fprintf(stderr, "Error: Cannot backup in-memory database.\n");
    return;
  }

  /* 1. Acquire EXCLUSIVE lock on pager (blocks all writers for the duration) */
  int retries = 50;
  while (!pager_lock(pager, EXCLUSIVE_LOCK)) {
    usleep(10000);
    if (--retries <= 0) {
      fprintf(stderr, "Error: Database is locked (exclusive lock timeout for backup).\n");
      return;
    }
  }

  /* 2. Write all dirty in-memory pages to WAL (or main file if not WAL) so on-disk state is current.
   * Do NOT checkpoint! */
  if (pager->is_dirty) {
    for (uint32_t i = 0; i < pager->num_pages; i++) {
      if (i < pager->max_pages && pager->pages[i]) {
        if (pager->is_dirty[i]) {
          pager_flush(pager, i);
        }
      }
    }
  } else if (!pager->use_wal) {
    for (uint32_t i = 0; i < pager->num_pages; i++) {
      if (i < pager->max_pages && pager->pages[i]) {
        pager_flush(pager, i);
      }
    }
  }

  /* 3. fdatasync to ensure durable state */
  if (pager->use_wal && pager->wal_fd != -1) {
    fdatasync(pager->wal_fd);
  } else if (pager->file_descriptor != -1) {
    fdatasync(pager->file_descriptor);
  }

  char dest_tmp[600];
  char dest_wal[600];
  char dest_wal_tmp[600];
  snprintf(dest_tmp, sizeof(dest_tmp), "%s.tmp", dest_filename);
  snprintf(dest_wal, sizeof(dest_wal), "%s-wal", dest_filename);
  snprintf(dest_wal_tmp, sizeof(dest_wal_tmp), "%s-wal.tmp", dest_filename);

  /* 4. Open dest.tmp */
  int dest_fd = open(dest_tmp, O_RDWR | O_CREAT | O_TRUNC | O_BINARY, S_IWUSR | S_IRUSR);
  if (dest_fd == -1) {
    fprintf(stderr, "Error: Unable to create backup file '%s': %s\n", dest_tmp, strerror(errno));
    pager_unlock(pager);
    return;
  }

  /* 5. Copy main file page-by-page into dest.tmp */
  off_t file_len = lseek(pager->file_descriptor, 0, SEEK_END);
  off_t total_pages = file_len / PAGE_SIZE;
  uint8_t page_buf[PAGE_SIZE];
  bool copy_failed = false;

  for (off_t p = 0; p < total_pages; p++) {
    lseek(pager->file_descriptor, p * PAGE_SIZE, SEEK_SET);
    if (read(pager->file_descriptor, page_buf, PAGE_SIZE) == PAGE_SIZE) {
      if (write(dest_fd, page_buf, PAGE_SIZE) != PAGE_SIZE) {
        copy_failed = true;
        break;
      }
    } else {
      copy_failed = true;
      break;
    }
  }

  fdatasync(dest_fd);
  close(dest_fd);

  if (copy_failed) {
    fprintf(stderr, "Error: Failed copying main database file to backup.\n");
    unlink(dest_tmp);
    pager_unlock(pager);
    return;
  }

  if (rename(dest_tmp, dest_filename) != 0) {
    fprintf(stderr, "Error: Failed to rename backup file '%s': %s\n", dest_filename, strerror(errno));
    unlink(dest_tmp);
    pager_unlock(pager);
    return;
  }

  /* 8. Copy WAL verbatim if WAL mode is active and has data */
  uint32_t captured_frames = 0;
  if (pager->use_wal && pager->wal_fd != -1) {
    off_t wal_len = lseek(pager->wal_fd, 0, SEEK_END);
    if (wal_len > 0) {
      int dest_wal_fd = open(dest_wal_tmp, O_RDWR | O_CREAT | O_TRUNC | O_BINARY, S_IWUSR | S_IRUSR);
      if (dest_wal_fd != -1) {
        lseek(pager->wal_fd, 0, SEEK_SET);
        char copy_buf[4096];
        ssize_t nread;
        bool wal_copy_failed = false;
        while ((nread = read(pager->wal_fd, copy_buf, sizeof(copy_buf))) > 0) {
          if (write(dest_wal_fd, copy_buf, (size_t)nread) != nread) {
            wal_copy_failed = true;
            break;
          }
        }
        fdatasync(dest_wal_fd);
        close(dest_wal_fd);

        if (!wal_copy_failed) {
          rename(dest_wal_tmp, dest_wal);
          off_t frame_size = pager->wal_frame_size ? pager->wal_frame_size : WAL_FRAME_SIZE_V1;
          off_t header_offset = (frame_size == WAL_FRAME_SIZE_V2) ? 4 : 0;
          if (wal_len >= header_offset) {
            captured_frames = (uint32_t)((wal_len - header_offset) / frame_size);
          }
        } else {
          unlink(dest_wal_tmp);
        }
      }
    } else {
      unlink(dest_wal);
    }
  } else {
    unlink(dest_wal);
  }

  pager_unlock(pager);
  printf("Backup completed to '%s' (%u WAL frames captured).\n", dest_filename, captured_frames);
}

bool pager_restore(const char* src_file, const char* dest_file,
                   uint64_t until_ts, bool use_ts,
                   uint64_t until_lsn, bool use_lsn) {
  struct stat st;
  if (stat(dest_file, &st) == 0) {
    printf("Error: '%s' already exists. Remove it first or choose a different output path.\n", dest_file);
    return false;
  }

  int src_fd = open(src_file, O_RDONLY | O_BINARY);
  if (src_fd == -1) {
    fprintf(stderr, "Error: Unable to open source backup file '%s': %s\n", src_file, strerror(errno));
    return false;
  }

  char src_wal[600];
  snprintf(src_wal, sizeof(src_wal), "%s-wal", src_file);
  int wal_fd = open(src_wal, O_RDONLY | O_BINARY);
  off_t wal_len = (wal_fd != -1) ? lseek(wal_fd, 0, SEEK_END) : 0;

  if (use_ts || use_lsn) {
    if (wal_fd == -1 || wal_len == 0) {
      printf("Error: backup WAL is empty or missing; UNTIL is not supported. Omit UNTIL to perform a full restore.\n");
      if (wal_fd != -1) close(wal_fd);
      close(src_fd);
      return false;
    }
  }

  bool is_v2 = false;
  uint32_t magic = 0;
  if (wal_fd != -1 && wal_len >= 4) {
    lseek(wal_fd, 0, SEEK_SET);
    if (read(wal_fd, &magic, 4) == 4 && magic == WAL_MAGIC) {
      is_v2 = true;
    }
  }

  if ((use_ts || use_lsn) && !is_v2) {
    printf("Error: backup WAL is v1 format; UNTIL is not supported. Omit UNTIL to perform a full restore.\n");
    if (wal_fd != -1) close(wal_fd);
    close(src_fd);
    return false;
  }

  off_t frame_size = is_v2 ? WAL_FRAME_SIZE_V2 : WAL_FRAME_SIZE_V1;
  off_t header_offset = is_v2 ? 4 : 0;
  off_t num_frames = (wal_fd != -1 && wal_len >= header_offset) ? (wal_len - header_offset) / frame_size : 0;

  if ((use_ts || use_lsn) && num_frames == 0) {
    printf("Error: backup WAL has no frames; UNTIL is not supported.\n");
    if (wal_fd != -1) close(wal_fd);
    close(src_fd);
    return false;
  }

  if (is_v2 && num_frames > 0 && (use_ts || use_lsn)) {
    uint64_t earliest_ts = 0, earliest_lsn = 0;
    lseek(wal_fd, header_offset + 4 + 4, SEEK_SET);
    if (read(wal_fd, &earliest_ts, 8) == 8 && read(wal_fd, &earliest_lsn, 8) == 8) {
      if (use_ts && until_ts < earliest_ts) {
        printf("Error: requested UNTIL timestamp %lu predates the earliest WAL frame in this backup (earliest: ts=%lu, lsn=%lu). No partial restore possible — consider restoring without UNTIL for the base snapshot.\n",
               (unsigned long)until_ts, (unsigned long)earliest_ts, (unsigned long)earliest_lsn);
        close(wal_fd);
        close(src_fd);
        return false;
      }
      if (use_lsn && until_lsn < earliest_lsn) {
        printf("Error: requested UNTIL LSN %lu predates the earliest WAL frame in this backup (earliest: ts=%lu, lsn=%lu). No partial restore possible — consider restoring without UNTIL for the base snapshot.\n",
               (unsigned long)until_lsn, (unsigned long)earliest_ts, (unsigned long)earliest_lsn);
        close(wal_fd);
        close(src_fd);
        return false;
      }
    }
  }

  typedef struct PageMapNode {
    uint32_t pnum;
    off_t    data_offset;
    struct PageMapNode* next;
  } PageMapNode;

  #define PAGE_MAP_BUCKETS 1024
  PageMapNode* buckets[PAGE_MAP_BUCKETS];
  memset(buckets, 0, sizeof(buckets));

  uint64_t max_applied_ts = 0;
  uint64_t max_applied_lsn = 0;
  uint8_t page_buf[PAGE_SIZE];

  for (off_t f = 0; f < num_frames; f++) {
    off_t frame_offset = header_offset + f * frame_size;
    uint32_t f_pnum, stored_crc;
    uint64_t frame_ts = 0, frame_lsn = 0;

    lseek(wal_fd, frame_offset, SEEK_SET);
    if (read(wal_fd, &f_pnum, 4) != 4 || read(wal_fd, &stored_crc, 4) != 4) {
      break;
    }
    if (is_v2) {
      if (read(wal_fd, &frame_ts, 8) != 8 || read(wal_fd, &frame_lsn, 8) != 8) {
        break;
      }
      if (use_ts && frame_ts > until_ts) {
        break;
      }
      if (use_lsn && frame_lsn > until_lsn) {
        break;
      }
    }
    off_t data_offset = frame_offset + (is_v2 ? (4 + 4 + 8 + 8) : (4 + 4));
    if (read(wal_fd, page_buf, PAGE_SIZE) != PAGE_SIZE) {
      break;
    }
    uint32_t computed_crc = calculate_crc32(page_buf, PAGE_SIZE);
    if (computed_crc != stored_crc) {
      continue;
    }

    if (is_v2) {
      if (frame_ts > max_applied_ts) max_applied_ts = frame_ts;
      if (frame_lsn > max_applied_lsn) max_applied_lsn = frame_lsn;
    }

    uint32_t b = f_pnum % PAGE_MAP_BUCKETS;
    PageMapNode* node = buckets[b];
    while (node) {
      if (node->pnum == f_pnum) {
        node->data_offset = data_offset;
        break;
      }
      node = node->next;
    }
    if (!node) {
      node = malloc(sizeof(PageMapNode));
      node->pnum = f_pnum;
      node->data_offset = data_offset;
      node->next = buckets[b];
      buckets[b] = node;
    }
  }

  char dest_tmp[600];
  snprintf(dest_tmp, sizeof(dest_tmp), "%s.tmp", dest_file);
  int dest_fd = open(dest_tmp, O_RDWR | O_CREAT | O_TRUNC | O_BINARY, S_IWUSR | S_IRUSR);
  if (dest_fd == -1) {
    fprintf(stderr, "Error: Unable to create restored file '%s': %s\n", dest_tmp, strerror(errno));
    for (int b = 0; b < PAGE_MAP_BUCKETS; b++) {
      PageMapNode* node = buckets[b];
      while (node) {
        PageMapNode* tmp = node->next;
        free(node);
        node = tmp;
      }
    }
    if (wal_fd != -1) close(wal_fd);
    close(src_fd);
    return false;
  }

  off_t src_len = lseek(src_fd, 0, SEEK_END);
  off_t total_src_pages = src_len / PAGE_SIZE;
  bool copy_failed = false;

  for (off_t p = 0; p < total_src_pages; p++) {
    lseek(src_fd, p * PAGE_SIZE, SEEK_SET);
    if (read(src_fd, page_buf, PAGE_SIZE) == PAGE_SIZE) {
      if (write(dest_fd, page_buf, PAGE_SIZE) != PAGE_SIZE) {
        copy_failed = true;
        break;
      }
    } else {
      copy_failed = true;
      break;
    }
  }

  if (!copy_failed) {
    for (int b = 0; b < PAGE_MAP_BUCKETS; b++) {
      PageMapNode* node = buckets[b];
      while (node) {
        lseek(wal_fd, node->data_offset, SEEK_SET);
        if (read(wal_fd, page_buf, PAGE_SIZE) == PAGE_SIZE) {
          lseek(dest_fd, (off_t)node->pnum * PAGE_SIZE, SEEK_SET);
          if (write(dest_fd, page_buf, PAGE_SIZE) != PAGE_SIZE) {
            copy_failed = true;
            break;
          }
        }
        node = node->next;
      }
      if (copy_failed) break;
    }
  }

  for (int b = 0; b < PAGE_MAP_BUCKETS; b++) {
    PageMapNode* node = buckets[b];
    while (node) {
      PageMapNode* tmp = node->next;
      free(node);
      node = tmp;
    }
  }

  fdatasync(dest_fd);
  close(dest_fd);
  if (wal_fd != -1) close(wal_fd);
  close(src_fd);

  if (copy_failed) {
    fprintf(stderr, "Error: Failed writing restored database.\n");
    unlink(dest_tmp);
    return false;
  }

  if (rename(dest_tmp, dest_file) != 0) {
    fprintf(stderr, "Error: Failed to rename restored file to '%s': %s\n", dest_file, strerror(errno));
    unlink(dest_tmp);
    return false;
  }

  if (is_v2) {
    printf("Restored to LSN %lu (timestamp %lu) -> '%s'.\n", (unsigned long)max_applied_lsn, (unsigned long)max_applied_ts, dest_file);
  } else {
    printf("Restored -> '%s'.\n", dest_file);
  }
  return true;
}
