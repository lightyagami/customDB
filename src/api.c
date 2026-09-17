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
#include <pthread.h>

typedef struct PathMutexNode {
  char filename[256];
  pthread_mutex_t file_mutex;
  int ref_count;
  struct PathMutexNode* next;
} PathMutexNode;

static PathMutexNode* g_path_mutex_head = NULL;
static pthread_mutex_t g_dbms_global_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t* get_file_path_mutex(const char* filename) {
  PathMutexNode* curr = g_path_mutex_head;
  while (curr) {
    if (strcmp(curr->filename, filename) == 0) {
      curr->ref_count++;
      return &curr->file_mutex;
    }
    curr = curr->next;
  }
  PathMutexNode* node = malloc(sizeof(PathMutexNode));
  strncpy(node->filename, filename, sizeof(node->filename) - 1);
  node->filename[sizeof(node->filename) - 1] = '\0';
  pthread_mutex_init(&node->file_mutex, NULL);
  node->ref_count = 1;
  node->next = g_path_mutex_head;
  g_path_mutex_head = node;
  return &node->file_mutex;
}

static void release_file_path_mutex(const char* filename) {
  PathMutexNode** pp = &g_path_mutex_head;
  while (*pp) {
    if (strcmp((*pp)->filename, filename) == 0) {
      (*pp)->ref_count--;
      if ((*pp)->ref_count <= 0) {
        PathMutexNode* to_free = *pp;
        *pp = (*pp)->next;
        pthread_mutex_destroy(&to_free->file_mutex);
        free(to_free);
      }
      return;
    }
    pp = &(*pp)->next;
  }
}

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
  pthread_mutex_t mutex;
  pthread_mutex_t* file_mutex;
  char filename[256];
};

struct dbms_stmt {
  dbms* db;
  Statement stmt;
  char sql[16384];
  bool executed;
  Vdbe* compiled_vm;
  
  /* Result cursor data */
  TableDef* target_def;
  Table target_table;
  Cursor* btree_cur;
  Value current_row_vals[MAX_COLUMNS];
  bool has_current_row;
};

int dbms_open(const char* filename, dbms** ppDb) {
  if (ppDb == NULL) return DBMS_MISUSE;

  pthread_mutex_lock(&g_dbms_global_mutex);

  pthread_mutex_t* fmutex = get_file_path_mutex(filename);
  pthread_mutex_lock(fmutex);

  dbms* db = malloc(sizeof(dbms));
  memset(db, 0, sizeof(dbms));
  pthread_mutex_init(&db->mutex, NULL);
  db->file_mutex = fmutex;
  strncpy(db->filename, filename, sizeof(db->filename) - 1);

  db->pager = pager_open(filename);
  if (db->pager == NULL) {
    pthread_mutex_destroy(&db->mutex);
    release_file_path_mutex(filename);
    free(db);
    *ppDb = NULL;
    pthread_mutex_unlock(fmutex);
    pthread_mutex_unlock(&g_dbms_global_mutex);
    return DBMS_ERROR;
  }

  catalog_load(&db->catalog, db->pager);
  *ppDb = db;

  pthread_mutex_unlock(fmutex);
  pthread_mutex_unlock(&g_dbms_global_mutex);
  return DBMS_OK;
}

int dbms_close(dbms* pDb) {
  if (pDb == NULL) return DBMS_OK;

  pthread_mutex_lock(&g_dbms_global_mutex);
  pthread_mutex_t* fmutex = pDb->file_mutex;
  if (fmutex) pthread_mutex_lock(fmutex);
  pthread_mutex_lock(&pDb->mutex);

  if (pDb->pager) {
    catalog_save(&pDb->catalog, pDb->pager);
    pager_close(pDb->pager);
  }

  pthread_mutex_unlock(&pDb->mutex);
  pthread_mutex_destroy(&pDb->mutex);

  char fn[256];
  strncpy(fn, pDb->filename, sizeof(fn) - 1);
  fn[sizeof(fn) - 1] = '\0';

  free(pDb);

  if (fmutex) {
    pthread_mutex_unlock(fmutex);
    release_file_path_mutex(fn);
  }
  pthread_mutex_unlock(&g_dbms_global_mutex);
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

  if (pDb->file_mutex) pthread_mutex_lock(pDb->file_mutex);
  pthread_mutex_lock(&pDb->mutex);

  Statement* stmt = calloc(1, sizeof(Statement));
  if (!stmt) {
    snprintf(pDb->last_error, sizeof(pDb->last_error), "Out of memory");
    if (errmsg) *errmsg = strdup(pDb->last_error);
    pthread_mutex_unlock(&pDb->mutex);
    if (pDb->file_mutex) pthread_mutex_unlock(pDb->file_mutex);
    return DBMS_ERROR;
  }
  PrepareResult prep = prepare_statement(sql, stmt);
  if (prep != PREPARE_SUCCESS) {
    snprintf(pDb->last_error, sizeof(pDb->last_error), "SQL parse error");
    if (errmsg) *errmsg = strdup(pDb->last_error);
    statement_free_children(stmt);
    free(stmt);
    pthread_mutex_unlock(&pDb->mutex);
    if (pDb->file_mutex) pthread_mutex_unlock(pDb->file_mutex);
    return DBMS_ERROR;
  }

  ExecuteResult res = execute_statement(stmt, &pDb->catalog, pDb->pager);
  statement_free_children(stmt);
  free(stmt);
  if (pDb->pager->lock_error) {
    snprintf(pDb->last_error, sizeof(pDb->last_error), "Database is locked by another process");
    if (errmsg) *errmsg = strdup(pDb->last_error);
    pthread_mutex_unlock(&pDb->mutex);
    if (pDb->file_mutex) pthread_mutex_unlock(pDb->file_mutex);
    return DBMS_BUSY;
  }
  if (res != EXECUTE_SUCCESS) {
    snprintf(pDb->last_error, sizeof(pDb->last_error), "SQL execution error (%d)", res);
    if (errmsg) *errmsg = strdup(pDb->last_error);
    pthread_mutex_unlock(&pDb->mutex);
    if (pDb->file_mutex) pthread_mutex_unlock(pDb->file_mutex);
    return DBMS_ERROR;
  }

  catalog_load(&pDb->catalog, pDb->pager);
  pthread_mutex_unlock(&pDb->mutex);
  if (pDb->file_mutex) pthread_mutex_unlock(pDb->file_mutex);
  return DBMS_OK;
}

int dbms_prepare_v2(dbms* pDb, const char* zSql, int nByte, dbms_stmt** ppStmt, const char** pzTail) {
  if (pDb == NULL || zSql == NULL || ppStmt == NULL) return DBMS_MISUSE;

  if (pDb->file_mutex) pthread_mutex_lock(pDb->file_mutex);
  pthread_mutex_lock(&pDb->mutex);
  catalog_load(&pDb->catalog, pDb->pager);

  char sql_buf[16384];
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
  for (int k = 0; k < MAX_COLUMNS; k++) {
    value_init(&stmt->stmt.bound_values[k]);
  }
  memcpy(stmt->sql, sql_buf, sizeof(stmt->sql) - 1);
  stmt->sql[sizeof(stmt->sql) - 1] = '\0';

  PrepareResult prep = prepare_statement(sql_buf, &stmt->stmt);
  if (prep != PREPARE_SUCCESS) {
    snprintf(pDb->last_error, sizeof(pDb->last_error), "Prepare error");
    value_free_row(stmt->stmt.bound_values, MAX_COLUMNS);
    free(stmt);
    *ppStmt = NULL;
    pthread_mutex_unlock(&pDb->mutex);
    if (pDb->file_mutex) pthread_mutex_unlock(pDb->file_mutex);
    return DBMS_ERROR;
  }

  /* Pre-compile VDBE bytecode for STATEMENT_INSERT */
  if (stmt->stmt.type == STATEMENT_INSERT) {
    TableDef* def = catalog_find(&pDb->catalog, stmt->stmt.table_name);
    if (def != NULL) {
      Vdbe* vm = vdbe_create(pDb->pager, &pDb->catalog);
      Value tbl_name_val;
      value_init(&tbl_name_val);
      value_set_text(&tbl_name_val, def->name);
      vdbe_add_inst(vm, OP_OpenWrite, 0, def->root_page_num, 0, tbl_name_val);
      value_free(&tbl_name_val);

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

  pthread_mutex_unlock(&pDb->mutex);
  if (pDb->file_mutex) pthread_mutex_unlock(pDb->file_mutex);
  return DBMS_OK;
}

int dbms_bind_int(dbms_stmt* pStmt, int index, int value) {
  if (pStmt == NULL || index < 1 || index > MAX_COLUMNS) return DBMS_MISUSE;
  pStmt->stmt.has_bound_values = true;
  pStmt->stmt.bound_values[index - 1].is_null = false;
  pStmt->stmt.bound_values[index - 1].int_val = value;
  snprintf(pStmt->stmt.raw_values[index - 1], MAX_RAW_VAL, "%d", value);
  return DBMS_OK;
}

int dbms_bind_double(dbms_stmt* pStmt, int index, double value) {
  if (pStmt == NULL || index < 1 || index > MAX_COLUMNS) return DBMS_MISUSE;
  pStmt->stmt.has_bound_values = true;
  pStmt->stmt.bound_values[index - 1].is_null = false;
  pStmt->stmt.bound_values[index - 1].double_val = value;
  snprintf(pStmt->stmt.raw_values[index - 1], MAX_RAW_VAL, "%.17g", value);
  return DBMS_OK;
}

int dbms_bind_text(dbms_stmt* pStmt, int index, const char* text, int len) {
  if (pStmt == NULL || index < 1 || index > MAX_COLUMNS) return DBMS_MISUSE;
  pStmt->stmt.has_bound_values = true;
  if (text == NULL) {
    pStmt->stmt.bound_values[index - 1].is_null = true;
    strcpy(pStmt->stmt.raw_values[index - 1], "NULL");
  } else {
    pStmt->stmt.bound_values[index - 1].is_null = false;
    if (len >= 0) {
      value_set_text_len(&pStmt->stmt.bound_values[index - 1], text, (uint32_t)len);
    } else {
      value_set_text(&pStmt->stmt.bound_values[index - 1], text);
    }
    snprintf(pStmt->stmt.raw_values[index - 1], MAX_RAW_VAL, "%.*s", MAX_RAW_VAL - 1, text);
  }
  return DBMS_OK;
}

int dbms_bind_blob(dbms_stmt* pStmt, int index, const void* blob, int len) {
  if (pStmt == NULL || index < 1 || index > MAX_COLUMNS) return DBMS_MISUSE;
  pStmt->stmt.has_bound_values = true;
  if (blob == NULL) {
    pStmt->stmt.bound_values[index - 1].is_null = true;
    strcpy(pStmt->stmt.raw_values[index - 1], "NULL");
  } else {
    pStmt->stmt.bound_values[index - 1].is_null = false;
    if (len >= 0) {
      value_set_text_len(&pStmt->stmt.bound_values[index - 1], (const char*)blob, (uint32_t)len);
    } else {
      value_set_text(&pStmt->stmt.bound_values[index - 1], (const char*)blob);
    }
    snprintf(pStmt->stmt.raw_values[index - 1], MAX_RAW_VAL, "%.*s", MAX_RAW_VAL - 1, (const char*)blob);
  }
  return DBMS_OK;
}

int dbms_bind_null(dbms_stmt* pStmt, int index) {
  if (pStmt == NULL || index < 1 || index > MAX_COLUMNS) return DBMS_MISUSE;
  pStmt->stmt.has_bound_values = true;
  pStmt->stmt.bound_values[index - 1].is_null = true;
  strcpy(pStmt->stmt.raw_values[index - 1], "NULL");
  return DBMS_OK;
}

int dbms_step(dbms_stmt* pStmt) {
  if (pStmt == NULL || pStmt->db == NULL) return DBMS_MISUSE;

  if (pStmt->db->file_mutex) pthread_mutex_lock(pStmt->db->file_mutex);
  pthread_mutex_lock(&pStmt->db->mutex);

  if (pStmt->stmt.type == STATEMENT_SELECT) {
    if (!pStmt->executed) {
      resolve_param_placeholders(&pStmt->stmt);
      pStmt->target_def = catalog_find(&pStmt->db->catalog, pStmt->stmt.table_name);
      if (pStmt->target_def == NULL) {
        pthread_mutex_unlock(&pStmt->db->mutex);
        if (pStmt->db->file_mutex) pthread_mutex_unlock(pStmt->db->file_mutex);
        return DBMS_ERROR;
      }

      pStmt->target_table = (Table){ pStmt->db->pager, pStmt->target_def };
      pStmt->btree_cur = btree_start(&pStmt->target_table);
      pStmt->executed = true;
    }

    while (pStmt->btree_cur && !pStmt->btree_cur->end_of_table) {
      value_free_row(pStmt->current_row_vals, MAX_COLUMNS);
      deserialize_row(pStmt->target_def, cursor_value(pStmt->btree_cur), pStmt->current_row_vals);
      cursor_advance(pStmt->btree_cur);

      if (pStmt->stmt.where_clause.has_where) {
        if (!eval_where_clause(pStmt->target_def, pStmt->current_row_vals, &pStmt->stmt.where_clause, &pStmt->db->catalog, pStmt->db->pager)) {
          continue;
        }
      }

      pStmt->has_current_row = true;
      pthread_mutex_unlock(&pStmt->db->mutex);
      if (pStmt->db->file_mutex) pthread_mutex_unlock(pStmt->db->file_mutex);
      return DBMS_ROW;
    }

    pStmt->has_current_row = false;
    value_free_row(pStmt->current_row_vals, MAX_COLUMNS);
    if (pStmt->btree_cur) {
      free(pStmt->btree_cur);
      pStmt->btree_cur = NULL;
    }
    pthread_mutex_unlock(&pStmt->db->mutex);
    if (pStmt->db->file_mutex) pthread_mutex_unlock(pStmt->db->file_mutex);
    return DBMS_DONE;
  }

  /* DML/DDL execution */
  if (!pStmt->executed) {
    /* Resolve bound '?' parameters before execution */
    resolve_param_placeholders(&pStmt->stmt);

    ExecuteResult res = execute_statement(&pStmt->stmt, &pStmt->db->catalog, pStmt->db->pager);
    pStmt->executed = true;
    if (res != EXECUTE_SUCCESS) {
      pthread_mutex_unlock(&pStmt->db->mutex);
      if (pStmt->db->file_mutex) pthread_mutex_unlock(pStmt->db->file_mutex);
      return DBMS_ERROR;
    }
  }

  pthread_mutex_unlock(&pStmt->db->mutex);
  if (pStmt->db->file_mutex) pthread_mutex_unlock(pStmt->db->file_mutex);
  return DBMS_DONE;
}

int dbms_reset(dbms_stmt* pStmt) {
  if (pStmt == NULL) return DBMS_MISUSE;
  if (pStmt->db && pStmt->db->file_mutex) pthread_mutex_lock(pStmt->db->file_mutex);
  if (pStmt->db) pthread_mutex_lock(&pStmt->db->mutex);
  if (pStmt->btree_cur) {
    free(pStmt->btree_cur);
    pStmt->btree_cur = NULL;
  }
  value_free_row(pStmt->current_row_vals, MAX_COLUMNS);
  pStmt->executed = false;
  pStmt->has_current_row = false;
  if (pStmt->db) pthread_mutex_unlock(&pStmt->db->mutex);
  if (pStmt->db && pStmt->db->file_mutex) pthread_mutex_unlock(pStmt->db->file_mutex);
  return DBMS_OK;
}

int dbms_finalize(dbms_stmt* pStmt) {
  if (pStmt == NULL) return DBMS_OK;
  dbms_reset(pStmt);
  if (pStmt->compiled_vm) {
    vdbe_free(pStmt->compiled_vm);
    pStmt->compiled_vm = NULL;
  }
  value_free_row(pStmt->current_row_vals, MAX_COLUMNS);
  value_free_row(pStmt->stmt.bound_values, MAX_COLUMNS);
  statement_free_children(&pStmt->stmt);
  free(pStmt);
  return DBMS_OK;
}

static int get_projected_col_idx(dbms_stmt* pStmt, int col) {
  if (pStmt->stmt.num_select_cols == 0) return col;
  if (col < 0 || col >= (int)pStmt->stmt.num_select_cols) return -1;
  const char* req_name = pStmt->stmt.select_cols[col].col_name;
  if (pStmt->target_def) {
    for (uint32_t c = 0; c < pStmt->target_def->num_cols; c++) {
      if (strcmp(pStmt->target_def->columns[c].name, req_name) == 0) {
        return (int)c;
      }
    }
  }
  return -1;
}

int dbms_column_count(dbms_stmt* pStmt) {
  if (pStmt == NULL || pStmt->target_def == NULL) return 0;
  if (pStmt->stmt.num_select_cols > 0) return pStmt->stmt.num_select_cols;
  return pStmt->target_def->num_cols;
}

const char* dbms_column_name(dbms_stmt* pStmt, int col) {
  if (pStmt == NULL || pStmt->target_def == NULL) return "";
  if (pStmt->stmt.num_select_cols > 0) {
    if (col < 0 || col >= (int)pStmt->stmt.num_select_cols) return "";
    return pStmt->stmt.select_cols[col].col_name;
  }
  if (col < 0 || col >= (int)pStmt->target_def->num_cols) return "";
  return pStmt->target_def->columns[col].name;
}

int dbms_column_type(dbms_stmt* pStmt, int col) {
  if (pStmt == NULL || pStmt->target_def == NULL) return 0;
  int actual = get_projected_col_idx(pStmt, col);
  if (actual < 0 || actual >= (int)pStmt->target_def->num_cols) return 0;
  return (int)pStmt->target_def->columns[actual].type;
}

int dbms_column_int(dbms_stmt* pStmt, int col) {
  if (pStmt == NULL || !pStmt->has_current_row) return 0;
  int actual = get_projected_col_idx(pStmt, col);
  if (actual < 0 || actual >= MAX_COLUMNS) return 0;
  if (pStmt->current_row_vals[actual].is_null) return 0;
  if (pStmt->target_def && actual < (int)pStmt->target_def->num_cols) {
    ColumnType type = pStmt->target_def->columns[actual].type;
    if (type == COL_FLOAT) return (int)pStmt->current_row_vals[actual].float_val;
    if (type == COL_DOUBLE || type == COL_NUMERIC || type == COL_DECIMAL) return (int)pStmt->current_row_vals[actual].double_val;
    if (type == COL_BOOL) return pStmt->current_row_vals[actual].bool_val ? 1 : 0;
    if (type == COL_TEXT || type == COL_VARCHAR) return atoi(pStmt->current_row_vals[actual].text_val);
  }
  return pStmt->current_row_vals[actual].int_val;
}

double dbms_column_double(dbms_stmt* pStmt, int col) {
  if (pStmt == NULL || !pStmt->has_current_row) return 0.0;
  int actual = get_projected_col_idx(pStmt, col);
  if (actual < 0 || actual >= MAX_COLUMNS) return 0.0;
  if (pStmt->current_row_vals[actual].is_null) return 0.0;
  if (pStmt->target_def && actual < (int)pStmt->target_def->num_cols) {
    ColumnType type = pStmt->target_def->columns[actual].type;
    if (type == COL_INT) return (double)pStmt->current_row_vals[actual].int_val;
    if (type == COL_FLOAT) return (double)pStmt->current_row_vals[actual].float_val;
    if (type == COL_BOOL) return pStmt->current_row_vals[actual].bool_val ? 1.0 : 0.0;
    if (type == COL_TEXT || type == COL_VARCHAR) return atof(pStmt->current_row_vals[actual].text_val);
  }
  return pStmt->current_row_vals[actual].double_val;
}

const char* dbms_column_text(dbms_stmt* pStmt, int col) {
  if (pStmt == NULL || !pStmt->has_current_row) return "";
  int actual = get_projected_col_idx(pStmt, col);
  if (actual < 0) {
    if (pStmt->stmt.num_select_cols > 0 && col >= 0 && col < (int)pStmt->stmt.num_select_cols) {
      static __thread char expr_buf[256];
      eval_expr_string(pStmt->stmt.select_cols[col].col_name, pStmt->target_def, pStmt->current_row_vals, expr_buf, sizeof(expr_buf));
      return expr_buf;
    }
    return "";
  }
  if (actual >= MAX_COLUMNS) return "";
  if (pStmt->current_row_vals[actual].is_null) return "";
  if (pStmt->target_def && actual < (int)pStmt->target_def->num_cols) {
    if (pStmt->target_def->columns[actual].type == COL_INT) {
      static __thread char int_buf[32];
      snprintf(int_buf, sizeof(int_buf), "%d", pStmt->current_row_vals[actual].int_val);
      return int_buf;
    } else if (pStmt->target_def->columns[actual].type == COL_FLOAT) {
      static __thread char flt_buf[64];
      snprintf(flt_buf, sizeof(flt_buf), "%.8g", (double)pStmt->current_row_vals[actual].float_val);
      return flt_buf;
    } else if (pStmt->target_def->columns[actual].type == COL_DOUBLE || pStmt->target_def->columns[actual].type == COL_NUMERIC || pStmt->target_def->columns[actual].type == COL_DECIMAL) {
      static __thread char dbl_buf[64];
      snprintf(dbl_buf, sizeof(dbl_buf), "%.8g", pStmt->current_row_vals[actual].double_val);
      return dbl_buf;
    } else if (pStmt->target_def->columns[actual].type == COL_BOOL) {
      return pStmt->current_row_vals[actual].bool_val ? "true" : "false";
    }
  }
  return pStmt->current_row_vals[actual].text_val;
}

const void* dbms_column_blob(dbms_stmt* pStmt, int col) {
  if (pStmt == NULL || !pStmt->has_current_row) return "";
  int actual = get_projected_col_idx(pStmt, col);
  if (actual < 0 || actual >= MAX_COLUMNS) return "";
  return pStmt->current_row_vals[actual].text_val;
}

int dbms_column_bytes(dbms_stmt* pStmt, int col) {
  if (pStmt == NULL || !pStmt->has_current_row) return 0;
  int actual = get_projected_col_idx(pStmt, col);
  if (actual < 0 || actual >= MAX_COLUMNS) return 0;
  return (int)strlen(pStmt->current_row_vals[actual].text_val);
}

int64_t dbms_last_insert_rowid(dbms* pDb) {
  if (pDb == NULL) return 0;
  return pDb->last_rowid;
}

int dbms_changes(dbms* pDb) {
  if (pDb == NULL) return 0;
  return pDb->last_changes;
}
