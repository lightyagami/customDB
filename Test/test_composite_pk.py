#!/usr/bin/env python3
import subprocess
import os

DB_FILE = "test_composite_pk.db"
EXE = "./db"

def run_db(db_path, commands):
    p = subprocess.Popen([EXE, db_path], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    input_str = "\n".join(commands) + "\n.exit\n"
    out, err = p.communicate(input_str)
    lines = [line.replace("db >", "").strip() for line in out.split("\n") if line.replace("db >", "").strip()]
    return lines

def test_composite_pk():
    if os.path.exists(DB_FILE):
        os.remove(DB_FILE)

    cmds = [
        "CREATE TABLE order_items (order_id INT, item_id INT, qty INT, PRIMARY KEY (order_id, item_id))",
        "INSERT INTO order_items VALUES (101, 1, 5)",
        "INSERT INTO order_items VALUES (101, 2, 3)",
        "INSERT INTO order_items VALUES (102, 1, 10)",
        "SELECT * FROM order_items"
    ]
    lines = run_db(DB_FILE, cmds)
    out = "\n".join(lines)
    assert "(101, 1, 5)" in out, f"Composite PK item 1 missing: {out}"
    assert "(101, 2, 3)" in out, f"Composite PK item 2 missing: {out}"
    assert "(102, 1, 10)" in out, f"Composite PK item 3 missing: {out}"

    print("✓ Composite Primary Keys (PRIMARY KEY (col1, col2)) integration test passed!")

    if os.path.exists(DB_FILE):
        os.remove(DB_FILE)

if __name__ == "__main__":
    test_composite_pk()
