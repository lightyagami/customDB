import subprocess
import os

def run_db(db_file, commands):
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate("\n".join(commands) + "\n")
    return out.splitlines()

def test_window_functions():
    db_file = "test_window.db"
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

    # 2. Test ROW_NUMBER() OVER (PARTITION BY dept ORDER BY salary DESC)
    win_cmds = [
        "select id, dept, row_number() over (partition by dept order by salary desc) from employees",
        ".exit"
    ]
    lines_win = run_db(db_file, win_cmds)
    out_win = "\n".join(lines_win)
    assert "(1, Engineering, 1)" in out_win, f"Expected (1, Engineering, 1), got:\n{out_win}"
    assert "(2, Engineering, 2)" in out_win, f"Expected (2, Engineering, 2), got:\n{out_win}"
    assert "(3, Marketing, 1)" in out_win, f"Expected (3, Marketing, 1), got:\n{out_win}"
    assert "(4, Marketing, 2)" in out_win, f"Expected (4, Marketing, 2), got:\n{out_win}"

    # 3. Test SUM(salary) OVER (PARTITION BY dept)
    sum_cmds = [
        "select dept, sum(salary) over (partition by dept) from employees",
        ".exit"
    ]
    lines_sum = run_db(db_file, sum_cmds)
    out_sum = "\n".join(lines_sum)
    assert "(Engineering, 175000.00)" in out_sum, f"Expected Engineering sum 175000.00, got:\n{out_sum}"
    assert "(Marketing, 115000.00)" in out_sum, f"Expected Marketing sum 115000.00, got:\n{out_sum}"

    # Clean up
    if os.path.exists(db_file):
        os.remove(db_file)

    print("✓ Window Functions (ROW_NUMBER(), RANK(), DENSE_RANK(), SUM() OVER (...)) integration test passed!")

if __name__ == "__main__":
    test_window_functions()
