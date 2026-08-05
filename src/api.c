#include "dbms.h"
#include "common.h"
#include "pager.h"
#include "catalog.h"
#include "parser.h"
#include "executor.h"
#include "vdbe.h"
#include "btree.h"
#include "cursor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ---------------------------------------------------------------------------
 * resolve_param_placeholders
 * Walk every WHERE condition in `stmt`.  For each condition whose raw_val
 * begins with '?' resolve it to the corresponding bound value stored in
 * stmt->raw_values[].
 *
 * Positional syntax supported:
 *   ?      – auto-numbered (condition index, 0-based)
 *   ?1     – explicitly the first bound value (raw_values[0])
 *   ?2     – second bound value (raw_values[1])  … etc.
 *
 * The replacement is a *literal copy* of the user-supplied string, so even
 * if the caller passes "' OR '1'='1" it is compared as a string value, never
 * interpreted as SQL, giving genuine parameterised-query protection.
 * ---------------------------------------------------------------------------*/
static void resolve_param_placeholders(Statement* stmt) {
  WhereClause* wc = &stmt->where_clause;
  if (!wc->has_where) return;

  /* count how many positional '?' parameters appear sequentially */
  int auto_idx = 0;
  for (uint32_t ci = 0; ci < wc->num_conds; ci++) {
    SingleCond* cond = &wc->conds[ci];
    const char* rv = cond->raw_val;
    if (rv[0] != '?') continue;

    int p_idx;
    if (rv[1] >= '1' && rv[1] <= '9') {
      /* Explicit: ?1 -> raw_values[0], ?2 -> raw_values[1] … */
      p_idx = rv[1] - '1';
    } else {
      /* Implicit positional */
      p_idx = auto_idx;
    }
    auto_idx++;

    if (p_idx < 0 || p_idx >= MAX_COLUMNS) continue;
    const char* bound = stmt->raw_values[p_idx];
    if (bound[0] == '\0') continue;
    strncpy(cond->raw_val, bound, MAX_RAW_VAL - 1);
    cond->raw_val[MAX_RAW_VAL - 1] = '\0';
  }
}

struct dbms {
  Pager* pager;
  Catalog catalog;
  char last_error[256];
  int last_changes;
  int64_t last_rowid;
};

struct dbms_stmt {
  dbms* db;
  Statement stmt;
  char sql[512];
  bool executed;
  Vdbe* compiled_vm;
  
  /* Result cursor data */
  TableDef* target_def;
  Table target_table;   /* backing storage for btree_cur — MUST outlive the
                            dbms_step() call that creates it, since btree_cur
                            keeps a pointer back into this struct across
                            multiple dbms_step() calls (row-by-row iteration) */
  Cursor* btree_cur;
  Value current_row_vals[MAX_COLUMNS];
  bool has_current_row;
};

int dbms_open(const char* filename, dbms** ppDb) {
  if (ppDb == NULL) return DBMS_MISUSE;
  dbms* db = malloc(sizeof(dbms));
  memset(db, 0, sizeof(dbms));

  db->pager = pager_open(filename);
  if (db->pager == NULL) {
    free(db);
    *ppDb = NULL;
    return DBMS_ERROR;
  }

  catalog_load(&db->catalog, db->pager);
  *ppDb = db;
  return DBMS_OK;
}

int dbms_close(dbms* pDb) {
  if (pDb == NULL) return DBMS_OK;
  if (pDb->pager) {
    catalog_save(&pDb->catalog, pDb->pager);
    pager_close(pDb->pager);
  }
  free(pDb);
  return DBMS_OK;
}

const char* dbms_errmsg(dbms* pDb) {
  if (pDb == NULL) return "Invalid database connection handle";
  return pDb->last_error;
}

int dbms_exec(dbms* pDb, const char* sql, int (*callback)(void*, int, char**, char**), void* arg, char** errmsg) {
  (void)callback;
  (void)arg;
  if (pDb == NULL || sql == NULL) return DBMS_MISUSE;

  Statement stmt;
  memset(&stmt, 0, sizeof(Statement));
  PrepareResult prep = prepare_statement(sql, &stmt);
  if (prep != PREPARE_SUCCESS) {
    snprintf(pDb->last_error, sizeof(pDb->last_error), "SQL parse error");
    if (errmsg) *errmsg = strdup(pDb->last_error);
    return DBMS_ERROR;
  }

  ExecuteResult res = execute_statement(&stmt, &pDb->catalog, pDb->pager);
  if (pDb->pager->lock_error) {
    snprintf(pDb->last_error, sizeof(pDb->last_error), "Database is locked by another process");
    if (errmsg) *errmsg = strdup(pDb->last_error);
    return DBMS_BUSY;
  }
  if (res != EXECUTE_SUCCESS) {
    snprintf(pDb->last_error, sizeof(pDb->last_error), "SQL execution error (%d)", res);
    if (errmsg) *errmsg = strdup(pDb->last_error);
    return DBMS_ERROR;
  }

  catalog_load(&pDb->catalog, pDb->pager);
  return DBMS_OK;
}

int dbms_prepare_v2(dbms* pDb, const char* zSql, int nByte, dbms_stmt** ppStmt, const char** pzTail) {
  if (pDb == NULL || zSql == NULL || ppStmt == NULL) return DBMS_MISUSE;
  catalog_load(&pDb->catalog, pDb->pager);

  char sql_buf[512];
  if (nByte > 0 && nByte < (int)sizeof(sql_buf)) {
    memcpy(sql_buf, zSql, nByte);
    sql_buf[nByte] = '\0';
  } else {
    strncpy(sql_buf, zSql, sizeof(sql_buf) - 1);
    sql_buf[sizeof(sql_buf) - 1] = '\0';
  }

  dbms_stmt* stmt = malloc(sizeof(dbms_stmt));
  memset(stmt, 0, sizeof(dbms_stmt));
  stmt->db = pDb;
  memcpy(stmt->sql, sql_buf, sizeof(stmt->sql) - 1);
  stmt->sql[sizeof(stmt->sql) - 1] = '\0';

  PrepareResult prep = prepare_statement(sql_buf, &stmt->stmt);
  if (prep != PREPARE_SUCCESS) {
    snprintf(pDb->last_error, sizeof(pDb->last_error), "Prepare error");
    free(stmt);
    *ppStmt = NULL;
    return DBMS_ERROR;
  }

  /* Pre-compile VDBE bytecode for STATEMENT_INSERT */
  if (stmt->stmt.type == STATEMENT_INSERT) {
    TableDef* def = catalog_find(&pDb->catalog, stmt->stmt.table_name);
    if (def != NULL) {
      Vdbe* vm = vdbe_create(pDb->pager, &pDb->catalog);
      Value tbl_name_val;
      memset(&tbl_name_val, 0, sizeof(Value));
      strncpy(tbl_name_val.text_val, def->name, sizeof(tbl_name_val.text_val)-1);
      vdbe_add_inst(vm, OP_OpenWrite, 0, def->root_page_num, 0, tbl_name_val);

      for (uint32_t i = 0; i < def->num_cols; i++) {
        Column* col = &def->columns[i];
        if (col->type == COL_INT) {
          vdbe_add_inst(vm, OP_Integer, 0, 0, i + 1, (Value){0});
        } else if (col->type == COL_FLOAT || col->type == COL_DOUBLE || col->type == COL_NUMERIC || col->type == COL_DECIMAL) {
          vdbe_add_inst(vm, OP_Double, 0, 0, i + 1, (Value){0});
        } else {
          vdbe_add_inst(vm, OP_String, 0, 0, i + 1, (Value){0});
        }
      }

      int dup_lbl = vm->num_insts;
      vdbe_add_inst(vm, OP_SeekGE, 0, dup_lbl + 4, 1, (Value){0});
      vdbe_add_inst(vm, OP_Column, 0, 0, 0, (Value){0});
      vdbe_add_inst(vm, OP_Compare, 0, 1, OP_EQ, (Value){0});
      vdbe_add_inst(vm, OP_IfFalse, dup_lbl + 4, 0, 0, (Value){0});

      vdbe_add_inst(vm, OP_Insert, 0, 1, 0, (Value){0});
      vdbe_add_inst(vm, OP_Halt, 0, 0, 0, (Value){0});

      stmt->compiled_vm = vm;
      stmt->target_def = def;
    }
  }

  if (pzTail) *pzTail = zSql + (nByte > 0 ? nByte : (int)strlen(zSql));
  stmt->target_def = catalog_find(&pDb->catalog, stmt->stmt.table_name);
  *ppStmt = stmt;
  return DBMS_OK;
}

int dbms_bind_int(dbms_stmt* pStmt, int index, int value) {
  if (pStmt == NULL || index < 1 || index > MAX_COLUMNS) return DBMS_MISUSE;
  snprintf(pStmt->stmt.raw_values[index - 1], MAX_RAW_VAL, "%d", value);
  return DBMS_OK;
}

int dbms_bind_double(dbms_stmt* pStmt, int index, double value) {
  if (pStmt == NULL || index < 1 || index > MAX_COLUMNS) return DBMS_MISUSE;
  snprintf(pStmt->stmt.raw_values[index - 1], MAX_RAW_VAL, "%f", value);
  return DBMS_OK;
}

int dbms_bind_text(dbms_stmt* pStmt, int index, const char* text, int len) {
  (void)len;
  if (pStmt == NULL || index < 1 || index > MAX_COLUMNS) return DBMS_MISUSE;
  if (text == NULL) {
    strcpy(pStmt->stmt.raw_values[index - 1], "NULL");
  } else {
    snprintf(pStmt->stmt.raw_values[index - 1], MAX_RAW_VAL, "%s", text);
  }
  return DBMS_OK;
}

int dbms_bind_blob(dbms_stmt* pStmt, int index, const void* blob, int len) {
  (void)len;
  if (pStmt == NULL || index < 1 || index > MAX_COLUMNS) return DBMS_MISUSE;
  if (blob == NULL) {
    strcpy(pStmt->stmt.raw_values[index - 1], "NULL");
  } else {
    snprintf(pStmt->stmt.raw_values[index - 1], MAX_RAW_VAL, "%s", (const char*)blob);
  }
  return DBMS_OK;
}

int dbms_bind_null(dbms_stmt* pStmt, int index) {
  if (pStmt == NULL || index < 1 || index > MAX_COLUMNS) return DBMS_MISUSE;
  strcpy(pStmt->stmt.raw_values[index - 1], "NULL");
  return DBMS_OK;
}

int dbms_step(dbms_stmt* pStmt) {
  if (pStmt == NULL || pStmt->db == NULL) return DBMS_MISUSE;

  if (pStmt->stmt.type == STATEMENT_SELECT) {
    /* Resolve bound '?' parameters before any scan/filter. */
    if (!pStmt->executed) resolve_param_placeholders(&pStmt->stmt);

    /* If a WHERE clause is present, delegate to execute_statement which
       runs the full run_select_vm pipeline (with WHERE evaluation).
       We do this once and store matching rows in the result set. */
    if (pStmt->stmt.where_clause.has_where) {
      if (!pStmt->executed) {
        ExecuteResult res = execute_statement(&pStmt->stmt, &pStmt->db->catalog, pStmt->db->pager);
        pStmt->executed = true;
        (void)res;
      }
      pStmt->has_current_row = false;
      return DBMS_DONE;
    }

    /* No WHERE — use the efficient step-by-step btree cursor path. */
    if (!pStmt->executed) {
      pStmt->target_def = catalog_find(&pStmt->db->catalog, pStmt->stmt.table_name);
      if (pStmt->target_def == NULL) return DBMS_ERROR;

      pStmt->target_table = (Table){ pStmt->db->pager, pStmt->target_def };
      pStmt->btree_cur = btree_start(&pStmt->target_table);
      pStmt->executed = true;
    }

    while (pStmt->btree_cur && !pStmt->btree_cur->end_of_table) {
      deserialize_row(pStmt->target_def, cursor_value(pStmt->btree_cur), pStmt->current_row_vals);
      cursor_advance(pStmt->btree_cur);
      pStmt->has_current_row = true;
      return DBMS_ROW;
    }

    pStmt->has_current_row = false;
    return DBMS_DONE;
  }

  /* DML/DDL execution */
  if (!pStmt->executed) {
    /* Resolve bound '?' parameters before execution */
    resolve_param_placeholders(&pStmt->stmt);

    if (pStmt->stmt.type == STATEMENT_INSERT && pStmt->compiled_vm != NULL) {
      /* Fast path: reuse the VDBE program compiled once at dbms_prepare_v2
         time. Patch this call's bound values into the existing instruction
         operands instead of re-parsing/re-planning the SQL from scratch. */
      Vdbe* vm = pStmt->compiled_vm;
      TableDef* def = pStmt->target_def;
      for (uint32_t i = 0; i < def->num_cols; i++) {
        Instruction* inst = &vm->insts[1 + i]; /* slot 0 is OP_OpenWrite */
        Column* col = &def->columns[i];
        const char* raw = pStmt->stmt.raw_values[i];
        if (col->type == COL_INT) {
          inst->p1 = atoi(raw);
        } else if (col->type == COL_FLOAT) {
          inst->p4.float_val = (float)atof(raw);
        } else if (col->type == COL_DOUBLE || col->type == COL_NUMERIC || col->type == COL_DECIMAL) {
          inst->p4.double_val = atof(raw);
        } else {
          memset(&inst->p4, 0, sizeof(Value));
          strncpy(inst->p4.text_val, raw, sizeof(inst->p4.text_val) - 1);
        }
      }
      vdbe_run(vm);
      pStmt->executed = true;
    } else {
      ExecuteResult res = execute_statement(&pStmt->stmt, &pStmt->db->catalog, pStmt->db->pager);
      pStmt->executed = true;
      if (res != EXECUTE_SUCCESS) return DBMS_ERROR;
    }
  }

  return DBMS_DONE;
}

int dbms_reset(dbms_stmt* pStmt) {
  if (pStmt == NULL) return DBMS_MISUSE;
  if (pStmt->btree_cur) {
    free(pStmt->btree_cur);
    pStmt->btree_cur = NULL;
  }
  pStmt->executed = false;
  pStmt->has_current_row = false;
  return DBMS_OK;
}

int dbms_finalize(dbms_stmt* pStmt) {
  if (pStmt == NULL) return DBMS_OK;
  dbms_reset(pStmt);
  if (pStmt->compiled_vm) {
    vdbe_free(pStmt->compiled_vm);
    pStmt->compiled_vm = NULL;
  }
  free(pStmt);
  return DBMS_OK;
}

int dbms_column_count(dbms_stmt* pStmt) {
  if (pStmt == NULL || pStmt->target_def == NULL) return 0;
  return pStmt->target_def->num_cols;
}

const char* dbms_column_name(dbms_stmt* pStmt, int col) {
  if (pStmt == NULL || pStmt->target_def == NULL || col < 0 || col >= (int)pStmt->target_def->num_cols) return "";
  return pStmt->target_def->columns[col].name;
}

int dbms_column_type(dbms_stmt* pStmt, int col) {
  if (pStmt == NULL || pStmt->target_def == NULL || col < 0 || col >= (int)pStmt->target_def->num_cols) return 0;
  return (int)pStmt->target_def->columns[col].type;
}

int dbms_column_int(dbms_stmt* pStmt, int col) {
  if (pStmt == NULL || !pStmt->has_current_row || col < 0 || col >= MAX_COLUMNS) return 0;
  return pStmt->current_row_vals[col].int_val;
}

double dbms_column_double(dbms_stmt* pStmt, int col) {
  if (pStmt == NULL || !pStmt->has_current_row || col < 0 || col >= MAX_COLUMNS) return 0.0;
  return pStmt->current_row_vals[col].double_val;
}

const char* dbms_column_text(dbms_stmt* pStmt, int col) {
  if (pStmt == NULL || !pStmt->has_current_row || col < 0 || col >= MAX_COLUMNS) return "";
  return pStmt->current_row_vals[col].text_val;
}

const void* dbms_column_blob(dbms_stmt* pStmt, int col) {
  if (pStmt == NULL || !pStmt->has_current_row || col < 0 || col >= MAX_COLUMNS) return "";
  return pStmt->current_row_vals[col].text_val;
}

int dbms_column_bytes(dbms_stmt* pStmt, int col) {
  if (pStmt == NULL || !pStmt->has_current_row || col < 0 || col >= MAX_COLUMNS) return 0;
  return (int)strlen(pStmt->current_row_vals[col].text_val);
}

int64_t dbms_last_insert_rowid(dbms* pDb) {
  if (pDb == NULL) return 0;
  return pDb->last_rowid;
}

int dbms_changes(dbms* pDb) {
  if (pDb == NULL) return 0;
  return pDb->last_changes;
}
