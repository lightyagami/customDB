#pragma once
#include "common.h"
#include "pager.h"

typedef enum {
  COL_INT,
  COL_TEXT,
  COL_FLOAT,
  COL_DOUBLE,
  COL_BOOL,
  COL_VARCHAR,
  COL_BLOB,
  COL_DATETIME,
  COL_DATE,
  COL_TIME,
  COL_TIMESTAMP,
  COL_NUMERIC,
  COL_DECIMAL,
  COL_VECTOR
} ColumnType;

typedef enum {
  COLL_NOT_SET = 0,
  COLL_BINARY  = 1,
  COLL_NOCASE  = 2,
  COLL_RTRIM   = 3
} CollationType;

static inline int compare_strings_collated(const char* s1, const char* s2, CollationType coll) {
  if (coll == COLL_NOCASE) {
    return strcasecmp(s1, s2);
  } else if (coll == COLL_RTRIM) {
    size_t len1 = strlen(s1);
    size_t len2 = strlen(s2);
    while (len1 > 0 && isspace((unsigned char)s1[len1 - 1])) len1--;
    while (len2 > 0 && isspace((unsigned char)s2[len2 - 1])) len2--;
    size_t min_len = (len1 < len2) ? len1 : len2;
    int res = strncmp(s1, s2, min_len);
    if (res != 0) return res;
    return (len1 > len2) - (len1 < len2);
  }
  return strcmp(s1, s2);
}

typedef struct {
  char       name[COL_NAME_SIZE];
  ColumnType type;
  uint32_t   size;   /* on-disk bytes: INT=4, FLOAT=4, TEXT=declared n, VARCHAR=max size */
  bool       has_index;
  uint32_t   index_root_page;

  /* Constraints */
  bool       is_not_null;
  bool       is_unique;
  bool       has_default;
  char       default_val[64];

  bool       has_check;
  uint32_t   check_op;
  char       check_val[64];

  bool       has_fk;
  char       fk_target_table[TBL_NAME_SIZE];
  char       fk_target_col[COL_NAME_SIZE];
  bool       fk_on_delete_cascade;
  bool       fk_on_update_cascade;

  bool          is_autoincrement;
  CollationType collation;

  /* Advanced Index metadata */
  bool       idx_is_partial;
  char       idx_where_col[COL_NAME_SIZE];
  uint32_t   idx_where_op;
  char       idx_where_val[64];
  bool       idx_is_expr;
  char       idx_expr_func[32];
} Column;

/* In-memory representation of a row value with Small String Optimization (SSO) */
typedef struct {
  int32_t  int_val;
  float    float_val;
  double   double_val;
  bool     bool_val;
  bool     is_null;
  char*    text_val;       /* Points to heap buffer or constant empty string; never NULL */
  uint32_t text_len;       /* String length in bytes */
  uint32_t text_cap;       /* Allocated heap capacity */
} Value;

/* Value Lifecycle Helpers */
void value_init(Value* v);
void value_free(Value* v);
void value_free_row(Value* values, uint32_t count);
void value_set_text(Value* v, const char* str);
void value_set_text_len(Value* v, const char* str, uint32_t len);
void value_copy(Value* dst, const Value* src);
void value_move(Value* dst, Value* src);

/* Full table definition (schema + B+ Tree root pointer).
 * col_offsets and row_size are computed, not stored on disk. */
typedef struct {
  char     name[IDX_NAME_SIZE];
  uint32_t root_page_num;
  uint32_t num_cols;
  Column   columns[MAX_COLUMNS];
  /* Composite Primary Key fields */
  uint32_t num_pk_cols;
  char     pk_cols[MAX_COLUMNS][COL_NAME_SIZE];
  /* Computed fields — not persisted: */
  uint32_t row_size;
  uint32_t col_offsets[MAX_COLUMNS];
  /* Virtual Table fields */
  bool     is_virtual;
  char     vtab_module[64];
  char     vtab_args[256];
  /* Row-level Time-To-Live (TTL in seconds; 0 = no TTL) */
  uint32_t default_ttl;
  /* WITH HISTORY temporal shadow table flag */
  bool     with_history;
} TableDef;

typedef struct {
  char view_name[TBL_NAME_SIZE];
  char select_sql[256];
} ViewDef;

typedef enum { TRIGGER_BEFORE, TRIGGER_AFTER } TriggerTiming;
typedef enum { TRIGGER_INSERT, TRIGGER_UPDATE, TRIGGER_DELETE } TriggerEvent;

typedef struct {
  char          name[TBL_NAME_SIZE];
  char          target_table[TBL_NAME_SIZE];
  TriggerTiming timing;
  TriggerEvent  event;
  char          action_sql[256];
} TriggerDef;

/* Catalog: all table, view, and trigger definitions in this database file */
typedef struct {
  uint32_t   num_tables;
  TableDef   tables[MAX_TABLES];
  uint32_t   num_views;
  ViewDef    views[MAX_VIEWS];
  uint32_t   num_triggers;
  TriggerDef triggers[MAX_TRIGGERS];
} Catalog;

/* ── Catalog I/O ─────────────────────────────────────────────────────────── */
void      catalog_load(Catalog* catalog, Pager* pager);
void      catalog_save(Catalog* catalog, Pager* pager);
uint32_t  catalog_get_reserved_pages(uint32_t num_tables);
TableDef* catalog_find(Catalog* catalog, const char* name);
ViewDef*  catalog_find_view(Catalog* catalog, const char* name);

/* Compute row_size and col_offsets from column definitions */
void      tabledef_compute(TableDef* def);

/* ── Row serialization ───────────────────────────────────────────────────── */
uint32_t serialize_row(TableDef* def, Value* values, void* dest);
uint32_t serialize_row_with_ttl(TableDef* def, Value* values, uint64_t expire_at, void* dest);
uint32_t deserialize_row(TableDef* def, void* src, Value* values);
uint32_t deserialize_row_with_ttl(TableDef* def, void* src, Value* values, uint64_t* out_expire_at);
void print_row(TableDef* def, Value* values);
void print_schema(TableDef* def);
