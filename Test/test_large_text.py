import os
import subprocess
import ctypes
import unittest

class TestLargeText(unittest.TestCase):
    DB_FILE = "test_large_text.db"
    LIB_FILE = "./libdbms.so"

    def setUp(self):
        self._cleanup()

    def tearDown(self):
        self._cleanup()

    def _cleanup(self):
        for f in [self.DB_FILE, f"{self.DB_FILE}-journal", f"{self.DB_FILE}-wal"]:
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

    def test_large_text_cli_insert_select_update(self):
        t500 = "A" * 500
        t3000 = "B" * 3000
        t3200 = "C" * 3200

        cmds = [
            "CREATE TABLE articles (id INT, title VARCHAR(4000), body TEXT);",
            f"INSERT INTO articles VALUES (1, '{t500}', '{t3000}');",
            "SELECT * FROM articles WHERE id = 1;",
            f"SELECT * FROM articles WHERE body = '{t3000}';",
            f"UPDATE articles SET body = '{t3200}' WHERE id = 1;",
            "SELECT * FROM articles WHERE id = 1;"
        ]
        out, err = self.run_db(self.DB_FILE, cmds)
        self.assertNotIn("Error", out)
        self.assertIn(f"(1, {t500}, {t3000})", out)
        self.assertIn(f"(1, {t500}, {t3200})", out)

    def test_large_text_capi_binding(self):
        self.assertTrue(os.path.exists(self.LIB_FILE))
        lib = ctypes.CDLL(self.LIB_FILE)

        lib.dbms_open.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
        lib.dbms_open.restype = ctypes.c_int
        lib.dbms_close.argtypes = [ctypes.c_void_p]
        lib.dbms_close.restype = ctypes.c_int
        lib.dbms_prepare_v2.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_char_p)]
        lib.dbms_prepare_v2.restype = ctypes.c_int
        lib.dbms_bind_int.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
        lib.dbms_bind_int.restype = ctypes.c_int
        lib.dbms_bind_text.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
        lib.dbms_bind_text.restype = ctypes.c_int
        lib.dbms_step.argtypes = [ctypes.c_void_p]
        lib.dbms_step.restype = ctypes.c_int
        lib.dbms_column_int.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.dbms_column_int.restype = ctypes.c_int
        lib.dbms_column_text.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.dbms_column_text.restype = ctypes.c_char_p
        lib.dbms_finalize.argtypes = [ctypes.c_void_p]
        lib.dbms_finalize.restype = ctypes.c_int

        db = ctypes.c_void_p()
        self.assertEqual(lib.dbms_open(self.DB_FILE.encode('utf-8'), ctypes.byref(db)), 0)

        # Create table
        stmt = ctypes.c_void_p()
        sql_create = b"CREATE TABLE payload (id INT, data TEXT);"
        self.assertEqual(lib.dbms_prepare_v2(db, sql_create, len(sql_create), ctypes.byref(stmt), None), 0)
        self.assertEqual(lib.dbms_step(stmt), 101) # DBMS_DONE
        lib.dbms_finalize(stmt)

        # Insert 3000-char string via bind_text
        t3000 = ("X" * 3000).encode('utf-8')
        stmt_ins = ctypes.c_void_p()
        sql_ins = b"INSERT INTO payload VALUES (?, ?);"
        self.assertEqual(lib.dbms_prepare_v2(db, sql_ins, len(sql_ins), ctypes.byref(stmt_ins), None), 0)
        self.assertEqual(lib.dbms_bind_int(stmt_ins, 1, 1), 0)
        self.assertEqual(lib.dbms_bind_text(stmt_ins, 2, t3000, len(t3000)), 0)
        self.assertEqual(lib.dbms_step(stmt_ins), 101)
        lib.dbms_finalize(stmt_ins)

        # Read back via column_text
        stmt_sel = ctypes.c_void_p()
        sql_sel = b"SELECT id, data FROM payload WHERE id = 1;"
        self.assertEqual(lib.dbms_prepare_v2(db, sql_sel, len(sql_sel), ctypes.byref(stmt_sel), None), 0)
        self.assertEqual(lib.dbms_step(stmt_sel), 100) # DBMS_ROW

        col0 = lib.dbms_column_int(stmt_sel, 0)
        col1 = lib.dbms_column_text(stmt_sel, 1).decode('utf-8')

        self.assertEqual(col0, 1)
        self.assertEqual(len(col1), 3000)
        self.assertEqual(col1, "X" * 3000)

        lib.dbms_finalize(stmt_sel)
        lib.dbms_close(db)

    def test_multi_overflow_pages_100kb(self):
        lib = ctypes.CDLL(self.LIB_FILE)
        lib.dbms_open.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
        lib.dbms_open.restype = ctypes.c_int
        lib.dbms_close.argtypes = [ctypes.c_void_p]
        lib.dbms_close.restype = ctypes.c_int
        lib.dbms_prepare_v2.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_char_p)]
        lib.dbms_prepare_v2.restype = ctypes.c_int
        lib.dbms_bind_int.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
        lib.dbms_bind_int.restype = ctypes.c_int
        lib.dbms_bind_text.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
        lib.dbms_bind_text.restype = ctypes.c_int
        lib.dbms_step.argtypes = [ctypes.c_void_p]
        lib.dbms_step.restype = ctypes.c_int
        lib.dbms_column_int.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.dbms_column_int.restype = ctypes.c_int
        lib.dbms_column_text.argtypes = [ctypes.c_void_p, ctypes.c_int]
        lib.dbms_column_text.restype = ctypes.c_char_p
        lib.dbms_finalize.argtypes = [ctypes.c_void_p]
        lib.dbms_finalize.restype = ctypes.c_int

        db = ctypes.c_void_p()
        self.assertEqual(lib.dbms_open(self.DB_FILE.encode('utf-8'), ctypes.byref(db)), 0)

        # Create table
        stmt = ctypes.c_void_p()
        sql_create = b"CREATE TABLE big_payload (id INT, data TEXT);"
        self.assertEqual(lib.dbms_prepare_v2(db, sql_create, len(sql_create), ctypes.byref(stmt), None), 0)
        self.assertEqual(lib.dbms_step(stmt), 101)
        lib.dbms_finalize(stmt)

        sizes = [10000, 50000, 100000]
        test_strings = {}
        for i, sz in enumerate(sizes, start=1):
            pattern = (f"Chunk_{i}_" * (sz // 8 + 1))[:sz]
            test_strings[i] = pattern
            stmt_ins = ctypes.c_void_p()
            sql_ins = b"INSERT INTO big_payload VALUES (?, ?);"
            self.assertEqual(lib.dbms_prepare_v2(db, sql_ins, len(sql_ins), ctypes.byref(stmt_ins), None), 0)
            self.assertEqual(lib.dbms_bind_int(stmt_ins, 1, i), 0)
            p_bytes = pattern.encode('utf-8')
            self.assertEqual(lib.dbms_bind_text(stmt_ins, 2, p_bytes, len(p_bytes)), 0)
            self.assertEqual(lib.dbms_step(stmt_ins), 101)
            lib.dbms_finalize(stmt_ins)

        # Verify all retrieved strings match exactly
        for i, expected in test_strings.items():
            stmt_sel = ctypes.c_void_p()
            sql_sel = f"SELECT id, data FROM big_payload WHERE id = {i};".encode('utf-8')
            self.assertEqual(lib.dbms_prepare_v2(db, sql_sel, len(sql_sel), ctypes.byref(stmt_sel), None), 0)
            self.assertEqual(lib.dbms_step(stmt_sel), 100) # DBMS_ROW
            val_id = lib.dbms_column_int(stmt_sel, 0)
            val_text = lib.dbms_column_text(stmt_sel, 1).decode('utf-8')
            self.assertEqual(val_id, i)
            self.assertEqual(len(val_text), len(expected))
            self.assertEqual(val_text, expected)
            lib.dbms_finalize(stmt_sel)

        lib.dbms_close(db)

if __name__ == "__main__":
    unittest.main()
