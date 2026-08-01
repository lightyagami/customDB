import subprocess
import os

def run_db(db_file, commands):
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate("\n".join(commands) + "\n")
    return [line.replace("db >", "").strip() for line in out.splitlines() if line.replace("db >", "").strip()]

def test_range_queries_and_indexes():
    db_file = "test_features.db"
    if os.path.exists(db_file):
        os.remove(db_file)

    # 1. Create table & insert data
    commands = [
        "create table products (id INT, price DOUBLE, name VARCHAR(50))",
        "insert into products values (1, 10.5, 'Pen')",
        "insert into products values (2, 99.99, 'Laptop')",
        "insert into products values (3, 25.0, 'Book')",
        "insert into products values (4, 5.0, 'Pencil')",
        "insert into products values (5, 50.0, 'Backpack')",
        "create index idx_price on products (price)",
        ".schema products"
    ]
    lines = run_db(db_file, commands)
    assert any("[INDEX root=" in l for l in lines), "Failed to find INDEX in schema output!"
    print("✓ Secondary index creation verified in schema")

    # 2. Test primary key range queries
    pk_range = [
        "select * from products where id >= 3",
        "select * from products where id < 3",
        ".exit"
    ]
    lines_pk = run_db(db_file, pk_range)
    assert any("(3, 25, Book)" in l for l in lines_pk), "PK range >= 3 missing Book!"
    assert any("(5, 50, Backpack)" in l for l in lines_pk), "PK range >= 3 missing Backpack!"
    assert any("(1, 10.5, Pen)" in l for l in lines_pk), "PK range < 3 missing Pen!"
    assert not any("(3, 25, Book)" in l for l in lines_pk if "Pencil" in l), "PK range error!"
    print("✓ Primary key range queries (>= and <) verified successfully")

    # 3. Test secondary index range queries (>, <=, =)
    idx_range = [
        "select * from products where price > 20.0",
        "select * from products where price <= 25.0",
        "select * from products where price = 99.99",
        ".exit"
    ]
    lines_idx = run_db(db_file, idx_range)
    assert any("(3, 25, Book)" in l for l in lines_idx), "Index range missing Book!"
    assert any("(5, 50, Backpack)" in l for l in lines_idx), "Index range missing Backpack!"
    assert any("(2, 99.99, Laptop)" in l for l in lines_idx), "Index point query missing Laptop!"
    print("✓ Secondary index range queries (>, <=, =) verified successfully")

    # 4. Test index synchronization on UPDATE and DELETE
    sync_cmds = [
        "update products set price = 100.0 where id = 4", # Pencil price 5 -> 100
        "select * from products where price > 80.0",
        "delete from products where id = 2",              # Delete Laptop
        "select * from products where price > 80.0",
        ".exit"
    ]
    lines_sync = run_db(db_file, sync_cmds)
    assert any("(4, 100, Pencil)" in l for l in lines_sync), "Updated index entry for Pencil missing!"
    assert any("(2, 99.99, Laptop)" in l for l in lines_sync), "Laptop present before delete!"
    
    # Count occurrences of Pencil and Laptop in output
    pencil_count = sum(1 for l in lines_sync if "Pencil" in l)
    laptop_count = sum(1 for l in lines_sync if "Laptop" in l)

    assert pencil_count == 2, f"Pencil should appear 2 times (before and after delete), got {pencil_count}!"
    assert laptop_count == 1, f"Laptop should appear 1 time (before delete only), got {laptop_count}!"
    print("✓ Index synchronization on UPDATE and DELETE verified successfully")

    # 5. Test ORDER BY and LIMIT queries
    sort_cmds = [
        "select * from products order by price asc",
        "select * from products order by price desc limit 2",
        ".exit"
    ]
    lines_sort = run_db(db_file, sort_cmds)
    # We want to check exact order in output lines
    # Remaining rows are: Pen (10.5), Book (25.0), Backpack (50.0), Pencil (100.0)
    # Filter only lines that look like tuples, stripping the prompt if present
    tuples = []
    for l in lines_sort:
        s = l.strip()
        if "db >" in s:
            s = s.replace("db >", "").strip()
        if s.startswith("("):
            tuples.append(s)
    
    # First 4 tuples are for ASC sort
    assert tuples[0] == "(1, 10.5, Pen)", f"Expected Pen first, got {tuples[0]}!"
    assert tuples[1] == "(3, 25, Book)", f"Expected Book second, got {tuples[1]}!"
    assert tuples[2] == "(5, 50, Backpack)", f"Expected Backpack third, got {tuples[2]}!"
    assert tuples[3] == "(4, 100, Pencil)", f"Expected Pencil fourth, got {tuples[3]}!"
    print("✓ ORDER BY price ASC verified successfully")
    
    # Next 2 tuples are for DESC sort with LIMIT 2
    assert tuples[4] == "(4, 100, Pencil)", f"Expected Pencil first in DESC, got {tuples[4]}!"
    assert tuples[5] == "(5, 50, Backpack)", f"Expected Backpack second in DESC, got {tuples[5]}!"
    assert len(tuples) == 6, f"Expected exactly 6 output tuples, got {len(tuples)}!"
    print("✓ ORDER BY price DESC LIMIT 2 verified successfully")

    # Clean up
    if os.path.exists(db_file):
        os.remove(db_file)
        
    print("All range query, index, sorting, and limit integration tests passed!")

if __name__ == "__main__":
    test_range_queries_and_indexes()
