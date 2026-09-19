#pragma once
#include "common.h"
#include "catalog.h"
#include "parser.h"
#include "pager.h"

typedef struct {
  Value row[MAX_COLUMNS];
  Value sort_keys[4];
  ColumnType sort_types[4];
  CollationType sort_colls[4];
  bool sort_descs[4];
  uint32_t num_sort_keys;
} RowSortEntry;

int compare_row_sort_entries(const void* a, const void* b);
void add_selected_row(RowSortEntry** p_entries, uint32_t* p_count, uint32_t* p_capacity,
                     Statement* stmt, TableDef* def, Value* row_vals);
void free_row_sort_entries(RowSortEntry* entries, uint32_t count, TableDef* def);
uint32_t deduplicate_distinct_entries(RowSortEntry* entries, uint32_t count, Statement* stmt, TableDef* def);

ExecuteResult execute_statement(Statement* stmt, Catalog* catalog, Pager* pager);
bool eval_where_clause(TableDef* def, Value* row_vals, WhereClause* wc, Catalog* catalog, Pager* pager);
void eval_expr_string(const char* expr, TableDef* def, Value* row_vals, char* out_buf, size_t out_size);
bool row_is_visible_and_active(TableDef* def, const void* row_bytes, uint64_t snapshot_xid, Value* out_values, uint64_t now_ts);

