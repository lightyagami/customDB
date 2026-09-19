import os
import socket
import time
import subprocess
import unittest

class TestNetworkServer(unittest.TestCase):
    def setUp(self):
        for f in ["test_srv.db", "test_srv.db-journal", "test_srv.db-wal"]:
            if os.path.exists(f):
                try: os.remove(f)
                except OSError: pass

    def tearDown(self):
        for f in ["test_srv.db", "test_srv.db-journal", "test_srv.db-wal"]:
            if os.path.exists(f):
                try: os.remove(f)
                except OSError: pass

    def test_tcp_server_client_interaction(self):
        port = 9876
        # Start db_server process
        server_proc = subprocess.Popen(["./db_server", str(port), "test_srv.db"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        time.sleep(0.3)

        def recv_response(sock):
            buf = ""
            while "Executed." not in buf and "Error:" not in buf:
                chunk = sock.recv(1024).decode('utf-8')
                if not chunk: break
                buf += chunk
            return buf

        try:
            # Connect over TCP
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.connect(("127.0.0.1", port))

            # Read welcome banner
            welcome = s.recv(1024).decode('utf-8')
            self.assertIn("DBMS Network Server", welcome)

            # Send PING
            s.sendall(b"PING\n")
            pong = s.recv(1024).decode('utf-8')
            self.assertIn("PONG", pong)

            # Create table
            s.sendall(b"CREATE TABLE remote_users (id INT, name TEXT, role TEXT);\n")
            res1 = recv_response(s)
            self.assertIn("Executed.", res1)

            # Multi-row Insert
            s.sendall(b"INSERT INTO remote_users VALUES (1, 'Admin', 'Root'), (2, 'Dev', 'Engineer');\n")
            res2 = recv_response(s)
            self.assertIn("Executed.", res2)

            # Query with DISTINCT / WHERE
            s.sendall(b"SELECT id, name, role FROM remote_users ORDER BY id ASC;\n")
            res3 = recv_response(s)
            self.assertIn("(1, Admin, Root)", res3)
            self.assertIn("(2, Dev, Engineer)", res3)
            self.assertIn("Executed.", res3)

            # Disconnect
            s.sendall(b".exit\n")
            s.close()

        finally:
            server_proc.terminate()
            server_proc.wait(timeout=2)

    def test_wal_server_socket_crud(self):
        db_file = "test_srv_wal.db"
        for f in [db_file, db_file + "-journal", db_file + "-wal", db_file + "-shm"]:
            if os.path.exists(f):
                try: os.remove(f)
                except OSError: pass

        # Create table via CLI in WAL mode
        cli = subprocess.run(["./db", db_file],
                             input="PRAGMA journal_mode = WAL;\nCREATE TABLE t (id INT, val TEXT);\n.exit\n",
                             text=True, capture_output=True)
        self.assertEqual(cli.returncode, 0)

        port = 9877
        server_proc = subprocess.Popen(["./db_server", str(port), db_file],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        time.sleep(0.3)

        def recv_response(sock):
            buf = ""
            while "Executed." not in buf and "Error:" not in buf:
                chunk = sock.recv(1024).decode('utf-8')
                if not chunk: break
                buf += chunk
            return buf

        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.connect(("127.0.0.1", port))
            welcome = s.recv(1024).decode('utf-8')
            self.assertIn("DBMS Network Server", welcome)

            s.sendall(b"INSERT INTO t VALUES (1, 'X');\n")
            res1 = recv_response(s)
            self.assertIn("Executed.", res1)

            s.sendall(b"SELECT * FROM t;\n")
            res2 = recv_response(s)
            self.assertIn("(1, X)", res2)
            self.assertIn("Executed.", res2)

            s.sendall(b".exit\n")
            s.close()
        finally:
            server_proc.terminate()
            server_proc.wait(timeout=2)
            for f in [db_file, db_file + "-journal", db_file + "-wal", db_file + "-shm"]:
                if os.path.exists(f):
                    try: os.remove(f)
                    except OSError: pass

    def test_wal_server_interleaved_transactions(self):
        db_file = "test_srv_interleaved.db"
        for f in [db_file, db_file + "-journal", db_file + "-wal", db_file + "-shm"]:
            if os.path.exists(f):
                try: os.remove(f)
                except OSError: pass

        # Create table via CLI in WAL mode
        cli = subprocess.run(["./db", db_file],
                             input="PRAGMA journal_mode = WAL;\nCREATE TABLE kv (k INT, v TEXT);\n.exit\n",
                             text=True, capture_output=True)
        self.assertEqual(cli.returncode, 0)

        port = 9878
        server_proc = subprocess.Popen(["./db_server", str(port), db_file],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        time.sleep(0.3)

        def recv_response(sock):
            buf = ""
            while "Executed." not in buf and "Error:" not in buf:
                chunk = sock.recv(1024).decode('utf-8')
                if not chunk: break
                buf += chunk
            return buf

        try:
            # Client A
            sa = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sa.connect(("127.0.0.1", port))
            sa.recv(1024)

            # Client B
            sb = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sb.connect(("127.0.0.1", port))
            sb.recv(1024)

            sa.sendall(b"BEGIN;\n")
            self.assertIn("Executed.", recv_response(sa))

            sb.sendall(b"BEGIN;\n")
            self.assertIn("Executed.", recv_response(sb))

            sb.sendall(b"INSERT INTO kv VALUES (2, 'from_B');\n")
            self.assertIn("Executed.", recv_response(sb))

            sb.sendall(b"COMMIT;\n")
            self.assertIn("Executed.", recv_response(sb))

            sa.sendall(b"INSERT INTO kv VALUES (1, 'from_A');\n")
            self.assertIn("Executed.", recv_response(sa))

            sa.sendall(b"COMMIT;\n")
            self.assertIn("Executed.", recv_response(sa))

            # Query results
            sa.sendall(b"SELECT * FROM kv ORDER BY k ASC;\n")
            res = recv_response(sa)
            self.assertIn("(1, from_A)", res)
            self.assertIn("(2, from_B)", res)

            sa.sendall(b".exit\n")
            sa.close()
            sb.sendall(b".exit\n")
            sb.close()
        finally:
            server_proc.terminate()
            server_proc.wait(timeout=2)
            for f in [db_file, db_file + "-journal", db_file + "-wal", db_file + "-shm"]:
                if os.path.exists(f):
                    try: os.remove(f)
                    except OSError: pass

    def test_wal_server_overlapping_transactions(self):
        """Verify that when connection B attempts an explicit transaction while connection A
        holds an active write lock, B receives an honest 'Database is locked' error rather than
        silently dropping writes or reporting false durability."""
        db_file = "test_srv_overlapping.db"
        for f in [db_file, db_file + "-journal", db_file + "-wal", db_file + "-shm"]:
            if os.path.exists(f):
                try: os.remove(f)
                except OSError: pass

        # Create table via CLI in WAL mode
        cli = subprocess.run(["./db", db_file],
                             input="PRAGMA journal_mode = WAL;\nCREATE TABLE t (id INT, val TEXT);\n.exit\n",
                             text=True, capture_output=True)
        self.assertEqual(cli.returncode, 0)

        port = 9880
        server_proc = subprocess.Popen(["./db_server", str(port), db_file],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        time.sleep(0.3)

        def recv_response(sock):
            buf = ""
            while "Executed." not in buf and "Error:" not in buf:
                chunk = sock.recv(1024).decode('utf-8')
                if not chunk: break
                buf += chunk
            return buf

        try:
            # Client A
            sa = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sa.connect(("127.0.0.1", port))
            sa.recv(1024)

            # Client B
            sb = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sb.connect(("127.0.0.1", port))
            sb.recv(1024)

            # A opens transaction and starts writing
            sa.sendall(b"BEGIN;\n")
            self.assertIn("Executed.", recv_response(sa))

            sa.sendall(b"INSERT INTO t VALUES (1, 'A1');\n")
            self.assertIn("Executed.", recv_response(sa))

            # B attempts BEGIN while A's write transaction is active -> must be rejected
            sb.sendall(b"BEGIN;\n")
            res_b_begin = recv_response(sb)
            self.assertIn("Error: Database is locked", res_b_begin)
            self.assertNotIn("Executed.", res_b_begin)

            # B attempts INSERT while A's write transaction is active -> must be rejected
            sb.sendall(b"INSERT INTO t VALUES (3, 'B1');\n")
            res_b_ins = recv_response(sb)
            self.assertIn("Error: Database is locked", res_b_ins)
            self.assertNotIn("Executed.", res_b_ins)

            # A finishes its write transaction
            sa.sendall(b"INSERT INTO t VALUES (2, 'A2');\n")
            self.assertIn("Executed.", recv_response(sa))

            sa.sendall(b"COMMIT;\n")
            self.assertIn("Executed.", recv_response(sa))

            # Verify A's rows are intact
            sa.sendall(b"SELECT * FROM t ORDER BY id ASC;\n")
            res_a = recv_response(sa)
            self.assertIn("(1, A1)", res_a)
            self.assertIn("(2, A2)", res_a)
            self.assertNotIn("(3, B1)", res_a)

            # Now that A has committed, B can open a transaction and write successfully
            sb.sendall(b"BEGIN;\n")
            self.assertIn("Executed.", recv_response(sb))

            sb.sendall(b"INSERT INTO t VALUES (3, 'B1');\n")
            self.assertIn("Executed.", recv_response(sb))

            sb.sendall(b"COMMIT;\n")
            self.assertIn("Executed.", recv_response(sb))

            # Both A and B now see all 3 rows
            sb.sendall(b"SELECT * FROM t ORDER BY id ASC;\n")
            res_final = recv_response(sb)
            self.assertIn("(1, A1)", res_final)
            self.assertIn("(2, A2)", res_final)
            self.assertIn("(3, B1)", res_final)

            sa.sendall(b".exit\n")
            sa.close()
            sb.sendall(b".exit\n")
            sb.close()
        finally:
            server_proc.terminate()
            server_proc.wait(timeout=2)
            for f in [db_file, db_file + "-journal", db_file + "-wal", db_file + "-shm"]:
                if os.path.exists(f):
                    try: os.remove(f)
                    except OSError: pass

    def test_server_error_reporting(self):
        db_file = "test_srv_err.db"
        for f in [db_file, db_file + "-journal", db_file + "-wal", db_file + "-shm"]:
            if os.path.exists(f):
                try: os.remove(f)
                except OSError: pass

        port = 9879
        server_proc = subprocess.Popen(["./db_server", str(port), db_file],
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        time.sleep(0.3)

        def recv_response(sock):
            buf = ""
            while "Executed." not in buf and "Error:" not in buf:
                chunk = sock.recv(1024).decode('utf-8')
                if not chunk: break
                buf += chunk
            return buf

        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.connect(("127.0.0.1", port))
            s.recv(1024)

            s.sendall(b"SELECT * FROM no_such_table;\n")
            res = recv_response(s)
            self.assertIn("Error:", res)
            self.assertNotIn("Executed.", res)

            s.sendall(b"INSERT INTO no_such_table VALUES (1);\n")
            res2 = recv_response(s)
            self.assertIn("Error:", res2)
            self.assertNotIn("Executed.", res2)

            s.sendall(b".exit\n")
            s.close()
        finally:
            server_proc.terminate()
            server_proc.wait(timeout=2)
            for f in [db_file, db_file + "-journal", db_file + "-wal", db_file + "-shm"]:
                if os.path.exists(f):
                    try: os.remove(f)
                    except OSError: pass

if __name__ == "__main__":
    unittest.main()
