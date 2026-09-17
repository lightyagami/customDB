import os
import subprocess
import unittest

class TestUpsert(unittest.TestCase):
    def setUp(self):
        for f in ["test_up.db", "test_up.db-journal", "test_up.db-wal"]:
            if os.path.exists(f):
                try: os.remove(f)
                except OSError: pass

    def tearDown(self):
        for f in ["test_up.db", "test_up.db-journal", "test_up.db-wal"]:
            if os.path.exists(f):
                try: os.remove(f)
                except OSError: pass

    def run_db(self, db_name, commands):
        p = subprocess.Popen(["./db", db_name], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        inp = "\n".join(commands) + "\n.exit\n"
        out, err = p.communicate(inp)
        lines = [l.replace("db > ", "").strip() for l in out.splitlines() if "(" in l and not l.startswith("db > CREATE")]
        return lines, out

    def test_upsert_modes(self):
        cmds = [
            "CREATE TABLE t_up (id INT PRIMARY KEY, val TEXT);",
            "INSERT INTO t_up VALUES (1, 'initial');",
            "INSERT OR IGNORE INTO t_up VALUES (1, 'ignored');",
            "SELECT val FROM t_up WHERE id = 1;",
            "INSERT OR REPLACE INTO t_up VALUES (1, 'replaced');",
            "SELECT val FROM t_up WHERE id = 1;",
            "INSERT INTO t_up VALUES (1, 'conflict') ON CONFLICT DO UPDATE SET val = 'updated';",
            "SELECT val FROM t_up WHERE id = 1;"
        ]
        lines, _ = self.run_db("test_up.db", cmds)
        self.assertIn("(initial)", lines)
        self.assertIn("(replaced)", lines)
        self.assertIn("(updated)", lines)

if __name__ == "__main__":
    unittest.main()
