#!/usr/bin/env python3
import subprocess
import os

DB_FILE = "test_fk_cascade.db"
EXE = "./db"

def run_db(db_path, commands):
    p = subprocess.Popen([EXE, db_path], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    input_str = "\n".join(commands) + "\n.exit\n"
    out, err = p.communicate(input_str)
    lines = [line.replace("db >", "").strip() for line in out.split("\n") if line.replace("db >", "").strip()]
    return lines

def test_fk_cascade():
    if os.path.exists(DB_FILE):
        os.remove(DB_FILE)

    # 1. Test ON DELETE CASCADE
    cmds1 = [
        "CREATE TABLE parents (id INT, name TEXT)",
        "CREATE TABLE children (id INT, parent_id INT REFERENCES parents(id) ON DELETE CASCADE ON UPDATE CASCADE, title TEXT)",
        "INSERT INTO parents VALUES (1, 'Parent 1')",
        "INSERT INTO parents VALUES (2, 'Parent 2')",
        "INSERT INTO children VALUES (10, 1, 'Child 1 of P1')",
        "INSERT INTO children VALUES (11, 1, 'Child 2 of P1')",
        "INSERT INTO children VALUES (12, 2, 'Child 1 of P2')",
        "DELETE FROM parents WHERE id = 1",
        "SELECT * FROM children"
    ]
    lines1 = run_db(DB_FILE, cmds1)
    out1 = "\n".join(lines1)
    assert "(12, 2, Child 1 of P2)" in out1, f"Child of Parent 2 missing: {out1}"
    assert "Child 1 of P1" not in out1, f"Child 1 of P1 was not cascaded deleted: {out1}"
    assert "Child 2 of P1" not in out1, f"Child 2 of P1 was not cascaded deleted: {out1}"
    print("✓ Foreign Key ON DELETE CASCADE verified successfully")

    # 2. Test ON UPDATE CASCADE
    cmds2 = [
        "UPDATE parents SET id = 20 WHERE id = 2",
        "SELECT * FROM children"
    ]
    lines2 = run_db(DB_FILE, cmds2)
    out2 = "\n".join(lines2)
    assert "(12, 20, Child 1 of P2)" in out2, f"Child foreign key parent_id was not cascaded updated to 20: {out2}"
    print("✓ Foreign Key ON UPDATE CASCADE verified successfully")

    if os.path.exists(DB_FILE):
        os.remove(DB_FILE)

if __name__ == "__main__":
    test_fk_cascade()
