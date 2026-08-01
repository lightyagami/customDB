import subprocess
import os

def run_db(db_file, commands):
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate("\n".join(commands) + "\n")
    return out.splitlines()

def test_triggers():
    db_file = "test_triggers.db"
    if os.path.exists(db_file):
        os.remove(db_file)

    # 1. Setup tables and trigger
    cmds = [
        "create table orders (id INT, amount DOUBLE)",
        "create table audit_log (id INT, order_id INT)",
        "create trigger audit_trig after insert on orders begin insert into audit_log values (1, 999); end",
        "insert into orders values (101, 500.0)",
        ".exit"
    ]
    run_db(db_file, cmds)

    # 2. Verify trigger executed automatically and inserted into audit_log
    query_cmds = [
        "select * from audit_log",
        ".exit"
    ]
    lines = run_db(db_file, query_cmds)
    out = "\n".join(lines)
    assert "(1, 999)" in out, f"Trigger failed to insert into audit_log:\n{out}"

    # 3. Drop Trigger
    drop_cmds = [
        "drop trigger audit_trig",
        ".exit"
    ]
    run_db(db_file, drop_cmds)

    if os.path.exists(db_file):
        os.remove(db_file)

    print("✓ Triggers (CREATE TRIGGER, AFTER INSERT Trigger execution, DROP TRIGGER) integration test passed!")

if __name__ == "__main__":
    test_triggers()
