import subprocess
import os

def run_db(db_file, commands):
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate("\n".join(commands) + "\n")
    return out.splitlines()

def test_wal_mode():
    db_file = "test_wal_mode.db"
    wal_file = "test_wal_mode.db-wal"

    if os.path.exists(db_file):
        os.remove(db_file)
    if os.path.exists(wal_file):
        os.remove(wal_file)

    # 1. Enable WAL mode and insert rows
    init_cmds = [
        "create table users (id INT, val DOUBLE)",
        "pragma journal_mode = wal",
        "insert into users values (1, 10.5)",
        "insert into users values (2, 20.5)",
        "insert into users values (3, 30.5)",
        ".exit"
    ]
    lines_init = run_db(db_file, init_cmds)
    assert any("[WAL] Journal mode set to WAL mode." in l for l in lines_init), f"Expected WAL mode set message, got: {lines_init}"
    assert os.path.exists(wal_file), "Expected -wal file to exist after writes in WAL mode!"
    assert os.path.getsize(wal_file) > 0, "-wal file should not be empty after writes!"
    print("✓ WAL mode enabled and page frames appended to -wal file")

    # 2. Select rows in WAL mode (verify read redirection from WAL file)
    sel_cmds = [
        "pragma journal_mode = wal",
        "select * from users order by id asc",
        ".exit"
    ]
    lines_sel = run_db(db_file, sel_cmds)
    tuples = []
    for l in lines_sel:
        s = l.strip()
        if "db >" in s:
            s = s.replace("db >", "").strip()
        if s.startswith("("):
            tuples.append(s)

    assert len(tuples) == 3, f"Expected 3 users read via WAL, got {len(tuples)}!"
    assert tuples[0] == "(1, 10.5)", f"Expected tuple (1, 10.5), got {tuples[0]}!"
    print("✓ Read redirection from -wal file verified successfully")

    # 3. Test WAL Checkpointing
    chk_cmds = [
        "pragma journal_mode = wal",
        "pragma wal_checkpoint",
        ".exit"
    ]
    lines_chk = run_db(db_file, chk_cmds)
    assert any("[WAL] Checkpoint completed." in l for l in lines_chk), f"Expected checkpoint completed message, got: {lines_chk}"
    assert os.path.getsize(wal_file) == 0, "-wal file should be truncated to 0 after checkpoint!"
    print("✓ PRAGMA wal_checkpoint successfully flushed frames into main database file")

    # 4. Switch back to Rollback Journaling
    rb_cmds = [
        "pragma journal_mode = delete",
        "select * from users order by id asc",
        ".exit"
    ]
    lines_rb = run_db(db_file, rb_cmds)
    tuples_rb = []
    for l in lines_rb:
        s = l.strip()
        if "db >" in s:
            s = s.replace("db >", "").strip()
        if s.startswith("("):
            tuples_rb.append(s)

    assert len(tuples_rb) == 3, f"Expected 3 users after returning to rollback mode, got {len(tuples_rb)}!"
    assert not os.path.exists(wal_file), "-wal file should be removed after switching to delete mode!"
    print("✓ Returned to Rollback mode (DELETE) successfully")

    # Clean up
    if os.path.exists(db_file):
        os.remove(db_file)
    if os.path.exists(wal_file):
        os.remove(wal_file)
    print("All Write-Ahead Logging (WAL) integration tests passed!")

if __name__ == "__main__":
    test_wal_mode()
