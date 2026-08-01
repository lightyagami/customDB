import subprocess
import os

def run_db(db_file, commands):
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate("\n".join(commands) + "\n")
    return out.splitlines()

def test_memory():
    db_file = ":memory:"
    export_file = "memory_export.db"

    if os.path.exists(export_file):
        os.remove(export_file)

    # 1. Create table and insert data into pure RAM :memory: database
    cmds = [
        "create table RAM_users (id INT, name VARCHAR(50))",
        "insert into RAM_users values (1, 'Alice')",
        "insert into RAM_users values (2, 'Bob')",
        "select * from RAM_users",
        f"vacuum into '{export_file}'",
        ".exit"
    ]
    lines = run_db(db_file, cmds)
    out = "\n".join(lines)
    assert "(1, Alice)" in out and "(2, Bob)" in out, f"RAM database query failed:\n{out}"
    assert "exported to 'memory_export.db' via VACUUM INTO" in out, f"VACUUM INTO failed:\n{out}"

    # 2. Verify exported disk database file exists and contains the exported RAM data
    assert os.path.exists(export_file), "Exported database file does not exist!"

    verify_cmds = [
        "select * from RAM_users",
        ".exit"
    ]
    lines_v = run_db(export_file, verify_cmds)
    out_v = "\n".join(lines_v)
    assert "(1, Alice)" in out_v and "(2, Bob)" in out_v, f"Exported disk database data verification failed:\n{out_v}"

    if os.path.exists(export_file):
        os.remove(export_file)

    print("✓ In-Memory Database Mode (:memory:) & VACUUM INTO export integration test passed!")

if __name__ == "__main__":
    test_memory()
