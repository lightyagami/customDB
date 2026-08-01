import subprocess
import os

def run_db(db_file, commands):
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate("\n".join(commands) + "\n")
    return out.splitlines()

def test_constraints():
    db_file = "test_constraints.db"
    if os.path.exists(db_file):
        os.remove(db_file)

    init_cmds = [
        "create table users (id INT, name VARCHAR(50) NOT NULL UNIQUE, age INT CHECK (age >= 18))",
        "create table orders (id INT, user_id INT REFERENCES users(id), amount DOUBLE)",
        
        # 1. NOT NULL / UNIQUE / CHECK validation on users
        "insert into users values (1, 'Alice', 25)",
        "insert into users values (2, 'Bob', 16)",                # Should fail CHECK (age >= 18)
        "insert into users values (3, 'Alice', 30)",              # Should fail UNIQUE
        
        # 2. FOREIGN KEY validation on orders
        "insert into orders values (101, 1, 500.0)",              # Should succeed (user 1 exists)
        "insert into orders values (102, 99, 150.0)",             # Should fail FK (user 99 does NOT exist)
        
        # 3. FOREIGN KEY ON DELETE protection
        "delete from users where id = 1",                         # Should fail FK (orders 101 references user 1)
        ".exit"
    ]

    lines = run_db(db_file, init_cmds)
    full_output = "\n".join(lines)

    assert "Error: CHECK constraint failed." in full_output, f"Expected CHECK constraint error, got:\n{full_output}"
    assert "Error: UNIQUE constraint failed." in full_output, f"Expected UNIQUE constraint error, got:\n{full_output}"
    assert "Error: FOREIGN KEY constraint failed." in full_output, f"Expected FK constraint error on insert, got:\n{full_output}"

    if os.path.exists(db_file):
        os.remove(db_file)
    print("✓ SQL Constraints System (NOT NULL, UNIQUE, DEFAULT, CHECK, FOREIGN KEY) integration test passed!")

if __name__ == "__main__":
    test_constraints()
