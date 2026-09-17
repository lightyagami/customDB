import os
import subprocess
import unittest

class TestScalarFuncs(unittest.TestCase):
    def setUp(self):
        for f in ["test_scalar.db", "test_scalar.db-journal", "test_scalar.db-wal"]:
            if os.path.exists(f):
                try: os.remove(f)
                except OSError: pass

    def tearDown(self):
        for f in ["test_scalar.db", "test_scalar.db-journal", "test_scalar.db-wal"]:
            if os.path.exists(f):
                try: os.remove(f)
                except OSError: pass

    def run_db(self, db_name, commands):
        p = subprocess.Popen(["./db", db_name], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        inp = "\n".join(commands) + "\n.exit\n"
        out, err = p.communicate(inp)
        lines = [l.replace("db > ", "").strip() for l in out.splitlines() if "(" in l and not l.startswith("db > CREATE")]
        return lines, out

    def test_cast_numbers(self):
        cmds = [
            "SELECT CAST(42 AS TEXT);",
            "SELECT CAST('100' AS INT);",
            "SELECT CAST('3.14' AS REAL);"
        ]
        lines, _ = self.run_db("test_scalar.db", cmds)
        self.assertIn("(42)", lines)
        self.assertIn("(100)", lines)
        self.assertIn("(3.14)", lines)

    def test_typeof(self):
        cmds = [
            "CREATE TABLE t_type (id INT, r REAL, t TEXT);",
            "INSERT INTO t_type VALUES (1, 3.14, 'hello');",
            "SELECT typeof(id), typeof(r), typeof(t) FROM t_type;",
            "SELECT typeof(123), typeof('text'), typeof(NULL);"
        ]
        lines, _ = self.run_db("test_scalar.db", cmds)
        self.assertIn("(integer, real, text)", lines)
        self.assertIn("(integer, text, null)", lines)

    def test_random(self):
        cmds = [
            "SELECT random();",
            "SELECT random();"
        ]
        lines, _ = self.run_db("test_scalar.db", cmds)
        self.assertEqual(len(lines), 2)
        self.assertNotEqual(lines[0], "(NULL)")
        self.assertNotEqual(lines[1], "(NULL)")
        self.assertNotEqual(lines[0], lines[1])

if __name__ == "__main__":
    unittest.main()
