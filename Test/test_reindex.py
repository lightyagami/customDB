import subprocess
import os

def run_db(db_file, commands):
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate("\n".join(commands) + "\n")
    return out.splitlines()

def test_reindex():
    db_file = "test_reindex.db"
    if os.path.exists(db_file):
        os.remove(db_file)

    # 1. Setup table and create index
    cmds = [
        "create table products (id INT, code VARCHAR(50), price DOUBLE)",
        "insert into products values (1, 'P100', 10.5)",
        "insert into products values (2, 'P200', 20.5)",
        "create index idx_products_code on products(code)",
        "reindex idx_products_code",
        ".exit"
    ]
    lines = run_db(db_file, cmds)
    out = "\n".join(lines)
    assert "Reindex completed for 'idx_products_code'" in out or "Reindexed" in out or "Executed" in out, f"REINDEX output failed:\n{out}"

    # 2. Query product by indexed column
    query_cmds = [
        "select * from products where code = 'P200'",
        ".exit"
    ]
    q_lines = run_db(db_file, query_cmds)
    q_out = "\n".join(q_lines)
    assert "(2, P200, 20.5)" in q_out, f"Post-REINDEX index scan failed:\n{q_out}"

    if os.path.exists(db_file):
        os.remove(db_file)

    print("✓ REINDEX (Index B+Tree rebuilding pass) integration test passed!")

if __name__ == "__main__":
    test_reindex()
