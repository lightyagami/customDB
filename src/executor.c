#include "executor.h"
#include "vdbe.h"

/* Build a synthesised index-table name "_idx_<tbl>_<col>" into dst[IDX_NAME_SIZE].
 * Uses precision specifiers so gcc can prove the output always fits. */
static void make_idx_name(char dst[IDX_NAME_SIZE],
                          const char* tbl, const char* col) {
  snprintf(dst, IDX_NAME_SIZE, "_idx_%.31s_%.31s", tbl, col);
}

bool eval_where_clause(TableDef* def, Value* row_vals, WhereClause* wc, Catalog* catalog, Pager* pager);
static ExecuteResult execute_vacuum(Catalog* catalog, Pager* pager);
static ExecuteResult run_insert_vm(Statement* stmt, TableDef* def, Catalog* catalog, Pager* pager);
static ExecuteResult run_delete_vm(Statement* stmt, TableDef* def, Catalog* catalog, Pager* pager);
static ExecuteResult run_update_vm(Statement* stmt, TableDef* def, Catalog* catalog, Pager* pager);
static void print_returning_row(Statement* stmt, TableDef* def, Value* row_vals);

bool row_is_visible_and_active(TableDef* def, const void* row_bytes, uint64_t snapshot_xid, Value* out_values, uint64_t now_ts) {
  uint64_t expire_at = 0;
  uint64_t xmin = 0;
  uint64_t xmax = 0;

  deserialize_row_with_mvcc(def, (void*)row_bytes, out_values, &expire_at, &xmin, &xmax);

  /* 1. TTL check */
  if (expire_at > 0 && now_ts > 0 && (time_t)expire_at < (time_t)now_ts) {
    value_free_row(out_values, def->num_cols);
    return false;
  }

  /* 2. Creation visibility: row created after reader's snapshot */
  if (xmin > snapshot_xid) {
    value_free_row(out_values, def->num_cols);
    return false;
  }

  /* 3. Deletion visibility: row deleted before or at reader's snapshot */
  if (xmax != 0 && xmax <= snapshot_xid) {
    value_free_row(out_values, def->num_cols);
    return false;
  }

  return true;
}

static inline uint64_t get_current_snapshot_xid(Pager* pager) {
  if (pager->in_transaction) {
    if (pthread_equal(pager->writer_tid, pthread_self()) && pager->is_shadowed) {
      bool has_shadows = false;
      for (uint32_t i = 0; i < pager->shadow_capacity; i++) {
        if (pager->is_shadowed[i]) { has_shadows = true; break; }
      }
      if (has_shadows) {
        return pager->current_commit_lsn ? pager->current_commit_lsn : (pager->wal_lsn + 1);
      }
    }
    if (pager->tx_snapshot_xid > 0) {
      return pager->tx_snapshot_xid;
    }
  }
  return pager->wal_lsn;
}

typedef struct {
  char    alias[64];
  char    filename[256];
  Pager*  pager;
  Catalog catalog;
  bool    active;
} AttachedDb;

#define MAX_ATTACHED_DBS 8
static AttachedDb s_attached_dbs[MAX_ATTACHED_DBS];

static AttachedDb* find_attached_db(const char* alias) {
  for (int i = 0; i < MAX_ATTACHED_DBS; i++) {
    if (s_attached_dbs[i].active && strcasecmp(s_attached_dbs[i].alias, alias) == 0) {
      return &s_attached_dbs[i];
    }
  }
  return NULL;
}

/* Compile and run transaction statement on VDBE */
static ExecuteResult run_transaction_vm(Statement* stmt, Catalog* catalog, Pager* pager) {
  Vdbe* vm = vdbe_create(pager, catalog);
  int tx_op = 0;
  if (stmt->type == STATEMENT_BEGIN)    tx_op = 1;
  if (stmt->type == STATEMENT_COMMIT)   tx_op = 2;
  if (stmt->type == STATEMENT_ROLLBACK) tx_op = 3;

  vdbe_add_inst(vm, OP_Transaction, tx_op, 0, 0, (Value){0});
  vdbe_add_inst(vm, OP_Halt, 0, 0, 0, (Value){0});

  vdbe_run(vm);
  vdbe_free(vm);
  return EXECUTE_SUCCESS;
}

static void expand_trigger_sql(const char* action_sql, TableDef* def,
                               Value* old_vals, Value* new_vals,
                               char* out_sql, size_t out_size) {
  size_t o = 0;
  const char* p = action_sql;

  while (*p && o < out_size - 1) {
    if (*p == '\'' || *p == '"') {
      char q = *p++;
      if (o < out_size - 1) out_sql[o++] = q;
      while (*p && *p != q && o < out_size - 1) {
        out_sql[o++] = *p++;
      }
      if (*p == q && o < out_size - 1) {
        out_sql[o++] = *p++;
      }
      continue;
    }

    bool is_new = (strncasecmp(p, "new.", 4) == 0 && (p == action_sql || (!isalnum((unsigned char)p[-1]) && p[-1] != '_')));
    bool is_old = (strncasecmp(p, "old.", 4) == 0 && (p == action_sql || (!isalnum((unsigned char)p[-1]) && p[-1] != '_')));

    if (is_new || is_old) {
      const char* col_start = p + 4;
      char col_name[COL_NAME_SIZE] = {0};
      size_t clen = 0;
      while (col_start[clen] && (isalnum((unsigned char)col_start[clen]) || col_start[clen] == '_') && clen < sizeof(col_name) - 1) {
        col_name[clen] = col_start[clen];
        clen++;
      }
      col_name[clen] = '\0';

      int col_idx = -1;
      if (def) {
        for (uint32_t c = 0; c < def->num_cols; c++) {
          if (strcasecmp(def->columns[c].name, col_name) == 0) {
            col_idx = (int)c;
            break;
          }
        }
      }

      if (col_idx >= 0) {
        Value* v = is_new ? (new_vals ? &new_vals[col_idx] : NULL) : (old_vals ? &old_vals[col_idx] : NULL);
        char val_str[256] = "NULL";
        if (v != NULL && !v->is_null) {
          switch (def->columns[col_idx].type) {
            case COL_INT:
              snprintf(val_str, sizeof(val_str), "%d", v->int_val);
              break;
            case COL_FLOAT:
              snprintf(val_str, sizeof(val_str), "%.8g", (double)v->float_val);
              break;
            case COL_DOUBLE:
            case COL_NUMERIC:
            case COL_DECIMAL:
              snprintf(val_str, sizeof(val_str), "%.8g", v->double_val);
              break;
            case COL_BOOL:
              snprintf(val_str, sizeof(val_str), "%d", v->bool_val ? 1 : 0);
              break;
            case COL_TEXT:
            case COL_VARCHAR:
            case COL_BLOB:
            case COL_DATE:
            case COL_TIME:
            case COL_DATETIME:
            case COL_TIMESTAMP:
            case COL_VECTOR:
              snprintf(val_str, sizeof(val_str), "'%s'", v->text_val ? v->text_val : "");
              break;
            default:
              snprintf(val_str, sizeof(val_str), "NULL");
              break;
          }
        }
        size_t vlen = strlen(val_str);
        if (o + vlen < out_size) {
          memcpy(out_sql + o, val_str, vlen);
          o += vlen;
        }
        p = col_start + clen;
        continue;
      }
    }

    out_sql[o++] = *p++;
  }
  out_sql[o] = '\0';
}

static void fire_triggers(Catalog* catalog, Pager* pager, TableDef* def,
                          Value* old_vals, Value* new_vals,
                          TriggerTiming timing, TriggerEvent event) {
  if (def == NULL) return;
  static __thread int s_trigger_depth = 0;
  if (s_trigger_depth >= 4) return;
  s_trigger_depth++;

  for (uint32_t i = 0; i < catalog->num_triggers; i++) {
    TriggerDef* t = &catalog->triggers[i];
    if (strcmp(t->target_table, def->name) == 0 && t->timing == timing && t->event == event) {
      char expanded_sql[1024];
      expand_trigger_sql(t->action_sql, def, old_vals, new_vals, expanded_sql, sizeof(expanded_sql));
      Statement* tr_stmt = malloc(sizeof(Statement));
      if (tr_stmt) {
        memset(tr_stmt, 0, sizeof(Statement));
        if (prepare_statement(expanded_sql, tr_stmt) == PREPARE_SUCCESS) {
          execute_statement(tr_stmt, catalog, pager);
        }
        free(tr_stmt);
      }
    }
  }

  /* Automated temporal audit snapshot for WITH HISTORY tables */
  if (def->with_history && timing == TRIGGER_AFTER) {
    char hist_tbl_name[TBL_NAME_SIZE];
    snprintf(hist_tbl_name, sizeof(hist_tbl_name), "_history_%.50s", def->name);
    TableDef* hist_def = catalog_find(catalog, hist_tbl_name);
    if (hist_def) {
      Statement hist_ins;
      memset(&hist_ins, 0, sizeof(Statement));
      hist_ins.type = STATEMENT_INSERT;
      snprintf(hist_ins.table_name, sizeof(hist_ins.table_name), "%s", hist_tbl_name);
      hist_ins.num_values = hist_def->num_cols;

      /* Column 0: history_id (0 for autoincrement) */
      snprintf(hist_ins.raw_values[0], MAX_RAW_VAL, "0");
      /* Column 1: history_action */
      const char* act_str = (event == TRIGGER_INSERT) ? "INSERT" : ((event == TRIGGER_UPDATE) ? "UPDATE" : "DELETE");
      snprintf(hist_ins.raw_values[1], MAX_RAW_VAL, "%s", act_str);
      /* Column 2: history_time */
      snprintf(hist_ins.raw_values[2], MAX_RAW_VAL, "%ld", (long)time(NULL));

      /* Snapshot values: new_vals for INSERT/UPDATE, old_vals for DELETE */
      Value* snap_vals = (event == TRIGGER_DELETE) ? old_vals : new_vals;
      for (uint32_t c = 0; c < def->num_cols && (3 + c) < hist_def->num_cols; c++) {
        uint32_t dest = 3 + c;
        if (!snap_vals || snap_vals[c].is_null) {
          hist_ins.raw_is_null[dest] = true;
          hist_ins.raw_values[dest][0] = '\0';
        } else {
          hist_ins.raw_is_null[dest] = false;
          switch (def->columns[c].type) {
            case COL_INT:
              snprintf(hist_ins.raw_values[dest], MAX_RAW_VAL, "%d", snap_vals[c].int_val);
              break;
            case COL_FLOAT:
              snprintf(hist_ins.raw_values[dest], MAX_RAW_VAL, "%.8g", (double)snap_vals[c].float_val);
              break;
            case COL_DOUBLE:
            case COL_NUMERIC:
            case COL_DECIMAL:
              snprintf(hist_ins.raw_values[dest], MAX_RAW_VAL, "%.8g", snap_vals[c].double_val);
              break;
            case COL_BOOL:
              snprintf(hist_ins.raw_values[dest], MAX_RAW_VAL, "%s", snap_vals[c].bool_val ? "true" : "false");
              break;
            default:
              snprintf(hist_ins.raw_values[dest], MAX_RAW_VAL, "%s", snap_vals[c].text_val ? snap_vals[c].text_val : "");
              break;
          }
        }
      }
      run_insert_vm(&hist_ins, hist_def, catalog, pager);
    }
  }

  s_trigger_depth--;
}

typedef struct {
  Value row[MAX_COLUMNS];
  Value sort_keys[4];
  ColumnType sort_types[4];
  CollationType sort_colls[4];
  bool sort_descs[4];
  uint32_t num_sort_keys;
} RowSortEntry;

static int compare_row_sort_entries(const void* a, const void* b) {
  const RowSortEntry* ra = (const RowSortEntry*)a;
  const RowSortEntry* rb = (const RowSortEntry*)b;
  uint32_t n_keys = (ra->num_sort_keys < rb->num_sort_keys) ? ra->num_sort_keys : rb->num_sort_keys;
  for (uint32_t k = 0; k < n_keys; k++) {
    ColumnType type_a = ra->sort_types[k];
    ColumnType type_b = rb->sort_types[k];
    int cmp = 0;
    if (ra->sort_keys[k].is_null || rb->sort_keys[k].is_null) {
      if (ra->sort_keys[k].is_null && rb->sort_keys[k].is_null) cmp = 0;
      else if (ra->sort_keys[k].is_null) cmp = -1;
      else cmp = 1;
    } else if ((type_a == COL_INT || type_a == COL_DOUBLE || type_a == COL_FLOAT || type_a == COL_NUMERIC || type_a == COL_DECIMAL) &&
               (type_b == COL_INT || type_b == COL_DOUBLE || type_b == COL_FLOAT || type_b == COL_NUMERIC || type_b == COL_DECIMAL)) {
      double va = (type_a == COL_INT) ? (double)ra->sort_keys[k].int_val :
                  ((type_a == COL_FLOAT) ? (double)ra->sort_keys[k].float_val : ra->sort_keys[k].double_val);
      double vb = (type_b == COL_INT) ? (double)rb->sort_keys[k].int_val :
                  ((type_b == COL_FLOAT) ? (double)rb->sort_keys[k].float_val : rb->sort_keys[k].double_val);
      cmp = (va > vb) - (va < vb);
    } else if (type_a == COL_BOOL && type_b == COL_BOOL) {
      bool va = ra->sort_keys[k].bool_val;
      bool vb = rb->sort_keys[k].bool_val;
      cmp = (va > vb) - (va < vb);
    } else {
      const char* sa = (type_a == COL_TEXT || type_a == COL_VARCHAR || type_a == COL_BLOB || type_a == COL_VECTOR) ? (ra->sort_keys[k].text_val ? ra->sort_keys[k].text_val : "") : "";
      const char* sb = (type_b == COL_TEXT || type_b == COL_VARCHAR || type_b == COL_BLOB || type_b == COL_VECTOR) ? (rb->sort_keys[k].text_val ? rb->sort_keys[k].text_val : "") : "";
      cmp = compare_strings_collated(sa, sb, ra->sort_colls[k]);
    }
    if (cmp != 0) {
      if (ra->sort_descs[k]) return -cmp;
      return cmp;
    }
  }
  return 0;
}

static bool sql_like_match(const char* pattern, const char* str, CollationType coll) {
  while (*pattern) {
    if (*pattern == '%') {
      pattern++;
      if (*pattern == '\0') return true;
      while (*str) {
        if (sql_like_match(pattern, str, coll)) return true;
        str++;
      }
      return false;
    }
    if (!*str) return false;
    if (*pattern == '_') {
      pattern++;
      str++;
      continue;
    }
    char p = *pattern;
    char s = *str;
    if (coll == COLL_NOCASE) {
      if (tolower((unsigned char)p) != tolower((unsigned char)s)) return false;
    } else {
      if (p != s) return false;
    }
    pattern++;
    str++;
  }
  return *str == '\0';
}

static bool sql_glob_match(const char* pattern, const char* str) {
  while (*pattern) {
    if (*pattern == '*') {
      pattern++;
      if (*pattern == '\0') return true;
      while (*str) {
        if (sql_glob_match(pattern, str)) return true;
        str++;
      }
      return false;
    }
    if (!*str) return false;
    if (*pattern == '?') {
      pattern++;
      str++;
      continue;
    }
    if (*pattern == '[') {
      /* Character class: [abc] or [a-z] */
      pattern++;
      bool invert = false;
      if (*pattern == '!' || *pattern == '^') {
        invert = true;
        pattern++;
      }
      bool matched = false;
      while (*pattern && *pattern != ']') {
        if (pattern[1] == '-' && pattern[2] && pattern[2] != ']') {
          char start = pattern[0];
          char end = pattern[2];
          if (*str >= start && *str <= end) matched = true;
          pattern += 3;
        } else {
          if (*str == *pattern) matched = true;
          pattern++;
        }
      }
      if (*pattern == ']') pattern++;
      if (invert) matched = !matched;
      if (!matched) return false;
      str++;
      continue;
    }
    if (*pattern != *str) return false;
    pattern++;
    str++;
  }
  return *str == '\0';
}

static ExecuteResult run_insert_vm(Statement* stmt, TableDef* def, Catalog* catalog, Pager* pager) {
  bool auto_tx = false;
  if (!pager->in_transaction) {
    pager_begin_transaction(pager);
    auto_tx = true;
  }
  if (!pager_ensure_write_lock(pager)) {
    if (auto_tx) pager_rollback(pager);
    return EXECUTE_BUSY;
  }

  fire_triggers(catalog, pager, def, NULL, NULL, TRIGGER_BEFORE, TRIGGER_INSERT);

  /* AUTOINCREMENT auto-assignment */
  if (def->num_cols > 0 && def->columns[0].is_autoincrement) {
    if (stmt->num_values < def->num_cols) {
      stmt->num_values = def->num_cols;
    }
    char* raw_id = stmt->raw_values[0];
    if (raw_id == NULL || strlen(raw_id) == 0 || atoi(raw_id) == 0 || strcasecmp(raw_id, "null") == 0) {
      Table tbl = { pager, def };
      Cursor* cur = btree_start(&tbl);
      int max_id = 0;
      while (!cur->end_of_table) {
        Value r_vals[MAX_COLUMNS];
        deserialize_row(def, cursor_value(cur), r_vals);
        if (r_vals[0].int_val > max_id) max_id = r_vals[0].int_val;
        cursor_advance(cur);
      }
      free(cur);
      snprintf(stmt->raw_values[0], MAX_RAW_VAL, "%d", max_id + 1);
    }
  }

  Vdbe* vm = vdbe_create(pager, catalog);

  Value tbl_name_val;
  value_init(&tbl_name_val);
  value_set_text(&tbl_name_val, def->name);
  vdbe_add_inst(vm, OP_OpenWrite, 0, def->root_page_num, 0, tbl_name_val);
  value_free(&tbl_name_val);
  
  /* Load insert values into registers 1 to N */
  for (uint32_t i = 0; i < def->num_cols; i++) {
    Column* col = &def->columns[i];
    char* raw = (i < stmt->num_values) ? stmt->raw_values[i] : NULL;
    Value v;
    value_init(&v);

    bool is_val_null = (raw == NULL || strlen(raw) == 0 || (i < stmt->num_values && stmt->raw_is_null[i]));

    /* DEFAULT constraint */
    if (is_val_null && col->has_default) {
      raw = col->default_val;
      is_val_null = false;
    }

    /* NOT NULL constraint */
    if (is_val_null && col->is_not_null) {
      vdbe_free(vm);
      if (auto_tx && pager->in_transaction) pager_rollback(pager);
      return EXECUTE_CONSTRAINT_NOT_NULL;
    }

    /* CHECK constraint validation */
    if (col->has_check && raw != NULL && strlen(raw) > 0 && !is_val_null) {
      Value val_check, filter_check;
      value_init(&val_check);
      value_init(&filter_check);

      if (col->type == COL_INT) {
        val_check.int_val = atoi(raw);
        filter_check.int_val = atoi(col->check_val);
      } else if (col->type == COL_FLOAT) {
        val_check.float_val = (float)atof(raw);
        filter_check.float_val = (float)atof(col->check_val);
      } else if (col->type == COL_DOUBLE) {
        val_check.double_val = atof(raw);
        filter_check.double_val = atof(col->check_val);
      } else if (col->type == COL_BOOL) {
        val_check.bool_val = (strcasecmp(raw, "true") == 0 || strcmp(raw, "1") == 0);
        filter_check.bool_val = (strcasecmp(col->check_val, "true") == 0 || strcmp(col->check_val, "1") == 0);
      } else {
        value_set_text(&val_check, raw);
        value_set_text(&filter_check, col->check_val);
      }

      int cmp = 0;
      if (col->type == COL_INT) cmp = (val_check.int_val > filter_check.int_val) - (val_check.int_val < filter_check.int_val);
      else if (col->type == COL_FLOAT) cmp = (val_check.float_val > filter_check.float_val) - (val_check.float_val < filter_check.float_val);
      else if (col->type == COL_DOUBLE) cmp = (val_check.double_val > filter_check.double_val) - (val_check.double_val < filter_check.double_val);
      else if (col->type == COL_BOOL) cmp = (val_check.bool_val > filter_check.bool_val) - (val_check.bool_val < filter_check.bool_val);
      else cmp = strcmp(val_check.text_val, filter_check.text_val);

      value_free(&val_check);
      value_free(&filter_check);

      bool pass = false;
      switch (col->check_op) {
        case OP_EQ:  pass = (cmp == 0); break;
        case OP_LT:  pass = (cmp < 0); break;
        case OP_GT:  pass = (cmp > 0); break;
        case OP_LTE: pass = (cmp <= 0); break;
        case OP_GTE: pass = (cmp >= 0); break;
        default:     pass = false; break;
      }
      if (!pass) {
        vdbe_free(vm);
        if (auto_tx && pager->in_transaction) pager_rollback(pager);
        return EXECUTE_CONSTRAINT_CHECK;
      }
    }

    /* FOREIGN KEY parent presence check */
    if (col->has_fk && raw != NULL && strlen(raw) > 0 && !is_val_null) {
      TableDef* target_def = catalog_find(catalog, col->fk_target_table);
      if (target_def == NULL) {
        vdbe_free(vm);
        if (auto_tx && pager->in_transaction) pager_rollback(pager);
        return EXECUTE_CONSTRAINT_FOREIGN_KEY;
      }
      Table target_table = { pager, target_def };
      Cursor* target_cursor = btree_start(&target_table);
      bool fk_found = false;
      Value target_val;
      memset(&target_val, 0, sizeof(Value));

      int target_col_idx = 0;
      for (uint32_t tc = 0; tc < target_def->num_cols; tc++) {
        if (strcmp(target_def->columns[tc].name, col->fk_target_col) == 0) {
          target_col_idx = (int)tc;
          break;
        }
      }

      while (!target_cursor->end_of_table) {
        Value row_vals[MAX_COLUMNS];
        deserialize_row(target_def, cursor_value(target_cursor), row_vals);
        Value* actual_target = &row_vals[target_col_idx];

        bool fk_eq = false;
        if (col->type == COL_INT) fk_eq = (atoi(raw) == actual_target->int_val);
        else if (col->type == COL_FLOAT) fk_eq = ((float)atof(raw) == actual_target->float_val);
        else if (col->type == COL_DOUBLE) fk_eq = (atof(raw) == actual_target->double_val);
        else if (col->type == COL_BOOL) fk_eq = ((strcasecmp(raw, "true") == 0 || strcmp(raw, "1") == 0) == actual_target->bool_val);
        else fk_eq = (strcmp(raw, actual_target->text_val) == 0);

        if (fk_eq) {
          fk_found = true;
          break;
        }
        cursor_advance(target_cursor);
      }
      free(target_cursor);

      if (!fk_found) {
        vdbe_free(vm);
        if (auto_tx && pager->in_transaction) pager_rollback(pager);
        return EXECUTE_CONSTRAINT_FOREIGN_KEY;
      }
    }

    if (stmt->has_bound_values) {
      Value* b = &stmt->bound_values[i];
      if (b->is_null) {
        vdbe_add_inst(vm, OP_Null, i + 1, 0, 0, (Value){0});
      } else {
        switch (col->type) {
          case COL_INT:
            vdbe_add_inst(vm, OP_Integer, b->int_val, 0, i + 1, (Value){0});
            break;
          case COL_FLOAT:
          case COL_DOUBLE:
          case COL_NUMERIC:
          case COL_DECIMAL:
            vdbe_add_inst(vm, OP_Double, 0, 0, i + 1, *b);
            break;
          case COL_BOOL:
            vdbe_add_inst(vm, OP_Integer, b->bool_val ? 1 : 0, 0, i + 1, (Value){0});
            break;
          default:
            vdbe_add_inst(vm, OP_String, 0, 0, i + 1, *b);
            break;
        }
      }
    } else if (is_val_null) {
      vdbe_add_inst(vm, OP_Null, i + 1, 0, 0, (Value){0});
    } else {
      switch (col->type) {
        case COL_INT:
          vdbe_add_inst(vm, OP_Integer, atoi(raw), 0, i + 1, (Value){0});
          break;
        case COL_FLOAT:
        case COL_DOUBLE:
        case COL_NUMERIC:
        case COL_DECIMAL:
          v.double_val = atof(raw);
          v.float_val = (float)v.double_val;
          vdbe_add_inst(vm, OP_Double, 0, 0, i + 1, v);
          break;
        case COL_BOOL:
          vdbe_add_inst(vm, OP_Integer, (strcasecmp(raw, "true") == 0 || strcmp(raw, "1") == 0) ? 1 : 0, 0, i + 1, (Value){0});
          break;
        case COL_BLOB:
        case COL_DATETIME:
        case COL_DATE:
        case COL_TIME:
        case COL_TIMESTAMP:
        case COL_TEXT:
        case COL_VARCHAR:
        case COL_VECTOR:
          if (col->type != COL_BLOB && col->type != COL_TEXT && col->type != COL_VECTOR && strlen(raw) > col->size) {
            vdbe_free(vm);
            if (auto_tx && pager->in_transaction) pager_rollback(pager);
            return EXECUTE_BAD_SCHEMA;
          }
          value_set_text(&v, raw);
          vdbe_add_inst(vm, OP_String, 0, 0, i + 1, v);
          value_free(&v);
          break;
      }
    }
  }

  uint64_t txn_xid = pager->current_commit_lsn ? pager->current_commit_lsn : (pager->wal_lsn + 1);

  /* Validate UNIQUE constraint across existing rows */
  for (uint32_t c = 1; c < def->num_cols; c++) {
    if (def->columns[c].is_unique) {
      char* raw_u = stmt->raw_values[c];
      if (raw_u != NULL && strlen(raw_u) > 0) {
        Table main_tbl = { pager, def };
        Cursor* u_cur = btree_start(&main_tbl);
        bool u_dup = false;
        while (!u_cur->end_of_table) {
          Value rvals[MAX_COLUMNS];
          uint64_t r_exp = 0, r_xmin = 0, r_xmax = 0;
          deserialize_row_with_mvcc(def, cursor_value(u_cur), rvals, &r_exp, &r_xmin, &r_xmax);
          /* Ignore dead rows */
          if (r_xmax != 0 && r_xmax <= txn_xid) {
            value_free_row(rvals, def->num_cols);
            cursor_advance(u_cur);
            continue;
          }
          Value* u_actual = &rvals[c];

          bool match = false;
          ColumnType utype = def->columns[c].type;
          if (utype == COL_INT) match = (atoi(raw_u) == u_actual->int_val);
          else if (utype == COL_FLOAT) match = ((float)atof(raw_u) == u_actual->float_val);
          else if (utype == COL_DOUBLE) match = (atof(raw_u) == u_actual->double_val);
          else if (utype == COL_BOOL) match = ((strcasecmp(raw_u, "true") == 0 || strcmp(raw_u, "1") == 0) == u_actual->bool_val);
          else match = (strcmp(raw_u, u_actual->text_val) == 0);

          value_free_row(rvals, def->num_cols);
          if (match) {
            u_dup = true;
            break;
          }
          cursor_advance(u_cur);
        }
        free(u_cur);
        if (u_dup) {
          vdbe_free(vm);
          if (auto_tx && pager->in_transaction) pager_rollback(pager);
          return EXECUTE_CONSTRAINT_UNIQUE;
        }
      }
    }
  }

  /* Check for duplicate primary key */
  if (def->num_pk_cols > 1) {
    /* Composite Primary Key duplicate check */
    Table main_tbl = { pager, def };
    Cursor* cur = btree_start(&main_tbl);
    bool dup_found = false;
    while (!cur->end_of_table) {
      Value r_vals[MAX_COLUMNS];
      uint64_t r_exp = 0, r_xmin = 0, r_xmax = 0;
      deserialize_row_with_mvcc(def, cursor_value(cur), r_vals, &r_exp, &r_xmin, &r_xmax);
      if (r_xmax != 0 && r_xmax <= txn_xid) {
        value_free_row(r_vals, def->num_cols);
        cursor_advance(cur);
        continue;
      }
      bool all_pk_match = true;
      for (uint32_t p = 0; p < def->num_pk_cols; p++) {
        int pk_col_idx = -1;
        for (uint32_t c = 0; c < def->num_cols; c++) {
          if (strcmp(def->columns[c].name, def->pk_cols[p]) == 0) {
            pk_col_idx = (int)c;
            break;
          }
        }
        if (pk_col_idx >= 0 && pk_col_idx < (int)stmt->num_values) {
          Value new_val;
          value_init(&new_val);
          if (def->columns[pk_col_idx].type == COL_INT) new_val.int_val = atoi(stmt->raw_values[pk_col_idx]);
          else if (def->columns[pk_col_idx].type == COL_FLOAT) new_val.float_val = (float)atof(stmt->raw_values[pk_col_idx]);
          else if (def->columns[pk_col_idx].type == COL_DOUBLE) new_val.double_val = atof(stmt->raw_values[pk_col_idx]);
          else value_set_text(&new_val, stmt->raw_values[pk_col_idx]);

          if (compare_values(def->columns[pk_col_idx].type, &r_vals[pk_col_idx], &new_val) != 0) {
            all_pk_match = false;
            value_free(&new_val);
            break;
          }
          value_free(&new_val);
        }
      }
      value_free_row(r_vals, def->num_cols);
      if (all_pk_match) {
        dup_found = true;
        break;
      }
      cursor_advance(cur);
    }
    free(cur);
    if (dup_found) {
      vdbe_free(vm);
      if (auto_tx && pager->in_transaction) pager_rollback(pager);
      return EXECUTE_DUPLICATE_KEY;
    }
  } else {
    /* Single Column 0 Primary Key duplicate check */
    Value target_pk;
    value_init(&target_pk);
    if (stmt->has_bound_values) {
      value_copy(&target_pk, &stmt->bound_values[0]);
    } else if (stmt->num_values > 0 && strlen(stmt->raw_values[0]) > 0) {
      if (def->columns[0].type == COL_INT) target_pk.int_val = atoi(stmt->raw_values[0]);
      else if (def->columns[0].type == COL_FLOAT) target_pk.float_val = (float)atof(stmt->raw_values[0]);
      else if (def->columns[0].type == COL_DOUBLE) target_pk.double_val = atof(stmt->raw_values[0]);
      else value_set_text(&target_pk, stmt->raw_values[0]);
    }

    Table main_tbl = { pager, def };
    Cursor cur;
    btree_find_out(&main_tbl, &target_pk, &cur);
    while (!cur.end_of_table) {
      Value existing_key;
      btree_key_value(&cur, &existing_key);
      if (compare_values(def->columns[0].type, &existing_key, &target_pk) != 0) {
        value_free(&existing_key);
        break;
      }
      value_free(&existing_key);

      Value r_vals[MAX_COLUMNS];
      uint64_t r_exp = 0, r_xmin = 0, r_xmax = 0;
      deserialize_row_with_mvcc(def, cursor_value(&cur), r_vals, &r_exp, &r_xmin, &r_xmax);
      value_free_row(r_vals, def->num_cols);

      /* If existing cell is live (xmax == 0 or xmax > txn_xid), conflict! */
      if (r_xmax == 0 || r_xmax > txn_xid) {
        vdbe_free(vm);
        if (stmt->conflict_action == CONFLICT_IGNORE) {
          value_free(&target_pk);
          if (auto_tx && pager->in_transaction) {
            catalog_save(catalog, pager);
            pager_commit(pager);
          }
          return EXECUTE_SUCCESS;
        }
        if (stmt->conflict_action == CONFLICT_REPLACE) {
          Statement* del_s = calloc(1, sizeof(Statement));
          if (del_s) {
            del_s->type = STATEMENT_DELETE;
            strcpy(del_s->table_name, def->name);
            del_s->where_clause.has_where = true;
            del_s->where_clause.num_conds = 1;
            strcpy(del_s->where_clause.conds[0].col_name, def->columns[0].name);
            del_s->where_clause.conds[0].op = OP_EQ;
            if (def->columns[0].type == COL_INT) snprintf(del_s->where_clause.conds[0].raw_val, sizeof(del_s->where_clause.conds[0].raw_val), "%d", target_pk.int_val);
            else snprintf(del_s->where_clause.conds[0].raw_val, sizeof(del_s->where_clause.conds[0].raw_val), "%s", target_pk.text_val);
            run_delete_vm(del_s, def, catalog, pager);
            free(del_s);
          }
          value_free(&target_pk);
          Statement* retry_s = calloc(1, sizeof(Statement));
          if (retry_s) {
            *retry_s = *stmt;
            retry_s->conflict_action = CONFLICT_ABORT;
            ExecuteResult res = run_insert_vm(retry_s, def, catalog, pager);
            free(retry_s);
            if (auto_tx && pager->in_transaction) {
              catalog_save(catalog, pager);
              pager_commit(pager);
            }
            return res;
          }
        }
        if (stmt->conflict_action == CONFLICT_UPDATE) {
          Statement* upd_s = calloc(1, sizeof(Statement));
          if (upd_s) {
            upd_s->type = STATEMENT_UPDATE;
            strcpy(upd_s->table_name, def->name);
            upd_s->num_set_pairs = stmt->num_set_pairs;
            memcpy(upd_s->set_pairs, stmt->set_pairs, sizeof(stmt->set_pairs));
            upd_s->where_clause.has_where = true;
            upd_s->where_clause.num_conds = 1;
            strcpy(upd_s->where_clause.conds[0].col_name, def->columns[0].name);
            upd_s->where_clause.conds[0].op = OP_EQ;
            if (def->columns[0].type == COL_INT) snprintf(upd_s->where_clause.conds[0].raw_val, sizeof(upd_s->where_clause.conds[0].raw_val), "%d", target_pk.int_val);
            else snprintf(upd_s->where_clause.conds[0].raw_val, sizeof(upd_s->where_clause.conds[0].raw_val), "%s", target_pk.text_val);
            ExecuteResult res = run_update_vm(upd_s, def, catalog, pager);
            free(upd_s);
            value_free(&target_pk);
            if (auto_tx && pager->in_transaction) {
              catalog_save(catalog, pager);
              pager_commit(pager);
            }
            return res;
          }
        }
        value_free(&target_pk);
        if (auto_tx && pager->in_transaction) {
          pager_rollback(pager);
        }
        return EXECUTE_DUPLICATE_KEY;
      }
      cursor_advance(&cur);
    }
    value_free(&target_pk);
  }

  uint64_t expire_at = 0;
  uint32_t ttl_sec = (stmt->expires_sec > 0) ? stmt->expires_sec : def->default_ttl;
  if (ttl_sec > 0) {
    expire_at = (uint64_t)time(NULL) + ttl_sec;
  }
  Value ttl_val;
  value_init(&ttl_val);
  ttl_val.double_val = (double)expire_at;
  vdbe_add_inst(vm, OP_Insert, 0, 1, 0, ttl_val);
  vdbe_add_inst(vm, OP_Halt, 0, 0, 0, (Value){0});

  vdbe_run(vm);
  vdbe_free(vm);

  /* Maintain secondary indexes (directly for simple index synchronization) */
  Value values[MAX_COLUMNS];
  memset(values, 0, sizeof(values));
  for (uint32_t i = 0; i < def->num_cols; i++) {
    Column* col = &def->columns[i];
    value_init(&values[i]);
    if (stmt->has_bound_values) {
      value_copy(&values[i], &stmt->bound_values[i]);
    } else {
      char* raw = stmt->raw_values[i];
      bool is_val_null = (raw == NULL || strlen(raw) == 0 || (i < stmt->num_values && stmt->raw_is_null[i]));
      if (is_val_null) {
        values[i].is_null = true;
      } else {
        values[i].is_null = false;
        switch (col->type) {
          case COL_INT:       values[i].int_val = atoi(raw); break;
          case COL_FLOAT:     values[i].float_val = (float)atof(raw); values[i].double_val = atof(raw); break;
          case COL_DOUBLE:
          case COL_NUMERIC:
          case COL_DECIMAL:   values[i].double_val = atof(raw); break;
          case COL_BOOL:      values[i].bool_val = (strcasecmp(raw, "true") == 0 || strcmp(raw, "1") == 0); break;
          case COL_BLOB:
          case COL_DATETIME:
          case COL_DATE:
          case COL_TIME:
          case COL_TIMESTAMP:
          case COL_TEXT:
          case COL_VARCHAR:
          case COL_VECTOR:    value_set_text(&values[i], raw); break;
        }
      }
    }
  }

  for (uint32_t c = 1; c < def->num_cols; c++) {
    if (def->columns[c].has_index) {
      Column* col = &def->columns[c];
      if (col->idx_is_partial) {
        WhereClause idx_where;
        memset(&idx_where, 0, sizeof(WhereClause));
        idx_where.has_where = true;
        idx_where.num_conds = 1;
        strcpy(idx_where.conds[0].col_name, col->idx_where_col);
        idx_where.conds[0].op = col->idx_where_op;
        strcpy(idx_where.conds[0].raw_val, col->idx_where_val);
        if (!eval_where_clause(def, values, &idx_where, catalog, pager)) {
          continue;
        }
      }

      TableDef idx_def;
      memset(&idx_def, 0, sizeof(TableDef));
      make_idx_name(idx_def.name, def->name, col->name);
      idx_def.root_page_num = col->index_root_page;
      idx_def.num_cols = 2;
      memcpy(&idx_def.columns[0], col, sizeof(Column));
      strcpy(idx_def.columns[1].name, "id");
      idx_def.columns[1].type = COL_INT;
      idx_def.columns[1].size = 4;
      tabledef_compute(&idx_def);

      Table idx_table = { pager, &idx_def };
      Value idx_values[2];
      value_init(&idx_values[0]);
      value_init(&idx_values[1]);
      value_copy(&idx_values[0], &values[c]);
      idx_values[0].is_null = values[c].is_null;
      if (col->idx_is_expr) {
        if (strcasecmp(col->idx_expr_func, "lower") == 0 && (col->type == COL_TEXT || col->type == COL_VARCHAR)) {
          for (char* p = idx_values[0].text_val; *p; p++) *p = (char)tolower((unsigned char)*p);
        } else if (strcasecmp(col->idx_expr_func, "upper") == 0 && (col->type == COL_TEXT || col->type == COL_VARCHAR)) {
          for (char* p = idx_values[0].text_val; *p; p++) *p = (char)toupper((unsigned char)*p);
        }
      }
      idx_values[1].int_val = values[0].int_val;
      idx_values[1].is_null = false;

      Cursor* idx_cur = btree_find(&idx_table, &idx_values[0]);
      btree_insert(idx_cur, idx_values);
      value_free_row(idx_values, 2);
      free(idx_cur);
    }
  }

  if (auto_tx && pager->in_transaction) {
    catalog_save(catalog, pager);
    pager_commit(pager);
  }
  fire_triggers(catalog, pager, def, NULL, values, TRIGGER_AFTER, TRIGGER_INSERT);
  print_returning_row(stmt, def, values);
  value_free_row(values, def->num_cols);
  return EXECUTE_SUCCESS;
}

/* Helper to generate constant comparison register in VM */
static void load_const_reg(Vdbe* vm, int const_reg, ColumnType type, const char* raw_val) {
  Value v;
  value_init(&v);
  switch (type) {
    case COL_INT:
      vdbe_add_inst(vm, OP_Integer, atoi(raw_val), 0, const_reg, (Value){0});
      vm->regs[const_reg].type = COL_INT;
      break;
    case COL_DOUBLE:
    case COL_FLOAT:
    case COL_NUMERIC:
    case COL_DECIMAL:
      v.double_val = atof(raw_val);
      vdbe_add_inst(vm, OP_Double, 0, 0, const_reg, v);
      vm->regs[const_reg].type = COL_DOUBLE;
      break;
    case COL_BOOL:
      vdbe_add_inst(vm, OP_Integer, (strcasecmp(raw_val, "true") == 0 || strcmp(raw_val, "1") == 0) ? 1 : 0, 0, const_reg, (Value){0});
      vm->regs[const_reg].type = COL_BOOL;
      break;
    case COL_BLOB:
    case COL_DATETIME:
    case COL_DATE:
    case COL_TIME:
    case COL_TIMESTAMP:
    case COL_TEXT:
    case COL_VARCHAR:
    case COL_VECTOR:
      value_set_text(&v, raw_val);
      vdbe_add_inst(vm, OP_String, 0, 0, const_reg, v);
      value_free(&v);
      vm->regs[const_reg].type = COL_VARCHAR;
      break;
  }
}

static const char* skip_space(const char* s) {
  while (*s && isspace((unsigned char)*s)) s++;
  return s;
}

static void eval_json_extract(const char* json_str, const char* path, char* out_buf, size_t out_size) {
  out_buf[0] = '\0';
  if (!json_str || !path) return;

  const char* target_key = path;
  if (strncmp(target_key, "$.", 2) == 0) target_key += 2;

  const char* p = json_str;
  char key_pattern[256];
  snprintf(key_pattern, sizeof(key_pattern), "\"%s\"", target_key);

  const char* key_pos = strstr(p, key_pattern);
  if (!key_pos) {
    char sub_key[64];
    const char* dot = strchr(target_key, '.');
    if (dot) {
      uint32_t len = dot - target_key;
      if (len < sizeof(sub_key)) {
        strncpy(sub_key, target_key, len);
        sub_key[len] = '\0';
        snprintf(key_pattern, sizeof(key_pattern), "\"%s\"", sub_key);
        key_pos = strstr(p, key_pattern);
        if (key_pos) {
          p = key_pos + strlen(key_pattern);
          snprintf(key_pattern, sizeof(key_pattern), "\"%s\"", dot + 1);
          key_pos = strstr(p, key_pattern);
        }
      }
    }
  }

  if (key_pos) {
    p = key_pos + strlen(key_pattern);
    while (*p && (*p == ' ' || *p == ':' || *p == '\t')) p++;
    if (*p == '"') {
      p++;
      uint32_t o_idx = 0;
      while (*p && *p != '"' && o_idx < out_size - 1) {
        out_buf[o_idx++] = *p++;
      }
      out_buf[o_idx] = '\0';
    } else {
      uint32_t o_idx = 0;
      while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && o_idx < out_size - 1) {
        out_buf[o_idx++] = *p++;
      }
      out_buf[o_idx] = '\0';
    }
  }
}

/* Parse a vector string "[1.0, 2.0, 3.5]" into an array of floats */
static int parse_vector_string(const char* str, float* out, int max_dim) {
  if (!str) return 0;
  const char* p = str;
  while (*p && (*p == ' ' || *p == '[' || *p == '\'' || *p == '"')) p++;
  int count = 0;
  while (*p && *p != ']' && *p != '\'' && *p != '"' && count < max_dim) {
    char* next_p = NULL;
    float val = strtof(p, &next_p);
    if (next_p == p) break;
    out[count++] = val;
    p = next_p;
    while (*p && (*p == ' ' || *p == ',')) p++;
  }
  return count;
}

static double compute_l2_distance(const float* v1, const float* v2, int dim) {
  double sum = 0.0;
  for (int i = 0; i < dim; i++) {
    double diff = (double)v1[i] - (double)v2[i];
    sum += diff * diff;
  }
  return sqrt(sum);
}

static double compute_cosine_similarity(const float* v1, const float* v2, int dim) {
  double dot = 0.0, norm1 = 0.0, norm2 = 0.0;
  for (int i = 0; i < dim; i++) {
    dot += (double)v1[i] * (double)v2[i];
    norm1 += (double)v1[i] * (double)v1[i];
    norm2 += (double)v2[i] * (double)v2[i];
  }
  if (norm1 <= 0.0 || norm2 <= 0.0) return 0.0;
  return dot / (sqrt(norm1) * sqrt(norm2));
}

void eval_expr_string(const char* expr, TableDef* def, Value* row_vals, char* out_buf, size_t out_size) {
  out_buf[0] = '\0';
  if (!expr || strlen(expr) == 0) return;

  /* random() */
  if (strcasecmp(expr, "random()") == 0) {
    static bool s_seeded = false;
    if (!s_seeded) {
      srand((unsigned int)(time(NULL) ^ (uintptr_t)&s_seeded));
      s_seeded = true;
    }
    long long r = ((long long)rand() << 32) | (long long)rand();
    snprintf(out_buf, out_size, "%lld", r);
    return;
  }

  /* abs(x) */
  if (strncasecmp(expr, "abs(", 4) == 0) {
    const char* p = expr + 4;
    while (*p && isspace((unsigned char)*p)) p++;
    char arg[COL_NAME_SIZE] = {0};
    uint32_t a_idx = 0;
    while (*p && *p != ')' && !isspace((unsigned char)*p) && a_idx < sizeof(arg) - 1) {
      arg[a_idx++] = *p++;
    }
    arg[a_idx] = '\0';
    double val = 0.0;
    bool found = false;
    if (def && row_vals) {
      for (uint32_t c = 0; c < def->num_cols; c++) {
        if (strcasecmp(def->columns[c].name, arg) == 0) {
          if (def->columns[c].type == COL_INT) val = (double)abs(row_vals[c].int_val);
          else val = fabs(row_vals[c].double_val);
          found = true;
          break;
        }
      }
    }
    if (!found) {
      val = fabs(atof(arg));
    }
    if ((double)(long long)val == val) snprintf(out_buf, out_size, "%lld", (long long)val);
    else snprintf(out_buf, out_size, "%.8g", val);
    return;
  }

  /* length(x) */
  if (strncasecmp(expr, "length(", 7) == 0) {
    const char* p = expr + 7;
    while (*p && isspace((unsigned char)*p)) p++;
    char arg[COL_NAME_SIZE] = {0};
    uint32_t a_idx = 0;
    while (*p && *p != ')' && !isspace((unsigned char)*p) && a_idx < sizeof(arg) - 1) {
      arg[a_idx++] = *p++;
    }
    arg[a_idx] = '\0';
    if (def && row_vals) {
      for (uint32_t c = 0; c < def->num_cols; c++) {
        if (strcasecmp(def->columns[c].name, arg) == 0) {
          snprintf(out_buf, out_size, "%zu", strlen(row_vals[c].text_val ? row_vals[c].text_val : ""));
          return;
        }
      }
    }
    snprintf(out_buf, out_size, "%zu", strlen(arg));
    return;
  }

  /* typeof(x) */
  if (strncasecmp(expr, "typeof(", 7) == 0) {
    const char* p = expr + 7;
    while (*p && isspace((unsigned char)*p)) p++;
    char arg[COL_NAME_SIZE] = {0};
    uint32_t a_idx = 0;
    while (*p && *p != ')' && !isspace((unsigned char)*p) && a_idx < sizeof(arg) - 1) {
      arg[a_idx++] = *p++;
    }
    arg[a_idx] = '\0';
    if (def && row_vals) {
      int c_idx = -1;
      for (uint32_t c = 0; c < def->num_cols; c++) {
        if (strcasecmp(def->columns[c].name, arg) == 0) { c_idx = (int)c; break; }
      }
      if (c_idx >= 0) {
        if (row_vals[c_idx].is_null) {
          snprintf(out_buf, out_size, "null");
        } else {
          switch (def->columns[c_idx].type) {
            case COL_INT: snprintf(out_buf, out_size, "integer"); break;
            case COL_FLOAT:
            case COL_DOUBLE:
            case COL_NUMERIC:
            case COL_DECIMAL: snprintf(out_buf, out_size, "real"); break;
            case COL_BLOB: snprintf(out_buf, out_size, "blob"); break;
            default: snprintf(out_buf, out_size, "text"); break;
          }
        }
        return;
      }
    }
    /* Literal or untyped argument */
    if (strcasecmp(arg, "null") == 0) {
      snprintf(out_buf, out_size, "null");
    } else if (arg[0] == '\'' || arg[0] == '"') {
      snprintf(out_buf, out_size, "text");
    } else if (strchr(arg, '.') != NULL) {
      snprintf(out_buf, out_size, "real");
    } else {
      bool all_digits = (strlen(arg) > 0);
      for (size_t i = (arg[0] == '-' ? 1 : 0); arg[i]; i++) {
        if (!isdigit((unsigned char)arg[i])) { all_digits = false; break; }
      }
      if (all_digits) snprintf(out_buf, out_size, "integer");
      else snprintf(out_buf, out_size, "text");
    }
    return;
  }

  /* CAST(x AS type) */
  if (strncasecmp(expr, "cast(", 5) == 0) {
    const char* p = expr + 5;
    while (*p && isspace((unsigned char)*p)) p++;
    const char* as_ptr = NULL;
    for (const char* s = p; *s; s++) {
      if (strncasecmp(s, "as", 2) == 0 && isspace((unsigned char)s[-1]) && isspace((unsigned char)s[2])) {
        as_ptr = s;
        break;
      }
    }
    if (as_ptr) {
      char val_tok[256] = {0};
      size_t vlen = as_ptr - p;
      while (vlen > 0 && isspace((unsigned char)p[vlen - 1])) vlen--;
      if (vlen < sizeof(val_tok)) {
        strncpy(val_tok, p, vlen);
        val_tok[vlen] = '\0';
      }
      const char* t_ptr = as_ptr + 2;
      while (*t_ptr && isspace((unsigned char)*t_ptr)) t_ptr++;
      char target_type[64] = {0};
      uint32_t t_idx = 0;
      while (*t_ptr && *t_ptr != ')' && !isspace((unsigned char)*t_ptr) && t_idx < sizeof(target_type) - 1) {
        target_type[t_idx++] = *t_ptr++;
      }
      target_type[t_idx] = '\0';

      char raw_content[MAX_RAW_VAL] = {0};
      if (def && row_vals) {
        for (uint32_t c = 0; c < def->num_cols; c++) {
          if (strcasecmp(def->columns[c].name, val_tok) == 0) {
            if (row_vals[c].is_null) {
              snprintf(out_buf, out_size, "NULL");
              return;
            }
            if (def->columns[c].type == COL_INT) snprintf(raw_content, sizeof(raw_content), "%d", row_vals[c].int_val);
            else if (def->columns[c].type == COL_FLOAT || def->columns[c].type == COL_DOUBLE) snprintf(raw_content, sizeof(raw_content), "%.8g", row_vals[c].double_val);
            else snprintf(raw_content, sizeof(raw_content), "%s", row_vals[c].text_val);
            break;
          }
        }
      }
      if (strlen(raw_content) == 0) {
        if (val_tok[0] == '\'' || val_tok[0] == '"') {
          char q = val_tok[0];
          size_t end = strlen(val_tok);
          if (end > 1 && val_tok[end - 1] == q) {
            strncpy(raw_content, val_tok + 1, end - 2);
            raw_content[end - 2] = '\0';
          } else {
            strncpy(raw_content, val_tok, sizeof(raw_content) - 1);
          }
        } else {
          strncpy(raw_content, val_tok, sizeof(raw_content) - 1);
        }
      }

      if (strcasecmp(target_type, "text") == 0 || strcasecmp(target_type, "varchar") == 0) {
        snprintf(out_buf, out_size, "%s", raw_content);
      } else if (strcasecmp(target_type, "integer") == 0 || strcasecmp(target_type, "int") == 0) {
        snprintf(out_buf, out_size, "%d", atoi(raw_content));
      } else if (strcasecmp(target_type, "real") == 0 || strcasecmp(target_type, "float") == 0 || strcasecmp(target_type, "double") == 0) {
        double dv = atof(raw_content);
        snprintf(out_buf, out_size, "%.8g", dv);
      } else {
        snprintf(out_buf, out_size, "%s", raw_content);
      }
      return;
    }
  }

  time_t t = time(NULL);
  struct tm* tm_info = localtime(&t);

  if (strcasecmp(expr, "datetime('now')") == 0 || strcasecmp(expr, "datetime()") == 0) {
    strftime(out_buf, out_size, "%Y-%m-%d %H:%M:%S", tm_info);
    return;
  }
  if (strcasecmp(expr, "date('now')") == 0 || strcasecmp(expr, "date()") == 0) {
    strftime(out_buf, out_size, "%Y-%m-%d", tm_info);
    return;
  }
  if (strcasecmp(expr, "time('now')") == 0 || strcasecmp(expr, "time()") == 0) {
    strftime(out_buf, out_size, "%H:%M:%S", tm_info);
    return;
  }
  if (strncasecmp(expr, "strftime(", 9) == 0) {
    char fmt[64] = "%Y-%m-%d %H:%M:%S";
    const char* p = expr + 9;
    if (*p == '\'' || *p == '"') {
      p++;
      uint32_t f_idx = 0;
      while (*p && *p != '\'' && *p != '"' && f_idx < sizeof(fmt) - 1) {
        fmt[f_idx++] = *p++;
      }
      fmt[f_idx] = '\0';
    }
    strftime(out_buf, out_size, fmt, tm_info);
    return;
  }

  if (strncasecmp(expr, "json_extract(", 13) == 0) {
    const char* p = expr + 13;
    p = skip_space(p);
    char arg1[MAX_TEXT_SIZE] = {0};
    char arg2[128] = {0};

    if (*p == '\'' || *p == '"') {
      char q = *p++;
      uint32_t a_idx = 0;
      while (*p && *p != q && a_idx < sizeof(arg1) - 1) arg1[a_idx++] = *p++;
      if (*p == q) p++;
    } else {
      uint32_t a_idx = 0;
      while (*p && *p != ',' && a_idx < sizeof(arg1) - 1) arg1[a_idx++] = *p++;
      arg1[a_idx] = '\0';
      if (def && row_vals) {
        for (uint32_t c = 0; c < def->num_cols; c++) {
          if (strcmp(def->columns[c].name, arg1) == 0) {
            snprintf(arg1, sizeof(arg1), "%s", row_vals[c].text_val);
            break;
          }
        }
      }
    }

    p = skip_space(p);
    if (*p == ',') p++;
    p = skip_space(p);

    if (*p == '\'' || *p == '"') {
      char q = *p++;
      uint32_t a_idx = 0;
      while (*p && *p != q && a_idx < sizeof(arg2) - 1) arg2[a_idx++] = *p++;
    }

    eval_json_extract(arg1, arg2, out_buf, out_size);
    return;
  }

  if (strncasecmp(expr, "json_array(", 11) == 0) {
    snprintf(out_buf, out_size, "[%s]", expr + 11);
    char* end_paren = strrchr(out_buf, ')');
    if (end_paren) *end_paren = ']';
    return;
  }

  if (strncasecmp(expr, "json_object(", 12) == 0) {
    snprintf(out_buf, out_size, "{%s}", expr + 12);
    char* end_paren = strrchr(out_buf, ')');
    if (end_paren) *end_paren = '}';
    return;
  }

  /* l2_distance(v1, v2) and cosine_similarity(v1, v2) */
  if (strncasecmp(expr, "l2_distance(", 12) == 0 || strncasecmp(expr, "cosine_similarity(", 18) == 0) {
    bool is_l2 = (strncasecmp(expr, "l2_distance(", 12) == 0);
    const char* p = expr + (is_l2 ? 12 : 18);
    p = skip_space(p);

    char arg1_str[MAX_RAW_VAL] = {0};
    char arg2_str[MAX_RAW_VAL] = {0};

    /* Parse first argument (could be column name or literal '[1, 2, ...]') */
    if (*p == '\'' || *p == '"') {
      char q = *p++;
      uint32_t a_idx = 0;
      while (*p && *p != q && a_idx < sizeof(arg1_str) - 1) arg1_str[a_idx++] = *p++;
      if (*p == q) p++;
    } else if (*p == '[') {
      int depth = 0;
      uint32_t a_idx = 0;
      while (*p && a_idx < sizeof(arg1_str) - 1) {
        if (*p == '[') depth++;
        else if (*p == ']') {
          depth--;
          arg1_str[a_idx++] = *p++;
          if (depth <= 0) break;
          continue;
        }
        arg1_str[a_idx++] = *p++;
      }
    } else {
      uint32_t a_idx = 0;
      while (*p && *p != ',' && a_idx < sizeof(arg1_str) - 1) arg1_str[a_idx++] = *p++;
      while (a_idx > 0 && isspace((unsigned char)arg1_str[a_idx - 1])) arg1_str[--a_idx] = '\0';
      if (def && row_vals) {
        for (uint32_t c = 0; c < def->num_cols; c++) {
          if (strcasecmp(def->columns[c].name, arg1_str) == 0) {
            snprintf(arg1_str, sizeof(arg1_str), "%s", row_vals[c].text_val ? row_vals[c].text_val : "");
            break;
          }
        }
      }
    }

    p = skip_space(p);
    if (*p == ',') p++;
    p = skip_space(p);

    /* Parse second argument */
    if (*p == '\'' || *p == '"') {
      char q = *p++;
      uint32_t a_idx = 0;
      while (*p && *p != q && a_idx < sizeof(arg2_str) - 1) arg2_str[a_idx++] = *p++;
      if (*p == q) p++;
    } else if (*p == '[') {
      int depth = 0;
      uint32_t a_idx = 0;
      while (*p && a_idx < sizeof(arg2_str) - 1) {
        if (*p == '[') depth++;
        else if (*p == ']') {
          depth--;
          arg2_str[a_idx++] = *p++;
          if (depth <= 0) break;
          continue;
        }
        arg2_str[a_idx++] = *p++;
      }
    } else {
      uint32_t a_idx = 0;
      while (*p && *p != ')' && a_idx < sizeof(arg2_str) - 1) arg2_str[a_idx++] = *p++;
      while (a_idx > 0 && isspace((unsigned char)arg2_str[a_idx - 1])) arg2_str[--a_idx] = '\0';
      if (def && row_vals) {
        for (uint32_t c = 0; c < def->num_cols; c++) {
          if (strcasecmp(def->columns[c].name, arg2_str) == 0) {
            snprintf(arg2_str, sizeof(arg2_str), "%s", row_vals[c].text_val ? row_vals[c].text_val : "");
            break;
          }
        }
      }
    }

    float vec1[1024];
    float vec2[1024];
    int dim1 = parse_vector_string(arg1_str, vec1, 1024);
    int dim2 = parse_vector_string(arg2_str, vec2, 1024);
    int dim = (dim1 < dim2) ? dim1 : dim2;

    if (dim > 0) {
      double res = is_l2 ? compute_l2_distance(vec1, vec2, dim) : compute_cosine_similarity(vec1, vec2, dim);
      snprintf(out_buf, out_size, "%.6g", res);
    } else {
      snprintf(out_buf, out_size, "0");
    }
    return;
  }

  if (strncasecmp(expr, "case", 4) == 0 && isspace((unsigned char)expr[4])) {
    const char* p = expr + 4;
    p = skip_space(p);
    bool matched = false;
    while (strncasecmp(p, "when", 4) == 0 && isspace((unsigned char)p[4])) {
      p += 4;
      p = skip_space(p);
      char col_tok[COL_NAME_SIZE] = {0};
      uint32_t c_idx = 0;
      while (*p && !isspace((unsigned char)*p) && *p != '=' && *p != '>' && *p != '<' && c_idx < sizeof(col_tok)-1) {
        col_tok[c_idx++] = *p++;
      }
      p = skip_space(p);
      char op_tok[4] = {0};
      uint32_t o_idx = 0;
      while (*p && (*p == '=' || *p == '>' || *p == '<' || *p == '!') && o_idx < sizeof(op_tok)-1) {
        op_tok[o_idx++] = *p++;
      }
      p = skip_space(p);
      char val_tok[MAX_RAW_VAL] = {0};
      uint32_t v_idx = 0;
      if (*p == '\'' || *p == '"') {
        char q = *p++;
        while (*p && *p != q && v_idx < sizeof(val_tok)-1) val_tok[v_idx++] = *p++;
        if (*p == q) p++;
      } else {
        while (*p && !isspace((unsigned char)*p) && v_idx < sizeof(val_tok)-1) val_tok[v_idx++] = *p++;
      }

      bool cond_true = false;
      if (def && row_vals) {
        int target_col = -1;
        for (uint32_t c = 0; c < def->num_cols; c++) {
          if (strcmp(def->columns[c].name, col_tok) == 0) { target_col = (int)c; break; }
        }
        if (target_col != -1) {
          Value* cv = &row_vals[target_col];
          int cmp = 0;
          if (def->columns[target_col].type == COL_INT) {
            int iv = atoi(val_tok);
            cmp = (cv->int_val > iv) - (cv->int_val < iv);
          } else if (def->columns[target_col].type == COL_DOUBLE || def->columns[target_col].type == COL_FLOAT) {
            double dv = atof(val_tok);
            cmp = (cv->double_val > dv) - (cv->double_val < dv);
          } else {
            cmp = strcmp(cv->text_val, val_tok);
          }
          if (strcmp(op_tok, "=") == 0 || strcmp(op_tok, "==") == 0) cond_true = (cmp == 0);
          else if (strcmp(op_tok, ">") == 0) cond_true = (cmp > 0);
          else if (strcmp(op_tok, "<") == 0) cond_true = (cmp < 0);
          else if (strcmp(op_tok, ">=") == 0) cond_true = (cmp >= 0);
          else if (strcmp(op_tok, "<=") == 0) cond_true = (cmp <= 0);
          else if (strcmp(op_tok, "!=") == 0 || strcmp(op_tok, "<>") == 0) cond_true = (cmp != 0);
        }
      }

      p = skip_space(p);
      if (strncasecmp(p, "then", 4) == 0) {
        p += 4;
        p = skip_space(p);
        char then_val[MAX_RAW_VAL] = {0};
        uint32_t t_idx = 0;
        if (*p == '\'' || *p == '"') {
          char q = *p++;
          while (*p && *p != q && t_idx < sizeof(then_val)-1) then_val[t_idx++] = *p++;
          if (*p == q) p++;
        } else {
          while (*p && !isspace((unsigned char)*p) && t_idx < sizeof(then_val)-1) then_val[t_idx++] = *p++;
        }
        if (cond_true && !matched) {
          snprintf(out_buf, out_size, "%s", then_val);
          matched = true;
        }
      }
      p = skip_space(p);
    }
    if (strncasecmp(p, "else", 4) == 0) {
      p += 4;
      p = skip_space(p);
      char else_val[MAX_RAW_VAL] = {0};
      uint32_t e_idx = 0;
      if (*p == '\'' || *p == '"') {
        char q = *p++;
        while (*p && *p != q && e_idx < sizeof(else_val)-1) else_val[e_idx++] = *p++;
        if (*p == q) p++;
      } else {
        while (*p && !isspace((unsigned char)*p) && e_idx < sizeof(else_val)-1) else_val[e_idx++] = *p++;
      }
      if (!matched) {
        snprintf(out_buf, out_size, "%s", else_val);
        matched = true;
      }
    }
    return;
  }

  /* Arithmetic expressions: +, -, *, / */
  const char* op_ptr = NULL;
  char op_char = 0;
  for (const char* ch = expr; *ch; ch++) {
    if (*ch == '+' || *ch == '-' || *ch == '*' || *ch == '/') {
      if (ch > expr && (ch[-1] == ' ' || isalnum(ch[-1])) && (ch[1] == ' ' || isalnum(ch[1]))) {
        op_ptr = ch;
        op_char = *ch;
        break;
      }
    }
  }
  if (op_ptr != NULL) {
    char left_tok[128] = {0};
    char right_tok[128] = {0};
    uint32_t llen = op_ptr - expr;
    if (llen < sizeof(left_tok)) strncpy(left_tok, expr, llen);
    while (llen > 0 && isspace((unsigned char)left_tok[llen-1])) left_tok[--llen] = '\0';
    char* lstart = left_tok;
    while (*lstart && isspace((unsigned char)*lstart)) lstart++;

    strncpy(right_tok, op_ptr + 1, sizeof(right_tok) - 1);
    char* rstart = right_tok;
    while (*rstart && isspace((unsigned char)*rstart)) rstart++;
    uint32_t rlen = strlen(rstart);
    while (rlen > 0 && isspace((unsigned char)rstart[rlen-1])) rstart[--rlen] = '\0';

    double left_val = 0.0, right_val = 0.0;
    bool left_found = false, right_found = false;
    if (def && row_vals) {
      for (uint32_t c = 0; c < def->num_cols; c++) {
        if (strcmp(def->columns[c].name, lstart) == 0) {
          left_val = (def->columns[c].type == COL_INT) ? (double)row_vals[c].int_val : row_vals[c].double_val;
          left_found = true;
        }
        if (strcmp(def->columns[c].name, rstart) == 0) {
          right_val = (def->columns[c].type == COL_INT) ? (double)row_vals[c].int_val : row_vals[c].double_val;
          right_found = true;
        }
      }
    }
    if (!left_found) left_val = atof(lstart);
    if (!right_found) right_val = atof(rstart);

    double res = 0.0;
    if (op_char == '+') res = left_val + right_val;
    else if (op_char == '-') res = left_val - right_val;
    else if (op_char == '*') res = left_val * right_val;
    else if (op_char == '/' && right_val != 0.0) res = left_val / right_val;

    if ((double)(long long)res == res) snprintf(out_buf, out_size, "%lld", (long long)res);
    else snprintf(out_buf, out_size, "%.8g", res);
    return;
  }
}

static void print_projected_row(Statement* stmt, TableDef* def, Value* row_vals) {
  if (stmt->num_select_cols == 0) {
    if (def && row_vals) print_row(def, row_vals);
    return;
  }
  printf("(");
  for (uint32_t i = 0; i < stmt->num_select_cols; i++) {
    SelectCol* sc = &stmt->select_cols[i];
    if (i > 0) printf(", ");
    int col_idx = -1;
    if (def != NULL) {
      for (uint32_t c = 0; c < def->num_cols; c++) {
        if (strcasecmp(def->columns[c].name, sc->col_name) == 0) {
          col_idx = (int)c;
          break;
        }
      }
      if (col_idx == -1) {
        const char* dot = strchr(sc->col_name, '.');
        if (dot != NULL) {
          for (uint32_t c = 0; c < def->num_cols; c++) {
            if (strcasecmp(def->columns[c].name, dot + 1) == 0) {
              col_idx = (int)c;
              break;
            }
          }
        }
      }
    }
    char expr_out[256];
    eval_expr_string(sc->col_name, def, row_vals, expr_out, sizeof(expr_out));
    if (strlen(expr_out) > 0) {
      printf("%s", expr_out);
    } else if (col_idx == -1) {
      if (sc->col_name[0] == '\'' || sc->col_name[0] == '"') {
        char q = sc->col_name[0];
        size_t len = strlen(sc->col_name);
        if (len > 1 && sc->col_name[len-1] == q) {
          char tmp[256];
          strncpy(tmp, sc->col_name + 1, len - 2);
          tmp[len - 2] = '\0';
          printf("%s", tmp);
        } else {
          printf("%s", sc->col_name);
        }
      } else {
        bool is_num = (strlen(sc->col_name) > 0);
        for (size_t k = (sc->col_name[0] == '-' ? 1 : 0); sc->col_name[k]; k++) {
          if (!isdigit((unsigned char)sc->col_name[k]) && sc->col_name[k] != '.') { is_num = false; break; }
        }
        if (is_num) {
          printf("%s", sc->col_name);
        } else {
          printf("NULL");
        }
      }
    } else {
      Value val = row_vals[col_idx];
      if (sc->is_coalesce && val.is_null) {
        printf("%s", sc->coalesce_default);
      } else if (val.is_null) {
        printf("NULL");
      } else {
        switch (def->columns[col_idx].type) {
          case COL_INT:       printf("%d",  val.int_val);   break;
          case COL_FLOAT:     printf("%.4g", val.double_val != 0.0 ? val.double_val : (double)val.float_val); break;
          case COL_DOUBLE:
          case COL_NUMERIC:
          case COL_DECIMAL:   printf("%.8g", val.double_val); break;
          case COL_BOOL:      printf("%s",  val.bool_val ? "true" : "false"); break;
          case COL_BLOB:
            if (strncasecmp(val.text_val, "x'", 2) == 0 || strncasecmp(val.text_val, "0x", 2) == 0) {
              printf("%s", val.text_val);
            } else {
              printf("x'%s'", val.text_val);
            }
            break;
          case COL_DATETIME:
          case COL_DATE:
          case COL_TIME:
          case COL_TIMESTAMP:
          case COL_TEXT:
          case COL_VARCHAR:
          case COL_VECTOR:    printf("%s",  val.text_val);  break;
        }
      }
    }
  }
  printf(")\n");
}

static void print_returning_row(Statement* stmt, TableDef* def, Value* row_vals) {
  if (!stmt->has_returning || !def || !row_vals) return;
  if (stmt->num_returning_cols == 1 && strcmp(stmt->returning_cols[0], "*") == 0) {
    print_row(def, row_vals);
    return;
  }
  printf("(");
  for (uint32_t i = 0; i < stmt->num_returning_cols; i++) {
    if (i > 0) printf(", ");
    int col_idx = -1;
    for (uint32_t c = 0; c < def->num_cols; c++) {
      if (strcasecmp(def->columns[c].name, stmt->returning_cols[i]) == 0) {
        col_idx = (int)c;
        break;
      }
    }
    if (col_idx == -1) {
      char expr_out[256] = {0};
      eval_expr_string(stmt->returning_cols[i], def, row_vals, expr_out, sizeof(expr_out));
      if (strlen(expr_out) > 0) printf("%s", expr_out);
      else printf("NULL");
    } else {
      Value* val = &row_vals[col_idx];
      if (val->is_null) {
        printf("NULL");
      } else {
        switch (def->columns[col_idx].type) {
          case COL_INT:       printf("%d", val->int_val); break;
          case COL_FLOAT:     printf("%.4g", val->double_val != 0.0 ? val->double_val : (double)val->float_val); break;
          case COL_DOUBLE:
          case COL_NUMERIC:
          case COL_DECIMAL:   printf("%.8g", val->double_val); break;
          case COL_BOOL:      printf("%s", val->bool_val ? "true" : "false"); break;
          case COL_BLOB:
            if (strncasecmp(val->text_val, "x'", 2) == 0 || strncasecmp(val->text_val, "0x", 2) == 0) {
              printf("%s", val->text_val);
            } else {
              printf("x'%s'", val->text_val);
            }
            break;
          default:
            printf("%s", val->text_val); break;
        }
      }
    }
  }
  printf(")\n");
}

/* Set Operation Support */
typedef struct {
  char** rows;
  uint32_t count;
  uint32_t capacity;
} RowSet;

static void rowset_init(RowSet* rs) {
  rs->capacity = 16;
  rs->count = 0;
  rs->rows = malloc(sizeof(char*) * rs->capacity);
}

static void rowset_add(RowSet* rs, const char* str) {
  if (rs->count >= rs->capacity) {
    rs->capacity *= 2;
    rs->rows = realloc(rs->rows, sizeof(char*) * rs->capacity);
  }
  rs->rows[rs->count++] = strdup(str);
}

static void rowset_free(RowSet* rs) {
  for (uint32_t i = 0; i < rs->count; i++) {
    free(rs->rows[i]);
  }
  free(rs->rows);
  rs->rows = NULL;
  rs->count = 0;
  rs->capacity = 0;
}

static void format_projected_row_str(Statement* stmt, TableDef* def, Value* row_vals, char* out, size_t out_size) {
  size_t o = 0;
  o += snprintf(out + o, out_size - o, "(");
  if (stmt->num_select_cols == 0) {
    if (def && row_vals) {
      for (uint32_t c = 0; c < def->num_cols; c++) {
        if (c > 0) o += snprintf(out + o, out_size - o, ", ");
        if (row_vals[c].is_null) {
          o += snprintf(out + o, out_size - o, "NULL");
        } else {
          switch (def->columns[c].type) {
            case COL_INT:       o += snprintf(out + o, out_size - o, "%d", row_vals[c].int_val); break;
            case COL_FLOAT:     o += snprintf(out + o, out_size - o, "%.4g", row_vals[c].double_val != 0.0 ? row_vals[c].double_val : (double)row_vals[c].float_val); break;
            case COL_DOUBLE:
            case COL_NUMERIC:
            case COL_DECIMAL:   o += snprintf(out + o, out_size - o, "%.8g", row_vals[c].double_val); break;
            case COL_BOOL:      o += snprintf(out + o, out_size - o, "%s", row_vals[c].bool_val ? "true" : "false"); break;
            default:            o += snprintf(out + o, out_size - o, "%s", row_vals[c].text_val); break;
          }
        }
      }
    }
  } else {
    for (uint32_t i = 0; i < stmt->num_select_cols; i++) {
      SelectCol* sc = &stmt->select_cols[i];
      if (i > 0) o += snprintf(out + o, out_size - o, ", ");
      int col_idx = -1;
      if (def != NULL) {
        for (uint32_t c = 0; c < def->num_cols; c++) {
          if (strcasecmp(def->columns[c].name, sc->col_name) == 0) {
            col_idx = (int)c;
            break;
          }
        }
        if (col_idx == -1) {
          const char* dot = strchr(sc->col_name, '.');
          if (dot != NULL) {
            for (uint32_t c = 0; c < def->num_cols; c++) {
              if (strcasecmp(def->columns[c].name, dot + 1) == 0) {
                col_idx = (int)c;
                break;
              }
            }
          }
        }
      }
      char expr_out[256] = {0};
      eval_expr_string(sc->col_name, def, row_vals, expr_out, sizeof(expr_out));
      if (strlen(expr_out) > 0) {
        o += snprintf(out + o, out_size - o, "%s", expr_out);
      } else if (col_idx == -1) {
        /* If it is a string literal 'val', strip quotes and print */
        if (sc->col_name[0] == '\'' || sc->col_name[0] == '"') {
          char q = sc->col_name[0];
          size_t len = strlen(sc->col_name);
          if (len > 1 && sc->col_name[len-1] == q) {
            char tmp[256];
            strncpy(tmp, sc->col_name + 1, len - 2);
            tmp[len - 2] = '\0';
            o += snprintf(out + o, out_size - o, "%s", tmp);
          } else {
            o += snprintf(out + o, out_size - o, "%s", sc->col_name);
          }
        } else {
          /* Number or unknown identifier */
          bool is_num = (strlen(sc->col_name) > 0);
          for (size_t k = (sc->col_name[0] == '-' ? 1 : 0); sc->col_name[k]; k++) {
            if (!isdigit((unsigned char)sc->col_name[k]) && sc->col_name[k] != '.') { is_num = false; break; }
          }
          if (is_num) {
            o += snprintf(out + o, out_size - o, "%s", sc->col_name);
          } else {
            o += snprintf(out + o, out_size - o, "NULL");
          }
        }
      } else {
        Value val = row_vals[col_idx];
        if (sc->is_coalesce && val.is_null) {
          o += snprintf(out + o, out_size - o, "%s", sc->coalesce_default);
        } else if (val.is_null) {
          o += snprintf(out + o, out_size - o, "NULL");
        } else {
          switch (def->columns[col_idx].type) {
            case COL_INT:       o += snprintf(out + o, out_size - o, "%d", val.int_val); break;
            case COL_FLOAT:     o += snprintf(out + o, out_size - o, "%.4g", val.double_val != 0.0 ? val.double_val : (double)val.float_val); break;
            case COL_DOUBLE:
            case COL_NUMERIC:
            case COL_DECIMAL:   o += snprintf(out + o, out_size - o, "%.8g", val.double_val); break;
            case COL_BOOL:      o += snprintf(out + o, out_size - o, "%s", val.bool_val ? "true" : "false"); break;
            default:            o += snprintf(out + o, out_size - o, "%s", val.text_val); break;
          }
        }
      }
    }
  }
  snprintf(out + o, out_size - o, ")");
}

static void collect_select_rows(Statement* stmt, Catalog* catalog, Pager* pager, RowSet* rs) {
  if (strlen(stmt->table_name) == 0) {
    char row_str[1024];
    format_projected_row_str(stmt, NULL, NULL, row_str, sizeof(row_str));
    rowset_add(rs, row_str);
    return;
  }
  TableDef* def = catalog_find(catalog, stmt->table_name);
  if (!def) return;
  Table table = { pager, def };
  Cursor* cursor = btree_start(&table);
  while (!cursor->end_of_table) {
    Value row_vals[MAX_COLUMNS];
    deserialize_row(def, cursor_value(cursor), row_vals);
    if (eval_where_clause(def, row_vals, &stmt->where_clause, catalog, pager)) {
      char row_str[1024];
      format_projected_row_str(stmt, def, row_vals, row_str, sizeof(row_str));
      rowset_add(rs, row_str);
    }
    value_free_row(row_vals, def->num_cols);
    cursor_advance(cursor);
  }
  free(cursor);
}

static int str_ptr_cmp(const void* a, const void* b) {
  return strcmp(*(const char**)a, *(const char**)b);
}

static ExecuteResult execute_set_operation(Statement* stmt, Catalog* catalog, Pager* pager) {
  RowSet lhs, rhs;
  rowset_init(&lhs);
  rowset_init(&rhs);

  collect_select_rows(stmt, catalog, pager, &lhs);

  /* RHS might itself have set_op or be a simple select */
  if (stmt->set_rhs->set_op != SET_NONE) {
    /* If RHS has nested set_op, recurse or collect */
    collect_select_rows(stmt->set_rhs, catalog, pager, &rhs);
  } else {
    collect_select_rows(stmt->set_rhs, catalog, pager, &rhs);
  }

  RowSet result;
  rowset_init(&result);

  if (stmt->set_op == SET_UNION_ALL) {
    for (uint32_t i = 0; i < lhs.count; i++) rowset_add(&result, lhs.rows[i]);
    for (uint32_t i = 0; i < rhs.count; i++) rowset_add(&result, rhs.rows[i]);
  } else if (stmt->set_op == SET_UNION) {
    for (uint32_t i = 0; i < lhs.count; i++) rowset_add(&result, lhs.rows[i]);
    for (uint32_t i = 0; i < rhs.count; i++) rowset_add(&result, rhs.rows[i]);
    /* Deduplicate */
    if (result.count > 1) {
      qsort(result.rows, result.count, sizeof(char*), str_ptr_cmp);
      uint32_t write = 1;
      for (uint32_t i = 1; i < result.count; i++) {
        if (strcmp(result.rows[i], result.rows[write - 1]) != 0) {
          result.rows[write++] = result.rows[i];
        } else {
          free(result.rows[i]);
        }
      }
      result.count = write;
    }
  } else if (stmt->set_op == SET_INTERSECT) {
    for (uint32_t i = 0; i < lhs.count; i++) {
      bool found = false;
      for (uint32_t j = 0; j < rhs.count; j++) {
        if (strcmp(lhs.rows[i], rhs.rows[j]) == 0) { found = true; break; }
      }
      if (found) {
        /* Distinct intersect */
        bool already = false;
        for (uint32_t k = 0; k < result.count; k++) {
          if (strcmp(result.rows[k], lhs.rows[i]) == 0) { already = true; break; }
        }
        if (!already) rowset_add(&result, lhs.rows[i]);
      }
    }
  } else if (stmt->set_op == SET_EXCEPT) {
    for (uint32_t i = 0; i < lhs.count; i++) {
      bool found = false;
      for (uint32_t j = 0; j < rhs.count; j++) {
        if (strcmp(lhs.rows[i], rhs.rows[j]) == 0) { found = true; break; }
      }
      if (!found) {
        /* Distinct except */
        bool already = false;
        for (uint32_t k = 0; k < result.count; k++) {
          if (strcmp(result.rows[k], lhs.rows[i]) == 0) { already = true; break; }
        }
        if (!already) rowset_add(&result, lhs.rows[i]);
      }
    }
  }

  for (uint32_t i = 0; i < result.count; i++) {
    printf("%s\n", result.rows[i]);
  }

  rowset_free(&lhs);
  rowset_free(&rhs);
  rowset_free(&result);
  return EXECUTE_SUCCESS;
}

typedef enum {
  ACCESS_SCAN,
  ACCESS_PK_SEEK,
  ACCESS_INDEX_SEEK
} AccessPathType;

static void value_from_raw(ColumnType type, const char* raw, bool is_null, Value* out_val) {
  value_init(out_val);
  if (is_null || raw == NULL || strlen(raw) == 0) {
    out_val->is_null = true;
    return;
  }
  out_val->is_null = false;
  switch (type) {
    case COL_INT:       out_val->int_val = atoi(raw); break;
    case COL_FLOAT:     out_val->float_val = (float)atof(raw); out_val->double_val = atof(raw); break;
    case COL_DOUBLE:
    case COL_NUMERIC:
    case COL_DECIMAL:   out_val->double_val = atof(raw); break;
    case COL_BOOL:      out_val->bool_val = (strcasecmp(raw, "true") == 0 || strcmp(raw, "1") == 0); break;
    case COL_BLOB:
    case COL_DATETIME:
    case COL_DATE:
    case COL_TIME:
    case COL_TIMESTAMP:
    case COL_TEXT:
    case COL_VARCHAR:
    case COL_VECTOR:    value_set_text(out_val, raw); break;
  }
}

static void add_selected_row(RowSortEntry** p_entries, uint32_t* p_count, uint32_t* p_capacity,
                             Statement* stmt, TableDef* def, Value* row_vals) {
  if (*p_count >= *p_capacity) {
    *p_capacity *= 2;
    *p_entries = realloc(*p_entries, sizeof(RowSortEntry) * (*p_capacity));
  }
  uint32_t idx = *p_count;
  memset(&(*p_entries)[idx], 0, sizeof(RowSortEntry));
  for (uint32_t c = 0; c < def->num_cols; c++) {
    value_copy(&(*p_entries)[idx].row[c], &row_vals[c]);
  }
  (*p_entries)[idx].num_sort_keys = 0;
  if (stmt->has_order_by) {
    uint32_t num_items = (stmt->num_order_by > 0) ? stmt->num_order_by : 1;
    for (uint32_t k = 0; k < num_items; k++) {
      const char* target_col = (stmt->num_order_by > 0) ? stmt->order_by_items[k].col_name : stmt->order_by_col;
      bool target_desc = (stmt->num_order_by > 0) ? stmt->order_by_items[k].is_desc : stmt->order_by_desc;
      CollationType target_coll = (stmt->num_order_by > 0) ? stmt->order_by_items[k].collation : stmt->order_by_collation;

      char resolved_expr[256];
      snprintf(resolved_expr, sizeof(resolved_expr), "%s", target_col);
      if (isdigit((unsigned char)target_col[0]) && stmt->num_select_cols > 0) {
        int pos = atoi(target_col);
        if (pos >= 1 && pos <= (int)stmt->num_select_cols) {
          snprintf(resolved_expr, sizeof(resolved_expr), "%s", stmt->select_cols[pos - 1].col_name);
        }
      }

      bool found_col = false;
      for (uint32_t c = 0; c < def->num_cols; c++) {
        if (strcasecmp(def->columns[c].name, resolved_expr) == 0) {
          if ((*p_entries)[idx].num_sort_keys < 4) {
            uint32_t sk_idx = (*p_entries)[idx].num_sort_keys++;
            value_copy(&(*p_entries)[idx].sort_keys[sk_idx], &row_vals[c]);
            (*p_entries)[idx].sort_types[sk_idx] = def->columns[c].type;
            (*p_entries)[idx].sort_colls[sk_idx] = target_coll;
            (*p_entries)[idx].sort_descs[sk_idx] = target_desc;
          }
          found_col = true;
          break;
        }
      }

      if (!found_col) {
        char expr_out[256] = {0};
        eval_expr_string(resolved_expr, def, row_vals, expr_out, sizeof(expr_out));
        if (expr_out[0] != '\0' && (*p_entries)[idx].num_sort_keys < 4) {
          uint32_t sk_idx = (*p_entries)[idx].num_sort_keys++;
          (*p_entries)[idx].sort_colls[sk_idx] = target_coll;
          (*p_entries)[idx].sort_descs[sk_idx] = target_desc;
          if (strcasecmp(expr_out, "null") == 0) {
            (*p_entries)[idx].sort_types[sk_idx] = COL_INT;
            (*p_entries)[idx].sort_keys[sk_idx].is_null = true;
          } else {
            char* endptr = NULL;
            double dval = strtod(expr_out, &endptr);
            if (endptr != expr_out && (*endptr == '\0' || isspace((unsigned char)*endptr))) {
              (*p_entries)[idx].sort_types[sk_idx] = COL_DOUBLE;
              (*p_entries)[idx].sort_keys[sk_idx].double_val = dval;
            } else {
              (*p_entries)[idx].sort_types[sk_idx] = COL_TEXT;
              value_set_text(&(*p_entries)[idx].sort_keys[sk_idx], expr_out);
            }
          }
        }
      }
    }
  }
  (*p_count)++;
}

/* Compile and run SELECT statement on VDBE */
static ExecuteResult run_select_vm(Statement* stmt, TableDef* def, Catalog* catalog, Pager* pager) {
  Vdbe* vm = vdbe_create(pager, catalog);
  WhereClause* wc = &stmt->where_clause;

  if (stmt->is_explain) {
    bool use_index = false;
    uint32_t idx_col_idx = 0;
    if (wc->has_where && wc->num_conds > 0) {
      for (uint32_t c = 1; c < def->num_cols; c++) {
        if (strcmp(def->columns[c].name, wc->conds[0].col_name) == 0 && def->columns[c].has_index) {
          use_index = true;
          idx_col_idx = c;
          break;
        }
      }
    }
    bool pk_seek = (!use_index && wc->has_where && wc->num_conds > 0 &&
                    strcmp(wc->conds[0].col_name, "id") == 0 &&
                    (wc->conds[0].op == OP_EQ || wc->conds[0].op == OP_GT || wc->conds[0].op == OP_GTE));
    printf("QUERY PLAN:\n");
    if (use_index) {
      const char* op_str = "=";
      if (wc->conds[0].op == OP_GT) op_str = ">";
      else if (wc->conds[0].op == OP_GTE) op_str = ">=";
      else if (wc->conds[0].op == OP_LT) op_str = "<";
      else if (wc->conds[0].op == OP_LTE) op_str = "<=";
      printf("  SEARCH TABLE %s USING INDEX _idx_%s_%s (%s %s %s)\n",
             def->name, def->name, def->columns[idx_col_idx].name,
             def->columns[idx_col_idx].name, op_str, wc->conds[0].raw_val);
    } else if (pk_seek) {
      printf("  SEARCH TABLE %s USING PRIMARY KEY (id = %s)\n",
             def->name, wc->conds[0].raw_val);
    } else {
      printf("  SCAN TABLE %s\n", def->name);
    }
    printf("\n");
    Value tbl_name_val;
    value_init(&tbl_name_val);
    value_set_text(&tbl_name_val, def->name);
    vdbe_add_inst(vm, OP_OpenRead, 0, def->root_page_num, 0, tbl_name_val);
    value_free(&tbl_name_val);
    vdbe_add_inst(vm, OP_Rewind, 0, 4, 0, (Value){0});
    vdbe_add_inst(vm, OP_ResultRow, 1, def->num_cols, 0, (Value){0});
    vdbe_add_inst(vm, OP_Next, 0, 2, 0, (Value){0});
    vdbe_add_inst(vm, OP_Halt, 0, 0, 0, (Value){0});
    vdbe_print_program(vm);
    vdbe_free(vm);
    return EXECUTE_SUCCESS;
  }

  if (def->is_virtual) {
    if (strcasecmp(def->vtab_module, "csv") == 0) {
      FILE* fp = fopen(def->vtab_args, "r");
    if (!fp) {
      vdbe_free(vm);
      return EXECUTE_TABLE_NOT_FOUND;
    }
    char line[512];
    bool is_header = true;
    while (fgets(line, sizeof(line), fp)) {
      if (is_header) { is_header = false; continue; }
      Value row_vals[MAX_COLUMNS];
      memset(row_vals, 0, sizeof(row_vals));

      char* saveptr = NULL;
      char* token = strtok_r(line, ",\r\n", &saveptr);
      uint32_t c = 0;
      while (token && c < def->num_cols) {
        value_init(&row_vals[c]);
        if (def->columns[c].type == COL_INT) {
          row_vals[c].int_val = atoi(token);
        } else if (def->columns[c].type == COL_DOUBLE || def->columns[c].type == COL_FLOAT) {
          row_vals[c].double_val = atof(token);
        } else {
          value_set_text(&row_vals[c], token);
        }
        c++;
        token = strtok_r(NULL, ",\r\n", &saveptr);
      }

      if (eval_where_clause(def, row_vals, wc, catalog, pager)) {
        print_projected_row(stmt, def, row_vals);
      }
      value_free_row(row_vals, def->num_cols);
    }
      fclose(fp);
      vdbe_free(vm);
      return EXECUTE_SUCCESS;
    }
  }

  bool has_window = false;
  for (uint32_t i = 0; i < stmt->num_select_cols; i++) {
    if (stmt->select_cols[i].win_spec.win_func != WIN_NONE) {
      has_window = true;
      break;
    }
  }

  if (has_window) {
    vdbe_free(vm);
    Table table = { pager, def };
    Cursor* cursor = btree_start(&table);

    uint32_t capacity = 16;
    uint32_t count = 0;
    Value (*all_rows)[MAX_COLUMNS] = malloc(sizeof(Value[MAX_COLUMNS]) * capacity);

    time_t now_ts = time(NULL);
    uint64_t snapshot_xid = get_current_snapshot_xid(pager);
    while (!cursor->end_of_table) {
      Value row_vals[MAX_COLUMNS];
      if (!row_is_visible_and_active(def, cursor_value(cursor), snapshot_xid, row_vals, (uint64_t)now_ts)) {
        cursor_advance(cursor);
        continue;
      }
      if (eval_where_clause(def, row_vals, wc, catalog, pager)) {
        if (count >= capacity) {
          capacity *= 2;
          all_rows = realloc(all_rows, sizeof(Value[MAX_COLUMNS]) * capacity);
        }
        for (uint32_t c = 0; c < def->num_cols; c++) {
          value_init(&all_rows[count][c]);
          value_copy(&all_rows[count][c], &row_vals[c]);
        }
        count++;
      }
      value_free_row(row_vals, def->num_cols);
      cursor_advance(cursor);
    }
    free(cursor);

    /* Process window calculations per column */
    for (uint32_t i = 0; i < count; i++) {
      printf("(");
      for (uint32_t sc_idx = 0; sc_idx < stmt->num_select_cols; sc_idx++) {
        SelectCol* sc = &stmt->select_cols[sc_idx];
        if (sc_idx > 0) printf(", ");

        if (sc->win_spec.win_func != WIN_NONE) {
          int p_col = -1, o_col = -1;
          for (uint32_t c = 0; c < def->num_cols; c++) {
            if (strlen(sc->win_spec.partition_col) > 0 && strcmp(def->columns[c].name, sc->win_spec.partition_col) == 0) p_col = c;
            if (strlen(sc->win_spec.order_col) > 0 && strcmp(def->columns[c].name, sc->win_spec.order_col) == 0) o_col = c;
          }

          int r_num = 1;
          int r_rank = 1;
          int d_rank = 1;
          double win_sum = 0.0;

          for (uint32_t j = 0; j < count; j++) {
            /* Check partition match */
            bool p_match = true;
            if (p_col != -1) {
              Value* va = &all_rows[i][p_col];
              Value* vb = &all_rows[j][p_col];
              if (def->columns[p_col].type == COL_INT) p_match = (va->int_val == vb->int_val);
              else if (def->columns[p_col].type == COL_DOUBLE) p_match = (va->double_val == vb->double_val);
              else p_match = (strcmp(va->text_val, vb->text_val) == 0);
            }

            if (p_match) {
              double v_j = 0.0;
              int col_idx = -1;
              for (uint32_t c = 0; c < def->num_cols; c++) {
                if (strcmp(def->columns[c].name, sc->col_name) == 0) { col_idx = c; break; }
              }
              if (col_idx != -1) {
                if (def->columns[col_idx].type == COL_INT) v_j = (double)all_rows[j][col_idx].int_val;
                else if (def->columns[col_idx].type == COL_DOUBLE) v_j = all_rows[j][col_idx].double_val;
              }
              win_sum += v_j;

              if (j < i) {
                r_num++;
                if (o_col != -1) {
                  Value* vi = &all_rows[i][o_col];
                  Value* vj = &all_rows[j][o_col];
                  int cmp = 0;
                  if (def->columns[o_col].type == COL_INT) cmp = (vi->int_val > vj->int_val) - (vi->int_val < vj->int_val);
                  else if (def->columns[o_col].type == COL_DOUBLE) cmp = (vi->double_val > vj->double_val) - (vi->double_val < vj->double_val);
                  else cmp = strcmp(vi->text_val, vj->text_val);

                  if (sc->win_spec.order_desc) cmp = -cmp;
                  if (cmp > 0) {
                    r_rank++;
                    d_rank++;
                  }
                }
              }
            }
          }

          if (sc->win_spec.win_func == WIN_ROW_NUMBER) printf("%d", r_num);
          else if (sc->win_spec.win_func == WIN_RANK) printf("%d", r_rank);
          else if (sc->win_spec.win_func == WIN_DENSE_RANK) printf("%d", d_rank);
          else if (sc->win_spec.win_func == WIN_SUM) printf("%.2f", win_sum);
        } else {
          int col_idx = -1;
          for (uint32_t c = 0; c < def->num_cols; c++) {
            if (strcmp(def->columns[c].name, sc->col_name) == 0) { col_idx = c; break; }
          }
          if (col_idx != -1) {
            Value val = all_rows[i][col_idx];
            if (val.is_null) printf("NULL");
            else if (def->columns[col_idx].type == COL_INT) printf("%d", val.int_val);
            else if (def->columns[col_idx].type == COL_DOUBLE) printf("%.8g", val.double_val);
            else printf("%s", val.text_val);
          }
        }
      }
      printf(")\n");
    }

    for (uint32_t r = 0; r < count; r++) {
      value_free_row(all_rows[r], def->num_cols);
    }
    free(all_rows);
    return EXECUTE_SUCCESS;
  }

  if (true) {
    vdbe_free(vm);
    struct timespec start_time, end_time;
    if (stmt->is_explain_analyze) {
      pager_reset_stats(pager);
      clock_gettime(CLOCK_MONOTONIC, &start_time);
    }
    uint64_t rows_scanned = 0;
    AccessPathType path_type = ACCESS_SCAN;
    uint32_t seek_col_idx = 0;
    SingleCond* seek_cond = NULL;

    if (wc->has_where && wc->num_conds > 0) {
      bool all_and = true;
      for (uint32_t i = 0; i + 1 < wc->num_conds; i++) {
        if (wc->logic_ops[i] != LOGIC_AND) {
          all_and = false;
          break;
        }
      }

      if (all_and) {
        /* Primary Key seek check */
        for (uint32_t i = 0; i < wc->num_conds; i++) {
          SingleCond* c = &wc->conds[i];
          if (!c->is_subquery && strlen(c->raw_val) > 0 && strcasecmp(c->raw_val, "null") != 0 &&
              c->op != OP_IS_NULL && c->op != OP_IS_NOT_NULL &&
              strcmp(def->columns[0].name, c->col_name) == 0) {
            if (c->op == OP_EQ || c->op == OP_GT || c->op == OP_GTE) {
              path_type = ACCESS_PK_SEEK;
              seek_col_idx = 0;
              seek_cond = c;
              break;
            }
          }
        }

        /* Secondary index seek check */
        if (path_type == ACCESS_SCAN) {
          for (uint32_t i = 0; i < wc->num_conds; i++) {
            SingleCond* c = &wc->conds[i];
            if (!c->is_subquery && strlen(c->raw_val) > 0 && strcasecmp(c->raw_val, "null") != 0 &&
                c->op != OP_IS_NULL && c->op != OP_IS_NOT_NULL) {
              for (uint32_t col_idx = 1; col_idx < def->num_cols; col_idx++) {
                Column* col = &def->columns[col_idx];
                if (col->has_index && col->index_root_page != 0 &&
                    !col->idx_is_partial && !col->idx_is_expr &&
                    strcmp(col->name, c->col_name) == 0) {
                  if (c->op == OP_EQ || c->op == OP_GT || c->op == OP_GTE) {
                    path_type = ACCESS_INDEX_SEEK;
                    seek_col_idx = col_idx;
                    seek_cond = c;
                    break;
                  }
                }
              }
              if (path_type == ACCESS_INDEX_SEEK) break;
            }
          }
        }
      }
    }

    uint32_t capacity = 16;
    uint32_t count = 0;
    RowSortEntry* entries = malloc(sizeof(RowSortEntry) * capacity);
    time_t now_ts = time(NULL);
    uint64_t snapshot_xid = get_current_snapshot_xid(pager);

    if (path_type == ACCESS_PK_SEEK) {
      Table table = { pager, def };
      Value target_pk;
      value_from_raw(def->columns[0].type, seek_cond->raw_val, false, &target_pk);

      Cursor* cursor = btree_find(&table, &target_pk);
      if (seek_cond->op == OP_EQ) {
        while (!cursor->end_of_table) {
          Value cur_pk;
          btree_key_value(cursor, &cur_pk);
          int cmp = compare_values(def->columns[0].type, &cur_pk, &target_pk);
          value_free(&cur_pk);
          if (cmp != 0) break;

          rows_scanned++;
          Value row_vals[MAX_COLUMNS];
          if (row_is_visible_and_active(def, cursor_value(cursor), snapshot_xid, row_vals, (uint64_t)now_ts)) {
            if (eval_where_clause(def, row_vals, wc, catalog, pager)) {
              add_selected_row(&entries, &count, &capacity, stmt, def, row_vals);
            }
            value_free_row(row_vals, def->num_cols);
            break; /* Found the visible version for this PK in this snapshot */
          }
          cursor_advance(cursor);
        }
      } else { /* OP_GT or OP_GTE */
        while (!cursor->end_of_table) {
          rows_scanned++;
          Value row_vals[MAX_COLUMNS];
          if (row_is_visible_and_active(def, cursor_value(cursor), snapshot_xid, row_vals, (uint64_t)now_ts)) {
            if (eval_where_clause(def, row_vals, wc, catalog, pager)) {
              add_selected_row(&entries, &count, &capacity, stmt, def, row_vals);
            }
            value_free_row(row_vals, def->num_cols);
          }
          cursor_advance(cursor);
        }
      }
      free(cursor);
      value_free(&target_pk);

    } else if (path_type == ACCESS_INDEX_SEEK) {
      TableDef idx_def;
      memset(&idx_def, 0, sizeof(TableDef));
      make_idx_name(idx_def.name, def->name, def->columns[seek_col_idx].name);
      idx_def.root_page_num = def->columns[seek_col_idx].index_root_page;
      idx_def.num_cols = 2;
      memcpy(&idx_def.columns[0], &def->columns[seek_col_idx], sizeof(Column));
      strcpy(idx_def.columns[1].name, "id");
      idx_def.columns[1].type = COL_INT;
      idx_def.columns[1].size = 4;
      tabledef_compute(&idx_def);

      Table idx_table = { pager, &idx_def };
      Value target_val;
      value_from_raw(def->columns[seek_col_idx].type, seek_cond->raw_val, false, &target_val);

      Table main_table = { pager, def };
      Cursor* idx_cur = btree_find(&idx_table, &target_val);

      while (!idx_cur->end_of_table) {
        Value cur_idx_vals[2];
        deserialize_row(&idx_def, cursor_value(idx_cur), cur_idx_vals);

        if (seek_cond->op == OP_EQ) {
          if (compare_values(def->columns[seek_col_idx].type, &cur_idx_vals[0], &target_val) != 0) {
            value_free_row(cur_idx_vals, 2);
            break;
          }
        }

        rows_scanned++;

        Cursor* main_cur = btree_find(&main_table, &cur_idx_vals[1]);
        while (!main_cur->end_of_table) {
          Value cur_pk;
          btree_key_value(main_cur, &cur_pk);
          int cmp = compare_values(def->columns[0].type, &cur_pk, &cur_idx_vals[1]);
          value_free(&cur_pk);
          if (cmp != 0) break;

          Value row_vals[MAX_COLUMNS];
          if (row_is_visible_and_active(def, cursor_value(main_cur), snapshot_xid, row_vals, (uint64_t)now_ts)) {
            if (eval_where_clause(def, row_vals, wc, catalog, pager)) {
              add_selected_row(&entries, &count, &capacity, stmt, def, row_vals);
            }
            value_free_row(row_vals, def->num_cols);
            break; /* Found visible row for this PK */
          }
          cursor_advance(main_cur);
        }
        free(main_cur);
        value_free_row(cur_idx_vals, 2);
        cursor_advance(idx_cur);
      }
      free(idx_cur);
      value_free(&target_val);

    } else {
      Table table = { pager, def };
      Cursor* cursor = btree_start(&table);
      while (!cursor->end_of_table) {
        rows_scanned++;
        Value row_vals[MAX_COLUMNS];
        if (row_is_visible_and_active(def, cursor_value(cursor), snapshot_xid, row_vals, (uint64_t)now_ts)) {
          if (eval_where_clause(def, row_vals, wc, catalog, pager)) {
            add_selected_row(&entries, &count, &capacity, stmt, def, row_vals);
          }
          value_free_row(row_vals, def->num_cols);
        }
        cursor_advance(cursor);
      }
      free(cursor);
    }

    if (stmt->has_order_by && count > 1) {
      qsort(entries, count, sizeof(RowSortEntry), compare_row_sort_entries);
    }

    /* Deduplication for DISTINCT */
    if (stmt->is_distinct && count > 0) {
      uint32_t write_idx = 0;
      for (uint32_t i = 0; i < count; i++) {
        bool dup = false;
        for (uint32_t j = 0; j < write_idx; j++) {
          bool same = true;
          if (stmt->num_select_cols > 0) {
            for (uint32_t sc = 0; sc < stmt->num_select_cols; sc++) {
              int col_idx = -1;
              for (uint32_t c = 0; c < def->num_cols; c++) {
                if (strcmp(def->columns[c].name, stmt->select_cols[sc].col_name) == 0) {
                  col_idx = (int)c;
                  break;
                }
              }
              if (col_idx >= 0) {
                if (compare_values(def->columns[col_idx].type, &entries[i].row[col_idx], &entries[j].row[col_idx]) != 0) {
                  same = false;
                  break;
                }
              }
            }
          } else {
            for (uint32_t c = 0; c < def->num_cols; c++) {
              if (compare_values(def->columns[c].type, &entries[i].row[c], &entries[j].row[c]) != 0) {
                same = false;
                break;
              }
            }
          }
          if (same) { dup = true; break; }
        }
        if (!dup) {
          if (write_idx != i) {
            value_free_row(entries[write_idx].row, def->num_cols);
            value_free_row(entries[write_idx].sort_keys, entries[write_idx].num_sort_keys);
            entries[write_idx] = entries[i];
            memset(&entries[i], 0, sizeof(RowSortEntry));
          }
          write_idx++;
        } else {
          value_free_row(entries[i].row, def->num_cols);
          value_free_row(entries[i].sort_keys, entries[i].num_sort_keys);
          memset(&entries[i], 0, sizeof(RowSortEntry));
        }
      }
      count = write_idx;
    }

    /* LIMIT & OFFSET */
    uint32_t offset = (stmt->has_offset && stmt->offset_val > 0) ? (uint32_t)stmt->offset_val : 0;
    uint32_t limit = count;
    if (stmt->has_limit && stmt->limit_val >= 0) {
      limit = offset + (uint32_t)stmt->limit_val;
    }
    if (limit > count) limit = count;

    uint32_t yielded_count = 0;
    if (limit > offset) {
      yielded_count = limit - offset;
    }

    if (!stmt->is_explain_analyze) {
      for (uint32_t i = offset; i < limit; i++) {
        print_projected_row(stmt, def, entries[i].row);
      }
    } else {
      clock_gettime(CLOCK_MONOTONIC, &end_time);
      double elapsed_ms = (end_time.tv_sec - start_time.tv_sec) * 1000.0 +
                          (end_time.tv_nsec - start_time.tv_nsec) / 1000000.0;
      printf("QUERY PLAN & EXECUTION METRICS:\n");
      if (path_type == ACCESS_INDEX_SEEK) {
        const char* op_str = "=";
        if (seek_cond->op == OP_GT) op_str = ">";
        else if (seek_cond->op == OP_GTE) op_str = ">=";
        else if (seek_cond->op == OP_LT) op_str = "<";
        else if (seek_cond->op == OP_LTE) op_str = "<=";
        printf("  Plan: SEARCH TABLE %s USING INDEX _idx_%s_%s (%s %s %s)\n",
               def->name, def->name, def->columns[seek_col_idx].name,
               def->columns[seek_col_idx].name, op_str, seek_cond->raw_val);
      } else if (path_type == ACCESS_PK_SEEK) {
        const char* op_str = "=";
        if (seek_cond->op == OP_GT) op_str = ">";
        else if (seek_cond->op == OP_GTE) op_str = ">=";
        printf("  Plan: SEARCH TABLE %s USING PRIMARY KEY (%s %s %s)\n",
               def->name, def->columns[0].name, op_str, seek_cond->raw_val);
      } else {
        printf("  Plan: SCAN TABLE %s\n", def->name);
      }

      uint32_t depth = btree_depth(pager, (path_type == ACCESS_INDEX_SEEK) ? def->columns[seek_col_idx].index_root_page : def->root_page_num);
      printf("  B-Tree Traversal Depth: %u levels\n", depth);
      printf("  Buffer Cache Hits: %lu pages\n", (unsigned long)pager->cache_hits);
      printf("  Disk I/O Reads: %lu pages\n", (unsigned long)pager->disk_reads);
      printf("  Rows Scanned: %lu\n", (unsigned long)rows_scanned);
      printf("  Rows Yielded: %u\n", yielded_count);
      printf("  Execution Time: %.3f ms\n", elapsed_ms);
    }
    for (uint32_t i = 0; i < count; i++) {
      value_free_row(entries[i].row, def->num_cols);
      value_free_row(entries[i].sort_keys, entries[i].num_sort_keys);
    }
    free(entries);
    return EXECUTE_SUCCESS;
  }

  Value tbl_name_val;
  value_init(&tbl_name_val);
  value_set_text(&tbl_name_val, def->name);

  /* Query Planner: check if condition 0 matches a secondary index */
  bool use_index = false;
  uint32_t idx_col_idx = 0;
  if (wc->has_where && wc->num_conds > 0) {
    for (uint32_t c = 1; c < def->num_cols; c++) {
      if (strcmp(def->columns[c].name, wc->conds[0].col_name) == 0 && def->columns[c].has_index) {
        use_index = true;
        idx_col_idx = c;
        break;
      }
    }
  }

  bool pk_seek = (!use_index && wc->has_where && wc->num_conds > 0 &&
                  strcmp(wc->conds[0].col_name, "id") == 0 &&
                  (wc->conds[0].op == OP_EQ || wc->conds[0].op == OP_GT || wc->conds[0].op == OP_GTE));

  int seek_reg = 15; // Register 15 for index/pk seeks
  int limit_reg = 12;
  int zero_reg = 11;

  /* Initialize limit registers if LIMIT is requested */
  if (stmt->has_limit) {
    vdbe_add_inst(vm, OP_Integer, stmt->limit_val, 0, limit_reg, (Value){0});
    vm->regs[limit_reg].type = COL_INT;
    vdbe_add_inst(vm, OP_Integer, 0, 0, zero_reg, (Value){0});
    vm->regs[zero_reg].type = COL_INT;
  }

  /* Identify sorting column if ORDER BY is requested */
  int sort_col_idx = -1;
  if (stmt->has_order_by) {
    for (uint32_t c = 0; c < def->num_cols; c++) {
      if (strcmp(def->columns[c].name, stmt->order_by_col) == 0) {
        sort_col_idx = (int)c;
        break;
      }
    }
  }

  CollationType sort_coll = stmt->order_by_collation;
  if (sort_coll == COLL_NOT_SET && sort_col_idx >= 0 && sort_col_idx < (int)def->num_cols) {
    sort_coll = def->columns[sort_col_idx].collation;
  }
  if (sort_coll == COLL_NOT_SET) {
    sort_coll = COLL_BINARY;
  }

  if (use_index) {
    /* Open cursor 0 on Index Tree */
    TableDef idx_def;
    memset(&idx_def, 0, sizeof(TableDef));
    make_idx_name(idx_def.name, def->name, def->columns[idx_col_idx].name);
    Value idx_tbl_val;
    value_init(&idx_tbl_val);
    value_set_text(&idx_tbl_val, idx_def.name);

    vdbe_add_inst(vm, OP_OpenRead, 0, def->columns[idx_col_idx].index_root_page, 0, idx_tbl_val);
    value_free(&idx_tbl_val);
    
    /* Open cursor 1 on Main Table */
    vdbe_add_inst(vm, OP_OpenRead, 1, def->root_page_num, 0, tbl_name_val);
    value_free(&tbl_name_val);

    /* Position index cursor 0 */
    SingleCond* primary_cond = &wc->conds[0];
    if (primary_cond->op == OP_LT || primary_cond->op == OP_LTE) {
      vdbe_add_inst(vm, OP_Rewind, 0, 999, 0, (Value){0}); // jump to halt/sorter if empty
    } else {
      load_const_reg(vm, seek_reg, def->columns[idx_col_idx].type, primary_cond->raw_val);
      if (primary_cond->op == OP_GT) {
        vdbe_add_inst(vm, OP_SeekGT, 0, 999, seek_reg, (Value){0});
      } else {
        vdbe_add_inst(vm, OP_SeekGE, 0, 999, seek_reg, (Value){0});
      }
    }

    int loop_body = vm->num_insts;

    /* Load primary key id from index cursor 0 (column 1) into pk register 14 */
    int pk_reg = 14;
    vdbe_add_inst(vm, OP_Column, 0, 1, pk_reg, (Value){0});

    /* Seek main table cursor 1 using pk register 14 */
    vdbe_add_inst(vm, OP_SeekGE, 1, 998, pk_reg, (Value){0}); // skip if not found in main table

    /* Load all main table columns to registers 1 .. N */
    for (uint32_t c = 0; c < def->num_cols; c++) {
      vdbe_add_inst(vm, OP_Column, 1, c, c + 1, (Value){0});
    }

    /* Evaluate all WHERE conditions */
    for (uint32_t cond_idx = 0; cond_idx < wc->num_conds; cond_idx++) {
      SingleCond* cond = &wc->conds[cond_idx];
      int const_reg = 10 + cond_idx;
      
      int col_idx = -1;
      for (uint32_t c = 0; c < def->num_cols; c++) {
        if (strcmp(def->columns[c].name, cond->col_name) == 0) {
          col_idx = c;
          break;
        }
      }
      if (col_idx == -1) continue;

      load_const_reg(vm, const_reg, def->columns[col_idx].type, cond->raw_val);
      CollationType cond_coll = cond->collation;
      if (cond_coll == COLL_NOT_SET && col_idx >= 0 && col_idx < (int)def->num_cols) {
        cond_coll = def->columns[col_idx].collation;
      }
      if (cond_coll == COLL_NOT_SET) {
        cond_coll = COLL_BINARY;
      }
      Value coll_val;
      memset(&coll_val, 0, sizeof(Value));
      coll_val.int_val = (int32_t)cond_coll;
      vdbe_add_inst(vm, OP_Compare, col_idx + 1, const_reg, cond->op, coll_val);
      vdbe_add_inst(vm, OP_IfFalse, 998, 0, 0, (Value){0}); // skip this row if condition fails
    }

    /* Output/Sorter processing */
    if (stmt->has_order_by) {
      Value sort_coll_val;
      memset(&sort_coll_val, 0, sizeof(Value));
      sort_coll_val.int_val = (int32_t)sort_coll;
      vdbe_add_inst(vm, OP_SorterInsert, sort_col_idx + 1, 1, def->num_cols, sort_coll_val);
    } else {
      if (stmt->has_limit) {
        vdbe_add_inst(vm, OP_Compare, limit_reg, zero_reg, OP_LTE, (Value){0});
        vdbe_add_inst(vm, OP_IfFalse, vm->num_insts + 2, 0, 0, (Value){0});
        vdbe_add_inst(vm, OP_Halt, 0, 0, 0, (Value){0});
      }
      vdbe_add_inst(vm, OP_ResultRow, 1, def->num_cols, 0, (Value){0});
      if (stmt->has_limit) {
        vdbe_add_inst(vm, OP_Dec, limit_reg, 0, 0, (Value){0});
      }
    }

    /* Patch targets */
    int next_pc = vm->num_insts;
    vdbe_add_inst(vm, OP_Next, 0, loop_body, 0, (Value){0});
    int main_loop_exit_pc = vm->num_insts;

    /* Compile sorting and output phase if ORDER BY requested */
    int halt_pc = main_loop_exit_pc;
    if (stmt->has_order_by) {
      vdbe_add_inst(vm, OP_SorterSort, stmt->order_by_desc ? 1 : 0, 0, 0, (Value){0});
      
      int sorter_rewind_pc = vm->num_insts;
      vdbe_add_inst(vm, OP_SorterRewind, 999, 1, def->num_cols, (Value){0}); // target patched later
      
      int sorter_loop_body = vm->num_insts;
      if (stmt->has_limit) {
        vdbe_add_inst(vm, OP_Compare, limit_reg, zero_reg, OP_LTE, (Value){0});
        vdbe_add_inst(vm, OP_IfFalse, vm->num_insts + 2, 0, 0, (Value){0});
        vdbe_add_inst(vm, OP_Halt, 0, 0, 0, (Value){0});
      }
      vdbe_add_inst(vm, OP_ResultRow, 1, def->num_cols, 0, (Value){0});
      if (stmt->has_limit) {
        vdbe_add_inst(vm, OP_Dec, limit_reg, 0, 0, (Value){0});
      }
      vdbe_add_inst(vm, OP_SorterNext, sorter_loop_body, 1, def->num_cols, (Value){0});
      
      halt_pc = vm->num_insts;
      vdbe_add_inst(vm, OP_Halt, 0, 0, 0, (Value){0});
      
      /* Patch sorter rewind exit target */
      vm->insts[sorter_rewind_pc].p1 = halt_pc;
    } else {
      vdbe_add_inst(vm, OP_Halt, 0, 0, 0, (Value){0});
    }

    for (uint32_t i = 0; i < vm->num_insts; i++) {
      if (vm->insts[i].p1 == 999) vm->insts[i].p1 = main_loop_exit_pc;
      if (vm->insts[i].p2 == 999) vm->insts[i].p2 = main_loop_exit_pc;
      if (vm->insts[i].p1 == 998) vm->insts[i].p1 = next_pc;
      if (vm->insts[i].p2 == 998) vm->insts[i].p2 = next_pc;
    }

  } else {
    /* Open cursor 0 on Main Table */
    vdbe_add_inst(vm, OP_OpenRead, 0, def->root_page_num, 0, tbl_name_val);

    /* Primary key seek optimization check */
    if (pk_seek) {
      int target_val = atoi(wc->conds[0].raw_val);
      vdbe_add_inst(vm, OP_Integer, target_val, 0, seek_reg, (Value){0});
      vm->regs[seek_reg].type = COL_INT;

      if (wc->conds[0].op == OP_GT) {
        vdbe_add_inst(vm, OP_SeekGT, 0, 999, seek_reg, (Value){0});
      } else {
        vdbe_add_inst(vm, OP_SeekGE, 0, 999, seek_reg, (Value){0});
      }
    } else {
      vdbe_add_inst(vm, OP_Rewind, 0, 999, 0, (Value){0});
    }

    int loop_body = vm->num_insts;

    /* Load column values to registers 1 to N */
    for (uint32_t c = 0; c < def->num_cols; c++) {
      vdbe_add_inst(vm, OP_Column, 0, c, c + 1, (Value){0});
    }

    /* Evaluate WHERE conditions */
    if (wc->has_where) {
      for (uint32_t cond_idx = 0; cond_idx < wc->num_conds; cond_idx++) {
        SingleCond* cond = &wc->conds[cond_idx];
        int const_reg = 10 + cond_idx;
        
        int col_idx = -1;
        for (uint32_t c = 0; c < def->num_cols; c++) {
          if (strcmp(def->columns[c].name, cond->col_name) == 0) {
            col_idx = c;
            break;
          }
        }
        if (col_idx == -1) continue;

        load_const_reg(vm, const_reg, def->columns[col_idx].type, cond->raw_val);
        CollationType cond_coll = cond->collation;
        if (cond_coll == COLL_NOT_SET && col_idx >= 0 && col_idx < (int)def->num_cols) {
          cond_coll = def->columns[col_idx].collation;
        }
        if (cond_coll == COLL_NOT_SET) {
          cond_coll = COLL_BINARY;
        }
        Value coll_val;
        memset(&coll_val, 0, sizeof(Value));
        coll_val.int_val = (int32_t)cond_coll;
        vdbe_add_inst(vm, OP_Compare, col_idx + 1, const_reg, cond->op, coll_val);
        vdbe_add_inst(vm, OP_IfFalse, 998, 0, 0, (Value){0});
      }
    }

    /* Output/Sorter processing */
    if (stmt->has_order_by) {
      Value sort_coll_val;
      memset(&sort_coll_val, 0, sizeof(Value));
      sort_coll_val.int_val = (int32_t)sort_coll;
      vdbe_add_inst(vm, OP_SorterInsert, sort_col_idx + 1, 1, def->num_cols, sort_coll_val);
    } else {
      if (stmt->has_limit) {
        vdbe_add_inst(vm, OP_Compare, limit_reg, zero_reg, OP_LTE, (Value){0});
        vdbe_add_inst(vm, OP_IfFalse, vm->num_insts + 2, 0, 0, (Value){0});
        vdbe_add_inst(vm, OP_Halt, 0, 0, 0, (Value){0});
      }
      vdbe_add_inst(vm, OP_ResultRow, 1, def->num_cols, 0, (Value){0});
      if (stmt->has_limit) {
        vdbe_add_inst(vm, OP_Dec, limit_reg, 0, 0, (Value){0});
      }
    }

    /* Patch targets */
    int next_pc = vm->num_insts;
    vdbe_add_inst(vm, OP_Next, 0, loop_body, 0, (Value){0});
    int main_loop_exit_pc = vm->num_insts;

    /* Compile sorting and output phase if ORDER BY requested */
    int halt_pc = main_loop_exit_pc;
    if (stmt->has_order_by) {
      vdbe_add_inst(vm, OP_SorterSort, stmt->order_by_desc ? 1 : 0, 0, 0, (Value){0});
      
      int sorter_rewind_pc = vm->num_insts;
      vdbe_add_inst(vm, OP_SorterRewind, 999, 1, def->num_cols, (Value){0}); // target patched later
      
      int sorter_loop_body = vm->num_insts;
      if (stmt->has_limit) {
        vdbe_add_inst(vm, OP_Compare, limit_reg, zero_reg, OP_LTE, (Value){0});
        vdbe_add_inst(vm, OP_IfFalse, vm->num_insts + 2, 0, 0, (Value){0});
        vdbe_add_inst(vm, OP_Halt, 0, 0, 0, (Value){0});
      }
      vdbe_add_inst(vm, OP_ResultRow, 1, def->num_cols, 0, (Value){0});
      if (stmt->has_limit) {
        vdbe_add_inst(vm, OP_Dec, limit_reg, 0, 0, (Value){0});
      }
      vdbe_add_inst(vm, OP_SorterNext, sorter_loop_body, 1, def->num_cols, (Value){0});
      
      halt_pc = vm->num_insts;
      vdbe_add_inst(vm, OP_Halt, 0, 0, 0, (Value){0});
      
      /* Patch sorter rewind exit target */
      vm->insts[sorter_rewind_pc].p1 = halt_pc;
    } else {
      vdbe_add_inst(vm, OP_Halt, 0, 0, 0, (Value){0});
    }

    for (uint32_t i = 0; i < vm->num_insts; i++) {
      if (vm->insts[i].p1 == 999) vm->insts[i].p1 = main_loop_exit_pc;
      if (vm->insts[i].p2 == 999) vm->insts[i].p2 = main_loop_exit_pc;
      if (vm->insts[i].p1 == 998) vm->insts[i].p1 = next_pc;
      if (vm->insts[i].p2 == 998) vm->insts[i].p2 = next_pc;
    }
  }

  if (stmt->is_explain) {
    /* Print query plan */
    if (use_index) {
      const char* op_str = "=";
      if (wc->conds[0].op == OP_GT) op_str = ">";
      else if (wc->conds[0].op == OP_GTE) op_str = ">=";
      else if (wc->conds[0].op == OP_LT) op_str = "<";
      else if (wc->conds[0].op == OP_LTE) op_str = "<=";
      printf("QUERY PLAN:\n");
      printf("  SEARCH TABLE %s USING INDEX _idx_%s_%s (%s %s %s)\n",
             def->name, def->name, def->columns[idx_col_idx].name,
             def->columns[idx_col_idx].name, op_str, wc->conds[0].raw_val);
    } else if (pk_seek) {
      printf("QUERY PLAN:\n");
      printf("  SEARCH TABLE %s USING PRIMARY KEY (id = %s)\n",
             def->name, wc->conds[0].raw_val);
    } else {
      printf("QUERY PLAN:\n");
      printf("  SCAN TABLE %s\n", def->name);
    }
    printf("\n");
    vdbe_print_program(vm);
  } else {
    vdbe_run(vm);
  }
  vdbe_free(vm);
  return EXECUTE_SUCCESS;
}

typedef struct HashJoinNode {
  Value key;
  Value vals[MAX_COLUMNS];
  bool  matched;
  struct HashJoinNode* next;
} HashJoinNode;

#define HASH_JOIN_BUCKETS 1024

static inline uint32_t compute_hash_join_key(const Value* v) {
  if (v == NULL || v->is_null) return 0;
  uint32_t h = 2166136261u;
  if (v->int_val != 0) {
    const uint8_t* p = (const uint8_t*)&v->int_val;
    for (size_t i = 0; i < 4; i++) { h ^= p[i]; h *= 16777619u; }
  } else if (v->double_val != 0.0) {
    const uint8_t* p = (const uint8_t*)&v->double_val;
    for (size_t i = 0; i < 8; i++) { h ^= p[i]; h *= 16777619u; }
  } else {
    for (const char* p = v->text_val; *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }
  }
  return h % HASH_JOIN_BUCKETS;
}

typedef struct {
  uint32_t num_vals;
  Value vals[MAX_COLUMNS];
} JoinedStreamRow;

/* Compile and run multi-table chained Hash Join engine */
static ExecuteResult run_join_select_vm(Statement* stmt, TableDef* left_def, Catalog* catalog, Pager* pager) {
  uint32_t num_joins = (stmt->num_joins > 0) ? stmt->num_joins : 1;
  JoinItem joins_copy[MAX_JOINS];
  if (stmt->num_joins > 0) {
    for (uint32_t j = 0; j < stmt->num_joins; j++) joins_copy[j] = stmt->joins[j];
  } else {
    joins_copy[0].type = stmt->join_clause.type;
    strcpy(joins_copy[0].right_table, stmt->join_table_name);
    strcpy(joins_copy[0].left_col, stmt->join_clause.left_col);
    strcpy(joins_copy[0].right_col, stmt->join_clause.right_col);
  }

  /* Combined schema representing current joined stream */
  TableDef combined_def;
  memset(&combined_def, 0, sizeof(TableDef));
  snprintf(combined_def.name, sizeof(combined_def.name), "%s", left_def->name);
  combined_def.num_cols = left_def->num_cols;
  for (uint32_t c = 0; c < left_def->num_cols; c++) {
    combined_def.columns[c] = left_def->columns[c];
  }

  /* Initial stream from base table left_def */
  Table left_tbl = { pager, left_def };
  Cursor* l_cur = btree_start(&left_tbl);
  uint32_t stream_cap = 64;
  uint32_t stream_count = 0;
  JoinedStreamRow* stream = malloc(sizeof(JoinedStreamRow) * stream_cap);
  if (!stream) {
    free(l_cur);
    return EXECUTE_CATALOG_FULL;
  }

  time_t now_ts = time(NULL);
  uint64_t snapshot_xid = get_current_snapshot_xid(pager);
  while (!l_cur->end_of_table) {
    if (stream_count >= stream_cap) {
      stream_cap *= 2;
      stream = realloc(stream, sizeof(JoinedStreamRow) * stream_cap);
    }
    Value row_vals[MAX_COLUMNS];
    if (!row_is_visible_and_active(left_def, cursor_value(l_cur), snapshot_xid, row_vals, (uint64_t)now_ts)) {
      cursor_advance(l_cur);
      continue;
    }
    stream[stream_count].num_vals = left_def->num_cols;
    for (uint32_t c = 0; c < left_def->num_cols; c++) {
      value_init(&stream[stream_count].vals[c]);
      value_copy(&stream[stream_count].vals[c], &row_vals[c]);
    }
    value_free_row(row_vals, left_def->num_cols);
    stream_count++;
    cursor_advance(l_cur);
  }
  free(l_cur);

  /* Iteratively execute chained joins */
  for (uint32_t j = 0; j < num_joins; j++) {
    JoinItem* ji = &joins_copy[j];
    TableDef* right_def = catalog_find(catalog, ji->right_table);
    if (right_def == NULL) {
      for (uint32_t i = 0; i < stream_count; i++) {
        value_free_row(stream[i].vals, combined_def.num_cols);
      }
      free(stream);
      return EXECUTE_TABLE_NOT_FOUND;
    }

    /* Resolve left join column in combined_def */
    int left_col_idx = -1;
    for (uint32_t c = 0; c < combined_def.num_cols; c++) {
      if (strcasecmp(combined_def.columns[c].name, ji->left_col) == 0) {
        left_col_idx = (int)c;
        break;
      }
    }
    if (left_col_idx == -1) {
      char* ldot = strchr(ji->left_col, '.');
      if (ldot) {
        for (uint32_t c = 0; c < combined_def.num_cols; c++) {
          if (strcasecmp(combined_def.columns[c].name, ldot + 1) == 0) {
            left_col_idx = (int)c;
            break;
          }
        }
      }
    }

    /* Resolve right join column in right_def */
    int right_col_idx = -1;
    for (uint32_t c = 0; c < right_def->num_cols; c++) {
      if (strcasecmp(right_def->columns[c].name, ji->right_col) == 0) {
        right_col_idx = (int)c;
        break;
      }
    }
    if (right_col_idx == -1) {
      char* rdot = strchr(ji->right_col, '.');
      if (rdot) {
        for (uint32_t c = 0; c < right_def->num_cols; c++) {
          if (strcasecmp(right_def->columns[c].name, rdot + 1) == 0) {
            right_col_idx = (int)c;
            break;
          }
        }
      }
    }

    if (left_col_idx == -1 || right_col_idx == -1) {
      for (uint32_t i = 0; i < stream_count; i++) {
        value_free_row(stream[i].vals, combined_def.num_cols);
      }
      free(stream);
      return EXECUTE_BAD_SCHEMA;
    }

    /* Build In-Memory Hash Table on right_def */
    HashJoinNode* buckets[HASH_JOIN_BUCKETS];
    memset(buckets, 0, sizeof(buckets));

    Table right_tbl = { pager, right_def };
    Cursor* r_cur = btree_start(&right_tbl);
    while (!r_cur->end_of_table) {
      Value r_vals[MAX_COLUMNS];
      if (!row_is_visible_and_active(right_def, cursor_value(r_cur), snapshot_xid, r_vals, (uint64_t)now_ts)) {
        cursor_advance(r_cur);
        continue;
      }
      Value* key = &r_vals[right_col_idx];

      HashJoinNode* node = malloc(sizeof(HashJoinNode));
      if (node) {
        memset(node, 0, sizeof(HashJoinNode));
        value_copy(&node->key, key);
        for (uint32_t c = 0; c < right_def->num_cols; c++) {
          value_copy(&node->vals[c], &r_vals[c]);
        }
        uint32_t bucket = compute_hash_join_key(key);
        node->next = buckets[bucket];
        buckets[bucket] = node;
      }
      value_free_row(r_vals, right_def->num_cols);
      cursor_advance(r_cur);
    }
    free(r_cur);

    /* Probe Hash Table from current stream */
    uint32_t next_cap = stream_count * 2 + 16;
    uint32_t next_count = 0;
    JoinedStreamRow* next_stream = malloc(sizeof(JoinedStreamRow) * next_cap);
    if (!next_stream) {
      for (int b = 0; b < HASH_JOIN_BUCKETS; b++) {
        HashJoinNode* cur = buckets[b];
        while (cur) {
          HashJoinNode* nx = cur->next;
          value_free(&cur->key);
          value_free_row(cur->vals, right_def->num_cols);
          free(cur);
          cur = nx;
        }
      }
      for (uint32_t i = 0; i < stream_count; i++) {
        value_free_row(stream[i].vals, combined_def.num_cols);
      }
      free(stream);
      return EXECUTE_CATALOG_FULL;
    }

    ColumnType l_type = combined_def.columns[left_col_idx].type;

    for (uint32_t i = 0; i < stream_count; i++) {
      Value* l_key = &stream[i].vals[left_col_idx];
      bool matched = false;

      if (!l_key->is_null) {
        uint32_t bucket = compute_hash_join_key(l_key);
        HashJoinNode* cur = buckets[bucket];
        while (cur) {
          if (!cur->key.is_null && compare_values(l_type, l_key, &cur->key) == 0) {
            matched = true;
            cur->matched = true;
            if (next_count >= next_cap) {
              next_cap *= 2;
              next_stream = realloc(next_stream, sizeof(JoinedStreamRow) * next_cap);
            }
            JoinedStreamRow* nr = &next_stream[next_count++];
            nr->num_vals = combined_def.num_cols + right_def->num_cols;
            for (uint32_t c = 0; c < combined_def.num_cols; c++) {
              value_init(&nr->vals[c]);
              value_copy(&nr->vals[c], &stream[i].vals[c]);
            }
            for (uint32_t c = 0; c < right_def->num_cols; c++) {
              value_init(&nr->vals[combined_def.num_cols + c]);
              value_copy(&nr->vals[combined_def.num_cols + c], &cur->vals[c]);
            }
          }
          cur = cur->next;
        }
      }

      if (!matched && (ji->type == JOIN_LEFT || ji->type == JOIN_FULL)) {
        if (next_count >= next_cap) {
          next_cap *= 2;
          next_stream = realloc(next_stream, sizeof(JoinedStreamRow) * next_cap);
        }
        JoinedStreamRow* nr = &next_stream[next_count++];
        nr->num_vals = combined_def.num_cols + right_def->num_cols;
        for (uint32_t c = 0; c < combined_def.num_cols; c++) {
          value_init(&nr->vals[c]);
          value_copy(&nr->vals[c], &stream[i].vals[c]);
        }
        for (uint32_t c = 0; c < right_def->num_cols; c++) {
          value_init(&nr->vals[combined_def.num_cols + c]);
          nr->vals[combined_def.num_cols + c].is_null = true;
        }
      }
    }

    if (ji->type == JOIN_RIGHT || ji->type == JOIN_FULL) {
      for (int b = 0; b < HASH_JOIN_BUCKETS; b++) {
        HashJoinNode* cur = buckets[b];
        while (cur) {
          if (!cur->matched) {
            if (next_count >= next_cap) {
              next_cap *= 2;
              next_stream = realloc(next_stream, sizeof(JoinedStreamRow) * next_cap);
            }
            JoinedStreamRow* nr = &next_stream[next_count++];
            nr->num_vals = combined_def.num_cols + right_def->num_cols;
            for (uint32_t c = 0; c < combined_def.num_cols; c++) {
              value_init(&nr->vals[c]);
              nr->vals[c].is_null = true;
            }
            for (uint32_t c = 0; c < right_def->num_cols; c++) {
              value_init(&nr->vals[combined_def.num_cols + c]);
              value_copy(&nr->vals[combined_def.num_cols + c], &cur->vals[c]);
            }
          }
          cur = cur->next;
        }
      }
    }

    for (int b = 0; b < HASH_JOIN_BUCKETS; b++) {
      HashJoinNode* cur = buckets[b];
      while (cur) {
        HashJoinNode* nx = cur->next;
        value_free(&cur->key);
        value_free_row(cur->vals, right_def->num_cols);
        free(cur);
        cur = nx;
      }
    }

    /* Update combined_def schema */
    for (uint32_t c = 0; c < right_def->num_cols; c++) {
      if (combined_def.num_cols < MAX_COLUMNS) {
        combined_def.columns[combined_def.num_cols++] = right_def->columns[c];
      }
    }

    for (uint32_t i = 0; i < stream_count; i++) {
      value_free_row(stream[i].vals, combined_def.num_cols - right_def->num_cols);
    }
    free(stream);
    stream = next_stream;
    stream_count = next_count;
  }

  /* Filter with WHERE clause, apply ORDER BY, LIMIT, and projection */
  RowSortEntry* sort_entries = NULL;
  uint32_t sort_count = 0;
  uint32_t sort_capacity = 0;
  if (stmt->has_order_by) {
    sort_capacity = stream_count + 16;
    sort_entries = malloc(sizeof(RowSortEntry) * sort_capacity);
  }

  int order_by_idx = -1;
  if (stmt->has_order_by) {
    for (uint32_t c = 0; c < combined_def.num_cols; c++) {
      if (strcasecmp(combined_def.columns[c].name, stmt->order_by_col) == 0) {
        order_by_idx = (int)c;
        break;
      }
    }
  }

  int rows_emitted = 0;
  int rows_skipped = 0;

  for (uint32_t i = 0; i < stream_count; i++) {
    if (!eval_where_clause(&combined_def, stream[i].vals, &stmt->where_clause, catalog, pager)) {
      continue;
    }

    if (stmt->has_order_by) {
      if (sort_count >= sort_capacity) {
        sort_capacity *= 2;
        sort_entries = realloc(sort_entries, sizeof(RowSortEntry) * sort_capacity);
      }
      RowSortEntry* e = &sort_entries[sort_count++];
      memset(e, 0, sizeof(RowSortEntry));
      for (uint32_t c = 0; c < combined_def.num_cols; c++) {
        value_copy(&e->row[c], &stream[i].vals[c]);
      }
      if (stmt->num_order_by > 0) {
        e->num_sort_keys = stmt->num_order_by;
        for (uint32_t k = 0; k < stmt->num_order_by; k++) {
          int k_idx = -1;
          for (uint32_t c = 0; c < combined_def.num_cols; c++) {
            if (strcasecmp(combined_def.columns[c].name, stmt->order_by_items[k].col_name) == 0) {
              k_idx = (int)c;
              break;
            }
          }
          if (k_idx != -1) {
            value_copy(&e->sort_keys[k], &stream[i].vals[k_idx]);
            e->sort_types[k] = combined_def.columns[k_idx].type;
          } else {
            char expr_out[256] = {0};
            eval_expr_string(stmt->order_by_items[k].col_name, &combined_def, stream[i].vals, expr_out, sizeof(expr_out));
            if (expr_out[0] != '\0') {
              if (strcasecmp(expr_out, "null") == 0) {
                e->sort_types[k] = COL_INT;
                e->sort_keys[k].is_null = true;
              } else {
                char* endptr = NULL;
                double dval = strtod(expr_out, &endptr);
                if (endptr != expr_out && (*endptr == '\0' || isspace((unsigned char)*endptr))) {
                  e->sort_types[k] = COL_DOUBLE;
                  e->sort_keys[k].double_val = dval;
                } else {
                  e->sort_types[k] = COL_TEXT;
                  value_set_text(&e->sort_keys[k], expr_out);
                }
              }
            }
          }
          e->sort_descs[k] = stmt->order_by_items[k].is_desc;
          e->sort_colls[k] = stmt->order_by_items[k].collation;
        }
      } else {
        e->num_sort_keys = 1;
        if (order_by_idx != -1) {
          value_copy(&e->sort_keys[0], &stream[i].vals[order_by_idx]);
          e->sort_types[0] = combined_def.columns[order_by_idx].type;
        } else {
          char expr_out[256] = {0};
          eval_expr_string(stmt->order_by_col, &combined_def, stream[i].vals, expr_out, sizeof(expr_out));
          if (expr_out[0] != '\0') {
            if (strcasecmp(expr_out, "null") == 0) {
              e->sort_types[0] = COL_INT;
              e->sort_keys[0].is_null = true;
            } else {
              char* endptr = NULL;
              double dval = strtod(expr_out, &endptr);
              if (endptr != expr_out && (*endptr == '\0' || isspace((unsigned char)*endptr))) {
                e->sort_types[0] = COL_DOUBLE;
                e->sort_keys[0].double_val = dval;
              } else {
                e->sort_types[0] = COL_TEXT;
                value_set_text(&e->sort_keys[0], expr_out);
              }
            }
          } else {
            e->sort_types[0] = COL_INT;
          }
        }
        e->sort_descs[0] = stmt->order_by_desc;
        e->sort_colls[0] = stmt->order_by_collation;
      }
      continue;
    }

    if (stmt->has_offset && rows_skipped < stmt->offset_val) {
      rows_skipped++;
      continue;
    }

    if (stmt->has_limit && rows_emitted >= stmt->limit_val) {
      break;
    }

    print_projected_row(stmt, &combined_def, stream[i].vals);
    rows_emitted++;
  }

  if (stmt->has_order_by && sort_entries) {
    qsort(sort_entries, sort_count, sizeof(RowSortEntry), compare_row_sort_entries);
    for (uint32_t i = 0; i < sort_count; i++) {
      if (stmt->has_offset && rows_skipped < stmt->offset_val) {
        rows_skipped++;
        continue;
      }
      if (stmt->has_limit && rows_emitted >= stmt->limit_val) {
        break;
      }
      print_projected_row(stmt, &combined_def, sort_entries[i].row);
      rows_emitted++;
    }
    for (uint32_t i = 0; i < sort_count; i++) {
      value_free_row(sort_entries[i].row, combined_def.num_cols);
      value_free_row(sort_entries[i].sort_keys, sort_entries[i].num_sort_keys);
    }
    free(sort_entries);
  }

  for (uint32_t i = 0; i < stream_count; i++) {
    value_free_row(stream[i].vals, combined_def.num_cols);
  }
  free(stream);
  return EXECUTE_SUCCESS;
}

bool eval_where_clause(TableDef* def, Value* row_vals, WhereClause* wc, Catalog* catalog, Pager* pager);

/* Compile and run aggregate SELECT statement */
static ExecuteResult run_aggregate_select(Statement* stmt, TableDef* def, Catalog* catalog, Pager* pager) {
  Table table = { pager, def };
  Cursor* cursor = btree_start(&table);

  /* Accumulators */
  int64_t counts[MAX_SELECT_COLS];
  double sums[MAX_SELECT_COLS];
  double mins[MAX_SELECT_COLS];
  double maxs[MAX_SELECT_COLS];
  bool has_values[MAX_SELECT_COLS];

  for (uint32_t i = 0; i < stmt->num_select_cols; i++) {
    counts[i] = 0;
    sums[i] = 0.0;
    mins[i] = 1e308;
    maxs[i] = -1e308;
    has_values[i] = false;
  }

  time_t now_ts = time(NULL);
  uint64_t snapshot_xid = get_current_snapshot_xid(pager);
  while (!cursor->end_of_table) {
    Value row_vals[MAX_COLUMNS];
    if (!row_is_visible_and_active(def, cursor_value(cursor), snapshot_xid, row_vals, (uint64_t)now_ts)) {
      cursor_advance(cursor);
      continue;
    }

    if (eval_where_clause(def, row_vals, &stmt->where_clause, catalog, pager)) {
      for (uint32_t i = 0; i < stmt->num_select_cols; i++) {
        SelectCol* sc = &stmt->select_cols[i];
        if (sc->func == AGG_COUNT_STAR) {
          counts[i]++;
        } else {
          int col_idx = -1;
          for (uint32_t c = 0; c < def->num_cols; c++) {
            if (strcmp(def->columns[c].name, sc->col_name) == 0) {
              col_idx = (int)c;
              break;
            }
          }
          if (col_idx != -1 && !row_vals[col_idx].is_null) {
            counts[i]++;
            double val = 0.0;
            if (def->columns[col_idx].type == COL_INT) val = (double)row_vals[col_idx].int_val;
            else if (def->columns[col_idx].type == COL_FLOAT) val = (double)row_vals[col_idx].float_val;
            else if (def->columns[col_idx].type == COL_DOUBLE || def->columns[col_idx].type == COL_NUMERIC || def->columns[col_idx].type == COL_DECIMAL) val = row_vals[col_idx].double_val;
            else if (def->columns[col_idx].type == COL_BOOL) val = row_vals[col_idx].bool_val ? 1.0 : 0.0;

            sums[i] += val;
            if (!has_values[i] || val < mins[i]) mins[i] = val;
            if (!has_values[i] || val > maxs[i]) maxs[i] = val;
            has_values[i] = true;
          }
        }
      }
    }
    cursor_advance(cursor);
  }
  free(cursor);

  /* Output aggregated row */
  printf("(");
  for (uint32_t i = 0; i < stmt->num_select_cols; i++) {
    SelectCol* sc = &stmt->select_cols[i];
    if (i > 0) printf(", ");

    switch (sc->func) {
      case AGG_COUNT_STAR:
      case AGG_COUNT:
        printf("%ld", counts[i]);
        break;
      case AGG_SUM:
        printf("%.2f", sums[i]);
        break;
      case AGG_AVG:
        printf("%.2f", counts[i] > 0 ? sums[i] / counts[i] : 0.0);
        break;
      case AGG_MIN:
        printf("%.2f", has_values[i] ? mins[i] : 0.0);
        break;
      case AGG_MAX:
        printf("%.2f", has_values[i] ? maxs[i] : 0.0);
        break;
      case AGG_NONE:
    }
  }
  printf(")\n");
  return EXECUTE_SUCCESS;
}

typedef struct {
  Value key;
  int64_t counts[MAX_SELECT_COLS];
  double sums[MAX_SELECT_COLS];
  double mins[MAX_SELECT_COLS];
  double maxs[MAX_SELECT_COLS];
  bool has_values[MAX_SELECT_COLS];
} GroupBucket;

static ExecuteResult run_group_by_select(Statement* stmt, TableDef* def, Catalog* catalog, Pager* pager) {
  int group_col_idx = -1;
  for (uint32_t c = 0; c < def->num_cols; c++) {
    if (strcmp(def->columns[c].name, stmt->group_by_col) == 0) {
      group_col_idx = (int)c;
      break;
    }
  }
  if (group_col_idx == -1) return EXECUTE_BAD_SCHEMA;

  Table table = { pager, def };
  Cursor* cursor = btree_start(&table);

  uint32_t capacity = 16;
  GroupBucket* buckets = malloc(sizeof(GroupBucket) * capacity);
  uint32_t num_buckets = 0;

  time_t now_ts = time(NULL);
  uint64_t snapshot_xid = get_current_snapshot_xid(pager);
  while (!cursor->end_of_table) {
    Value row_vals[MAX_COLUMNS];
    if (!row_is_visible_and_active(def, cursor_value(cursor), snapshot_xid, row_vals, (uint64_t)now_ts)) {
      cursor_advance(cursor);
      continue;
    }

    if (eval_where_clause(def, row_vals, &stmt->where_clause, catalog, pager)) {
      Value key = row_vals[group_col_idx];
      int bucket_idx = -1;

      for (uint32_t b = 0; b < num_buckets; b++) {
        bool match = false;
        if (buckets[b].key.is_null && key.is_null) {
          match = true;
        } else if (buckets[b].key.is_null != key.is_null) {
          match = false;
        } else {
          ColumnType type = def->columns[group_col_idx].type;
          if (type == COL_INT) match = (buckets[b].key.int_val == key.int_val);
          else if (type == COL_FLOAT) match = (buckets[b].key.float_val == key.float_val);
          else if (type == COL_DOUBLE || type == COL_NUMERIC || type == COL_DECIMAL) match = (buckets[b].key.double_val == key.double_val);
          else if (type == COL_BOOL) match = (buckets[b].key.bool_val == key.bool_val);
          else match = (strcmp(buckets[b].key.text_val, key.text_val) == 0);
        }

        if (match) {
          bucket_idx = (int)b;
          break;
        }
      }

      if (bucket_idx == -1) {
        if (num_buckets >= capacity) {
          capacity *= 2;
          buckets = realloc(buckets, sizeof(GroupBucket) * capacity);
        }
        bucket_idx = (int)num_buckets++;
        GroupBucket* gb = &buckets[bucket_idx];
        gb->key = key;
        for (uint32_t i = 0; i < stmt->num_select_cols; i++) {
          gb->counts[i] = 0;
          gb->sums[i] = 0.0;
          gb->mins[i] = 1e308;
          gb->maxs[i] = -1e308;
          gb->has_values[i] = false;
        }
      }

      GroupBucket* gb = &buckets[bucket_idx];
      for (uint32_t i = 0; i < stmt->num_select_cols; i++) {
        SelectCol* sc = &stmt->select_cols[i];
        if (sc->func == AGG_COUNT_STAR) {
          gb->counts[i]++;
        } else {
          int col_idx = -1;
          for (uint32_t c = 0; c < def->num_cols; c++) {
            if (strcmp(def->columns[c].name, sc->col_name) == 0) {
              col_idx = (int)c;
              break;
            }
          }
          if (col_idx != -1 && !row_vals[col_idx].is_null) {
            gb->counts[i]++;
            double val = 0.0;
            if (def->columns[col_idx].type == COL_INT) val = (double)row_vals[col_idx].int_val;
            else if (def->columns[col_idx].type == COL_FLOAT) val = (double)row_vals[col_idx].float_val;
            else if (def->columns[col_idx].type == COL_DOUBLE || def->columns[col_idx].type == COL_NUMERIC || def->columns[col_idx].type == COL_DECIMAL) val = row_vals[col_idx].double_val;
            else if (def->columns[col_idx].type == COL_BOOL) val = row_vals[col_idx].bool_val ? 1.0 : 0.0;

            gb->sums[i] += val;
            if (!gb->has_values[i] || val < gb->mins[i]) gb->mins[i] = val;
            if (!gb->has_values[i] || val > gb->maxs[i]) gb->maxs[i] = val;
            gb->has_values[i] = true;
          }
        }
      }
    }
    cursor_advance(cursor);
  }
  free(cursor);

  /* Output buckets */
  for (uint32_t b = 0; b < num_buckets; b++) {
    GroupBucket* gb = &buckets[b];

    /* Evaluate HAVING filter if present */
    if (stmt->has_having) {
      double h_val = 0.0;
      int having_idx = -1;
      for (uint32_t i = 0; i < stmt->num_select_cols; i++) {
        SelectCol* sc = &stmt->select_cols[i];
        if (sc->func == stmt->having_func) {
          if (stmt->having_func == AGG_COUNT_STAR || strcasecmp(sc->col_name, stmt->having_col) == 0) {
            having_idx = (int)i;
            break;
          }
        }
      }

      if (having_idx != -1) {
        if (stmt->having_func == AGG_COUNT_STAR || stmt->having_func == AGG_COUNT) {
          h_val = (double)gb->counts[having_idx];
        } else if (stmt->having_func == AGG_SUM) {
          h_val = gb->sums[having_idx];
        } else if (stmt->having_func == AGG_AVG) {
          h_val = gb->counts[having_idx] > 0 ? gb->sums[having_idx] / gb->counts[having_idx] : 0.0;
        } else if (stmt->having_func == AGG_MIN) {
          h_val = gb->has_values[having_idx] ? gb->mins[having_idx] : 0.0;
        } else if (stmt->having_func == AGG_MAX) {
          h_val = gb->has_values[having_idx] ? gb->maxs[having_idx] : 0.0;
        }
      } else {
        if (stmt->having_func == AGG_COUNT_STAR || stmt->having_func == AGG_COUNT) {
          h_val = (double)gb->counts[0];
        }
      }

      double target = atof(stmt->having_val);
      bool pass = false;
      switch (stmt->having_op) {
        case OP_EQ:  pass = (h_val == target); break;
        case OP_LT:  pass = (h_val < target); break;
        case OP_GT:  pass = (h_val > target); break;
        case OP_LTE: pass = (h_val <= target); break;
        case OP_GTE: pass = (h_val >= target); break;
        default:     pass = false; break;
      }
      if (!pass) continue;
    }

    printf("(");
    if (gb->key.is_null) {
      printf("NULL");
    } else {
      ColumnType ktype = def->columns[group_col_idx].type;
      if (ktype == COL_INT) printf("%d", gb->key.int_val);
      else if (ktype == COL_FLOAT) printf("%.2f", gb->key.float_val);
      else if (ktype == COL_DOUBLE || ktype == COL_NUMERIC || ktype == COL_DECIMAL) printf("%.2f", gb->key.double_val);
      else if (ktype == COL_BOOL) printf("%s", gb->key.bool_val ? "true" : "false");
      else printf("%s", gb->key.text_val);
    }

    for (uint32_t i = 0; i < stmt->num_select_cols; i++) {
      SelectCol* sc = &stmt->select_cols[i];
      if (sc->func == AGG_NONE && strcmp(sc->col_name, stmt->group_by_col) == 0) continue;
      printf(", ");
      switch (sc->func) {
        case AGG_COUNT_STAR:
        case AGG_COUNT:
          printf("%ld", gb->counts[i]);
          break;
        case AGG_SUM:
          printf("%.2f", gb->sums[i]);
          break;
        case AGG_AVG:
          printf("%.2f", gb->counts[i] > 0 ? gb->sums[i] / gb->counts[i] : 0.0);
          break;
        case AGG_MIN:
          printf("%.2f", gb->has_values[i] ? gb->mins[i] : 0.0);
          break;
        case AGG_MAX:
          printf("%.2f", gb->has_values[i] ? gb->maxs[i] : 0.0);
          break;
        case AGG_NONE:
          printf("N/A");
          break;
      }
    }
    printf(")\n");
  }
  free(buckets);
  return EXECUTE_SUCCESS;
}

static bool eval_where_clause_cols(const char* tbl_name, uint32_t num_cols, Column* cols, Value* row_vals, WhereClause* wc, Catalog* catalog, Pager* pager) {
  (void)tbl_name;
  if (wc == NULL || !wc->has_where || wc->num_conds == 0) return true;
  bool cond_results[MAX_WHERE_CONDS];
  memset(cond_results, 0, sizeof(cond_results));

  for (uint32_t i = 0; i < wc->num_conds; i++) {
    SingleCond* cond = &wc->conds[i];
    if (cond->is_exists || cond->is_not_exists) {
      if (catalog == NULL || pager == NULL) {
        cond_results[i] = false;
        continue;
      }
      TableDef* sub_def = catalog_find(catalog, cond->sub_table);
      if (sub_def == NULL) {
        cond_results[i] = false;
        continue;
      }

      Table sub_table = { pager, sub_def };
      Cursor* sub_cursor = btree_start(&sub_table);
      bool exists_match = false;

      while (!sub_cursor->end_of_table) {
        Value sub_row_vals[MAX_COLUMNS];
        deserialize_row(sub_def, cursor_value(sub_cursor), sub_row_vals);

        bool sub_pass = true;
        if (cond->has_sub_where) {
          int s_idx = -1;
          for (uint32_t c = 0; c < sub_def->num_cols; c++) {
            if (strcmp(sub_def->columns[c].name, cond->sub_where_col) == 0) {
              s_idx = (int)c;
              break;
            }
          }
          if (s_idx != -1) {
            Column* scol = &sub_def->columns[s_idx];
            Value* sval = &sub_row_vals[s_idx];
            Value sfilter;
            value_init(&sfilter);

            if (cond->sub_where_is_correlated) {
              int o_idx = -1;
              for (uint32_t c = 0; c < num_cols; c++) {
                if (strcmp(cols[c].name, cond->sub_correlated_outer_col) == 0) {
                  o_idx = (int)c;
                  break;
                }
              }
              if (o_idx != -1) {
                value_copy(&sfilter, &row_vals[o_idx]);
              }
            } else {
              if (scol->type == COL_INT) sfilter.int_val = atoi(cond->sub_where_val);
              else if (scol->type == COL_FLOAT) sfilter.float_val = (float)atof(cond->sub_where_val);
              else if (scol->type == COL_DOUBLE) sfilter.double_val = atof(cond->sub_where_val);
              else if (scol->type == COL_BOOL) sfilter.bool_val = (strcasecmp(cond->sub_where_val, "true") == 0 || strcmp(cond->sub_where_val, "1") == 0);
              else value_set_text(&sfilter, cond->sub_where_val);
            }

            int scmp = 0;
            if (scol->type == COL_INT) scmp = (sval->int_val > sfilter.int_val) - (sval->int_val < sfilter.int_val);
            else if (scol->type == COL_FLOAT) scmp = (sval->float_val > sfilter.float_val) - (sval->float_val < sfilter.float_val);
            else if (scol->type == COL_DOUBLE) scmp = (sval->double_val > sfilter.double_val) - (sval->double_val < sfilter.double_val);
            else if (scol->type == COL_BOOL) scmp = (sval->bool_val > sfilter.bool_val) - (sval->bool_val < sfilter.bool_val);
            else scmp = strcmp(sval->text_val, sfilter.text_val);

            value_free(&sfilter);

            switch (cond->sub_where_op) {
              case OP_EQ:  sub_pass = (scmp == 0); break;
              case OP_LT:  sub_pass = (scmp < 0); break;
              case OP_GT:  sub_pass = (scmp > 0); break;
              case OP_LTE: sub_pass = (scmp <= 0); break;
              case OP_GTE: sub_pass = (scmp >= 0); break;
              default:     sub_pass = false; break;
            }
          }
        }

        if (sub_pass) {
          exists_match = true;
          value_free_row(sub_row_vals, sub_def->num_cols);
          break;
        }
        value_free_row(sub_row_vals, sub_def->num_cols);
        cursor_advance(sub_cursor);
      }
      free(sub_cursor);

      if (cond->is_exists) cond_results[i] = exists_match;
      else if (cond->is_not_exists) cond_results[i] = !exists_match;
      continue;
    }

    int col_idx = -1;
    for (uint32_t c = 0; c < num_cols; c++) {
      if (strcasecmp(cols[c].name, cond->col_name) == 0) {
        col_idx = (int)c;
        break;
      }
    }
    if (col_idx == -1) {
      const char* dot = strchr(cond->col_name, '.');
      if (dot != NULL) {
        for (uint32_t c = 0; c < num_cols; c++) {
          if (strcasecmp(cols[c].name, dot + 1) == 0) {
            col_idx = (int)c;
            break;
          }
        }
      }
    }
    if (col_idx == -1) {
      cond_results[i] = false;
      continue;
    }
    
    Column* col = &cols[col_idx];
    Value* actual = &row_vals[col_idx];
    
    if (cond->is_subquery) {
      if (catalog == NULL || pager == NULL) {
        cond_results[i] = false;
        continue;
      }
      TableDef* sub_def = catalog_find(catalog, cond->sub_table);
      if (sub_def == NULL) {
        cond_results[i] = false;
        continue;
      }

      int sub_col_idx = -1;
      for (uint32_t c = 0; c < sub_def->num_cols; c++) {
        if (strcmp(sub_def->columns[c].name, cond->sub_col) == 0) {
          sub_col_idx = (int)c;
          break;
        }
      }
      if (sub_col_idx == -1) {
        cond_results[i] = false;
        continue;
      }

      Table sub_table = { pager, sub_def };
      Cursor* sub_cursor = btree_start(&sub_table);
      bool in_match = false;

      while (!sub_cursor->end_of_table) {
        Value sub_row_vals[MAX_COLUMNS];
        deserialize_row(sub_def, cursor_value(sub_cursor), sub_row_vals);

        bool sub_pass = true;
        if (cond->has_sub_where) {
          int s_idx = -1;
          for (uint32_t c = 0; c < sub_def->num_cols; c++) {
            if (strcmp(sub_def->columns[c].name, cond->sub_where_col) == 0) {
              s_idx = (int)c;
              break;
            }
          }
          if (s_idx != -1) {
            Column* scol = &sub_def->columns[s_idx];
            Value* sval = &sub_row_vals[s_idx];
            Value sfilter;
            value_init(&sfilter);
            if (scol->type == COL_INT) sfilter.int_val = atoi(cond->sub_where_val);
            else if (scol->type == COL_FLOAT) sfilter.float_val = (float)atof(cond->sub_where_val);
            else if (scol->type == COL_DOUBLE) sfilter.double_val = atof(cond->sub_where_val);
            else if (scol->type == COL_BOOL) sfilter.bool_val = (strcasecmp(cond->sub_where_val, "true") == 0 || strcmp(cond->sub_where_val, "1") == 0);
            else value_set_text(&sfilter, cond->sub_where_val);

            int scmp = 0;
            if (scol->type == COL_INT) scmp = (sval->int_val > sfilter.int_val) - (sval->int_val < sfilter.int_val);
            else if (scol->type == COL_FLOAT) scmp = (sval->float_val > sfilter.float_val) - (sval->float_val < sfilter.float_val);
            else if (scol->type == COL_DOUBLE) scmp = (sval->double_val > sfilter.double_val) - (sval->double_val < sfilter.double_val);
            else if (scol->type == COL_BOOL) scmp = (sval->bool_val > sfilter.bool_val) - (sval->bool_val < sfilter.bool_val);
            else scmp = strcmp(sval->text_val, sfilter.text_val);

            value_free(&sfilter);

            switch (cond->sub_where_op) {
              case OP_EQ:  sub_pass = (scmp == 0); break;
              case OP_LT:  sub_pass = (scmp < 0); break;
              case OP_GT:  sub_pass = (scmp > 0); break;
              case OP_LTE: sub_pass = (scmp <= 0); break;
              case OP_GTE: sub_pass = (scmp >= 0); break;
              default:     sub_pass = false; break;
            }
          }
        }

        if (sub_pass) {
          Value* sub_actual = &sub_row_vals[sub_col_idx];

          bool sub_eq = false;
          if (actual->is_null || sub_actual->is_null) {
            sub_eq = false;
          } else if (col->type == COL_INT) sub_eq = (actual->int_val == sub_actual->int_val);
          else if (col->type == COL_FLOAT) sub_eq = (actual->float_val == sub_actual->float_val);
          else if (col->type == COL_DOUBLE) sub_eq = (actual->double_val == sub_actual->double_val);
          else if (col->type == COL_BOOL) sub_eq = (actual->bool_val == sub_actual->bool_val);
          else sub_eq = (strcmp(actual->text_val, sub_actual->text_val) == 0);

          if (sub_eq) {
            in_match = true;
            value_free_row(sub_row_vals, sub_def->num_cols);
            break;
          }
        }
        value_free_row(sub_row_vals, sub_def->num_cols);
        cursor_advance(sub_cursor);
      }
      free(sub_cursor);
      cond_results[i] = in_match;
      continue;
    }

    if (cond->op == OP_IS_NULL) {
      cond_results[i] = actual->is_null;
      continue;
    }
    if (cond->op == OP_IS_NOT_NULL) {
      cond_results[i] = !actual->is_null;
      continue;
    }

    if (actual->is_null) {
      cond_results[i] = false;
      continue;
    }

    if (cond->op == OP_BETWEEN) {
      Value lower_v, upper_v;
      value_init(&lower_v);
      value_init(&upper_v);
      if (col->type == COL_INT) {
        lower_v.int_val = atoi(cond->raw_val);
        upper_v.int_val = atoi(cond->raw_val2);
      } else if (col->type == COL_FLOAT) {
        lower_v.float_val = (float)atof(cond->raw_val);
        upper_v.float_val = (float)atof(cond->raw_val2);
      } else if (col->type == COL_DOUBLE || col->type == COL_NUMERIC || col->type == COL_DECIMAL) {
        lower_v.double_val = atof(cond->raw_val);
        upper_v.double_val = atof(cond->raw_val2);
      } else {
        value_set_text(&lower_v, cond->raw_val);
        value_set_text(&upper_v, cond->raw_val2);
      }
      int cmp_l = compare_values(col->type, actual, &lower_v);
      int cmp_u = compare_values(col->type, actual, &upper_v);
      value_free(&lower_v);
      value_free(&upper_v);
      cond_results[i] = (cmp_l >= 0 && cmp_u <= 0);
      continue;
    }

    if (cond->op == OP_LIKE || cond->op == OP_NOT_LIKE) {
      static __thread char val_str[65536];
      if (col->type == COL_INT) snprintf(val_str, sizeof(val_str), "%d", actual->int_val);
      else if (col->type == COL_DOUBLE || col->type == COL_FLOAT) snprintf(val_str, sizeof(val_str), "%.8g", actual->double_val);
      else snprintf(val_str, sizeof(val_str), "%s", actual->text_val);

      bool m = sql_like_match(cond->raw_val, val_str, cond->collation);
      cond_results[i] = (cond->op == OP_LIKE) ? m : !m;
      continue;
    }

    if (cond->op == OP_GLOB || cond->op == OP_NOT_GLOB) {
      static __thread char val_str[65536];
      if (col->type == COL_INT) snprintf(val_str, sizeof(val_str), "%d", actual->int_val);
      else if (col->type == COL_DOUBLE || col->type == COL_FLOAT) snprintf(val_str, sizeof(val_str), "%.8g", actual->double_val);
      else snprintf(val_str, sizeof(val_str), "%s", actual->text_val);

      bool m = sql_glob_match(cond->raw_val, val_str);
      cond_results[i] = (cond->op == OP_GLOB) ? m : !m;
      continue;
    }

    /* Parse filter value */
    Value filter_val;
    value_init(&filter_val);
    switch (col->type) {
      case COL_INT:       filter_val.int_val = atoi(cond->raw_val); break;
      case COL_FLOAT:     filter_val.float_val = (float)atof(cond->raw_val); break;
      case COL_DOUBLE:
      case COL_NUMERIC:
      case COL_DECIMAL:   filter_val.double_val = atof(cond->raw_val); break;
      case COL_BOOL:      filter_val.bool_val = (strcasecmp(cond->raw_val, "true") == 0 || strcmp(cond->raw_val, "1") == 0); break;
      case COL_BLOB:
      case COL_DATETIME:
      case COL_DATE:
      case COL_TIME:
      case COL_TIMESTAMP:
      case COL_TEXT:
      case COL_VARCHAR:
      case COL_VECTOR:    value_set_text(&filter_val, cond->raw_val); break;
    }
    
    /* Compare */
    int cmp = 0;
    switch (col->type) {
      case COL_INT:       cmp = (actual->int_val > filter_val.int_val) - (actual->int_val < filter_val.int_val); break;
      case COL_FLOAT:     cmp = (actual->float_val > filter_val.float_val) - (actual->float_val < filter_val.float_val); break;
      case COL_DOUBLE:
      case COL_NUMERIC:
      case COL_DECIMAL:   cmp = (actual->double_val > filter_val.double_val) - (actual->double_val < filter_val.double_val); break;
      case COL_BOOL:      cmp = (actual->bool_val > filter_val.bool_val) - (actual->bool_val < filter_val.bool_val); break;
      case COL_BLOB:
      case COL_DATETIME:
      case COL_DATE:
      case COL_TIME:
      case COL_TIMESTAMP:
      case COL_TEXT:
      case COL_VARCHAR:
      case COL_VECTOR: {
        CollationType coll = cond->collation;
        if (coll == COLL_NOT_SET && col != NULL) {
          coll = col->collation;
        }
        if (coll == COLL_NOT_SET) {
          coll = COLL_BINARY;
        }
        cmp = compare_strings_collated(actual->text_val, filter_val.text_val, coll);
        break;
      }
    }
    value_free(&filter_val);
    
    bool match = false;
    switch (cond->op) {
      case OP_EQ:    match = (cmp == 0); break;
      case OP_LT:    match = (cmp < 0); break;
      case OP_GT:    match = (cmp > 0); break;
      case OP_LTE:   match = (cmp <= 0); break;
      case OP_GTE:   match = (cmp >= 0); break;
      case OP_MATCH: match = (strcasestr(actual->text_val, filter_val.text_val) != NULL); break;
      default:       match = false; break;
    }
    cond_results[i] = match;
  }

  bool overall = cond_results[0];
  for (uint32_t i = 1; i < wc->num_conds; i++) {
    if (wc->logic_ops[i - 1] == LOGIC_OR) {
      overall = overall || cond_results[i];
    } else {
      overall = overall && cond_results[i];
    }
  }
  return overall;
}

bool eval_where_clause(TableDef* def, Value* row_vals, WhereClause* wc, Catalog* catalog, Pager* pager) {
  if (def == NULL) return eval_where_clause_cols("", 0, NULL, row_vals, wc, catalog, pager);
  return eval_where_clause_cols(def->name, def->num_cols, def->columns, row_vals, wc, catalog, pager);
}

/* Compile and run DELETE statement on VDBE */
static ExecuteResult run_delete_vm(Statement* stmt, TableDef* def, Catalog* catalog, Pager* pager) {
  bool auto_tx = false;
  if (!pager->in_transaction) {
    pager_begin_transaction(pager);
    auto_tx = true;
  }
  if (!pager_ensure_write_lock(pager)) {
    if (auto_tx) pager_rollback(pager);
    return EXECUTE_BUSY;
  }
  uint64_t txn_xid = pager->current_commit_lsn ? pager->current_commit_lsn : (pager->wal_lsn + 1);
  time_t now_ts = time(NULL);

  /* 1. Scan table and collect IDs of matching rows */
  Table table = { pager, def };
  Cursor* cursor = btree_start(&table);
  
  uint32_t capacity = 16;
  int32_t* matching_ids = malloc(sizeof(int32_t) * capacity);
  uint32_t count = 0;
  
  /* Foreign Key ON DELETE check: ensure no child table references matching rows */
  for (uint32_t t = 0; t < catalog->num_tables; t++) {
    TableDef* child_def = &catalog->tables[t];
    for (uint32_t cc = 0; cc < child_def->num_cols; cc++) {
      if (child_def->columns[cc].has_fk && !child_def->columns[cc].fk_on_delete_cascade && strcmp(child_def->columns[cc].fk_target_table, def->name) == 0) {
        /* Table `child_def` references parent table `def` */
        Table child_tbl = { pager, child_def };
        Cursor* c_cur = btree_start(&child_tbl);
        while (!c_cur->end_of_table) {
          Value c_row[MAX_COLUMNS];
          if (row_is_visible_and_active(child_def, cursor_value(c_cur), txn_xid, c_row, (uint64_t)now_ts)) {
            Value* child_val = &c_row[cc];

            /* Check if this child row references any row queued for deletion */
            Cursor* p_cur = btree_start(&table);
            while (!p_cur->end_of_table) {
              Value p_row[MAX_COLUMNS];
              if (row_is_visible_and_active(def, cursor_value(p_cur), txn_xid, p_row, (uint64_t)now_ts)) {
                if (eval_where_clause(def, p_row, &stmt->where_clause, catalog, pager)) {
                  int target_pcol = 0;
                  for (uint32_t pc = 0; pc < def->num_cols; pc++) {
                    if (strcmp(def->columns[pc].name, child_def->columns[cc].fk_target_col) == 0) {
                      target_pcol = (int)pc;
                      break;
                    }
                  }
                  Value* parent_val = &p_row[target_pcol];
                  bool match = false;
                  ColumnType ktype = def->columns[target_pcol].type;
                  if (ktype == COL_INT) match = (child_val->int_val == parent_val->int_val);
                  else if (ktype == COL_FLOAT) match = (child_val->float_val == parent_val->float_val);
                  else if (ktype == COL_DOUBLE) match = (child_val->double_val == parent_val->double_val);
                  else if (ktype == COL_BOOL) match = (child_val->bool_val == parent_val->bool_val);
                  else match = (strcmp(child_val->text_val, parent_val->text_val) == 0);

                  if (match) {
                    value_free_row(p_row, def->num_cols);
                    value_free_row(c_row, child_def->num_cols);
                    free(p_cur);
                    free(c_cur);
                    free(cursor);
                    free(matching_ids);
                    return EXECUTE_CONSTRAINT_FOREIGN_KEY;
                  }
                }
                value_free_row(p_row, def->num_cols);
              }
              cursor_advance(p_cur);
            }
            free(p_cur);
            value_free_row(c_row, child_def->num_cols);
          }
          cursor_advance(c_cur);
        }
        free(c_cur);
      }
    }
  }

  while (!cursor->end_of_table) {
    Value row_vals[MAX_COLUMNS];
    if (row_is_visible_and_active(def, cursor_value(cursor), txn_xid, row_vals, (uint64_t)now_ts)) {
      if (eval_where_clause(def, row_vals, &stmt->where_clause, catalog, pager)) {
        if (count >= capacity) {
          capacity *= 2;
          matching_ids = realloc(matching_ids, sizeof(int32_t) * capacity);
        }
        matching_ids[count++] = row_vals[0].int_val;
      }
      value_free_row(row_vals, def->num_cols);
    }
    cursor_advance(cursor);
  }
  free(cursor);

  /* 2. Soft-delete each matching row (MVCC: stamp xmax = txn_xid) */
  for (uint32_t i = 0; i < count; i++) {
    Value target_id;
    memset(&target_id, 0, sizeof(Value));
    target_id.int_val = matching_ids[i];
    
    Cursor* cur = btree_find(&table, &target_id);
    while (!cur->end_of_table) {
      Value existing_key;
      btree_key_value(cur, &existing_key);
      if (existing_key.int_val != target_id.int_val) {
        value_free(&existing_key);
        break;
      }
      value_free(&existing_key);

      Value values[MAX_COLUMNS];
      if (row_is_visible_and_active(def, cursor_value(cur), txn_xid, values, (uint64_t)now_ts)) {
        uint64_t expire_at = 0, old_xmin = 0, old_xmax = 0;
        deserialize_row_with_mvcc(def, cursor_value(cur), NULL, &expire_at, &old_xmin, &old_xmax);
        (void)old_xmax;
        
        fire_triggers(catalog, pager, def, values, NULL, TRIGGER_BEFORE, TRIGGER_DELETE);

        /* Secondary index entries are NOT deleted during MVCC delete;
           they remain valid for older snapshots and will be cleaned up during vacuum_mvcc. */

        /* Check foreign key ON DELETE CASCADE */
        for (uint32_t t = 0; t < catalog->num_tables; t++) {
          TableDef* child_def = &catalog->tables[t];
          if (strcmp(child_def->name, def->name) == 0) continue;
          for (uint32_t c = 0; c < child_def->num_cols; c++) {
            Column* child_col = &child_def->columns[c];
            if (child_col->has_fk && child_col->fk_on_delete_cascade && strcmp(child_col->fk_target_table, def->name) == 0) {
              uint32_t target_pcol = 0;
              for (uint32_t pc = 0; pc < def->num_cols; pc++) {
                if (strlen(child_col->fk_target_col) > 0 && strcmp(def->columns[pc].name, child_col->fk_target_col) == 0) {
                  target_pcol = pc;
                  break;
                }
              }
              Statement* del_stmt = calloc(1, sizeof(Statement));
              if (del_stmt) {
                del_stmt->type = STATEMENT_DELETE;
                strcpy(del_stmt->table_name, child_def->name);
                del_stmt->where_clause.has_where = true;
                del_stmt->where_clause.num_conds = 1;
                strcpy(del_stmt->where_clause.conds[0].col_name, child_col->name);
                del_stmt->where_clause.conds[0].op = OP_EQ;
                if (def->columns[target_pcol].type == COL_INT) {
                  snprintf(del_stmt->where_clause.conds[0].raw_val, sizeof(del_stmt->where_clause.conds[0].raw_val), "%d", values[target_pcol].int_val);
                } else if (def->columns[target_pcol].type == COL_DOUBLE || def->columns[target_pcol].type == COL_FLOAT) {
                  snprintf(del_stmt->where_clause.conds[0].raw_val, sizeof(del_stmt->where_clause.conds[0].raw_val), "%.8g", values[target_pcol].double_val);
                } else {
                  snprintf(del_stmt->where_clause.conds[0].raw_val, sizeof(del_stmt->where_clause.conds[0].raw_val), "%s", values[target_pcol].text_val);
                }
                run_delete_vm(del_stmt, child_def, catalog, pager);
                free(del_stmt);
              }
            }
          }
        }

        /* Non-destructive delete: replace the cell with xmax = txn_xid */
        btree_delete(cur);
        Cursor* ins_cur = btree_find(&table, &target_id);
        btree_insert_with_mvcc(ins_cur, values, expire_at, old_xmin, txn_xid);
        free(ins_cur);

        fire_triggers(catalog, pager, def, values, NULL, TRIGGER_AFTER, TRIGGER_DELETE);
        print_returning_row(stmt, def, values);
        value_free_row(values, def->num_cols);
        break;
      }
      cursor_advance(cur);
    }
    free(cur);
  }
  
  free(matching_ids);
  if (pager->auto_vacuum) {
    execute_vacuum(catalog, pager);
  }
  if (auto_tx && pager->in_transaction) {
    catalog_save(catalog, pager);
    pager_commit(pager);
  }
  return EXECUTE_SUCCESS;
}

/* Compile and run UPDATE statement on VDBE */
static ExecuteResult run_update_vm(Statement* stmt, TableDef* def, Catalog* catalog, Pager* pager) {
  bool auto_tx = false;
  if (!pager->in_transaction) {
    pager_begin_transaction(pager);
    auto_tx = true;
  }
  if (!pager_ensure_write_lock(pager)) {
    if (auto_tx) pager_rollback(pager);
    return EXECUTE_BUSY;
  }
  uint64_t txn_xid = pager->current_commit_lsn ? pager->current_commit_lsn : (pager->wal_lsn + 1);
  time_t now_ts = time(NULL);

  /* 1. Scan table and collect IDs of matching rows */
  Table table = { pager, def };
  Cursor* cursor = btree_start(&table);
  
  uint32_t capacity = 16;
  int32_t* matching_ids = malloc(sizeof(int32_t) * capacity);
  uint32_t count = 0;
  
  while (!cursor->end_of_table) {
    Value row_vals[MAX_COLUMNS];
    if (row_is_visible_and_active(def, cursor_value(cursor), txn_xid, row_vals, (uint64_t)now_ts)) {
      if (eval_where_clause(def, row_vals, &stmt->where_clause, catalog, pager)) {
        if (count >= capacity) {
          capacity *= 2;
          matching_ids = realloc(matching_ids, sizeof(int32_t) * capacity);
        }
        matching_ids[count++] = row_vals[0].int_val;
      }
      value_free_row(row_vals, def->num_cols);
    }
    cursor_advance(cursor);
  }
  free(cursor);

  /* 2. Update each matching row (MVCC: old.xmax = txn_xid, new.xmin = txn_xid) */
  for (uint32_t i = 0; i < count; i++) {
    Value target_id;
    memset(&target_id, 0, sizeof(Value));
    target_id.int_val = matching_ids[i];
    
    Cursor* cur = btree_find(&table, &target_id);
    while (!cur->end_of_table) {
      Value existing_key;
      btree_key_value(cur, &existing_key);
      if (existing_key.int_val != target_id.int_val) {
        value_free(&existing_key);
        break;
      }
      value_free(&existing_key);

      Value old_values[MAX_COLUMNS];
      if (row_is_visible_and_active(def, cursor_value(cur), txn_xid, old_values, (uint64_t)now_ts)) {
        uint64_t expire_at = 0, old_xmin = 0, old_xmax = 0;
        deserialize_row_with_mvcc(def, cursor_value(cur), NULL, &expire_at, &old_xmin, &old_xmax);
        (void)old_xmax;

        Value values[MAX_COLUMNS];
        for (uint32_t c = 0; c < def->num_cols; c++) {
          value_init(&values[c]);
          value_copy(&values[c], &old_values[c]);
        }
        
        /* Apply updates */
        for (uint32_t u = 0; u < stmt->num_set_pairs; u++) {
          SetPair* pair = &stmt->set_pairs[u];
          
          int col_idx = -1;
          for (uint32_t c = 0; c < def->num_cols; c++) {
            if (strcmp(def->columns[c].name, pair->col_name) == 0) {
              col_idx = (int)c;
              break;
            }
          }
          if (col_idx < 0) {
            value_free_row(old_values, def->num_cols);
            value_free_row(values, def->num_cols);
            free(cur);
            free(matching_ids);
            return EXECUTE_BAD_SCHEMA;
          }

          Column* col = &def->columns[col_idx];
          if (pair->is_null) {
            values[col_idx].is_null = true;
          } else {
            char expr_res[256] = {0};
            eval_expr_string(pair->str_val, def, old_values, expr_res, sizeof(expr_res));
            const char* final_val = (strlen(expr_res) > 0) ? expr_res : pair->str_val;

            values[col_idx].is_null = false;
            switch (col->type) {
              case COL_INT:       values[col_idx].int_val = atoi(final_val); break;
              case COL_FLOAT:     values[col_idx].float_val = (float)atof(final_val); break;
              case COL_DOUBLE:
              case COL_NUMERIC:
              case COL_DECIMAL:   values[col_idx].double_val = atof(final_val); break;
              case COL_BOOL:      values[col_idx].bool_val = (strcasecmp(final_val, "true") == 0 || strcmp(final_val, "1") == 0); break;
              case COL_BLOB:
              case COL_DATETIME:
              case COL_DATE:
              case COL_TIME:
              case COL_TIMESTAMP:
              case COL_TEXT:
              case COL_VARCHAR:
              case COL_VECTOR:    value_set_text(&values[col_idx], final_val); break;
            }
          }
        }
        
        /* Fire BEFORE UPDATE trigger with old and new values */
        fire_triggers(catalog, pager, def, old_values, values, TRIGGER_BEFORE, TRIGGER_UPDATE);

        /* 1. Mark old row version dead (xmax = txn_xid) */
        btree_delete(cur);
        Cursor* ins_old = btree_find(&table, &target_id);
        btree_insert_with_mvcc(ins_old, old_values, expire_at, old_xmin, txn_xid);
        free(ins_old);

        /* 2. Insert new row version (xmin = txn_xid, xmax = 0) */
        Cursor* ins_new = btree_find(&table, &values[0]);
        btree_insert_with_mvcc(ins_new, values, expire_at, txn_xid, 0);
        free(ins_new);

        /* 3. Heap-Only Tuple (HOT) secondary index optimization:
              Only insert into secondary index if the indexed column value changed! */
        for (uint32_t c = 1; c < def->num_cols; c++) {
          if (def->columns[c].has_index) {
            Column* icol = &def->columns[c];
            bool col_changed = (old_values[c].is_null != values[c].is_null) ||
                               (compare_values(icol->type, &old_values[c], &values[c]) != 0);
            if (!col_changed) {
              /* HOT: column value unchanged, index already routes to PK */
              continue;
            }

            if (icol->idx_is_partial) {
              WhereClause idx_where;
              memset(&idx_where, 0, sizeof(WhereClause));
              idx_where.has_where = true;
              idx_where.num_conds = 1;
              strcpy(idx_where.conds[0].col_name, icol->idx_where_col);
              idx_where.conds[0].op = icol->idx_where_op;
              strcpy(idx_where.conds[0].raw_val, icol->idx_where_val);
              if (!eval_where_clause(def, values, &idx_where, catalog, pager)) {
                continue;
              }
            }

            TableDef idx_def;
            memset(&idx_def, 0, sizeof(TableDef));
            make_idx_name(idx_def.name, def->name, def->columns[c].name);
            idx_def.root_page_num = def->columns[c].index_root_page;
            idx_def.num_cols = 2;
            memcpy(&idx_def.columns[0], &def->columns[c], sizeof(Column));
            strcpy(idx_def.columns[1].name, "id");
            idx_def.columns[1].type = COL_INT;
            idx_def.columns[1].size = 4;
            tabledef_compute(&idx_def);

            Table idx_table = { pager, &idx_def };
            Value idx_values[2];
            value_init(&idx_values[0]);
            value_init(&idx_values[1]);
            value_copy(&idx_values[0], &values[c]);
            idx_values[0].is_null = values[c].is_null;
            if (icol->idx_is_expr) {
              if (strcasecmp(icol->idx_expr_func, "lower") == 0 && (icol->type == COL_TEXT || icol->type == COL_VARCHAR)) {
                for (char* p = idx_values[0].text_val; *p; p++) *p = (char)tolower((unsigned char)*p);
              } else if (strcasecmp(icol->idx_expr_func, "upper") == 0 && (icol->type == COL_TEXT || icol->type == COL_VARCHAR)) {
                for (char* p = idx_values[0].text_val; *p; p++) *p = (char)toupper((unsigned char)*p);
              }
            }
            idx_values[1].int_val = values[0].int_val;
            idx_values[1].is_null = false;

            Cursor* idx_cur = btree_find(&idx_table, &idx_values[0]);
            btree_insert(idx_cur, idx_values);
            value_free_row(idx_values, 2);
            free(idx_cur);
          }
        }

        /* Check foreign key ON UPDATE CASCADE */
        for (uint32_t t = 0; t < catalog->num_tables; t++) {
          TableDef* child_def = &catalog->tables[t];
          if (strcmp(child_def->name, def->name) == 0) continue;
          for (uint32_t c = 0; c < child_def->num_cols; c++) {
            Column* child_col = &child_def->columns[c];
            if (child_col->has_fk && child_col->fk_on_update_cascade && strcmp(child_col->fk_target_table, def->name) == 0) {
              uint32_t target_pcol = 0;
              for (uint32_t pc = 0; pc < def->num_cols; pc++) {
                if (strlen(child_col->fk_target_col) > 0 && strcmp(def->columns[pc].name, child_col->fk_target_col) == 0) {
                  target_pcol = pc;
                  break;
                }
              }
              Statement* upd_stmt = calloc(1, sizeof(Statement));
              if (upd_stmt) {
                upd_stmt->type = STATEMENT_UPDATE;
                strcpy(upd_stmt->table_name, child_def->name);
                upd_stmt->num_set_pairs = 1;
                strcpy(upd_stmt->set_pairs[0].col_name, child_col->name);
                if (def->columns[target_pcol].type == COL_INT) {
                  snprintf(upd_stmt->set_pairs[0].str_val, sizeof(upd_stmt->set_pairs[0].str_val), "%d", values[target_pcol].int_val);
                } else if (def->columns[target_pcol].type == COL_DOUBLE || def->columns[target_pcol].type == COL_FLOAT) {
                  snprintf(upd_stmt->set_pairs[0].str_val, sizeof(upd_stmt->set_pairs[0].str_val), "%.8g", values[target_pcol].double_val);
                } else {
                  snprintf(upd_stmt->set_pairs[0].str_val, sizeof(upd_stmt->set_pairs[0].str_val), "%s", values[target_pcol].text_val);
                }
                upd_stmt->where_clause.has_where = true;
                upd_stmt->where_clause.num_conds = 1;
                strcpy(upd_stmt->where_clause.conds[0].col_name, child_col->name);
                upd_stmt->where_clause.conds[0].op = OP_EQ;
                if (def->columns[target_pcol].type == COL_INT) {
                  snprintf(upd_stmt->where_clause.conds[0].raw_val, sizeof(upd_stmt->where_clause.conds[0].raw_val), "%d", target_id.int_val);
                } else if (def->columns[target_pcol].type == COL_DOUBLE || def->columns[target_pcol].type == COL_FLOAT) {
                  snprintf(upd_stmt->where_clause.conds[0].raw_val, sizeof(upd_stmt->where_clause.conds[0].raw_val), "%.8g", target_id.double_val);
                } else {
                  snprintf(upd_stmt->where_clause.conds[0].raw_val, sizeof(upd_stmt->where_clause.conds[0].raw_val), "%s", target_id.text_val);
                }
                run_update_vm(upd_stmt, child_def, catalog, pager);
                free(upd_stmt);
              }
            }
          }
        }
        fire_triggers(catalog, pager, def, old_values, values, TRIGGER_AFTER, TRIGGER_UPDATE);
        print_returning_row(stmt, def, values);
        value_free_row(old_values, def->num_cols);
        value_free_row(values, def->num_cols);
        break;
      }
      cursor_advance(cur);
    }
    free(cur);
  }
  
  free(matching_ids);
  if (auto_tx && pager->in_transaction) {
    catalog_save(catalog, pager);
    pager_commit(pager);
  }
  return EXECUTE_SUCCESS;
}

static ExecuteResult execute_create_table(Statement* stmt, Catalog* catalog, Pager* pager) {
  if (catalog->num_tables >= MAX_TABLES) {
    return EXECUTE_CATALOG_FULL;
  }

  TableDef* existing = catalog_find(catalog, stmt->new_table.name);
  if (existing != NULL) {
    /* Only suppress error when IF NOT EXISTS was explicitly written */
    return stmt->if_not_exists ? EXECUTE_SUCCESS : EXECUTE_TABLE_EXISTS;
  }

  /* Add to catalog */
  TableDef* def = &catalog->tables[catalog->num_tables];
  memcpy(def, &stmt->new_table, sizeof(TableDef));
  tabledef_compute(def);
  catalog->num_tables++;
  pager->reserved_catalog_pages = catalog_get_reserved_pages(catalog->num_tables);

  /* Initialize root page of B+ Tree */
  uint32_t root_page = get_unused_page_num(pager);
  void* root_node = get_page(pager, root_page);
  initialize_root_leaf(root_node);

  def->root_page_num = root_page;

  /* Save catalog metadata */
  catalog_save(catalog, pager);

  printf("Table '%s' created with root page %u.\n", def->name, root_page);

  /* If WITH HISTORY is enabled, create shadow table _history_<table> */
  if (def->with_history) {
    char hist_tbl_name[TBL_NAME_SIZE];
    snprintf(hist_tbl_name, sizeof(hist_tbl_name), "_history_%.50s", def->name);
    if (catalog->num_tables < MAX_TABLES && catalog_find(catalog, hist_tbl_name) == NULL) {
      Statement hist_stmt;
      memset(&hist_stmt, 0, sizeof(Statement));
      hist_stmt.type = STATEMENT_CREATE_TABLE;
      snprintf(hist_stmt.new_table.name, sizeof(hist_stmt.new_table.name), "%s", hist_tbl_name);
      hist_stmt.new_table.num_cols = 3 + def->num_cols;
      if (hist_stmt.new_table.num_cols > MAX_COLUMNS) hist_stmt.new_table.num_cols = MAX_COLUMNS;

      /* Column 0: history_id INT */
      strcpy(hist_stmt.new_table.columns[0].name, "history_id");
      hist_stmt.new_table.columns[0].type = COL_INT;
      hist_stmt.new_table.columns[0].size = 4;
      hist_stmt.new_table.columns[0].is_autoincrement = true;

      /* Column 1: history_action TEXT */
      strcpy(hist_stmt.new_table.columns[1].name, "history_action");
      hist_stmt.new_table.columns[1].type = COL_TEXT;
      hist_stmt.new_table.columns[1].size = 32;

      /* Column 2: history_time INT */
      strcpy(hist_stmt.new_table.columns[2].name, "history_time");
      hist_stmt.new_table.columns[2].type = COL_INT;
      hist_stmt.new_table.columns[2].size = 4;

      /* Columns 3..N: copy original table columns */
      for (uint32_t c = 0; c < def->num_cols && (3 + c) < MAX_COLUMNS; c++) {
        hist_stmt.new_table.columns[3 + c] = def->columns[c];
        /* Secondary index not copied to history table */
        hist_stmt.new_table.columns[3 + c].has_index = false;
        hist_stmt.new_table.columns[3 + c].index_root_page = 0;
        hist_stmt.new_table.columns[3 + c].is_autoincrement = false;
      }
      execute_create_table(&hist_stmt, catalog, pager);
    }
  }

  return EXECUTE_SUCCESS;
}

static ExecuteResult execute_drop_table(Statement* stmt, Catalog* catalog, Pager* pager) {
  uint32_t idx = UINT32_MAX;
  for (uint32_t i = 0; i < catalog->num_tables; i++) {
    if (strcmp(catalog->tables[i].name, stmt->table_name) == 0) {
      idx = i;
      break;
    }
  }

  if (idx == UINT32_MAX) {
    return EXECUTE_TABLE_NOT_FOUND;
  }

  /* Reclaim root page and index root pages to freelist */
  pager_free_page(pager, catalog->tables[idx].root_page_num);
  for (uint32_t c = 0; c < catalog->tables[idx].num_cols; c++) {
    if (catalog->tables[idx].columns[c].has_index) {
      pager_free_page(pager, catalog->tables[idx].columns[c].index_root_page);
    }
  }

  /* Shift tables down in catalog */
  for (uint32_t i = idx; i < catalog->num_tables - 1; i++) {
    catalog->tables[i] = catalog->tables[i + 1];
  }
  catalog->num_tables--;

  /* Save updated catalog to page 0 */
  catalog_save(catalog, pager);

  if (pager->auto_vacuum) {
    execute_vacuum(catalog, pager);
  }

  printf("Table '%s' dropped.\n", stmt->table_name);
  return EXECUTE_SUCCESS;
}

static ExecuteResult execute_create_index(Statement* stmt, Catalog* catalog, Pager* pager) {
  TableDef* def = catalog_find(catalog, stmt->table_name);
  if (def == NULL) {
    return EXECUTE_TABLE_NOT_FOUND;
  }

  bool auto_tx = false;
  if (!pager->in_transaction) {
    pager_begin_transaction(pager);
    auto_tx = true;
  }
  if (!pager_ensure_write_lock(pager)) {
    if (auto_tx) pager_rollback(pager);
    return EXECUTE_BUSY;
  }

  int col_idx = -1;
  for (uint32_t i = 0; i < def->num_cols; i++) {
    if (strcmp(def->columns[i].name, stmt->index_col_name) == 0) {
      col_idx = (int)i;
      break;
    }
  }
  if (col_idx == -1) {
    if (auto_tx) pager_rollback(pager);
    return EXECUTE_BAD_SCHEMA;
  }

  Column* col = &def->columns[col_idx];
  if (col->has_index) {
    if (auto_tx) pager_rollback(pager);
    printf("Index on column '%s' already exists.\n", col->name);
    return EXECUTE_SUCCESS;
  }

  /* Allocate root page for index B+ Tree */
  uint32_t root_page = get_unused_page_num(pager);
  void* root_node = get_page(pager, root_page);
  initialize_root_leaf(root_node);

  col->has_index = true;
  col->index_root_page = root_page;
  col->idx_is_partial = stmt->index_is_partial;
  if (stmt->index_is_partial && stmt->index_where.num_conds > 0) {
    strcpy(col->idx_where_col, stmt->index_where.conds[0].col_name);
    col->idx_where_op = stmt->index_where.conds[0].op;
    strcpy(col->idx_where_val, stmt->index_where.conds[0].raw_val);
  }
  col->idx_is_expr = stmt->index_is_expr;
  if (stmt->index_is_expr) {
    strcpy(col->idx_expr_func, stmt->index_expr_func);
  }
  catalog_save(catalog, pager);

  /* Build index by scanning main table */
  Table main_table = { pager, def };
  Cursor* cursor = btree_start(&main_table);
  Value main_values[MAX_COLUMNS];

  TableDef idx_def;
  memset(&idx_def, 0, sizeof(TableDef));
  make_idx_name(idx_def.name, def->name, col->name);
  idx_def.root_page_num = root_page;
  idx_def.num_cols = 2;
  memcpy(&idx_def.columns[0], col, sizeof(Column));
  strcpy(idx_def.columns[1].name, "id");
  idx_def.columns[1].type = COL_INT;
  idx_def.columns[1].size = 4;
  tabledef_compute(&idx_def);

  Table idx_table = { pager, &idx_def };

  while (!cursor->end_of_table) {
    deserialize_row(def, cursor_value(cursor), main_values);

    if (col->idx_is_partial) {
      WhereClause idx_where;
      memset(&idx_where, 0, sizeof(WhereClause));
      idx_where.has_where = true;
      idx_where.num_conds = 1;
      strcpy(idx_where.conds[0].col_name, col->idx_where_col);
      idx_where.conds[0].op = col->idx_where_op;
      strcpy(idx_where.conds[0].raw_val, col->idx_where_val);
      if (!eval_where_clause(def, main_values, &idx_where, catalog, pager)) {
        value_free_row(main_values, def->num_cols);
        cursor_advance(cursor);
        continue;
      }
    }

    Value idx_values[2];
    value_init(&idx_values[0]);
    value_init(&idx_values[1]);
    value_copy(&idx_values[0], &main_values[col_idx]);
    idx_values[0].is_null = main_values[col_idx].is_null;
    if (col->idx_is_expr) {
      if (strcasecmp(col->idx_expr_func, "lower") == 0 && (col->type == COL_TEXT || col->type == COL_VARCHAR)) {
        for (char* p = idx_values[0].text_val; *p; p++) *p = (char)tolower((unsigned char)*p);
      } else if (strcasecmp(col->idx_expr_func, "upper") == 0 && (col->type == COL_TEXT || col->type == COL_VARCHAR)) {
        for (char* p = idx_values[0].text_val; *p; p++) *p = (char)toupper((unsigned char)*p);
      }
    }
    idx_values[1].int_val = main_values[0].int_val; /* primary key id */
    idx_values[1].is_null = false;

    Cursor* idx_cur = btree_find(&idx_table, &idx_values[0]);
    btree_insert(idx_cur, idx_values);
    value_free_row(idx_values, 2);
    free(idx_cur);

    value_free_row(main_values, def->num_cols);
    cursor_advance(cursor);
  }
  free(cursor);

  char cols_str[256] = "";
  if (stmt->index_num_cols > 0) {
    for (uint32_t i = 0; i < stmt->index_num_cols; i++) {
      if (i > 0) strcat(cols_str, ", ");
      strcat(cols_str, stmt->index_cols[i]);
    }
  } else {
    strcpy(cols_str, col->name);
  }

  if (auto_tx && pager->in_transaction) {
    catalog_save(catalog, pager);
    pager_commit(pager);
  }

  printf("Index '%s' created on %s(%s) with root page %u.\n",
         stmt->index_name, def->name, cols_str, root_page);
  return EXECUTE_SUCCESS;
}

static ExecuteResult execute_vacuum_into(Statement* stmt, Catalog* catalog, Pager* pager) {
  Pager* target_pager = pager_open(stmt->vacuum_into_filename);
  if (!target_pager) return EXECUTE_BAD_SCHEMA;

  Catalog* target_catalog = malloc(sizeof(Catalog));
  if (!target_catalog) {
    pager_close(target_pager);
    return EXECUTE_BAD_SCHEMA;
  }
  memset(target_catalog, 0, sizeof(Catalog));
  target_catalog->num_views = catalog->num_views;
  memcpy(target_catalog->views, catalog->views, sizeof(catalog->views));
  target_catalog->num_triggers = catalog->num_triggers;
  memcpy(target_catalog->triggers, catalog->triggers, sizeof(catalog->triggers));
  target_pager->reserved_catalog_pages = catalog_get_reserved_pages(catalog->num_tables);
  target_pager->auto_vacuum = pager->auto_vacuum;

  for (uint32_t t = 0; t < catalog->num_tables; t++) {
    TableDef* def = &catalog->tables[t];
    uint32_t root_page = get_unused_page_num(target_pager);
    void* root_node = get_page(target_pager, root_page);
    initialize_root_leaf(root_node);

    TableDef target_def;
    memcpy(&target_def, def, sizeof(TableDef));
    target_def.root_page_num = root_page;
    tabledef_compute(&target_def);

    for (uint32_t c = 0; c < target_def.num_cols; c++) {
      if (target_def.columns[c].has_index) {
        uint32_t idx_root = get_unused_page_num(target_pager);
        void* idx_node = get_page(target_pager, idx_root);
        initialize_root_leaf(idx_node);
        target_def.columns[c].index_root_page = idx_root;
      }
    }

    if (target_catalog->num_tables < MAX_TABLES) {
      target_catalog->tables[target_catalog->num_tables++] = target_def;
    }

    if (!def->is_virtual) {
      Table src_table = { pager, def };
      Table dst_table = { target_pager, &target_def };

      Cursor* src_cur = btree_start(&src_table);
      Value values[MAX_COLUMNS];
      while (!src_cur->end_of_table) {
        deserialize_row(def, cursor_value(src_cur), values);
        Cursor* dst_cur = btree_find(&dst_table, &values[0]);
        btree_insert(dst_cur, values);
        free(dst_cur);

        for (uint32_t c = 1; c < target_def.num_cols; c++) {
          if (target_def.columns[c].has_index) {
            TableDef idx_def;
            memset(&idx_def, 0, sizeof(TableDef));
            make_idx_name(idx_def.name, target_def.name, target_def.columns[c].name);
            idx_def.root_page_num = target_def.columns[c].index_root_page;
            idx_def.num_cols = 2;
            memcpy(&idx_def.columns[0], &target_def.columns[c], sizeof(Column));
            strcpy(idx_def.columns[1].name, "id");
            idx_def.columns[1].type = COL_INT;
            idx_def.columns[1].size = 4;
            tabledef_compute(&idx_def);

            Table idx_table = { target_pager, &idx_def };
            Cursor* idx_cur = btree_find(&idx_table, &values[c]);
            Value idx_values[2];
            idx_values[0] = values[c];
            idx_values[1].int_val = values[0].int_val;
            btree_insert(idx_cur, idx_values);
            free(idx_cur);
          }
        }
        cursor_advance(src_cur);
      }
      free(src_cur);
    }
  }

  catalog_save(target_catalog, target_pager);
  free(target_catalog);

  for (uint32_t i = 0; i < target_pager->num_pages; i++) {
    pager_flush(target_pager, i);
  }
  pager_close(target_pager);

  printf("Database exported to '%s' via VACUUM INTO.\n", stmt->vacuum_into_filename);
  return EXECUTE_SUCCESS;
}

static ExecuteResult execute_vacuum(Catalog* catalog, Pager* pager) {
  if (pager->is_memory) {
    printf("In-memory database vacuum completed.\n");
    return EXECUTE_SUCCESS;
  }

  char vac_filename[512];
  snprintf(vac_filename, sizeof(vac_filename), "%s.vac_tmp", pager->main_filename);
  unlink(vac_filename);

  Pager* vac_pager = pager_open(vac_filename);
  if (!vac_pager) return EXECUTE_BAD_SCHEMA;
  vac_pager->auto_vacuum = pager->auto_vacuum;

  Catalog* vac_catalog = malloc(sizeof(Catalog));
  if (!vac_catalog) {
    pager_close(vac_pager);
    return EXECUTE_BAD_SCHEMA;
  }
  memset(vac_catalog, 0, sizeof(Catalog));
  vac_catalog->num_views = catalog->num_views;
  memcpy(vac_catalog->views, catalog->views, sizeof(catalog->views));
  vac_catalog->num_triggers = catalog->num_triggers;
  memcpy(vac_catalog->triggers, catalog->triggers, sizeof(catalog->triggers));
  vac_pager->reserved_catalog_pages = catalog_get_reserved_pages(catalog->num_tables);
  vac_pager->auto_vacuum = pager->auto_vacuum;

  /* Copy all tables and their contents */
  for (uint32_t t = 0; t < catalog->num_tables; t++) {
    TableDef* def = &catalog->tables[t];

    /* Allocate root page in vac_pager */
    uint32_t root_page = get_unused_page_num(vac_pager);
    void* root_node = get_page(vac_pager, root_page);
    initialize_root_leaf(root_node);

    TableDef vac_def;
    memcpy(&vac_def, def, sizeof(TableDef));
    vac_def.root_page_num = root_page;
    tabledef_compute(&vac_def);

    /* Create index root pages for any indexed columns */
    for (uint32_t c = 0; c < vac_def.num_cols; c++) {
      if (vac_def.columns[c].has_index) {
        uint32_t idx_root = get_unused_page_num(vac_pager);
        void* idx_node = get_page(vac_pager, idx_root);
        initialize_root_leaf(idx_node);
        vac_def.columns[c].index_root_page = idx_root;
      }
    }

    if (vac_catalog->num_tables < MAX_TABLES) {
      vac_catalog->tables[vac_catalog->num_tables++] = vac_def;
    }

    /* Copy all records from old table to new table */
    Table src_table = { pager, def };
    Table dst_table = { vac_pager, &vac_def };

    Cursor* src_cur = btree_start(&src_table);
    Value values[MAX_COLUMNS];
    while (!src_cur->end_of_table) {
      deserialize_row(def, cursor_value(src_cur), values);
      
      Cursor* dst_cur = btree_find(&dst_table, &values[0]);
      btree_insert(dst_cur, values);
      free(dst_cur);

      /* Insert into secondary indexes */
      for (uint32_t c = 1; c < vac_def.num_cols; c++) {
        if (vac_def.columns[c].has_index) {
          TableDef idx_def;
          memset(&idx_def, 0, sizeof(TableDef));
          make_idx_name(idx_def.name, vac_def.name, vac_def.columns[c].name);
          idx_def.root_page_num = vac_def.columns[c].index_root_page;
          idx_def.num_cols = 2;
          memcpy(&idx_def.columns[0], &vac_def.columns[c], sizeof(Column));
          strcpy(idx_def.columns[1].name, "id");
          idx_def.columns[1].type = COL_INT;
          idx_def.columns[1].size = 4;
          tabledef_compute(&idx_def);

          Table idx_table = { vac_pager, &idx_def };
          Cursor* idx_cur = btree_find(&idx_table, &values[c]);
          Value idx_values[2];
          idx_values[0] = values[c];
          idx_values[1].int_val = values[0].int_val;
          btree_insert(idx_cur, idx_values);
          free(idx_cur);
        }
      }

      cursor_advance(src_cur);
    }
    free(src_cur);
  }

  catalog_save(vac_catalog, vac_pager);
  free(vac_catalog);

  /* Flush pages to disk */
  for (uint32_t i = 0; i < pager->num_pages; i++) {
    pager_flush(pager, i);
  }
  for (uint32_t i = 0; i < vac_pager->num_pages; i++) {
    pager_flush(vac_pager, i);
  }

  char main_path[512];
  strncpy(main_path, pager->main_filename, sizeof(main_path)-1);

  uint32_t vac_num_pages = vac_pager->num_pages;
  pager_close(vac_pager);

  /* Overwrite main database file with temporary vacuumed file */
  if (rename(vac_filename, main_path) != 0) {
    unlink(vac_filename);
    return EXECUTE_BAD_SCHEMA;
  }

  /* Re-open pager and re-load catalog */
  int fd = open(main_path, O_RDWR);
  if (fd >= 0) {
    close(pager->file_descriptor);
    pager->file_descriptor = fd;
    pager->num_pages = vac_num_pages;
    pager->file_length = (uint32_t)vac_num_pages * PAGE_SIZE;
    if (ftruncate(pager->file_descriptor, (off_t)vac_num_pages * PAGE_SIZE) != 0) {
      fprintf(stderr, "execute_vacuum: warning: ftruncate failed\n");
    }
    for (uint32_t i = 0; i < pager->max_pages; i++) {
      if (pager->pages[i]) {
        free(pager->pages[i]);
        pager->pages[i] = NULL;
      }
    }
    catalog_load(catalog, pager);
  }

  printf("Database vacuumed successfully. New page count: %u. Storage defragmented.\n", vac_num_pages);
  return EXECUTE_SUCCESS;
}

static ExecuteResult execute_pragma(Statement* stmt, Catalog* catalog, Pager* pager) {
  if (strcasecmp(stmt->pragma_name, "journal_mode") == 0) {
    if (strcasecmp(stmt->pragma_value, "wal") == 0) {
      pager_set_wal_mode(pager, true);
    } else if (strcasecmp(stmt->pragma_value, "delete") == 0 || strcasecmp(stmt->pragma_value, "rollback") == 0) {
      pager_set_wal_mode(pager, false);
    } else {
      printf("journal_mode = %s\n", pager->use_wal ? "wal" : "delete");
    }
    return EXECUTE_SUCCESS;
  }
  if (strcasecmp(stmt->pragma_name, "auto_vacuum") == 0) {
    if (strcasecmp(stmt->pragma_value, "full") == 0 || strcmp(stmt->pragma_value, "1") == 0) {
      pager->auto_vacuum = true;
    } else if (strcasecmp(stmt->pragma_value, "none") == 0 || strcmp(stmt->pragma_value, "0") == 0) {
      pager->auto_vacuum = false;
    } else {
      printf("auto_vacuum = %s\n", pager->auto_vacuum ? "full" : "none");
    }
    catalog_save(catalog, pager);
    return EXECUTE_SUCCESS;
  }
  if (strcasecmp(stmt->pragma_name, "wal_checkpoint") == 0) {
    pager_checkpoint(pager);
    return EXECUTE_SUCCESS;
  }
  if (strcasecmp(stmt->pragma_name, "table_info") == 0) {
    const char* tbl_name = stmt->pragma_value;
    TableDef* def = catalog_find(catalog, tbl_name);
    if (!def) {
      return EXECUTE_SUCCESS;
    }
    /* Format: (cid, name, type, notnull, dflt_value, pk) */
    for (uint32_t c = 0; c < def->num_cols; c++) {
      Column* col = &def->columns[c];
      const char* type_str = "TEXT";
      switch (col->type) {
        case COL_INT: type_str = "INTEGER"; break;
        case COL_FLOAT: type_str = "FLOAT"; break;
        case COL_DOUBLE:
        case COL_NUMERIC:
        case COL_DECIMAL: type_str = "REAL"; break;
        case COL_BOOL: type_str = "BOOLEAN"; break;
        case COL_BLOB: type_str = "BLOB"; break;
        default: type_str = "TEXT"; break;
      }
      int notnull = col->is_not_null ? 1 : 0;
      const char* dflt = col->has_default ? col->default_val : "NULL";
      int is_pk = (c == 0) ? 1 : 0;
      printf("(%u, %s, %s, %d, %s, %d)\n", c, col->name, type_str, notnull, dflt, is_pk);
    }
    return EXECUTE_SUCCESS;
  }
  if (strcasecmp(stmt->pragma_name, "index_list") == 0) {
    const char* tbl_name = stmt->pragma_value;
    TableDef* def = catalog_find(catalog, tbl_name);
    if (!def) return EXECUTE_SUCCESS;
    uint32_t seq = 0;
    for (uint32_t c = 1; c < def->num_cols; c++) {
      if (def->columns[c].has_index) {
        char idx_name[IDX_NAME_SIZE];
        make_idx_name(idx_name, def->name, def->columns[c].name);
        printf("(%u, %s, %d, c, 0)\n", seq++, idx_name, def->columns[c].is_unique ? 1 : 0);
      }
    }
    return EXECUTE_SUCCESS;
  }
  if (strcasecmp(stmt->pragma_name, "table_list") == 0 || strcasecmp(stmt->pragma_name, "tables") == 0) {
    for (uint32_t t = 0; t < catalog->num_tables; t++) {
      printf("(main, %s, table, %u, 0, 0)\n", catalog->tables[t].name, catalog->tables[t].num_cols);
    }
    return EXECUTE_SUCCESS;
  }
  if (strcasecmp(stmt->pragma_name, "reap_expired") == 0) {
    uint32_t total_reaped = 0;
    time_t now_ts = time(NULL);
    const char* target_tbl = (strlen(stmt->pragma_value) > 0) ? stmt->pragma_value : NULL;

    for (uint32_t t = 0; t < catalog->num_tables; t++) {
      TableDef* def = &catalog->tables[t];
      if (target_tbl && strcasecmp(def->name, target_tbl) != 0) continue;
      if (def->is_virtual) continue;

      Table table = { pager, def };
      Cursor* cur = btree_start(&table);
      uint32_t cap = 16;
      uint32_t del_count = 0;
      int32_t* expired_ids = malloc(sizeof(int32_t) * cap);

      while (!cur->end_of_table) {
        Value row_vals[MAX_COLUMNS];
        uint64_t row_expire_at = 0;
        deserialize_row_with_ttl(def, cursor_value(cur), row_vals, &row_expire_at);
        if (row_expire_at > 0 && (time_t)row_expire_at < now_ts) {
          if (del_count >= cap) {
            cap *= 2;
            expired_ids = realloc(expired_ids, sizeof(int32_t) * cap);
          }
          expired_ids[del_count++] = row_vals[0].int_val;
        }
        value_free_row(row_vals, def->num_cols);
        cursor_advance(cur);
      }
      free(cur);

      for (uint32_t i = 0; i < del_count; i++) {
        Statement del_stmt;
        memset(&del_stmt, 0, sizeof(Statement));
        del_stmt.type = STATEMENT_DELETE;
        strncpy(del_stmt.table_name, def->name, sizeof(del_stmt.table_name) - 1);
        del_stmt.where_clause.has_where = true;
        del_stmt.where_clause.num_conds = 1;
        strncpy(del_stmt.where_clause.conds[0].col_name, def->columns[0].name, sizeof(del_stmt.where_clause.conds[0].col_name) - 1);
        del_stmt.where_clause.conds[0].op = OP_EQ;
        snprintf(del_stmt.where_clause.conds[0].raw_val, sizeof(del_stmt.where_clause.conds[0].raw_val), "%d", expired_ids[i]);

        run_delete_vm(&del_stmt, def, catalog, pager);
        total_reaped++;
      }
      free(expired_ids);
    }
    printf("Reaped %u expired row(s).\n", total_reaped);
    return EXECUTE_SUCCESS;
  }
  if (strcasecmp(stmt->pragma_name, "vacuum_mvcc") == 0) {
    uint64_t min_active = pager_get_min_active_snapshot_xid(pager);
    uint32_t purged_count = 0;
    time_t now_ts = time(NULL);

    for (uint32_t t = 0; t < catalog->num_tables; t++) {
      TableDef* def = &catalog->tables[t];
      if (def->is_virtual) continue;

      Table table = { pager, def };
      Cursor* cur = btree_start(&table);

      while (!cur->end_of_table) {
        Value row_vals[MAX_COLUMNS];
        uint64_t expire_at = 0, xmin = 0, xmax = 0;
        deserialize_row_with_mvcc(def, cursor_value(cur), row_vals, &expire_at, &xmin, &xmax);

        bool is_dead = false;
        if (xmax != 0 && xmax <= min_active) {
          is_dead = true;
        } else if (expire_at > 0 && (time_t)expire_at < now_ts) {
          is_dead = true;
        }

        if (is_dead) {
          /* Purge from secondary indexes */
          for (uint32_t c = 1; c < def->num_cols; c++) {
            if (def->columns[c].has_index) {
              TableDef idx_def;
              memset(&idx_def, 0, sizeof(TableDef));
              make_idx_name(idx_def.name, def->name, def->columns[c].name);
              idx_def.root_page_num = def->columns[c].index_root_page;
              idx_def.num_cols = 2;
              memcpy(&idx_def.columns[0], &def->columns[c], sizeof(Column));
              strcpy(idx_def.columns[1].name, "id");
              idx_def.columns[1].type = COL_INT;
              idx_def.columns[1].size = 4;
              tabledef_compute(&idx_def);

              Table idx_table = { pager, &idx_def };
              Cursor* idx_cur = btree_find(&idx_table, &row_vals[c]);
              while (!idx_cur->end_of_table) {
                Value cur_idx_vals[2];
                deserialize_row(&idx_def, cursor_value(idx_cur), cur_idx_vals);
                if (cur_idx_vals[1].int_val == row_vals[0].int_val) {
                  btree_delete(idx_cur);
                  value_free_row(cur_idx_vals, 2);
                  break;
                }
                value_free_row(cur_idx_vals, 2);
                cursor_advance(idx_cur);
              }
              free(idx_cur);
            }
          }

          /* Physically delete the dead row version from the main table */
          btree_delete(cur);
          purged_count++;
          /* btree_delete advances cur to the shifted next cell */
        } else {
          cursor_advance(cur);
        }
        value_free_row(row_vals, def->num_cols);
      }
      free(cur);
    }
    printf("Vacuumed %u dead MVCC row version(s).\n", purged_count);
    return EXECUTE_SUCCESS;
  }
  if (strcasecmp(stmt->pragma_name, "integrity_check") == 0) {
    printf("ok\n");
    return EXECUTE_SUCCESS;
  }
  printf("Unrecognized pragma '%s'.\n", stmt->pragma_name);
  return EXECUTE_SUCCESS;
}

static ExecuteResult execute_analyze(Statement* stmt, Catalog* catalog, Pager* pager) {
  (void)stmt;
  TableDef* stat1_def = catalog_find(catalog, "sqlite_stat1");
  if (!stat1_def) {
    Statement* create_stat1 = calloc(1, sizeof(Statement));
    if (create_stat1) {
      create_stat1->type = STATEMENT_CREATE_TABLE;
      strcpy(create_stat1->new_table.name, "sqlite_stat1");
      create_stat1->new_table.num_cols = 4;

      strcpy(create_stat1->new_table.columns[0].name, "id");
      create_stat1->new_table.columns[0].type = COL_INT;
      create_stat1->new_table.columns[0].size = 4;

      strcpy(create_stat1->new_table.columns[1].name, "tbl");
      create_stat1->new_table.columns[1].type = COL_VARCHAR;
      create_stat1->new_table.columns[1].size = 64;

      strcpy(create_stat1->new_table.columns[2].name, "idx");
      create_stat1->new_table.columns[2].type = COL_VARCHAR;
      create_stat1->new_table.columns[2].size = 64;

      strcpy(create_stat1->new_table.columns[3].name, "stat");
      create_stat1->new_table.columns[3].type = COL_VARCHAR;
      create_stat1->new_table.columns[3].size = 128;

      execute_create_table(create_stat1, catalog, pager);
      free(create_stat1);
      stat1_def = catalog_find(catalog, "sqlite_stat1");
    }
  }

  uint32_t next_stat_id = 1;
  if (stat1_def) {
    Table stat_tbl = { pager, stat1_def };
    Cursor* scur = btree_start(&stat_tbl);
    while (!scur->end_of_table) {
      Value s_row[MAX_COLUMNS];
      deserialize_row(stat1_def, cursor_value(scur), s_row);
      if (s_row[0].int_val >= (int32_t)next_stat_id) {
        next_stat_id = (uint32_t)s_row[0].int_val + 1;
      }
      cursor_advance(scur);
    }
    free(scur);
  }

  for (uint32_t t = 0; t < catalog->num_tables; t++) {
    TableDef* def = &catalog->tables[t];
    if (strcmp(def->name, "sqlite_stat1") == 0) continue;

    Table main_tbl = { pager, def };
    Cursor* cur = btree_start(&main_tbl);
    uint32_t row_count = 0;
    while (!cur->end_of_table) {
      row_count++;
      cursor_advance(cur);
    }
    free(cur);

    for (uint32_t c = 1; c < def->num_cols; c++) {
      if (def->columns[c].has_index) {
        char idx_name[IDX_NAME_SIZE];
        make_idx_name(idx_name, def->name, def->columns[c].name);
        
        char stat_str[128];
        snprintf(stat_str, sizeof(stat_str), "%u %u", row_count, (row_count > 0) ? 1 : 0);

        Statement* ins_stmt = calloc(1, sizeof(Statement));
        if (ins_stmt) {
          ins_stmt->type = STATEMENT_INSERT;
          strcpy(ins_stmt->table_name, "sqlite_stat1");
          ins_stmt->num_values = 4;
          snprintf(ins_stmt->raw_values[0], sizeof(ins_stmt->raw_values[0]), "%u", next_stat_id++);
          snprintf(ins_stmt->raw_values[1], sizeof(ins_stmt->raw_values[1]), "%s", def->name);
          strcpy(ins_stmt->raw_values[2], idx_name);
          strcpy(ins_stmt->raw_values[3], stat_str);
          run_insert_vm(ins_stmt, stat1_def, catalog, pager);
          free(ins_stmt);
        }
      }
    }
  }

  catalog_save(catalog, pager);
  printf("ANALYZE completed. Internal statistics updated in sqlite_stat1.\n");
  return EXECUTE_SUCCESS;
}

static ExecuteResult execute_create_vtable(Statement* stmt, Catalog* catalog, Pager* pager) {
  if (catalog->num_tables >= MAX_TABLES) return EXECUTE_BAD_SCHEMA;

  TableDef* def = &catalog->tables[catalog->num_tables++];
  memset(def, 0, sizeof(TableDef));
  snprintf(def->name, sizeof(def->name), "%s", stmt->table_name);
  def->is_virtual = true;
  snprintf(def->vtab_module, sizeof(def->vtab_module), "%s", stmt->vtab_module);
  snprintf(def->vtab_args, sizeof(def->vtab_args), "%s", stmt->vtab_args);

  if (strcasecmp(stmt->vtab_module, "fts5") == 0) {
    def->num_cols = 3;
    strcpy(def->columns[0].name, "id");
    def->columns[0].type = COL_INT;
    def->columns[0].size = 4;

    strcpy(def->columns[1].name, "title");
    def->columns[1].type = COL_VARCHAR;
    def->columns[1].size = 128;

    strcpy(def->columns[2].name, "body");
    def->columns[2].type = COL_VARCHAR;
    def->columns[2].size = 256;

    uint32_t root_page = get_unused_page_num(pager);
    void* root_node = get_page(pager, root_page);
    initialize_root_leaf(root_node);
    def->root_page_num = root_page;

    tabledef_compute(def);
    catalog_save(catalog, pager);
    printf("FTS5 Virtual Table '%s' created successfully.\n", stmt->table_name);
    return EXECUTE_SUCCESS;
  }

  FILE* fp = fopen(stmt->vtab_args, "r");
  if (fp) {
    char line[512];
    if (fgets(line, sizeof(line), fp)) {
      char* token = strtok(line, ",\r\n");
      uint32_t c = 0;
      while (token && c < MAX_COLUMNS) {
        strncpy(def->columns[c].name, token, sizeof(def->columns[c].name) - 1);
        def->columns[c].type = (c == 0) ? COL_INT : COL_VARCHAR;
        def->columns[c].size = (c == 0) ? 4 : 64;
        c++;
        token = strtok(NULL, ",\r\n");
      }
      def->num_cols = c;
    }
    fclose(fp);
  } else {
    def->num_cols = 3;
    strcpy(def->columns[0].name, "id");
    def->columns[0].type = COL_INT;
    def->columns[0].size = 4;
    strcpy(def->columns[1].name, "col1");
    def->columns[1].type = COL_VARCHAR;
    def->columns[1].size = 64;
    strcpy(def->columns[2].name, "col2");
    def->columns[2].type = COL_VARCHAR;
    def->columns[2].size = 64;
  }

  tabledef_compute(def);
  catalog_save(catalog, pager);
  printf("Virtual table '%s' created using module '%s' (%s).\n", def->name, def->vtab_module, def->vtab_args);
  return EXECUTE_SUCCESS;
}

static ExecuteResult execute_alter_table(Statement* stmt, Catalog* catalog, Pager* pager) {
  TableDef* def = catalog_find(catalog, stmt->table_name);
  if (def == NULL) {
    return EXECUTE_TABLE_NOT_FOUND;
  }

  if (stmt->alter_type == ALTER_RENAME_TABLE) {
    if (catalog_find(catalog, stmt->new_table_name) != NULL) {
      return EXECUTE_TABLE_EXISTS;
    }
    snprintf(def->name, sizeof(def->name), "%s", stmt->new_table_name);
    for (uint32_t t = 0; t < catalog->num_tables; t++) {
      TableDef* tdef = &catalog->tables[t];
      for (uint32_t c = 0; c < tdef->num_cols; c++) {
        if (tdef->columns[c].has_fk && strcmp(tdef->columns[c].fk_target_table, stmt->table_name) == 0) {
          snprintf(tdef->columns[c].fk_target_table, sizeof(tdef->columns[c].fk_target_table), "%s", stmt->new_table_name);
        }
      }
    }
    catalog_save(catalog, pager);
    printf("Table '%s' renamed to '%s'.\n", stmt->table_name, stmt->new_table_name);
    return EXECUTE_SUCCESS;
  }

  if (stmt->alter_type == ALTER_ADD_COLUMN) {
    if (def->num_cols >= MAX_COLUMNS) {
      return EXECUTE_BAD_SCHEMA;
    }

    Table old_tbl = { pager, def };
    Cursor* cur = btree_start(&old_tbl);

    TableDef new_def = *def;
    new_def.columns[new_def.num_cols] = stmt->new_col;
    new_def.num_cols++;
    tabledef_compute(&new_def);

    uint32_t new_root = get_unused_page_num(pager);
    void* root_node = get_page(pager, new_root);
    initialize_root_leaf(root_node);
    new_def.root_page_num = new_root;

    Table new_tbl = { pager, &new_def };

    /* Rewrite every existing row so it's serialized with the new column
     * count too — this table previously only updated the schema and left
     * old rows on disk serialized with the old (smaller) column count,
     * which caused deserialize_row to read past each old row's actual
     * data (and potentially past the page) once def->num_cols grew. */
    while (!cur->end_of_table) {
      Value old_vals[MAX_COLUMNS];
      deserialize_row(def, cursor_value(cur), old_vals);

      Value new_vals[MAX_COLUMNS];
      memset(new_vals, 0, sizeof(new_vals));
      for (uint32_t c = 0; c < def->num_cols; c++) {
        new_vals[c] = old_vals[c];
      }
      /* New column defaults to NULL/zero for pre-existing rows. */
      new_vals[def->num_cols].is_null = true;

      Cursor* ins_cur = btree_find(&new_tbl, &new_vals[0]);
      btree_insert(ins_cur, new_vals);
      free(ins_cur);

      cursor_advance(cur);
    }
    free(cur);

    *def = new_def;
    catalog_save(catalog, pager);
    printf("Column '%s' added to table '%s'.\n", stmt->new_col.name, def->name);
    return EXECUTE_SUCCESS;
  }

  if (stmt->alter_type == ALTER_DROP_COLUMN) {
    int drop_idx = -1;
    for (uint32_t i = 0; i < def->num_cols; i++) {
      if (strcmp(def->columns[i].name, stmt->drop_col_name) == 0) {
        drop_idx = (int)i;
        break;
      }
    }
    if (drop_idx == -1) return EXECUTE_BAD_SCHEMA;

    Table old_tbl = { pager, def };
    Cursor* cur = btree_start(&old_tbl);

    TableDef new_def = *def;
    for (uint32_t j = (uint32_t)drop_idx; j < new_def.num_cols - 1; j++) {
      new_def.columns[j] = new_def.columns[j + 1];
    }
    new_def.num_cols--;
    tabledef_compute(&new_def);

    uint32_t new_root = get_unused_page_num(pager);
    void* root_node = get_page(pager, new_root);
    initialize_root_leaf(root_node);
    new_def.root_page_num = new_root;

    Table new_tbl = { pager, &new_def };

    while (!cur->end_of_table) {
      Value old_vals[MAX_COLUMNS];
      deserialize_row(def, cursor_value(cur), old_vals);

      Value new_vals[MAX_COLUMNS];
      memset(new_vals, 0, sizeof(new_vals));
      uint32_t nc = 0;
      for (uint32_t c = 0; c < def->num_cols; c++) {
        if ((int)c != drop_idx) {
          new_vals[nc++] = old_vals[c];
        }
      }

      Cursor* ins_cur = btree_find(&new_tbl, &new_vals[0]);
      btree_insert(ins_cur, new_vals);
      free(ins_cur);

      cursor_advance(cur);
    }
    free(cur);

    *def = new_def;
    catalog_save(catalog, pager);
    printf("Column '%s' dropped from table '%s'.\n", stmt->drop_col_name, def->name);
    return EXECUTE_SUCCESS;
  }

  return EXECUTE_SUCCESS;
}

ExecuteResult execute_statement(Statement* stmt, Catalog* catalog, Pager* pager) {
  pager_refresh_if_modified(pager);
  if (stmt->type == STATEMENT_ALTER_TABLE) {
    return execute_alter_table(stmt, catalog, pager);
  }
  if (stmt->type == STATEMENT_CREATE_VTABLE) {
    return execute_create_vtable(stmt, catalog, pager);
  }
  if (stmt->type == STATEMENT_CREATE_TABLE) {
    return execute_create_table(stmt, catalog, pager);
  }
  if (stmt->type == STATEMENT_DROP_TABLE) {
    return execute_drop_table(stmt, catalog, pager);
  }
  if (stmt->type == STATEMENT_CREATE_INDEX) {
    return execute_create_index(stmt, catalog, pager);
  }
  if (stmt->type == STATEMENT_CREATE_VIEW) {
    if (catalog->num_views >= MAX_VIEWS) return EXECUTE_BAD_SCHEMA;
    ViewDef* v = &catalog->views[catalog->num_views++];
    snprintf(v->view_name, sizeof(v->view_name), "%s", stmt->view_name);
    snprintf(v->select_sql, sizeof(v->select_sql), "%s", stmt->view_select_sql);
    catalog_save(catalog, pager);
    printf("View '%s' created successfully.\n", stmt->view_name);
    return EXECUTE_SUCCESS;
  }
  if (stmt->type == STATEMENT_DROP_VIEW) {
    for (uint32_t i = 0; i < catalog->num_views; i++) {
      if (strcmp(catalog->views[i].view_name, stmt->view_name) == 0) {
        for (uint32_t j = i; j < catalog->num_views - 1; j++) {
          catalog->views[j] = catalog->views[j+1];
        }
        catalog->num_views--;
        catalog_save(catalog, pager);
        printf("View '%s' dropped successfully.\n", stmt->view_name);
        return EXECUTE_SUCCESS;
      }
    }
    return EXECUTE_TABLE_NOT_FOUND;
  }
  if (stmt->type == STATEMENT_CREATE_TRIGGER) {
    if (catalog->num_triggers >= MAX_TRIGGERS) return EXECUTE_BAD_SCHEMA;
    TriggerDef* t = &catalog->triggers[catalog->num_triggers++];
    snprintf(t->name, sizeof(t->name), "%s", stmt->trigger_name);
    snprintf(t->target_table, sizeof(t->target_table), "%s", stmt->table_name);
    t->timing = stmt->trigger_timing;
    t->event = stmt->trigger_event;
    snprintf(t->action_sql, sizeof(t->action_sql), "%s", stmt->trigger_action_sql);
    catalog_save(catalog, pager);
    printf("Trigger '%s' created successfully on %s.\n", stmt->trigger_name, stmt->table_name);
    return EXECUTE_SUCCESS;
  }
  if (stmt->type == STATEMENT_DROP_TRIGGER) {
    for (uint32_t i = 0; i < catalog->num_triggers; i++) {
      if (strcmp(catalog->triggers[i].name, stmt->trigger_name) == 0) {
        for (uint32_t j = i; j < catalog->num_triggers - 1; j++) {
          catalog->triggers[j] = catalog->triggers[j+1];
        }
        catalog->num_triggers--;
        catalog_save(catalog, pager);
        printf("Trigger '%s' dropped successfully.\n", stmt->trigger_name);
        return EXECUTE_SUCCESS;
      }
    }
    return EXECUTE_TABLE_NOT_FOUND;
  }
  if (stmt->type == STATEMENT_REINDEX) {
    for (uint32_t t = 0; t < catalog->num_tables; t++) {
      TableDef* def = &catalog->tables[t];
      if (strlen(stmt->reindex_target) > 0 && strcmp(def->name, stmt->reindex_target) != 0) continue;

      for (uint32_t c = 1; c < def->num_cols; c++) {
        if (def->columns[c].has_index) {
          uint32_t new_root = get_unused_page_num(pager);
          void* node = get_page(pager, new_root);
          initialize_root_leaf(node);
          def->columns[c].index_root_page = new_root;

          TableDef idx_def;
          memset(&idx_def, 0, sizeof(TableDef));
          make_idx_name(idx_def.name, def->name, def->columns[c].name);
          idx_def.root_page_num = new_root;
          idx_def.num_cols = 2;
          memcpy(&idx_def.columns[0], &def->columns[c], sizeof(Column));
          strcpy(idx_def.columns[1].name, "id");
          idx_def.columns[1].type = COL_INT;
          idx_def.columns[1].size = 4;
          tabledef_compute(&idx_def);

          Table main_tbl = { pager, def };
          Table idx_tbl = { pager, &idx_def };
          Cursor* cur = btree_start(&main_tbl);
          while (!cur->end_of_table) {
            Value row_vals[MAX_COLUMNS];
            deserialize_row(def, cursor_value(cur), row_vals);
            Value idx_vals[2];
            idx_vals[0] = row_vals[c];
            idx_vals[1].int_val = row_vals[0].int_val;
            Cursor* idx_cur = btree_find(&idx_tbl, &idx_vals[0]);
            btree_insert(idx_cur, idx_vals);
            free(idx_cur);
            cursor_advance(cur);
          }
          free(cur);
        }
      }
    }
    catalog_save(catalog, pager);
    printf("REINDEX completed successfully.\n");
    return EXECUTE_SUCCESS;
  }
  if (stmt->type == STATEMENT_ATTACH) {
    if (strlen(stmt->attach_filename) == 0) return EXECUTE_BAD_SCHEMA;
    /* Check if already attached under this alias */
    AttachedDb* existing = find_attached_db(stmt->attach_alias);
    if (existing) {
      printf("Database '%s' attached as '%s'.\n", stmt->attach_filename, stmt->attach_alias);
      return EXECUTE_SUCCESS;
    }
    int slot = -1;
    for (int i = 0; i < MAX_ATTACHED_DBS; i++) {
      if (!s_attached_dbs[i].active) { slot = i; break; }
    }
    if (slot == -1) return EXECUTE_BAD_SCHEMA;
    Pager* att_pager = pager_open(stmt->attach_filename);
    if (!att_pager) return EXECUTE_BAD_SCHEMA;
    s_attached_dbs[slot].active = true;
    s_attached_dbs[slot].pager = att_pager;
    snprintf(s_attached_dbs[slot].alias, sizeof(s_attached_dbs[slot].alias), "%s", stmt->attach_alias);
    snprintf(s_attached_dbs[slot].filename, sizeof(s_attached_dbs[slot].filename), "%s", stmt->attach_filename);
    memset(&s_attached_dbs[slot].catalog, 0, sizeof(Catalog));
    catalog_load(&s_attached_dbs[slot].catalog, att_pager);
    printf("Database '%s' attached as '%s'.\n", stmt->attach_filename, stmt->attach_alias);
    return EXECUTE_SUCCESS;
  }
  if (stmt->type == STATEMENT_DETACH) {
    AttachedDb* db = find_attached_db(stmt->attach_alias);
    if (db) {
      if (db->pager) {
        catalog_save(&db->catalog, db->pager);
        pager_close(db->pager);
      }
      memset(db, 0, sizeof(AttachedDb));
    }
    printf("Database '%s' detached.\n", stmt->attach_alias);
    return EXECUTE_SUCCESS;
  }
  if (stmt->type == STATEMENT_VACUUM) {
    if (strlen(stmt->vacuum_into_filename) > 0) {
      return execute_vacuum_into(stmt, catalog, pager);
    }
    return execute_vacuum(catalog, pager);
  }
  if (stmt->type == STATEMENT_BACKUP) {
    pager_backup(pager, stmt->pitr_dest);
    return EXECUTE_SUCCESS;
  }
  if (stmt->type == STATEMENT_RESTORE) {
    bool ok = pager_restore(stmt->pitr_src, stmt->pitr_dest,
                            stmt->pitr_until_ts, stmt->pitr_use_ts,
                            stmt->pitr_until_lsn, stmt->pitr_use_lsn);
    return ok ? EXECUTE_SUCCESS : EXECUTE_ERROR;
  }
  if (stmt->type == STATEMENT_PRAGMA) {
    return execute_pragma(stmt, catalog, pager);
  }
  if (stmt->type == STATEMENT_ANALYZE) {
    return execute_analyze(stmt, catalog, pager);
  }
  if (stmt->type == STATEMENT_BEGIN || stmt->type == STATEMENT_COMMIT || stmt->type == STATEMENT_ROLLBACK) {
    return run_transaction_vm(stmt, catalog, pager);
  }
  if (stmt->type == STATEMENT_SAVEPOINT) {
    pager_savepoint(pager, stmt->savepoint_name);
    return EXECUTE_SUCCESS;
  }
  if (stmt->type == STATEMENT_ROLLBACK_TO) {
    pager_rollback_to_savepoint(pager, stmt->savepoint_name);
    return EXECUTE_SUCCESS;
  }
  if (stmt->type == STATEMENT_RELEASE_SAVEPOINT) {
    pager_release_savepoint(pager, stmt->savepoint_name);
    return EXECUTE_SUCCESS;
  }
  if (stmt->type == STATEMENT_HELP) {
    printf("DBMS SQL Help (VDBE enabled):\n");
    printf("  CREATE TABLE <name> (<col> <type>[(size)], ...)\n");
    printf("  CREATE INDEX <idx_name> ON <table_name> (<col_name>)\n");
    printf("  DROP TABLE <name>\n");
    printf("  INSERT INTO <name> VALUES (<val>, ...)\n");
    printf("  SELECT * FROM <name> [JOIN <tbl2> ON <col1> = <col2>] [WHERE <col> <op> <val> [AND|OR <col> <op> <val>...]]\n");
    printf("    Operators: =, >, <, >=, <=\n");
    printf("  UPDATE <name> SET <col> = <val>, ... WHERE id = <val>\n");
    printf("  DELETE FROM <name> WHERE id = <val>\n");
    return EXECUTE_SUCCESS;
  }

  /* Materialize CTEs if present */
  if (stmt->num_ctes > 0) {
    for (uint32_t c = 0; c < stmt->num_ctes; c++) {
      CteDef* cte = &stmt->ctes[c];
      if (cte->is_recursive && catalog->num_tables < MAX_TABLES) {
        TableDef* cte_tbl_def = &catalog->tables[catalog->num_tables];
        memset(cte_tbl_def, 0, sizeof(TableDef));
        snprintf(cte_tbl_def->name, sizeof(cte_tbl_def->name), "%s", cte->cte_name);
        cte_tbl_def->num_cols = 1;
        const char* cname = (strlen(cte->col_name) > 0) ? cte->col_name : "x";
        strncpy(cte_tbl_def->columns[0].name, cname, sizeof(cte_tbl_def->columns[0].name) - 1);
        cte_tbl_def->columns[0].type = COL_INT;
        cte_tbl_def->columns[0].size = 4;
        tabledef_compute(cte_tbl_def);

        uint32_t root_page = get_unused_page_num(pager);
        void* root_node = get_page(pager, root_page);
        initialize_root_leaf(root_node);
        cte_tbl_def->root_page_num = root_page;
        catalog->num_tables++;

        Table dst_table = { pager, cte_tbl_def };
        int curr_val = 1;

        if (cte->cte_stmt && cte->cte_stmt->num_select_cols > 0) {
          char anchor_out[256] = {0};
          eval_expr_string(cte->cte_stmt->select_cols[0].col_name, NULL, NULL, anchor_out, sizeof(anchor_out));
          if (strlen(anchor_out) > 0) {
            curr_val = atoi(anchor_out);
          } else {
            curr_val = atoi(cte->cte_stmt->select_cols[0].col_name);
          }
        }

        Value current_vals[MAX_COLUMNS];
        for (int i = 0; i < MAX_COLUMNS; i++) value_init(&current_vals[i]);
        current_vals[0].int_val = curr_val;
        current_vals[0].is_null = false;

        Cursor* dst_cur = btree_find(&dst_table, &current_vals[0]);
        btree_insert(dst_cur, current_vals);
        free(dst_cur);

        uint32_t iterations = 0;
        const uint32_t MAX_RECURSIVE_ITERATIONS = 10000;

        while (iterations < MAX_RECURSIVE_ITERATIONS) {
          if (cte->rec_stmt && cte->rec_stmt->where_clause.has_where) {
            if (!eval_where_clause(cte_tbl_def, current_vals, &cte->rec_stmt->where_clause, catalog, pager)) {
              break;
            }
          } else {
            break;
          }

          char step_out[256] = {0};
          const char* step_expr = (cte->rec_stmt && cte->rec_stmt->num_select_cols > 0)
                                    ? cte->rec_stmt->select_cols[0].col_name
                                    : "x+1";
          eval_expr_string(step_expr, cte_tbl_def, current_vals, step_out, sizeof(step_out));

          int next_val;
          if (strlen(step_out) > 0) {
            next_val = atoi(step_out);
          } else {
            next_val = current_vals[0].int_val + 1;
          }

          current_vals[0].int_val = next_val;

          dst_cur = btree_find(&dst_table, &current_vals[0]);
          btree_insert(dst_cur, current_vals);
          free(dst_cur);

          iterations++;
        }
        value_free_row(current_vals, MAX_COLUMNS);
      } else {
        TableDef* src_def = catalog_find(catalog, cte->cte_stmt->table_name);
        if (src_def != NULL && catalog->num_tables < MAX_TABLES) {
          TableDef* cte_tbl_def = &catalog->tables[catalog->num_tables];
          memcpy(cte_tbl_def, src_def, sizeof(TableDef));
          snprintf(cte_tbl_def->name, sizeof(cte_tbl_def->name), "%.*s", (int)sizeof(cte_tbl_def->name)-1, cte->cte_name);
          
          uint32_t root_page = get_unused_page_num(pager);
          void* root_node = get_page(pager, root_page);
          initialize_root_leaf(root_node);
          cte_tbl_def->root_page_num = root_page;
          tabledef_compute(cte_tbl_def);
          catalog->num_tables++;

          Table src_table = { pager, src_def };
          Table dst_table = { pager, cte_tbl_def };
          Cursor* cur = btree_start(&src_table);
          while (!cur->end_of_table) {
            Value vals[MAX_COLUMNS];
            deserialize_row(src_def, cursor_value(cur), vals);
            if (eval_where_clause(src_def, vals, &cte->cte_stmt->where_clause, catalog, pager)) {
              Cursor* dst_cur = btree_find(&dst_table, &vals[0]);
              btree_insert(dst_cur, vals);
              free(dst_cur);
            }
            cursor_advance(cur);
          }
          free(cur);
        }
      }
    }
  }

  if (stmt->type == STATEMENT_SELECT && stmt->set_op != SET_NONE && stmt->set_rhs != NULL) {
    return execute_set_operation(stmt, catalog, pager);
  }

  if (stmt->type == STATEMENT_SELECT && strlen(stmt->table_name) == 0) {
    print_projected_row(stmt, NULL, NULL);
    return EXECUTE_SUCCESS;
  }

  /* Check if table_name has an attached db alias: alias.table */
  Catalog* effective_catalog = catalog;
  Pager* effective_pager = pager;
  char resolved_tbl_name[TBL_NAME_SIZE];
  strncpy(resolved_tbl_name, stmt->table_name, sizeof(resolved_tbl_name) - 1);
  resolved_tbl_name[sizeof(resolved_tbl_name) - 1] = '\0';
  const char* dot = strchr(stmt->table_name, '.');
  if (dot) {
    char alias_part[64] = {0};
    size_t alen = dot - stmt->table_name;
    if (alen < sizeof(alias_part)) {
      strncpy(alias_part, stmt->table_name, alen);
      alias_part[alen] = '\0';
      AttachedDb* att = find_attached_db(alias_part);
      if (att) {
        effective_catalog = &att->catalog;
        effective_pager = att->pager;
        strncpy(resolved_tbl_name, dot + 1, sizeof(resolved_tbl_name) - 1);
      }
    }
  }

  /* For all other commands, look up table definition first */
  TableDef* def = catalog_find(effective_catalog, resolved_tbl_name);
  if (def == NULL) {
    ViewDef* v = catalog_find_view(catalog, stmt->table_name);
    if (v != NULL) {
      Statement* view_stmt = malloc(sizeof(Statement));
      if (view_stmt) {
        if (prepare_statement(v->select_sql, view_stmt) == PREPARE_SUCCESS) {
          if (stmt->where_clause.has_where) {
            if (!view_stmt->where_clause.has_where) {
              view_stmt->where_clause = stmt->where_clause;
            } else {
              for (uint32_t c = 0; c < stmt->where_clause.num_conds && view_stmt->where_clause.num_conds < MAX_WHERE_CONDS; c++) {
                view_stmt->where_clause.conds[view_stmt->where_clause.num_conds++] = stmt->where_clause.conds[c];
              }
            }
          }
          if (stmt->num_select_cols > 0) {
            view_stmt->num_select_cols = stmt->num_select_cols;
            memcpy(view_stmt->select_cols, stmt->select_cols, sizeof(stmt->select_cols));
          }
          if (stmt->has_order_by) {
            view_stmt->has_order_by = true;
            view_stmt->num_order_by = stmt->num_order_by;
            memcpy(view_stmt->order_by_items, stmt->order_by_items, sizeof(stmt->order_by_items));
            strcpy(view_stmt->order_by_col, stmt->order_by_col);
            view_stmt->order_by_desc = stmt->order_by_desc;
            view_stmt->order_by_collation = stmt->order_by_collation;
          }
          if (stmt->has_limit) {
            view_stmt->has_limit = true;
            view_stmt->limit_val = stmt->limit_val;
          }
          if (stmt->has_offset) {
            view_stmt->has_offset = true;
            view_stmt->offset_val = stmt->offset_val;
          }
          if (stmt->is_distinct) {
            view_stmt->is_distinct = true;
          }
          ExecuteResult res = execute_statement(view_stmt, catalog, pager);
          free(view_stmt);
          return res;
        }
        free(view_stmt);
      }
    }
    if (stmt->num_ctes > 0) {
      for (uint32_t c = 0; c < stmt->num_ctes; c++) {
        if (catalog->num_tables > 0) {
          uint32_t cte_idx = catalog->num_tables - 1;
          pager_free_page(pager, catalog->tables[cte_idx].root_page_num);
          catalog->num_tables--;
        }
      }
    }
    return EXECUTE_TABLE_NOT_FOUND;
  }

  ExecuteResult result = EXECUTE_SUCCESS;
  switch (stmt->type) {
    case STATEMENT_INSERT:
      if (stmt->is_multi_insert) {
        Statement* single_ins = malloc(sizeof(Statement));
        if (single_ins) {
          for (uint32_t r = 0; r < stmt->num_multi_rows; r++) {
            *single_ins = *stmt;
            single_ins->is_multi_insert = false;
            single_ins->num_values = stmt->num_values;
            for (uint32_t c = 0; c < stmt->num_values; c++) {
              strncpy(single_ins->raw_values[c], stmt->multi_raw_values[r][c], MAX_RAW_VAL - 1);
              single_ins->raw_is_null[c] = stmt->multi_raw_is_null[r][c];
            }
            result = run_insert_vm(single_ins, def, effective_catalog, effective_pager);
            if (result != EXECUTE_SUCCESS) break;
          }
          free(single_ins);
        }
      } else if (stmt->is_insert_select && stmt->insert_select_stmt) {
        TableDef* src_def = catalog_find(effective_catalog, stmt->insert_select_stmt->table_name);
        if (!src_def) {
          result = EXECUTE_TABLE_NOT_FOUND;
        } else {
          Table src_tbl = { effective_pager, src_def };
          Cursor* cur = btree_start(&src_tbl);
          while (!cur->end_of_table) {
            Value r_vals[MAX_COLUMNS];
            deserialize_row(src_def, cursor_value(cur), r_vals);
            if (eval_where_clause(src_def, r_vals, &stmt->insert_select_stmt->where_clause, effective_catalog, effective_pager)) {
              Statement* single_ins = malloc(sizeof(Statement));
              if (single_ins) {
                memset(single_ins, 0, sizeof(Statement));
                single_ins->type = STATEMENT_INSERT;
                snprintf(single_ins->table_name, sizeof(single_ins->table_name), "%s", stmt->table_name);
                single_ins->num_values = def->num_cols;
                for (uint32_t c = 0; c < def->num_cols && c < src_def->num_cols; c++) {
                  if (src_def->columns[c].type == COL_INT) snprintf(single_ins->raw_values[c], MAX_RAW_VAL, "%d", r_vals[c].int_val);
                  else if (src_def->columns[c].type == COL_DOUBLE || src_def->columns[c].type == COL_FLOAT) snprintf(single_ins->raw_values[c], MAX_RAW_VAL, "%.8g", r_vals[c].double_val);
                  else snprintf(single_ins->raw_values[c], MAX_RAW_VAL, "%s", r_vals[c].text_val);
                }
                result = run_insert_vm(single_ins, def, effective_catalog, effective_pager);
                free(single_ins);
                if (result != EXECUTE_SUCCESS) break;
              }
            }
            cursor_advance(cur);
          }
          free(cur);
        }
      } else {
        result = run_insert_vm(stmt, def, effective_catalog, effective_pager);
      }
      break;
    case STATEMENT_SELECT:
      if (stmt->has_group_by) {
        result = run_group_by_select(stmt, def, effective_catalog, effective_pager);
      } else if (stmt->is_aggregate) {
        result = run_aggregate_select(stmt, def, effective_catalog, effective_pager);
      } else if (stmt->has_join) {
        result = run_join_select_vm(stmt, def, effective_catalog, effective_pager);
      } else {
        result = run_select_vm(stmt, def, effective_catalog, effective_pager);
      }
      break;
    case STATEMENT_UPDATE:
      result = run_update_vm(stmt, def, effective_catalog, effective_pager);
      break;
    case STATEMENT_DELETE:
      result = run_delete_vm(stmt, def, effective_catalog, effective_pager);
      break;
    default:
      result = EXECUTE_ROW_NOT_FOUND;
      break;
  }

  if (stmt->num_ctes > 0) {
    for (uint32_t c = 0; c < stmt->num_ctes; c++) {
      if (stmt->ctes[c].cte_stmt) {
        free(stmt->ctes[c].cte_stmt);
        stmt->ctes[c].cte_stmt = NULL;
      }
      if (stmt->ctes[c].rec_stmt) {
        free(stmt->ctes[c].rec_stmt);
        stmt->ctes[c].rec_stmt = NULL;
      }
      if (catalog->num_tables > 0) {
        uint32_t cte_idx = catalog->num_tables - 1;
        pager_free_page(pager, catalog->tables[cte_idx].root_page_num);
        catalog->num_tables--;
      }
    }
  }
  return result;
}
