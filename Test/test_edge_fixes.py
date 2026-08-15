import os
import subprocess
import unittest

class TestEdgeFixes(unittest.TestCase):
    def setUp(self):
        for f in ["test_edge.db", "test_edge.db-journal", "test_edge.db-wal"]:
            if os.path.exists(f):
                try: os.remove(f)
                except OSError: pass

    def tearDown(self):
        for f in ["test_edge.db", "test_edge.db-journal", "test_edge.db-wal"]:
            if os.path.exists(f):
                try: os.remove(f)
                except OSError: pass

    def run_db(self, db_name, commands):
        p = subprocess.Popen(["./db", db_name], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        inp = "\n".join(commands) + "\n.exit\n"
        out, err = p.communicate(inp)
        lines = []
        for l in out.splitlines():
            s = l.replace("db > ", "").strip()
            if s.startswith("(") and s.endswith(")"):
                lines.append(s)
        return lines, out

    def test_multi_index_analyze(self):
        cmds = [
            "CREATE TABLE t1 (id INT, a INT, b INT);",
            "CREATE INDEX idx_t1_a ON t1 (a);",
            "CREATE INDEX idx_t1_b ON t1 (b);",
            "CREATE TABLE t2 (id INT, c INT);",
            "CREATE INDEX idx_t2_c ON t2 (c);",
            "INSERT INTO t1 VALUES (1, 10, 20), (2, 30, 40);",
            "INSERT INTO t2 VALUES (1, 100), (2, 200);",
            "ANALYZE;",
            "SELECT * FROM sqlite_stat1 ORDER BY id ASC;"
        ]
        lines, out = self.run_db("test_edge.db", cmds)
        self.assertIn("ANALYZE completed.", out)
        # Verify all 3 indexes exist in sqlite_stat1 with unique primary keys
        self.assertEqual(len(lines), 3)
        self.assertIn("_idx_t1_a", lines[0])
        self.assertIn("_idx_t1_b", lines[1])
        self.assertIn("_idx_t2_c", lines[2])

    def test_fk_cascade_non_pk_and_text(self):
        cmds = [
            "CREATE TABLE authors (id INT, email TEXT, name TEXT);",
            "CREATE TABLE posts (id INT, author_email TEXT REFERENCES authors(email) ON DELETE CASCADE ON UPDATE CASCADE, content TEXT);",
            "INSERT INTO authors VALUES (1, 'alice@test.com', 'Alice');",
            "INSERT INTO authors VALUES (2, 'bob@test.com', 'Bob');",
            "INSERT INTO posts VALUES (10, 'alice@test.com', 'Post 1 by Alice');",
            "INSERT INTO posts VALUES (20, 'bob@test.com', 'Post 1 by Bob');",
            "DELETE FROM authors WHERE id = 1;",
            "SELECT * FROM posts;"
        ]
        lines, _ = self.run_db("test_edge.db", cmds)
        self.assertEqual(len(lines), 1)
        self.assertIn("Post 1 by Bob", lines[0])

    def test_view_predicate_merging_and_modifiers(self):
        cmds = [
            "CREATE TABLE emp (id INT, dept TEXT, salary INT);",
            "INSERT INTO emp VALUES (1, 'Engineering', 9000);",
            "INSERT INTO emp VALUES (2, 'Engineering', 5000);",
            "INSERT INTO emp VALUES (3, 'Engineering', 7000);",
            "INSERT INTO emp VALUES (4, 'Marketing', 9000);",
            "CREATE VIEW high_eng AS SELECT * FROM emp WHERE dept = 'Engineering';",
            # Query view with outer WHERE, ORDER BY, and LIMIT
            "SELECT id, salary FROM high_eng WHERE salary >= 7000 ORDER BY salary DESC LIMIT 1;"
        ]
        lines, _ = self.run_db("test_edge.db", cmds)
        self.assertEqual(len(lines), 1)
        self.assertEqual(lines[0], "(1, 9000)")

    def test_trigger_recursion_guard(self):
        cmds = [
            "CREATE TABLE t_ping (id INT, val INT);",
            "CREATE TABLE t_pong (id INT, val INT);",
            "CREATE TRIGGER tr_ping AFTER INSERT ON t_ping BEGIN INSERT INTO t_pong VALUES (1, 100); END;",
            "CREATE TRIGGER tr_pong AFTER INSERT ON t_pong BEGIN INSERT INTO t_ping VALUES (2, 200); END;",
            "INSERT INTO t_ping VALUES (10, 500);",
            "SELECT * FROM t_ping;",
            "SELECT * FROM t_pong;"
        ]
        lines, out = self.run_db("test_edge.db", cmds)
        # Should terminate safely without stack overflow / segfault
        self.assertIn("Executed.", out)

if __name__ == "__main__":
    unittest.main()
