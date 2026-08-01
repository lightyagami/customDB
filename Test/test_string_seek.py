import subprocess
import os

def run_db(db_file, commands):
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate("\n".join(commands) + "\n")
    return out.splitlines()

def test_string_seek_and_range():
    db_file = "test_string.db"
    if os.path.exists(db_file):
        os.remove(db_file)

    # 1. Create table, insert strings, and create index on name (VARCHAR)
    commands = [
        "create table users (id INT, name VARCHAR(50), city VARCHAR(50))",
        "insert into users values (1, 'Alice', 'NewYork')",
        "insert into users values (2, 'Bob', 'SanFrancisco')",
        "insert into users values (3, 'Charlie', 'London')",
        "insert into users values (4, 'David', 'Tokyo')",
        "insert into users values (5, 'Eve', 'Paris')",
        "create index idx_name on users (name)",
        ".schema users"
    ]
    lines = run_db(db_file, commands)
    assert any("[INDEX root=" in l for l in lines), "Failed to find idx_name in schema output!"
    print("✓ String index idx_name created successfully")

    # 2. Test point seek on index (WHERE name = 'Bob')
    point_cmds = [
        "select * from users where name = 'Bob'",
        ".exit"
    ]
    lines_point = run_db(db_file, point_cmds)
    tuples_point = []
    for l in lines_point:
        s = l.strip()
        if "db >" in s:
            s = s.replace("db >", "").strip()
        if s.startswith("("):
            tuples_point.append(s)

    assert len(tuples_point) == 1, f"Expected 1 record for Bob, got {len(tuples_point)}! Tuples: {tuples_point}"
    assert tuples_point[0] == "(2, Bob, SanFrancisco)", f"Expected Bob tuple, got {tuples_point[0]}!"
    print("✓ String index point seek (WHERE name = 'Bob') verified successfully")

    # 3. Test string range scan (WHERE name >= 'Charlie')
    range_cmds = [
        "select * from users where name >= 'Charlie' order by name asc",
        ".exit"
    ]
    lines_range = run_db(db_file, range_cmds)
    tuples_range = []
    for l in lines_range:
        s = l.strip()
        if "db >" in s:
            s = s.replace("db >", "").strip()
        if s.startswith("("):
            tuples_range.append(s)

    assert len(tuples_range) == 3, f"Expected 3 records for name >= Charlie, got {len(tuples_range)}! Tuples: {tuples_range}"
    assert tuples_range[0] == "(3, Charlie, London)", f"Expected Charlie first, got {tuples_range[0]}!"
    assert tuples_range[1] == "(4, David, Tokyo)", f"Expected David second, got {tuples_range[1]}!"
    assert tuples_range[2] == "(5, Eve, Paris)", f"Expected Eve third, got {tuples_range[2]}!"
    print("✓ String index range scan (WHERE name >= 'Charlie') verified successfully")

    # Clean up
    if os.path.exists(db_file):
        os.remove(db_file)
    print("All text-based index seek and range scan tests passed!")

if __name__ == "__main__":
    test_string_seek_and_range()
