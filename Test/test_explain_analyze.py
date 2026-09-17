import subprocess
import os

def run_db(db_file, commands):
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate("\n".join(commands) + "\n")
    return out.splitlines()

def test_explain_analyze():
    db_file = "test_explain_analyze.db"
    if os.path.exists(db_file):
        os.remove(db_file)

    init_cmds = [
        "create table users (id INT, age INT, name VARCHAR(50))",
        "create index idx_age on users (age)",
        "insert into users values (1, 20, 'Alice')",
        "insert into users values (2, 30, 'Bob')",
        "insert into users values (3, 40, 'Charlie')",
        "explain analyze select * from users",
        "explain analyze select * from users where id = 1",
        "explain analyze select * from users where age >= 30 limit 1",
        ".exit"
    ]

    lines = run_db(db_file, init_cmds)
    full_output = "\n".join(lines)

    assert "QUERY PLAN & EXECUTION METRICS:" in full_output, f"Missing metrics banner:\n{full_output}"
    assert "Plan: SCAN TABLE users" in full_output, f"Missing SCAN plan:\n{full_output}"
    assert "Plan: SEARCH TABLE users USING PRIMARY KEY (id = 1)" in full_output, f"Missing PK plan:\n{full_output}"
    assert "Plan: SEARCH TABLE users USING INDEX _idx_users_age (age >= 30)" in full_output, f"Missing Index plan:\n{full_output}"
    assert "B-Tree Traversal Depth: 1 levels" in full_output, f"Missing B-Tree depth:\n{full_output}"
    assert "Buffer Cache Hits:" in full_output, f"Missing cache hits:\n{full_output}"
    assert "Disk I/O Reads:" in full_output, f"Missing disk reads:\n{full_output}"
    assert "Rows Scanned: 3" in full_output, f"Missing Rows Scanned:\n{full_output}"
    assert "Rows Yielded: 1" in full_output, f"Missing Rows Yielded:\n{full_output}"
    assert "Execution Time:" in full_output, f"Missing Execution Time:\n{full_output}"

    if os.path.exists(db_file):
        os.remove(db_file)
    print("✓ EXPLAIN ANALYZE integration test passed!")

if __name__ == "__main__":
    test_explain_analyze()
