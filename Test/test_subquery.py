import subprocess
import os

def run_db(db_file, commands):
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate("\n".join(commands) + "\n")
    return out.splitlines()

def test_subquery():
    db_file = "test_subquery.db"
    if os.path.exists(db_file):
        os.remove(db_file)

    init_cmds = [
        "create table users (id INT, name VARCHAR(50), age INT)",
        "create table orders (id INT, user_id INT, amount DOUBLE)",
        "insert into users values (1, 'Alice', 30)",
        "insert into users values (2, 'Bob', 20)",
        "insert into users values (3, 'Charlie', 35)",
        "insert into orders values (101, 1, 500.0)",
        "insert into orders values (102, 2, 150.0)",
        "insert into orders values (103, 3, 750.0)",
        "select * from orders where user_id in (select id from users where age >= 30)",
        ".exit"
    ]

    lines = run_db(db_file, init_cmds)
    results = []
    for l in lines:
        s = l.strip()
        if "db >" in s:
            s = s.replace("db >", "").strip()
        if s.startswith("("):
            results.append(s)

    assert len(results) == 2, f"Expected 2 orders for users with age >= 30, got:\n{results}"
    assert any("(101, 1, 500" in r for r in results), f"Expected order 101, got:\n{results}"
    assert any("(103, 3, 750" in r for r in results), f"Expected order 103, got:\n{results}"

    if os.path.exists(db_file):
        os.remove(db_file)
    print("✓ Subqueries (WHERE col IN (SELECT ...)) integration test passed!")

if __name__ == "__main__":
    test_subquery()
