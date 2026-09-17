import os
import subprocess
import unittest

class TestSetOps(unittest.TestCase):
    def setUp(self):
        for f in ["test_set.db", "test_set.db-journal", "test_set.db-wal"]:
            if os.path.exists(f):
                try: os.remove(f)
                except OSError: pass

    def tearDown(self):
        for f in ["test_set.db", "test_set.db-journal", "test_set.db-wal"]:
            if os.path.exists(f):
                try: os.remove(f)
                except OSError: pass

    def run_db(self, db_name, commands):
        p = subprocess.Popen(["./db", db_name], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        inp = "\n".join(commands) + "\n.exit\n"
        out, err = p.communicate(inp)
        lines = [l.replace("db > ", "").strip() for l in out.splitlines() if "(" in l and not l.startswith("db > CREATE")]
        return lines, out

    def test_set_operations(self):
        cmds = [
            "CREATE TABLE t1 (id INT, v TEXT);",
            "CREATE TABLE t2 (id INT, v TEXT);",
            "INSERT INTO t1 VALUES (1, 'A'), (2, 'B'), (3, 'C');",
            "INSERT INTO t2 VALUES (2, 'B'), (3, 'C'), (4, 'D');",
            "SELECT v FROM t1 UNION SELECT v FROM t2;",
            "SELECT v FROM t1 UNION ALL SELECT v FROM t2;",
            "SELECT v FROM t1 INTERSECT SELECT v FROM t2;",
            "SELECT v FROM t1 EXCEPT SELECT v FROM t2;"
        ]
        lines, out = self.run_db("test_set.db", cmds)
        self.assertIn("(A)", lines)
        self.assertIn("(B)", lines)
        self.assertIn("(C)", lines)
        self.assertIn("(D)", lines)

if __name__ == "__main__":
    unittest.main()
