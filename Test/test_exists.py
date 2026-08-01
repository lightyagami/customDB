import subprocess
import os

def run_db(db_file, commands):
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate("\n".join(commands) + "\n")
    return out.splitlines()

def test_exists():
    db_file = "test_exists.db"
    if os.path.exists(db_file):
        os.remove(db_file)

    # 1. Initialize tables & rows
    cmds = [
        "create table users (id INT, name VARCHAR(50))",
        "create table orders (id INT, user_id INT, amount DOUBLE)",
        "insert into users values (1, 'Alice')",
        "insert into users values (2, 'Bob')",
        "insert into users values (3, 'Charlie')",
        "insert into orders values (101, 1, 500.0)",
        "insert into orders values (102, 1, 150.0)",
        "insert into orders values (103, 2, 300.0)",
        ".exit"
    ]
    run_db(db_file, cmds)

    # 2. Correlated EXISTS query (Users with at least one order)
    exists_cmds = [
        "select * from users where exists (select 1 from orders where orders.user_id = users.id)",
        ".exit"
    ]
    lines = run_db(db_file, exists_cmds)
    out_exists = "\n".join(lines)

    assert "(1, Alice)" in out_exists, f"Missing Alice in EXISTS output:\n{out_exists}"
    assert "(2, Bob)" in out_exists, f"Missing Bob in EXISTS output:\n{out_exists}"
    assert "Charlie" not in out_exists, f"Charlie should NOT be in EXISTS output:\n{out_exists}"

    # 3. Correlated NOT EXISTS query (Users with NO orders)
    not_exists_cmds = [
        "select * from users where not exists (select 1 from orders where orders.user_id = users.id)",
        ".exit"
    ]
    lines_not = run_db(db_file, not_exists_cmds)
    out_not = "\n".join(lines_not)

    assert "(3, Charlie)" in out_not, f"Missing Charlie in NOT EXISTS output:\n{out_not}"
    assert "Alice" not in out_not, f"Alice should NOT be in NOT EXISTS output:\n{out_not}"
    assert "Bob" not in out_not, f"Bob should NOT be in NOT EXISTS output:\n{out_not}"

    if os.path.exists(db_file):
        os.remove(db_file)
    print("✓ Correlated Subqueries & EXISTS / NOT EXISTS integration test passed!")

if __name__ == "__main__":
    test_exists()
