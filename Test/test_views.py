import subprocess
import os

def run_db(db_file, commands):
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate("\n".join(commands) + "\n")
    return out.splitlines()

def test_views():
    db_file = "test_views.db"
    if os.path.exists(db_file):
        os.remove(db_file)

    # 1. Setup base table
    cmds = [
        "create table users (id INT, dept VARCHAR(50), active INT)",
        "insert into users values (1, 'Engineering', 1)",
        "insert into users values (2, 'Marketing', 0)",
        "insert into users values (3, 'Engineering', 1)",
        "create view active_eng as select * from users where dept = 'Engineering'",
        ".exit"
    ]
    run_db(db_file, cmds)

    # 2. Query View
    view_cmds = [
        "select * from active_eng",
        ".exit"
    ]
    lines = run_db(db_file, view_cmds)
    out = "\n".join(lines)
    assert "(1, Engineering, 1)" in out, f"Missing User 1 in View output:\n{out}"
    assert "(3, Engineering, 1)" in out, f"Missing User 3 in View output:\n{out}"
    assert "Marketing" not in out, f"Marketing should be filtered out by View:\n{out}"

    # 3. Drop View
    drop_cmds = [
        "drop view active_eng",
        ".exit"
    ]
    run_db(db_file, drop_cmds)

    if os.path.exists(db_file):
        os.remove(db_file)

    print("✓ Views (CREATE VIEW, View Query Expansion, DROP VIEW) integration test passed!")

if __name__ == "__main__":
    test_views()
