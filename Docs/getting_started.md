# Getting Started & Verification

This document describes how to compile the database engine, run the interactive SQL REPL shell, and execute the automated test suites.

---

## Compilation

Build the database engine from the source directory using `make`:

```bash
make clean && make
```

This compiles all modules with standard warnings (`-Wall -Wextra`) and optimization level `-O2`, creating the executable binary `./db`.

---

## Interactive Shell (REPL)

Start the REPL by specifying a database file:

```bash
./db my_database.db
```

### Supported SQL Commands & Syntax:
* **Create Table**:
  ```sql
  create table users (id INT, rating DOUBLE, name VARCHAR(50))
  ```
* **Insert Rows**:
  ```sql
  insert into users values (1, 85.5, Alice)
  insert into users values (2, 99.0, Bob)
  insert into users values (3, 10.5, Charlie)
  ```
* **Select & Range Queries**:
  ```sql
  select * from users where id >= 2
  select * from users where rating < 90.0
  ```
* **Sorting & Limits**:
  ```sql
  select * from users order by rating desc
  select * from users order by rating asc limit 2
  ```
* **Secondary Indexes**:
  ```sql
  create index idx_rating on users (rating)
  .schema users
  ```
* **Relational JOINs**:
  ```sql
  create table profiles (user_id INT, city VARCHAR(50))
  insert into profiles values (1, NewYork)
  insert into profiles values (2, SanFrancisco)
  
  select users.name, profiles.city from users join profiles on users.id = profiles.user_id
  ```
* **Transactions**:
  ```sql
  begin
  insert into users values (4, 75.0, David)
  commit
  ```
* **Inspect Schema & Exit**:
  ```sql
  .schema users
  .exit
  ```

---

## Running the Automated Test Suite

All integration tests are located in the `Test/` directory. You can run them using python:

1. **Range Queries, Indexes, Sorting, and Limits**:
   ```bash
   python3 Test/test_db.py
   ```
2. **ACID Crash Recovery**:
   ```bash
   python3 Test/test_tx.py
   ```
3. **Index Nested Loop Joins**:
   ```bash
   python3 Test/test_join.py
   ```
4. **Concurrency Locks**:
   ```bash
   python3 Test/test_lock.py
   ```

To run all tests concurrently and verify the complete database state:
```bash
python3 Test/test_db.py && python3 Test/test_tx.py && python3 Test/test_join.py && python3 Test/test_lock.py
```
If successful, all scripts will exit with status `0` and print verification confirmations.
