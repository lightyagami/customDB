# Concurrency & Transaction Management

This document details the design of our ACID transaction processor, Write-Ahead Rollback Journal, and Five-State Concurrency Lock Engine.

---

## ACID Transactions & Rollback Journaling

To guarantee atomicity and durability in the event of power loss or process crashes, the engine uses a rollback journal file (`<dbname>-journal`):

### 1. Write-Ahead Journaling Protocol
1. **Begin Transaction**: The engine opens a new rollback journal file.
2. **Page Journaling**: Before modifying any page in the main database file:
   - The engine snapshots the unmodified, clean page content from disk.
   - It appends the page number and original $4\,\text{KB}$ bytes to the journal.
   - Sets a bit in an in-memory page tracking mask (`page_is_journaled`) to avoid redundant logging of the same page.
3. **Commit**: 
   - All modified cached pages are flushed to the main database file.
   - The journal file is closed and deleted (`unlink`).
4. **Rollback**:
   - The engine opens the journal, reads all logged pages, and overwrites the main database file with the original page states.
   - The journal is deleted.

---

### 2. Crash Recovery State Machine
On startup, the pager checks if a non-empty journal file exists on disk (indicating a previous process crashed or was killed mid-transaction):
- If the journal file exists:
  - The engine reads all logged pages from the journal and restores them to the main database file.
  - The journal is unlinked.
  - This recovers the database to a clean, pre-transaction state.
- If no journal exists, the database is considered clean and starts normally.

---

## Five-State Concurrency Lock Engine

Multi-process concurrency is coordinated using POSIX advisory locks via `fcntl` on three dedicated bytes beyond database data space:
- **`PENDING_BYTE`** = `0x10000` (64KB offset)
- **`RESERVED_BYTE`** = `0x10001`
- **`SHARED_BYTE`** = `0x10002`

```
  +------------+
  |  UNLOCKED  |
  +------------+
        |
        v
  +------------+
  |   SHARED   | <--- Multiple concurrent readers
  +------------+
        |
        v
  +------------+
  |  RESERVED  | <--- Only 1 process intending to write (readers still allowed)
  +------------+
        |
        v
  +------------+
  |  PENDING   | <--- Blocks new readers; waiting for current readers to exit
  +------------+
        |
        v
  +------------+
  | EXCLUSIVE  | <--- Single writer has exclusive read/write access
  +------------+
```

### Transition Mechanics & Rules
1. **Acquire `SHARED` lock (Reader)**:
   - Verifies no other process holds a write lock on the `PENDING_BYTE`. If blocked, it fails.
   - Acquires a read lock (`F_RDLCK`) on `SHARED_BYTE`.
2. **Acquire `RESERVED` lock (Pending Writer)**:
   - Acquires a write lock (`F_WRLCK`) on `RESERVED_BYTE`. Fails if another writer holds it.
   - Readers can continue to read during this state.
3. **Acquire `PENDING` lock**:
   - Acquires a write lock (`F_WRLCK`) on `PENDING_BYTE`. This blocks new readers from acquiring `SHARED`.
4. **Acquire `EXCLUSIVE` lock**:
   - Upgrades its own read lock on `SHARED_BYTE` to a write lock (`F_WRLCK`).
   - Succeeds only if all other readers have finished and released their locks on `SHARED_BYTE`.
5. **Unlock**:
   - Releases locks on all three bytes, returning to `UNLOCKED` (NO_LOCK).

### Busy-Retry Handler
To prevent queries from aborting immediately under momentary conflicts, lock acquisition retries up to 50 times with a `10ms` sleep between retries (providing a `500ms` total timeout).
