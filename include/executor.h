#pragma once
#include "common.h"
#include "catalog.h"
#include "parser.h"
#include "pager.h"

ExecuteResult execute_statement(Statement* stmt, Catalog* catalog, Pager* pager);
bool eval_where_clause(TableDef* def, Value* row_vals, WhereClause* wc, Catalog* catalog, Pager* pager);
