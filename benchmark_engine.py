import os
import time
import sqlite3
import subprocess
def run_aerodb(db_file, sql_commands):
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    inp = "\n".join(sql_commands) + "\n.exit\n"
    t0 = time.perf_counter()
    out, err = p.communicate(inp)
    t1 = time.perf_counter()
    return (t1 - t0) * 1000.0, out
def run_sqlite(db_file, sql_commands):
    conn = sqlite3.connect(db_file)
    cur = conn.cursor()
    t0 = time.perf_counter()
    for sql in sql_commands:
        cur.execute(sql)
    conn.commit()
    t1 = time.perf_counter()
    conn.close()
    return (t1 - t0) * 1000.0
def main():
    print("==========================================================================")
    print("AeroDB vs SQLite3: Comparative Performance Benchmark")
    print("==========================================================================")
    # 1. Bulk Insertion Benchmark (1,000 rows)
    aero_file = "bench_aero.db"
    sqlite_file = "bench_sqlite.db"
    for f in [aero_file, sqlite_file, f"{aero_file}-journal", f"{aero_file}-wal"]:
        if os.path.exists(f): os.remove(f)
    insert_cmds = ["CREATE TABLE users (id INT, name VARCHAR(50), score DOUBLE);"]
    for i in range(1, 1001):
        insert_cmds.append(f"INSERT INTO users VALUES ({i}, 'User_Name_{i}', {i * 1.5});")
    t_aero_insert, _ = run_aerodb(aero_file, insert_cmds)
    t_sqlite_insert = run_sqlite(sqlite_file, insert_cmds)
    # 2. Point Lookup Benchmark (100 random lookups)
    lookup_cmds = [f"SELECT * FROM users WHERE id = {i * 10};" for i in range(1, 101)]
    t_aero_lookup, _ = run_aerodb(aero_file, lookup_cmds)
    
    conn = sqlite3.connect(sqlite_file)
