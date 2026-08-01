import subprocess
import os

def run_db(db_file, commands):
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate("\n".join(commands) + "\n")
    return out.splitlines()

def test_generalized_delete_update():
    db_file = "test_del_upd.db"
    if os.path.exists(db_file):
        os.remove(db_file)

    # 1. Initialize table, values, and index
    commands = [
        "create table products (id INT, price DOUBLE, name VARCHAR(50))",
        "insert into products values (1, 10.5, 'Pen')",
        "insert into products values (2, 99.99, 'Laptop')",
        "insert into products values (3, 25.0, 'Book')",
        "insert into products values (4, 5.0, 'Pencil')",
        "insert into products values (5, 50.0, 'Backpack')",
        "create index idx_price on products (price)",
        "select * from products order by price asc",
        ".exit"
    ]
    lines = run_db(db_file, commands)
    tuples = []
    for l in lines:
        s = l.strip()
        if "db >" in s:
            s = s.replace("db >", "").strip()
        if s.startswith("("):
            tuples.append(s)
    assert len(tuples) == 5, f"Expected 5 initial products, got {len(tuples)}! Tuples: {tuples}"

    # 2. Test conditional UPDATE (WHERE name = 'Pen')
    # Update Pen price from 10.5 to 120.0
    update_cmds = [
        "update products set price = 120.0 where name = 'Pen'",
        "select * from products order by price desc",
        ".exit"
    ]
    lines_upd = run_db(db_file, update_cmds)
    tuples_upd = []
    for l in lines_upd:
        s = l.strip()
        if "db >" in s:
            s = s.replace("db >", "").strip()
        if s.startswith("("):
            tuples_upd.append(s)
    # First tuple should now be Pen (120.0) instead of Laptop (99.99)
    assert tuples_upd[0] == "(1, 120, Pen)", f"Expected Pen updated first in DESC, got {tuples_upd[0]}!"
    print("✓ Generalized UPDATE with WHERE clause and secondary index sync verified successfully")

    # 3. Test conditional DELETE (WHERE price > 40.0)
    # This should delete Pen (120.0), Laptop (99.99), and Backpack (50.0).
    # Leaving only Book (25.0) and Pencil (5.0).
    delete_cmds = [
        "delete from products where price > 40.0",
        "select * from products order by price asc",
        ".exit"
    ]
    lines_del = run_db(db_file, delete_cmds)
    tuples_del = []
    for l in lines_del:
        s = l.strip()
        if "db >" in s:
            s = s.replace("db >", "").strip()
        if s.startswith("("):
            tuples_del.append(s)
    assert len(tuples_del) == 2, f"Expected 2 remaining products, got {len(tuples_del)}! Tuples: {tuples_del}"
    assert tuples_del[0] == "(4, 5, Pencil)", f"Expected Pencil first, got {tuples_del[0]}!"
    assert tuples_del[1] == "(3, 25, Book)", f"Expected Book second, got {tuples_del[1]}!"
    print("✓ Generalized DELETE with WHERE clause and secondary index sync verified successfully")

    # 4. Test DELETE all (without WHERE clause)
    del_all_cmds = [
        "delete from products",
        "select * from products",
        ".exit"
    ]
    lines_all = run_db(db_file, del_all_cmds)
    tuples_all = []
    for l in lines_all:
        s = l.strip()
        if "db >" in s:
            s = s.replace("db >", "").strip()
        if s.startswith("("):
            tuples_all.append(s)
    assert len(tuples_all) == 0, f"Expected 0 remaining products after DELETE all, got {len(tuples_all)}!"
    print("✓ DELETE all rows (without WHERE) verified successfully")

    # Clean up
    if os.path.exists(db_file):
        os.remove(db_file)
    print("All generalized delete and update integration tests passed!")

if __name__ == "__main__":
    test_generalized_delete_update()
