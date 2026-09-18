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

static const char* parse_order_by_expr(const char* p, char* dest, uint32_t max_len) {
  p = skip_whitespace(p);
  uint32_t len = 0;
  int paren_depth = 0;
  int bracket_depth = 0;
  bool in_single_quote = false;
  bool in_double_quote = false;

  while (*p) {
    if (!in_single_quote && !in_double_quote) {
      if (*p == '\'') { in_single_quote = true; }
      else if (*p == '"') { in_double_quote = true; }
      else if (*p == '(') { paren_depth++; }
      else if (*p == ')') { if (paren_depth > 0) paren_depth--; }
      else if (*p == '[') { bracket_depth++; }
      else if (*p == ']') { if (bracket_depth > 0) bracket_depth--; }
      else if (paren_depth == 0 && bracket_depth == 0) {
        if (*p == ',' || *p == ';') break;
        if (isspace((unsigned char)*p)) {
          const char* la = skip_whitespace(p);
          if (strncasecmp(la, "asc", 3) == 0 && (isspace((unsigned char)la[3]) || la[3] == ',' || la[3] == ';' || la[3] == '\0')) {
            break;
          }
          if (strncasecmp(la, "desc", 4) == 0 && (isspace((unsigned char)la[4]) || la[4] == ',' || la[4] == ';' || la[4] == '\0')) {
            break;
          }
          if (strncasecmp(la, "collate", 7) == 0 && (isspace((unsigned char)la[7]) || la[7] == '\0')) {
            break;
          }
          if (strncasecmp(la, "limit", 5) == 0 && (isspace((unsigned char)la[5]) || la[5] == '\0')) {
            break;
          }
          if (strncasecmp(la, "offset", 6) == 0 && (isspace((unsigned char)la[6]) || la[6] == '\0')) {
            break;
          }
        }
      }
    } else if (in_single_quote) {
      if (*p == '\'') in_single_quote = false;
    } else if (in_double_quote) {
      if (*p == '"') in_double_quote = false;
    }

    if (len < max_len - 1) {
      dest[len++] = *p;
    }
    p++;
  }
  while (len > 0 && isspace((unsigned char)dest[len - 1])) {
    len--;
  }
  dest[len] = '\0';
  return p;
}


/* Helper to parse a string value (handles single/double quotes, vector brackets, or unquoted strings) */
static const char* parse_value_token_ex(const char* p, char* dest, uint32_t max_len, bool* out_is_null, bool* out_was_quoted) {
  p = skip_whitespace(p);
  uint32_t len = 0;
  bool was_quoted = false;
  bool is_null = false;

  if ((*p == 'x' || *p == 'X') && (p[1] == '\'' || p[1] == '"')) {
    was_quoted = true;
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
    was_quoted = true;
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
  } else if (*p == '[') {
    /* Vector literal or JSON array: [1.0, 2.0, 3.0] */
    int bdepth = 0;
    while (*p) {
      if (*p == '[') bdepth++;
      else if (*p == ']') {
        bdepth--;
        if (len < max_len - 1) dest[len++] = *p;
        p++;
        if (bdepth <= 0) break;
        continue;
      }
      if (len < max_len - 1) dest[len++] = *p;
      p++;
    }
  } else {
    while (*p && !isspace((unsigned char)*p) && *p != ',' && *p != ')' && *p != '=' && *p != ';') {
      if (len < max_len - 1) {
        dest[len++] = *p;
      }
      p++;
    }
    if (len == 4 && strcasecmp(dest, "null") == 0) {
      is_null = true;
    }
  }
  dest[len] = '\0';
  if (out_is_null) *out_is_null = is_null;
  if (out_was_quoted) *out_was_quoted = was_quoted;
  return p;
}

static const char* parse_value_token(const char* p, char* dest, uint32_t max_len) {
  return parse_value_token_ex(p, dest, max_len, NULL, NULL);
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
    if (!*p || *p == ';') break;
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
      } else if (strncasecmp(p, "is not null", 11) == 0 && (isspace((unsigned char)p[11]) || p[11] == '\0' || p[11] == ')' || p[11] == ';')) {
        cond->op = OP_IS_NOT_NULL;
        p += 11;
      } else if (strncasecmp(p, "is null", 7) == 0 && (isspace((unsigned char)p[7]) || p[7] == '\0' || p[7] == ')' || p[7] == ';')) {
        cond->op = OP_IS_NULL;
        p += 7;
      } else if (strncasecmp(p, "match", 5) == 0 && (isspace((unsigned char)p[5]) || p[5] == '\0' || p[5] == '\'')) {
        cond->op = OP_MATCH;
        p += 5;
      } else if (strncasecmp(p, "not like", 8) == 0 && (isspace((unsigned char)p[8]) || p[8] == '\'' || p[8] == '"')) {
        cond->op = OP_NOT_LIKE;
        p += 8;
      } else if (strncasecmp(p, "not glob", 8) == 0 && (isspace((unsigned char)p[8]) || p[8] == '\'' || p[8] == '"')) {
        cond->op = OP_NOT_GLOB;
        p += 8;
      } else if (strncasecmp(p, "like", 4) == 0 && (isspace((unsigned char)p[4]) || p[4] == '\'' || p[4] == '"')) {
        cond->op = OP_LIKE;
        p += 4;
      } else if (strncasecmp(p, "glob", 4) == 0 && (isspace((unsigned char)p[4]) || p[4] == '\'' || p[4] == '"')) {
        cond->op = OP_GLOB;
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

            p = parse_value_token(p, cond->sub_where_val, sizeof(cond->sub_where_val));
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
      col->size = MAX_TEXT_SIZE;
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
        col->size = MAX_TEXT_SIZE; /* default size for TEXT / unsized VARCHAR */
      }
    } else if (strcasecmp(type_name, "VECTOR") == 0) {
      col->type = COL_VECTOR;
      p = skip_whitespace(p);
      if (*p == '(') {
        p++;
        char dim_str[16];
        p = parse_value_token(p, dim_str, sizeof(dim_str));
        uint32_t dim = (uint32_t)atoi(dim_str);
        if (dim == 0 || dim > 1024) dim = 128;
        col->size = dim * 16; /* allow ample text size for array string [x, y, z...] */
        if (col->size > MAX_TEXT_SIZE) col->size = MAX_TEXT_SIZE;
        p = skip_whitespace(p);
        if (*p == ')') p++;
      } else {
        col->size = 512;
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
        p = parse_value_token(p, col->default_val, sizeof(col->default_val));
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

        p = parse_value_token(p, col->check_val, sizeof(col->check_val));
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

  /* Handle EXPLAIN / EXPLAIN ANALYZE prefix */
  if (strncasecmp(p, "explain", 7) == 0 && isspace((unsigned char)p[7])) {
    p += 7;
    p = skip_whitespace(p);
    if (strncasecmp(p, "analyze", 7) == 0 && (p[7] == '\0' || isspace((unsigned char)p[7]))) {
      out->is_explain_analyze = true;
      p += 7;
      p = skip_whitespace(p);
    } else {
      out->is_explain = true;
    }
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
      cte->col_name[0] = '\0';
      if (*p == '(') {
        p++;
        p = parse_identifier(p, cte->col_name, COL_NAME_SIZE);
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
      memset(cte->cte_stmt, 0, sizeof(Statement));
      cte->rec_stmt = NULL;

      if (cte->is_recursive) {
        /* Search for UNION [ALL] */
        const char* u_ptr = NULL;
        for (const char* s = sub_buf; *s; s++) {
          if (strncasecmp(s, "union", 5) == 0 && (s == sub_buf || isspace((unsigned char)s[-1])) && isspace((unsigned char)s[5])) {
            u_ptr = s;
            break;
          }
        }
        if (u_ptr != NULL) {
          char anchor_buf[512] = {0};
          size_t a_len = u_ptr - sub_buf;
          if (a_len < sizeof(anchor_buf)) {
            memcpy(anchor_buf, sub_buf, a_len);
            anchor_buf[a_len] = '\0';
          }
          prepare_statement(anchor_buf, cte->cte_stmt);

          const char* rec_start = u_ptr + 5;
          rec_start = skip_whitespace(rec_start);
          if (strncasecmp(rec_start, "all", 3) == 0 && isspace((unsigned char)rec_start[3])) {
            rec_start += 3;
            rec_start = skip_whitespace(rec_start);
          }
          cte->rec_stmt = malloc(sizeof(Statement));
          memset(cte->rec_stmt, 0, sizeof(Statement));
          prepare_statement(rec_start, cte->rec_stmt);
        } else {
          prepare_statement(sub_buf, cte->cte_stmt);
        }
      } else {
        prepare_statement(sub_buf, cte->cte_stmt);
      }
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

    /* INSERT OR REPLACE / INSERT OR IGNORE */
    p = skip_whitespace(p);
    if (strncasecmp(p, "or", 2) == 0 && isspace((unsigned char)p[2])) {
      p += 2;
      p = skip_whitespace(p);
      if (strncasecmp(p, "replace", 7) == 0) {
        out->conflict_action = CONFLICT_REPLACE;
        p += 7;
      } else if (strncasecmp(p, "ignore", 6) == 0) {
        out->conflict_action = CONFLICT_IGNORE;
        p += 6;
      }
    }

    p = skip_whitespace(p);
    if (strncasecmp(p, "into", 4) == 0 && isspace((unsigned char)p[4])) {
      p += 4;
    }

    p = parse_identifier(p, out->table_name, TBL_NAME_SIZE);
    if (strlen(out->table_name) == 0) return PREPARE_SYNTAX_ERROR;

    /* Optional column list: INSERT INTO t (col1, col2) VALUES ... */
    p = skip_whitespace(p);
    char col_list[MAX_COLUMNS][COL_NAME_SIZE];
    uint32_t col_list_count = 0;
    if (*p == '(') {
      /* Peek ahead: if it looks like a SELECT subquery, skip col list */
      const char* peek = p + 1;
      while (*peek && isspace((unsigned char)*peek)) peek++;
      if (strncasecmp(peek, "select", 6) != 0) {
        p++; /* consume '(' */
        while (*p && *p != ')') {
          p = skip_whitespace(p);
          if (col_list_count < MAX_COLUMNS)
            p = parse_identifier(p, col_list[col_list_count++], COL_NAME_SIZE);
          p = skip_whitespace(p);
          if (*p == ',') p++;
        }
        if (*p == ')') p++;
        p = skip_whitespace(p);
      }
    }

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
      if (out->num_multi_rows >= MAX_MULTI_ROWS) return PREPARE_SYNTAX_ERROR;
      p = skip_whitespace(p);
      if (*p != '(') break;
      p++;

      uint32_t col_idx = 0;
      while (*p && *p != ')') {
        if (col_idx >= MAX_COLUMNS) return PREPARE_SYNTAX_ERROR;

        char tmp_val[MAX_RAW_VAL];
        bool val_is_null = false;
        p = parse_value_token_ex(p, tmp_val, MAX_RAW_VAL, &val_is_null, NULL);

        /* Map to column position via col_list if provided */
        uint32_t dest = col_idx;
        if (col_list_count > 0 && col_idx < col_list_count) {
          dest = col_idx; /* store in order; executor maps by col_list later */
        }
        snprintf(out->multi_raw_values[out->num_multi_rows][dest], MAX_RAW_VAL, "%s", tmp_val);
        out->multi_raw_is_null[out->num_multi_rows][dest] = val_is_null;
        if (out->num_multi_rows == 0) {
          snprintf(out->raw_values[dest], MAX_RAW_VAL, "%s", tmp_val);
          out->raw_is_null[dest] = val_is_null;
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
    if (out->num_values == 0) return PREPARE_SYNTAX_ERROR;

    /* ON CONFLICT DO UPDATE SET col = val, ... */
    p = skip_whitespace(p);
    if (strncasecmp(p, "on conflict", 11) == 0 && isspace((unsigned char)p[11])) {
      p += 11;
      p = skip_whitespace(p);
      if (strncasecmp(p, "do update", 9) == 0 && isspace((unsigned char)p[9])) {
        p += 9;
        p = skip_whitespace(p);
        if (strncasecmp(p, "set", 3) == 0 && isspace((unsigned char)p[3])) {
          p += 3;
          out->conflict_action = CONFLICT_UPDATE;
          out->num_set_pairs = 0;
          while (*p) {
            p = skip_whitespace(p);
            if (out->num_set_pairs >= MAX_COLUMNS) break;
            SetPair* pair = &out->set_pairs[out->num_set_pairs];
            p = parse_identifier(p, pair->col_name, COL_NAME_SIZE);
            if (strlen(pair->col_name) == 0) break;
            p = skip_whitespace(p);
            if (*p != '=') break;
            p++;
            p = parse_value_token_ex(p, pair->str_val, MAX_RAW_VAL, &pair->is_null, NULL);
            out->num_set_pairs++;
            p = skip_whitespace(p);
            if (*p == ',') p++;
            else break;
          }
        }
      } else if (strncasecmp(p, "do nothing", 10) == 0) {
        p += 10;
        out->conflict_action = CONFLICT_IGNORE;
      }
    }

    /* EXPIRES <seconds> */
    p = skip_whitespace(p);
    if (strncasecmp(p, "expires", 7) == 0 && isspace((unsigned char)p[7])) {
      p += 7;
      p = skip_whitespace(p);
      out->expires_sec = (uint32_t)atoi(p);
      while (*p && isdigit((unsigned char)*p)) p++;
    }

    /* RETURNING col, col2 | * */
    p = skip_whitespace(p);
    if (strncasecmp(p, "returning", 9) == 0 && (isspace((unsigned char)p[9]) || p[9] == '*')) {
      p += 9;
      out->has_returning = true;
      out->num_returning_cols = 0;
      p = skip_whitespace(p);
      if (*p == '*') {
        strncpy(out->returning_cols[0], "*", COL_NAME_SIZE - 1);
        out->num_returning_cols = 1;
        p++;
      } else {
        while (*p && *p != ';' && *p != '\0') {
          p = skip_whitespace(p);
          if (out->num_returning_cols >= MAX_SELECT_COLS) break;
          p = parse_identifier(p, out->returning_cols[out->num_returning_cols++], COL_NAME_SIZE);
          p = skip_whitespace(p);
          if (*p == ',') p++;
          else break;
        }
      }
    }

    /* Check EXPIRES after RETURNING if placed at end */
    p = skip_whitespace(p);
    if (strncasecmp(p, "expires", 7) == 0 && isspace((unsigned char)p[7])) {
      p += 7;
      p = skip_whitespace(p);
      out->expires_sec = (uint32_t)atoi(p);
      while (*p && isdigit((unsigned char)*p)) p++;
    }

    return PREPARE_SUCCESS;
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
      while (*p && *p != ';' && strncasecmp(p, "from", 4) != 0 &&
             strncasecmp(p, "union", 5) != 0 && strncasecmp(p, "intersect", 9) != 0 && strncasecmp(p, "except", 6) != 0) {
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
            sc->win_spec.win_func = WIN_COUNT;
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
          p = parse_value_token(p, sc->coalesce_default, sizeof(sc->coalesce_default));
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
              if (depth == 0 && (*p == ',' || *p == ';' || strncasecmp(p, "from", 4) == 0 ||
                                 strncasecmp(p, "union", 5) == 0 || strncasecmp(p, "intersect", 9) == 0 || strncasecmp(p, "except", 6) == 0)) break;
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
        } else if (*p != '\0' && *p != ';' && strncasecmp(p, "from", 4) != 0 &&
                   strncasecmp(p, "union", 5) != 0 && strncasecmp(p, "intersect", 9) != 0 && strncasecmp(p, "except", 6) != 0) {
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
      /* Check for trailing set operations on bare select */
      p = skip_whitespace(p);
      SetOp bare_set_op = SET_NONE;
      const char* bare_rhs = NULL;
      if (strncasecmp(p, "union", 5) == 0 && (isspace((unsigned char)p[5]) || p[5] == '\0')) {
        p += 5; p = skip_whitespace(p);
        if (strncasecmp(p, "all", 3) == 0 && (isspace((unsigned char)p[3]) || p[3] == '\0')) {
          bare_set_op = SET_UNION_ALL; p += 3;
        } else {
          bare_set_op = SET_UNION;
        }
        bare_rhs = p;
      } else if (strncasecmp(p, "intersect", 9) == 0 && (isspace((unsigned char)p[9]) || p[9] == '\0')) {
        bare_set_op = SET_INTERSECT; p += 9; bare_rhs = p;
      } else if (strncasecmp(p, "except", 6) == 0 && (isspace((unsigned char)p[6]) || p[6] == '\0')) {
        bare_set_op = SET_EXCEPT; p += 6; bare_rhs = p;
      }
      if (bare_set_op != SET_NONE && bare_rhs != NULL) {
        out->set_op = bare_set_op;
        out->set_rhs = malloc(sizeof(Statement));
        if (!out->set_rhs) return PREPARE_SYNTAX_ERROR;
        memset(out->set_rhs, 0, sizeof(Statement));
        PrepareResult pr = prepare_statement(bare_rhs, out->set_rhs);
        if (pr != PREPARE_SUCCESS) {
          free(out->set_rhs);
          out->set_rhs = NULL;
          return pr;
        }
      }
      return PREPARE_SUCCESS;
    }

    out->num_joins = 0;
    while (*p) {
      p = skip_whitespace(p);
      JoinType j_type = JOIN_INNER;
      bool is_join = false;
      if (strncasecmp(p, "left", 4) == 0) {
        j_type = JOIN_LEFT; p += 4; p = skip_whitespace(p);
        if (strncasecmp(p, "outer", 5) == 0) { p += 5; p = skip_whitespace(p); }
        if (strncasecmp(p, "join", 4) == 0) { p += 4; }
        is_join = true;
      } else if (strncasecmp(p, "right", 5) == 0) {
        j_type = JOIN_RIGHT; p += 5; p = skip_whitespace(p);
        if (strncasecmp(p, "outer", 5) == 0) { p += 5; p = skip_whitespace(p); }
        if (strncasecmp(p, "join", 4) == 0) { p += 4; }
        is_join = true;
      } else if (strncasecmp(p, "full", 4) == 0) {
        j_type = JOIN_FULL; p += 4; p = skip_whitespace(p);
        if (strncasecmp(p, "outer", 5) == 0) { p += 5; p = skip_whitespace(p); }
        if (strncasecmp(p, "join", 4) == 0) { p += 4; }
        is_join = true;
      } else if (strncasecmp(p, "inner", 5) == 0) {
        j_type = JOIN_INNER; p += 5; p = skip_whitespace(p);
        if (strncasecmp(p, "join", 4) == 0) { p += 4; }
        is_join = true;
      } else if (strncasecmp(p, "join", 4) == 0) {
        j_type = JOIN_INNER; p += 4;
        is_join = true;
      }

      if (!is_join) break;
      if (out->num_joins >= MAX_JOINS) return PREPARE_SYNTAX_ERROR;

      JoinItem* ji = &out->joins[out->num_joins++];
      ji->type = j_type;
      p = parse_identifier(p, ji->right_table, TBL_NAME_SIZE);
      if (strlen(ji->right_table) == 0) return PREPARE_SYNTAX_ERROR;

      p = skip_whitespace(p);
      if (strncasecmp(p, "on", 2) != 0) return PREPARE_SYNTAX_ERROR;
      p += 2;

      char left_tok[COL_NAME_SIZE];
      p = parse_identifier(p, left_tok, sizeof(left_tok));
      char* left_dot = strchr(left_tok, '.');
      snprintf(ji->left_col, COL_NAME_SIZE, "%s", left_dot ? left_dot + 1 : left_tok);

      p = skip_whitespace(p);
      if (*p != '=') return PREPARE_SYNTAX_ERROR;
      p++;

      char right_tok[COL_NAME_SIZE];
      p = parse_identifier(p, right_tok, sizeof(right_tok));
      char* right_dot = strchr(right_tok, '.');
      snprintf(ji->right_col, COL_NAME_SIZE, "%s", right_dot ? right_dot + 1 : right_tok);

      out->has_join = true;
      if (out->num_joins == 1) {
        out->join_clause.type = ji->type;
        strcpy(out->join_table_name, ji->right_table);
        strcpy(out->join_clause.left_col, ji->left_col);
        strcpy(out->join_clause.right_col, ji->right_col);
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

      p = parse_value_token(p, out->having_val, sizeof(out->having_val));
    }

    /* Parse ORDER BY */
    p = skip_whitespace(p);
    if (strncasecmp(p, "order by", 8) == 0) {
      p += 8;
      out->has_order_by = true;
      out->num_order_by = 0;
      while (*p) {
        p = skip_whitespace(p);
        if (*p == '\0' || *p == ';') break;
        if (out->num_order_by >= 4) break;
        OrderByItem* item = &out->order_by_items[out->num_order_by++];
        p = parse_order_by_expr(p, item->col_name, sizeof(item->col_name));
        if (strlen(item->col_name) == 0) return PREPARE_SYNTAX_ERROR;

        p = skip_whitespace(p);
        if (strncasecmp(p, "collate", 7) == 0 && (isspace((unsigned char)p[7]) || p[7] == '\0')) {
          p += 7;
          p = skip_whitespace(p);
          if (strncasecmp(p, "nocase", 6) == 0) { item->collation = COLL_NOCASE; p += 6; }
          else if (strncasecmp(p, "rtrim", 5) == 0) { item->collation = COLL_RTRIM; p += 5; }
          else if (strncasecmp(p, "binary", 6) == 0) { item->collation = COLL_BINARY; p += 6; }
        } else {
          item->collation = COLL_BINARY;
        }

        p = skip_whitespace(p);
        if (strncasecmp(p, "desc", 4) == 0 && (isspace((unsigned char)p[4]) || p[4] == ',' || p[4] == ';' || p[4] == '\0')) {
          item->is_desc = true;
          p += 4;
        } else if (strncasecmp(p, "asc", 3) == 0 && (isspace((unsigned char)p[3]) || p[3] == ',' || p[3] == ';' || p[3] == '\0')) {
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

    /* Set operators: UNION [ALL], INTERSECT, EXCEPT */
    p = skip_whitespace(p);
    SetOp detected_op = SET_NONE;
    const char* rhs_start = NULL;
    if (strncasecmp(p, "union", 5) == 0 && (isspace((unsigned char)p[5]) || p[5] == '\0')) {
      p += 5;
      p = skip_whitespace(p);
      if (strncasecmp(p, "all", 3) == 0 && (isspace((unsigned char)p[3]) || p[3] == '\0')) {
        detected_op = SET_UNION_ALL;
        p += 3;
      } else {
        detected_op = SET_UNION;
      }
      rhs_start = p;
    } else if (strncasecmp(p, "intersect", 9) == 0 && (isspace((unsigned char)p[9]) || p[9] == '\0')) {
      detected_op = SET_INTERSECT;
      p += 9;
      rhs_start = p;
    } else if (strncasecmp(p, "except", 6) == 0 && (isspace((unsigned char)p[6]) || p[6] == '\0')) {
      detected_op = SET_EXCEPT;
      p += 6;
      rhs_start = p;
    }

    if (detected_op != SET_NONE && rhs_start != NULL) {
      out->set_op = detected_op;
      out->set_rhs = malloc(sizeof(Statement));
      if (!out->set_rhs) return PREPARE_SYNTAX_ERROR;
      memset(out->set_rhs, 0, sizeof(Statement));
      PrepareResult pr = prepare_statement(rhs_start, out->set_rhs);
      if (pr != PREPARE_SUCCESS) {
        free(out->set_rhs);
        out->set_rhs = NULL;
        return pr;
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
      p = skip_whitespace(p);

      if (*p == '\'' || *p == '"') {
        p = parse_value_token_ex(p, pair->str_val, MAX_RAW_VAL, &pair->is_null, NULL);
        pair->is_null = false; /* explicitly quoted, cannot be SQL NULL */
      } else {
        uint32_t s_idx = 0;
        int depth = 0;
        while (*p && s_idx < MAX_RAW_VAL - 1) {
          if (*p == '(') depth++;
          else if (*p == ')') depth--;
          if (depth == 0) {
            if (*p == ',' || *p == ';' || strncasecmp(p, "where", 5) == 0) break;
          }
          pair->str_val[s_idx++] = *p++;
        }
        pair->str_val[s_idx] = '\0';
        while (s_idx > 0 && isspace((unsigned char)pair->str_val[s_idx - 1])) {
          pair->str_val[--s_idx] = '\0';
        }
        if (strcasecmp(pair->str_val, "null") == 0) {
          pair->is_null = true;
        } else {
          pair->is_null = false;
        }
      }
      out->num_set_pairs++;

      p = skip_whitespace(p);
      if (*p == ',') {
        p++;
      } else if (strncasecmp(p, "where", 5) != 0 && *p != '\0' && *p != ';') {
        return PREPARE_SYNTAX_ERROR;
      }
    }

    p = parse_where_clause(p, &out->where_clause);
    if (p == NULL) return PREPARE_SYNTAX_ERROR;

    /* RETURNING col, col2 | * */
    p = skip_whitespace(p);
    if (strncasecmp(p, "returning", 9) == 0 && (isspace((unsigned char)p[9]) || p[9] == '*')) {
      p += 9;
      out->has_returning = true;
      out->num_returning_cols = 0;
      p = skip_whitespace(p);
      if (*p == '*') {
        strncpy(out->returning_cols[0], "*", COL_NAME_SIZE - 1);
        out->num_returning_cols = 1;
        p++;
      } else {
        while (*p && *p != ';' && *p != '\0') {
          p = skip_whitespace(p);
          if (out->num_returning_cols >= MAX_SELECT_COLS) break;
          p = parse_identifier(p, out->returning_cols[out->num_returning_cols++], COL_NAME_SIZE);
          p = skip_whitespace(p);
          if (*p == ',') p++;
          else break;
        }
      }
    }

    p = skip_whitespace(p);
    if (*p == ';') p++;
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

    /* RETURNING col, col2 | * */
    p = skip_whitespace(p);
    if (strncasecmp(p, "returning", 9) == 0 && (isspace((unsigned char)p[9]) || p[9] == '*')) {
      p += 9;
      out->has_returning = true;
      out->num_returning_cols = 0;
      p = skip_whitespace(p);
      if (*p == '*') {
        strncpy(out->returning_cols[0], "*", COL_NAME_SIZE - 1);
        out->num_returning_cols = 1;
        p++;
      } else {
        while (*p && *p != ';' && *p != '\0') {
          p = skip_whitespace(p);
          if (out->num_returning_cols >= MAX_SELECT_COLS) break;
          p = parse_identifier(p, out->returning_cols[out->num_returning_cols++], COL_NAME_SIZE);
          p = skip_whitespace(p);
          if (*p == ',') p++;
          else break;
        }
      }
    }

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
      p = parse_identifier(p, out->view_name, TBL_NAME_SIZE);
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
      p = parse_identifier(p, out->trigger_name, TBL_NAME_SIZE);
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

      /* Find closing parenthesis of columns to check for WITH TTL = <sec> */
      const char* post = skip_whitespace(p);
      if (*post == '(') {
        int depth = 1;
        post++;
        while (*post && depth > 0) {
          if (*post == '(') depth++;
          else if (*post == ')') depth--;
          post++;
        }
      }
      while (*post) {
        post = skip_whitespace(post);
        if (*post == ';' || *post == '\0') break;
        if (*post == ',') { post++; post = skip_whitespace(post); }

        if (strncasecmp(post, "with", 4) == 0 && isspace((unsigned char)post[4])) {
          post += 4;
          post = skip_whitespace(post);
        }

        if (strncasecmp(post, "ttl", 3) == 0 && (isspace((unsigned char)post[3]) || post[3] == '=')) {
          post += 3;
          post = skip_whitespace(post);
          if (*post == '=') { post++; post = skip_whitespace(post); }
          out->new_table.default_ttl = (uint32_t)atoi(post);
          while (isdigit((unsigned char)*post)) post++;
        } else if (strncasecmp(post, "history", 7) == 0 && (isspace((unsigned char)post[7]) || post[7] == ';' || post[7] == ',' || post[7] == '\0')) {
          out->new_table.with_history = true;
          post += 7;
        } else {
          break;
        }
      }

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
      p = parse_identifier(p, out->view_name, TBL_NAME_SIZE);
      return PREPARE_SUCCESS;
    }
    if (strncasecmp(p, "trigger", 7) == 0) {
      p += 7;
      out->type = STATEMENT_DROP_TRIGGER;
      p = parse_identifier(p, out->trigger_name, TBL_NAME_SIZE);
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

  if (strncasecmp(p, "backup", 6) == 0 && (isspace((unsigned char)p[6]) || p[6] == '\0')) {
    p += 6;
    p = skip_whitespace(p);
    if (strncasecmp(p, "database", 8) == 0 && (isspace((unsigned char)p[8]) || p[8] == '\0')) {
      p += 8;
      p = skip_whitespace(p);
    }
    if (strncasecmp(p, "to", 2) == 0 && (isspace((unsigned char)p[2]) || p[2] == '\0')) {
      p += 2;
      p = skip_whitespace(p);
    } else {
      return PREPARE_SYNTAX_ERROR;
    }
    out->type = STATEMENT_BACKUP;
    p = parse_value_token(p, out->pitr_dest, sizeof(out->pitr_dest));
    if (strlen(out->pitr_dest) == 0) return PREPARE_SYNTAX_ERROR;
    return PREPARE_SUCCESS;
  }

  if (strncasecmp(p, "restore", 7) == 0 && (isspace((unsigned char)p[7]) || p[7] == '\0')) {
    p += 7;
    p = skip_whitespace(p);
    if (strncasecmp(p, "database", 8) == 0 && (isspace((unsigned char)p[8]) || p[8] == '\0')) {
      p += 8;
      p = skip_whitespace(p);
    }
    if (strncasecmp(p, "from", 4) == 0 && (isspace((unsigned char)p[4]) || p[4] == '\0')) {
      p += 4;
      p = skip_whitespace(p);
    } else {
      return PREPARE_SYNTAX_ERROR;
    }
    out->type = STATEMENT_RESTORE;
    p = parse_value_token(p, out->pitr_src, sizeof(out->pitr_src));
    if (strlen(out->pitr_src) == 0) return PREPARE_SYNTAX_ERROR;
    p = skip_whitespace(p);

    if (strncasecmp(p, "until", 5) == 0 && (isspace((unsigned char)p[5]) || p[5] == '\0')) {
      p += 5;
      p = skip_whitespace(p);
      if (strncasecmp(p, "timestamp", 9) == 0 && (isspace((unsigned char)p[9]) || p[9] == '\0')) {
        p += 9;
        p = skip_whitespace(p);
        char ts_str[64] = {0};
        p = parse_value_token(p, ts_str, sizeof(ts_str));
        out->pitr_until_ts = (uint64_t)strtoull(ts_str, NULL, 10);
        out->pitr_use_ts = true;
      } else if (strncasecmp(p, "lsn", 3) == 0 && (isspace((unsigned char)p[3]) || p[3] == '\0')) {
        p += 3;
        p = skip_whitespace(p);
        char lsn_str[64] = {0};
        p = parse_value_token(p, lsn_str, sizeof(lsn_str));
        out->pitr_until_lsn = (uint64_t)strtoull(lsn_str, NULL, 10);
        out->pitr_use_lsn = true;
      } else {
        return PREPARE_SYNTAX_ERROR;
      }
      p = skip_whitespace(p);
    }

    if (strncasecmp(p, "to", 2) == 0 && (isspace((unsigned char)p[2]) || p[2] == '\0')) {
      p += 2;
      p = skip_whitespace(p);
      p = parse_value_token(p, out->pitr_dest, sizeof(out->pitr_dest));
      if (strlen(out->pitr_dest) == 0) return PREPARE_SYNTAX_ERROR;
    } else {
      snprintf(out->pitr_dest, sizeof(out->pitr_dest), "%.240s_restored.db", out->pitr_src);
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
    } else if (*p == '(') {
      p++;
      p = skip_whitespace(p);
      if (*p == '\'' || *p == '"') {
        p = parse_value_token(p, out->pragma_value, sizeof(out->pragma_value));
      } else {
        p = parse_identifier(p, out->pragma_value, sizeof(out->pragma_value));
      }
      p = skip_whitespace(p);
      if (*p == ')') p++;
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

void statement_free_children(Statement* stmt) {
  if (!stmt) return;
  for (uint32_t c = 0; c < stmt->num_ctes; c++) {
    if (stmt->ctes[c].cte_stmt) {
      statement_free_children(stmt->ctes[c].cte_stmt);
      free(stmt->ctes[c].cte_stmt);
      stmt->ctes[c].cte_stmt = NULL;
    }
    if (stmt->ctes[c].rec_stmt) {
      statement_free_children(stmt->ctes[c].rec_stmt);
      free(stmt->ctes[c].rec_stmt);
      stmt->ctes[c].rec_stmt = NULL;
    }
  }
  if (stmt->insert_select_stmt) {
    statement_free_children(stmt->insert_select_stmt);
    free(stmt->insert_select_stmt);
    stmt->insert_select_stmt = NULL;
  }
  if (stmt->set_rhs) {
    statement_free_children(stmt->set_rhs);
    free(stmt->set_rhs);
    stmt->set_rhs = NULL;
  }
}
