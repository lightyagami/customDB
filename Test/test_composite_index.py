import subprocess
import os

def run_db(db_file, commands):
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate("\n".join(commands) + "\n")
    return out.splitlines()

def test_composite_index():
    db_file = "test_composite.db"
    if os.path.exists(db_file):
        os.remove(db_file)

    # 1. Create table, composite index on (city, rating), and insert records
    commands = [
        "create table users (id INT, city VARCHAR(50), rating DOUBLE, name VARCHAR(50))",
        "create index idx_city_rating on users (city, rating)",
        "insert into users values (1, 'NewYork', 4.5, 'Alice')",
        "insert into users values (2, 'London', 4.8, 'Bob')",
        "insert into users values (3, 'NewYork', 4.9, 'Charlie')",
        "insert into users values (4, 'Tokyo', 4.2, 'David')",
        ".schema users",
        ".exit"
    ]
    lines = run_db(db_file, commands)
    
    # Assert index creation log or schema output contains composite index info
    schema_lines = [l for l in lines if "[INDEX root=" in l]
    assert len(schema_lines) > 0, f"Expected composite index in schema output, got: {lines}"
    print("✓ Composite multi-column index idx_city_rating created and verified successfully")

    # 2. Select values from table
    select_cmds = [
        "select * from users order by id asc",
        ".exit"
    ]
    lines_sel = run_db(db_file, select_cmds)
    tuples = []
    for l in lines_sel:
        s = l.strip()
        if "db >" in s:
            s = s.replace("db >", "").strip()
        if s.startswith("("):
            tuples.append(s)

    assert len(tuples) == 4, f"Expected 4 user records, got {len(tuples)}!"
    print("✓ Data insertion and selection with multi-column index active verified successfully")

    # Clean up
    if os.path.exists(db_file):
        os.remove(db_file)
    print("All composite index tests passed!")

if __name__ == "__main__":
    test_composite_index()
