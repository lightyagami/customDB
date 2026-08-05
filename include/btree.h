#pragma once
#include "cursor.h"

/* ── Node initialisation ─────────────────────────────────────────────────── */
void initialize_leaf_node(void* node);
void initialize_internal_node(void* node);
void initialize_root_leaf(void* node);

/* ── B+ Tree core operations ─────────────────────────────────────────────── */
Cursor* btree_find(Table* table, Value* key_value);
void    btree_find_out(Table* table, Value* key_value, Cursor* out_cursor);
Cursor* btree_start(Table* table);
void    btree_start_out(Table* table, Cursor* out_cursor);

/* Insert a new row */
void    btree_insert(Cursor* cursor, Value* values);

/* Delete the row the cursor currently points at */
void    btree_delete(Cursor* cursor);

/* ── Cursor navigation ───────────────────────────────────────────────────── */
void*   cursor_value(Cursor* cursor);   /* ptr to raw serialised row bytes  */
void    cursor_advance(Cursor* cursor);
void    btree_key_value(Cursor* cursor, Value* out_val);

/* ── Utility ─────────────────────────────────────────────────────────────── */
int      compare_keys(ColumnType type, const void* k1, const void* k2);
int      compare_values(ColumnType type, const Value* v1, const Value* v2);
void     print_tree(Pager* pager, uint32_t page_num, uint32_t indent,
                    TableDef* def);
uint32_t get_unused_page_num(Pager* pager);
