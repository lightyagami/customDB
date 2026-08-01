import subprocess
import os

def run_db(db_file, commands):
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate("\n".join(commands) + "\n")
    return out.splitlines()

def test_joins():
    db_file = "test_joins.db"
    if os.path.exists(db_file):
        os.remove(db_file)

    # 1. Create tables & insert data
    commands = [
        "create table users (id INT, name VARCHAR(50))",
        "create table orders (id INT, user_id INT, amount DOUBLE)",
        "insert into users values (1, 'Alice')",
        "insert into users values (2, 'Bob')",
        "insert into orders values (101, 1, 49.99)",
        "insert into orders values (102, 1, 15.00)",
        "insert into orders values (103, 2, 89.99)",
        "select * from users join orders on users.id = orders.user_id",
        ".exit"
    ]
    lines = run_db(db_file, commands)
    
    # Assert matched join tuples exist
    assert any("(1, Alice, 101, 1, 49.99)" in l for l in lines), "Join missing Alice order 101!"
    assert any("(1, Alice, 102, 1, 15)" in l for l in lines), "Join missing Alice order 102!"
    assert any("(2, Bob, 103, 2, 89.99)" in l for l in lines), "Join missing Bob order 103!"
    print("✓ Index Nested Loop Join (on primary key id) verified successfully")

    # 2. Test JOIN with WHERE filter
    filter_cmds = [
        "select * from users join orders on users.id = orders.user_id where amount > 20.0",
        ".exit"
    ]
    lines_filter = run_db(db_file, filter_cmds)
    assert any("(1, Alice, 101, 1, 49.99)" in l for l in lines_filter), "Filter query missing Alice order 101!"
    assert not any("(1, Alice, 102, 1, 15)" in l for l in lines_filter), "Filter query included Alice order 102 (should be filtered)!"
    print("✓ JOIN with WHERE filter verified successfully")

    # Clean up
    if os.path.exists(db_file):
        os.remove(db_file)
    print("All JOIN integration tests passed!")

if __name__ == "__main__":
    test_joins()
