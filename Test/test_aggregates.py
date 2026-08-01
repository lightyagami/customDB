import subprocess
import os

def run_db(db_file, commands):
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate("\n".join(commands) + "\n")
    return out.splitlines()

def test_aggregates():
    db_file = "test_aggregates.db"
    if os.path.exists(db_file):
        os.remove(db_file)

    init_cmds = [
        "create table sales (id INT, amount DOUBLE, qty INT)",
        "insert into sales values (1, 100.0, 2)",
        "insert into sales values (2, 200.0, 5)",
        "insert into sales values (3, 300.0, 3)",
        "select count(*), sum(amount), avg(amount), min(amount), max(amount) from sales",
        "select count(*), sum(amount) from sales where amount > 150.0",
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

    assert len(results) == 2, f"Expected 2 result rows, got:\n{lines}"
    assert results[0] == "(3, 600.00, 200.00, 100.00, 300.00)", f"Unexpected aggregate row 1: {results[0]}"
    assert results[1] == "(2, 500.00)", f"Unexpected aggregate row 2: {results[1]}"

    if os.path.exists(db_file):
        os.remove(db_file)
    print("✓ Aggregate functions (COUNT/SUM/AVG/MIN/MAX) integration test passed!")

if __name__ == "__main__":
    test_aggregates()
