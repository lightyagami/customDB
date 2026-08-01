import subprocess
import os
import time

def test_concurrency_locking():
    db_file = "test_lock.db"
    if os.path.exists(db_file):
        os.remove(db_file)

    # 1. Initialize database with one table and row
    p_init = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    p_init.communicate("create table items (id INT, name VARCHAR(50))\ninsert into items values (1, 'Initial')\n.exit\n")

    # 2. Spawn Process A and begin a write transaction (acquires RESERVED lock)
    p_a = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    p_a.stdin.write("begin\n")
    p_a.stdin.write("insert into items values (2, 'ProcessA')\n")
    p_a.stdin.flush()
    time.sleep(0.5) # Wait for Process A to write lock the database

    # 3. Spawn Process B.
    # Process B should still be able to READ from the database concurrently
    p_b_read = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out_b_read, err_b_read = p_b_read.communicate("select * from items\n.exit\n")
    assert "Initial" in out_b_read, f"Process B failed to read initial row! Output:\n{out_b_read}"
    assert "ProcessA" not in out_b_read, f"Process B read uncommitted transaction data! Output:\n{out_b_read}"
    print("✓ Concurrent readers allowed while write transaction holds RESERVED lock")

    # Process B tries to WRITE (should fail/timeout because Process A holds RESERVED lock)
    p_b_write = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out_b_write, err_b_write = p_b_write.communicate("insert into items values (3, 'ProcessB')\n.exit\n")
    assert "Database is locked" in out_b_write or "Database is locked" in err_b_write, f"Process B write should have failed! Output:\n{out_b_write}\nStderr:\n{err_b_write}"
    print("✓ Writer blocks and times out correctly when another writer holds the lock")

    # 4. Commit Process A transaction and close it
    p_a.stdin.write("commit\n.exit\n")
    p_a.stdin.flush()
    time.sleep(0.5)

    # 5. Process B should now be able to write successfully!
    p_b_write2 = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out_b_write2, err_b_write2 = p_b_write2.communicate("insert into items values (3, 'ProcessB')\nselect * from items\n.exit\n")
    
    assert "ProcessA" in out_b_write2, "Process A changes not visible after commit!"
    assert "ProcessB" in out_b_write2, "Process B failed to write after lock was released!"
    print("✓ Lock released successfully; new writes processed successfully")

    # Clean up
    if os.path.exists(db_file):
        os.remove(db_file)
    print("All concurrency locking tests passed!")

if __name__ == "__main__":
    test_concurrency_locking()
