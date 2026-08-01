import subprocess
import os

def run_db(db_file, commands):
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate("\n".join(commands) + "\n")
    return out.splitlines()

def test_explain():
    db_file = "test_explain.db"
    if os.path.exists(db_file):
        os.remove(db_file)

    init_cmds = [
        "create table users (id INT, age INT, name VARCHAR(50))",
        "create index idx_age on users (age)",
        "insert into users values (1, 20, 'Alice')",
        "insert into users values (2, 30, 'Bob')",
        "explain select * from users",
        "explain select * from users where id = 1",
        "explain select * from users where age > 25",
        ".exit"
    ]

    lines = run_db(db_file, init_cmds)
    full_output = "\n".join(lines)

    assert "QUERY PLAN:\n  SCAN TABLE users" in full_output, f"Expected SCAN TABLE users, got:\n{full_output}"
    assert "QUERY PLAN:\n  SEARCH TABLE users USING PRIMARY KEY (id = 1)" in full_output, f"Expected PK seek query plan, got:\n{full_output}"
    assert "QUERY PLAN:\n  SEARCH TABLE users USING INDEX _idx_users_age (age > 25)" in full_output, f"Expected Index search query plan, got:\n{full_output}"
    assert "VDBE Program (" in full_output, f"Expected VDBE Program bytecode output, got:\n{full_output}"

    if os.path.exists(db_file):
        os.remove(db_file)
    print("✓ EXPLAIN query plan integration test passed!")

if __name__ == "__main__":
    test_explain()
