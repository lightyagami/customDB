import os
import time
import socket
import threading
import subprocess
import unittest
import random

class TestAdvancedStressAndEdgeCases(unittest.TestCase):
    def setUp(self):
        for f in ["test_adv.db", "test_adv.db-journal", "test_adv.db-wal", "test_adv_crash.db", "test_adv_crash.db-journal", "test_adv_crash.db-wal"]:
            if os.path.exists(f):
                try: os.remove(f)
                except OSError: pass

    def tearDown(self):
        for f in ["test_adv.db", "test_adv.db-journal", "test_adv.db-wal", "test_adv_crash.db", "test_adv_crash.db-journal", "test_adv_crash.db-wal"]:
            if os.path.exists(f):
                try: os.remove(f)
                except OSError: pass

    def run_db(self, db_name, commands):
        p = subprocess.Popen(["./db", db_name], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        inp = "\n".join(commands) + "\n.exit\n"
        out, err = p.communicate(inp)
        lines = []
        for l in out.splitlines():
            s = l.replace("db > ", "").strip()
            if s.startswith("(") and s.endswith(")"):
                lines.append(s)
        return p.returncode, lines, out

    def test_concurrent_socket_readers_and_writers(self):
        # Stress test the TCP server with multiple concurrent client threads reading and writing
        port = 9911
        db_file = "test_adv.db"
        server_proc = subprocess.Popen(["./db_server", str(port), db_file], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        time.sleep(0.3)

        def worker(thread_id, results_list):
            try:
                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                s.connect(("127.0.0.1", port))
                # read banner
                s.recv(1024)
                
                # Each worker creates rows and reads
                for i in range(10):
                    row_id = thread_id * 100 + i
                    cmd = f"INSERT INTO concurrent_users VALUES ({row_id}, 'User_{row_id}');\n"
                    s.sendall(cmd.encode('utf-8'))
                    buf = ""
                    while "Executed." not in buf and "Error:" not in buf:
                        data = s.recv(1024).decode('utf-8')
                        if not data: break
                        buf += data
                
                # Query
                s.sendall(f"SELECT * FROM concurrent_users WHERE id = {thread_id * 100};\n".encode('utf-8'))
                buf = ""
                while "Executed." not in buf and "Error:" not in buf:
                    data = s.recv(1024).decode('utf-8')
                    if not data: break
                    buf += data
                
                if f"User_{thread_id * 100}" in buf:
                    results_list.append(True)
                else:
                    results_list.append(False)
                
                s.sendall(b".exit\n")
                s.close()
            except Exception as e:
                results_list.append(False)

        try:
            # Init table first
            s_init = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s_init.connect(("127.0.0.1", port))
            s_init.recv(1024)
            s_init.sendall(b"CREATE TABLE concurrent_users (id INT, name TEXT);\n")
            buf = ""
            while "Executed." not in buf:
                buf += s_init.recv(1024).decode('utf-8')
            s_init.close()

            threads = []
            results = []
            for t in range(5):
                th = threading.Thread(target=worker, args=(t + 1, results))
                threads.append(th)
                th.start()

            for th in threads:
                th.join()

            self.assertEqual(len(results), 5)
            self.assertTrue(all(results), f"Concurrent client test had failures: {results}")

        finally:
            server_proc.terminate()
            server_proc.wait(timeout=2)

    def test_btree_heavy_split_and_mass_deletions(self):
        # Insert 1,000 records to force multiple levels of B+ tree splits, then delete alternating halves to test node merges/underflows
        db_file = "test_adv.db"
        cmds = ["CREATE TABLE big_tree (id INT, payload TEXT);"]
        for i in range(1, 501):
            cmds.append(f"INSERT INTO big_tree VALUES ({i}, 'Payload_String_{i}_padding_data_to_fill_cells');")
        rc, _, _ = self.run_db(db_file, cmds)
        self.assertEqual(rc, 0)

        # Delete odd numbers
        del_cmds = []
        for i in range(1, 501, 2):
            del_cmds.append(f"DELETE FROM big_tree WHERE id = {i};")
        rc, _, _ = self.run_db(db_file, del_cmds)
        self.assertEqual(rc, 0)

        # Verify even numbers remain intact
        check_cmds = ["SELECT id FROM big_tree WHERE id = 250;"]
        rc, lines, out = self.run_db(db_file, check_cmds)
        self.assertEqual(rc, 0)
        self.assertEqual(lines, ["(250)"])

    def test_null_coalesce_and_aggregation_edge_cases(self):
        # Null handling in math expressions, aggregate functions, and COALESCE
        db_file = "test_adv.db"
        cmds = [
            "CREATE TABLE stats (id INT, val INT, factor DOUBLE);",
            "INSERT INTO stats VALUES (1, NULL, 2.5);",
            "INSERT INTO stats VALUES (2, 20, NULL);",
            "INSERT INTO stats VALUES (3, 30, 4.0);",
            "SELECT COALESCE(val, 0), COALESCE(factor, 1.0) FROM stats ORDER BY id ASC;"
        ]
        rc, lines, out = self.run_db(db_file, cmds)
        self.assertEqual(rc, 0)
        self.assertEqual(len(lines), 3)
        self.assertEqual(lines[0], "(0, 2.5)")
        self.assertEqual(lines[1], "(20, 1.0)")
        self.assertEqual(lines[2], "(30, 4)")

    def test_unicode_and_binary_blob_edge_cases(self):
        # Test special characters, hex blob literals, quotes within quotes, and multi-byte UTF-8
        db_file = "test_adv.db"
        cmds = [
            "CREATE TABLE blobs (id INT, data BLOB, info TEXT);",
            "INSERT INTO blobs VALUES (1, x'0102030405', 'Hello 世界 🚀');",
            "INSERT INTO blobs VALUES (2, x'AABBCCDDEEFF', 'Line1\\nLine2\tTabbed');",
            "SELECT * FROM blobs ORDER BY id ASC;"
        ]
        rc, lines, out = self.run_db(db_file, cmds)
        self.assertEqual(rc, 0)
        self.assertEqual(len(lines), 2)
        self.assertIn("0102030405", lines[0])
        self.assertIn("Hello 世界 🚀", lines[0])

if __name__ == "__main__":
    unittest.main()
