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

if __name__ == "__main__":
    unittest.main()
