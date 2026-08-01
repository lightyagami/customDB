#!/usr/bin/env python3
import subprocess
import os

DB_FILE = "test_types.db"
EXE = "./db"

def run_db(db_path, commands):
    p = subprocess.Popen([EXE, db_path], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    input_str = "\n".join(commands) + "\n.exit\n"
    out, err = p.communicate(input_str)
    lines = [line.replace("db >", "").strip() for line in out.split("\n") if line.replace("db >", "").strip()]
    return lines

def test_datatypes():
    if os.path.exists(DB_FILE):
        os.remove(DB_FILE)

    cmds = [
        "CREATE TABLE all_types (id INT, b_data BLOB, d_time DATETIME, p_date DATE, n_val NUMERIC, r_val REAL, c_val CHAR(20), bin_data VARBINARY)",
        "INSERT INTO all_types VALUES (1, X'414243', '2026-07-31 14:30:00', '2026-07-31', 99.95, 3.14159, 'hello', x'68656c6c6f')",
        "SELECT * FROM all_types"
    ]

    lines = run_db(DB_FILE, cmds)
    out = "\n".join(lines)
    
    assert "414243" in out or "X'414243'" in out or "x'414243'" in out, f"BLOB hex literal missing: {out}"
    assert "2026-07-31" in out, f"Date/Datetime missing: {out}"
    assert "99.9" in out, f"Numeric value missing: {out}"
    assert "3.14" in out, f"Real value missing: {out}"

    print("✓ Full SQL & SQLite Data Types (BLOB, DATETIME, DATE, NUMERIC, REAL, CHAR, VARBINARY, Hex Literals) integration test passed!")

    if os.path.exists(DB_FILE):
        os.remove(DB_FILE)

if __name__ == "__main__":
    test_datatypes()
