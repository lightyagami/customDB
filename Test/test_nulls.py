import subprocess
import os

def run_db(db_file, commands):
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate("\n".join(commands) + "\n")
    return out.splitlines()

def test_null_system():
    db_file = "test_nulls.db"
    if os.path.exists(db_file):
        os.remove(db_file)

    # 1. Create table & insert NULL values
    cmds = [
        "create table users (id INT, age INT, name VARCHAR(50))",
        "insert into users values (1, NULL, 'Alice')",
        "insert into users values (2, 25, NULL)",
        "insert into users values (3, 30, 'Bob')",
        ".exit"
    ]
    run_db(db_file, cmds)

    # 2. Test IS NULL query
    null_cmds = [
        "select * from users where age is null",
        ".exit"
    ]
    lines_null = run_db(db_file, null_cmds)
    out_null = "\n".join(lines_null)
    assert "(1, NULL, Alice)" in out_null, f"Expected (1, NULL, Alice), got:\n{out_null}"
    assert "Bob" not in out_null, f"Bob should not match IS NULL filter:\n{out_null}"

    # 3. Test IS NOT NULL query
    not_null_cmds = [
        "select * from users where age is not null",
        ".exit"
    ]
    lines_not_null = run_db(db_file, not_null_cmds)
    out_not_null = "\n".join(lines_not_null)
    assert "(2, 25, NULL)" in out_not_null, f"Expected (2, 25, NULL), got:\n{out_not_null}"
    assert "(3, 30, Bob)" in out_not_null, f"Expected (3, 30, Bob), got:\n{out_not_null}"
    assert "Alice" not in out_not_null, f"Alice should not match IS NOT NULL filter:\n{out_not_null}"

    # 4. Test COALESCE scalar function
    coalesce_cmds = [
        "select coalesce(age, 0) from users",
        ".exit"
    ]
    lines_coal = run_db(db_file, coalesce_cmds)
    out_coal = "\n".join(lines_coal)
    assert "(0)" in out_coal, f"Expected COALESCE default (0), got:\n{out_coal}"

    # 5. Test literal string 'NULL' vs SQL NULL
    str_null_cmds = [
        "create table str_test (id INT, txt VARCHAR(50))",
        "insert into str_test values (1, 'NULL'), (2, NULL)",
        ".exit"
    ]
    run_db(db_file, str_null_cmds)

    out1 = "\n".join(run_db(db_file, ["select * from str_test where txt is null", ".exit"]))
    assert "(2, NULL)" in out1, f"Row 2 should match IS NULL: {out1}"
    assert "(1, NULL)" not in out1, f"Row 1 should NOT match IS NULL: {out1}"

    out2 = "\n".join(run_db(db_file, ["select * from str_test where txt = 'NULL'", ".exit"]))
    assert "(1, NULL)" in out2, f"Row 1 should match txt = 'NULL': {out2}"
    assert "(2, NULL)" not in out2, f"Row 2 should NOT match txt = 'NULL': {out2}"

    # Clean up
    if os.path.exists(db_file):
        os.remove(db_file)

    print("✓ SQL NULL Value System (IS NULL, IS NOT NULL, COALESCE, Disk Persistence) integration test passed!")

if __name__ == "__main__":
    test_null_system()
