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

    # 3. Test NEW/OLD references in trigger body (e.g. UPDATE ... WHERE id = new.id)
    trig_cmds = [
        "create table t (id int primary key, val int)",
        "create trigger tr_upd after insert on t begin update t set val = val + 100 where id = new.id; end",
        "insert into t values (1, 10)",
        "insert into t values (2, 20)",
        ".exit"
    ]
    run_db(db_file, trig_cmds)

    query_t = [
        "select * from t",
        ".exit"
    ]
    lines_t = run_db(db_file, query_t)
    out_t = "\n".join(lines_t)
    assert "(1, 110)" in out_t, f"Trigger with new.id failed to update row 1: {out_t}"
    assert "(2, 120)" in out_t, f"Trigger with new.id failed to update row 2: {out_t}"

    # 4. Test OLD/NEW references on UPDATE
    upd_cmds = [
        "create table audit (id int primary key, old_val int, new_val int)",
        "create trigger tr_audit after update on t begin insert into audit values (old.id, old.val, new.val); end",
        "update t set val = 300 where id = 1",
        ".exit"
    ]
    run_db(db_file, upd_cmds)

    query_audit = [
        "select * from audit",
        ".exit"
    ]
    lines_audit = run_db(db_file, query_audit)
    out_audit = "\n".join(lines_audit)
    assert "(1, 110, 300)" in out_audit, f"AFTER UPDATE trigger with old/new failed: {out_audit}"

    # 5. Drop Trigger
    drop_cmds = [
        "drop trigger audit_trig",
        "drop trigger tr_upd",
        "drop trigger tr_audit",
        ".exit"
    ]
    run_db(db_file, drop_cmds)

    if os.path.exists(db_file):
        os.remove(db_file)

    print("✓ Triggers (CREATE TRIGGER, AFTER INSERT Trigger execution, NEW/OLD bindings, DROP TRIGGER) integration test passed!")

if __name__ == "__main__":
    test_triggers()
