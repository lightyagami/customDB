#include "parser.h"

/* Helper to strip whitespace */
static const char* skip_whitespace(const char* p) {
  while (*p && isspace((unsigned char)*p)) p++;
  return p;
}

static const char* parse_identifier(const char* p, char* dest, uint32_t max_len) {
  p = skip_whitespace(p);
  uint32_t len = 0;
  while (*p && (isalnum((unsigned char)*p) || *p == '_' || *p == '.')) {
    if (len < max_len - 1) {
      dest[len++] = *p;
    }
    p++;
  }
  dest[len] = '\0';
  return p;
}

/* Helper to parse a string value (handles single/double quotes or unquoted strings) */
static const char* parse_value_token(const char* p, char* dest, uint32_t max_len) {
  p = skip_whitespace(p);
  uint32_t len = 0;
  if ((*p == 'x' || *p == 'X') && (p[1] == '\'' || p[1] == '"')) {
    char quote = p[1];
    if (len < max_len - 1) dest[len++] = *p;
    p++;
    if (len < max_len - 1) dest[len++] = *p;
    p++;
    while (*p && *p != quote) {
      if (len < max_len - 1) dest[len++] = *p;
      p++;
    }
    if (*p == quote) {
      if (len < max_len - 1) dest[len++] = *p;
      p++;
    }
  } else if (*p == '\'' || *p == '"') {
    char quote = *p++;
    while (*p) {
      if (*p == quote) {
        if (p[1] == quote) {
          if (len < max_len - 1) dest[len++] = quote;
          p += 2;
          continue;
        }
        p++;
        break;
      }
      if (len < max_len - 1) {
        dest[len++] = *p;
      }
      p++;
    }
  } else {
    while (*p && !isspace((unsigned char)*p) && *p != ',' && *p != ')' && *p != '=') {
      if (len < max_len - 1) {
        dest[len++] = *p;
      }
      p++;
    }
  }
  dest[len] = '\0';
  return p;
}

static const char* parse_where_clause(const char* p, WhereClause* wc) {
  memset(wc, 0, sizeof(WhereClause));
  p = skip_whitespace(p);
  if (strncasecmp(p, "where", 5) != 0) {
    wc->has_where = false;
    return p;
  }
  p += 5;
  wc->has_where = true;
  wc->num_conds = 0;

  while (*p) {
    p = skip_whitespace(p);
    if (!*p) break;
    if (wc->num_conds >= MAX_WHERE_CONDS) break;
    SingleCond* cond = &wc->conds[wc->num_conds];
    cond->is_subquery = false;
    cond->is_exists = false;
    cond->is_not_exists = false;

    if (strncasecmp(p, "not exists", 10) == 0) {
      cond->is_not_exists = true;
      p += 10;
    } else if (strncasecmp(p, "exists", 6) == 0) {
      cond->is_exists = true;
      p += 6;
    }

    if (cond->is_exists || cond->is_not_exists) {
      p = skip_whitespace(p);
      if (*p == '(') p++;
      p = skip_whitespace(p);

      if (strncasecmp(p, "select", 6) == 0) {
        p += 6;
        p = skip_whitespace(p);
        char dummy_col[COL_NAME_SIZE];
        if (isdigit((unsigned char)*p)) {
          while (*p && !isspace((unsigned char)*p)) p++;
        } else {
          p = parse_identifier(p, dummy_col, COL_NAME_SIZE);
        }
        p = skip_whitespace(p);
        if (strncasecmp(p, "from", 4) == 0) {
          p += 4;
          p = skip_whitespace(p);
          p = parse_identifier(p, cond->sub_table, TBL_NAME_SIZE);
        }
        p = skip_whitespace(p);
        if (strncasecmp(p, "where", 5) == 0) {
          p += 5;
          cond->has_sub_where = true;

          char left_tok[COL_NAME_SIZE];
          p = parse_identifier(p, left_tok, COL_NAME_SIZE);
          char* dot = strchr(left_tok, '.');
          if (dot != NULL) {
            snprintf(cond->sub_where_col, COL_NAME_SIZE, "%s", dot + 1);
          } else {
            snprintf(cond->sub_where_col, COL_NAME_SIZE, "%s", left_tok);
          }

          p = skip_whitespace(p);
          if (strncmp(p, ">=", 2) == 0) { cond->sub_where_op = OP_GTE; p += 2; }
          else if (strncmp(p, "<=", 2) == 0) { cond->sub_where_op = OP_LTE; p += 2; }
          else if (*p == '>') { cond->sub_where_op = OP_GT; p++; }
          else if (*p == '<') { cond->sub_where_op = OP_LT; p++; }
          else if (*p == '=') { cond->sub_where_op = OP_EQ; p++; }

          p = skip_whitespace(p);
          char right_tok[MAX_RAW_VAL];
          p = parse_identifier(p, right_tok, MAX_RAW_VAL);
          dot = strchr(right_tok, '.');
          if (dot != NULL) {
            cond->sub_where_is_correlated = true;
            snprintf(cond->sub_correlated_outer_col, COL_NAME_SIZE, "%s", dot + 1);
          } else {
            snprintf(cond->sub_where_val, MAX_RAW_VAL, "%s", right_tok);
          }
        }
        p = skip_whitespace(p);
        if (*p == ')') p++;
      }
    } else {
      p = parse_identifier(p, cond->col_name, COL_NAME_SIZE);
      if (strlen(cond->col_name) == 0) return NULL;

      p = skip_whitespace(p);
      if (strncmp(p, ">=", 2) == 0) {
        cond->op = OP_GTE;
        p += 2;
      } else if (strncmp(p, "<=", 2) == 0) {
        cond->op = OP_LTE;
        p += 2;
      } else if (*p == '>') {
        cond->op = OP_GT;
        p++;
      } else if (*p == '<') {
        cond->op = OP_LT;
        p++;
      } else if (*p == '=') {
        cond->op = OP_EQ;
        p++;
      } else if (strncasecmp(p, "is not null", 11) == 0 && (isspace((unsigned char)p[11]) || p[11] == '\0' || p[11] == ')')) {
        cond->op = OP_IS_NOT_NULL;
        p += 11;
      } else if (strncasecmp(p, "is null", 7) == 0 && (isspace((unsigned char)p[7]) || p[7] == '\0' || p[7] == ')')) {
        cond->op = OP_IS_NULL;
        p += 7;
      } else if (strncasecmp(p, "match", 5) == 0 && (isspace((unsigned char)p[5]) || p[5] == '\0' || p[5] == '\'')) {
        cond->op = OP_MATCH;
        p += 5;
      } else if (strncasecmp(p, "like", 4) == 0 && (isspace((unsigned char)p[4]) || p[4] == '\'' || p[4] == '"')) {
        cond->op = OP_LIKE;
        p += 4;
      } else if (strncasecmp(p, "between", 7) == 0 && isspace((unsigned char)p[7])) {
        cond->op = OP_BETWEEN;
        p += 7;
        p = parse_value_token(p, cond->raw_val, MAX_RAW_VAL);
        p = skip_whitespace(p);
        if (strncasecmp(p, "and", 3) != 0) return NULL;
        p += 3;
        p = parse_value_token(p, cond->raw_val2, MAX_RAW_VAL);
      } else if (strncasecmp(p, "in", 2) == 0 && (isspace((unsigned char)p[2]) || p[2] == '(')) {
        cond->op = OP_IN;
        p += 2;
        p = skip_whitespace(p);
        if (*p == '(') p++;
        p = skip_whitespace(p);

        if (strncasecmp(p, "select", 6) == 0) {
          cond->is_subquery = true;
          p += 6;
          p = skip_whitespace(p);
          p = parse_identifier(p, cond->sub_col, COL_NAME_SIZE);
          p = skip_whitespace(p);
          if (strncasecmp(p, "from", 4) == 0) {
            p += 4;
            p = skip_whitespace(p);
            p = parse_identifier(p, cond->sub_table, TBL_NAME_SIZE);
          }
          p = skip_whitespace(p);
          if (strncasecmp(p, "where", 5) == 0) {
            p += 5;
            cond->has_sub_where = true;
            p = parse_identifier(p, cond->sub_where_col, COL_NAME_SIZE);
            p = skip_whitespace(p);
            if (strncmp(p, ">=", 2) == 0) { cond->sub_where_op = OP_GTE; p += 2; }
            else if (strncmp(p, "<=", 2) == 0) { cond->sub_where_op = OP_LTE; p += 2; }
            else if (*p == '>') { cond->sub_where_op = OP_GT; p++; }
            else if (*p == '<') { cond->sub_where_op = OP_LT; p++; }
            else if (*p == '=') { cond->sub_where_op = OP_EQ; p++; }

            p = parse_value_token(p, cond->sub_where_val, MAX_RAW_VAL);
          }
          p = skip_whitespace(p);
          if (*p == ')') p++;
        }
      } else {
        return NULL;
      }
    }

    if (!cond->is_subquery && !cond->is_exists && !cond->is_not_exists && cond->op != OP_IS_NULL && cond->op != OP_IS_NOT_NULL && cond->op != OP_BETWEEN) {
      p = parse_value_token(p, cond->raw_val, MAX_RAW_VAL);
      if (strlen(cond->raw_val) == 0) return NULL;
    }

    p = skip_whitespace(p);
    if (strncasecmp(p, "collate", 7) == 0) {
      p += 7;
      p = skip_whitespace(p);
      if (strncasecmp(p, "nocase", 6) == 0) { cond->collation = COLL_NOCASE; p += 6; }
      else if (strncasecmp(p, "rtrim", 5) == 0) { cond->collation = COLL_RTRIM; p += 5; }
      else if (strncasecmp(p, "binary", 6) == 0) { cond->collation = COLL_BINARY; p += 6; }
    }

    wc->num_conds++;

    p = skip_whitespace(p);
    if (strncasecmp(p, "and", 3) == 0 && (isspace((unsigned char)p[3]) || p[3] == '\0')) {
      if (wc->num_conds - 1 < MAX_WHERE_CONDS - 1) {
        wc->logic_ops[wc->num_conds - 1] = LOGIC_AND;
      }
      p += 3;
    } else if (strncasecmp(p, "or", 2) == 0 && (isspace((unsigned char)p[2]) || p[2] == '\0')) {
      if (wc->num_conds - 1 < MAX_WHERE_CONDS - 1) {
        wc->logic_ops[wc->num_conds - 1] = LOGIC_OR;
      }
      p += 2;
    } else {
      break;
    }
  }

  return p;
}

/* Parses column list: (id INT, active BOOL, rating DOUBLE, name VARCHAR(100)) */
static PrepareResult parse_create_cols(const char* p, TableDef* def) {
  p = skip_whitespace(p);
  if (*p != '(') return PREPARE_BAD_SCHEMA;
  p++; /* skip '(' */

  def->num_cols = 0;

  while (*p && *p != ')') {
    if (def->num_cols >= MAX_COLUMNS) return PREPARE_BAD_SCHEMA;

    p = skip_whitespace(p);
    if (strncasecmp(p, "primary key", 11) == 0) {
      p += 11;
      p = skip_whitespace(p);
      if (*p == '(') {
        p++;
        while (*p && *p != ')') {
          p = skip_whitespace(p);
          if (def->num_pk_cols < MAX_COLUMNS) {
            p = parse_identifier(p, def->pk_cols[def->num_pk_cols], COL_NAME_SIZE);
            def->num_pk_cols++;
          }
          p = skip_whitespace(p);
          if (*p == ',') p++;
        }
        if (*p == ')') p++;
      }
      p = skip_whitespace(p);
      if (*p == ',') p++;
      continue;
    }

    Column* col = &def->columns[def->num_cols];
    memset(col, 0, sizeof(Column));

    p = parse_identifier(p, col->name, COL_NAME_SIZE);
    if (strlen(col->name) == 0) return PREPARE_BAD_SCHEMA;

    p = skip_whitespace(p);
    char type_name[32];
    p = parse_identifier(p, type_name, sizeof(type_name));
    
    if (strcasecmp(type_name, "INT") == 0 || strcasecmp(type_name, "INTEGER") == 0 ||
        strcasecmp(type_name, "BIGINT") == 0 || strcasecmp(type_name, "SMALLINT") == 0 ||
        strcasecmp(type_name, "TINYINT") == 0 || strcasecmp(type_name, "INT2") == 0 ||
        strcasecmp(type_name, "INT4") == 0 || strcasecmp(type_name, "INT8") == 0) {
      col->type = COL_INT;
      col->size = 4;
    } else if (strcasecmp(type_name, "FLOAT") == 0 || strcasecmp(type_name, "REAL") == 0) {
      col->type = COL_FLOAT;
      col->size = 4;
    } else if (strcasecmp(type_name, "DOUBLE") == 0) {
      p = skip_whitespace(p);
      if (strncasecmp(p, "precision", 9) == 0) p += 9;
      col->type = COL_DOUBLE;
      col->size = 8;
    } else if (strcasecmp(type_name, "NUMERIC") == 0 || strcasecmp(type_name, "DECIMAL") == 0) {
      col->type = (strcasecmp(type_name, "NUMERIC") == 0) ? COL_NUMERIC : COL_DECIMAL;
      col->size = 8;
      p = skip_whitespace(p);
      if (*p == '(') {
        while (*p && *p != ')') p++;
        if (*p == ')') p++;
      }
    } else if (strcasecmp(type_name, "BOOL") == 0 || strcasecmp(type_name, "BOOLEAN") == 0) {
      col->type = COL_BOOL;
      col->size = 1;
    } else if (strcasecmp(type_name, "BLOB") == 0 || strcasecmp(type_name, "VARBINARY") == 0 ||
               strcasecmp(type_name, "BINARY") == 0 || strcasecmp(type_name, "BYTEA") == 0) {
      col->type = COL_BLOB;
      col->size = 256;
      p = skip_whitespace(p);
      if (*p == '(') {
        while (*p && *p != ')') p++;
        if (*p == ')') p++;
      }
    } else if (strcasecmp(type_name, "DATETIME") == 0) {
      col->type = COL_DATETIME;
      col->size = 64;
    } else if (strcasecmp(type_name, "TIMESTAMP") == 0) {
      col->type = COL_TIMESTAMP;
      col->size = 64;
    } else if (strcasecmp(type_name, "DATE") == 0) {
      col->type = COL_DATE;
      col->size = 64;
    } else if (strcasecmp(type_name, "TIME") == 0) {
      col->type = COL_TIME;
      col->size = 64;
    } else if (strcasecmp(type_name, "TEXT") == 0 || strcasecmp(type_name, "VARCHAR") == 0 ||
               strcasecmp(type_name, "CHAR") == 0 || strcasecmp(type_name, "CLOB") == 0 ||
               strcasecmp(type_name, "STRING") == 0) {
      col->type = (strcasecmp(type_name, "TEXT") == 0 || strcasecmp(type_name, "CLOB") == 0) ? COL_TEXT : COL_VARCHAR;
      p = skip_whitespace(p);
      if (*p == '(') {
        p++;
        char size_str[16];
        p = parse_value_token(p, size_str, sizeof(size_str));
        col->size = (uint32_t)atoi(size_str);
        if (col->size == 0 || col->size > MAX_TEXT_SIZE) {
          return PREPARE_BAD_SCHEMA;
        }
        p = skip_whitespace(p);
        if (*p == ')') p++;
      } else {
        col->size = 64; /* default fallback size */
      }
    } else {
      return PREPARE_BAD_SCHEMA;
    }

    col->has_index = false;
    col->index_root_page = 0;
    col->is_not_null = false;
    col->is_unique = false;
    col->has_default = false;
    col->has_check = false;
    col->has_fk = false;

    /* Primary key validation: the first column must be named 'id' and must be INT */
    if (def->num_cols == 0) {
      if (col->type != COL_INT) {
        return PREPARE_BAD_SCHEMA;
      }
    }

    /* Parse inline column constraints */
    while (true) {
      p = skip_whitespace(p);
      if (strncasecmp(p, "not null", 8) == 0) {
        col->is_not_null = true;
        p += 8;
      } else if (strncasecmp(p, "unique", 6) == 0) {
        col->is_unique = true;
        p += 6;
      } else if (strncasecmp(p, "default", 7) == 0) {
        p += 7;
        col->has_default = true;
        p = parse_value_token(p, col->default_val, MAX_RAW_VAL);
      } else if (strncasecmp(p, "check", 5) == 0) {
        p += 5;
        col->has_check = true;
        p = skip_whitespace(p);
        if (*p == '(') p++;
        p = skip_whitespace(p);

        char check_col[COL_NAME_SIZE];
        p = parse_identifier(p, check_col, COL_NAME_SIZE);
        p = skip_whitespace(p);

        if (strncmp(p, ">=", 2) == 0) { col->check_op = OP_GTE; p += 2; }
        else if (strncmp(p, "<=", 2) == 0) { col->check_op = OP_LTE; p += 2; }
        else if (*p == '>') { col->check_op = OP_GT; p++; }
        else if (*p == '<') { col->check_op = OP_LT; p++; }
        else if (*p == '=') { col->check_op = OP_EQ; p++; }

        p = parse_value_token(p, col->check_val, MAX_RAW_VAL);
        p = skip_whitespace(p);
        if (*p == ')') p++;
      } else if (strncasecmp(p, "references", 10) == 0) {
        p += 10;
        col->has_fk = true;
        p = parse_identifier(p, col->fk_target_table, TBL_NAME_SIZE);
        p = skip_whitespace(p);
        if (*p == '(') {
          p++;
          p = parse_identifier(p, col->fk_target_col, COL_NAME_SIZE);
          p = skip_whitespace(p);
          if (*p == ')') p++;
        }
        while (true) {
          p = skip_whitespace(p);
          if (strncasecmp(p, "on delete cascade", 17) == 0) {
            col->fk_on_delete_cascade = true;
            p += 17;
          } else if (strncasecmp(p, "on update cascade", 17) == 0) {
            col->fk_on_update_cascade = true;
            p += 17;
          } else {
            break;
          }
        }
      } else if (strncasecmp(p, "autoincrement", 13) == 0) {
        col->is_autoincrement = true;
        p += 13;
      } else if (strncasecmp(p, "primary key", 11) == 0) {
        p += 11;
        p = skip_whitespace(p);
        if (*p == '(') {
          p++;
          while (*p && *p != ')') {
            p = skip_whitespace(p);
            if (def->num_pk_cols < MAX_COLUMNS) {
              p = parse_identifier(p, def->pk_cols[def->num_pk_cols], COL_NAME_SIZE);
              def->num_pk_cols++;
            }
            p = skip_whitespace(p);
            if (*p == ',') p++;
          }
          if (*p == ')') p++;
        }
      } else if (strncasecmp(p, "collate", 7) == 0) {
        p += 7;
        p = skip_whitespace(p);
        if (strncasecmp(p, "nocase", 6) == 0) { col->collation = COLL_NOCASE; p += 6; }
        else if (strncasecmp(p, "rtrim", 5) == 0) { col->collation = COLL_RTRIM; p += 5; }
        else if (strncasecmp(p, "binary", 6) == 0) { col->collation = COLL_BINARY; p += 6; }
      } else {
        break;
      }
    }

    def->num_cols++;
    p = skip_whitespace(p);
    if (*p == ',') {
      p++;
    } else if (*p != ')') {
      return PREPARE_BAD_SCHEMA;
    }
  }

  if (*p == ')') p++;
  return PREPARE_SUCCESS;
}

static const char* parse_window_over(const char* p, WindowSpec* spec) {
  p = skip_whitespace(p);
  if (strncasecmp(p, "over", 4) == 0 && (isspace((unsigned char)p[4]) || p[4] == '(')) {
    p += 4;
    p = skip_whitespace(p);
    if (*p == '(') p++;
    p = skip_whitespace(p);

    if (strncasecmp(p, "partition by", 12) == 0) {
      p += 12;
      p = skip_whitespace(p);
      p = parse_identifier(p, spec->partition_col, COL_NAME_SIZE);
      p = skip_whitespace(p);
    }
    if (strncasecmp(p, "order by", 8) == 0) {
      p += 8;
      p = skip_whitespace(p);
      p = parse_identifier(p, spec->order_col, COL_NAME_SIZE);
      p = skip_whitespace(p);
      if (strncasecmp(p, "desc", 4) == 0) {
        spec->order_desc = true;
        p += 4;
      } else if (strncasecmp(p, "asc", 3) == 0) {
        spec->order_desc = false;
        p += 3;
      }
      p = skip_whitespace(p);
    }
    if (*p == ')') p++;
  }
  return p;
}

/* prepare_statement parses command line into a Statement */
PrepareResult prepare_statement(const char* input, Statement* out) {
  memset(out, 0, sizeof(Statement));
  const char* p = skip_whitespace(input);

  /* Handle EXPLAIN prefix */
  if (strncasecmp(p, "explain", 7) == 0 && isspace((unsigned char)p[7])) {
    out->is_explain = true;
    p += 7;
    p = skip_whitespace(p);
  }

  /* Handle WITH clause for CTEs */
  if (strncasecmp(p, "with", 4) == 0 && isspace((unsigned char)p[4])) {
    p += 4;
    p = skip_whitespace(p);
    bool is_rec = false;
    if (strncasecmp(p, "recursive", 9) == 0 && isspace((unsigned char)p[9])) {
      is_rec = true;
      p += 9;
      p = skip_whitespace(p);
    }
    while (*p && out->num_ctes < 4) {
      CteDef* cte = &out->ctes[out->num_ctes];
      cte->is_recursive = is_rec;
      p = parse_identifier(p, cte->cte_name, TBL_NAME_SIZE);
      p = skip_whitespace(p);
      if (*p == '(') {
        p++;
        while (*p && *p != ')') p++;
        if (*p == ')') p++;
        p = skip_whitespace(p);
      }
      if (strncasecmp(p, "as", 2) == 0) { p += 2; p = skip_whitespace(p); }
      if (*p == '(') p++;
      p = skip_whitespace(p);

      char sub_buf[512];
      int b_idx = 0;
      int depth = 1;
      while (*p && depth > 0) {
        if (*p == '(') depth++;
        else if (*p == ')') depth--;
        if (depth > 0 && b_idx < (int)sizeof(sub_buf) - 1) {
          sub_buf[b_idx++] = *p;
        }
        p++;
      }
      sub_buf[b_idx] = '\0';
      
      cte->cte_stmt = malloc(sizeof(Statement));
      prepare_statement(sub_buf, cte->cte_stmt);
      out->num_ctes++;

      p = skip_whitespace(p);
      if (*p == ',') {
        p++;
        p = skip_whitespace(p);
      } else {
        break;
      }
    }
  }

  if (strncasecmp(p, "alter", 5) == 0 && isspace((unsigned char)p[5])) {
    out->type = STATEMENT_ALTER_TABLE;
    p += 5;
    p = skip_whitespace(p);
    if (strncasecmp(p, "table", 5) == 0 && isspace((unsigned char)p[5])) {
      p += 5;
      p = skip_whitespace(p);
    }
    p = parse_identifier(p, out->table_name, TBL_NAME_SIZE);
    p = skip_whitespace(p);
    if (strncasecmp(p, "rename to", 9) == 0) {
      out->alter_type = ALTER_RENAME_TABLE;
      p += 9;
      p = skip_whitespace(p);
      p = parse_identifier(p, out->new_table_name, TBL_NAME_SIZE);
    } else if (strncasecmp(p, "rename", 6) == 0) {
      out->alter_type = ALTER_RENAME_TABLE;
      p += 6;
      p = skip_whitespace(p);
      if (strncasecmp(p, "to", 2) == 0) { p += 2; p = skip_whitespace(p); }
      p = parse_identifier(p, out->new_table_name, TBL_NAME_SIZE);
    } else if (strncasecmp(p, "add column", 10) == 0 || strncasecmp(p, "add", 3) == 0) {
      out->alter_type = ALTER_ADD_COLUMN;
      if (strncasecmp(p, "add column", 10) == 0) p += 10;
      else p += 3;
      p = skip_whitespace(p);
      Column col;
      memset(&col, 0, sizeof(Column));
      p = parse_identifier(p, col.name, COL_NAME_SIZE);
      p = skip_whitespace(p);
      char type_buf[32];
      p = parse_identifier(p, type_buf, sizeof(type_buf));
      if (strcasecmp(type_buf, "int") == 0 || strcasecmp(type_buf, "integer") == 0) { col.type = COL_INT; col.size = 4; }
      else if (strcasecmp(type_buf, "double") == 0 || strcasecmp(type_buf, "real") == 0) { col.type = COL_DOUBLE; col.size = 8; }
      else { col.type = COL_TEXT; col.size = MAX_TEXT_SIZE; }
      out->new_col = col;
    } else if (strncasecmp(p, "drop column", 11) == 0 || strncasecmp(p, "drop", 4) == 0) {
      out->alter_type = ALTER_DROP_COLUMN;
      if (strncasecmp(p, "drop column", 11) == 0) p += 11;
      else p += 4;
      p = skip_whitespace(p);
      p = parse_identifier(p, out->drop_col_name, COL_NAME_SIZE);
    } else {
      return PREPARE_SYNTAX_ERROR;
    }
    return PREPARE_SUCCESS;
  }

  if (strncasecmp(p, "insert", 6) == 0) {
    out->type = STATEMENT_INSERT;
    p += 6;
    
    p = skip_whitespace(p);
    if (strncasecmp(p, "into", 4) == 0) {
      p += 4;
    }
    
    p = parse_identifier(p, out->table_name, TBL_NAME_SIZE);
    if (strlen(out->table_name) == 0) return PREPARE_SYNTAX_ERROR;

    p = skip_whitespace(p);
    if (strncasecmp(p, "select", 6) == 0) {
      out->is_insert_select = true;
      out->insert_select_stmt = malloc(sizeof(Statement));
      memset(out->insert_select_stmt, 0, sizeof(Statement));
      PrepareResult pr = prepare_statement(p, out->insert_select_stmt);
      if (pr != PREPARE_SUCCESS) {
        free(out->insert_select_stmt);
        out->insert_select_stmt = NULL;
        return pr;
      }
      return PREPARE_SUCCESS;
    }

    if (strncasecmp(p, "values", 6) == 0) {
      p += 6;
    }

    out->num_values = 0;
    out->num_multi_rows = 0;

    while (*p) {
      p = skip_whitespace(p);
      if (*p != '(') break;
      p++;

      uint32_t col_idx = 0;
      while (*p && *p != ')') {
        if (col_idx >= MAX_COLUMNS) return PREPARE_SYNTAX_ERROR;
        
        p = parse_value_token(p, out->multi_raw_values[out->num_multi_rows][col_idx], MAX_RAW_VAL);
        if (out->num_multi_rows == 0) {
          strncpy(out->raw_values[col_idx], out->multi_raw_values[0][col_idx], MAX_RAW_VAL - 1);
        }
        col_idx++;

        p = skip_whitespace(p);
        if (*p == ',') {
          p++;
        } else if (*p != ')') {
          return PREPARE_SYNTAX_ERROR;
        }
      }
      if (*p == ')') p++;
      out->num_values = col_idx;
      out->num_multi_rows++;

      p = skip_whitespace(p);
      if (*p == ',') {
        p++;
      } else {
        break;
      }
    }
    if (out->num_multi_rows > 1) {
      out->is_multi_insert = true;
    }
    return (out->num_values > 0) ? PREPARE_SUCCESS : PREPARE_SYNTAX_ERROR;
  }

  if (strncasecmp(p, "select", 6) == 0) {
    out->type = STATEMENT_SELECT;
    p += 6;

    p = skip_whitespace(p);
    if (strncasecmp(p, "distinct", 8) == 0 && (isspace((unsigned char)p[8]) || p[8] == '*' || isalpha(p[8]))) {
      out->is_distinct = true;
      p += 8;
      p = skip_whitespace(p);
    }
    if (*p == '*') {
      p++;
    } else {
      /* Parse column or aggregate function list */
      out->num_select_cols = 0;
      while (*p && strncasecmp(p, "from", 4) != 0) {
        if (out->num_select_cols >= MAX_SELECT_COLS) return PREPARE_SYNTAX_ERROR;
        SelectCol* sc = &out->select_cols[out->num_select_cols];
        p = skip_whitespace(p);

        if (strncasecmp(p, "row_number()", 12) == 0) {
          sc->win_spec.win_func = WIN_ROW_NUMBER;
          p += 12;
          p = parse_window_over(p, &sc->win_spec);
        } else if (strncasecmp(p, "rank()", 6) == 0) {
          sc->win_spec.win_func = WIN_RANK;
          p += 6;
          p = parse_window_over(p, &sc->win_spec);
        } else if (strncasecmp(p, "dense_rank()", 12) == 0) {
          sc->win_spec.win_func = WIN_DENSE_RANK;
          p += 12;
          p = parse_window_over(p, &sc->win_spec);
        } else if (strncasecmp(p, "count(*)", 8) == 0) {
          sc->func = AGG_COUNT_STAR;
          out->is_aggregate = true;
          p += 8;
        } else if (strncasecmp(p, "count(", 6) == 0) {
          sc->func = AGG_COUNT;
          out->is_aggregate = true;
          p += 6;
          p = parse_identifier(p, sc->col_name, COL_NAME_SIZE);
          p = skip_whitespace(p);
          if (*p == ')') p++;
          p = parse_window_over(p, &sc->win_spec);
          if (strlen(sc->win_spec.partition_col) > 0 || strlen(sc->win_spec.order_col) > 0) {
            sc->win_spec.win_func = WIN_SUM;
            out->is_aggregate = false;
          }
        } else if (strncasecmp(p, "sum(", 4) == 0) {
          sc->func = AGG_SUM;
          out->is_aggregate = true;
          p += 4;
          p = parse_identifier(p, sc->col_name, COL_NAME_SIZE);
          p = skip_whitespace(p);
          if (*p == ')') p++;
          p = parse_window_over(p, &sc->win_spec);
          if (strlen(sc->win_spec.partition_col) > 0 || strlen(sc->win_spec.order_col) > 0) {
            sc->win_spec.win_func = WIN_SUM;
            out->is_aggregate = false;
          }
        } else if (strncasecmp(p, "avg(", 4) == 0) {
          sc->func = AGG_AVG;
          out->is_aggregate = true;
          p += 4;
          p = parse_identifier(p, sc->col_name, COL_NAME_SIZE);
          p = skip_whitespace(p);
          if (*p == ')') p++;
          p = parse_window_over(p, &sc->win_spec);
          if (strlen(sc->win_spec.partition_col) > 0 || strlen(sc->win_spec.order_col) > 0) {
            sc->win_spec.win_func = WIN_AVG;
            out->is_aggregate = false;
          }
        } else if (strncasecmp(p, "min(", 4) == 0) {
          sc->func = AGG_MIN;
          out->is_aggregate = true;
          p += 4;
          p = parse_identifier(p, sc->col_name, COL_NAME_SIZE);
          p = skip_whitespace(p);
          if (*p == ')') p++;
          p = parse_window_over(p, &sc->win_spec);
          if (strlen(sc->win_spec.partition_col) > 0 || strlen(sc->win_spec.order_col) > 0) {
            sc->win_spec.win_func = WIN_MIN;
            out->is_aggregate = false;
          }
        } else if (strncasecmp(p, "max(", 4) == 0) {
          sc->func = AGG_MAX;
          out->is_aggregate = true;
          p += 4;
          p = parse_identifier(p, sc->col_name, COL_NAME_SIZE);
          p = skip_whitespace(p);
          if (*p == ')') p++;
          p = parse_window_over(p, &sc->win_spec);
          if (strlen(sc->win_spec.partition_col) > 0 || strlen(sc->win_spec.order_col) > 0) {
            sc->win_spec.win_func = WIN_MAX;
            out->is_aggregate = false;
          }
        } else if (strncasecmp(p, "coalesce(", 9) == 0) {
          sc->func = AGG_NONE;
          sc->is_coalesce = true;
          p += 9;
          p = skip_whitespace(p);
          p = parse_identifier(p, sc->col_name, COL_NAME_SIZE);
          p = skip_whitespace(p);
          if (*p == ',') p++;
          p = skip_whitespace(p);
          p = parse_value_token(p, sc->coalesce_default, MAX_RAW_VAL);
          p = skip_whitespace(p);
          if (*p == ')') p++;
        } else {
          /* Plain column identifier or function expression */
          sc->func = AGG_NONE;
          if ((*p == '\'' || *p == '"' || isdigit(*p)) && strchr(p, '(') == NULL) {
            p = parse_value_token(p, sc->col_name, sizeof(sc->col_name));
          } else {
            uint32_t idx = 0;
            int depth = 0;
            while (*p && idx < sizeof(sc->col_name) - 1) {
              if (*p == '(') depth++;
              else if (*p == ')') depth--;
              if (depth == 0 && (*p == ',' || strncasecmp(p, "from", 4) == 0)) break;
              sc->col_name[idx++] = *p++;
            }
            sc->col_name[idx] = '\0';
            while (idx > 0 && isspace((unsigned char)sc->col_name[idx-1])) {
              sc->col_name[--idx] = '\0';
            }
          }
        }

        out->num_select_cols++;
        p = skip_whitespace(p);
        if (*p == ',') {
          p++;
        } else if (*p != '\0' && strncasecmp(p, "from", 4) != 0) {
          return PREPARE_SYNTAX_ERROR;
        }
      }
    }

    p = skip_whitespace(p);
    if (strncasecmp(p, "from", 4) == 0) {
      p += 4;
      p = parse_identifier(p, out->table_name, TBL_NAME_SIZE);
      if (strlen(out->table_name) == 0) return PREPARE_SYNTAX_ERROR;
    } else {
      out->table_name[0] = '\0';
      return PREPARE_SUCCESS;
    }

    p = skip_whitespace(p);
    out->join_clause.type = JOIN_INNER;
    if (strncasecmp(p, "left", 4) == 0) {
      out->join_clause.type = JOIN_LEFT;
      p += 4;
      p = skip_whitespace(p);
      if (strncasecmp(p, "outer", 5) == 0) { p += 5; p = skip_whitespace(p); }
      if (strncasecmp(p, "join", 4) == 0) { p += 4; }
      out->has_join = true;
    } else if (strncasecmp(p, "right", 5) == 0) {
      out->join_clause.type = JOIN_RIGHT;
      p += 5;
      p = skip_whitespace(p);
      if (strncasecmp(p, "outer", 5) == 0) { p += 5; p = skip_whitespace(p); }
      if (strncasecmp(p, "join", 4) == 0) { p += 4; }
      out->has_join = true;
    } else if (strncasecmp(p, "full", 4) == 0) {
      out->join_clause.type = JOIN_FULL;
      p += 4;
      p = skip_whitespace(p);
      if (strncasecmp(p, "outer", 5) == 0) { p += 5; p = skip_whitespace(p); }
      if (strncasecmp(p, "join", 4) == 0) { p += 4; }
      out->has_join = true;
    } else if (strncasecmp(p, "inner", 5) == 0) {
      out->join_clause.type = JOIN_INNER;
      p += 5;
      p = skip_whitespace(p);
      if (strncasecmp(p, "join", 4) == 0) { p += 4; }
      out->has_join = true;
    } else if (strncasecmp(p, "join", 4) == 0) {
      out->join_clause.type = JOIN_INNER;
      p += 4;
      out->has_join = true;
    }

    if (out->has_join) {
      p = parse_identifier(p, out->join_table_name, TBL_NAME_SIZE);
      if (strlen(out->join_table_name) == 0) return PREPARE_SYNTAX_ERROR;

      p = skip_whitespace(p);
      if (strncasecmp(p, "on", 2) != 0) return PREPARE_SYNTAX_ERROR;
      p += 2;

      /* Parse left column (handles both users.id or id) */
      char left_tok[COL_NAME_SIZE];
      p = parse_identifier(p, left_tok, sizeof(left_tok));
      char* left_dot = strchr(left_tok, '.');
      if (left_dot != NULL) {
        snprintf(out->join_clause.left_col, COL_NAME_SIZE, "%s", left_dot + 1);
      } else {
        snprintf(out->join_clause.left_col, COL_NAME_SIZE, "%s", left_tok);
      }

      p = skip_whitespace(p);
      if (*p != '=') return PREPARE_SYNTAX_ERROR;
      p++;

      /* Parse right column (handles both orders.user_id or user_id) */
      char right_tok[COL_NAME_SIZE];
      p = parse_identifier(p, right_tok, sizeof(right_tok));
      char* right_dot = strchr(right_tok, '.');
      if (right_dot != NULL) {
        snprintf(out->join_clause.right_col, COL_NAME_SIZE, "%s", right_dot + 1);
      } else {
        snprintf(out->join_clause.right_col, COL_NAME_SIZE, "%s", right_tok);
      }
    }

    p = parse_where_clause(p, &out->where_clause);
    if (p == NULL) return PREPARE_SYNTAX_ERROR;

    /* Parse GROUP BY */
    p = skip_whitespace(p);
    if (strncasecmp(p, "group by", 8) == 0) {
      p += 8;
      out->has_group_by = true;
      p = skip_whitespace(p);
      p = parse_identifier(p, out->group_by_col, COL_NAME_SIZE);
      if (strlen(out->group_by_col) == 0) return PREPARE_SYNTAX_ERROR;
    }

    /* Parse HAVING */
    p = skip_whitespace(p);
    if (strncasecmp(p, "having", 6) == 0) {
      p += 6;
      out->has_having = true;
      p = skip_whitespace(p);

      if (strncasecmp(p, "count(*)", 8) == 0) {
        out->having_func = AGG_COUNT_STAR;
        p += 8;
      } else if (strncasecmp(p, "count(", 6) == 0) {
        out->having_func = AGG_COUNT;
        p += 6;
        p = parse_identifier(p, out->having_col, COL_NAME_SIZE);
        p = skip_whitespace(p);
        if (*p == ')') p++;
      } else if (strncasecmp(p, "sum(", 4) == 0) {
        out->having_func = AGG_SUM;
        p += 4;
        p = parse_identifier(p, out->having_col, COL_NAME_SIZE);
        p = skip_whitespace(p);
        if (*p == ')') p++;
      } else if (strncasecmp(p, "avg(", 4) == 0) {
        out->having_func = AGG_AVG;
        p += 4;
        p = parse_identifier(p, out->having_col, COL_NAME_SIZE);
        p = skip_whitespace(p);
        if (*p == ')') p++;
      } else if (strncasecmp(p, "min(", 4) == 0) {
        out->having_func = AGG_MIN;
        p += 4;
        p = parse_identifier(p, out->having_col, COL_NAME_SIZE);
        p = skip_whitespace(p);
        if (*p == ')') p++;
      } else if (strncasecmp(p, "max(", 4) == 0) {
        out->having_func = AGG_MAX;
        p += 4;
        p = parse_identifier(p, out->having_col, COL_NAME_SIZE);
        p = skip_whitespace(p);
        if (*p == ')') p++;
      }

      p = skip_whitespace(p);
      if (strncmp(p, ">=", 2) == 0) { out->having_op = OP_GTE; p += 2; }
      else if (strncmp(p, "<=", 2) == 0) { out->having_op = OP_LTE; p += 2; }
      else if (*p == '>') { out->having_op = OP_GT; p++; }
      else if (*p == '<') { out->having_op = OP_LT; p++; }
      else if (*p == '=') { out->having_op = OP_EQ; p++; }
      else { return PREPARE_SYNTAX_ERROR; }

      p = parse_value_token(p, out->having_val, MAX_RAW_VAL);
    }

    /* Parse ORDER BY */
    p = skip_whitespace(p);
    if (strncasecmp(p, "order by", 8) == 0) {
      p += 8;
      out->has_order_by = true;
      out->num_order_by = 0;
      while (*p) {
        p = skip_whitespace(p);
        if (out->num_order_by >= 4) break;
        OrderByItem* item = &out->order_by_items[out->num_order_by++];
        p = parse_identifier(p, item->col_name, COL_NAME_SIZE);
        if (strlen(item->col_name) == 0) return PREPARE_SYNTAX_ERROR;

        p = skip_whitespace(p);
        if (strncasecmp(p, "collate", 7) == 0) {
          p += 7;
          p = skip_whitespace(p);
          if (strncasecmp(p, "nocase", 6) == 0) { item->collation = COLL_NOCASE; p += 6; }
          else if (strncasecmp(p, "rtrim", 5) == 0) { item->collation = COLL_RTRIM; p += 5; }
          else if (strncasecmp(p, "binary", 6) == 0) { item->collation = COLL_BINARY; p += 6; }
        } else {
          item->collation = COLL_BINARY;
        }

        p = skip_whitespace(p);
        if (strncasecmp(p, "desc", 4) == 0) {
          item->is_desc = true;
          p += 4;
        } else if (strncasecmp(p, "asc", 3) == 0) {
          item->is_desc = false;
          p += 3;
        } else {
          item->is_desc = false;
        }

        p = skip_whitespace(p);
        if (*p == ',') {
          p++;
        } else {
          break;
        }
      }
      if (out->num_order_by > 0) {
        snprintf(out->order_by_col, sizeof(out->order_by_col), "%s", out->order_by_items[0].col_name);
        out->order_by_desc = out->order_by_items[0].is_desc;
        out->order_by_collation = out->order_by_items[0].collation;
      }
    }

    /* Parse LIMIT & OFFSET */
    p = skip_whitespace(p);
    if (strncasecmp(p, "limit", 5) == 0) {
      p += 5;
      out->has_limit = true;
      p = skip_whitespace(p);

      char limit_tok[32];
      p = parse_identifier(p, limit_tok, sizeof(limit_tok));
      if (strlen(limit_tok) == 0) return PREPARE_SYNTAX_ERROR;
      out->limit_val = atoi(limit_tok);

      p = skip_whitespace(p);
      if (strncasecmp(p, "offset", 6) == 0) {
        p += 6;
        out->has_offset = true;
        p = skip_whitespace(p);
        char offset_tok[32];
        p = parse_identifier(p, offset_tok, sizeof(offset_tok));
        out->offset_val = atoi(offset_tok);
      } else if (*p == ',') {
        p++;
        out->has_offset = true;
        out->offset_val = out->limit_val;
        p = skip_whitespace(p);
        char count_tok[32];
        p = parse_identifier(p, count_tok, sizeof(count_tok));
        out->limit_val = atoi(count_tok);
      }
    }

    return PREPARE_SUCCESS;
  }

  if (strncasecmp(p, "update", 6) == 0) {
    out->type = STATEMENT_UPDATE;
    p += 6;

    p = parse_identifier(p, out->table_name, TBL_NAME_SIZE);
    if (strlen(out->table_name) == 0) return PREPARE_SYNTAX_ERROR;

    p = skip_whitespace(p);
    if (strncasecmp(p, "set", 3) != 0) return PREPARE_SYNTAX_ERROR;
    p += 3;

    out->num_set_pairs = 0;
    while (*p) {
      p = skip_whitespace(p);
      if (strncasecmp(p, "where", 5) == 0) {
        break;
      }
      
      if (out->num_set_pairs >= MAX_COLUMNS) return PREPARE_SYNTAX_ERROR;
      SetPair* pair = &out->set_pairs[out->num_set_pairs];
      
      p = parse_identifier(p, pair->col_name, COL_NAME_SIZE);
      if (strlen(pair->col_name) == 0) return PREPARE_SYNTAX_ERROR;

      p = skip_whitespace(p);
      if (*p != '=') return PREPARE_SYNTAX_ERROR;
      p++;

      p = parse_value_token(p, pair->str_val, MAX_RAW_VAL);
      out->num_set_pairs++;

      p = skip_whitespace(p);
      if (*p == ',') {
        p++;
      } else if (strncasecmp(p, "where", 5) != 0 && *p != '\0') {
        return PREPARE_SYNTAX_ERROR;
      }
    }

    p = parse_where_clause(p, &out->where_clause);
    if (p == NULL) return PREPARE_SYNTAX_ERROR;
    return PREPARE_SUCCESS;
  }

  if (strncasecmp(p, "delete", 6) == 0) {
    out->type = STATEMENT_DELETE;
    p += 6;

    p = skip_whitespace(p);
    if (strncasecmp(p, "from", 4) == 0) {
      p += 4;
    } else {
      return PREPARE_SYNTAX_ERROR;
    }

    p = parse_identifier(p, out->table_name, TBL_NAME_SIZE);
    if (strlen(out->table_name) == 0) return PREPARE_SYNTAX_ERROR;

    p = parse_where_clause(p, &out->where_clause);
    if (p == NULL) return PREPARE_SYNTAX_ERROR;

    return PREPARE_SUCCESS;
  }

  if (strncasecmp(p, "create", 6) == 0) {
    p += 6;
    p = skip_whitespace(p);
    if (strncasecmp(p, "virtual table", 13) == 0) {
      p += 13;
      p = skip_whitespace(p);
      /* Skip optional IF NOT EXISTS */
      if (strncasecmp(p, "if not exists", 13) == 0) { p += 13; p = skip_whitespace(p); out->if_not_exists = true; }
      out->type = STATEMENT_CREATE_VTABLE;
      p = parse_identifier(p, out->table_name, TBL_NAME_SIZE);
      if (strlen(out->table_name) == 0) return PREPARE_SYNTAX_ERROR;

      p = skip_whitespace(p);
      if (strncasecmp(p, "using", 5) != 0) return PREPARE_SYNTAX_ERROR;
      p += 5;

      p = parse_identifier(p, out->vtab_module, sizeof(out->vtab_module));
      p = skip_whitespace(p);
      if (*p == '(') {
        p++;
        p = parse_value_token(p, out->vtab_args, sizeof(out->vtab_args));
        p = skip_whitespace(p);
        if (*p == ')') p++;
      }
      return PREPARE_SUCCESS;
    }
    if (strncasecmp(p, "index", 5) == 0) {
      p += 5;
      out->type = STATEMENT_CREATE_INDEX;
      p = parse_identifier(p, out->index_name, COL_NAME_SIZE);
      if (strlen(out->index_name) == 0) return PREPARE_SYNTAX_ERROR;

      p = skip_whitespace(p);
      if (strncasecmp(p, "on", 2) != 0) return PREPARE_SYNTAX_ERROR;
      p += 2;

      p = parse_identifier(p, out->table_name, TBL_NAME_SIZE);
      if (strlen(out->table_name) == 0) return PREPARE_SYNTAX_ERROR;

      p = skip_whitespace(p);
      if (*p != '(') return PREPARE_SYNTAX_ERROR;
      p++;

      p = skip_whitespace(p);
      if (strncasecmp(p, "lower(", 6) == 0 || strncasecmp(p, "upper(", 6) == 0) {
        out->index_is_expr = true;
        if (strncasecmp(p, "lower(", 6) == 0) { strcpy(out->index_expr_func, "lower"); p += 6; }
        else { strcpy(out->index_expr_func, "upper"); p += 6; }
        p = parse_identifier(p, out->index_expr_col, COL_NAME_SIZE);
        p = skip_whitespace(p);
        if (*p == ')') p++;
        snprintf(out->index_cols[0], COL_NAME_SIZE, "%s", out->index_expr_col);
        out->index_num_cols = 1;
      } else {
        out->index_num_cols = 0;
        while (*p && *p != ')') {
          p = skip_whitespace(p);
          if (out->index_num_cols >= MAX_COLUMNS) return PREPARE_SYNTAX_ERROR;
          p = parse_identifier(p, out->index_cols[out->index_num_cols], COL_NAME_SIZE);
          if (strlen(out->index_cols[out->index_num_cols]) == 0) return PREPARE_SYNTAX_ERROR;
          out->index_num_cols++;

          p = skip_whitespace(p);
          if (*p == ',') {
            p++;
          } else if (*p != ')') {
            return PREPARE_SYNTAX_ERROR;
          }
        }
      }
      if (*p == ')') p++;

      if (out->index_num_cols > 0) {
        snprintf(out->index_col_name, COL_NAME_SIZE, "%s", out->index_cols[0]);
      }

      p = skip_whitespace(p);
      if (strncasecmp(p, "where", 5) == 0) {
        out->index_is_partial = true;
        p += 5;
        p = parse_where_clause(p, &out->index_where);
      }

      return PREPARE_SUCCESS;
    }

    if (strncasecmp(p, "view", 4) == 0) {
      p += 4;
      out->type = STATEMENT_CREATE_VIEW;
      p = parse_identifier(p, out->view_name, IDX_NAME_SIZE);
      if (strlen(out->view_name) == 0) return PREPARE_SYNTAX_ERROR;
      p = skip_whitespace(p);
      if (strncasecmp(p, "as", 2) == 0) p += 2;
      p = skip_whitespace(p);
      strncpy(out->view_select_sql, p, sizeof(out->view_select_sql) - 1);
      return PREPARE_SUCCESS;
    }

    if (strncasecmp(p, "trigger", 7) == 0) {
      p += 7;
      out->type = STATEMENT_CREATE_TRIGGER;
      p = parse_identifier(p, out->trigger_name, IDX_NAME_SIZE);
      if (strlen(out->trigger_name) == 0) return PREPARE_SYNTAX_ERROR;
      p = skip_whitespace(p);

      if (strncasecmp(p, "before", 6) == 0) { out->trigger_timing = TRIGGER_BEFORE; p += 6; }
      else if (strncasecmp(p, "after", 5) == 0) { out->trigger_timing = TRIGGER_AFTER; p += 5; }
      else out->trigger_timing = TRIGGER_AFTER;
      p = skip_whitespace(p);

      if (strncasecmp(p, "insert", 6) == 0) { out->trigger_event = TRIGGER_INSERT; p += 6; }
      else if (strncasecmp(p, "update", 6) == 0) { out->trigger_event = TRIGGER_UPDATE; p += 6; }
      else if (strncasecmp(p, "delete", 6) == 0) { out->trigger_event = TRIGGER_DELETE; p += 6; }
      p = skip_whitespace(p);

      if (strncasecmp(p, "on", 2) == 0) p += 2;
      p = skip_whitespace(p);
      p = parse_identifier(p, out->table_name, TBL_NAME_SIZE);
      p = skip_whitespace(p);

      if (strncasecmp(p, "begin", 5) == 0) p += 5;
      p = skip_whitespace(p);

      int a_idx = 0;
      while (*p && strncasecmp(p, "end", 3) != 0 && a_idx < (int)sizeof(out->trigger_action_sql) - 1) {
        out->trigger_action_sql[a_idx++] = *p++;
      }
      out->trigger_action_sql[a_idx] = '\0';
      return PREPARE_SUCCESS;
    }

    if (strncasecmp(p, "table", 5) == 0) {
      p += 5;
      p = skip_whitespace(p);
      /* Skip optional IF NOT EXISTS */
      if (strncasecmp(p, "if not exists", 13) == 0) { p += 13; p = skip_whitespace(p); out->if_not_exists = true; }
      out->type = STATEMENT_CREATE_TABLE;
      p = parse_identifier(p, out->new_table.name, TBL_NAME_SIZE);
      if (strlen(out->new_table.name) == 0) return PREPARE_SYNTAX_ERROR;

      PrepareResult res = parse_create_cols(p, &out->new_table);
      if (res != PREPARE_SUCCESS) return res;

      return PREPARE_SUCCESS;
    }

    return PREPARE_UNRECOGNIZED_STATEMENT;
  }

  if (strncasecmp(p, "drop", 4) == 0) {
    p += 4;
    p = skip_whitespace(p);
    if (strncasecmp(p, "view", 4) == 0) {
      p += 4;
      out->type = STATEMENT_DROP_VIEW;
      p = parse_identifier(p, out->view_name, IDX_NAME_SIZE);
      return PREPARE_SUCCESS;
    }
    if (strncasecmp(p, "trigger", 7) == 0) {
      p += 7;
      out->type = STATEMENT_DROP_TRIGGER;
      p = parse_identifier(p, out->trigger_name, IDX_NAME_SIZE);
      return PREPARE_SUCCESS;
    }
    if (strncasecmp(p, "table", 5) == 0) {
      p += 5;
      p = skip_whitespace(p);
      out->type = STATEMENT_DROP_TABLE;
      p = parse_identifier(p, out->table_name, TBL_NAME_SIZE);
      if (strlen(out->table_name) == 0) return PREPARE_SYNTAX_ERROR;
      return PREPARE_SUCCESS;
    }
    return PREPARE_UNRECOGNIZED_STATEMENT;
  }

  if (strncasecmp(p, "begin", 5) == 0) {
    out->type = STATEMENT_BEGIN;
    return PREPARE_SUCCESS;
  }

  if (strncasecmp(p, "commit", 6) == 0) {
    out->type = STATEMENT_COMMIT;
    return PREPARE_SUCCESS;
  }

  if (strncasecmp(p, "savepoint", 9) == 0) {
    p += 9;
    out->type = STATEMENT_SAVEPOINT;
    p = parse_identifier(p, out->savepoint_name, sizeof(out->savepoint_name));
    if (strlen(out->savepoint_name) == 0) return PREPARE_SYNTAX_ERROR;
    return PREPARE_SUCCESS;
  }

  if (strncasecmp(p, "rollback", 8) == 0) {
    p += 8;
    p = skip_whitespace(p);
    if (strncasecmp(p, "to", 2) == 0) {
      p += 2;
      p = skip_whitespace(p);
      if (strncasecmp(p, "savepoint", 9) == 0) {
        p += 9;
      }
      out->type = STATEMENT_ROLLBACK_TO;
      p = parse_identifier(p, out->savepoint_name, sizeof(out->savepoint_name));
      if (strlen(out->savepoint_name) == 0) return PREPARE_SYNTAX_ERROR;
      return PREPARE_SUCCESS;
    }
    out->type = STATEMENT_ROLLBACK;
    return PREPARE_SUCCESS;
  }

  if (strncasecmp(p, "release", 7) == 0) {
    p += 7;
    p = skip_whitespace(p);
    if (strncasecmp(p, "savepoint", 9) == 0) {
      p += 9;
    }
    out->type = STATEMENT_RELEASE_SAVEPOINT;
    p = parse_identifier(p, out->savepoint_name, sizeof(out->savepoint_name));
    if (strlen(out->savepoint_name) == 0) return PREPARE_SYNTAX_ERROR;
    return PREPARE_SUCCESS;
  }

  if (strncasecmp(p, "vacuum", 6) == 0) {
    p += 6;
    p = skip_whitespace(p);
    out->type = STATEMENT_VACUUM;
    if (strncasecmp(p, "into", 4) == 0) {
      p += 4;
      p = skip_whitespace(p);
      p = parse_value_token(p, out->vacuum_into_filename, sizeof(out->vacuum_into_filename));
    }
    return PREPARE_SUCCESS;
  }

  if (strncasecmp(p, "analyze", 7) == 0) {
    p += 7;
    out->type = STATEMENT_ANALYZE;
    p = skip_whitespace(p);
    if (*p != '\0') {
      p = parse_identifier(p, out->table_name, TBL_NAME_SIZE);
    }
    return PREPARE_SUCCESS;
  }

  if (strncasecmp(p, "pragma", 6) == 0) {
    p += 6;
    p = skip_whitespace(p);
    out->type = STATEMENT_PRAGMA;
    p = parse_identifier(p, out->pragma_name, sizeof(out->pragma_name));
    p = skip_whitespace(p);
    if (*p == '=') {
      p++;
      p = skip_whitespace(p);
      p = parse_identifier(p, out->pragma_value, sizeof(out->pragma_value));
    }
    return PREPARE_SUCCESS;
  }

  if (strncasecmp(p, "reindex", 7) == 0) {
    p += 7;
    out->type = STATEMENT_REINDEX;
    p = parse_identifier(p, out->reindex_target, IDX_NAME_SIZE);
    return PREPARE_SUCCESS;
  }

  if (strncasecmp(p, "attach", 6) == 0) {
    p += 6;
    out->type = STATEMENT_ATTACH;
    p = skip_whitespace(p);
    if (strncasecmp(p, "database", 8) == 0) { p += 8; p = skip_whitespace(p); }
    if (*p == '\'') p++;
    int f_idx = 0;
    while (*p && *p != '\'' && f_idx < (int)sizeof(out->attach_filename) - 1) {
      out->attach_filename[f_idx++] = *p++;
    }
    out->attach_filename[f_idx] = '\0';
    if (*p == '\'') p++;
    p = skip_whitespace(p);
    if (strncasecmp(p, "as", 2) == 0) p += 2;
    p = parse_identifier(p, out->attach_alias, sizeof(out->attach_alias));
    return PREPARE_SUCCESS;
  }

  if (strncasecmp(p, "detach", 6) == 0) {
    p += 6;
    out->type = STATEMENT_DETACH;
    p = skip_whitespace(p);
    if (strncasecmp(p, "database", 8) == 0) { p += 8; p = skip_whitespace(p); }
    p = parse_identifier(p, out->attach_alias, sizeof(out->attach_alias));
    return PREPARE_SUCCESS;
  }

  if (strncasecmp(p, "help", 4) == 0 || strcmp(p, ".help") == 0) {
    out->type = STATEMENT_HELP;
    return PREPARE_SUCCESS;
  }

  return PREPARE_UNRECOGNIZED_STATEMENT;
}
