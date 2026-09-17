import os
import subprocess
import unittest

class TestAttachCross(unittest.TestCase):
    def setUp(self):
        for f in ["main.db", "ext.db"]:
            if os.path.exists(f):
                try: os.remove(f)
                except OSError: pass

    def tearDown(self):
        for f in ["main.db", "ext.db"]:
            if os.path.exists(f):
                try: os.remove(f)
                except OSError: pass

    def run_db(self, db_name, commands):
        p = subprocess.Popen(["./db", db_name], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        inp = "\n".join(commands) + "\n.exit\n"
        out, err = p.communicate(inp)
        lines = [l.replace("db > ", "").strip() for l in out.splitlines() if "(" in l and not l.startswith("db > CREATE")]
        return lines, out

    def test_cross_db_query(self):
        # 1. Populate ext.db
        cmds1 = [
            "CREATE TABLE remote (id INT, note TEXT);",
            "INSERT INTO remote VALUES (99, 'remote_payload');"
        ]
        self.run_db("ext.db", cmds1)

        # 2. Attach from main.db and query
        cmds2 = [
            "ATTACH 'ext.db' AS ext;",
            "SELECT * FROM ext.remote;"
        ]
        lines, out = self.run_db("main.db", cmds2)
        self.assertIn("(99, remote_payload)", lines)

if __name__ == "__main__":
    unittest.main()
