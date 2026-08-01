# VDBE Bytecode VM

This document details the register-based Virtual Database Engine (VDBE) and register allocation patterns.

---

## VM Execution Model

The VDBE is a register-based bytecode interpreter. Its state is stored in a `Vdbe` struct containing:
- An array of memory registers (`regs`), capable of storing variables of any type (manifest typing).
- An array of open cursor structures (`cursors`) to manage active B+ Tree table and index scans.
- An in-memory transient sorter structure (`sorter`) to handle output sorting.

---

## Instruction Format

Every bytecode instruction is defined by `VdbeOp`:
```c
typedef struct {
  Opcode   op;      /* Opcode name */
  int      p1;      /* First parameter */
  int      p2;      /* Second parameter */
  int      p3;      /* Third parameter */
  union {
    int      int_val;
    double   double_val;
    char     text_val[256];
  } p4;             /* Constant payload parameter */
} VdbeOp;
```

---

## Code Generation & Register Allocation

During query compilation inside [executor.c](file:///home/venomsnake/Downloads/DBMS/src/executor.c), registers are allocated systematically:
- **`Registers 1 to N`**: Staging area for table/index columns (where $N$ is the column count).
- **`Register 11 (zero_reg)`**: Holds constant integer `0`.
- **`Register 12 (limit_reg)`**: Keeps track of remaining row counts for `LIMIT`.
- **`Register 14 (pk_reg)`**: Holds primary key lookups during secondary-index-to-main-table joins.
- **`Register 15 (seek_reg)`**: Stores boundary search values for range selections.

---

## Opcode Directory

The VM executes a diverse instruction set categorized by function:

### 1. Database & Cursor Opcodes
- **`OP_OpenRead (P1, P2)`**: Opens a B+ Tree table cursor on cursor slot `P1` at root page `P2`.
- **`OP_OpenWrite (P1, P2)`**: Opens a write-enabled cursor on slot `P1` at root page `P2`.
- **`OP_Rewind (P1, P2)`**: Positions cursor `P1` at the beginning of the table. If table is empty, jumps to `P2`.
- **`OP_Next (P1, P2)`**: Advances cursor `P1`. If there are more rows, jumps to `P2`.
- **`OP_SeekGE (P1, P2, P3)`**: Seeks cursor `P1` to the first cell containing a key $\ge$ value in register `P3`. If no match, jumps to `P2`.
- **`OP_Column (P1, P2, P3)`**: Decodes the row at cursor `P1`, extracts column `P2`, and stores the value in register `P3`.

### 2. Sorter & Output Control Opcodes
- **`OP_SorterInsert (P1, P2, P3)`**: Extracts a row from registers starting at `P2` containing `P3` columns, and stores it in the transient sorter array. Uses register `P1` as the sorting key.
- **`OP_SorterSort`**: Performs an in-memory `qsort` on the accumulated sorter rows. Automatically uses type-aware comparison rules based on the sort key type.
- **`OP_SorterRewind (P1, P2)`**: Prepares to iterate the sorted rows. If empty, jumps to `P2`.
- **`OP_SorterNext (P1, P2)`**: Advances to the next sorted row. If more rows exist, jumps to `P2`.
- **`OP_Dec (P1, P2)`**: Decrements limit counter register `P1`. If the register value reaches `0`, jumps to instruction `P2` to terminate the query.
- **`OP_ResultRow (P1, P2)`**: Outputs the current values stored in registers `P1` to `P2` to the user terminal.
- **`OP_Halt`**: Terminates program execution.
