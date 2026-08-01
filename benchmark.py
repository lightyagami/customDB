#!/usr/bin/env python3
import subprocess
import time
import sqlite3
import os

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.join(SCRIPT_DIR, "db")
CUSTOM_DB_FILE = os.path.join(SCRIPT_DIR, "bench_custom.db")
SQLITE_DB_FILE = os.path.join(SCRIPT_DIR, "bench_sqlite.db")

def run_custom_db(commands):
    p = subprocess.Popen([EXE, CUSTOM_DB_FILE], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    input_str = "\n".join(commands) + "\n.exit\n"
    start = time.perf_counter()
    p.communicate(input_str)
    end = time.perf_counter()
    return end - start

def run_sqlite_db(commands):
    conn = sqlite3.connect(SQLITE_DB_FILE)
    cursor = conn.cursor()
    start = time.perf_counter()
    for cmd in commands:
        try:
            res = cursor.execute(cmd)
            if cmd.startswith("SELECT"):
                res.fetchall()
        except Exception:
            pass
    conn.commit()
    end = time.perf_counter()
    conn.close()
    return end - start

def main():
    if os.path.exists(CUSTOM_DB_FILE): os.remove(CUSTOM_DB_FILE)
    if os.path.exists(SQLITE_DB_FILE): os.remove(SQLITE_DB_FILE)

    print("==================================================================")
    print("      DBMS ENGINE PERFORMANCE BENCHMARK (CUSTOM C vs SQLITE3)     ")
    print("==================================================================\n")

    N = 5000

    # 1. Bulk Insertion within Transaction
    insert_cmds = ["CREATE TABLE users (id INT, name TEXT, age INT, city TEXT)", "BEGIN TRANSACTION;"]
    for i in range(1, N + 1):
        insert_cmds.append(f"INSERT INTO users VALUES ({i}, 'User_{i}', {20 + (i % 50)}, 'City_{i % 10}')")
    insert_cmds.append("COMMIT;")

    t_custom_ins = run_custom_db(insert_cmds)
    t_sqlite_ins = run_sqlite_db(insert_cmds)

    print(f"1. Bulk Insertion ({N} rows in 1 Transaction):")
    print(f"   - Custom C DBMS: {t_custom_ins:.4f}s ({N/t_custom_ins:.0f} rows/sec)")
    print(f"   - SQLite3:       {t_sqlite_ins:.4f}s ({N/t_sqlite_ins:.0f} rows/sec)\n")

    # 2. Secondary B+Tree Index Creation
    idx_cmds = ["CREATE INDEX idx_age ON users (age);"]
    t_custom_idx = run_custom_db(idx_cmds)
    t_sqlite_idx = run_sqlite_db(idx_cmds)

    print(f"2. Secondary Index Creation on {N} rows:")
    print(f"   - Custom C DBMS: {t_custom_idx:.4f}s")
    print(f"   - SQLite3:       {t_sqlite_idx:.4f}s\n")

    # 3. Point Seeks & Range Scans
    scan_cmds = [
        "SELECT * FROM users WHERE age = 35;",
        "SELECT * FROM users WHERE age >= 25 AND age <= 30;",
        "SELECT * FROM users WHERE id = 2500;"
    ]
    t_custom_scan = run_custom_db(scan_cmds)
    t_sqlite_scan = run_sqlite_db(scan_cmds)

    print("3. Point Seeks & Range Scans (Pure Engine Execution Time):")
    print(f"   - Custom C DBMS: {t_custom_scan:.4f}s")
    print(f"   - SQLite3:       {t_sqlite_scan:.4f}s\n")

    # 4. Aggregations & GROUP BY
    agg_cmds = [
        "SELECT city, COUNT(*), AVG(age), SUM(age) FROM users GROUP BY city;"
    ]
    t_custom_agg = run_custom_db(agg_cmds)
    t_sqlite_agg = run_sqlite_db(agg_cmds)

    print("4. Aggregations & GROUP BY (10 Groups):")
    print(f"   - Custom C DBMS: {t_custom_agg:.4f}s")
    print(f"   - SQLite3:       {t_sqlite_agg:.4f}s\n")

    # 5. UPDATE & DELETE Operations
    mod_cmds = [
        "BEGIN TRANSACTION;",
        "UPDATE users SET age = 99 WHERE city = 'City_1';",
        "DELETE FROM users WHERE age = 99;",
        "COMMIT;"
    ]
    t_custom_mod = run_custom_db(mod_cmds)
    t_sqlite_mod = run_sqlite_db(mod_cmds)

    print("5. UPDATE & DELETE Operations (500 rows affected):")
    print(f"   - Custom C DBMS: {t_custom_mod:.4f}s")
    print(f"   - SQLite3:       {t_sqlite_mod:.4f}s\n")

    # Clean up DB files
    if os.path.exists(CUSTOM_DB_FILE): os.remove(CUSTOM_DB_FILE)
    if os.path.exists(SQLITE_DB_FILE): os.remove(SQLITE_DB_FILE)
    print("==================================================================")

if __name__ == "__main__":
    main()
