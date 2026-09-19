#include "catalog.h"
#include <stdlib.h>
#include <string.h>

/* ── Value Lifecycle Functions ───────────────────────────────────────────── */
static char s_empty_str[] = "";

void value_init(Value* v) {
  if (!v) return;
  memset(v, 0, sizeof(Value));
  v->text_val = s_empty_str;
}

void value_free(Value* v) {
  if (!v) return;
  if (v->text_val && v->text_val != s_empty_str) {
    free(v->text_val);
  }
  v->text_val = s_empty_str;
  v->text_len = 0;
  v->text_cap = 0;
}

void value_free_row(Value* values, uint32_t count) {
  if (!values) return;
  for (uint32_t i = 0; i < count; i++) {
    value_free(&values[i]);
  }
}

void value_set_text_len(Value* v, const char* str, uint32_t len) {
  if (!v) return;
  if (str == NULL) {
    value_free(v);
    v->is_null = true;
    return;
  }
  if (v->text_cap < len + 1 || v->text_val == NULL || v->text_val == s_empty_str) {
    char* new_buf = malloc(len + 1);
    if (!new_buf) return;
    if (v->text_val && v->text_val != s_empty_str) free(v->text_val);
    v->text_val = new_buf;
    v->text_cap = len + 1;
  }
  if (str && len > 0) memcpy(v->text_val, str, len);
  v->text_val[len] = '\0';
  v->text_len = len;
  v->is_null = false;
}

void value_set_text(Value* v, const char* str) {
  if (!v) return;
  if (str == NULL) {
    value_free(v);
    v->is_null = true;
    return;
  }
  value_set_text_len(v, str, (uint32_t)strlen(str));
}

void value_copy(Value* dst, const Value* src) {
  if (!dst || !src) return;
  if (dst == src) return;
  value_free(dst);
  *dst = *src;
  dst->text_val = s_empty_str;
  dst->text_cap = 0;
  dst->text_len = 0;
  if (src->text_val && src->text_val != s_empty_str && !src->is_null) {
    value_set_text_len(dst, src->text_val, src->text_len);
  }
}

void value_move(Value* dst, Value* src) {
  if (!dst || !src) return;
  if (dst == src) return;
  value_free(dst);
  *dst = *src;
  value_init(src);
}

/* ── On-disk catalog entry layout ────────────────────────────────────────── */
#define DISK_COL_SIZE    200u
#define VTAB_BASE_OFFSET (IDX_NAME_SIZE + 4u + 4u + MAX_COLUMNS * DISK_COL_SIZE)
#define DISK_TTL_OFFSET     (VTAB_BASE_OFFSET + 1u + 64u + 256u)
#define DISK_HISTORY_OFFSET (DISK_TTL_OFFSET + 4u)
#define DISK_ENTRY_SIZE     (DISK_HISTORY_OFFSET + 1u)

/* ── Computed fields ─────────────────────────────────────────────────────── */
void tabledef_compute(TableDef* def) {
  uint32_t off = 0;
  for (uint32_t i = 0; i < def->num_cols; i++) {
    def->col_offsets[i] = off;
    off += def->columns[i].size;
  }
  def->row_size = off;
}

uint32_t catalog_get_reserved_pages(uint32_t num_tables) {
  (void)num_tables;
  return 33;
}

void catalog_load(Catalog* catalog, Pager* pager) {
  uint8_t* page0 = (uint8_t*)get_page(pager, 0);

  uint32_t num;
  memcpy(&num, page0, 4);
  catalog->num_tables = (num <= MAX_TABLES) ? num : 0;
  pager->reserved_catalog_pages = 33;
  memcpy(&pager->freelist_head, page0 + 4092, 4);
  uint64_t saved_lsn = 0;
  memcpy(&saved_lsn, page0 + 4084, 8);
  if (saved_lsn > pager->wal_lsn) {
    pager->wal_lsn = saved_lsn;
  }

  for (uint32_t t = 0; t < catalog->num_tables; t++) {
    uint32_t page_num = (t == 0) ? 0 : (t < 15 ? t : t + 1);
    uint32_t page_offset = (t == 0) ? 4 : 0;

    uint8_t* page = (uint8_t*)get_page(pager, page_num);
    uint8_t* base = page + page_offset;
    TableDef* def = &catalog->tables[t];
    memset(def, 0, sizeof(TableDef));

    memcpy(def->name,          base,              IDX_NAME_SIZE);
    def->name[IDX_NAME_SIZE - 1] = '\0';
    memcpy(&def->root_page_num, base + IDX_NAME_SIZE, 4);
    memcpy(&def->num_cols,      base + IDX_NAME_SIZE + 4, 4);
    if (def->num_cols > MAX_COLUMNS) def->num_cols = 0;

    for (uint32_t c = 0; c < def->num_cols; c++) {
      uint8_t* col_base = base + IDX_NAME_SIZE + 8 + c * DISK_COL_SIZE;
      Column* col = &def->columns[c];
      memset(col, 0, sizeof(Column));
      uint32_t off = 0;

      memcpy(col->name,  col_base + off, COL_NAME_SIZE); col->name[COL_NAME_SIZE - 1] = '\0'; off += COL_NAME_SIZE;
      memcpy(&col->type, col_base + off, 4); off += 4;
      memcpy(&col->size, col_base + off, 4); off += 4;
      uint32_t idx_flag = 0;
      memcpy(&idx_flag, col_base + off, 4); off += 4;
      col->has_index = (idx_flag != 0);
      memcpy(&col->index_root_page, col_base + off, 4); off += 4;

      uint32_t flags = 0;
      memcpy(&flags, col_base + off, 4); off += 4;
      col->is_not_null = (flags & 1) != 0;
      col->is_unique   = (flags & 2) != 0;
      col->has_default = (flags & 4) != 0;
      col->has_check   = (flags & 8) != 0;
      col->has_fk      = (flags & 16) != 0;
      col->is_autoincrement = (flags & 32) != 0;
      col->collation   = (CollationType)((flags >> 6) & 3);
      col->fk_on_delete_cascade = (flags & 256) != 0;
      col->fk_on_update_cascade = (flags & 512) != 0;
      col->idx_is_partial       = (flags & 1024) != 0;
      col->idx_is_expr          = (flags & 2048) != 0;

      memcpy(&col->check_op, col_base + off, 4); off += 4;
      memcpy(col->default_val, col_base + off, 16); col->default_val[15] = '\0'; off += 16;
      memcpy(col->check_val, col_base + off, 16); col->check_val[15] = '\0'; off += 16;
      memcpy(col->fk_target_table, col_base + off, 16); col->fk_target_table[15] = '\0'; off += 16;
      memcpy(col->fk_target_col, col_base + off, 16); col->fk_target_col[15] = '\0'; off += 16;
      memcpy(col->idx_where_col, col_base + off, 16); col->idx_where_col[15] = '\0'; off += 16;
      memcpy(&col->idx_where_op, col_base + off, 4); off += 4;
      memcpy(col->idx_where_val, col_base + off, 16); col->idx_where_val[15] = '\0'; off += 16;
      memcpy(col->idx_expr_func, col_base + off, 16); col->idx_expr_func[15] = '\0'; off += 16;
    }

    uint8_t is_v = 0;
    memcpy(&is_v, base + VTAB_BASE_OFFSET, 1);
    def->is_virtual = (is_v == 1);
    memcpy(def->vtab_module, base + VTAB_BASE_OFFSET + 1, 64); def->vtab_module[63] = '\0';
    memcpy(def->vtab_args, base + VTAB_BASE_OFFSET + 65, 256); def->vtab_args[255] = '\0';
    memcpy(&def->default_ttl, base + DISK_TTL_OFFSET, 4);
    uint8_t is_h = 0;
    memcpy(&is_h, base + DISK_HISTORY_OFFSET, 1);
    def->with_history = (is_h == 1);
    tabledef_compute(def);
  }

  uint8_t* vt_page_ptr = (uint8_t*)get_page(pager, 15);
  memcpy(&catalog->num_views, vt_page_ptr, 4);
  if (catalog->num_views > 4) catalog->num_views = 0;
  memcpy(catalog->views, vt_page_ptr + 4, sizeof(ViewDef) * catalog->num_views);

  memcpy(&catalog->num_triggers, vt_page_ptr + 4 + sizeof(ViewDef) * 4, 4);
  if (catalog->num_triggers > 4) catalog->num_triggers = 0;
  memcpy(catalog->triggers, vt_page_ptr + 8 + sizeof(ViewDef) * 4, sizeof(TriggerDef) * catalog->num_triggers);

  uint8_t av_byte = 0;
  memcpy(&av_byte, vt_page_ptr + 500, 1);
  pager->auto_vacuum = (av_byte == 1);
}

void catalog_save(Catalog* catalog, Pager* pager) {
  /* Refuse to write if a lock could not be acquired — safer than corrupting data */
  if (pager->lock_error) {
    fprintf(stderr, "Error: catalog_save aborted — database is locked.\n");
    return;
  }
  pager->reserved_catalog_pages = 33;

  pager_shadow_page_write(pager, 0);
  uint8_t* page0 = (uint8_t*)get_page(pager, 0);
  memcpy(page0, &catalog->num_tables, 4);

  for (uint32_t t = 0; t < catalog->num_tables; t++) {
    uint32_t page_num = (t == 0) ? 0 : (t < 15 ? t : t + 1);
    uint32_t page_offset = (t == 0) ? 4 : 0;

    pager_shadow_page_write(pager, page_num);
    uint8_t* page = (uint8_t*)get_page(pager, page_num);
    uint8_t* base = page + page_offset;
    TableDef* def = &catalog->tables[t];

    memcpy(base,                   def->name,          IDX_NAME_SIZE);
    memcpy(base + IDX_NAME_SIZE,   &def->root_page_num, 4);
    memcpy(base + IDX_NAME_SIZE+4, &def->num_cols,      4);

    for (uint32_t c = 0; c < def->num_cols; c++) {
      uint8_t* col_base = base + IDX_NAME_SIZE + 8 + c * DISK_COL_SIZE;
      Column* col = &def->columns[c];
      uint32_t off = 0;

      memcpy(col_base + off, col->name, COL_NAME_SIZE); off += COL_NAME_SIZE;
      memcpy(col_base + off, &col->type, 4); off += 4;
      memcpy(col_base + off, &col->size, 4); off += 4;
      uint32_t idx_flag = col->has_index ? 1 : 0;
      memcpy(col_base + off, &idx_flag, 4); off += 4;
      memcpy(col_base + off, &col->index_root_page, 4); off += 4;

      uint32_t flags = (col->is_not_null ? 1 : 0) |
                       (col->is_unique   ? 2 : 0) |
                       (col->has_default ? 4 : 0) |
                       (col->has_check   ? 8 : 0) |
                       (col->has_fk      ? 16 : 0) |
                       (col->is_autoincrement ? 32 : 0) |
                       ((col->collation & 3) << 6) |
                       (col->fk_on_delete_cascade ? 256 : 0) |
                       (col->fk_on_update_cascade ? 512 : 0) |
                       (col->idx_is_partial ? 1024 : 0) |
                       (col->idx_is_expr    ? 2048 : 0);
      memcpy(col_base + off, &flags, 4); off += 4;
      memcpy(col_base + off, &col->check_op, 4); off += 4;
      memcpy(col_base + off, col->default_val, 16); off += 16;
      memcpy(col_base + off, col->check_val, 16); off += 16;
      memcpy(col_base + off, col->fk_target_table, 16); off += 16;
      memcpy(col_base + off, col->fk_target_col, 16); off += 16;
      memcpy(col_base + off, col->idx_where_col, 16); off += 16;
      memcpy(col_base + off, &col->idx_where_op, 4); off += 4;
      memcpy(col_base + off, col->idx_where_val, 16); off += 16;
      memcpy(col_base + off, col->idx_expr_func, 16); off += 16;
    }

    uint8_t is_v = def->is_virtual ? 1 : 0;
    memcpy(base + VTAB_BASE_OFFSET, &is_v, 1);
    memcpy(base + VTAB_BASE_OFFSET + 1, def->vtab_module, 64);
    memcpy(base + VTAB_BASE_OFFSET + 65, def->vtab_args, 256);
    memcpy(base + DISK_TTL_OFFSET, &def->default_ttl, 4);
    uint8_t is_h = def->with_history ? 1 : 0;
    memcpy(base + DISK_HISTORY_OFFSET, &is_h, 1);
  }

  pager_shadow_page_write(pager, 15);
  uint8_t* vt_page_ptr = (uint8_t*)get_page(pager, 15);
  memcpy(vt_page_ptr, &catalog->num_views, 4);
  memcpy(vt_page_ptr + 4, catalog->views, sizeof(ViewDef) * 4);

  memcpy(vt_page_ptr + 4 + sizeof(ViewDef) * 4, &catalog->num_triggers, 4);
  memcpy(vt_page_ptr + 8 + sizeof(ViewDef) * 4, catalog->triggers, sizeof(TriggerDef) * 4);

  uint8_t av_byte = pager->auto_vacuum ? 1 : 0;
  memcpy(vt_page_ptr + 500, &av_byte, 1);

  uint8_t* p0 = (uint8_t*)get_page(pager, 0);
  memcpy(p0 + 4092, &pager->freelist_head, 4);
  memcpy(p0 + 4084, &pager->wal_lsn, 8);

  if (!pager->use_wal) {
    for (uint32_t p = 0; p < pager->max_pages; p++) {
      if (pager->pages[p] != NULL) {
        pager_flush(pager, p);
      }
    }
  }
}

TableDef* catalog_find(Catalog* catalog, const char* name) {
  for (uint32_t i = 0; i < catalog->num_tables; i++) {
    if (strcmp(catalog->tables[i].name, name) == 0)
      return &catalog->tables[i];
  }
  return NULL;
}

ViewDef* catalog_find_view(Catalog* catalog, const char* name) {
  for (uint32_t i = 0; i < catalog->num_views; i++) {
    if (strcmp(catalog->views[i].view_name, name) == 0)
      return &catalog->views[i];
  }
  return NULL;
}

static uint32_t put_varint(uint8_t* p, uint64_t v) {
  uint32_t i = 0;
  while (v >= 0x80) {
    p[i++] = (uint8_t)((v & 0x7f) | 0x80);
    v >>= 7;
  }
  p[i++] = (uint8_t)(v & 0x7f);
  return i;
}

static uint32_t get_varint(const uint8_t* p, uint64_t* v) {
  uint64_t result = 0;
  uint32_t i = 0;
  while (i < 10) {
    uint8_t b = p[i++];
    result |= ((uint64_t)(b & 0x7f)) << (7 * (i - 1));
    if (!(b & 0x80)) break;
  }
  *v = result;
  return i;
}

uint32_t serialize_row_with_mvcc(TableDef* def, Value* values, uint64_t expire_at, uint64_t xmin, uint64_t xmax, void* dest) {
  uint8_t* out = (uint8_t*)dest;
  
  uint8_t hdr_buf[PAGE_SIZE];
  uint32_t hdr_len = 0;
  
  uint8_t stack_body[16384];
  uint8_t* ser_body = stack_body;
  uint32_t ser_body_cap = sizeof(stack_body);
  uint32_t body_len = 0;

  for (uint32_t i = 0; i < def->num_cols; i++) {
    Column* col = &def->columns[i];
    uint64_t serial_type = 0;

    if (values[i].is_null) {
      serial_type = 0;
    } else {
      switch (col->type) {
        case COL_INT: {
          int32_t val = values[i].int_val;
          if (val == 0) {
            serial_type = 8;
          } else if (val == 1) {
            serial_type = 9;
          } else if (val >= -128 && val <= 127) {
            serial_type = 1;
            if (body_len + 1 > ser_body_cap) {
              uint32_t new_cap = (body_len + 1 + 4096) * 2;
              uint8_t* new_buf = (ser_body == stack_body) ? malloc(new_cap) : realloc(ser_body, new_cap);
              if (new_buf) {
                if (ser_body == stack_body) memcpy(new_buf, stack_body, body_len);
                ser_body = new_buf;
                ser_body_cap = new_cap;
              }
            }
            ser_body[body_len++] = (uint8_t)val;
          } else if (val >= -32768 && val <= 32767) {
            serial_type = 2;
            int16_t short_val = (int16_t)val;
            if (body_len + 2 > ser_body_cap) {
              uint32_t new_cap = (body_len + 2 + 4096) * 2;
              uint8_t* new_buf = (ser_body == stack_body) ? malloc(new_cap) : realloc(ser_body, new_cap);
              if (new_buf) {
                if (ser_body == stack_body) memcpy(new_buf, stack_body, body_len);
                ser_body = new_buf;
                ser_body_cap = new_cap;
              }
            }
            memcpy(ser_body + body_len, &short_val, 2);
            body_len += 2;
          } else {
            serial_type = 4;
            if (body_len + 4 > ser_body_cap) {
              uint32_t new_cap = (body_len + 4 + 4096) * 2;
              uint8_t* new_buf = (ser_body == stack_body) ? malloc(new_cap) : realloc(ser_body, new_cap);
              if (new_buf) {
                if (ser_body == stack_body) memcpy(new_buf, stack_body, body_len);
                ser_body = new_buf;
                ser_body_cap = new_cap;
              }
            }
            memcpy(ser_body + body_len, &val, 4);
            body_len += 4;
          }
          break;
        }
        case COL_BOOL: {
          bool val = values[i].bool_val;
          serial_type = val ? 9 : 8;
          break;
        }
        case COL_FLOAT: {
          serial_type = 7;
          double val = (values[i].double_val != 0.0) ? values[i].double_val : (double)values[i].float_val;
          if (body_len + 8 > ser_body_cap) {
            uint32_t new_cap = (body_len + 8 + 4096) * 2;
            uint8_t* new_buf = (ser_body == stack_body) ? malloc(new_cap) : realloc(ser_body, new_cap);
            if (new_buf) {
              if (ser_body == stack_body) memcpy(new_buf, stack_body, body_len);
              ser_body = new_buf;
              ser_body_cap = new_cap;
            }
          }
          memcpy(ser_body + body_len, &val, 8);
          body_len += 8;
          break;
        }
        case COL_DOUBLE:
        case COL_NUMERIC:
        case COL_DECIMAL: {
          serial_type = 7;
          double val = values[i].double_val;
          if (body_len + 8 > ser_body_cap) {
            uint32_t new_cap = (body_len + 8 + 4096) * 2;
            uint8_t* new_buf = (ser_body == stack_body) ? malloc(new_cap) : realloc(ser_body, new_cap);
            if (new_buf) {
              if (ser_body == stack_body) memcpy(new_buf, stack_body, body_len);
              ser_body = new_buf;
              ser_body_cap = new_cap;
            }
          }
          memcpy(ser_body + body_len, &val, 8);
          body_len += 8;
          break;
        }
        case COL_BLOB:
        case COL_DATETIME:
        case COL_DATE:
        case COL_TIME:
        case COL_TIMESTAMP:
        case COL_TEXT:
        case COL_VARCHAR:
        case COL_VECTOR: {
          const char* str = values[i].text_val ? values[i].text_val : "";
          uint32_t len = values[i].text_len > 0 ? values[i].text_len : (uint32_t)strlen(str);
          serial_type = (col->type == COL_BLOB) ? (12 + 2 * (uint64_t)len) : (13 + 2 * (uint64_t)len);
          if (body_len + len > ser_body_cap) {
            uint32_t new_cap = (body_len + len + 4096) * 2;
            uint8_t* new_buf = (ser_body == stack_body) ? malloc(new_cap) : realloc(ser_body, new_cap);
            if (new_buf) {
              if (ser_body == stack_body) memcpy(new_buf, stack_body, body_len);
              ser_body = new_buf;
              ser_body_cap = new_cap;
            }
          }
          if (len > 0) memcpy(ser_body + body_len, str, len);
          body_len += len;
          break;
        }
      }
    }
    
    if (hdr_len + 16 <= sizeof(hdr_buf)) {
      hdr_len += put_varint(hdr_buf + hdr_len, serial_type);
    }
  }

  /* If expire_at > 0, append a special varint marker (10 = TTL present) and body */
  if (expire_at > 0) {
    if (hdr_len + 16 <= sizeof(hdr_buf)) {
      hdr_len += put_varint(hdr_buf + hdr_len, SERIAL_TYPE_TTL); /* 10 = TTL 8-byte uint64 follows in body */
    }
    if (body_len + 8 > ser_body_cap) {
      uint32_t new_cap = (body_len + 8 + 4096) * 2;
      uint8_t* new_buf = (ser_body == stack_body) ? malloc(new_cap) : realloc(ser_body, new_cap);
      if (new_buf) {
        if (ser_body == stack_body) memcpy(new_buf, stack_body, body_len);
        ser_body = new_buf;
        ser_body_cap = new_cap;
      }
    }
    memcpy(ser_body + body_len, &expire_at, 8);
    body_len += 8;
  }

  /* If xmin > 0 or xmax > 0, append varint markers and bodies for both */
  if (xmin > 0 || xmax > 0) {
    if (hdr_len + 16 <= sizeof(hdr_buf)) {
      hdr_len += put_varint(hdr_buf + hdr_len, SERIAL_TYPE_XMIN);
    }
    if (body_len + 8 > ser_body_cap) {
      uint32_t new_cap = (body_len + 8 + 4096) * 2;
      uint8_t* new_buf = (ser_body == stack_body) ? malloc(new_cap) : realloc(ser_body, new_cap);
      if (new_buf) {
        if (ser_body == stack_body) memcpy(new_buf, stack_body, body_len);
        ser_body = new_buf;
        ser_body_cap = new_cap;
      }
    }
    memcpy(ser_body + body_len, &xmin, 8);
    body_len += 8;

    if (hdr_len + 16 <= sizeof(hdr_buf)) {
      hdr_len += put_varint(hdr_buf + hdr_len, SERIAL_TYPE_XMAX);
    }
    if (body_len + 8 > ser_body_cap) {
      uint32_t new_cap = (body_len + 8 + 4096) * 2;
      uint8_t* new_buf = (ser_body == stack_body) ? malloc(new_cap) : realloc(ser_body, new_cap);
      if (new_buf) {
        if (ser_body == stack_body) memcpy(new_buf, stack_body, body_len);
        ser_body = new_buf;
        ser_body_cap = new_cap;
      }
    }
    memcpy(ser_body + body_len, &xmax, 8);
    body_len += 8;
  }

  /* Compute and write header size varint (including its own size) */
  uint32_t total_hdr_size = hdr_len + 1;
  while (1) {
    uint8_t temp[16];
    uint32_t vlen = put_varint(temp, total_hdr_size);
    if (vlen + hdr_len == total_hdr_size) break;
    total_hdr_size = vlen + hdr_len;
  }

  uint32_t p = put_varint(out, total_hdr_size);
  memcpy(out + p, hdr_buf, hdr_len);
  p += hdr_len;
  
  if (body_len > 0) {
    memcpy(out + p, ser_body, body_len);
    p += body_len;
  }

  if (ser_body != stack_body) {
    free(ser_body);
  }

  return p;
}

uint32_t serialize_row_with_ttl(TableDef* def, Value* values, uint64_t expire_at, void* dest) {
  return serialize_row_with_mvcc(def, values, expire_at, 0, 0, dest);
}

uint32_t serialize_row(TableDef* def, Value* values, void* dest) {
  return serialize_row_with_mvcc(def, values, 0, 0, 0, dest);
}

uint32_t deserialize_row_with_mvcc(TableDef* def, void* src, Value* values, uint64_t* out_expire_at, uint64_t* out_xmin, uint64_t* out_xmax) {
  const uint8_t* in = (const uint8_t*)src;
  
  uint64_t total_hdr_size = 0;
  uint32_t p = get_varint(in, &total_hdr_size);
  
  uint32_t hdr_start = p;
  uint32_t body_offset = (uint32_t)total_hdr_size;
  
  uint32_t cur_hdr = hdr_start;
  uint32_t cur_body = body_offset;

  for (uint32_t i = 0; i < def->num_cols; i++) {
    Column* col = &def->columns[i];
    uint64_t serial_type = 0;
    cur_hdr += get_varint(in + cur_hdr, &serial_type);
    
    if (values) value_init(&values[i]);

    if (serial_type == 0) {
      /* NULL value */
      if (values) values[i].is_null = true;
    } else if (serial_type == 1) {
      /* 8-bit signed int */
      int8_t val = (int8_t)in[cur_body++];
      if (values) {
        if (col->type == COL_DOUBLE || col->type == COL_NUMERIC || col->type == COL_DECIMAL) values[i].double_val = val;
        else if (col->type == COL_FLOAT) values[i].float_val = val;
        else values[i].int_val = val;
      }
    } else if (serial_type == 2) {
      /* 16-bit signed int */
      int16_t val;
      memcpy(&val, in + cur_body, 2);
      cur_body += 2;
      if (values) {
        if (col->type == COL_DOUBLE || col->type == COL_NUMERIC || col->type == COL_DECIMAL) values[i].double_val = val;
        else if (col->type == COL_FLOAT) values[i].float_val = val;
        else values[i].int_val = val;
      }
    } else if (serial_type == 4) {
      /* 32-bit signed int */
      int32_t val;
      memcpy(&val, in + cur_body, 4);
      cur_body += 4;
      if (values) {
        if (col->type == COL_DOUBLE || col->type == COL_NUMERIC || col->type == COL_DECIMAL) values[i].double_val = val;
        else if (col->type == COL_FLOAT) values[i].float_val = val;
        else values[i].int_val = val;
      }
    } else if (serial_type == 7) {
      /* 64-bit IEEE double */
      double val;
      memcpy(&val, in + cur_body, 8);
      cur_body += 8;
      if (values) {
        if (col->type == COL_FLOAT) { values[i].float_val = (float)val; values[i].double_val = val; }
        else if (col->type == COL_DOUBLE || col->type == COL_NUMERIC || col->type == COL_DECIMAL) values[i].double_val = val;
        else values[i].int_val = (int32_t)val;
      }
    } else if (serial_type == 8) {
      /* Constant 0 */
      if (values) {
        if (col->type == COL_BOOL) values[i].bool_val = false;
        else if (col->type == COL_DOUBLE || col->type == COL_NUMERIC || col->type == COL_DECIMAL) values[i].double_val = 0.0;
        else if (col->type == COL_FLOAT) values[i].float_val = 0.0f;
        else values[i].int_val = 0;
      }
    } else if (serial_type == 9) {
      /* Constant 1 */
      if (values) {
        if (col->type == COL_BOOL) values[i].bool_val = true;
        else if (col->type == COL_DOUBLE || col->type == COL_NUMERIC || col->type == COL_DECIMAL) values[i].double_val = 1.0;
        else if (col->type == COL_FLOAT) values[i].float_val = 1.0f;
        else values[i].int_val = 1;
      }
    } else if (serial_type >= 12) {
      /* String/Blob of arbitrary length */
      uint32_t len = 0;
      if (serial_type % 2 == 0) {
        len = (uint32_t)((serial_type - 12) / 2);
      } else {
        len = (uint32_t)((serial_type - 13) / 2);
      }
      if (values) value_set_text_len(&values[i], (const char*)(in + cur_body), len);
      cur_body += len;
    }
  }

  uint64_t exp = 0;
  uint64_t xmin = 0;
  uint64_t xmax = 0;
  while (cur_hdr < body_offset) {
    uint64_t extra_type = 0;
    cur_hdr += get_varint(in + cur_hdr, &extra_type);
    if (extra_type == SERIAL_TYPE_TTL) {
      memcpy(&exp, in + cur_body, 8);
      cur_body += 8;
    } else if (extra_type == SERIAL_TYPE_XMIN) {
      memcpy(&xmin, in + cur_body, 8);
      cur_body += 8;
    } else if (extra_type == SERIAL_TYPE_XMAX) {
      memcpy(&xmax, in + cur_body, 8);
      cur_body += 8;
    } else {
      break;
    }
  }
  if (out_expire_at) {
    *out_expire_at = exp;
  }
  if (out_xmin) {
    *out_xmin = xmin;
  }
  if (out_xmax) {
    *out_xmax = xmax;
  }

  return cur_body;
}

uint32_t deserialize_row_with_ttl(TableDef* def, void* src, Value* values, uint64_t* out_expire_at) {
  return deserialize_row_with_mvcc(def, src, values, out_expire_at, NULL, NULL);
}

uint32_t deserialize_row(TableDef* def, void* src, Value* values) {
  return deserialize_row_with_mvcc(def, src, values, NULL, NULL, NULL);
}

void print_row(TableDef* def, Value* values) {
  printf("(");
  for (uint32_t i = 0; i < def->num_cols; i++) {
    if (i) printf(", ");
    if (values[i].is_null) {
      printf("NULL");
    } else {
      switch (def->columns[i].type) {
        case COL_INT:       printf("%d",  values[i].int_val);   break;
        case COL_FLOAT:     printf("%.4g", values[i].double_val != 0.0 ? values[i].double_val : (double)values[i].float_val); break;
        case COL_DOUBLE:
        case COL_NUMERIC:
        case COL_DECIMAL:   printf("%.8g", values[i].double_val); break;
        case COL_BOOL:      printf("%s",  values[i].bool_val ? "true" : "false"); break;
        case COL_BLOB:
          if (strncasecmp(values[i].text_val, "x'", 2) == 0 || strncasecmp(values[i].text_val, "0x", 2) == 0) {
            printf("%s", values[i].text_val);
          } else {
            printf("x'%s'", values[i].text_val);
          }
          break;
        case COL_DATETIME:
        case COL_DATE:
        case COL_TIME:
        case COL_TIMESTAMP:
        case COL_TEXT:
        case COL_VARCHAR:
        case COL_VECTOR:    printf("%s",  values[i].text_val);  break;
      }
    }
  }
  printf(")\n");
}

void print_schema(TableDef* def) {
  printf("CREATE TABLE %s (\n", def->name);
  for (uint32_t i = 0; i < def->num_cols; i++) {
    Column* col = &def->columns[i];
    printf("  %s ", col->name);
    switch (col->type) {
      case COL_INT:       printf("INT");          break;
      case COL_FLOAT:     printf("FLOAT");        break;
      case COL_DOUBLE:    printf("DOUBLE");       break;
      case COL_BOOL:      printf("BOOL");         break;
      case COL_BLOB:      printf("BLOB");         break;
      case COL_DATETIME:  printf("DATETIME");     break;
      case COL_DATE:      printf("DATE");         break;
      case COL_TIME:      printf("TIME");         break;
      case COL_TIMESTAMP: printf("TIMESTAMP");    break;
      case COL_NUMERIC:   printf("NUMERIC");      break;
      case COL_DECIMAL:   printf("DECIMAL");      break;
      case COL_TEXT:      printf("TEXT(%u)", col->size); break;
      case COL_VARCHAR:   printf("VARCHAR(%u)", col->size); break;
      case COL_VECTOR:    printf("VECTOR(%u)", col->size / 16 > 0 ? col->size / 16 : col->size); break;
    }
    if (col->has_index) printf(" [INDEX root=%u]", col->index_root_page);
    if (i < def->num_cols - 1) printf(",");
    printf("\n");
  }
  printf(")");
  if (def->default_ttl > 0) printf(" WITH TTL %u", def->default_ttl);
  if (def->with_history) printf(" WITH HISTORY");
  printf(";\n");
}
