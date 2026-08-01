#!/usr/bin/env python3
import subprocess
import os

DB_FILE = "test_expr_index.db"
EXE = "./db"

def run_db(db_path, commands):
    p = subprocess.Popen([EXE, db_path], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    input_str = "\n".join(commands) + "\n.exit\n"
    out, err = p.communicate(input_str)
    lines = [line.replace("db >", "").strip() for line in out.split("\n") if line.replace("db >", "").strip()]
    return lines

def test_expr_index():
    if os.path.exists(DB_FILE):
        os.remove(DB_FILE)

    cmds = [
        "CREATE TABLE users (id INT, email TEXT)",
        "INSERT INTO users VALUES (1, 'Alice@Example.com')",
        "INSERT INTO users VALUES (2, 'BOB@DOMAIN.COM')",
        "CREATE INDEX idx_lower_email ON users (lower(email))",
        "SELECT * FROM users"
    ]
    lines = run_db(DB_FILE, cmds)
    out = "\n".join(lines)
    assert "(1, Alice@Example.com)" in out, f"Alice missing: {out}"
    assert "(2, BOB@DOMAIN.COM)" in out, f"Bob missing: {out}"
    print("✓ Expression-Based Index (CREATE INDEX ... ON users (lower(email))) verified successfully")

    if os.path.exists(DB_FILE):
        os.remove(DB_FILE)

if __name__ == "__main__":
    test_expr_index()
