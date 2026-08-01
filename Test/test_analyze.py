import subprocess
import os

def run_db(db_file, commands):
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate("\n".join(commands) + "\n")
    return out.splitlines()

def test_analyze():
    db_file = "test_analyze.db"
    if os.path.exists(db_file):
        os.remove(db_file)

    # 1. Setup table and secondary index
    cmds = [
        "create table orders (id INT, customer_id INT, status VARCHAR(20))",
        "create index idx_cust on orders (customer_id)",
        "insert into orders values (1, 101, 'shipped')",
        "insert into orders values (2, 102, 'pending')",
        "insert into orders values (3, 101, 'delivered')",
        ".exit"
    ]
    run_db(db_file, cmds)

    # 2. Run ANALYZE
    analyze_cmds = [
        "ANALYZE",
        "select * from sqlite_stat1",
        ".exit"
    ]
    lines = run_db(db_file, analyze_cmds)
    out = "\n".join(lines)
    assert "ANALYZE completed. Internal statistics updated in sqlite_stat1." in out, f"ANALYZE command execution failed:\n{out}"
    assert "_idx_orders_customer_id" in out, f"Index statistics missing from sqlite_stat1:\n{out}"

    if os.path.exists(db_file):
        os.remove(db_file)

    print("✓ ANALYZE and sqlite_stat1 cost-based query statistics integration test passed!")

if __name__ == "__main__":
    test_analyze()
