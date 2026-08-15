import os
import time
import socket
import threading
import subprocess
import unittest

class TestHighConcurrencyWrites(unittest.TestCase):
    def setUp(self):
        for f in ["test_hconc.db", "test_hconc.db-journal", "test_hconc.db-wal"]:
            if os.path.exists(f):
                try: os.remove(f)
                except OSError: pass

    def tearDown(self):
        for f in ["test_hconc.db", "test_hconc.db-journal", "test_hconc.db-wal"]:
            if os.path.exists(f):
                try: os.remove(f)
                except OSError: pass

    def test_parallel_writes_across_distinct_tables(self):
        port = 9922
        db_file = "test_hconc.db"
        server_proc = subprocess.Popen(["./db_server", str(port), db_file], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        time.sleep(0.3)

        num_threads = 6
        rows_per_thread = 20

        try:
            # 1. Initialize tables for each thread
            s_init = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s_init.connect(("127.0.0.1", port))
            s_init.recv(1024)
            for t in range(num_threads):
                s_init.sendall(f"CREATE TABLE worker_tbl_{t} (id INT, val TEXT);\n".encode('utf-8'))
                buf = ""
                while "Executed." not in buf:
                    buf += s_init.recv(1024).decode('utf-8')
            s_init.close()

            # 2. Launch concurrent writer threads
            results = []
            def writer_worker(thread_id, res_list):
                try:
                    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                    s.connect(("127.0.0.1", port))
                    s.recv(1024)
                    
                    for r in range(rows_per_thread):
                        cmd = f"INSERT INTO worker_tbl_{thread_id} VALUES ({r + 1}, 'Thread_{thread_id}_Item_{r + 1}');\n"
                        s.sendall(cmd.encode('utf-8'))
                        buf = ""
                        while "Executed." not in buf and "Error:" not in buf:
                            data = s.recv(1024).decode('utf-8')
                            if not data: break
                            buf += data
                    
                    # Verify count
                    s.sendall(f"SELECT COUNT(id) FROM worker_tbl_{thread_id};\n".encode('utf-8'))
                    buf = ""
                    while "Executed." not in buf and "Error:" not in buf:
                        data = s.recv(1024).decode('utf-8')
                        if not data: break
                        buf += data
                    
                    if f"({rows_per_thread})" in buf:
                        res_list.append(True)
                    else:
                        res_list.append(False)
                    s.close()
                except Exception as e:
                    res_list.append(False)

            threads = []
            for t in range(num_threads):
                th = threading.Thread(target=writer_worker, args=(t, results))
                threads.append(th)
                th.start()

            for th in threads:
                th.join()

            self.assertEqual(len(results), num_threads)
            self.assertTrue(all(results), f"High concurrency write failures: {results}")

        finally:
            server_proc.terminate()
            server_proc.wait(timeout=2)

if __name__ == "__main__":
    unittest.main()
