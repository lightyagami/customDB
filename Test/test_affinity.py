import subprocess
import os

def run_db(db_file, commands):
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate("\n".join(commands) + "\n")
    return out.splitlines()

def test_affinity():
    db_file = "test_affinity.db"
    if os.path.exists(db_file):
        os.remove(db_file)

    # 1. Create table with INT and VARCHAR columns
    cmds = [
        "create table account (id INT, num_col INT, text_col VARCHAR(50))",
        "insert into account values (1, '100', 500)",
        "insert into account values (2, 200, '600')",
        ".exit"
    ]
    run_db(db_file, cmds)

    # 2. Query data and verify type affinity coercion
    query_cmds = [
        "select * from account where num_col >= 100",
        ".exit"
    ]
    lines = run_db(db_file, query_cmds)
    out = "\n".join(lines)
    assert "(1, 100, 500)" in out, f"Type affinity failed for row 1:\n{out}"
    assert "(2, 200, 600)" in out, f"Type affinity failed for row 2:\n{out}"

    if os.path.exists(db_file):
        os.remove(db_file)

    print("✓ Flexible Manifest Typing & Type Affinity integration test passed!")

if __name__ == "__main__":
    test_affinity()
