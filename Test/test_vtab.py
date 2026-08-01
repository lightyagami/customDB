import subprocess
import os

def run_db(db_file, commands):
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate("\n".join(commands) + "\n")
    return out.splitlines()

def test_vtab():
    db_file = "test_vtab.db"
    csv_file = "users_data.csv"

    if os.path.exists(db_file):
        os.remove(db_file)

    # 1. Create external CSV file
    with open(csv_file, "w") as f:
        f.write("id,username,city\n")
        f.write("1,Alice,New York\n")
        f.write("2,Bob,London\n")
        f.write("3,Charlie,Tokyo\n")

    # 2. Create Virtual Table using csv module
    create_cmds = [
        f"create virtual table v_users using csv('{csv_file}')",
        ".exit"
    ]
    lines_create = run_db(db_file, create_cmds)
    out_create = "\n".join(lines_create)
    assert "Virtual table 'v_users' created using module 'csv'" in out_create, f"Virtual table creation failed:\n{out_create}"

    # 3. Query Virtual Table
    query_cmds = [
        "select * from v_users where id = 2",
        ".exit"
    ]
    lines_q = run_db(db_file, query_cmds)
    out_q = "\n".join(lines_q)
    assert "(2, Bob, London)" in out_q, f"Virtual table query failed:\n{out_q}"

    # Clean up
    if os.path.exists(db_file):
        os.remove(db_file)
    if os.path.exists(csv_file):
        os.remove(csv_file)

    print("✓ Virtual Tables Framework (CREATE VIRTUAL TABLE ... USING csv(...)) integration test passed!")

if __name__ == "__main__":
    test_vtab()
