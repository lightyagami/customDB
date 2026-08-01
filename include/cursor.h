#pragma once
#include "common.h"
#include "catalog.h"
#include "pager.h"

/* A Table is a live handle: pager + pointer into the catalog's TableDef.
 * root_page_num is read/written through def->root_page_num so the catalog
 * stays in sync automatically. */
typedef struct {
  Pager*    pager;
  TableDef* def;     /* points into Catalog.tables[] */
} Table;

typedef struct {
  Table*   table;
  uint32_t page_num;
  uint32_t cell_num;
  bool     end_of_table;
} Cursor;
