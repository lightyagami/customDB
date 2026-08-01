import subprocess
import os
import time

def test_transactions_and_crash_recovery():
    db_file = "test_acid.db"
    journal_file = "test_acid.db-journal"
    for f in [db_file, journal_file]:
        if os.path.exists(f):
            os.remove(f)

    # 1. Start process and begin transaction, insert row, then SIGKILL
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    p.stdin.write("create table items (id INT, name VARCHAR(50))\n")
    p.stdin.write("insert into items values (1, 'BaseItem')\n")
    p.stdin.write("begin\n")
    p.stdin.write("insert into items values (2, 'CrashItem')\n")
    p.stdin.flush()
    time.sleep(0.5)

    # Verify journal file was created during transaction
    assert os.path.exists(journal_file), "Journal file was not created during transaction!"
    print("✓ Rollback journal file created during active transaction")

    # SIGKILL the process mid-transaction
    p.kill()
    p.wait()
    print("✓ Process killed mid-transaction with SIGKILL")

    # 2. Re-open database; pager_open should detect journal and recover!
    p2 = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p2.communicate("select * from items\n.exit\n")

    assert "[RECOVERY]" in out, f"Recovery banner missing from output!\n{out}"
    assert "BaseItem" in out, "BaseItem missing after recovery!"
    assert "CrashItem" not in out, "CrashItem present after recovery (transaction failed to rollback!)"
    assert not os.path.exists(journal_file), "Journal file was not deleted after recovery!"
    print("✓ Crash recovery successfully restored database to pre-transaction state!")

    # Clean up
    for f in [db_file, journal_file]:
        if os.path.exists(f):
            os.remove(f)
    print("All transaction and ACID crash recovery tests passed!")

if __name__ == "__main__":
    test_transactions_and_crash_recovery()
