#pragma once
#include "common.h"

typedef enum {
  NO_LOCK = 0,
  SHARED_LOCK = 1,
  RESERVED_LOCK = 2,
  PENDING_LOCK = 3,
  EXCLUSIVE_LOCK = 4
} PagerLockState;

typedef struct {
  int      file_descriptor;
  uint32_t file_length;
  uint32_t num_pages;
  uint32_t max_pages;       /* capacity of pages list */
  void**   pages;           /* resizable array of page pointers */
  
  /* Transaction & Journaling */
  bool     in_transaction;
  int      journal_fd;
  char     journal_filename[512];
  char     main_filename[256];
  bool*    page_is_journaled;
  bool*    is_dirty;
  uint32_t num_pages_at_tx_start;
  
  /* Concurrency Lock State */
  PagerLockState lock_state;

#define WAL_MAGIC          0x574C3200u   /* "WL2\0" */
#define WAL_FRAME_SIZE_V1  (4 + 4 + PAGE_SIZE)          /* 4104 */
#define WAL_FRAME_SIZE_V2  (4 + 4 + 8 + 8 + PAGE_SIZE)  /* 4120 */

  /* Write-Ahead Logging (WAL Mode) */
  bool     use_wal;
  int      wal_fd;
  char     wal_filename[512];
  uint32_t wal_frame_size;
  uint64_t wal_lsn;
  uint64_t current_commit_ts;
  uint64_t current_commit_lsn;

  /* Savepoints */
  uint32_t num_savepoints;
  struct {
    char     name[64];
    off_t    journal_offset;
    uint32_t num_pages_at_savepoint;
  } savepoints[16];

  /* Dynamic catalog reservation bound */
  uint32_t reserved_catalog_pages;

  /* Auto-Vacuum Pointer-Map Mode */
  bool     auto_vacuum;

  /* In-Memory Database Mode */
  bool     is_memory;

  /* Freelist */
  uint32_t freelist_head;

  /* Table-Level Fine-Grained Concurrency Locks */
  pthread_rwlock_t table_rwlocks[MAX_TABLES];

  /* MVCC Concurrency, Shadow-Page COW & Snapshot Epoch Registry */
  void**           shadow_pages;
  bool*            is_shadowed;
  uint32_t         shadow_capacity;
  pthread_t        writer_tid;
  pthread_mutex_t  swap_mutex;
  pthread_mutex_t  epoch_mutex;
  pthread_mutex_t  writer_mutex;
  struct {
    uint64_t snapshot_xid;
    bool     active;
  } active_snapshots[64];
  struct RetiredPage* retired_pages_head;

  /* Set to true if a lock could not be acquired after retries.
   * Callers should check this and propagate the error rather than
   * proceeding with potentially unsafe writes. */
  bool     lock_error;

  /* Performance & I/O Metrics */
  uint64_t disk_reads;
  uint64_t cache_hits;
} Pager;

typedef struct RetiredPage {
  void* buffer;
  uint64_t retired_lsn;
  struct RetiredPage* next;
} RetiredPage;

Pager*   pager_open(const char* filename);
void     pager_reset_stats(Pager* pager);
void*    get_page(Pager* pager, uint32_t page_num);
void     pager_flush(Pager* pager, uint32_t page_num);
uint32_t get_unused_page_num(Pager* pager);
void     pager_free_page(Pager* pager, uint32_t page_num);
void     pager_close(Pager* pager);

/* Concurrency Locks */
bool     pager_lock(Pager* pager, PagerLockState lock_type);
void     pager_unlock(Pager* pager);

/* Transactions & ACID Journaling */
void     pager_begin_transaction(Pager* pager);
bool     pager_ensure_write_lock(Pager* pager);
void     pager_journal_page(Pager* pager, uint32_t page_num);
void     pager_commit(Pager* pager);
void     pager_rollback(Pager* pager);

/* Savepoints */
void     pager_savepoint(Pager* pager, const char* name);
void     pager_rollback_to_savepoint(Pager* pager, const char* name);
void     pager_release_savepoint(Pager* pager, const char* name);

/* Write-Ahead Logging (WAL) & PITR */
void     pager_set_wal_mode(Pager* pager, bool enable_wal);
void     pager_checkpoint(Pager* pager);
void     pager_backup(Pager* pager, const char* dest_filename);
bool     pager_restore(const char* src_file, const char* dest_file,
                       uint64_t until_ts, bool use_ts,
                       uint64_t until_lsn, bool use_lsn);

/* MVCC Snapshot Registry & Epoch Reclamation */
uint64_t pager_register_snapshot(Pager* pager);
void     pager_unregister_snapshot(Pager* pager, uint64_t snapshot_xid);
uint64_t pager_get_min_active_snapshot_xid(Pager* pager);
void     pager_reclaim_retired_pages(Pager* pager);
void     pager_shadow_page_write(Pager* pager, uint32_t page_num);
void     pager_refresh_if_modified(Pager* pager);
