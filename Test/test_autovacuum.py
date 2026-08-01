import subprocess
import os

def run_db(db_file, commands):
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate("\n".join(commands) + "\n")
    return out.splitlines()

def test_autovacuum():
    db_file = "test_autovacuum.db"
    if os.path.exists(db_file):
        os.remove(db_file)

    # 1. Enable PRAGMA auto_vacuum = full
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate("PRAGMA auto_vacuum = full\ncreate table logs (id INT, msg VARCHAR(100))\n.exit\n")

    # 2. Create 10 tables to expand database file size
    create_cmds = ["PRAGMA auto_vacuum = full"] + [f"create table t{i} (id INT, val VARCHAR(50))" for i in range(1, 11)]
    create_cmds.append(".exit")
    
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    p.communicate("\n".join(create_cmds) + "\n")

    size_before = os.path.getsize(db_file)

    # 3. Drop 8 tables with auto_vacuum active
    drop_cmds = ["PRAGMA auto_vacuum = full"]
    for i in range(3, 11):
        drop_cmds.append(f"drop table t{i}")
    drop_cmds.append(".exit")
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out_del, err_del = p.communicate("\n".join(drop_cmds) + "\n")

    size_after = os.path.getsize(db_file)
    assert size_after < size_before, f"Auto-vacuum failed to shrink file: before={size_before}, after={size_after}, out={out_del}"

    # 4. Verify remaining tables intact
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out_verify, _ = p.communicate("insert into t1 values (1, 'test')\nselect * from t1\n.exit\n")
    assert "(1, test)" in out_verify, f"Data integrity check failed after auto-vacuum:\n{out_verify}"

    if os.path.exists(db_file):
        os.remove(db_file)

    print(f"✓ PRAGMA auto_vacuum = full verified successfully (shrank from {size_before} to {size_after} bytes)!")

if __name__ == "__main__":
    test_autovacuum()
