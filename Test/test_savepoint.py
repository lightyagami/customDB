import subprocess
import os

def run_db(db_file, commands):
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate("\n".join(commands) + "\n")
    return out.splitlines()

def test_savepoint():
    db_file = "test_savepoint.db"
    if os.path.exists(db_file):
        os.remove(db_file)

    # 1. Initialize table
    init_cmds = [
        "create table accounts (id INT, name VARCHAR(50), balance DOUBLE)",
        "insert into accounts values (1, 'Alice', 1000.0)",
        ".exit"
    ]
    run_db(db_file, init_cmds)

    # 2. Transaction with savepoints
    tx_cmds = [
        "begin",
        "insert into accounts values (2, 'Bob', 500.0)",
        "savepoint sp1",
        "insert into accounts values (3, 'Charlie', 250.0)",
        "savepoint sp2",
        "insert into accounts values (4, 'David', 100.0)",
        "rollback to savepoint sp2",                          # Reverts David (id 4)
        "select * from accounts",
        "rollback to savepoint sp1",                          # Reverts Charlie (id 3)
        "select * from accounts",
        "commit",
        ".exit"
    ]

    lines = run_db(db_file, tx_cmds)
    full_output = "\n".join(lines)

    assert "Savepoint 'sp1' created." in full_output, f"Expected sp1 creation output, got:\n{full_output}"
    assert "Savepoint 'sp2' created." in full_output, f"Expected sp2 creation output, got:\n{full_output}"
    assert "Rolled back to savepoint 'sp2'." in full_output, f"Expected sp2 rollback, got:\n{full_output}"
    assert "Rolled back to savepoint 'sp1'." in full_output, f"Expected sp1 rollback, got:\n{full_output}"

    # 3. Final verification of remaining rows (Alice & Bob)
    check_cmds = [
        "select * from accounts",
        ".exit"
    ]
    final_lines = run_db(db_file, check_cmds)
    final_out = "\n".join(final_lines)

    assert "(1, Alice," in final_out, f"Missing Alice in final output:\n{final_out}"
    assert "(2, Bob," in final_out, f"Missing Bob in final output:\n{final_out}"
    assert "Charlie" not in final_out, f"Charlie should have been rolled back:\n{final_out}"
    assert "David" not in final_out, f"David should have been rolled back:\n{final_out}"

    if os.path.exists(db_file):
        os.remove(db_file)
    print("✓ Transaction Savepoints (SAVEPOINT, ROLLBACK TO, RELEASE) integration test passed!")

if __name__ == "__main__":
    test_savepoint()
