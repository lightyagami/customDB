import subprocess
import os

def run_db(db_file, commands):
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate("\n".join(commands) + "\n")
    return out.splitlines()

def test_groupby():
    db_file = "test_groupby.db"
    if os.path.exists(db_file):
        os.remove(db_file)

    init_cmds = [
        "create table emp (id INT, dept VARCHAR(50), salary DOUBLE)",
        "insert into emp values (1, 'HR', 5000.0)",
        "insert into emp values (2, 'HR', 6000.0)",
        "insert into emp values (3, 'IT', 8000.0)",
        "insert into emp values (4, 'IT', 9000.0)",
        "insert into emp values (5, 'IT', 7000.0)",
        "insert into emp values (6, 'Sales', 4000.0)",
        "select count(*), sum(salary) from emp group by dept",
        "select count(*), avg(salary) from emp group by dept having count(*) > 2",
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

    assert len(results) == 4, f"Expected 4 result rows (3 group rows + 1 having row), got:\n{results}"
    assert "(HR, 2, 11000.00)" in results, f"Expected HR group row, got:\n{results}"
    assert "(IT, 3, 24000.00)" in results, f"Expected IT group row, got:\n{results}"
    assert "(Sales, 1, 4000.00)" in results, f"Expected Sales group row, got:\n{results}"
    assert "(IT, 3, 8000.00)" in results[-1], f"Expected HAVING IT row, got:\n{results}"

    if os.path.exists(db_file):
        os.remove(db_file)
    print("✓ GROUP BY and HAVING integration test passed!")

if __name__ == "__main__":
    test_groupby()
