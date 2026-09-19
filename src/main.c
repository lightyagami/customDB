#include "common.h"
#include "pager.h"
#include "catalog.h"
#include "parser.h"
#include "executor.h"
#include "btree.h"

static void print_prompt() { printf("db > "); fflush(stdout); }

static void read_input(char* buffer, uint32_t max_len, Pager* pager, Catalog* catalog) {
  (void)catalog;
  if (fgets(buffer, max_len, stdin) == NULL) {
    printf("\n");
    if (pager) pager_close(pager);
    exit(EXIT_SUCCESS);
  }

  /* Strip newline */
  size_t len = strlen(buffer);
  if (len > 0 && buffer[len - 1] == '\n') {
    buffer[len - 1] = '\0';
  }
}

MetaCommandResult do_meta_command(const char* input, Catalog* catalog, Pager* pager) {
  (void)catalog;
  if (strcmp(input, ".exit") == 0) {
    pager_close(pager);
    exit(EXIT_SUCCESS);
  }

  if (strcmp(input, ".tables") == 0) {
    if (catalog->num_tables == 0) {
      printf("No tables defined.\n");
    } else {
      for (uint32_t i = 0; i < catalog->num_tables; i++) {
        printf("%s\n", catalog->tables[i].name);
      }
    }
    return META_COMMAND_SUCCESS;
  }

  if (strncmp(input, ".schema ", 8) == 0) {
    const char* tbl_name = input + 8;
    TableDef* def = catalog_find(catalog, tbl_name);
    if (def == NULL) {
      printf("Table '%s' not found.\n", tbl_name);
    } else {
      print_schema(def);
    }
    return META_COMMAND_SUCCESS;
  }

  if (strncmp(input, ".btree ", 7) == 0) {
    const char* tbl_name = input + 7;
    TableDef* def = catalog_find(catalog, tbl_name);
    if (def == NULL) {
      printf("Table '%s' not found.\n", tbl_name);
    } else {
      printf("Tree for table '%s':\n", tbl_name);
      print_tree(pager, def->root_page_num, 0, def);
    }
    return META_COMMAND_SUCCESS;
  }

  if (strcmp(input, ".constants") == 0) {
    printf("Constants:\n");
    printf("  PAGE_SIZE: %d\n", PAGE_SIZE);
    printf("  TABLE_MAX_PAGES: (dynamic/unlimited)\n");
    printf("  MAX_TABLES: %d\n", MAX_TABLES);
    printf("  MAX_COLUMNS: %d\n", MAX_COLUMNS);
    return META_COMMAND_SUCCESS;
  }

  return META_COMMAND_UNRECOGNIZED_COMMAND;
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    printf("Must supply a database filename.\n");
    exit(EXIT_FAILURE);
  }

  const char* filename = argv[1];
  Pager* pager = pager_open(filename);

  Catalog* catalog = malloc(sizeof(Catalog));
  if (!catalog) {
    fprintf(stderr, "Out of memory allocating catalog\n");
    exit(EXIT_FAILURE);
  }
  memset(catalog, 0, sizeof(Catalog));
  catalog_load(catalog, pager);

  char input_buffer[65536];

  while (true) {
    print_prompt();
    read_input(input_buffer, sizeof(input_buffer), pager, catalog);

    if (input_buffer[0] == '.') {
      switch (do_meta_command(input_buffer, catalog, pager)) {
        case META_COMMAND_SUCCESS:
          continue;
        case META_COMMAND_UNRECOGNIZED_COMMAND:
          printf("Unrecognized command '%s'\n", input_buffer);
          continue;
      }
    }

    Statement* statement = calloc(1, sizeof(Statement));
    if (!statement) {
      printf("Error: Out of memory.\n");
      continue;
    }
    PrepareResult prep_res = prepare_statement(input_buffer, statement);
    if (prep_res != PREPARE_SUCCESS) {
      switch (prep_res) {
        case PREPARE_NEGATIVE_ID:
          printf("ID must be positive.\n");
          break;
        case PREPARE_VALUE_TOO_LONG:
          printf("String is too long.\n");
          break;
        case PREPARE_SYNTAX_ERROR:
          printf("Syntax error.\n");
          break;
        case PREPARE_UNRECOGNIZED_STATEMENT:
          printf("Unrecognized keyword at start of '%s'.\n", input_buffer);
          break;
        case PREPARE_BAD_SCHEMA:
          printf("Error: Invalid table/column schema or constraints.\n");
          break;
        default:
          break;
      }
      statement_free_children(statement);
      free(statement);
      continue;
    }

    switch (execute_statement(statement, catalog, pager)) {
      case EXECUTE_SUCCESS:
        printf("Executed.\n");
        break;
      case EXECUTE_DUPLICATE_KEY:
        printf("Error: Duplicate key.\n");
        break;
      case EXECUTE_ROW_NOT_FOUND:
        printf("Error: Row not found.\n");
        break;
      case EXECUTE_TABLE_NOT_FOUND:
        printf("Error: Table not found.\n");
        break;
      case EXECUTE_TABLE_EXISTS:
        printf("Error: Table already exists.\n");
        break;
      case EXECUTE_CATALOG_FULL:
        printf("Error: Catalog is full (max 6 tables).\n");
        break;
      case EXECUTE_BAD_SCHEMA:
        printf("Error: Column count/type mismatch or primary key violation.\n");
        break;
      case EXECUTE_TYPE_MISMATCH:
        printf("Error: Type mismatch.\n");
        break;
      case EXECUTE_CONSTRAINT_NOT_NULL:
        printf("Error: NOT NULL constraint failed.\n");
        break;
      case EXECUTE_CONSTRAINT_UNIQUE:
        printf("Error: UNIQUE constraint failed.\n");
        break;
      case EXECUTE_CONSTRAINT_CHECK:
        printf("Error: CHECK constraint failed.\n");
        break;
      case EXECUTE_CONSTRAINT_FOREIGN_KEY:
        printf("Error: FOREIGN KEY constraint failed.\n");
        break;
      case EXECUTE_BUSY:
        printf("Error: Database is locked.\n");
        break;
      case EXECUTE_ERROR:
        break;
    }

    statement_free_children(statement);
    free(statement);
  }

  return 0;
}
