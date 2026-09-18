import subprocess
import os
import time
import pytest

def run_db(db_file, commands):
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate("\n".join(commands) + "\n")
    return out.splitlines()

def test_mvcc_snapshot_isolation():
    """Verify that an active reader sees a consistent snapshot isolated from concurrent committed writes."""
    db_file = "test_mvcc_iso.db"
    for f in [db_file, f"{db_file}-wal", f"{db_file}-journal"]:
        if os.path.exists(f):
            os.remove(f)

    # 1. Initialize table with 1 row
    p_init = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    p_init.communicate("PRAGMA journal_mode = WAL;\nCREATE TABLE t (id INT, val TEXT);\nINSERT INTO t VALUES (1, 'initial');\n.exit\n")

    # 2. Start Reader A process with an open transaction (holding snapshot)
    p_reader = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    p_reader.stdin.write("begin\n")
    p_reader.stdin.write("select * from t\n")
    p_reader.stdin.flush()
    time.sleep(0.2)

    # 3. Writer B inserts and commits rows (2 and 3)
    p_writer = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    p_writer.communicate("INSERT INTO t VALUES (2, 'second');\nINSERT INTO t VALUES (3, 'third');\n.exit\n")

    # 4. Reader A queries again within its transaction — must NOT see rows 2 or 3
    p_reader.stdin.write("select * from t\n")
    p_reader.stdin.write("commit\n")
    p_reader.stdin.write("select * from t\n")
    p_reader.stdin.write(".exit\n")
    out_r, _ = p_reader.communicate()

    lines = [l.strip() for l in out_r.splitlines() if "(" in l and not l.startswith("CREATE")]
    # First select inside transaction: (1, initial)
    assert "(1, initial)" in lines[0]
    # Second select inside transaction (after concurrent writes): still only (1, initial)
    assert "(1, initial)" in lines[1]
    assert "(2, second)" not in lines[1]
    assert "(3, third)" not in lines[1]

    # After commit, new statement sees all rows
    assert any("(2, second)" in l for l in lines)
    assert any("(3, third)" in l for l in lines)

    if os.path.exists(db_file):
        os.remove(db_file)

def test_mvcc_nonblocking_reads():
    """Verify that readers are never blocked by an in-flight uncommitted write transaction."""
    db_file = "test_mvcc_noblock.db"
    for f in [db_file, f"{db_file}-wal", f"{db_file}-journal"]:
        if os.path.exists(f):
            os.remove(f)

    # 1. Initialize table
    p_init = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    p_init.communicate("CREATE TABLE items (id INT, name TEXT);\nINSERT INTO items VALUES (1, 'Item1');\n.exit\n")

    # 2. Writer process begins transaction and mutates table
    p_writer = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    p_writer.stdin.write("begin\n")
    p_writer.stdin.write("INSERT INTO items VALUES (2, 'UncommittedItem');\n")
    p_writer.stdin.flush()
    time.sleep(0.3)

    # 3. Concurrent reader executes SELECT while writer holds transaction
    start_t = time.time()
    p_reader = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out_r, err_r = p_reader.communicate("SELECT * FROM items;\n.exit\n")
    duration = time.time() - start_t

    # Must complete quickly without timeout/lock error
    assert duration < 2.0, f"Reader was blocked! Duration: {duration}s"
    assert "Item1" in out_r
    assert "UncommittedItem" not in out_r
    assert "Database is locked" not in out_r and "Database is locked" not in err_r

    # 4. Commit writer
    p_writer.stdin.write("commit\n.exit\n")
    p_writer.communicate()

    if os.path.exists(db_file):
        os.remove(db_file)

def test_mvcc_soft_delete_and_update_visibility():
    """Verify MVCC soft delete (xmax) and update versions across snapshots."""
    db_file = "test_mvcc_upd_del.db"
    for f in [db_file, f"{db_file}-wal", f"{db_file}-journal"]:
        if os.path.exists(f):
            os.remove(f)

    # 1. Setup table with rows 1, 2, 3
    init_cmds = [
        "CREATE TABLE products (id INT PRIMARY KEY, name TEXT, qty INT);",
        "INSERT INTO products VALUES (1, 'Apples', 10);",
        "INSERT INTO products VALUES (2, 'Bananas', 20);",
        "INSERT INTO products VALUES (3, 'Cherries', 30);",
        ".exit"
    ]
    run_db(db_file, init_cmds)

    # 2. Update Bananas quantity and delete Cherries
    mutate_cmds = [
        "UPDATE products SET qty = 99 WHERE id = 2;",
        "DELETE FROM products WHERE id = 3;",
        ".exit"
    ]
    run_db(db_file, mutate_cmds)

    # 3. Verify current snapshot sees updated and deleted state
    query_cmds = [
        "SELECT id, name, qty FROM products ORDER BY id ASC;",
        ".exit"
    ]
    lines = run_db(db_file, query_cmds)
    out = "\n".join(lines)
    assert "(1, Apples, 10)" in out
    assert "(2, Bananas, 99)" in out
    assert "Cherries" not in out

    # 4. Run vacuum_mvcc to reclaim dead versions
    vac_cmds = [
        "PRAGMA vacuum_mvcc;",
        "SELECT id, name, qty FROM products ORDER BY id ASC;",
        ".exit"
    ]
    vac_lines = run_db(db_file, vac_cmds)
    vac_out = "\n".join(vac_lines)
    assert "Vacuumed" in vac_out
    assert "(2, Bananas, 99)" in vac_out
    assert "Cherries" not in vac_out

    if os.path.exists(db_file):
        os.remove(db_file)

def test_mvcc_hot_update_and_index_seek():
    """Verify Heap-Only Tuple (HOT) optimization and secondary index seek across updates/deletes."""
    db_file = "test_mvcc_hot.db"
    for f in [db_file, f"{db_file}-wal", f"{db_file}-journal"]:
        if os.path.exists(f):
            os.remove(f)

    # 1. Setup table with secondary index
    setup_cmds = [
        "CREATE TABLE accounts (id INT PRIMARY KEY, email TEXT, balance INT);",
        "CREATE INDEX idx_email ON accounts (email);",
        "INSERT INTO accounts VALUES (1, 'alice@example.com', 100);",
        "INSERT INTO accounts VALUES (2, 'bob@example.com', 200);",
        ".exit"
    ]
    run_db(db_file, setup_cmds)

    # 2. HOT update: change non-indexed column (balance)
    hot_cmds = [
        "UPDATE accounts SET balance = 150 WHERE id = 1;",
        "SELECT id, email, balance FROM accounts WHERE email = 'alice@example.com';",
        ".exit"
    ]
    out_hot = "\n".join(run_db(db_file, hot_cmds))
    assert "(1, alice@example.com, 150)" in out_hot

    # 3. Indexed column update: change email
    idx_upd_cmds = [
        "UPDATE accounts SET email = 'alice_new@example.com' WHERE id = 1;",
        "SELECT id, email, balance FROM accounts WHERE email = 'alice_new@example.com';",
        "SELECT id, email, balance FROM accounts WHERE email = 'alice@example.com';",
        ".exit"
    ]
    out_idx = "\n".join(run_db(db_file, idx_upd_cmds))
    assert "(1, alice_new@example.com, 150)" in out_idx
    # Old email must not be returned
    lines_old = [l for l in out_idx.splitlines() if "(1, alice@example.com" in l]
    assert len(lines_old) == 0

    # 4. Delete row 2 and verify index seek returns nothing
    del_cmds = [
        "DELETE FROM accounts WHERE id = 2;",
        "SELECT id, email, balance FROM accounts WHERE email = 'bob@example.com';",
        "PRAGMA vacuum_mvcc;",
        "SELECT count(*) FROM accounts;",
        ".exit"
    ]
    out_del = "\n".join(run_db(db_file, del_cmds))
    assert "bob@example.com" not in out_del
    assert "Vacuumed" in out_del

    if os.path.exists(db_file):
        os.remove(db_file)

def test_mvcc_concurrent_readers_and_writes_with_vacuum():
    """Verify that an active reader transaction pins the snapshot across concurrent updates, deletes, and queries."""
    db_file = "test_mvcc_vac_iso.db"
    for f in [db_file, f"{db_file}-wal", f"{db_file}-journal"]:
        if os.path.exists(f):
            os.remove(f)

    # 1. Initialize table with 2 rows
    p_init = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    p_init.communicate("PRAGMA journal_mode = WAL;\nCREATE TABLE stock (id INT PRIMARY KEY, symbol TEXT, price INT);\nINSERT INTO stock VALUES (1, 'GOOG', 100);\nINSERT INTO stock VALUES (2, 'AAPL', 150);\n.exit\n")

    # 2. Reader A begins transaction and executes initial query
    p_reader = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    p_reader.stdin.write("begin\n")
    p_reader.stdin.write("SELECT id, symbol, price FROM stock WHERE id = 1;\n")
    p_reader.stdin.flush()
    time.sleep(0.2)

    # 3. Writer B updates GOOG to 200, deletes AAPL, and inserts MSFT
    p_writer = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    p_writer.communicate("UPDATE stock SET price = 200 WHERE id = 1;\nDELETE FROM stock WHERE id = 2;\nINSERT INTO stock VALUES (3, 'MSFT', 300);\n.exit\n")

    # 4. Reader A queries again within its transaction:
    # Must still see GOOG at 100, AAPL at 150, and must NOT see MSFT
    p_reader.stdin.write("SELECT id, symbol, price FROM stock WHERE id = 1;\n")
    p_reader.stdin.write("SELECT id, symbol, price FROM stock WHERE id = 2;\n")
    p_reader.stdin.write("SELECT count(*) FROM stock;\n")
    p_reader.stdin.write("commit\n")
    # After commit, new queries see updated state
    p_reader.stdin.write("SELECT id, symbol, price FROM stock WHERE id = 1;\n")
    p_reader.stdin.write("SELECT count(*) FROM stock;\n")
    p_reader.stdin.write(".exit\n")
    out_r, _ = p_reader.communicate()

    lines = [l.strip() for l in out_r.splitlines() if "(" in l and not l.startswith("CREATE")]
    # First select inside tx: (1, GOOG, 100)
    assert "(1, GOOG, 100)" in lines[0]
    # Second select inside tx: still (1, GOOG, 100)
    assert "(1, GOOG, 100)" in lines[1]
    # Third select inside tx: (2, AAPL, 150)
    assert "(2, AAPL, 150)" in lines[2]
    # Fourth select inside tx: count is 2
    assert "(2)" in lines[3]

    # After commit:
    # Fifth select: (1, GOOG, 200)
    assert "(1, GOOG, 200)" in lines[4]
    # Sixth select: count is 2 (GOOG and MSFT)
    assert "(2)" in lines[5]

    if os.path.exists(db_file):
        os.remove(db_file)

