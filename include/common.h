#pragma once

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <io.h>
  #include <direct.h>
  #include <process.h>
  #define strcasecmp _stricmp
  #define strncasecmp _strnicmp
  #define strtok_r strtok_s
  #define ftruncate(fd, sz) _chsize_s(fd, (sz))
  #define fsync(fd) _commit(fd)
  #ifndef O_BINARY
    #define O_BINARY _O_BINARY
  #endif
  #ifndef F_RDLCK
    #define F_RDLCK 0
    #define F_WRLCK 1
    #define F_UNLCK 2
  #endif
#else
  #include <unistd.h>
  #include <pthread.h>
  #ifndef O_BINARY
    #define O_BINARY 0
  #endif
#endif

/* ── Storage constants ───────────────────────────────────────────────────── */
#define PAGE_SIZE        4096
#define INVALID_PAGE_NUM UINT32_MAX

#define MAX_RAW_VAL    4096   /* max char length of a raw parsed value token */

/* ── Schema limits ───────────────────────────────────────────────────────── */
#define MAX_VIEWS        4    /* max views per db file                      */
#define MAX_TRIGGERS     4    /* max triggers per db file                   */
#define MAX_COLUMNS     16    /* max columns per table                      */
#define MAX_MULTI_ROWS   8    /* max rows in multi-row insert               */
#define MAX_TABLES    1024    /* max tables per db file (multi-page catalog) */
#define MAX_TEXT_SIZE 4096    /* max on-disk bytes for a TEXT column        */
#define COL_NAME_SIZE   32    /* max bytes for a column name                */
#define TBL_NAME_SIZE   64    /* max bytes for a table/index name           */
#define IDX_NAME_SIZE  128    /* max bytes for synthesised index name       */

/* ── Error codes ─────────────────────────────────────────────────────────── */
typedef enum {
  EXECUTE_SUCCESS,
  EXECUTE_DUPLICATE_KEY,
  EXECUTE_ROW_NOT_FOUND,
  EXECUTE_TABLE_NOT_FOUND,
  EXECUTE_TABLE_EXISTS,
  EXECUTE_CATALOG_FULL,
  EXECUTE_BAD_SCHEMA,
  EXECUTE_TYPE_MISMATCH,
  EXECUTE_CONSTRAINT_NOT_NULL,
  EXECUTE_CONSTRAINT_UNIQUE,
  EXECUTE_CONSTRAINT_CHECK,
  EXECUTE_CONSTRAINT_FOREIGN_KEY,
  EXECUTE_BUSY,
} ExecuteResult;

typedef enum {
  META_COMMAND_SUCCESS,
  META_COMMAND_UNRECOGNIZED_COMMAND,
} MetaCommandResult;

typedef enum {
  PREPARE_SUCCESS,
  PREPARE_NEGATIVE_ID,
  PREPARE_VALUE_TOO_LONG,
  PREPARE_SYNTAX_ERROR,
  PREPARE_UNRECOGNIZED_STATEMENT,
  PREPARE_BAD_SCHEMA,
} PrepareResult;
