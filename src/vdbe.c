#include "vdbe.h"

Vdbe* vdbe_create(Pager* pager, Catalog* catalog) {
  Vdbe* vm = malloc(sizeof(Vdbe));
  memset(vm, 0, sizeof(Vdbe));
  vm->pager = pager;
  vm->catalog = catalog;
  vm->max_insts = 16;
  vm->insts = malloc(sizeof(Instruction) * vm->max_insts);
  return vm;
}

static int compare_sorter_entries(const void* a, const void* b) {
  const SorterEntry* sa = (const SorterEntry*)a;
  const SorterEntry* sb = (const SorterEntry*)b;
  ColumnType type = sa->sort_type;
  int cmp = 0;

  if (type == COL_INT) {
    int32_t va = sa->sort_key.int_val;
    int32_t vb = sb->sort_key.int_val;
    cmp = (va > vb) - (va < vb);
  } else if (type == COL_DOUBLE || type == COL_FLOAT) {
    double va = (type == COL_DOUBLE) ? sa->sort_key.double_val : sa->sort_key.float_val;
    double vb = (type == COL_DOUBLE) ? sb->sort_key.double_val : sb->sort_key.float_val;
    cmp = (va > vb) - (va < vb);
  } else if (type == COL_BOOL) {
    bool va = sa->sort_key.bool_val;
    bool vb = sb->sort_key.bool_val;
    cmp = (va > vb) - (va < vb);
  } else {
    cmp = compare_strings_collated(sa->sort_key.text_val, sb->sort_key.text_val, sa->sort_collation);
  }

  if (sa->sort_desc) {
    return -cmp;
  }
  return cmp;
}

void vdbe_free(Vdbe* vm) {
  if (!vm) return;
  for (uint32_t c = 0; c < MAX_CURSORS; c++) {
    if (vm->cursors[c].is_open) {
      if (vm->cursors[c].btree_cursor) {
        free(vm->cursors[c].btree_cursor);
        vm->cursors[c].btree_cursor = NULL;
      }
    }
  }
  for (uint32_t r = 0; r < MAX_REGISTERS; r++) {
    value_free(&vm->regs[r].val);
  }
  if (vm->sorter.entries) {
    for (uint32_t s = 0; s < vm->sorter.count; s++) {
      value_free(&vm->sorter.entries[s].sort_key);
      value_free_row(vm->sorter.entries[s].row_values, MAX_COLUMNS);
    }
    free(vm->sorter.entries);
  }
  if (vm->insts) {
    for (uint32_t i = 0; i < vm->num_insts; i++) {
      value_free(&vm->insts[i].p4);
    }
    free(vm->insts);
  }
  free(vm);
}

void vdbe_add_inst(Vdbe* vm, Opcode op, int p1, int p2, int p3, Value p4) {
  if (vm->num_insts >= vm->max_insts) {
    vm->max_insts *= 2;
    vm->insts = realloc(vm->insts, sizeof(Instruction) * vm->max_insts);
  }
  Instruction* i = &vm->insts[vm->num_insts++];
  memset(i, 0, sizeof(Instruction));
  i->op = op;
  i->p1 = p1;
  i->p2 = p2;
  i->p3 = p3;
  value_copy(&i->p4, &p4);
}

void vdbe_run(Vdbe* vm) {
  vm->pc = 0;
  bool halted = false;

  while (!halted && vm->pc < vm->num_insts) {
    Instruction* i = &vm->insts[vm->pc];
    vm->pc++; // Increment PC by default

    switch (i->op) {
      case OP_Transaction: {
        if (i->p1 == 1) {
          pager_begin_transaction(vm->pager);
        } else if (i->p1 == 2) {
          catalog_save(vm->catalog, vm->pager);
          pager_commit(vm->pager);
        } else if (i->p1 == 3) {
          pager_rollback(vm->pager);
        }
        break;
      }
      case OP_Integer: {
        value_free(&vm->regs[i->p3].val);
        vm->regs[i->p3].val.int_val = i->p1;
        vm->regs[i->p3].type = COL_INT;
        vm->regs[i->p3].is_null = false;
        break;
      }
      case OP_String: {
        value_copy(&vm->regs[i->p3].val, &i->p4);
        vm->regs[i->p3].type = COL_TEXT;
        vm->regs[i->p3].is_null = false;
        break;
      }
      case OP_Double: {
        value_copy(&vm->regs[i->p3].val, &i->p4);
        vm->regs[i->p3].type = COL_DOUBLE;
        vm->regs[i->p3].is_null = false;
        break;
      }
      case OP_OpenRead:
      case OP_OpenWrite: {
        uint32_t cursor_idx = i->p1;
        uint32_t root_page = i->p2;
        char* tbl_name = i->p4.text_val;
        VmCursor* vc = &vm->cursors[cursor_idx];

        if (!vc->is_open) {
          bool found = false;
          if (strncmp(tbl_name, "_idx_", 5) == 0) {
            /* Resolve synthetic index TableDef dynamically */
            for (uint32_t t = 0; t < vm->catalog->num_tables; t++) {
              TableDef* mdef = &vm->catalog->tables[t];
              for (uint32_t c = 1; c < mdef->num_cols; c++) {
                char expected[128];
                snprintf(expected, sizeof(expected), "_idx_%s_%s", mdef->name, mdef->columns[c].name);
                if (strcmp(tbl_name, expected) == 0) {
                  memset(&vc->def, 0, sizeof(TableDef));
                  snprintf(vc->def.name, sizeof(vc->def.name), "%.*s", (int)sizeof(vc->def.name) - 1, tbl_name);
                  vc->def.root_page_num = root_page;
                  vc->def.num_cols = 2;
                  memcpy(&vc->def.columns[0], &mdef->columns[c], sizeof(Column));
                  strcpy(vc->def.columns[1].name, "id");
                  vc->def.columns[1].type = COL_INT;
                  vc->def.columns[1].size = 4;
                  tabledef_compute(&vc->def);
                  found = true;
                  break;
                }
              }
              if (found) break;
            }
          } else {
            TableDef* catalog_def = catalog_find(vm->catalog, tbl_name);
            if (catalog_def) {
              memcpy(&vc->def, catalog_def, sizeof(TableDef));
              found = true;
            }
          }

          if (!found) {
            fprintf(stderr, "VM Error: Table or index '%s' not found.\n", tbl_name);
            exit(1);
          }

          vc->table_handle.pager = vm->pager;
          vc->table_handle.def = &vc->def;
          vc->btree_cursor = NULL;
          vc->is_open = true;
        }
        break;
      }
      case OP_Rewind: {
        uint32_t cursor_idx = i->p1;
        uint32_t jump_pc = i->p2;
        VmCursor* vc = &vm->cursors[cursor_idx];
        if (vc->btree_cursor) free(vc->btree_cursor);
        vc->btree_cursor = btree_start(&vc->table_handle);
        if (vc->btree_cursor->end_of_table) {
          vm->pc = jump_pc;
        }
        break;
      }
      case OP_Next: {
        uint32_t cursor_idx = i->p1;
        uint32_t jump_pc = i->p2;
        VmCursor* vc = &vm->cursors[cursor_idx];
        cursor_advance(vc->btree_cursor);
        if (vc->btree_cursor->end_of_table) {
          /* Do not jump, proceed to next instruction */
        } else {
          vm->pc = jump_pc;
        }
        break;
      }
      case OP_SeekGE: {
        uint32_t cursor_idx = i->p1;
        uint32_t jump_pc = i->p2;
        uint32_t reg_idx = i->p3;
        VmCursor* vc = &vm->cursors[cursor_idx];
        if (vc->btree_cursor) free(vc->btree_cursor);
        
        vc->btree_cursor = btree_find(&vc->table_handle, &vm->regs[reg_idx].val);
        void* node = get_page(vm->pager, vc->btree_cursor->page_num);
        uint32_t num_cells = *(uint32_t*)((uint8_t*)node + 6);
        if (vc->btree_cursor->cell_num >= num_cells) {
          vm->pc = jump_pc;
        }
        break;
      }
      case OP_SeekGT: {
        uint32_t cursor_idx = i->p1;
        uint32_t jump_pc = i->p2;
        uint32_t reg_idx = i->p3;
        VmCursor* vc = &vm->cursors[cursor_idx];
        if (vc->btree_cursor) free(vc->btree_cursor);
        
        vc->btree_cursor = btree_find(&vc->table_handle, &vm->regs[reg_idx].val);
        void* node = get_page(vm->pager, vc->btree_cursor->page_num);
        uint32_t num_cells = *(uint32_t*)((uint8_t*)node + 6);
        if (vc->btree_cursor->cell_num >= num_cells) {
          vm->pc = jump_pc;
        } else {
          /* Skip keys that are equal to register value */
          Value existing_val;
          btree_key_value(vc->btree_cursor, &existing_val);
          int cmp = compare_values(vc->def.columns[0].type, &existing_val, &vm->regs[reg_idx].val);
          if (cmp == 0) {
            cursor_advance(vc->btree_cursor);
            if (vc->btree_cursor->end_of_table) {
              vm->pc = jump_pc;
            }
          }
        }
        break;
      }
      case OP_Column: {
        uint32_t cursor_idx = i->p1;
        uint32_t col_idx = i->p2;
        uint32_t dest_reg = i->p3;
        VmCursor* vc = &vm->cursors[cursor_idx];
        Value row_vals[MAX_COLUMNS];
        deserialize_row(&vc->def, cursor_value(vc->btree_cursor), row_vals);
        value_copy(&vm->regs[dest_reg].val, &row_vals[col_idx]);
        vm->regs[dest_reg].type = vc->def.columns[col_idx].type;
        vm->regs[dest_reg].is_null = row_vals[col_idx].is_null;
        value_free_row(row_vals, vc->def.num_cols);
        break;
      }
      case OP_Null: {
        uint32_t dest_reg = i->p1;
        value_free(&vm->regs[dest_reg].val);
        vm->regs[dest_reg].is_null = true;
        vm->regs[dest_reg].val.is_null = true;
        break;
      }
      case OP_Compare: {
        uint32_t reg_a = i->p1;
        uint32_t reg_b = i->p2;
        CompOp comp_op = (CompOp)i->p3;
        Register* ra = &vm->regs[reg_a];
        Register* rb = &vm->regs[reg_b];

        if (comp_op == OP_IS_NULL) {
          vm->cmp_result = (ra->is_null || ra->val.is_null);
          break;
        }
        if (comp_op == OP_IS_NOT_NULL) {
          vm->cmp_result = !(ra->is_null || ra->val.is_null);
          break;
        }

        if (ra->is_null || ra->val.is_null || rb->is_null || rb->val.is_null) {
          vm->cmp_result = false;
          break;
        }

        int cmp = 0;
        if (ra->type == COL_INT || rb->type == COL_INT) {
          int32_t va = (ra->type == COL_INT) ? ra->val.int_val : (int32_t)ra->val.double_val;
          int32_t vb = (rb->type == COL_INT) ? rb->val.int_val : (int32_t)rb->val.double_val;
          cmp = (va > vb) - (va < vb);
        } else if (ra->type == COL_DOUBLE || rb->type == COL_DOUBLE || ra->type == COL_FLOAT || rb->type == COL_FLOAT ||
                   ra->type == COL_NUMERIC || rb->type == COL_NUMERIC || ra->type == COL_DECIMAL || rb->type == COL_DECIMAL) {
          double va = (ra->type == COL_FLOAT) ? (double)ra->val.float_val : (ra->type == COL_INT ? (double)ra->val.int_val : ra->val.double_val);
          double vb = (rb->type == COL_FLOAT) ? (double)rb->val.float_val : (rb->type == COL_INT ? (double)rb->val.int_val : rb->val.double_val);
          cmp = (va > vb) - (va < vb);
        } else if (ra->type == COL_BOOL || rb->type == COL_BOOL) {
          bool va = ra->val.bool_val;
          bool vb = rb->val.bool_val;
          cmp = (va > vb) - (va < vb);
        } else {
          CollationType coll = (CollationType)i->p4.int_val;
          cmp = compare_strings_collated(ra->val.text_val, rb->val.text_val, coll);
        }

        switch (comp_op) {
          case OP_EQ:  vm->cmp_result = (cmp == 0); break;
          case OP_GT:  vm->cmp_result = (cmp > 0); break;
          case OP_LT:  vm->cmp_result = (cmp < 0); break;
          case OP_GTE: vm->cmp_result = (cmp >= 0); break;
          case OP_LTE: vm->cmp_result = (cmp <= 0); break;
          case OP_MATCH: vm->cmp_result = (strstr(ra->val.text_val, rb->val.text_val) != NULL); break;
          default:     vm->cmp_result = false; break;
        }
        break;
      }
      case OP_IfFalse: {
        uint32_t jump_pc = i->p1;
        if (!vm->cmp_result) {
          vm->pc = jump_pc;
        }
        break;
      }
      case OP_ResultRow: {
        uint32_t start_reg = i->p1;
        uint32_t count = i->p2;
        printf("(");
        for (uint32_t r = 0; r < count; r++) {
          if (r > 0) printf(", ");
          Register* reg = &vm->regs[start_reg + r];
          if (reg->is_null || reg->val.is_null) {
            printf("NULL");
          } else {
            switch (reg->type) {
              case COL_INT:       printf("%d",  reg->val.int_val); break;
              case COL_FLOAT:     printf("%.4g", reg->val.double_val != 0.0 ? reg->val.double_val : (double)reg->val.float_val); break;
              case COL_DOUBLE:
              case COL_NUMERIC:
              case COL_DECIMAL:   printf("%.8g", reg->val.double_val); break;
              case COL_BOOL:      printf("%s",  reg->val.bool_val ? "true" : "false"); break;
              case COL_BLOB:
                if (strncasecmp(reg->val.text_val, "x'", 2) == 0 || strncasecmp(reg->val.text_val, "0x", 2) == 0) {
                  printf("%s", reg->val.text_val);
                } else {
                  printf("x'%s'", reg->val.text_val);
                }
                break;
              case COL_DATETIME:
              case COL_DATE:
              case COL_TIME:
              case COL_TIMESTAMP:
              case COL_TEXT:
              case COL_VARCHAR:   printf("%s",  reg->val.text_val); break;
            }
          }
        }
        printf(")\n");
        break;
      }
      case OP_Insert: {
        uint32_t cursor_idx = i->p1;
        uint32_t start_reg = i->p2;
        VmCursor* vc = &vm->cursors[cursor_idx];
        
        Value values[MAX_COLUMNS];
        memset(values, 0, sizeof(values));
        for (uint32_t c = 0; c < vc->def.num_cols; c++) {
          value_copy(&values[c], &vm->regs[start_reg + c].val);
          values[c].is_null = vm->regs[start_reg + c].is_null;
        }

        /* Seek and insert using stack Cursor */
        Cursor bcur;
        btree_find_out(&vc->table_handle, &values[0], &bcur);
        btree_insert(&bcur, values);
        value_free_row(values, vc->def.num_cols);
        break;
      }
      case OP_Delete: {
        uint32_t cursor_idx = i->p1;
        VmCursor* vc = &vm->cursors[cursor_idx];
        btree_delete(vc->btree_cursor);
        break;
      }
      case OP_SorterInsert: {
        int sort_key_reg = i->p1;
        int start_reg = i->p2;
        int num_cols = i->p3;
        
        if (vm->sorter.count >= vm->sorter.capacity) {
          vm->sorter.capacity = vm->sorter.capacity == 0 ? 16 : vm->sorter.capacity * 2;
          vm->sorter.entries = realloc(vm->sorter.entries, sizeof(SorterEntry) * vm->sorter.capacity);
        }
        SorterEntry* entry = &vm->sorter.entries[vm->sorter.count++];
        memset(entry, 0, sizeof(SorterEntry));
        for (int c = 0; c < num_cols; c++) {
          value_copy(&entry->row_values[c], &vm->regs[start_reg + c].val);
        }
        value_copy(&entry->sort_key, &vm->regs[sort_key_reg].val);
        entry->sort_type = vm->regs[sort_key_reg].type;
        entry->sort_collation = (CollationType)i->p4.int_val;
        break;
      }
      case OP_SorterSort: {
        int desc_flag = i->p1;
        if (vm->sorter.count > 0) {
          for (uint32_t e = 0; e < vm->sorter.count; e++) {
            vm->sorter.entries[e].sort_desc = (desc_flag != 0);
          }
          qsort(vm->sorter.entries, vm->sorter.count, sizeof(SorterEntry), compare_sorter_entries);
        }
        break;
      }
      case OP_SorterRewind: {
        uint32_t jump_pc = i->p1;
        int start_reg = i->p2;
        int num_cols = i->p3;
        
        vm->sorter.cursor_idx = 0;
        if (vm->sorter.count == 0) {
          vm->pc = jump_pc;
        } else {
          SorterEntry* entry = &vm->sorter.entries[0];
          for (int c = 0; c < num_cols; c++) {
            value_copy(&vm->regs[start_reg + c].val, &entry->row_values[c]);
          }
        }
        break;
      }
      case OP_SorterNext: {
        uint32_t jump_pc = i->p1;
        int start_reg = i->p2;
        int num_cols = i->p3;
        
        vm->sorter.cursor_idx++;
        if (vm->sorter.cursor_idx < vm->sorter.count) {
          SorterEntry* entry = &vm->sorter.entries[vm->sorter.cursor_idx];
          for (int c = 0; c < num_cols; c++) {
            value_copy(&vm->regs[start_reg + c].val, &entry->row_values[c]);
          }
          vm->pc = jump_pc;
        }
        break;
      }
      case OP_Dec: {
        vm->regs[i->p1].val.int_val--;
        break;
      }
      case OP_Add: {
        int reg1 = i->p1;
        int reg2 = i->p2;
        int dest = i->p3;
        Register* r1 = &vm->regs[reg1];
        Register* r2 = &vm->regs[reg2];
        Register* rd = &vm->regs[dest];
        if (r1->type == COL_INT && r2->type == COL_INT) {
          rd->val.int_val = r1->val.int_val + r2->val.int_val;
          rd->type = COL_INT;
        } else {
          double v1 = (r1->type == COL_INT) ? r1->val.int_val : (r1->type == COL_FLOAT ? r1->val.float_val : r1->val.double_val);
          double v2 = (r2->type == COL_INT) ? r2->val.int_val : (r2->type == COL_FLOAT ? r2->val.float_val : r2->val.double_val);
          rd->val.double_val = v1 + v2;
          rd->type = COL_DOUBLE;
        }
        rd->is_null = (r1->is_null || r2->is_null);
        break;
      }
      case OP_Subtract: {
        int reg1 = i->p1;
        int reg2 = i->p2;
        int dest = i->p3;
        Register* r1 = &vm->regs[reg1];
        Register* r2 = &vm->regs[reg2];
        Register* rd = &vm->regs[dest];
        if (r1->type == COL_INT && r2->type == COL_INT) {
          rd->val.int_val = r1->val.int_val - r2->val.int_val;
          rd->type = COL_INT;
        } else {
          double v1 = (r1->type == COL_INT) ? r1->val.int_val : (r1->type == COL_FLOAT ? r1->val.float_val : r1->val.double_val);
          double v2 = (r2->type == COL_INT) ? r2->val.int_val : (r2->type == COL_FLOAT ? r2->val.float_val : r2->val.double_val);
          rd->val.double_val = v1 - v2;
          rd->type = COL_DOUBLE;
        }
        rd->is_null = (r1->is_null || r2->is_null);
        break;
      }
      case OP_Multiply: {
        int reg1 = i->p1;
        int reg2 = i->p2;
        int dest = i->p3;
        Register* r1 = &vm->regs[reg1];
        Register* r2 = &vm->regs[reg2];
        Register* rd = &vm->regs[dest];
        if (r1->type == COL_INT && r2->type == COL_INT) {
          rd->val.int_val = r1->val.int_val * r2->val.int_val;
          rd->type = COL_INT;
        } else {
          double v1 = (r1->type == COL_INT) ? r1->val.int_val : (r1->type == COL_FLOAT ? r1->val.float_val : r1->val.double_val);
          double v2 = (r2->type == COL_INT) ? r2->val.int_val : (r2->type == COL_FLOAT ? r2->val.float_val : r2->val.double_val);
          rd->val.double_val = v1 * v2;
          rd->type = COL_DOUBLE;
        }
        rd->is_null = (r1->is_null || r2->is_null);
        break;
      }
      case OP_Divide: {
        int reg1 = i->p1;
        int reg2 = i->p2;
        int dest = i->p3;
        Register* r1 = &vm->regs[reg1];
        Register* r2 = &vm->regs[reg2];
        Register* rd = &vm->regs[dest];
        double v1 = (r1->type == COL_INT) ? r1->val.int_val : (r1->type == COL_FLOAT ? r1->val.float_val : r1->val.double_val);
        double v2 = (r2->type == COL_INT) ? r2->val.int_val : (r2->type == COL_FLOAT ? r2->val.float_val : r2->val.double_val);
        if (v2 == 0.0) {
          rd->is_null = true;
        } else {
          rd->val.double_val = v1 / v2;
          rd->type = COL_DOUBLE;
          rd->is_null = (r1->is_null || r2->is_null);
        }
        break;
      }
      case OP_Remainder: {
        int reg1 = i->p1;
        int reg2 = i->p2;
        int dest = i->p3;
        Register* r1 = &vm->regs[reg1];
        Register* r2 = &vm->regs[reg2];
        Register* rd = &vm->regs[dest];
        int v1 = (r1->type == COL_INT) ? r1->val.int_val : (int)r1->val.double_val;
        int v2 = (r2->type == COL_INT) ? r2->val.int_val : (int)r2->val.double_val;
        if (v2 == 0) {
          rd->is_null = true;
        } else {
          rd->val.int_val = v1 % v2;
          rd->type = COL_INT;
          rd->is_null = (r1->is_null || r2->is_null);
        }
        break;
      }
      case OP_Halt: {
        halted = true;
        break;
      }
    }
  }
}

void vdbe_print_program(Vdbe* vm) {
  printf("VDBE Program (%u instructions):\n", vm->num_insts);
  for (uint32_t i = 0; i < vm->num_insts; i++) {
    Instruction* inst = &vm->insts[i];
    printf("  %02u: ", i);
    switch (inst->op) {
      case OP_Transaction: printf("OP_Transaction p1=%d\n", inst->p1); break;
      case OP_Integer:     printf("OP_Integer p1=%d, p3=%d\n", inst->p1, inst->p3); break;
      case OP_String:      printf("OP_String p4='%s', p3=%d\n", inst->p4.text_val, inst->p3); break;
      case OP_Double:      printf("OP_Double p4=%f, p3=%d\n", inst->p4.double_val, inst->p3); break;
      case OP_OpenRead:    printf("OP_OpenRead cursor=%d, root_page=%d, tbl_name='%s'\n", inst->p1, inst->p2, inst->p4.text_val); break;
      case OP_OpenWrite:   printf("OP_OpenWrite cursor=%d, root_page=%d, tbl_name='%s'\n", inst->p1, inst->p2, inst->p4.text_val); break;
      case OP_Rewind:      printf("OP_Rewind cursor=%d, jump_target=%d\n", inst->p1, inst->p2); break;
      case OP_Next:        printf("OP_Next cursor=%d, jump_target=%d\n", inst->p1, inst->p2); break;
      case OP_SeekGE:      printf("OP_SeekGE cursor=%d, jump_target=%d, reg_key=%d\n", inst->p1, inst->p2, inst->p3); break;
      case OP_SeekGT:      printf("OP_SeekGT cursor=%d, jump_target=%d, reg_key=%d\n", inst->p1, inst->p2, inst->p3); break;
      case OP_Column:      printf("OP_Column cursor=%d, col_idx=%d, dest_reg=%d\n", inst->p1, inst->p2, inst->p3); break;
      case OP_Compare:     printf("OP_Compare reg_a=%d, reg_b=%d, op=%d\n", inst->p1, inst->p2, inst->p3); break;
      case OP_IfFalse:     printf("OP_IfFalse jump_target=%d\n", inst->p1); break;
      case OP_ResultRow:   printf("OP_ResultRow start_reg=%d, count=%d\n", inst->p1, inst->p2); break;
      case OP_Insert:      printf("OP_Insert cursor=%d, start_reg=%d\n", inst->p1, inst->p2); break;
      case OP_Delete:      printf("OP_Delete cursor=%d\n", inst->p1); break;
      case OP_SorterInsert: printf("OP_SorterInsert sort_key_reg=%d, start_reg=%d, num_cols=%d\n", inst->p1, inst->p2, inst->p3); break;
      case OP_SorterSort:   printf("OP_SorterSort desc_flag=%d\n", inst->p1); break;
      case OP_SorterRewind: printf("OP_SorterRewind jump_target=%d, start_reg=%d, num_cols=%d\n", inst->p1, inst->p2, inst->p3); break;
      case OP_SorterNext:   printf("OP_SorterNext jump_target=%d, start_reg=%d, num_cols=%d\n", inst->p1, inst->p2, inst->p3); break;
      case OP_Dec:         printf("OP_Dec reg=%d\n", inst->p1); break;
      case OP_Add:         printf("OP_Add reg1=%d, reg2=%d, dest=%d\n", inst->p1, inst->p2, inst->p3); break;
      case OP_Subtract:    printf("OP_Subtract reg1=%d, reg2=%d, dest=%d\n", inst->p1, inst->p2, inst->p3); break;
      case OP_Multiply:    printf("OP_Multiply reg1=%d, reg2=%d, dest=%d\n", inst->p1, inst->p2, inst->p3); break;
      case OP_Divide:      printf("OP_Divide reg1=%d, reg2=%d, dest=%d\n", inst->p1, inst->p2, inst->p3); break;
      case OP_Remainder:   printf("OP_Remainder reg1=%d, reg2=%d, dest=%d\n", inst->p1, inst->p2, inst->p3); break;
      case OP_Null:        printf("OP_Null dest_reg=%d\n", inst->p1); break;
      case OP_Halt:        printf("OP_Halt\n"); break;
    }
  }
}
