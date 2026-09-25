import os
import time
import socket
import threading
import subprocess
import pytest

def clean_files(prefix):
    for ext in ["", "-journal", "-wal"]:
        f = f"{prefix}{ext}"
        if os.path.exists(f):
            try:
                os.remove(f)
            except OSError:
                pass

def test_multiclient_server_exact_row_counts():
    """
    Stress Test 1: Multiple concurrent client connections to ./db_server hammering writes
    into the same table. Asserts exact row count matches total successful inserts.
    """
    db_file = "test_stress_server.db"
    port = 9931
    clean_files(db_file)

    server_proc = subprocess.Popen(["./db_server", str(port), db_file], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    time.sleep(0.3)

    num_clients = 6
    rows_per_client = 25
    expected_total = num_clients * rows_per_client

    try:
        # 1. Create shared table
        s_init = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s_init.connect(("127.0.0.1", port))
        s_init.recv(1024)
        s_init.sendall(b"CREATE TABLE stress_users (id INT, client_id INT, val TEXT);\n")
        buf = ""
        while "Executed." not in buf:
            buf += s_init.recv(1024).decode('utf-8')
        s_init.close()

        successful_inserts = [0] * num_clients

        def client_worker(cid):
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.connect(("127.0.0.1", port))
            s.recv(1024)
            for r in range(rows_per_client):
                row_id = cid * 1000 + r + 1
                cmd = f"INSERT INTO stress_users VALUES ({row_id}, {cid}, 'Client_{cid}_Row_{r}');\n"
                s.sendall(cmd.encode('utf-8'))
                resp = ""
                while "Executed." not in resp and "Error:" not in resp:
                    chunk = s.recv(1024).decode('utf-8')
                    if not chunk:
                        break
                    resp += chunk
                if "Executed." in resp:
                    successful_inserts[cid] += 1
            s.close()

        threads = [threading.Thread(target=client_worker, args=(i,)) for i in range(num_clients)]
        start_t = time.time()
        for th in threads:
            th.start()
        for th in threads:
            th.join()
        elapsed = time.time() - start_t

        total_client_success = sum(successful_inserts)
        print(f"\n[Test 1] Multi-client server: {total_client_success}/{expected_total} writes in {elapsed:.3f}s")

        # Verify exact row count via CLI (executor run_aggregate_select)
        p_check = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        out, _ = p_check.communicate("SELECT COUNT(id) FROM stress_users;\n.exit\n")

        assert f"({total_client_success})" in out, f"Mismatch in server row count! Expected ({total_client_success}), Got: {out}"
        assert total_client_success == expected_total, f"Some client inserts failed! Successful: {successful_inserts}"

    finally:
        server_proc.terminate()
        server_proc.wait(timeout=2)
        clean_files(db_file)


def test_multiprocess_cli_exact_row_counts():
    """
    Stress Test 2: Multiple independent CLI (./db) OS processes hammering writes against the same file.
    Tests POSIX fcntl RESERVED lock contention, retry/backoff, and asserts exact final row count.
    """
    db_file = "test_stress_cli.db"
    clean_files(db_file)

    # Initialize table
    p_init = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    p_init.communicate("CREATE TABLE proc_stress (id INT, proc_id INT, payload TEXT);\n.exit\n")

    num_procs = 4
    rows_per_proc = 15

    def proc_worker(pid, res_queue):
        success_count = 0
        for r in range(rows_per_proc):
            row_id = pid * 1000 + r + 1
            retries = 30
            while retries > 0:
                p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                out, err = p.communicate(f"INSERT INTO proc_stress VALUES ({row_id}, {pid}, 'Payload_{pid}_{r}');\n.exit\n")
                if "Executed." in out:
                    success_count += 1
                    break
                elif "Database is locked" in out or "Database is locked" in err:
                    retries -= 1
                    time.sleep(0.03)
                else:
                    break
        res_queue.append(success_count)

    res_list = []
    threads = [threading.Thread(target=proc_worker, args=(i, res_list)) for i in range(num_procs)]
    start_t = time.time()
    for th in threads:
        th.start()
    for th in threads:
        th.join()
    elapsed = time.time() - start_t

    total_proc_success = sum(res_list)
    print(f"\n[Test 2] Multi-process CLI: {total_proc_success}/{num_procs * rows_per_proc} writes in {elapsed:.3f}s")

    # Verify exact row count via CLI
    p_check = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out_c, _ = p_check.communicate("SELECT COUNT(id) FROM proc_stress;\n.exit\n")
    clean_files(db_file)

    assert f"({total_proc_success})" in out_c, f"CLI row count mismatch! Expected ({total_proc_success}), Got:\n{out_c}"
    assert total_proc_success == num_procs * rows_per_proc, f"Some process writes failed permanently: {res_list}"


def test_rollback_mode_reader_blocks_writer_timeout():
    """
    Stress Test 3: Explicitly verifies that in default rollback-journal mode,
    a sustained reader holding a SHARED lock blocks a writer's commit until timeout.
    """
    db_file = "test_stress_rollback.db"
    clean_files(db_file)

    p_init = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    p_init.communicate("CREATE TABLE test_rb (id INT, val TEXT);\nINSERT INTO test_rb VALUES (1, 'Initial');\n.exit\n")

    # 1. Start a long-running reader in Process A (holds SHARED lock)
    p_reader = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    p_reader.stdin.write("BEGIN;\n")
    p_reader.stdin.write("SELECT * FROM test_rb;\n")
    p_reader.stdin.flush()
    time.sleep(0.2)

    # 2. Process B attempts a write and commit while Process A holds SHARED lock.
    # In rollback mode, commit attempts to upgrade to EXCLUSIVE_LOCK and will timeout after 500ms.
    p_writer = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    start_t = time.time()
    out_w, err_w = p_writer.communicate("INSERT INTO test_rb VALUES (2, 'WriterRow');\n.exit\n")
    elapsed = time.time() - start_t

    # Clean up reader
    p_reader.stdin.write("COMMIT;\n.exit\n")
    p_reader.stdin.flush()
    p_reader.communicate()

    clean_files(db_file)
    print(f"\n[Test 3] Rollback reader-blocks-writer: elapsed={elapsed:.3f}s, out='{out_w.strip()}', err='{err_w.strip()}'")

    # Confirms it failed after the 500ms timeout with exit code 1 and honest error
    assert elapsed >= 0.45, f"Writer did not wait for the exclusive lock timeout! Elapsed: {elapsed}"
    assert "Database is locked" in err_w or "Database is locked" in out_w, f"Did not see expected lock error! Got err: {err_w}, out: {out_w}"


def test_cross_boundary_server_and_cli_composition():
    """
    Stress Test 4: Composition of ./db_server and external ./db CLI process concurrently
    writing and reading against the same database file in WAL mode.
    """
    db_file = "test_stress_cross.db"
    port = 9932
    clean_files(db_file)

    p_init = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    p_init.communicate("PRAGMA journal_mode = wal;\nCREATE TABLE cross_tbl (id INT, source TEXT);\n.exit\n")

    server_proc = subprocess.Popen(["./db_server", str(port), db_file], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    time.sleep(0.3)

    try:
        # Background server worker with retry
        server_success = [0]
        def server_writer():
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.connect(("127.0.0.1", port))
            s.recv(1024)
            for i in range(20):
                cmd = f"INSERT INTO cross_tbl VALUES ({100 + i}, 'Server');\n"
                retries = 30
                while retries > 0:
                    s.sendall(cmd.encode('utf-8'))
                    buf = ""
                    while "Executed." not in buf and "Error:" not in buf:
                        data = s.recv(1024).decode('utf-8')
                        if not data: break
                        buf += data
                    if "Executed." in buf:
                        server_success[0] += 1
                        break
                    elif "Database is locked" in buf:
                        retries -= 1
                        time.sleep(0.03)
                    else:
                        break
                time.sleep(0.01)
            s.close()

        # Concurrent CLI worker with retry
        cli_success = [0]
        def cli_writer():
            for i in range(20):
                retries = 30
                while retries > 0:
                    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                    out, _ = p.communicate(f"INSERT INTO cross_tbl VALUES ({200 + i}, 'CLI');\n.exit\n")
                    if "Executed." in out:
                        cli_success[0] += 1
                        break
                    time.sleep(0.03)
                    retries -= 1

        th_srv = threading.Thread(target=server_writer)
        th_cli = threading.Thread(target=cli_writer)

        start_t = time.time()
        th_srv.start()
        th_cli.start()
        th_srv.join()
        th_cli.join()
        elapsed = time.time() - start_t

        print(f"\n[Test 4] Cross-boundary composition: Server={server_success[0]}/20, CLI={cli_success[0]}/20 in {elapsed:.3f}s")

        # Verify total count via CLI
        p_check = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        out_c, _ = p_check.communicate("SELECT COUNT(id) FROM cross_tbl;\n.exit\n")
        total = server_success[0] + cli_success[0]

        assert f"({total})" in out_c, f"Row count mismatch! Expected ({total}), got:\n{out_c}"
        assert server_success[0] == 20
        assert cli_success[0] == 20

    finally:
        server_proc.terminate()
        server_proc.wait(timeout=2)
        clean_files(db_file)
