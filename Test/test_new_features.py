import os
import subprocess
import unittest

class TestNewFeatures(unittest.TestCase):
    def setUp(self):
        for f in ["test_feat.db", "test_feat.db-journal", "test_feat.db-wal"]:
            if os.path.exists(f):
                try: os.remove(f)
                except OSError: pass

    def tearDown(self):
        for f in ["test_feat.db", "test_feat.db-journal", "test_feat.db-wal"]:
            if os.path.exists(f):
                try: os.remove(f)
                except OSError: pass

    def run_db(self, db_name, commands):
        p = subprocess.Popen(["./db", db_name], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        inp = "\n".join(commands) + "\n.exit\n"
        out, err = p.communicate(inp)
        lines = [l.replace("db > ", "").strip() for l in out.splitlines() if "(" in l and not l.startswith("db > CREATE")]
        return lines, out

    def test_distinct(self):
        cmds = [
            "CREATE TABLE t_dist (id INT, cat TEXT);",
            "INSERT INTO t_dist VALUES (1, 'Fruit');",
            "INSERT INTO t_dist VALUES (2, 'Veg');",
            "INSERT INTO t_dist VALUES (3, 'Fruit');",
            "INSERT INTO t_dist VALUES (4, 'Fruit');",
            "INSERT INTO t_dist VALUES (5, 'Veg');",
            "SELECT DISTINCT cat FROM t_dist ORDER BY cat ASC;"
        ]
        lines, _ = self.run_db("test_feat.db", cmds)
        self.assertEqual(len(lines), 2)
        self.assertEqual(lines[0], "(Fruit)")
        self.assertEqual(lines[1], "(Veg)")

    def test_like_wildcard(self):
        cmds = [
            "CREATE TABLE t_like (id INT, word TEXT);",
            "INSERT INTO t_like VALUES (1, 'apple');",
            "INSERT INTO t_like VALUES (2, 'application');",
            "INSERT INTO t_like VALUES (3, 'banana');",
            "INSERT INTO t_like VALUES (4, 'cat');",
            "SELECT word FROM t_like WHERE word LIKE 'app%' ORDER BY id ASC;"
        ]
        lines, _ = self.run_db("test_feat.db", cmds)
        self.assertEqual(len(lines), 2)
        self.assertEqual(lines[0], "(apple)")
        self.assertEqual(lines[1], "(application)")

    def test_between(self):
        cmds = [
            "CREATE TABLE t_between (id INT, age INT);",
            "INSERT INTO t_between VALUES (1, 15);",
            "INSERT INTO t_between VALUES (2, 20);",
            "INSERT INTO t_between VALUES (3, 25);",
            "INSERT INTO t_between VALUES (4, 35);",
            "SELECT id, age FROM t_between WHERE age BETWEEN 20 AND 30 ORDER BY id ASC;"
        ]
        lines, _ = self.run_db("test_feat.db", cmds)
        self.assertEqual(len(lines), 2)
        self.assertEqual(lines[0], "(2, 20)")
        self.assertEqual(lines[1], "(3, 25)")

    def test_limit_offset_pagination(self):
        cmds = [
            "CREATE TABLE t_page (id INT, val INT);",
            "INSERT INTO t_page VALUES (1, 10);",
            "INSERT INTO t_page VALUES (2, 20);",
            "INSERT INTO t_page VALUES (3, 30);",
            "INSERT INTO t_page VALUES (4, 40);",
            "INSERT INTO t_page VALUES (5, 50);",
            "SELECT val FROM t_page ORDER BY val ASC LIMIT 2 OFFSET 2;"
        ]
        lines, _ = self.run_db("test_feat.db", cmds)
        self.assertEqual(len(lines), 2)
        self.assertEqual(lines[0], "(30)")
        self.assertEqual(lines[1], "(40)")

    def test_multi_column_order_by(self):
        cmds = [
            "CREATE TABLE t_multi_sort (id INT, dept TEXT, salary INT);",
            "INSERT INTO t_multi_sort VALUES (1, 'HR', 5000);",
            "INSERT INTO t_multi_sort VALUES (2, 'Eng', 9000);",
            "INSERT INTO t_multi_sort VALUES (3, 'Eng', 7000);",
            "INSERT INTO t_multi_sort VALUES (4, 'HR', 6000);",
            "SELECT dept, salary FROM t_multi_sort ORDER BY dept ASC, salary DESC;"
        ]
        lines, _ = self.run_db("test_feat.db", cmds)
        self.assertEqual(len(lines), 4)
        self.assertEqual(lines[0], "(Eng, 9000)")
        self.assertEqual(lines[1], "(Eng, 7000)")
        self.assertEqual(lines[2], "(HR, 6000)")
        self.assertEqual(lines[3], "(HR, 5000)")

    def test_multi_row_insert(self):
        cmds = [
            "CREATE TABLE t_multi_ins (id INT, name TEXT);",
            "INSERT INTO t_multi_ins VALUES (1, 'Alpha'), (2, 'Beta'), (3, 'Gamma');",
            "SELECT * FROM t_multi_ins ORDER BY id ASC;"
        ]
        lines, _ = self.run_db("test_feat.db", cmds)
        self.assertEqual(len(lines), 3)
        self.assertEqual(lines[0], "(1, Alpha)")
        self.assertEqual(lines[1], "(2, Beta)")
        self.assertEqual(lines[2], "(3, Gamma)")

    def test_insert_into_select(self):
        cmds = [
            "CREATE TABLE src (id INT, name TEXT);",
            "CREATE TABLE dst (id INT, name TEXT);",
            "INSERT INTO src VALUES (1, 'One'), (2, 'Two'), (3, 'Three');",
            "INSERT INTO dst SELECT * FROM src WHERE id >= 2;",
            "SELECT * FROM dst ORDER BY id ASC;"
        ]
        lines, _ = self.run_db("test_feat.db", cmds)
        self.assertEqual(len(lines), 2)
        self.assertEqual(lines[0], "(2, Two)")
        self.assertEqual(lines[1], "(3, Three)")

    def test_case_when_and_arithmetic(self):
        cmds = [
            "CREATE TABLE products (id INT, price DOUBLE, qty INT);",
            "INSERT INTO products VALUES (1, 10.0, 5), (2, 25.0, 2);",
            "SELECT price * qty, CASE WHEN qty > 3 THEN 'Bulk' ELSE 'Standard' END FROM products ORDER BY id ASC;"
        ]
        lines, _ = self.run_db("test_feat.db", cmds)
        self.assertEqual(len(lines), 2)
        self.assertEqual(lines[0], "(50, Bulk)")
        self.assertEqual(lines[1], "(50, Standard)")

    def test_freelist_page_recycling(self):
        cmds = [
            "CREATE TABLE t_drop (id INT, val TEXT);",
            "INSERT INTO t_drop VALUES (1, 'foo');",
            "DROP TABLE t_drop;",
            "CREATE TABLE t_reuse (id INT, val TEXT);",
            "INSERT INTO t_reuse VALUES (1, 'reused');",
            "SELECT * FROM t_reuse;"
        ]
        lines, out = self.run_db("test_feat.db", cmds)
        self.assertIn("(1, reused)", lines)
        self.assertIn("Table 't_reuse' created with root page 33.", out)

if __name__ == "__main__":
    unittest.main()
