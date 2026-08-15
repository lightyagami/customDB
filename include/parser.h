#pragma once
#include "common.h"
#include "catalog.h"

typedef enum {
  STATEMENT_INSERT,
  STATEMENT_SELECT,
  STATEMENT_UPDATE,
  STATEMENT_DELETE,
  STATEMENT_CREATE_TABLE,
  STATEMENT_DROP_TABLE,
  STATEMENT_CREATE_INDEX,
  STATEMENT_CREATE_VIEW,
  STATEMENT_DROP_VIEW,
  STATEMENT_CREATE_TRIGGER,
  STATEMENT_DROP_TRIGGER,
  STATEMENT_REINDEX,
  STATEMENT_ATTACH,
  STATEMENT_DETACH,
  STATEMENT_BEGIN,
  STATEMENT_COMMIT,
  STATEMENT_ROLLBACK,
  STATEMENT_SAVEPOINT,
  STATEMENT_ROLLBACK_TO,
  STATEMENT_RELEASE_SAVEPOINT,
  STATEMENT_VACUUM,
  STATEMENT_PRAGMA,
  STATEMENT_ANALYZE,
  STATEMENT_CREATE_VTABLE,
  STATEMENT_ALTER_TABLE,
  STATEMENT_HELP,
} StatementType;

typedef enum {
  ALTER_RENAME_TABLE,
  ALTER_ADD_COLUMN,
  ALTER_DROP_COLUMN,
} AlterType;

typedef enum {
  OP_EQ,          /* =  */
  OP_GT,          /* >  */
  OP_LT,          /* <  */
  OP_GTE,         /* >= */
  OP_LTE,         /* <= */
  OP_IN,          /* IN */
  OP_IS_NULL,     /* IS NULL */
  OP_IS_NOT_NULL, /* IS NOT NULL */
  OP_MATCH,       /* MATCH */
  OP_LIKE,        /* LIKE */
  OP_BETWEEN      /* BETWEEN */
} CompOp;

#define MAX_WHERE_CONDS 4

typedef struct WhereClause WhereClause;

typedef enum {
  LOGIC_AND,
  LOGIC_OR
} LogicOp;

typedef struct {
  char          col_name[COL_NAME_SIZE];
  bool          is_desc;
  CollationType collation;
} OrderByItem;

struct SingleCond {
  char   col_name[COL_NAME_SIZE];
  CompOp op;
  char   raw_val[MAX_RAW_VAL];
  char   raw_val2[MAX_RAW_VAL]; /* For BETWEEN upper bound */
  bool   is_subquery;
  char   sub_table[TBL_NAME_SIZE];
  char   sub_col[COL_NAME_SIZE];
  char   sub_where_col[COL_NAME_SIZE];
  CompOp sub_where_op;
  char   sub_where_val[MAX_RAW_VAL];
  bool   has_sub_where;
  
  /* Correlated EXISTS / NOT EXISTS */
  bool          is_exists;
  bool          is_not_exists;
  bool          sub_where_is_correlated;
  char          sub_correlated_outer_col[COL_NAME_SIZE];

  CollationType collation;
};

struct WhereClause {
  bool              has_where;
  uint32_t          num_conds;
  struct SingleCond conds[MAX_WHERE_CONDS];
  LogicOp           logic_ops[MAX_WHERE_CONDS - 1];
};

typedef struct SingleCond SingleCond;
typedef struct WhereClause WhereClause;

typedef struct {
  char col_name[COL_NAME_SIZE];
  char str_val[MAX_RAW_VAL];
} SetPair;

typedef enum {
  JOIN_INNER,
  JOIN_LEFT,
  JOIN_RIGHT,
  JOIN_FULL
} JoinType;

#define MAX_JOINS 8
typedef struct {
  JoinType type;
  char     right_table[TBL_NAME_SIZE];
  char     left_col[COL_NAME_SIZE];
  char     right_col[COL_NAME_SIZE];
} JoinItem;

typedef struct {
  JoinType type;
  char     left_col[COL_NAME_SIZE];
  char     right_col[COL_NAME_SIZE];
} JoinClause;

typedef enum { AGG_NONE, AGG_COUNT_STAR, AGG_COUNT, AGG_SUM, AGG_AVG, AGG_MIN, AGG_MAX } AggFunc;

typedef enum {
  WIN_NONE,
  WIN_ROW_NUMBER,
  WIN_RANK,
  WIN_DENSE_RANK,
  WIN_SUM,
  WIN_AVG,
  WIN_MIN,
  WIN_MAX
} WindowFunc;

typedef struct {
  WindowFunc win_func;
  char       partition_col[COL_NAME_SIZE];
  char       order_col[COL_NAME_SIZE];
  bool       order_desc;
} WindowSpec;

#define MAX_SELECT_COLS 8
typedef struct {
  AggFunc    func;
  char       col_name[256];
  bool       is_coalesce;
  char       coalesce_default[MAX_RAW_VAL];
  WindowSpec win_spec;
} SelectCol;

typedef struct Statement Statement;

typedef struct {
  char       cte_name[TBL_NAME_SIZE];
  Statement* cte_stmt;
  bool       is_recursive;
  int        rec_start;
  int        rec_end;
  int        rec_step;
} CteDef;

struct Statement {
  StatementType type;
  bool          is_explain;
  char          table_name[TBL_NAME_SIZE];

  /* CTEs */
  uint32_t num_ctes;
  CteDef   ctes[4];

  /* SELECT columns / Aggregates */
  bool       is_distinct;
  bool       is_aggregate;
  uint32_t   num_select_cols;
  SelectCol  select_cols[MAX_SELECT_COLS];

  /* INSERT */
  char     raw_values[MAX_COLUMNS][MAX_RAW_VAL];
  uint32_t num_values;
  bool     is_multi_insert;
  uint32_t num_multi_rows;
  char     multi_raw_values[16][MAX_COLUMNS][MAX_RAW_VAL];
  bool     is_insert_select;
  Statement* insert_select_stmt;

  /* SELECT / DELETE WHERE, UPDATE WHERE */
  WhereClause where_clause;

  /* UPDATE SET */
  SetPair  set_pairs[MAX_COLUMNS];
  uint32_t num_set_pairs;

  /* CREATE TABLE */
  TableDef new_table;
  bool     if_not_exists;   /* true when IF NOT EXISTS was present */

  /* CREATE INDEX */
  char     index_name[COL_NAME_SIZE];
  char     index_col_name[COL_NAME_SIZE];
  uint32_t index_num_cols;
  char     index_cols[MAX_COLUMNS][COL_NAME_SIZE];
  bool     index_is_partial;
  WhereClause index_where;
  bool     index_is_expr;
  char     index_expr_func[32];
  char     index_expr_col[COL_NAME_SIZE];

  /* JOIN */
  bool       has_join;
  char       join_table_name[TBL_NAME_SIZE];
  JoinClause join_clause;
  uint32_t   num_joins;
  JoinItem   joins[MAX_JOINS];

  /* ORDER BY */
  bool          has_order_by;
  char          order_by_col[COL_NAME_SIZE];
  bool          order_by_desc;
  CollationType order_by_collation;
  uint32_t      num_order_by;
  OrderByItem   order_by_items[4];

  /* LIMIT & OFFSET */
  bool       has_limit;
  int        limit_val;
  bool       has_offset;
  int        offset_val;

  /* GROUP BY */
  bool       has_group_by;
  char       group_by_col[COL_NAME_SIZE];

  /* HAVING */
  bool       has_having;
  AggFunc    having_func;
  char       having_col[COL_NAME_SIZE];
  CompOp     having_op;
  char       having_val[MAX_RAW_VAL];

  /* PRAGMA */
  char pragma_name[64];
  char pragma_value[64];

  /* SAVEPOINT */
  char savepoint_name[64];

  /* VIEW */
  char view_name[TBL_NAME_SIZE];
  char view_select_sql[256];

  /* TRIGGER */
  char          trigger_name[TBL_NAME_SIZE];
  TriggerTiming trigger_timing;
  TriggerEvent  trigger_event;
  char          trigger_action_sql[256];

  /* REINDEX */
  char reindex_target[IDX_NAME_SIZE];

  /* ATTACH / DETACH */
  char attach_filename[256];
  char attach_alias[64];
  char db_alias[64];

  /* VIRTUAL TABLE */
  char vtab_module[64];
  char vtab_args[256];

  /* VACUUM INTO */
  char vacuum_into_filename[256];

  /* ALTER TABLE */
  AlterType alter_type;
  char      new_table_name[TBL_NAME_SIZE];
  Column    new_col;
  char      drop_col_name[COL_NAME_SIZE];
};

PrepareResult prepare_statement(const char* input, Statement* out);
