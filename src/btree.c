#include "btree.h"

/* ════════════════════════════════════════════════════════════════════════════
 *  Slotted Page Leaf Node Layout (4-byte aligned):
 *
 *  Common Header (8 bytes):
 *    [node_type : 1] [is_root : 1] [pad : 2] [parent_ptr : 4]
 *
 *  Leaf Header (20 bytes total):
 *    Common Header (8B) + [num_cells : 4] + [next_leaf : 4] + [free_space_offset : 2] + [pad : 2]
 *
 *  Slot Array (starts at offset 20):
 *    Each slot is 4 bytes:
 *      [offset : 2] [size : 2]
 *    (Key is read directly from serialized row data at offset)
 *
 *  Internal Node Layout (32-byte keys, 16 bytes header):
 *    Common Header (8B) + [num_keys : 4] + [right_child : 4]
 *    Each slot is 36 bytes: [child_page : 4] [key_bytes : 32]
 * ══════════════════════════════════════════════════════════════════════════ */

typedef enum { NODE_INTERNAL = 0, NODE_LEAF = 1 } NodeType;

/* Common header offsets */
#define NODE_TYPE_OFFSET        0u
#define IS_ROOT_OFFSET          1u
#define PARENT_POINTER_OFFSET   4u
#define COMMON_NODE_HEADER_SIZE 8u

/* Leaf Header offsets */
#define LEAF_NODE_NUM_CELLS_OFFSET  8u
#define LEAF_NODE_NEXT_LEAF_OFFSET  12u
#define LEAF_NODE_FREE_SPACE_OFFSET 16u
#define LEAF_NODE_HEADER_SIZE       20u

/* Internal Node offsets */
#define INTERNAL_NODE_NUM_KEYS_OFFSET    8u
#define INTERNAL_NODE_RIGHT_CHILD_OFFSET 12u
#define INTERNAL_NODE_HEADER_SIZE        16u
#define INTERNAL_NODE_CHILD_SIZE         4u
#define INTERNAL_NODE_KEY_SIZE           32u
#define INTERNAL_NODE_CELL_SIZE          (INTERNAL_NODE_CHILD_SIZE + INTERNAL_NODE_KEY_SIZE)
#define INTERNAL_NODE_MAX_KEYS           32u

/* Underflow limits */
const uint32_t LEAF_NODE_MIN_CELLS = 2u;
#define INTERNAL_NODE_MIN_KEYS (INTERNAL_NODE_MAX_KEYS / 2)

/* Slot representation */
typedef struct {
  uint16_t offset;
  uint16_t size;
} PageSlot;

/* Temp cell container for B+ Tree splits */
typedef struct {
  uint32_t size;
  void*    data;
} TempCell;

/* ── Common Node Accessors ───────────────────────────────────────────────── */
static inline NodeType get_node_type(void* node) {
  return (NodeType)*((uint8_t*)node + NODE_TYPE_OFFSET);
}
static inline void set_node_type(void* node, NodeType t) {
  *((uint8_t*)node + NODE_TYPE_OFFSET) = (uint8_t)t;
}
static inline bool is_node_root(void* node) {
  return *((uint8_t*)node + IS_ROOT_OFFSET);
}
static inline void set_node_root(void* node, bool v) {
  *((uint8_t*)node + IS_ROOT_OFFSET) = (uint8_t)v;
}
static inline uint32_t* node_parent(void* node) {
  return (uint32_t*)((uint8_t*)node + PARENT_POINTER_OFFSET);
}

/* ── Leaf Accessors ──────────────────────────────────────────────────────── */
uint32_t* leaf_node_num_cells(void* node) {
  return (uint32_t*)((uint8_t*)node + LEAF_NODE_NUM_CELLS_OFFSET);
}
static inline uint32_t* leaf_node_next_leaf(void* node) {
  return (uint32_t*)((uint8_t*)node + LEAF_NODE_NEXT_LEAF_OFFSET);
}
static inline uint16_t* leaf_node_free_space(void* node) {
  return (uint16_t*)((uint8_t*)node + LEAF_NODE_FREE_SPACE_OFFSET);
}
static inline PageSlot* leaf_node_slot(void* node, uint32_t i) {
  return (PageSlot*)((uint8_t*)node + LEAF_NODE_HEADER_SIZE) + i;
}
static inline void* leaf_node_value(void* node, uint32_t i) {
  return (uint8_t*)node + leaf_node_slot(node, i)->offset;
}

/* ── Internal Accessors ──────────────────────────────────────────────────── */
static inline uint32_t* internal_node_num_keys(void* node) {
  return (uint32_t*)((uint8_t*)node + INTERNAL_NODE_NUM_KEYS_OFFSET);
}
static inline uint32_t* internal_node_right_child(void* node) {
  return (uint32_t*)((uint8_t*)node + INTERNAL_NODE_RIGHT_CHILD_OFFSET);
}
static inline void* internal_node_cell(void* node, uint32_t i) {
  return (uint8_t*)node + INTERNAL_NODE_HEADER_SIZE + i * INTERNAL_NODE_CELL_SIZE;
}
static inline uint32_t* internal_node_child(void* node, uint32_t i) {
  uint32_t n = *internal_node_num_keys(node);
  if (i > n) { fprintf(stderr, "internal_node_child: index %u > num_keys %u\n", i, n); exit(1); }
  if (i == n) return internal_node_right_child(node);
  return (uint32_t*)internal_node_cell(node, i);
}
static inline void* internal_node_key(void* node, uint32_t i) {
  return (uint8_t*)internal_node_cell(node, i) + INTERNAL_NODE_CHILD_SIZE;
}

/* ── Key Comparison Engine ───────────────────────────────────────────────── */
int compare_keys(ColumnType type, const void* k1, const void* k2) {
  switch (type) {
    case COL_INT: {
      int32_t v1 = *(const int32_t*)k1;
      int32_t v2 = *(const int32_t*)k2;
      return (v1 > v2) - (v1 < v2);
    }
    case COL_FLOAT: {
      float v1 = *(const float*)k1;
      float v2 = *(const float*)k2;
      return (v1 > v2) - (v1 < v2);
    }
    case COL_DOUBLE:
    case COL_NUMERIC:
    case COL_DECIMAL: {
      double v1 = *(const double*)k1;
      double v2 = *(const double*)k2;
      return (v1 > v2) - (v1 < v2);
    }
    case COL_BOOL: {
      bool v1 = *(const bool*)k1;
      bool v2 = *(const bool*)k2;
      return (v1 > v2) - (v1 < v2);
    }
    case COL_BLOB:
    case COL_DATETIME:
    case COL_DATE:
    case COL_TIME:
    case COL_TIMESTAMP:
    case COL_TEXT:
      return strcmp((const char*)k1, (const char*)k2);
    case COL_VARCHAR: {
      uint16_t len1, len2;
      memcpy(&len1, k1, 2);
      memcpy(&len2, k2, 2);
      int res = memcmp((const uint8_t*)k1 + 2, (const uint8_t*)k2 + 2, (len1 < len2) ? len1 : len2);
      if (res != 0) return res;
      return (len1 > len2) - (len1 < len2);
    }
  }
  return 0;
}

int compare_values(ColumnType type, const Value* v1, const Value* v2) {
  if (v1->is_null && v2->is_null) return 0;
  if (v1->is_null) return -1;
  if (v2->is_null) return 1;
  switch (type) {
    case COL_INT:
      return (v1->int_val > v2->int_val) - (v1->int_val < v2->int_val);
    case COL_FLOAT: {
      float f1 = v1->double_val != 0.0 ? (float)v1->double_val : v1->float_val;
      float f2 = v2->double_val != 0.0 ? (float)v2->double_val : v2->float_val;
      return (f1 > f2) - (f1 < f2);
    }
    case COL_DOUBLE:
    case COL_NUMERIC:
    case COL_DECIMAL: {
      double d1 = v1->double_val != 0.0 ? v1->double_val : (double)v1->float_val;
      double d2 = v2->double_val != 0.0 ? v2->double_val : (double)v2->float_val;
      return (d1 > d2) - (d1 < d2);
    }
    case COL_BOOL:
      return (v1->bool_val > v2->bool_val) - (v1->bool_val < v2->bool_val);
    case COL_BLOB:
    case COL_DATETIME:
    case COL_DATE:
    case COL_TIME:
    case COL_TIMESTAMP:
    case COL_TEXT:
    case COL_VARCHAR:
      return strcmp(v1->text_val, v2->text_val);
  }
  return 0;
}

/* Helper to serialize just Column 0 key into a raw 32-byte key buffer */
static void serialize_col0_key(TableDef* def, Value* key_val, uint8_t* out_buf) {
  memset(out_buf, 0, INTERNAL_NODE_KEY_SIZE);
  ColumnType type = def->columns[0].type;
  switch (type) {
    case COL_INT:    memcpy(out_buf, &key_val->int_val, 4); break;
    case COL_FLOAT:  memcpy(out_buf, &key_val->float_val, 4); break;
    case COL_DOUBLE:
    case COL_NUMERIC:
    case COL_DECIMAL: memcpy(out_buf, &key_val->double_val, 8); break;
    case COL_BOOL:   out_buf[0] = key_val->bool_val ? 1 : 0; break;
    /* Store first (INTERNAL_NODE_KEY_SIZE-1) chars as prefix key — intentional truncation */
    case COL_BLOB:
    case COL_DATETIME:
    case COL_DATE:
    case COL_TIME:
    case COL_TIMESTAMP:
    case COL_TEXT:   snprintf((char*)out_buf, INTERNAL_NODE_KEY_SIZE, "%.*s",
                              INTERNAL_NODE_KEY_SIZE - 1, key_val->text_val); break;
    case COL_VARCHAR: {
      uint16_t len = (uint16_t)strlen(key_val->text_val);
      if (len > INTERNAL_NODE_KEY_SIZE - 2) len = INTERNAL_NODE_KEY_SIZE - 2;
      memcpy(out_buf, &len, 2);
      memcpy(out_buf + 2, key_val->text_val, len);
      break;
    }
  }
}

/* ── Node Initialisation ─────────────────────────────────────────────────── */
void initialize_leaf_node(void* node) {
  set_node_type(node, NODE_LEAF);
  set_node_root(node, false);
  *leaf_node_num_cells(node) = 0;
  *leaf_node_next_leaf(node) = 0;
  *leaf_node_free_space(node) = 4096;
}

void initialize_internal_node(void* node) {
  set_node_type(node, NODE_INTERNAL);
  set_node_root(node, false);
  *internal_node_num_keys(node) = 0;
  *internal_node_right_child(node) = INVALID_PAGE_NUM;
}

void initialize_root_leaf(void* node) {
  initialize_leaf_node(node);
  set_node_root(node, true);
}

/* ── Overflow Pages Architecture ─────────────────────────────────────────── */
#define OVERFLOW_HEADER_SIZE    4u
#define OVERFLOW_PAYLOAD_SIZE   (PAGE_SIZE - OVERFLOW_HEADER_SIZE)
#define BTREE_MAX_LOCAL_PAYLOAD 1024u

static uint32_t btree_write_overflow_chain(Pager* pager, const uint8_t* data, uint32_t len) {
  if (len == 0) return 0;
  uint32_t first_page = 0;
  uint32_t prev_page = 0;
  uint32_t bytes_written = 0;

  while (bytes_written < len) {
    uint32_t new_page = get_unused_page_num(pager);
    pager_journal_page(pager, new_page);
    uint8_t* pbuf = (uint8_t*)get_page(pager, new_page);
    memset(pbuf, 0, PAGE_SIZE);

    if (first_page == 0) {
      first_page = new_page;
    }
    if (prev_page != 0) {
      uint8_t* prev_buf = (uint8_t*)get_page(pager, prev_page);
      memcpy(prev_buf, &new_page, sizeof(uint32_t));
    }

    uint32_t chunk_size = len - bytes_written;
    if (chunk_size > OVERFLOW_PAYLOAD_SIZE) chunk_size = OVERFLOW_PAYLOAD_SIZE;

    memcpy(pbuf + OVERFLOW_HEADER_SIZE, data + bytes_written, chunk_size);
    bytes_written += chunk_size;
    prev_page = new_page;
  }

  if (prev_page != 0) {
    uint8_t* prev_buf = (uint8_t*)get_page(pager, prev_page);
    uint32_t zero = 0;
    memcpy(prev_buf, &zero, sizeof(uint32_t));
  }

  return first_page;
}

static void btree_free_overflow_chain(Pager* pager, uint32_t first_page) {
  uint32_t cur = first_page;
  while (cur != 0 && cur != INVALID_PAGE_NUM && cur >= pager->reserved_catalog_pages) {
    pager_journal_page(pager, cur);
    uint8_t* pbuf = (uint8_t*)get_page(pager, cur);
    uint32_t next = 0;
    memcpy(&next, pbuf, sizeof(uint32_t));
    pager_free_page(pager, cur);
    cur = next;
  }
}

static void btree_read_overflow_chain(Pager* pager, uint32_t first_page, uint8_t* dst, uint32_t len) {
  uint32_t cur = first_page;
  uint32_t bytes_read = 0;
  while (cur != 0 && cur != INVALID_PAGE_NUM && bytes_read < len) {
    uint8_t* pbuf = (uint8_t*)get_page(pager, cur);
    uint32_t next = 0;
    memcpy(&next, pbuf, sizeof(uint32_t));

    uint32_t chunk_size = len - bytes_read;
    if (chunk_size > OVERFLOW_PAYLOAD_SIZE) chunk_size = OVERFLOW_PAYLOAD_SIZE;

    memcpy(dst + bytes_read, pbuf + OVERFLOW_HEADER_SIZE, chunk_size);
    bytes_read += chunk_size;
    cur = next;
  }
}

static pthread_key_t s_btree_overflow_key;
static pthread_once_t s_btree_overflow_once = PTHREAD_ONCE_INIT;

static void btree_overflow_destructor(void* ptr) {
  if (ptr) free(ptr);
}

static void btree_make_overflow_key(void) {
  pthread_key_create(&s_btree_overflow_key, btree_overflow_destructor);
}

static __thread uint8_t* s_overflow_buf = NULL;
static __thread uint32_t s_overflow_cap = 0;

__attribute__((destructor)) static void btree_thread_cleanup(void) {
  if (s_overflow_buf) {
    free(s_overflow_buf);
    s_overflow_buf = NULL;
    s_overflow_cap = 0;
  }
}

static void serialize_cell_for_leaf(TableDef* def, Value* values, uint64_t expire_at, Pager* pager, uint8_t* out_cell, uint32_t* out_cell_size) {
  uint32_t estimated_len = 256;
  for (uint32_t i = 0; i < def->num_cols; i++) {
    if (!values[i].is_null) {
      if (values[i].text_val) {
        estimated_len += values[i].text_len > 0 ? values[i].text_len : (uint32_t)strlen(values[i].text_val);
      }
      estimated_len += 32;
    }
  }

  uint8_t stack_buf[16384];
  uint8_t* ser_buf = stack_buf;
  uint32_t ser_cap = sizeof(stack_buf);

  if (ser_cap < estimated_len + 1024) {
    ser_cap = estimated_len + 65536;
    ser_buf = malloc(ser_cap);
  }

  uint32_t total_size = serialize_row_with_ttl(def, values, expire_at, ser_buf);
  while (total_size > ser_cap) {
    ser_cap = total_size + 65536;
    uint8_t* new_buf = (ser_buf == stack_buf) ? malloc(ser_cap) : realloc(ser_buf, ser_cap);
    if (new_buf) {
      ser_buf = new_buf;
      total_size = serialize_row_with_ttl(def, values, expire_at, ser_buf);
    } else {
      break;
    }
  }

  if (total_size <= BTREE_MAX_LOCAL_PAYLOAD) {
    memcpy(out_cell, ser_buf, total_size);
    *out_cell_size = total_size;
  } else {
    uint32_t local_chunk = BTREE_MAX_LOCAL_PAYLOAD - 8;
    uint32_t overflow_len = total_size - local_chunk;
    uint32_t first_overflow = btree_write_overflow_chain(pager, ser_buf + local_chunk, overflow_len);

    memcpy(out_cell, &total_size, 4);
    memcpy(out_cell + 4, &first_overflow, 4);
    memcpy(out_cell + 8, ser_buf, local_chunk);

    *out_cell_size = BTREE_MAX_LOCAL_PAYLOAD;
  }

  if (ser_buf != stack_buf) {
    free(ser_buf);
  }
}

static uint32_t btree_get_varint(const uint8_t* p, uint64_t* v) {
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

static void extract_col0_from_packed_record(TableDef* def, const void* record_bytes, uint32_t cell_size, Value* out_val) {
  const uint8_t* in = (const uint8_t*)record_bytes;
  if (cell_size >= BTREE_MAX_LOCAL_PAYLOAD) {
    uint32_t total_size = 0;
    uint32_t first_overflow_page = 0;
    memcpy(&total_size, in, 4);
    memcpy(&first_overflow_page, in + 4, 4);
    if (total_size > cell_size && first_overflow_page != 0) {
      in += 8;
    }
  }

  uint64_t total_hdr_size = 0;
  uint32_t p = btree_get_varint(in, &total_hdr_size);
  
  uint64_t serial_type = 0;
  btree_get_varint(in + p, &serial_type);
  
  uint32_t body_offset = (uint32_t)total_hdr_size;
  Column* col = &def->columns[0];
  
  value_init(out_val);
  if (serial_type == 1) {
    out_val->int_val = (int8_t)in[body_offset];
  } else if (serial_type == 2) {
    int16_t val;
    memcpy(&val, in + body_offset, 2);
    out_val->int_val = val;
  } else if (serial_type == 4) {
    int32_t val;
    memcpy(&val, in + body_offset, 4);
    out_val->int_val = val;
  } else if (serial_type == 7) {
    double val;
    memcpy(&val, in + body_offset, 8);
    if (col->type == COL_FLOAT) { out_val->float_val = (float)val; out_val->double_val = val; }
    else if (col->type == COL_DOUBLE || col->type == COL_NUMERIC || col->type == COL_DECIMAL) out_val->double_val = val;
    else out_val->int_val = (int32_t)val;
  } else if (serial_type == 8) {
    if (col->type == COL_BOOL) out_val->bool_val = false;
    else if (col->type == COL_DOUBLE || col->type == COL_NUMERIC || col->type == COL_DECIMAL) out_val->double_val = 0.0;
    else if (col->type == COL_FLOAT) { out_val->float_val = 0.0f; out_val->double_val = 0.0; }
    else out_val->int_val = 0;
  } else if (serial_type == 9) {
    if (col->type == COL_BOOL) out_val->bool_val = true;
    else if (col->type == COL_DOUBLE || col->type == COL_NUMERIC || col->type == COL_DECIMAL) out_val->double_val = 1.0;
    else if (col->type == COL_FLOAT) { out_val->float_val = 1.0f; out_val->double_val = 1.0; }
    else out_val->int_val = 1;
  } else if (serial_type >= 12) {
    uint32_t len = 0;
    if (serial_type % 2 == 0) {
      len = (uint32_t)((serial_type - 12) / 2);
    } else {
      len = (uint32_t)((serial_type - 13) / 2);
    }
    value_set_text_len(out_val, (const char*)(in + body_offset), len);
  }
}

/* ── Node max key (copied to out_buf) ────────────────────────────────────── */
static void get_node_max_key(Pager* pager, void* node, TableDef* def, uint8_t* out_key_buf) {
  memset(out_key_buf, 0, INTERNAL_NODE_KEY_SIZE);
  switch (get_node_type(node)) {
    case NODE_LEAF: {
      uint32_t nc = *leaf_node_num_cells(node);
      if (nc == 0) return;
      uint32_t last_idx = nc - 1;
      void* row_val = leaf_node_value(node, last_idx);
      uint32_t cell_size = leaf_node_slot(node, last_idx)->size;
      
      Value max_val;
      extract_col0_from_packed_record(def, row_val, cell_size, &max_val);
      serialize_col0_key(def, &max_val, out_key_buf);
      value_free(&max_val);
      return;
    }
    case NODE_INTERNAL: {
      uint32_t rc_page = *internal_node_right_child(node);
      if (rc_page == INVALID_PAGE_NUM || rc_page == 0) return;
      void* rc = get_page(pager, rc_page);
      if (!rc) return;
      get_node_max_key(pager, rc, def, out_key_buf);
      return;
    }
  }
}

/* ── print_tree ──────────────────────────────────────────────────────────── */
static void indent(uint32_t n) { for (uint32_t i = 0; i < n; i++) printf("  "); }

void print_tree(Pager* pager, uint32_t page_num, uint32_t level, TableDef* def) {
  void* node = get_page(pager, page_num);
  switch (get_node_type(node)) {
    case NODE_INTERNAL: {
      uint32_t nk = *internal_node_num_keys(node);
      indent(level); printf("- internal (size %u)\n", nk);
      for (uint32_t i = 0; i < nk; i++) {
        print_tree(pager, *internal_node_child(node, i), level + 1, def);
        indent(level + 1);
        printf("- key\n");
      }
      print_tree(pager, *internal_node_right_child(node), level + 1, def);
      break;
    }
    case NODE_LEAF: {
      uint32_t nc = *leaf_node_num_cells(node);
      indent(level); printf("- leaf (size %u)\n", nc);
      for (uint32_t i = 0; i < nc; i++) {
        indent(level + 1);
        Value val[MAX_COLUMNS];
        deserialize_row(def, leaf_node_value(node, i), val);
        printf("- ");
        print_row(def, val);
      }
      break;
    }
  }
}

uint32_t btree_depth(Pager* pager, uint32_t root_page_num) {
  if (!pager || root_page_num == INVALID_PAGE_NUM) return 0;
  uint32_t depth = 1;
  uint32_t page_num = root_page_num;
  void* node = get_page(pager, page_num);
  while (node && get_node_type(node) == NODE_INTERNAL) {
    depth++;
    page_num = *internal_node_child(node, 0);
    node = get_page(pager, page_num);
  }
  return depth;
}

/* ── Slotted Page Defragmentation ────────────────────────────────────────── */
void leaf_node_defragment(void* node) {
  uint32_t num_cells = *leaf_node_num_cells(node);
  if (num_cells == 0) {
    *leaf_node_free_space(node) = 4096;
    return;
  }

  uint8_t temp[PAGE_SIZE];
  memset(temp, 0, PAGE_SIZE);

  uint32_t header_and_slots_size = LEAF_NODE_HEADER_SIZE + num_cells * sizeof(PageSlot);
  memcpy(temp, node, header_and_slots_size);

  uint16_t free_offset = 4096;
  PageSlot* temp_slots = (PageSlot*)(temp + LEAF_NODE_HEADER_SIZE);

  for (uint32_t i = 0; i < num_cells; i++) {
    PageSlot* old_slot = leaf_node_slot(node, i);
    free_offset -= old_slot->size;
    memcpy(temp + free_offset, (uint8_t*)node + old_slot->offset, old_slot->size);
    temp_slots[i].offset = free_offset;
  }

  *(uint16_t*)(temp + LEAF_NODE_FREE_SPACE_OFFSET) = free_offset;
  memcpy(node, temp, PAGE_SIZE);
}

/* ── B+ Tree Search ──────────────────────────────────────────────────────── */
static uint32_t leaf_node_find(void* node, const uint8_t* raw_key, TableDef* def) {
  uint32_t nc = *leaf_node_num_cells(node);
  uint32_t lo = 0, hi = nc;
  ColumnType key_type = def->columns[0].type;

  if (key_type == COL_INT) {
    int32_t target_key = *(int32_t*)raw_key;
    while (lo < hi) {
      uint32_t mid = (lo + hi) / 2;
      void* mid_cell = leaf_node_value(node, mid);
      uint32_t cell_size = leaf_node_slot(node, mid)->size;
      Value mid_val;
      extract_col0_from_packed_record(def, mid_cell, cell_size, &mid_val);
      int32_t m_int = mid_val.int_val;
      value_free(&mid_val);
      if (m_int < target_key) lo = mid + 1;
      else                    hi = mid;
    }
    return lo;
  }

  while (lo < hi) {
    uint32_t mid = (lo + hi) / 2;
    void* mid_cell = leaf_node_value(node, mid);
    uint32_t cell_size = leaf_node_slot(node, mid)->size;
    
    Value mid_val;
    extract_col0_from_packed_record(def, mid_cell, cell_size, &mid_val);
    
    uint8_t mid_raw_key[INTERNAL_NODE_KEY_SIZE];
    serialize_col0_key(def, &mid_val, mid_raw_key);
    value_free(&mid_val);

    if (compare_keys(key_type, mid_raw_key, raw_key) < 0) lo = mid + 1;
    else                                                   hi = mid;
  }
  return lo;
}

static uint32_t internal_node_find_child(void* node, const uint8_t* raw_key, TableDef* def) {
  uint32_t nk = *internal_node_num_keys(node);
  uint32_t lo = 0, hi = nk;
  ColumnType key_type = def->columns[0].type;

  if (key_type == COL_INT) {
    int32_t target_key = *(int32_t*)raw_key;
    while (lo < hi) {
      uint32_t mid = (lo + hi) / 2;
      int32_t mid_key = *(int32_t*)internal_node_key(node, mid);
      if (mid_key < target_key) lo = mid + 1;
      else                      hi = mid;
    }
    return lo;
  }

  while (lo < hi) {
    uint32_t mid = (lo + hi) / 2;
    void* mid_key = internal_node_key(node, mid);
    if (compare_keys(key_type, mid_key, raw_key) < 0) lo = mid + 1;
    else                                              hi = mid;
  }
  return lo;
}

static Cursor* internal_node_find(Table* table, uint32_t page_num, const uint8_t* raw_key);

static Cursor* leaf_node_navigate(Table* table, uint32_t page_num, const uint8_t* raw_key) {
  void* node = get_page(table->pager, page_num);
  Cursor* c = malloc(sizeof(Cursor));
  c->table       = table;
  c->page_num    = page_num;
  c->end_of_table= false;
  c->cell_num    = leaf_node_find(node, raw_key, table->def);
  if (c->cell_num >= *leaf_node_num_cells(node)) {
    uint32_t next = *leaf_node_next_leaf(node);
    if (next == 0) {
      c->end_of_table = true;
    } else {
      c->page_num = next;
      c->cell_num = 0;
      void* next_node = get_page(table->pager, next);
      if (*leaf_node_num_cells(next_node) == 0) {
        c->end_of_table = true;
      }
    }
  }
  return c;
}

static Cursor* internal_node_find(Table* table, uint32_t page_num, const uint8_t* raw_key) {
  void* node = get_page(table->pager, page_num);
  uint32_t child_idx = internal_node_find_child(node, raw_key, table->def);
  uint32_t child_page = *internal_node_child(node, child_idx);
  void* child = get_page(table->pager, child_page);
  switch (get_node_type(child)) {
    case NODE_LEAF:     return leaf_node_navigate(table, child_page, raw_key);
    case NODE_INTERNAL: return internal_node_find(table, child_page, raw_key);
  }
  return NULL;
}

Cursor* btree_find(Table* table, Value* key_value) {
  uint8_t raw_key[INTERNAL_NODE_KEY_SIZE];
  serialize_col0_key(table->def, key_value, raw_key);

  void* root = get_page(table->pager, table->def->root_page_num);
  if (get_node_type(root) == NODE_LEAF)
    return leaf_node_navigate(table, table->def->root_page_num, raw_key);
  return internal_node_find(table, table->def->root_page_num, raw_key);
}

void btree_find_out(Table* table, Value* key_value, Cursor* out_cursor) {
  uint8_t raw_key[INTERNAL_NODE_KEY_SIZE];
  serialize_col0_key(table->def, key_value, raw_key);

  uint32_t page_num = table->def->root_page_num;
  void* node = get_page(table->pager, page_num);
  while (get_node_type(node) == NODE_INTERNAL) {
    uint32_t child_idx = internal_node_find_child(node, raw_key, table->def);
    page_num = *internal_node_child(node, child_idx);
    node = get_page(table->pager, page_num);
  }

  out_cursor->table = table;
  out_cursor->page_num = page_num;
  out_cursor->end_of_table = false;
  out_cursor->cell_num = leaf_node_find(node, raw_key, table->def);
  if (out_cursor->cell_num >= *leaf_node_num_cells(node)) {
    uint32_t next = *leaf_node_next_leaf(node);
    if (next == 0) {
      out_cursor->end_of_table = true;
    } else {
      out_cursor->page_num = next;
      out_cursor->cell_num = 0;
      void* next_node = get_page(table->pager, next);
      if (*leaf_node_num_cells(next_node) == 0) {
        out_cursor->end_of_table = true;
      }
    }
  }
}

Cursor* btree_start(Table* table) {
  Cursor* c = malloc(sizeof(Cursor));
  btree_start_out(table, c);
  return c;
}

void btree_start_out(Table* table, Cursor* out_cursor) {
  uint32_t page_num = table->def->root_page_num;
  void* node = get_page(table->pager, page_num);
  while (get_node_type(node) == NODE_INTERNAL) {
    page_num = *internal_node_child(node, 0);
    node = get_page(table->pager, page_num);
  }

  out_cursor->table = table;
  out_cursor->page_num = page_num;
  out_cursor->cell_num = 0;
  out_cursor->end_of_table = (*leaf_node_num_cells(node) == 0);
  pager_journal_page(table->pager, page_num);
}

void btree_key_value(Cursor* cursor, Value* out_val) {
  Value values[MAX_COLUMNS];
  deserialize_row(cursor->table->def, cursor_value(cursor), values);
  value_init(out_val);
  value_copy(out_val, &values[0]);
  value_free_row(values, cursor->table->def->num_cols);
}

/* ── Cursor value & navigation ───────────────────────────────────────────── */
void* cursor_value(Cursor* cursor) {
  void* node = get_page(cursor->table->pager, cursor->page_num);
  PageSlot* slot = leaf_node_slot(node, cursor->cell_num);
  uint8_t* cell_data = (uint8_t*)node + slot->offset;

  if (slot->size < BTREE_MAX_LOCAL_PAYLOAD) {
    return cell_data;
  }

  uint32_t total_size = 0;
  uint32_t first_overflow_page = 0;
  memcpy(&total_size, cell_data, 4);
  memcpy(&first_overflow_page, cell_data + 4, 4);

  if (total_size <= slot->size || first_overflow_page == 0) {
    return cell_data;
  }

  if (s_overflow_cap < total_size + 64) {
    s_overflow_cap = total_size + 65536;
    s_overflow_buf = realloc(s_overflow_buf, s_overflow_cap);
    pthread_once(&s_btree_overflow_once, btree_make_overflow_key);
    pthread_setspecific(s_btree_overflow_key, s_overflow_buf);
  }

  uint32_t local_chunk = slot->size - 8;
  memcpy(s_overflow_buf, cell_data + 8, local_chunk);

  if (total_size > local_chunk && first_overflow_page != 0) {
    btree_read_overflow_chain(cursor->table->pager, first_overflow_page, s_overflow_buf + local_chunk, total_size - local_chunk);
  }

  return s_overflow_buf;
}

void cursor_advance(Cursor* cursor) {
  void* node = get_page(cursor->table->pager, cursor->page_num);
  cursor->cell_num++;
  if (cursor->cell_num >= *leaf_node_num_cells(node)) {
    uint32_t next = *leaf_node_next_leaf(node);
    if (next == 0) cursor->end_of_table = true;
    else { cursor->page_num = next; cursor->cell_num = 0; }
  }
}

/* ── Internal node separator update ──────────────────────────────────────── */
static void update_internal_node_key(void* node, const uint8_t* old_key, const uint8_t* new_key, TableDef* def) {
  uint32_t idx = internal_node_find_child(node, old_key, def);
  memcpy(internal_node_key(node, idx), new_key, INTERNAL_NODE_KEY_SIZE);
}

/* Forward declarations for B+ tree structure modifications */
static void internal_node_insert(Table* table, uint32_t parent_page, uint32_t child_page);
static void internal_node_split_and_insert(Table* table, uint32_t parent_page, uint32_t child_page);
void handle_internal_underflow(Table* table, uint32_t page_num);
void internal_remove_child(Table* table, uint32_t parent_page, uint32_t child_index);
void leaf_node_handle_underflow(Table* table, uint32_t page_num);

/* ── Root splitting ──────────────────────────────────────────────────────── */
static void create_new_root(Table* table, uint32_t right_child_page) {
  void* root        = get_page(table->pager, table->def->root_page_num);
  void* right_child = get_page(table->pager, right_child_page);
  uint32_t left_page = get_unused_page_num(table->pager);
  void* left_child  = get_page(table->pager, left_page);

  if (get_node_type(root) == NODE_INTERNAL) {
    initialize_internal_node(right_child);
    initialize_internal_node(left_child);
  }

  memcpy(left_child, root, PAGE_SIZE);
  set_node_root(left_child, false);

  if (get_node_type(left_child) == NODE_INTERNAL) {
    uint32_t nk = *internal_node_num_keys(left_child);
    for (uint32_t i = 0; i <= nk; i++) {
      void* ch = get_page(table->pager, *internal_node_child(left_child, i));
      *node_parent(ch) = left_page;
    }
  }

  initialize_internal_node(root);
  set_node_root(root, true);
  *internal_node_num_keys(root) = 1;
  *internal_node_child(root, 0) = left_page;
  
  uint8_t lmax[INTERNAL_NODE_KEY_SIZE];
  get_node_max_key(table->pager, left_child, table->def, lmax);
  memcpy(internal_node_key(root, 0), lmax, INTERNAL_NODE_KEY_SIZE);
  *internal_node_right_child(root) = right_child_page;
  *node_parent(left_child)  = table->def->root_page_num;
  *node_parent(right_child) = table->def->root_page_num;
}

/* ── Internal node insert / split ────────────────────────────────────────── */
static void internal_node_insert(Table* table, uint32_t parent_page, uint32_t child_page) {
  void* parent  = get_page(table->pager, parent_page);
  void* child   = get_page(table->pager, child_page);

  uint8_t cmax[INTERNAL_NODE_KEY_SIZE];
  get_node_max_key(table->pager, child, table->def, cmax);

  uint32_t idx  = internal_node_find_child(parent, cmax, table->def);
  uint32_t nk   = *internal_node_num_keys(parent);
  *node_parent(child) = parent_page;

  if (nk >= INTERNAL_NODE_MAX_KEYS) {
    internal_node_split_and_insert(table, parent_page, child_page);
    return;
  }

  uint32_t rcp = *internal_node_right_child(parent);
  if (rcp == INVALID_PAGE_NUM) {
    *internal_node_right_child(parent) = child_page;
    return;
  }
  void* rc = get_page(table->pager, rcp);
  (*internal_node_num_keys(parent))++;

  uint8_t rcmax[INTERNAL_NODE_KEY_SIZE];
  get_node_max_key(table->pager, rc, table->def, rcmax);

  if (compare_keys(table->def->columns[0].type, cmax, rcmax) > 0) {
    *internal_node_child(parent, nk) = rcp;
    memcpy(internal_node_key(parent, nk), rcmax, INTERNAL_NODE_KEY_SIZE);
    *internal_node_right_child(parent) = child_page;
  } else {
    for (uint32_t i = nk; i > idx; i--) {
      memcpy(internal_node_child(parent, i), internal_node_child(parent, i-1), INTERNAL_NODE_CELL_SIZE);
    }
    *internal_node_child(parent, idx) = child_page;
    memcpy(internal_node_key(parent, idx), cmax, INTERNAL_NODE_KEY_SIZE);
  }
}

static void internal_node_split_and_insert(Table* table, uint32_t parent_page, uint32_t child_page) {
  uint32_t old_page = parent_page;
  void* old_node    = get_page(table->pager, old_page);
  
  uint8_t old_max[INTERNAL_NODE_KEY_SIZE];
  get_node_max_key(table->pager, old_node, table->def, old_max);

  void* child    = get_page(table->pager, child_page);
  uint8_t cmax[INTERNAL_NODE_KEY_SIZE];
  get_node_max_key(table->pager, child, table->def, cmax);

  uint32_t new_page = get_unused_page_num(table->pager);
  bool splitting_root = is_node_root(old_node);

  void* parent;
  if (splitting_root) {
    create_new_root(table, new_page);
    parent   = get_page(table->pager, table->def->root_page_num);
    old_page = *internal_node_child(parent, 0);
    old_node = get_page(table->pager, old_page);
  } else {
    parent = get_page(table->pager, *node_parent(old_node));
    void* nn = get_page(table->pager, new_page);
    initialize_internal_node(nn);
  }

  uint32_t* old_nk = internal_node_num_keys(old_node);
  uint32_t cur_page = *internal_node_right_child(old_node);
  void* cur = get_page(table->pager, cur_page);

  internal_node_insert(table, new_page, cur_page);
  *node_parent(cur) = new_page;
  *internal_node_right_child(old_node) = INVALID_PAGE_NUM;

  for (int32_t i = (int32_t)INTERNAL_NODE_MAX_KEYS - 1; i > (int32_t)(INTERNAL_NODE_MAX_KEYS / 2); i--) {
    cur_page = *internal_node_child(old_node, (uint32_t)i);
    cur = get_page(table->pager, cur_page);
    internal_node_insert(table, new_page, cur_page);
    *node_parent(cur) = new_page;
    (*old_nk)--;
  }

  *internal_node_right_child(old_node) = *internal_node_child(old_node, *old_nk - 1);
  (*old_nk)--;

  uint8_t max_after[INTERNAL_NODE_KEY_SIZE];
  get_node_max_key(table->pager, old_node, table->def, max_after);

  uint32_t dest = (compare_keys(table->def->columns[0].type, cmax, max_after) < 0) ? old_page : new_page;
  internal_node_insert(table, dest, child_page);
  
  uint8_t new_old_max[INTERNAL_NODE_KEY_SIZE];
  get_node_max_key(table->pager, old_node, table->def, new_old_max);
  update_internal_node_key(parent, old_max, new_old_max, table->def);

  if (!splitting_root)
    internal_node_insert(table, *node_parent(old_node), new_page);
}

/* ── Leaf Insert with Slotted Defrag ─────────────────────────────────────── */
static void leaf_node_split_and_insert(Cursor* cursor, Value* values, uint64_t expire_at);

static void leaf_node_insert(Cursor* cursor, Value* values, uint64_t expire_at) {
  void* node  = get_page(cursor->table->pager, cursor->page_num);
  pager_journal_page(cursor->table->pager, cursor->page_num);
  uint32_t nc = *leaf_node_num_cells(node);

  /* Serialize the row into a cell (with overflow chain if oversized) */
  uint8_t temp_buf[BTREE_MAX_LOCAL_PAYLOAD];
  uint32_t size = 0;
  serialize_cell_for_leaf(cursor->table->def, values, expire_at, cursor->table->pager, temp_buf, &size);

  uint16_t free_space = *leaf_node_free_space(node);
  uint32_t slots_end = LEAF_NODE_HEADER_SIZE + nc * sizeof(PageSlot);
  uint32_t needed = sizeof(PageSlot) + size;

  /* If there is not enough contiguous free space, defragment */
  if (free_space < slots_end || (free_space - slots_end) < needed) {
    leaf_node_defragment(node);
    free_space = *leaf_node_free_space(node);
    /* If still full, split the node */
    if (free_space < slots_end || (free_space - slots_end) < needed) {
      leaf_node_split_and_insert(cursor, values, expire_at);
      return;
    }
  }

  /* Make room in slot array */
  for (uint32_t i = nc; i > cursor->cell_num; i--) {
    *leaf_node_slot(node, i) = *leaf_node_slot(node, i - 1);
  }

  /* Insert cell data at bottom */
  *leaf_node_free_space(node) -= size;
  uint16_t new_offset = *leaf_node_free_space(node);
  memcpy((uint8_t*)node + new_offset, temp_buf, size);

  /* Set slot */
  PageSlot* slot = leaf_node_slot(node, cursor->cell_num);
  slot->offset = new_offset;
  slot->size = size;

  (*leaf_node_num_cells(node))++;
}

/* ── Slotted Split ───────────────────────────────────────────────────────── */
static void leaf_node_split_and_insert(Cursor* cursor, Value* values, uint64_t expire_at) {
  void* old_node   = get_page(cursor->table->pager, cursor->page_num);
  
  uint8_t old_max[INTERNAL_NODE_KEY_SIZE];
  get_node_max_key(cursor->table->pager, old_node, cursor->table->def, old_max);

  uint32_t new_page= get_unused_page_num(cursor->table->pager);
  void* new_node   = get_page(cursor->table->pager, new_page);
  initialize_leaf_node(new_node);

  *node_parent(new_node) = *node_parent(old_node);
  *leaf_node_next_leaf(new_node) = *leaf_node_next_leaf(old_node);
  *leaf_node_next_leaf(old_node) = new_page;

  uint32_t num_cells = *leaf_node_num_cells(old_node);
  uint32_t total_cells = num_cells + 1;
  TempCell* temp_cells = malloc(sizeof(TempCell) * total_cells);

  uint8_t new_val_buf[BTREE_MAX_LOCAL_PAYLOAD];
  uint32_t new_val_size = 0;
  serialize_cell_for_leaf(cursor->table->def, values, expire_at, cursor->table->pager, new_val_buf, &new_val_size);

  /* Build list of all cells (existing + new) */
  for (uint32_t i = 0; i < total_cells; i++) {
    if (i == cursor->cell_num) {
      temp_cells[i].size = new_val_size;
      temp_cells[i].data = malloc(new_val_size);
      memcpy(temp_cells[i].data, new_val_buf, new_val_size);
    } else {
      uint32_t src_idx = (i < cursor->cell_num) ? i : i - 1;
      PageSlot* src_slot = leaf_node_slot(old_node, src_idx);
      temp_cells[i].size = src_slot->size;
      temp_cells[i].data = malloc(src_slot->size);
      memcpy(temp_cells[i].data, (uint8_t*)old_node + src_slot->offset, src_slot->size);
    }
  }

  /* Divide cells evenly by count */
  uint32_t lsplit = total_cells / 2;
  uint32_t rsplit = total_cells - lsplit;

  /* Re-initialize old_node and new_node to wipe existing slots and data */
  bool was_root = is_node_root(old_node);
  initialize_leaf_node(old_node);
  if (was_root) set_node_root(old_node, true);
  *leaf_node_next_leaf(old_node) = new_page;

  /* Populate left child (old_node) */
  uint16_t old_offset = 4096;
  for (uint32_t i = 0; i < lsplit; i++) {
    old_offset -= temp_cells[i].size;
    memcpy((uint8_t*)old_node + old_offset, temp_cells[i].data, temp_cells[i].size);
    PageSlot* slot = leaf_node_slot(old_node, i);
    slot->offset = old_offset;
    slot->size = temp_cells[i].size;
  }
  *leaf_node_num_cells(old_node) = lsplit;
  *leaf_node_free_space(old_node) = old_offset;

  /* Populate right child (new_node) */
  uint16_t new_offset = 4096;
  for (uint32_t i = 0; i < rsplit; i++) {
    uint32_t temp_idx = lsplit + i;
    new_offset -= temp_cells[temp_idx].size;
    memcpy((uint8_t*)new_node + new_offset, temp_cells[temp_idx].data, temp_cells[temp_idx].size);
    PageSlot* slot = leaf_node_slot(new_node, i);
    slot->offset = new_offset;
    slot->size = temp_cells[temp_idx].size;
  }
  *leaf_node_num_cells(new_node) = rsplit;
  *leaf_node_free_space(new_node) = new_offset;

  /* Clean up temp cells */
  for (uint32_t i = 0; i < total_cells; i++) {
    free(temp_cells[i].data);
  }
  free(temp_cells);

  /* Insert right child page pointer into parent */
  if (is_node_root(old_node)) {
    create_new_root(cursor->table, new_page);
  } else {
    uint32_t pp = *node_parent(old_node);
    void* parent = get_page(cursor->table->pager, pp);
    
    uint8_t new_old_max[INTERNAL_NODE_KEY_SIZE];
    get_node_max_key(cursor->table->pager, old_node, cursor->table->def, new_old_max);
    update_internal_node_key(parent, old_max, new_old_max, cursor->table->def);
    internal_node_insert(cursor->table, pp, new_page);
  }
}

void btree_insert_with_ttl(Cursor* cursor, Value* values, uint64_t expire_at) {
  leaf_node_insert(cursor, values, expire_at);
}

void btree_insert(Cursor* cursor, Value* values) {
  btree_insert_with_ttl(cursor, values, 0);
}

/* ── Slotted Borrows & Merges ────────────────────────────────────────────── */
static uint32_t find_child_index(void* parent, uint32_t child_page) {
  uint32_t n = *internal_node_num_keys(parent);
  for (uint32_t i = 0; i <= n; i++)
    if (*internal_node_child(parent, i) == child_page) return i;
  fprintf(stderr, "Error: child page not found in parent\n");
  exit(EXIT_FAILURE);
}

static void leaf_borrow_from_left(Table* t, void* self, void* left, uint32_t parent_page, uint32_t self_idx) {
  uint32_t lc = *leaf_node_num_cells(left);
  uint32_t sc = *leaf_node_num_cells(self);
  void* parent = get_page(t->pager, parent_page);
  uint32_t npk = *internal_node_num_keys(parent);

  /* Copy last cell of left to temp */
  PageSlot* left_last_slot = leaf_node_slot(left, lc - 1);
  uint16_t size = left_last_slot->size;
  uint8_t data_buf[PAGE_SIZE];
  memcpy(data_buf, leaf_node_value(left, lc - 1), size);

  /* Defragment self if needed */
  uint16_t free_space = *leaf_node_free_space(self);
  uint32_t slots_end = LEAF_NODE_HEADER_SIZE + sc * sizeof(PageSlot);
  uint32_t needed = sizeof(PageSlot) + size;

  if (free_space < slots_end || (free_space - slots_end) < needed) {
    leaf_node_defragment(self);
    free_space = *leaf_node_free_space(self);
  }

  /* Shift self's slots right */
  for (uint32_t i = sc; i > 0; i--) {
    *leaf_node_slot(self, i) = *leaf_node_slot(self, i - 1);
  }

  /* Copy left cell data to self bottom */
  *leaf_node_free_space(self) -= size;
  uint16_t new_offset = *leaf_node_free_space(self);
  memcpy((uint8_t*)self + new_offset, data_buf, size);

  /* Write slot 0 */
  PageSlot* slot = leaf_node_slot(self, 0);
  slot->offset = new_offset;
  slot->size = size;

  (*leaf_node_num_cells(left))--;
  (*leaf_node_num_cells(self))++;

  uint8_t left_max[INTERNAL_NODE_KEY_SIZE];
  get_node_max_key(t->pager, left, t->def, left_max);
  memcpy(internal_node_key(parent, self_idx - 1), left_max, INTERNAL_NODE_KEY_SIZE);

  if (self_idx < npk) {
    uint8_t self_max[INTERNAL_NODE_KEY_SIZE];
    get_node_max_key(t->pager, self, t->def, self_max);
    memcpy(internal_node_key(parent, self_idx), self_max, INTERNAL_NODE_KEY_SIZE);
  }
}

static void leaf_borrow_from_right(Table* t, void* self, void* right, uint32_t parent_page, uint32_t self_idx) {
  uint32_t rc = *leaf_node_num_cells(right);
  uint32_t sc = *leaf_node_num_cells(self);
  void* parent = get_page(t->pager, parent_page);

  /* Copy first cell of right to temp */
  PageSlot* right_first_slot = leaf_node_slot(right, 0);
  uint16_t size = right_first_slot->size;
  uint8_t data_buf[PAGE_SIZE];
  memcpy(data_buf, leaf_node_value(right, 0), size);

  /* Defragment self if needed */
  uint16_t free_space = *leaf_node_free_space(self);
  uint32_t slots_end = LEAF_NODE_HEADER_SIZE + sc * sizeof(PageSlot);
  uint32_t needed = sizeof(PageSlot) + size;

  if (free_space < slots_end || (free_space - slots_end) < needed) {
    leaf_node_defragment(self);
    free_space = *leaf_node_free_space(self);
  }

  /* Copy right cell data to self bottom */
  *leaf_node_free_space(self) -= size;
  uint16_t new_offset = *leaf_node_free_space(self);
  memcpy((uint8_t*)self + new_offset, data_buf, size);

  /* Append slot to self */
  PageSlot* slot = leaf_node_slot(self, sc);
  slot->offset = new_offset;
  slot->size = size;

  /* Shift right's slots left */
  for (uint32_t i = 0; i < rc - 1; i++) {
    *leaf_node_slot(right, i) = *leaf_node_slot(right, i + 1);
  }

  (*leaf_node_num_cells(self))++;
  (*leaf_node_num_cells(right))--;

  uint8_t self_max[INTERNAL_NODE_KEY_SIZE];
  get_node_max_key(t->pager, self, t->def, self_max);
  memcpy(internal_node_key(parent, self_idx), self_max, INTERNAL_NODE_KEY_SIZE);
}

static void leaf_merge(Table* t, void* left, void* right, uint32_t parent_page, uint32_t right_idx) {
  uint32_t rc = *leaf_node_num_cells(right);

  /* Copy all cells from right to left */
  for (uint32_t i = 0; i < rc; i++) {
    uint32_t lc = *leaf_node_num_cells(left);
    PageSlot* rslot = leaf_node_slot(right, i);
    uint16_t size = rslot->size;

    /* Defragment left if needed */
    uint16_t free_space = *leaf_node_free_space(left);
    uint32_t slots_end = LEAF_NODE_HEADER_SIZE + lc * sizeof(PageSlot);
    uint32_t needed = sizeof(PageSlot) + size;

    if (free_space < slots_end || (free_space - slots_end) < needed) {
      leaf_node_defragment(left);
      free_space = *leaf_node_free_space(left);
    }

    /* Copy data to left bottom */
    *leaf_node_free_space(left) -= size;
    uint16_t new_offset = *leaf_node_free_space(left);
    memcpy((uint8_t*)left + new_offset, leaf_node_value(right, i), size);

    /* Set slot in left */
    PageSlot* slot = leaf_node_slot(left, lc);
    slot->offset = new_offset;
    slot->size = size;

    (*leaf_node_num_cells(left))++;
  }

  *leaf_node_next_leaf(left) = *leaf_node_next_leaf(right);
  *leaf_node_num_cells(right) = 0;

  internal_remove_child(t, parent_page, right_idx);
}

void leaf_node_handle_underflow(Table* t, uint32_t page_num) {
  void* node  = get_page(t->pager, page_num);
  if (is_node_root(node)) return;
  if (*leaf_node_num_cells(node) >= LEAF_NODE_MIN_CELLS) return;

  uint32_t pp  = *node_parent(node);
  void* parent = get_page(t->pager, pp);
  uint32_t si  = find_child_index(parent, page_num);
  uint32_t nk  = *internal_node_num_keys(parent);

  if (si > 0) {
    uint32_t lp = *internal_node_child(parent, si-1);
    void* left  = get_page(t->pager, lp);
    if (*leaf_node_num_cells(left) > LEAF_NODE_MIN_CELLS)
      leaf_borrow_from_left(t, node, left, pp, si);
    else
      leaf_merge(t, left, node, pp, si);
    return;
  }
  if (si < nk) {
    uint32_t rp  = *internal_node_child(parent, si+1);
    void* right  = get_page(t->pager, rp);
    if (*leaf_node_num_cells(right) > LEAF_NODE_MIN_CELLS)
      leaf_borrow_from_right(t, node, right, pp, si);
    else
      leaf_merge(t, node, right, pp, si+1);
  }
}

/* ── Internal Node Borrows & Merges ──────────────────────────────────────── */
static void internal_borrow_from_left(Table* t, void* self, uint32_t self_page,
    void* left, uint32_t parent_page, uint32_t self_idx) {
  uint32_t ln = *internal_node_num_keys(left);
  void* parent = get_page(t->pager, parent_page);
  uint32_t mcp  = *internal_node_right_child(left);
  uint32_t sn   = *internal_node_num_keys(self);

  for (uint32_t i = sn; i > 0; i--)
    memcpy(internal_node_cell(self, i), internal_node_cell(self, i-1), INTERNAL_NODE_CELL_SIZE);
  (*internal_node_num_keys(self))++;
  *internal_node_child(self, 0) = mcp;
  memcpy(internal_node_key(self, 0), internal_node_key(parent, self_idx-1), INTERNAL_NODE_KEY_SIZE);

  void* mc = get_page(t->pager, mcp);
  *node_parent(mc) = self_page;

  *internal_node_right_child(left) = *internal_node_child(left, ln-1);
  (*internal_node_num_keys(left))--;
  memcpy(internal_node_key(parent, self_idx-1), internal_node_key(left, ln-1), INTERNAL_NODE_KEY_SIZE);
}

static void internal_borrow_from_right(Table* t, void* self, uint32_t self_page,
    void* right, uint32_t parent_page, uint32_t self_idx) {
  uint32_t rn  = *internal_node_num_keys(right);
  void* parent = get_page(t->pager, parent_page);
  uint32_t mcp = *internal_node_child(right, 0);
  uint32_t cur_r = *internal_node_right_child(self);

  (*internal_node_num_keys(self))++;
  uint32_t nl = *internal_node_num_keys(self)-1;
  *internal_node_child(self, nl) = cur_r;
  memcpy(internal_node_key(self, nl), internal_node_key(parent, self_idx), INTERNAL_NODE_KEY_SIZE);
  *internal_node_right_child(self) = mcp;

  void* mc = get_page(t->pager, mcp);
  *node_parent(mc) = self_page;

  for (uint32_t i = 0; i < rn-1; i++)
    memcpy(internal_node_cell(right, i), internal_node_cell(right, i+1), INTERNAL_NODE_CELL_SIZE);
  (*internal_node_num_keys(right))--;
  memcpy(internal_node_key(parent, self_idx), internal_node_key(right, 0), INTERNAL_NODE_KEY_SIZE);
}

static void internal_merge_nodes(Table* t, void* left, uint32_t left_page,
    void* right, uint32_t parent_page, uint32_t right_idx) {
  void* parent    = get_page(t->pager, parent_page);
  uint32_t ln     = *internal_node_num_keys(left);
  uint32_t rn     = *internal_node_num_keys(right);

  uint32_t* slot  = internal_node_child(left, ln);
  slot[0] = *internal_node_right_child(left);
  memcpy(internal_node_key(left, ln), internal_node_key(parent, right_idx-1), INTERNAL_NODE_KEY_SIZE);

  for (uint32_t i = 0; i < rn; i++) {
    uint32_t cp  = *internal_node_child(right, i);
    *internal_node_child(left, ln + 1 + i) = cp;
    memcpy(internal_node_key(left, ln + 1 + i), internal_node_key(right, i), INTERNAL_NODE_KEY_SIZE);
    void* ch = get_page(t->pager, cp);
    *node_parent(ch) = left_page;
  }

  uint32_t rrc = *internal_node_right_child(right);
  *internal_node_right_child(left) = rrc;
  void* rrcn = get_page(t->pager, rrc);
  *node_parent(rrcn) = left_page;

  *internal_node_num_keys(left)  = ln + 1 + rn;
  *internal_node_num_keys(right) = 0;
  internal_remove_child(t, parent_page, right_idx);
}

void internal_remove_child(Table* t, uint32_t parent_page, uint32_t child_idx) {
  void* parent    = get_page(t->pager, parent_page);
  uint32_t nk     = *internal_node_num_keys(parent);

  if (child_idx == nk) {
    *internal_node_right_child(parent) = *internal_node_child(parent, nk-1);
    (*internal_node_num_keys(parent))--;
  } else {
    if (child_idx > 0)
      memcpy(internal_node_key(parent, child_idx-1), internal_node_key(parent, child_idx), INTERNAL_NODE_KEY_SIZE);
    for (uint32_t i = child_idx; i < nk-1; i++)
      memcpy(internal_node_cell(parent, i), internal_node_cell(parent, i+1), INTERNAL_NODE_CELL_SIZE);
    (*internal_node_num_keys(parent))--;
  }

  nk = *internal_node_num_keys(parent);

  if (is_node_root(parent) && nk == 0) {
    uint32_t ocp = *internal_node_right_child(parent);
    void* oc = get_page(t->pager, ocp);
    memcpy(parent, oc, PAGE_SIZE);
    set_node_root(parent, true);
    if (get_node_type(parent) == NODE_INTERNAL) {
      uint32_t n = *internal_node_num_keys(parent);
      for (uint32_t i = 0; i <= n; i++) {
        void* gc = get_page(t->pager, *internal_node_child(parent, i));
        *node_parent(gc) = t->def->root_page_num;
      }
    }
    return;
  }

  if (!is_node_root(parent) && nk < INTERNAL_NODE_MIN_KEYS)
    handle_internal_underflow(t, parent_page);
}

void handle_internal_underflow(Table* t, uint32_t page_num) {
  void* node = get_page(t->pager, page_num);
  if (is_node_root(node)) return;
  if (*internal_node_num_keys(node) >= INTERNAL_NODE_MIN_KEYS) return;

  uint32_t pp  = *node_parent(node);
  void* parent = get_page(t->pager, pp);
  uint32_t si  = find_child_index(parent, page_num);
  uint32_t nk  = *internal_node_num_keys(parent);

  if (si > 0) {
    uint32_t lp = *internal_node_child(parent, si-1);
    void* left  = get_page(t->pager, lp);
    if (*internal_node_num_keys(left) > INTERNAL_NODE_MIN_KEYS)
      internal_borrow_from_left(t, node, page_num, left, pp, si);
    else
      internal_merge_nodes(t, left, lp, node, pp, si);
    return;
  }
  if (si < nk) {
    uint32_t rp = *internal_node_child(parent, si+1);
    void* right = get_page(t->pager, rp);
    if (*internal_node_num_keys(right) > INTERNAL_NODE_MIN_KEYS)
      internal_borrow_from_right(t, node, page_num, right, pp, si);
    else
      internal_merge_nodes(t, node, page_num, right, pp, si+1);
  }
}

/* ── Slotted Leaf Node Delete ────────────────────────────────────────────── */
void btree_delete(Cursor* cursor) {
  void* node  = get_page(cursor->table->pager, cursor->page_num);
  uint32_t nc = *leaf_node_num_cells(node);
  uint32_t ci = cursor->cell_num;

  PageSlot* slot = leaf_node_slot(node, ci);
  if (slot->size >= BTREE_MAX_LOCAL_PAYLOAD) {
    uint8_t* cell_data = (uint8_t*)node + slot->offset;
    uint32_t total_size = 0;
    uint32_t first_overflow_page = 0;
    memcpy(&total_size, cell_data, 4);
    memcpy(&first_overflow_page, cell_data + 4, 4);
    if (total_size > slot->size && first_overflow_page != 0) {
      btree_free_overflow_chain(cursor->table->pager, first_overflow_page);
    }
  }

  uint8_t old_max[INTERNAL_NODE_KEY_SIZE];
  get_node_max_key(cursor->table->pager, node, cursor->table->def, old_max);
  bool was_max = (ci == nc - 1);

  /* Shift slots left in the slot array */
  for (uint32_t i = ci; i < nc-1; i++) {
    *leaf_node_slot(node, i) = *leaf_node_slot(node, i + 1);
  }
  (*leaf_node_num_cells(node))--;

  /* Update parent separator if max changed */
  if (was_max && !is_node_root(node) && *leaf_node_num_cells(node) > 0) {
    uint32_t pp  = *node_parent(node);
    void* parent = get_page(cursor->table->pager, pp);
    if (cursor->page_num != *internal_node_right_child(parent)) {
      uint8_t new_max[INTERNAL_NODE_KEY_SIZE];
      get_node_max_key(cursor->table->pager, node, cursor->table->def, new_max);
      update_internal_node_key(parent, old_max, new_max, cursor->table->def);
    }
  }

  leaf_node_handle_underflow(cursor->table, cursor->page_num);
}
