import subprocess
import os

def run_db(db_file, commands):
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate("\n".join(commands) + "\n")
    return out.splitlines()

def test_autoincrement():
    db_file = "test_autoincrement.db"
    if os.path.exists(db_file):
        os.remove(db_file)

    # 1. Create table with AUTOINCREMENT primary key
    cmds = [
        "create table items (id INT AUTOINCREMENT, name VARCHAR(50), price DOUBLE)",
        "insert into items values (0, 'Laptop', 999.0)", # 0 triggers autoincrement -> 1
        "insert into items values (0, 'Phone', 499.0)",  # 0 triggers autoincrement -> 2
        "insert into items values (0, 'Tablet', 299.0)", # 0 triggers autoincrement -> 3
        ".exit"
    ]
    run_db(db_file, cmds)

    # 2. Query items to verify incremental IDs 1, 2, 3
    query_cmds = [
        "select * from items",
        ".exit"
    ]
    lines = run_db(db_file, query_cmds)
    out = "\n".join(lines)
    assert "(1, Laptop, 999)" in out, f"Expected ID 1 for Laptop, got:\n{out}"
    assert "(2, Phone, 499)" in out, f"Expected ID 2 for Phone, got:\n{out}"
    assert "(3, Tablet, 299)" in out, f"Expected ID 3 for Tablet, got:\n{out}"

    if os.path.exists(db_file):
        os.remove(db_file)

    print("✓ AUTOINCREMENT (Monotonic ID generation) integration test passed!")

if __name__ == "__main__":
    test_autoincrement()
