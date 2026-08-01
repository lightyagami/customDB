import subprocess
import os

def run_db(db_file, commands):
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate("\n".join(commands) + "\n")
    return out.splitlines()

def test_dynamic_catalog():
    db_file = "test_dynamic_catalog.db"
    if os.path.exists(db_file):
        os.remove(db_file)

    # 1. Create 25 tables dynamically in session 1
    create_cmds = []
    for i in range(1, 26):
        create_cmds.append(f"create table tbl_{i} (id INT, val VARCHAR(50))")
        create_cmds.append(f"insert into tbl_{i} values (1, 'val_{i}')")
    create_cmds.append(".exit")

    run_db(db_file, create_cmds)

    # 2. Re-open database file in session 2 and verify schema & data loading across process restart
    verify_cmds = []
    for i in range(1, 26):
        verify_cmds.append(f"select * from tbl_{i}")
    verify_cmds.append(".exit")

    lines = run_db(db_file, verify_cmds)
    full_output = "\n".join(lines)

    for i in range(1, 26):
        assert f"(1, val_{i})" in full_output, f"Missing data for tbl_{i} in output:\n{full_output}"

    if os.path.exists(db_file):
        os.remove(db_file)
    print("✓ Dynamic Multi-Page Catalog (25+ Tables) integration test passed!")

if __name__ == "__main__":
    test_dynamic_catalog()
