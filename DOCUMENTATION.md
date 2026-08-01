# Custom C DBMS — Architecture & Reference Manual

A high-performance, transactional, B+Tree page-based Relational Database Management System (RDBMS) written in C.

---

## Architecture Overview

The DBMS engine is organized into decoupled layers:

```
                  +-----------------------------------+
                  |        CLI / Application          |
                  +-----------------------------------+
                                    |
                                    v
                  +-----------------------------------+
                  |           SQL Parser              |
                  |     (Tokens, AST, Statements)     |
                  +-----------------------------------+
                                    |
                                    v
                  +-----------------------------------+
                  |       VDBE Bytecode Engine        |
                  |    (Virtual Machine Execution)    |
                  +-----------------------------------+
                                    |
                                    v
                  +-----------------------------------+
                  |      B+Tree Storage Subsystem     |
                  |    (Leaf Nodes, Internal Nodes)   |
                  +-----------------------------------+
                                    |
                                    v
                  +-----------------------------------+
                  |        Pager & Cache Pool         |
                  |   (4KB Pages, WAL, Journaling)    |
                  +-----------------------------------+
```

1. **Pager Subsystem (`src/pager.c`)**: Manages 4KB database page frames, dirty page caching, Write-Ahead Logging (`-wal`), Rollback Journaling (`-journal`), and ACID crash recovery.
2. **Catalog & Schema Subsystem (`src/catalog.c`)**: Manages dynamic multi-page catalog metadata, table definitions, column types, offset calculations, views, triggers, and disk serialization.
3. **B+Tree Storage Subsystem (`src/btree.c`)**: Implements slotted-page B+Tree storage for tables and secondary indexes with fast integer register lookups.
4. **Parser Subsystem (`src/parser.c`)**: Tokenizes SQL inputs and builds AST statement representations for DDL, DML, DQL, TCL, and Virtual Table queries.
5. **Virtual Database Engine / VDBE (`src/vdbe.c`, `src/executor.c`)**: Executes register-based VDBE bytecode instructions, index seeks, aggregations, CTEs, joins, and window functions.

---

## Key Features & Capabilities

### 1. Data Types & Storage
- **Numeric**: `INT`, `BIGINT`, `SMALLINT`, `TINYINT`, `FLOAT`, `DOUBLE`, `REAL`, `NUMERIC`, `DECIMAL`.
- **Text & Binary**: `TEXT`, `VARCHAR(N)`, `CHAR(N)`, `CLOB`, `BLOB`, `VARBINARY`.
- **Temporal & Logical**: `DATETIME`, `DATE`, `TIME`, `TIMESTAMP`, `BOOLEAN`.
- **Literals**: Hexadecimal binary literals (`X'414243'`, `x'68656c6c6f'`).
- **Flexible Typing**: Manifest typing and type affinity rules.

### 2. Schema Evolution & DDL
- **Table Operations**: `CREATE TABLE`, `DROP TABLE`.
- **`ALTER TABLE`**: `RENAME TO`, `ADD COLUMN`, `DROP COLUMN`.
- **Constraints**: `NOT NULL`, `UNIQUE`, `DEFAULT <val>`, `CHECK (<expr>)`, `FOREIGN KEY`.
- **Composite Primary Keys**: `PRIMARY KEY (col1, col2)`.
- **Referential Integrity**: `ON DELETE CASCADE` and `ON UPDATE CASCADE`.
- **Key Generation**: `AUTOINCREMENT` monotonic key generation.

### 3. Advanced Indexing
- **Single & Composite Indexes**: `CREATE INDEX idx ON table (col1, col2)`.
- **Partial Indexes**: `CREATE INDEX idx ON table (col) WHERE status = 'active'`.
- **Expression Indexes**: `CREATE INDEX idx ON table (lower(email))`.
- **Reindexing**: `REINDEX`.

### 4. Advanced Queries & Analytics
- **Joins**: `INNER JOIN`, `LEFT OUTER JOIN`, `RIGHT OUTER JOIN`, `FULL OUTER JOIN`.
- **CTEs**: `WITH` and `WITH RECURSIVE` Common Table Expressions.
- **Window Functions**: `ROW_NUMBER()`, `RANK()`, `DENSE_RANK()`, `SUM() OVER (PARTITION BY ... ORDER BY ...)`.
- **Aggregations**: `COUNT()`, `SUM()`, `AVG()`, `MIN()`, `MAX()`, `GROUP BY`, `HAVING`.
- **Subqueries**: Correlated `EXISTS`, `NOT EXISTS`, `IN (SELECT ...)`.
- **Extensions**: FTS5 Full-Text Search (`MATCH`), Native JSON (`json_extract`), Date/Time (`strftime`), Triggers (`CREATE TRIGGER`), Views (`CREATE VIEW`), Collations (`COLLATE NOCASE`), Virtual Tables (`csv`).

### 5. Transactions & ACID Safety
- **Transaction Controls**: `BEGIN`, `COMMIT`, `ROLLBACK`.
- **Savepoints**: `SAVEPOINT <name>`, `ROLLBACK TO <name>`, `RELEASE <name>`.
- **Logging Modes**: Rollback Journaling (`-journal`) and Write-Ahead Logging (`-wal`).
- **Concurrency Locking**: SHARED, RESERVED, PENDING, EXCLUSIVE byte locks.
- **Vacuum Defragmentation**: `VACUUM` and `VACUUM INTO '<file>'`.

---

## Compilation & Usage

### Building the Database
Ensure `gcc` and `make` are installed:
```bash
make clean && make
```

### Running the Interactive REPL
To start an interactive database session:
```bash
./db my_database.db
```

To run in **In-Memory Mode**:
```bash
./db :memory:
```

---

## SQL Examples & Reference

### Schema Evolution & DDL
```sql
-- Create table with constraints and foreign key cascades
CREATE TABLE departments (
    dept_id INT PRIMARY KEY,
    dept_name TEXT NOT NULL
);

CREATE TABLE employees (
    id INT,
    name TEXT NOT NULL,
    email TEXT UNIQUE,
    salary REAL DEFAULT 50000.0,
    dept_id INT REFERENCES departments(dept_id) ON DELETE CASCADE ON UPDATE CASCADE,
    PRIMARY KEY (id, dept_id)
);

-- Schema Evolution
ALTER TABLE employees RENAME TO staff;
ALTER TABLE staff ADD COLUMN age INT;
ALTER TABLE staff DROP COLUMN age;
```

### Advanced Indexing
```sql
-- Partial Index
CREATE INDEX idx_active_staff ON staff (salary) WHERE dept_id = 1;

-- Expression Index
CREATE INDEX idx_lower_email ON staff (lower(email));
```

### Advanced Querying & Analytics
```sql
-- Recursive CTE (Hierarchical Generation)
WITH RECURSIVE cnt(x) AS (
    SELECT 1
    UNION ALL
    SELECT x + 1 FROM cnt WHERE x < 10
)
SELECT x FROM cnt;

-- Window Functions
SELECT id, name, salary,
       ROW_NUMBER() OVER (PARTITION BY dept_id ORDER BY salary DESC) as rank
FROM staff;

-- Full-Text Search
CREATE VIRTUAL TABLE docs USING fts5(title, body);
INSERT INTO docs VALUES ('Doc 1', 'Database engines are fast');
SELECT * FROM docs WHERE docs MATCH 'database';
```

### Transactions & Savepoints
```sql
BEGIN TRANSACTION;
INSERT INTO staff VALUES (1, 'Alice', 'alice@example.com', 75000, 1);
SAVEPOINT sp1;
UPDATE staff SET salary = 80000 WHERE id = 1;
ROLLBACK TO sp1;
COMMIT;
```

---

## Testing & Benchmarking

### Running the Integration Test Suite
To run all 41 automated Python integration tests:
```bash
for f in Test/test_*.py; do python3 "$f" || exit 1; done
```

### Running the Performance Benchmark
To run the performance benchmark against SQLite3:
```bash
python3 benchmark.py
```
