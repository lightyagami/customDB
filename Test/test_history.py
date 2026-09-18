import subprocess
import os
import pytest

def run_db(db_path, commands):
    proc = subprocess.Popen(
        ["./db", db_path],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    stdout, stderr = proc.communicate("\n".join(commands) + "\n")
    lines = [line.strip() for line in stdout.splitlines() if line.strip()]
    return lines

def test_temporal_history_tables():
    db_file = "test_history.db"
    if os.path.exists(db_file):
        os.remove(db_file)

    # 1. Create table WITH HISTORY
    init_cmds = [
        "create table products (id INT, name VARCHAR(50), price INT) WITH HISTORY",
        "insert into products values (1, 'Widget', 100)",
        "update products set price = 120 where id = 1",
        "delete from products where id = 1",
        ".exit"
    ]
    out_init = "\n".join(run_db(db_file, init_cmds))
    assert "Executed." in out_init

    # 2. Query history table
    hist_cmds = [
        "select history_action, id, price from _history_products",
        ".exit"
    ]
    out_hist = "\n".join(run_db(db_file, hist_cmds))
    assert "(INSERT, 1, 100)" in out_hist, f"Expected INSERT snapshot in history: {out_hist}"
    assert "(UPDATE, 1, 120)" in out_hist, f"Expected UPDATE snapshot in history: {out_hist}"
    assert "(DELETE, 1, 120)" in out_hist, f"Expected DELETE snapshot in history: {out_hist}"

    # Clean up
    if os.path.exists(db_file):
        os.remove(db_file)

def test_history_combined_with_ttl_ordering():
    db_file = "test_history_ttl.db"
    if os.path.exists(db_file):
        os.remove(db_file)

    # Test WITH TTL first, then WITH HISTORY
    cmds = [
        "create table s1 (id INT, note TEXT) WITH TTL = 60 WITH HISTORY",
        "create table s2 (id INT, note TEXT) WITH HISTORY WITH TTL = 60",
        "insert into s1 values (1, 'active')",
        "insert into s2 values (2, 'active')",
        "select history_action, id from _history_s1",
        "select history_action, id from _history_s2",
        ".exit"
    ]
    out = "\n".join(run_db(db_file, cmds))
    assert "(INSERT, 1)" in out, f"Expected _history_s1 to record insert: {out}"
    assert "(INSERT, 2)" in out, f"Expected _history_s2 to record insert: {out}"

    if os.path.exists(db_file):
        os.remove(db_file)
