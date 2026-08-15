import os
import subprocess
import threading
import ctypes
import unittest

class TestBugFixes(unittest.TestCase):
    def setUp(self):
        for f in ["test_bf.db", "test_bf.db-journal", "test_bf.db-wal", "test_bf.db.vac_tmp", ":memory:"]:
            if os.path.exists(f):
                try:
                    os.remove(f)
                except OSError:
                    pass

    def tearDown(self):
        for f in ["test_bf.db", "test_bf.db-journal", "test_bf.db-wal", "test_bf.db.vac_tmp", ":memory:"]:
            if os.path.exists(f):
                try:
                    os.remove(f)
                except OSError:
                    pass

    def run_db(self, db_name, commands):
        p = subprocess.Popen(["./db", db_name], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        inp = "\n".join(commands) + "\n.exit\n"
        out, err = p.communicate(inp)
        return out, err

    def test_duplicate_primary_key_rejected(self):
        cmds = [
            "CREATE TABLE users (id INT, name TEXT);",
            "INSERT INTO users VALUES (1, 'Alice');",
            "INSERT INTO users VALUES (1, 'Bob');",
            "SELECT * FROM users;"
        ]
        out, err = self.run_db("test_bf.db", cmds)
        self.assertIn("Error: Duplicate key", out)
        # Verify only 1 row was inserted
        self.assertIn("(1, Alice)", out)
        self.assertNotIn("(1, Bob)", out)

    def test_escaped_quotes_in_strings(self):
        cmds = [
            "CREATE TABLE authors (id INT, name TEXT);",
            "INSERT INTO authors VALUES (1, 'Flannery O''Connor');",
            "SELECT * FROM authors WHERE id = 1;"
        ]
        out, err = self.run_db("test_bf.db", cmds)
        self.assertIn("Flannery O'Connor", out)

    def test_direct_select_projection_order_by_limit(self):
        cmds = [
            "CREATE TABLE scores (id INT, player TEXT, score INT);",
            "INSERT INTO scores VALUES (1, 'Alice', 50);",
            "INSERT INTO scores VALUES (2, 'Bob', 90);",
            "INSERT INTO scores VALUES (3, 'Charlie', 75);",
            "SELECT player, score FROM scores ORDER BY score DESC LIMIT 2;"
        ]
        out, err = self.run_db("test_bf.db", cmds)
        lines = [line.replace("db > ", "").strip() for line in out.splitlines() if "(" in line]
        self.assertEqual(len(lines), 2)
        self.assertEqual(lines[0], "(Bob, 90)")
        self.assertEqual(lines[1], "(Charlie, 75)")

    def test_rollback_file_truncation(self):
        # Create table with 1 row
        cmds = [
            "CREATE TABLE items (id INT, val TEXT);",
            "INSERT INTO items VALUES (1, 'baseline');"
        ]
        self.run_db("test_bf.db", cmds)
        base_size = os.path.getsize("test_bf.db")

        # Start transaction, insert many rows to grow pages, then rollback
        tx_cmds = [
            "BEGIN TRANSACTION;"
        ]
        for i in range(2, 50):
            tx_cmds.append(f"INSERT INTO items VALUES ({i}, 'large_payload_padding_data_{i}');")
        tx_cmds.append("ROLLBACK;")
        self.run_db("test_bf.db", tx_cmds)

        after_size = os.path.getsize("test_bf.db")
        self.assertEqual(base_size, after_size, f"File size after rollback {after_size} != baseline {base_size}")

    def test_in_memory_vacuum_no_disk_file(self):
        if os.path.exists(":memory:"):
            os.remove(":memory:")
        cmds = [
            "CREATE TABLE mem (id INT, val TEXT);",
            "INSERT INTO mem VALUES (1, 'foo');",
            "VACUUM;"
        ]
        out, err = self.run_db(":memory:", cmds)
        self.assertIn("vacuum completed", out)
        self.assertFalse(os.path.exists(":memory:"), "In-memory vacuum created a file named ':memory:' on disk")

    def test_multithreaded_concurrent_sorters(self):
        # Test concurrent sorting with libdbms.so to ensure no g_desc_flag data races
        lib = ctypes.CDLL("./libdbms.so")
        lib.dbms_open.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
        lib.dbms_open.restype = ctypes.c_int
        lib.dbms_close.argtypes = [ctypes.c_void_p]
        lib.dbms_close.restype = ctypes.c_int
        lib.dbms_exec.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_char_p)]
        lib.dbms_exec.restype = ctypes.c_int

        db_handle = ctypes.c_void_p()
        res = lib.dbms_open(b"test_bf.db", ctypes.byref(db_handle))
        self.assertEqual(res, 0)

        setup_sql = b"CREATE TABLE t_sort (id INT, num INT);"
        lib.dbms_exec(db_handle, setup_sql, None, None, None)
        for i in range(1, 20):
            ins = f"INSERT INTO t_sort VALUES ({i}, {i * 7 % 19});".encode('utf-8')
            lib.dbms_exec(db_handle, ins, None, None, None)

        lib.dbms_close(db_handle)

        errors = []
        def worker(is_desc):
            p = subprocess.Popen(["./db", "test_bf.db"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            order = "DESC" if is_desc else "ASC"
            inp = f"SELECT * FROM t_sort ORDER BY num {order};\n.exit\n"
            out, err = p.communicate(inp)
            nums = []
            for line in out.splitlines():
                if line.startswith("(") and "," in line:
                    parts = line.strip("()\n ").split(",")
                    nums.append(int(parts[1].strip()))
            if is_desc and nums != sorted(nums, reverse=True):
                errors.append(f"DESC sort incorrect: {nums}")
            elif not is_desc and nums != sorted(nums):
                errors.append(f"ASC sort incorrect: {nums}")

        threads = []
        for i in range(10):
            t = threading.Thread(target=worker, args=(i % 2 == 0,))
            threads.append(t)
            t.start()

        for t in threads:
            t.join()

        self.assertEqual(len(errors), 0, f"Concurrent sorting produced errors: {errors}")

    def test_prepared_statement_projection_api(self):
        lib = ctypes.CDLL("./libdbms.so")
        lib.dbms_open.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
        lib.dbms_open.restype = ctypes.c_int
        lib.dbms_close.argtypes = [ctypes.c_void_p]
        lib.dbms_close.restype = ctypes.c_int
        lib.dbms_prepare_v2.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_char_p)]
        lib.dbms_prepare_v2.restype = ctypes.c_int
        lib.dbms_step.argtypes = [ctypes.c_void_p]
        lib.dbms_step.restype = ctypes.c_int
        lib.dbms_column_count.argtypes = [ctypes.c_void_p]
        lib.dbms_column_count.restype = ctypes.c_int
        lib.dbms_column_name.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.dbms_column_name.restype = ctypes.c_char_p
        lib.dbms_column_text.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.dbms_column_text.restype = ctypes.c_char_p
        lib.dbms_column_int.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.dbms_column_int.restype = ctypes.c_int
        lib.dbms_finalize.argtypes = [ctypes.c_void_p]
        lib.dbms_finalize.restype = ctypes.c_int
        lib.dbms_exec.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_char_p)]
        lib.dbms_exec.restype = ctypes.c_int

        db = ctypes.c_void_p()
        self.assertEqual(lib.dbms_open(b"test_bf.db", ctypes.byref(db)), 0)

        lib.dbms_exec(db, b"CREATE TABLE employees (id INT, name TEXT, salary DOUBLE, dept TEXT);", None, None, None)
        lib.dbms_exec(db, b"INSERT INTO employees VALUES (1, 'Alice', 95000.0, 'Engineering');", None, None, None)

        stmt = ctypes.c_void_p()
        sql = b"SELECT dept, name FROM employees WHERE id = 1;"
        self.assertEqual(lib.dbms_prepare_v2(db, sql, len(sql), ctypes.byref(stmt), None), 0)

        self.assertEqual(lib.dbms_column_count(stmt), 2)
        self.assertEqual(lib.dbms_column_name(stmt, 0).decode('utf-8'), "dept")
        self.assertEqual(lib.dbms_column_name(stmt, 1).decode('utf-8'), "name")

        self.assertEqual(lib.dbms_step(stmt), 100) # DBMS_ROW

        self.assertEqual(lib.dbms_column_text(stmt, 0).decode('utf-8'), "Engineering")
        self.assertEqual(lib.dbms_column_text(stmt, 1).decode('utf-8'), "Alice")

        self.assertEqual(lib.dbms_finalize(stmt), 0)
        self.assertEqual(lib.dbms_close(db), 0)

if __name__ == "__main__":
    unittest.main()
