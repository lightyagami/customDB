import subprocess
import os

def run_db(db_file, commands):
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate("\n".join(commands) + "\n")
    return out.splitlines()

def test_ctes():
    db_file = "test_ctes.db"
    if os.path.exists(db_file):
        os.remove(db_file)

    # 1. Initialize employees table
    cmds = [
        "create table employees (id INT, dept VARCHAR(50), salary DOUBLE)",
        "insert into employees values (1, 'Engineering', 95000.0)",
        "insert into employees values (2, 'Engineering', 80000.0)",
        "insert into employees values (3, 'Marketing', 60000.0)",
        "insert into employees values (4, 'Marketing', 55000.0)",
        ".exit"
    ]
    run_db(db_file, cmds)

    # 2. Query using CTE: WITH high_sal AS (SELECT * FROM employees WHERE salary >= 80000) SELECT * FROM high_sal
    cte_cmds = [
        "with high_sal as (select * from employees where salary >= 80000) select * from high_sal",
        ".exit"
    ]
    lines_cte = run_db(db_file, cte_cmds)
    out_cte = "\n".join(lines_cte)
    assert "(1, Engineering, 95000)" in out_cte, f"Missing Employee 1 in CTE output:\n{out_cte}"
    assert "(2, Engineering, 80000)" in out_cte, f"Missing Employee 2 in CTE output:\n{out_cte}"
    assert "Marketing" not in out_cte, f"Marketing should be filtered out by CTE:\n{out_cte}"

    # Clean up
    if os.path.exists(db_file):
        os.remove(db_file)

    print("✓ Common Table Expressions (WITH cte_name AS (...)) integration test passed!")

if __name__ == "__main__":
    test_ctes()
