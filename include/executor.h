#pragma once
#include "common.h"
#include "catalog.h"
#include "parser.h"
#include "pager.h"

ExecuteResult execute_statement(Statement* stmt, Catalog* catalog, Pager* pager);
