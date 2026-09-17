import subprocess
import time
import os
import urllib.request
import urllib.error
import json
import pytest

class TestHttpEndpoint:
    @classmethod
    def setup_class(cls):
        for f in ["test_http.db", "test_http.db-journal", "test_http.db-wal"]:
            if os.path.exists(f):
                try: os.remove(f)
                except OSError: pass
        cls.port = 8999
        cls.server_proc = subprocess.Popen(["./db_server", str(cls.port), "test_http.db"], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        time.sleep(0.4)

    @classmethod
    def teardown_class(cls):
        cls.server_proc.terminate()
        cls.server_proc.wait(timeout=2)
        for f in ["test_http.db", "test_http.db-journal", "test_http.db-wal"]:
            if os.path.exists(f):
                try: os.remove(f)
                except OSError: pass

    def test_get_health(self):
        url = f"http://127.0.0.1:{self.port}/health"
        req = urllib.request.Request(url, method="GET")
        with urllib.request.urlopen(req, timeout=3) as resp:
            assert resp.status == 200
            data = json.loads(resp.read().decode('utf-8'))
            assert data.get("status") == "ok"

    def test_post_query(self):
        url = f"http://127.0.0.1:{self.port}/query"

        # Create table
        create_payload = json.dumps({"sql": "CREATE TABLE items (id INT, name TEXT, price DOUBLE);"}).encode('utf-8')
        req = urllib.request.Request(url, data=create_payload, headers={"Content-Type": "application/json"}, method="POST")
        with urllib.request.urlopen(req, timeout=3) as resp:
            assert resp.status == 200

        # Insert items
        insert_payload = json.dumps({"sql": "INSERT INTO items VALUES (1, 'Widget', 19.99), (2, 'Gadget', 29.50);"}).encode('utf-8')
        req = urllib.request.Request(url, data=insert_payload, headers={"Content-Type": "application/json"}, method="POST")
        with urllib.request.urlopen(req, timeout=3) as resp:
            assert resp.status == 200

        # Select items
        select_payload = json.dumps({"sql": "SELECT id, name, price FROM items ORDER BY id ASC;"}).encode('utf-8')
        req = urllib.request.Request(url, data=select_payload, headers={"Content-Type": "application/json"}, method="POST")
        with urllib.request.urlopen(req, timeout=3) as resp:
            assert resp.status == 200
            data = json.loads(resp.read().decode('utf-8'))
            assert "columns" in data
            assert data["columns"] == ["id", "name", "price"]
            assert len(data["rows"]) == 2
            assert data["rows"][0] == ["1", "Widget", "19.99"]
            assert data["rows"][1] == ["2", "Gadget", "29.5"]

    def test_post_raw_query(self):
        url = f"http://127.0.0.1:{self.port}/query"
        raw_sql = b"SELECT 1 FROM items WHERE id = 1;"
        req = urllib.request.Request(url, data=raw_sql, headers={"Content-Type": "text/plain"}, method="POST")
        with urllib.request.urlopen(req, timeout=3) as resp:
            assert resp.status == 200
            data = json.loads(resp.read().decode('utf-8'))
            assert len(data["rows"]) == 1
