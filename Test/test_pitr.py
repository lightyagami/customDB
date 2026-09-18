import subprocess
import os
import time
import struct

def run_db(db_file, commands):
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate("\n".join(commands) + "\n")
    return [l.strip() for l in out.splitlines() if l.strip()], [l.strip() for l in err.splitlines() if l.strip()]

def get_tuples(lines):
    tuples = []
    for l in lines:
        s = l
        if "db >" in s:
            s = s.replace("db >", "").strip()
        if s.startswith("("):
            tuples.append(s)
    return tuples

def cleanup(*files):
    for f in files:
        for suffix in ["", "-wal", "-journal", ".tmp", "-wal.tmp"]:
            path = f + suffix
            if os.path.exists(path):
                try:
                    os.remove(path)
                except OSError:
                    pass

def test_pitr_wal_and_restore():
    db_file = "test_pitr_live.db"
    backup_file = "test_pitr_backup.db"
    rec_t1 = "test_pitr_rec_t1.db"
    rec_lsn2 = "test_pitr_rec_lsn2.db"
    rec_all = "test_pitr_rec_all.db"
    rec_fail = "test_pitr_rec_fail.db"

    cleanup(db_file, backup_file, rec_t1, rec_lsn2, rec_all, rec_fail)

    # 1. Initialize table in WAL mode
    lines, _ = run_db(db_file, [
        "create table events (id INT, val VARCHAR(32))",
        "pragma journal_mode = wal",
        ".exit"
    ])

    # Batch 1: ids 1, 2
    run_db(db_file, [
        "pragma journal_mode = wal",
        "insert into events values (1, 'batch1_a')",
        "insert into events values (2, 'batch1_b')",
        ".exit"
    ])

    t1 = int(time.time())
    # Sleep 1.5 seconds so that batch 2 commits in a subsequent second
    time.sleep(1.5)

    # Batch 2: ids 3, 4
    run_db(db_file, [
        "pragma journal_mode = wal",
        "insert into events values (3, 'batch2_a')",
        "insert into events values (4, 'batch2_b')",
        ".exit"
    ])

    # Check WAL file: should have V2 magic
    wal_path = db_file + "-wal"
    assert os.path.exists(wal_path)
    with open(wal_path, "rb") as f:
        magic = struct.unpack("<I", f.read(4))[0]
        assert magic == 0x574C3200, f"Expected WAL_MAGIC 0x574C3200, got 0x{magic:08X}"

        # Inspect frames to find batch 2 LSN
        frame_size = 4 + 4 + 8 + 8 + 4096
        wal_len = os.path.getsize(wal_path)
        num_frames = (wal_len - 4) // frame_size
        assert num_frames >= 2, f"Expected at least 2 frames, got {num_frames}"
        f.seek(4 + (num_frames - 1) * frame_size + 8 + 8) # LSN offset of last frame
        lsn2 = struct.unpack("<Q", f.read(8))[0]

    # Batch 3: ids 5, 6
    run_db(db_file, [
        "pragma journal_mode = wal",
        "insert into events values (5, 'batch3_a')",
        "insert into events values (6, 'batch3_b')",
        ".exit"
    ])

    # 2. BACKUP DATABASE
    lines_bk, _ = run_db(db_file, [
        "pragma journal_mode = wal",
        f"backup database to '{backup_file}'",
        ".exit"
    ])
    assert any("Backup completed to" in l for l in lines_bk), f"Backup failed: {lines_bk}"
    assert os.path.exists(backup_file), "Backup main file missing!"
    assert os.path.exists(backup_file + "-wal"), "Backup WAL file missing!"
    assert os.path.getsize(backup_file + "-wal") == os.path.getsize(wal_path), "Backup WAL size mismatch!"

    # 3. RESTORE UNTIL TIMESTAMP t1 (should only have batch 1: id 1, 2)
    lines_res1, _ = run_db(db_file, [
        f"restore database from '{backup_file}' until timestamp {t1} to '{rec_t1}'",
        ".exit"
    ])
    assert any("Restored to LSN" in l for l in lines_res1), f"Restore t1 failed: {lines_res1}"
    assert os.path.exists(rec_t1), "Restored t1 file missing!"

    lines_q1, _ = run_db(rec_t1, [
        "select * from events order by id asc",
        ".exit"
    ])
    tups1 = get_tuples(lines_q1)
    assert len(tups1) == 2, f"Expected 2 rows in rec_t1, got {len(tups1)}: {tups1}"
    assert tups1 == ["(1, batch1_a)", "(2, batch1_b)"], f"Unexpected tuples in rec_t1: {tups1}"

    # 4. RESTORE UNTIL LSN (should have batch 1 and 2: ids 1, 2, 3, 4)
    lines_res2, _ = run_db(db_file, [
        f"restore database from '{backup_file}' until lsn {lsn2} to '{rec_lsn2}'",
        ".exit"
    ])
    assert any("Restored to LSN" in l for l in lines_res2), f"Restore lsn failed: {lines_res2}"
    assert os.path.exists(rec_lsn2), "Restored lsn file missing!"

    lines_q2, _ = run_db(rec_lsn2, [
        "select * from events order by id asc",
        ".exit"
    ])
    tups2 = get_tuples(lines_q2)
    assert len(tups2) == 4, f"Expected 4 rows in rec_lsn2, got {len(tups2)}: {tups2}"
    assert tups2 == ["(1, batch1_a)", "(2, batch1_b)", "(3, batch2_a)", "(4, batch2_b)"], f"Unexpected tuples: {tups2}"

    # 5. RESTORE ALL (no UNTIL clause — all 6 rows present)
    lines_res3, _ = run_db(db_file, [
        f"restore database from '{backup_file}' to '{rec_all}'",
        ".exit"
    ])
    assert any("Restored to LSN" in l for l in lines_res3), f"Restore all failed: {lines_res3}"
    assert os.path.exists(rec_all), "Restored all file missing!"

    lines_q3, _ = run_db(rec_all, [
        "select * from events order by id asc",
        ".exit"
    ])
    tups3 = get_tuples(lines_q3)
    assert len(tups3) == 6, f"Expected 6 rows in rec_all, got {len(tups3)}: {tups3}"

    # 6. RESTORE OUT OF RANGE (timestamp 1 predates earliest WAL frame)
    lines_fail, _ = run_db(db_file, [
        f"restore database from '{backup_file}' until timestamp 1 to '{rec_fail}'",
        ".exit"
    ])
    assert any("predates the earliest WAL frame" in l for l in lines_fail), f"Expected out of range error, got: {lines_fail}"
    assert not os.path.exists(rec_fail), "Should not create output file on failure!"

    # 7. RESTORE CLOBBER GUARD (attempting to restore to rec_all when rec_all already exists)
    lines_clobber, _ = run_db(db_file, [
        f"restore database from '{backup_file}' to '{rec_all}'",
        ".exit"
    ])
    assert any("already exists" in l for l in lines_clobber), f"Expected already exists error, got: {lines_clobber}"

    # Cleanup
    cleanup(db_file, backup_file, rec_t1, rec_lsn2, rec_all, rec_fail)

def test_pitr_non_wal_backup_restore():
    db_file = "test_pitr_nonwal.db"
    backup_file = "test_pitr_nonwal_bk.db"
    rec_file = "test_pitr_nonwal_rec.db"

    cleanup(db_file, backup_file, rec_file)

    run_db(db_file, [
        "create table items (id INT, name VARCHAR(20))",
        "insert into items values (10, 'alpha')",
        "insert into items values (20, 'beta')",
        f"backup database to '{backup_file}'",
        ".exit"
    ])

    assert os.path.exists(backup_file)
    assert not os.path.exists(backup_file + "-wal")

    lines_res, _ = run_db(db_file, [
        f"restore database from '{backup_file}' to '{rec_file}'",
        ".exit"
    ])
    assert any("Restored" in l for l in lines_res), f"Non-WAL restore failed: {lines_res}"

    lines_q, _ = run_db(rec_file, [
        "select * from items order by id asc",
        ".exit"
    ])
    tups = get_tuples(lines_q)
    assert tups == ["(10, alpha)", "(20, beta)"], f"Unexpected tuples: {tups}"

    cleanup(db_file, backup_file, rec_file)

if __name__ == "__main__":
    test_pitr_wal_and_restore()
    test_pitr_non_wal_backup_restore()
    print("All PITR tests passed!")
