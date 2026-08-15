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

  /* Write-Ahead Logging (WAL Mode) */
  bool     use_wal;
  int      wal_fd;
  char     wal_filename[512];

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

  /* Set to true if a lock could not be acquired after retries.
   * Callers should check this and propagate the error rather than
   * proceeding with potentially unsafe writes. */
  bool     lock_error;
} Pager;

Pager*   pager_open(const char* filename);
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
void     pager_journal_page(Pager* pager, uint32_t page_num);
void     pager_commit(Pager* pager);
void     pager_rollback(Pager* pager);

/* Savepoints */
void     pager_savepoint(Pager* pager, const char* name);
void     pager_rollback_to_savepoint(Pager* pager, const char* name);
void     pager_release_savepoint(Pager* pager, const char* name);

/* Write-Ahead Logging (WAL) */
void     pager_set_wal_mode(Pager* pager, bool enable_wal);
void     pager_checkpoint(Pager* pager);
