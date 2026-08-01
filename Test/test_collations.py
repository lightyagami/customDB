import subprocess
import os

def run_db(db_file, commands):
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate("\n".join(commands) + "\n")
    return out.splitlines()

def test_collations():
    db_file = "test_collations.db"
    if os.path.exists(db_file):
        os.remove(db_file)

    # 1. Setup table with default NOCASE collation
    cmds = [
        "create table users (id INT, username VARCHAR(50) collate nocase)",
        "insert into users values (1, 'Alice')",
        "insert into users values (2, 'BOB')",
        "insert into users values (3, 'charlie')",
        ".exit"
    ]
    run_db(db_file, cmds)

    # 2. Case-insensitive WHERE query
    query_cmds = [
        "select * from users where username = 'alice'",
        "select * from users where username = 'bob'",
        ".exit"
    ]
    lines = run_db(db_file, query_cmds)
    out = "\n".join(lines)
    assert "(1, Alice)" in out, f"Case-insensitive match for Alice failed:\n{out}"
    assert "(2, BOB)" in out, f"Case-insensitive match for BOB failed:\n{out}"

    # 3. Query-level COLLATE override in WHERE
    override_cmds = [
        "select * from users where username = 'alice' collate binary",
        ".exit"
    ]
    lines_bin = run_db(db_file, override_cmds)
    out_bin = "\n".join(lines_bin)
    assert "(1, Alice)" not in out_bin, f"Binary collation should NOT match 'alice' with 'Alice':\n{out_bin}"

    # 4. Case-insensitive ORDER BY
    order_cmds = [
        "select * from users order by username collate nocase asc",
        ".exit"
    ]
    lines_ord = run_db(db_file, order_cmds)
    out_ord = "\n".join(lines_ord)
    # Expected order: Alice, BOB, charlie
    alice_pos = out_ord.find("Alice")
    bob_pos = out_ord.find("BOB")
    charlie_pos = out_ord.find("charlie")
    assert alice_pos != -1 and bob_pos != -1 and charlie_pos != -1, f"Missing users in ORDER BY:\n{out_ord}"
    assert alice_pos < bob_pos < charlie_pos, f"NOCASE ORDER BY sorting failed:\n{out_ord}"

    # 5. COLLATE RTRIM testing
    rtrim_cmds = [
        "create table items (id INT, code VARCHAR(50) collate rtrim)",
        "insert into items values (10, 'ITEM1   ')",
        "select * from items where code = 'ITEM1'",
        ".exit"
    ]
    lines_rtrim = run_db(db_file, rtrim_cmds)
    out_rtrim = "\n".join(lines_rtrim)
    assert "(10, ITEM1" in out_rtrim, f"RTRIM collation failed to match trailing whitespace:\n{out_rtrim}"

    if os.path.exists(db_file):
        os.remove(db_file)

    print("✓ Custom Collations (COLLATE NOCASE, RTRIM, BINARY in WHERE, ORDER BY, CREATE TABLE) integration test passed!")

if __name__ == "__main__":
    test_collations()
