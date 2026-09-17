import subprocess
import os

def run_db(db_file, commands):
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate("\n".join(commands) + "\n")
    return out.splitlines()

def test_recursive_cte():
    db_file = "test_recursive.db"
    if os.path.exists(db_file):
        os.remove(db_file)

    # 1. Execute WITH RECURSIVE cnt(x) AS (...) SELECT * FROM cnt
    cmds = [
        "with recursive cnt(x) as (select 1 union all select x+1 from cnt where x < 5) select * from cnt",
        ".exit"
    ]
    lines = run_db(db_file, cmds)
    out = "\n".join(lines)

    assert "(1)" in out and "(5)" in out, f"Recursive CTE output check failed:\n{out}"
    assert "(6)" not in out, f"Recursive CTE ran past WHERE condition x < 5:\n{out}"
    rows = [line.replace("db > ", "").strip() for line in lines if "(" in line and line.strip().endswith(")")]
    assert rows == ["(1)", "(2)", "(3)", "(4)", "(5)"], f"Unexpected rows returned: {rows}"

    # 2. Execute with bound x < 3 to ensure dynamically evaluated condition
    cmds_3 = [
        "with recursive cnt(x) as (select 1 union all select x+1 from cnt where x < 3) select * from cnt",
        ".exit"
    ]
    lines_3 = run_db(db_file, cmds_3)
    rows_3 = [line.replace("db > ", "").strip() for line in lines_3 if "(" in line and line.strip().endswith(")")]
    assert rows_3 == ["(1)", "(2)", "(3)"], f"Expected 3 rows for x < 3, got: {rows_3}"

    if os.path.exists(db_file):
        os.remove(db_file)

    print("✓ Recursive Common Table Expressions (WITH RECURSIVE) integration test passed!")

if __name__ == "__main__":
    test_recursive_cte()
