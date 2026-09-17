import os
import subprocess
import unittest

class TestReturning(unittest.TestCase):
    def setUp(self):
        for f in ["test_ret.db", "test_ret.db-journal", "test_ret.db-wal"]:
            if os.path.exists(f):
                try: os.remove(f)
                except OSError: pass

    def tearDown(self):
        for f in ["test_ret.db", "test_ret.db-journal", "test_ret.db-wal"]:
            if os.path.exists(f):
                try: os.remove(f)
                except OSError: pass

    def run_db(self, db_name, commands):
        p = subprocess.Popen(["./db", db_name], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        inp = "\n".join(commands) + "\n.exit\n"
        out, err = p.communicate(inp)
        lines = [l.replace("db > ", "").strip() for l in out.splitlines() if "(" in l and not l.startswith("db > CREATE")]
        return lines, out

    def test_returning_crud(self):
        cmds = [
            "CREATE TABLE tr (id INT PRIMARY KEY, name TEXT);",
            "INSERT INTO tr VALUES (1, 'Alice') RETURNING *;",
            "INSERT INTO tr VALUES (2, 'Bob') RETURNING name;",
            "UPDATE tr SET name = 'Alicia' WHERE id = 1 RETURNING name;",
            "DELETE FROM tr WHERE id = 2 RETURNING id;"
        ]
        lines, _ = self.run_db("test_ret.db", cmds)
        self.assertIn("(1, Alice)", lines)
        self.assertIn("(Bob)", lines)
        self.assertIn("(Alicia)", lines)
        self.assertIn("(2)", lines)

if __name__ == "__main__":
    unittest.main()
