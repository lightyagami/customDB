import os
import subprocess
import unittest

class TestGlob(unittest.TestCase):
    def setUp(self):
        for f in ["test_glob.db", "test_glob.db-journal", "test_glob.db-wal"]:
            if os.path.exists(f):
                try: os.remove(f)
                except OSError: pass

    def tearDown(self):
        for f in ["test_glob.db", "test_glob.db-journal", "test_glob.db-wal"]:
            if os.path.exists(f):
                try: os.remove(f)
                except OSError: pass

    def run_db(self, db_name, commands):
        p = subprocess.Popen(["./db", db_name], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        inp = "\n".join(commands) + "\n.exit\n"
        out, err = p.communicate(inp)
        lines = [l.replace("db > ", "").strip() for l in out.splitlines() if "(" in l and not l.startswith("db > CREATE")]
        return lines, out

    def test_glob_patterns(self):
        cmds = [
            "CREATE TABLE tg (id INT, word TEXT);",
            "INSERT INTO tg VALUES (1, 'hello'), (2, 'help'), (3, 'world'), (4, 'hero');",
            "SELECT word FROM tg WHERE word GLOB 'h*' ORDER BY id ASC;",
            "SELECT word FROM tg WHERE word GLOB 'hel?' ORDER BY id ASC;",
            "SELECT word FROM tg WHERE word NOT GLOB 'h*' ORDER BY id ASC;"
        ]
        lines, _ = self.run_db("test_glob.db", cmds)
        self.assertIn("(hello)", lines)
        self.assertIn("(help)", lines)
        self.assertIn("(hero)", lines)
        self.assertIn("(world)", lines)

if __name__ == "__main__":
    unittest.main()
