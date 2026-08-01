#!/usr/bin/env python3
import subprocess
import os

DB_FILE = "test_alter.db"
EXE = "./db"

def run_db(db_path, commands):
    p = subprocess.Popen([EXE, db_path], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    input_str = "\n".join(commands) + "\n.exit\n"
    out, err = p.communicate(input_str)
    lines = [line.replace("db >", "").strip() for line in out.split("\n") if line.replace("db >", "").strip()]
    return lines

def test_alter():
    if os.path.exists(DB_FILE):
        os.remove(DB_FILE)

    # 1. RENAME TABLE
    cmds1 = [
        "CREATE TABLE old_name (id INT, value TEXT)",
        "INSERT INTO old_name VALUES (1, 'test')",
        "ALTER TABLE old_name RENAME TO new_name",
        "SELECT * FROM new_name"
    ]
    lines1 = run_db(DB_FILE, cmds1)
    out1 = "\n".join(lines1)
    assert "(1, test)" in out1, f"Rename failed: {out1}"
    print("✓ ALTER TABLE RENAME TO verified successfully")

    # 2. ADD COLUMN
    cmds2 = [
        "ALTER TABLE new_name ADD COLUMN age INT",
        "INSERT INTO new_name VALUES (2, 'hello', 25)",
        "SELECT * FROM new_name"
    ]
    lines2 = run_db(DB_FILE, cmds2)
    out2 = "\n".join(lines2)
    assert "(2, hello, 25)" in out2, f"Add column failed: {out2}"
    print("✓ ALTER TABLE ADD COLUMN verified successfully")

    # 3. DROP COLUMN
    cmds3 = [
        "ALTER TABLE new_name DROP COLUMN value",
        "SELECT * FROM new_name"
    ]
    lines3 = run_db(DB_FILE, cmds3)
    out3 = "\n".join(lines3)
    assert "(1, 0)" in out3 or "(1)" in out3, f"Drop column failed: {out3}"
    assert "(2, 25)" in out3, f"Drop column row 2 failed: {out3}"
    print("✓ ALTER TABLE DROP COLUMN verified successfully")

    if os.path.exists(DB_FILE):
        os.remove(DB_FILE)

if __name__ == "__main__":
    test_alter()
