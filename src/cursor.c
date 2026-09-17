#include "btree.h"

Table* table_open(TableDef* def, Pager* pager) {
  Table* t  = malloc(sizeof(Table));
  if (!t) return NULL;
  t->pager  = pager;
  t->def    = def;
  return t;
}

void table_close(Table* t) {
  free(t);
}
