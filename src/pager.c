#include "pager.h"

static void check_and_recover_journal(Pager* pager) {
  struct stat st;
  if (stat(pager->journal_filename, &st) == 0 && st.st_size > 0) {
    int jfd = open(pager->journal_filename, O_RDONLY);
    if (jfd != -1) {
      uint32_t pnum;
      uint8_t page_buf[PAGE_SIZE];
      while (read(jfd, &pnum, 4) == 4) {
        if (read(jfd, page_buf, PAGE_SIZE) == PAGE_SIZE) {
          lseek(pager->file_descriptor, (off_t)pnum * PAGE_SIZE, SEEK_SET);
          write(pager->file_descriptor, page_buf, PAGE_SIZE);
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

  int fd = open(filename, O_RDWR | O_CREAT, S_IWUSR | S_IRUSR);
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

  struct stat wal_st;
  if (stat(pager->wal_filename, &wal_st) == 0 && wal_st.st_size > 0) {
    pager->use_wal = true;
    pager->wal_fd = open(pager->wal_filename, O_RDWR, S_IWUSR | S_IRUSR);
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

  return pager;
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

  pager->journal_fd = open(pager->journal_filename, O_RDWR | O_CREAT | O_TRUNC, S_IWUSR | S_IRUSR);
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
  (void)w;

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
      for (uint32_t i = old_max; i < pager->max_pages + 1024; i++) {
        pager->page_is_journaled[i] = false;
      }
    }
    if (pager->is_dirty) {
      pager->is_dirty = realloc(pager->is_dirty, sizeof(bool) * (pager->max_pages + 1024));
      for (uint32_t i = old_max; i < pager->max_pages + 1024; i++) {
        pager->is_dirty[i] = false;
      }
    }
  }

  if (pager->pages[page_num] == NULL) {
    void* page = malloc(PAGE_SIZE);
    memset(page, 0, PAGE_SIZE);

    bool read_from_wal = false;
    if (pager->use_wal && pager->wal_fd != -1) {
      off_t wal_len = lseek(pager->wal_fd, 0, SEEK_END);
      off_t frame_size = 4 + 4 + PAGE_SIZE; /* pnum + crc32 + page_data */
      off_t num_frames = wal_len / frame_size;
      for (off_t f = num_frames - 1; f >= 0; f--) {
        uint32_t f_pnum, stored_crc;
        lseek(pager->wal_fd, f * frame_size, SEEK_SET);
        if (read(pager->wal_fd, &f_pnum, 4) == 4 && read(pager->wal_fd, &stored_crc, 4) == 4 && f_pnum == page_num) {
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
      pager->wal_fd = open(pager->wal_filename, O_RDWR | O_CREAT, S_IWUSR | S_IRUSR);
    }
    if (pager->wal_fd != -1) {
      uint32_t crc = calculate_crc32((const uint8_t*)pager->pages[page_num], PAGE_SIZE);
      uint8_t frame_buf[4 + 4 + PAGE_SIZE];
      memcpy(frame_buf, &page_num, 4);
      memcpy(frame_buf + 4, &crc, 4);
      memcpy(frame_buf + 8, pager->pages[page_num], PAGE_SIZE);
      lseek(pager->wal_fd, 0, SEEK_END);
      ssize_t w = write(pager->wal_fd, frame_buf, sizeof(frame_buf));
      (void)w;
      /* No fdatasync here — batched once per transaction by the caller
       * (pager_commit), not once per page. Fsyncing every individual page
       * flush turns an O(1)-syncs-per-transaction commit into O(pages),
       * which measured as a 5x throughput regression on bulk inserts. */
      return;
    }
  }

  ssize_t bytes = pwrite(pager->file_descriptor, pager->pages[page_num], PAGE_SIZE, (off_t)page_num * PAGE_SIZE);
  if (bytes == -1) {
    /* Don't exit on Android — just report */
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
  for (uint32_t i = 0; i < pager->num_pages; i++) {
    if (i < pager->max_pages && pager->pages[i]) {
      if (pager->is_dirty == NULL || pager->is_dirty[i]) {
        pager_flush(pager, i);
      }
    }
  }

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

  int jfd = open(pager->journal_filename, O_RDONLY);
  if (jfd != -1) {
    uint32_t pnum;
    uint8_t page_buf[PAGE_SIZE];
    while (read(jfd, &pnum, 4) == 4) {
      if (read(jfd, page_buf, PAGE_SIZE) == PAGE_SIZE) {
        if (pnum < pager->max_pages && pager->pages[pnum]) {
          memcpy(pager->pages[pnum], page_buf, PAGE_SIZE);
        }
        lseek(pager->file_descriptor, (off_t)pnum * PAGE_SIZE, SEEK_SET);
        write(pager->file_descriptor, page_buf, PAGE_SIZE);
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
      ftruncate(pager->file_descriptor, (off_t)pager->file_length);
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
  int jfd = open(pager->journal_filename, O_RDWR);
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
          write(pager->file_descriptor, page_buf, PAGE_SIZE);
        }
      }
      ftruncate(jfd, target_offset);
    }
    close(jfd);
  }

  pager->num_pages = pager->savepoints[idx].num_pages_at_savepoint;
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
      if (pager->is_dirty == NULL || pager->is_dirty[i]) {
        pager_flush(pager, i);
      }
      free(pager->pages[i]);
      pager->pages[i] = NULL;
    }
  }
  fdatasync(pager->file_descriptor);
  pager_unlock(pager);
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
  free(pager);
}

void pager_set_wal_mode(Pager* pager, bool enable_wal) {
  if (enable_wal) {
    if (pager->use_wal) return;
    pager->use_wal = true;
    if (pager->wal_fd == -1) {
      pager->wal_fd = open(pager->wal_filename, O_RDWR | O_CREAT, S_IWUSR | S_IRUSR);
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
    pager->wal_fd = open(pager->wal_filename, O_RDWR | O_CREAT, S_IWUSR | S_IRUSR);
  }
  if (pager->wal_fd == -1) return;

  off_t wal_len = lseek(pager->wal_fd, 0, SEEK_END);
  off_t frame_size = 4 + 4 + PAGE_SIZE; /* pnum + crc32 + page_data */
  off_t num_frames = wal_len / frame_size;

  if (num_frames > 0) {
    uint8_t page_buf[PAGE_SIZE];
    for (off_t f = 0; f < num_frames; f++) {
      uint32_t f_pnum, stored_crc;
      lseek(pager->wal_fd, f * frame_size, SEEK_SET);
      if (read(pager->wal_fd, &f_pnum, 4) == 4 && read(pager->wal_fd, &stored_crc, 4) == 4) {
        if (read(pager->wal_fd, page_buf, PAGE_SIZE) == PAGE_SIZE) {
          uint32_t computed_crc = calculate_crc32(page_buf, PAGE_SIZE);
          if (computed_crc == stored_crc) {
            pwrite(pager->file_descriptor, page_buf, PAGE_SIZE, (off_t)f_pnum * PAGE_SIZE);
            if (f_pnum < pager->max_pages && pager->pages[f_pnum]) {
              memcpy(pager->pages[f_pnum], page_buf, PAGE_SIZE);
            }
          }
        }
      }
    }
    fdatasync(pager->file_descriptor);
    ftruncate(pager->wal_fd, 0);
    lseek(pager->wal_fd, 0, SEEK_SET);
    fsync(pager->wal_fd);
  }

  for (uint32_t i = 0; i < pager->num_pages; i++) {
    if (i < pager->max_pages && pager->pages[i]) {
      free(pager->pages[i]);
      pager->pages[i] = NULL;
    }
  }

  printf("[WAL] Checkpoint completed. All frames written to main database file.\n");
}
