import subprocess
import os

def run_db(db_file, commands):
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate("\n".join(commands) + "\n")
    return out.splitlines()

def test_vacuum():
    db_file = "test_vacuum.db"
    if os.path.exists(db_file):
        os.remove(db_file)

    # 1. Create table and insert records spanning multiple pages
    init_cmds = [
        "create table items (id INT, price DOUBLE, name VARCHAR(100))"
    ]
    for i in range(1, 200):
        init_cmds.append(f"insert into items values ({i}, {i * 1.5}, 'ItemName_{i}_padding_bytes_to_span_pages')")
    init_cmds.append(".exit")

    run_db(db_file, init_cmds)
    size_before_del = os.path.getsize(db_file)
    print(f"Initial DB file size with 100 items: {size_before_del} bytes")

    # 2. Drop table and create small table
    del_cmds = [
        "drop table items",
        "create table items (id INT, price DOUBLE, name VARCHAR(100))",
        "insert into items values (1, 1.5, 'ItemName_1')",
        "insert into items values (2, 3.0, 'ItemName_2')",
        ".exit"
    ]
    run_db(db_file, del_cmds)
    size_after_del = os.path.getsize(db_file)
    print(f"DB file size after dropping table (before VACUUM): {size_after_del} bytes")

    # 3. Run VACUUM
    vac_cmds = [
        "vacuum",
        "select * from items order by id asc",
        ".exit"
    ]
    lines_vac = run_db(db_file, vac_cmds)
    size_after_vac = os.path.getsize(db_file)
    print(f"DB file size after VACUUM: {size_after_vac} bytes")

    # Verify that file size decreased after VACUUM
    assert size_after_vac < size_after_del, f"Expected VACUUM to shrink file size, but size_after_vac={size_after_vac} >= size_after_del={size_after_del}"
    print("✓ VACUUM successfully defragmented storage and reduced file size")

    # Verify remaining data integrity (2 items remaining)
    tuples = []
    for l in lines_vac:
        s = l.strip()
        if "db >" in s:
            s = s.replace("db >", "").strip()
        if s.startswith("("):
            tuples.append(s)

    assert len(tuples) == 2, f"Expected 2 remaining items, got {len(tuples)}!"
    assert tuples[0] == "(1, 1.5, ItemName_1)", f"Expected Item 1, got {tuples[0]}!"
    assert tuples[1] == "(2, 3, ItemName_2)", f"Expected Item 2, got {tuples[1]}!"
    print("✓ Data integrity verified 100% intact after VACUUM")

    # Clean up
    if os.path.exists(db_file):
        os.remove(db_file)
    print("All VACUUM integration tests passed!")

if __name__ == "__main__":
    test_vacuum()
