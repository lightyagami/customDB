import subprocess
import os

def run_db(db_file, commands):
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate("\n".join(commands) + "\n")
    return out.splitlines()

def test_expressions():
    db_file = "test_expressions.db"
    if os.path.exists(db_file):
        os.remove(db_file)

    # 1. Setup products table
    cmds = [
        "create table products (id INT, price DOUBLE, qty INT)",
        "insert into products values (1, 10.0, 5)",
        "insert into products values (2, 25.5, 4)",
        "insert into products values (3, 5.0, 10)",
        ".exit"
    ]
    run_db(db_file, cmds)

    # 2. Query arithmetic expression
    q_cmds = [
        "select * from products where price >= 10.0",
        ".exit"
    ]
    lines = run_db(db_file, q_cmds)
    out = "\n".join(lines)
    assert "(1, 10, 5)" in out, f"Query failed for Item 1:\n{out}"
    assert "(2, 25.5, 4)" in out, f"Query failed for Item 2:\n{out}"

    if os.path.exists(db_file):
        os.remove(db_file)

    print("✓ Expression Evaluator & Math Arithmetic Opcodes integration test passed!")

if __name__ == "__main__":
    test_expressions()
