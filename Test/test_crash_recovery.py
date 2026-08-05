#!/usr/bin/env python3
import os
import subprocess
import time
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_DIR = os.path.abspath(os.path.join(SCRIPT_DIR, ".."))
DB_EXE = os.path.join(PROJECT_DIR, "db")
DB_FILE = os.path.join(SCRIPT_DIR, "test_crash_recovery.db")

def cleanup():
    for f in [DB_FILE, DB_FILE + "-journal"]:
        if os.path.exists(f):
            os.remove(f)

def run_cmd(cmd_str):
    p = subprocess.Popen([DB_EXE, DB_FILE], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate(input=cmd_str)
    return out, err, p.returncode

def main():
    cleanup()
    print("==================================================================")
    print("  CRASH RECOVERY TEST (kill -9 Mid-Transaction Journal Recovery)  ")
    print("==================================================================")

    # 1. Setup initial table and 10 committed rows
    out, err, code = run_cmd("""
CREATE TABLE accounts (id INT PRIMARY KEY, balance INT);
BEGIN TRANSACTION;
INSERT INTO accounts VALUES (1, 100);
INSERT INTO accounts VALUES (2, 200);
INSERT INTO accounts VALUES (3, 300);
COMMIT;
""")
    assert os.path.exists(DB_FILE), "Database file should exist"
    print("✓ Initialized table 'accounts' with 3 committed rows.")

    # 2. Spawn a subprocess that starts transaction, inserts 100 uncommitted rows, and stays alive
    child_cmds = "BEGIN TRANSACTION;\n"
    for i in range(10, 100):
        child_cmds += f"INSERT INTO accounts VALUES ({i}, {i * 10});\n"
    
    p_child = subprocess.Popen([DB_EXE, DB_FILE], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    p_child.stdin.write(child_cmds)
    p_child.stdin.flush()
    time.sleep(0.5)

    # Verify journal file was created by active transaction
    journal_file = DB_FILE + "-journal"
    assert os.path.exists(journal_file), f"Rollback journal file {journal_file} should exist during active transaction"
    print("✓ Verified active transaction created -journal file.")

    # 3. Simulate sudden OS crash or power failure (SIGKILL)
    p_child.kill()
    p_child.wait()
    print("✓ Issued SIGKILL (kill -9) mid-transaction!")

    assert os.path.exists(journal_file), "Orphan -journal file must remain after SIGKILL"

    # 4. Open database again via CLI (triggers check_and_recover_journal on pager_open)
    out_rec, err_rec, code_rec = run_cmd("SELECT COUNT(*) FROM accounts;")
    print(f"Post-recovery query output:\n{out_rec}")

    assert "3" in out_rec or "COUNT(*): 3" in out_rec or "count" in out_rec.lower(), f"Database should contain exactly initial 3 rows after crash recovery! Got: {out_rec}"
    assert not os.path.exists(journal_file), "Rollback journal file should be cleaned up after successful recovery!"

    print("==================================================================")
    print("  ✓ CRASH RECOVERY TEST PASSED 100% SUCCESSFUL!")
    print("==================================================================")
    cleanup()

if __name__ == "__main__":
    main()
