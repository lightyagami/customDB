import subprocess
import time
import os
import pytest

def run_db(cmds, db_path="test_ttl.db"):
    if os.path.exists(db_path):
        os.remove(db_path)
    proc = subprocess.Popen(["./db", db_path], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = proc.communicate(input=cmds)
    if os.path.exists(db_path):
        os.remove(db_path)
    return out

def test_row_level_ttl_table_default():
    # Table with default TTL = 1 second
    script = """
    CREATE TABLE cache (id INT, val TEXT) WITH TTL = 1;
    INSERT INTO cache VALUES (1, 'hello');
    INSERT INTO cache VALUES (2, 'world');
    SELECT * FROM cache;
    .exit
    """
    out = run_db(script, "test_ttl_1.db")
    assert "(1, hello)" in out
    assert "(2, world)" in out

    # Now sleep and test expiry
    db_path = "test_ttl_sleep.db"
    if os.path.exists(db_path):
        os.remove(db_path)
    p1 = subprocess.Popen(["./db", db_path], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    p1.communicate("""
    CREATE TABLE cache (id INT, val TEXT) WITH TTL = 1;
    INSERT INTO cache VALUES (1, 'temporary');
    .exit
    """)
    time.sleep(2)
    p2 = subprocess.Popen(["./db", db_path], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out2, _ = p2.communicate("""
    SELECT * FROM cache;
    .exit
    """)
    if os.path.exists(db_path):
        os.remove(db_path)
    assert "(1, temporary)" not in out2

def test_insert_expires_override():
    db_path = "test_ttl_override.db"
    if os.path.exists(db_path):
        os.remove(db_path)
    p1 = subprocess.Popen(["./db", db_path], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    p1.communicate("""
    CREATE TABLE cache (id INT, val TEXT);
    INSERT INTO cache VALUES (1, 'short_lived') EXPIRES 1;
    INSERT INTO cache VALUES (2, 'permanent');
    .exit
    """)
    time.sleep(2)
    p2 = subprocess.Popen(["./db", db_path], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out2, _ = p2.communicate("""
    SELECT * FROM cache;
    .exit
    """)
    if os.path.exists(db_path):
        os.remove(db_path)
    assert "(1, short_lived)" not in out2
    assert "(2, permanent)" in out2

def test_pragma_reap_expired():
    db_path = "test_ttl_reap.db"
    if os.path.exists(db_path):
        os.remove(db_path)
    p1 = subprocess.Popen(["./db", db_path], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    p1.communicate("""
    CREATE TABLE cache (id INT, val TEXT);
    INSERT INTO cache VALUES (1, 'exp1') EXPIRES 1;
    INSERT INTO cache VALUES (2, 'exp2') EXPIRES 1;
    INSERT INTO cache VALUES (3, 'keep');
    .exit
    """)
    time.sleep(2)
    p2 = subprocess.Popen(["./db", db_path], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out2, _ = p2.communicate("""
    PRAGMA reap_expired;
    SELECT * FROM cache;
    .exit
    """)
    if os.path.exists(db_path):
        os.remove(db_path)
    assert "Reaped 2 expired row(s)." in out2
    assert "(3, keep)" in out2
    assert "(1, exp1)" not in out2
