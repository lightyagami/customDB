#pragma once
#include "common.h"
#include "catalog.h"
#include "pager.h"
#include "btree.h"
#include "parser.h"

typedef enum {
  OP_Transaction,
  OP_Integer,
  OP_String,
  OP_Double,
  OP_OpenRead,
  OP_OpenWrite,
  OP_Rewind,
  OP_Next,
  OP_SeekGE,
  OP_SeekGT,
  OP_Column,
  OP_Compare,
  OP_IfFalse,
  OP_ResultRow,
  OP_Insert,
  OP_Delete,
  OP_SorterInsert,
  OP_SorterSort,
  OP_SorterRewind,
  OP_SorterNext,
  OP_Dec,
  OP_Add,
  OP_Subtract,
  OP_Multiply,
  OP_Divide,
  OP_Remainder,
  OP_Null,
  OP_Halt
} Opcode;

typedef struct {
  Opcode   op;
  int      p1;   /* Register index, cursor index, or write_flag */
  int      p2;   /* Column index or jump target PC */
  int      p3;   /* Dest register index or compare op (CompOp) */
  Value    p4;   /* Immediate value payload (if any) */
} Instruction;

#define MAX_REGISTERS 16
#define MAX_CURSORS   4

typedef struct {
  Value         row_values[MAX_COLUMNS];
  Value         sort_key;
  ColumnType    sort_type;
  CollationType sort_collation;
  bool          sort_desc;
} SorterEntry;

typedef struct {
  SorterEntry* entries;
  uint32_t     count;
  uint32_t     capacity;
  uint32_t     cursor_idx;
} VmSorter;

typedef struct {
  Value      val;
  ColumnType type;
  bool       is_null;
} Register;

typedef struct {
  Cursor*   btree_cursor;
  TableDef  def;
  bool      is_open;
  Table     table_handle;
} VmCursor;

typedef struct {
  Instruction* insts;
  uint32_t     num_insts;
  uint32_t     max_insts;
  Register     regs[MAX_REGISTERS];
  VmCursor     cursors[MAX_CURSORS];
  Pager*       pager;
  Catalog*     catalog;
  bool         cmp_result;
  uint32_t     pc;
  VmSorter     sorter;
} Vdbe;

Vdbe* vdbe_create(Pager* pager, Catalog* catalog);
void  vdbe_free(Vdbe* vm);
void  vdbe_add_inst(Vdbe* vm, Opcode op, int p1, int p2, int p3, Value p4);
void  vdbe_run(Vdbe* vm);
void  vdbe_print_program(Vdbe* vm);
