import subprocess
import os

def run_db(db_file, commands):
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate("\n".join(commands) + "\n")
    return out.splitlines()

def test_outer_joins():
    db_file = "test_outer_joins.db"
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
        "insert into orders values (102, 2, 150.0)",
        "insert into orders values (103, 4, 999.0)", # user_id 4 does not exist in users
        ".exit"
    ]
    run_db(db_file, cmds)

    # 2. Test LEFT JOIN (Charlie has no orders, should output (3, Charlie, NULL, NULL, NULL))
    left_cmds = [
        "select * from users left join orders on users.id = orders.user_id",
        ".exit"
    ]
    lines_left = run_db(db_file, left_cmds)
    out_left = "\n".join(lines_left)
    assert "(3, Charlie, NULL, NULL, NULL)" in out_left, f"Expected Charlie with NULL orders in LEFT JOIN, got:\n{out_left}"

    # 3. Test RIGHT JOIN (Order 103 has user_id 4, should output (NULL, NULL, 103, 4, 999))
    right_cmds = [
        "select * from users right join orders on users.id = orders.user_id",
        ".exit"
    ]
    lines_right = run_db(db_file, right_cmds)
    out_right = "\n".join(lines_right)
    assert "(NULL, NULL, 103, 4, 999)" in out_right, f"Expected NULL user for Order 103 in RIGHT JOIN, got:\n{out_right}"

    # 4. Test FULL JOIN (Both unmatched Charlie and unmatched Order 103 should appear)
    full_cmds = [
        "select * from users full join orders on users.id = orders.user_id",
        ".exit"
    ]
    lines_full = run_db(db_file, full_cmds)
    out_full = "\n".join(lines_full)
    assert "(3, Charlie, NULL, NULL, NULL)" in out_full, f"Missing Charlie in FULL JOIN:\n{out_full}"
    assert "(NULL, NULL, 103, 4, 999)" in out_full, f"Missing Order 103 in FULL JOIN:\n{out_full}"

    # Clean up
    if os.path.exists(db_file):
        os.remove(db_file)

    print("✓ Outer Joins (LEFT OUTER JOIN, RIGHT OUTER JOIN, FULL OUTER JOIN) integration test passed!")

if __name__ == "__main__":
    test_outer_joins()
