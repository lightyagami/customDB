#!/usr/bin/env python3
import subprocess
import os

DB_FILE = "test_partial_index.db"
EXE = "./db"

def run_db(db_path, commands):
    p = subprocess.Popen([EXE, db_path], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    input_str = "\n".join(commands) + "\n.exit\n"
    out, err = p.communicate(input_str)
    lines = [line.replace("db >", "").strip() for line in out.split("\n") if line.replace("db >", "").strip()]
    return lines

def test_partial_index():
    if os.path.exists(DB_FILE):
        os.remove(DB_FILE)

    cmds = [
        "CREATE TABLE users (id INT, name TEXT, age INT, status TEXT)",
        "INSERT INTO users VALUES (1, 'Alice', 25, 'active')",
        "INSERT INTO users VALUES (2, 'Bob', 17, 'inactive')",
        "INSERT INTO users VALUES (3, 'Charlie', 30, 'active')",
        "CREATE INDEX idx_active_age ON users (age) WHERE status = 'active'",
        "SELECT * FROM users WHERE status = 'active'"
    ]
    lines = run_db(DB_FILE, cmds)
    out = "\n".join(lines)
    assert "(1, Alice, 25, active)" in out, f"Alice missing: {out}"
    assert "(3, Charlie, 30, active)" in out, f"Charlie missing: {out}"
    assert "Bob" not in out, f"Bob should not be in active users: {out}"
    print("✓ Partial Index (CREATE INDEX ... WHERE status = 'active') verified successfully")

    if os.path.exists(DB_FILE):
        os.remove(DB_FILE)

if __name__ == "__main__":
    test_partial_index()
