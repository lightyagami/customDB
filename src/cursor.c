#include "btree.h"

/* cursor.c: Table lifecycle. Cursor ops live in btree.c (btree_find, etc.) */

Table* table_open(TableDef* def, Pager* pager) {
  Table* t  = malloc(sizeof(Table));
  t->pager  = pager;
  t->def    = def;
  return t;
}

void table_close(Table* t) {
  free(t);
}
