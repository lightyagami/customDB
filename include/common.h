#pragma once

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>

/* ── Storage constants ───────────────────────────────────────────────────── */
#define PAGE_SIZE        4096
#define INVALID_PAGE_NUM UINT32_MAX

#define MAX_RAW_VAL    1024   /* max char length of a raw parsed value token */

/* ── Schema limits ───────────────────────────────────────────────────────── */
#define MAX_COLUMNS     16    /* max columns per table                      */
#define MAX_TABLES    1024    /* max tables per db file (multi-page catalog) */
#define MAX_TEXT_SIZE 1024    /* max on-disk bytes for a TEXT column        */
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
