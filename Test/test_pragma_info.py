import os
import subprocess
import unittest

class TestPragmaInfo(unittest.TestCase):
    def setUp(self):
        for f in ["test_prg.db", "test_prg.db-journal", "test_prg.db-wal"]:
            if os.path.exists(f):
                try: os.remove(f)
                except OSError: pass

    def tearDown(self):
        for f in ["test_prg.db", "test_prg.db-journal", "test_prg.db-wal"]:
            if os.path.exists(f):
                try: os.remove(f)
                except OSError: pass

    def run_db(self, db_name, commands):
        p = subprocess.Popen(["./db", db_name], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        inp = "\n".join(commands) + "\n.exit\n"
        out, err = p.communicate(inp)
        lines = [l.replace("db > ", "").strip() for l in out.splitlines() if "(" in l and not l.startswith("db > CREATE")]
        return lines, out

    def test_pragma_table_info(self):
        cmds = [
            "CREATE TABLE users (id INT PRIMARY KEY, name TEXT NOT NULL, age INT);",
            "PRAGMA table_info(users);"
        ]
        lines, _ = self.run_db("test_prg.db", cmds)
        self.assertIn("(0, id, INTEGER, 0, NULL, 1)", lines)
        self.assertIn("(1, name, TEXT, 1, NULL, 0)", lines)
        self.assertIn("(2, age, INTEGER, 0, NULL, 0)", lines)

    def test_pragma_table_list(self):
        cmds = [
            "CREATE TABLE t1 (id INT);",
            "PRAGMA table_list;"
        ]
        lines, _ = self.run_db("test_prg.db", cmds)
        self.assertIn("(main, t1, table, 1, 0, 0)", lines)

if __name__ == "__main__":
    unittest.main()
