import subprocess
import socket
import time
import os
import pytest

class TestPubSub:
    @classmethod
    def setup_class(cls):
        for f in ["test_pubsub.db", "test_pubsub.db-journal", "test_pubsub.db-wal"]:
            if os.path.exists(f):
                try: os.remove(f)
                except OSError: pass
        cls.port = 9123
        cls.server_proc = subprocess.Popen(["./db_server", str(cls.port), "test_pubsub.db"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        time.sleep(0.4)

    @classmethod
    def teardown_class(cls):
        cls.server_proc.terminate()
        cls.server_proc.wait(timeout=2)
        for f in ["test_pubsub.db", "test_pubsub.db-journal", "test_pubsub.db-wal"]:
            if os.path.exists(f):
                try: os.remove(f)
                except OSError: pass

    def test_listen_notify(self):
        # Client 1: Listener
        s1 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s1.connect(("127.0.0.1", self.port))
        welcome1 = s1.recv(1024).decode('utf-8')
        assert "DBMS Network Server" in welcome1

        # Subscribe to channel 'orders'
        s1.sendall(b"LISTEN orders;\n")
        resp1 = s1.recv(1024).decode('utf-8')
        assert "Executed." in resp1

        # Client 2: Notifier
        s2 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s2.connect(("127.0.0.1", self.port))
        welcome2 = s2.recv(1024).decode('utf-8')
        assert "DBMS Network Server" in welcome2

        # Send NOTIFY orders, 'order #42 created'
        s2.sendall(b"NOTIFY orders, 'order #42 created';\n")
        resp2 = s2.recv(1024).decode('utf-8')
        assert "Executed." in resp2

        # Verify Client 1 receives the notification
        s1.settimeout(3.0)
        notif = s1.recv(1024).decode('utf-8')
        assert "NOTIFY orders order #42 created" in notif

        s1.close()
        s2.close()
