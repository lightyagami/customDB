#pragma once
#include "common.h"
#include "catalog.h"
#include "parser.h"
#include "pager.h"

ExecuteResult execute_statement(Statement* stmt, Catalog* catalog, Pager* pager);
bool eval_where_clause(TableDef* def, Value* row_vals, WhereClause* wc, Catalog* catalog, Pager* pager);
void eval_expr_string(const char* expr, TableDef* def, Value* row_vals, char* out_buf, size_t out_size);
bool row_is_visible_and_active(TableDef* def, const void* row_bytes, uint64_t snapshot_xid, Value* out_values, uint64_t now_ts);
