import os
import subprocess
import unittest

class TestMultiTableJoins(unittest.TestCase):
    def setUp(self):
        for f in ["test_joins.db", "test_joins.db-journal", "test_joins.db-wal"]:
            if os.path.exists(f):
                try: os.remove(f)
                except OSError: pass

    def tearDown(self):
        for f in ["test_joins.db", "test_joins.db-journal", "test_joins.db-wal"]:
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
        return p.returncode, lines, out

    def test_three_table_hash_join(self):
        # Setup users, orders, products
        cmds = [
            "CREATE TABLE users (id INT, name TEXT);",
            "CREATE TABLE products (id INT, title TEXT, price DOUBLE);",
            "CREATE TABLE orders (id INT, user_id INT, product_id INT, qty INT);",
            "INSERT INTO users VALUES (1, 'Alice');",
            "INSERT INTO users VALUES (2, 'Bob');",
            "INSERT INTO products VALUES (10, 'Keyboard', 75.0);",
            "INSERT INTO products VALUES (20, 'Monitor', 300.0);",
            "INSERT INTO orders VALUES (100, 1, 10, 2);",
            "INSERT INTO orders VALUES (101, 2, 20, 1);",
            "SELECT users.name, products.title, orders.qty FROM users JOIN orders ON users.id = orders.user_id JOIN products ON orders.product_id = products.id ORDER BY users.name ASC;"
        ]
        rc, lines, out = self.run_db("test_joins.db", cmds)
        self.assertEqual(rc, 0)
        self.assertEqual(len(lines), 2)
        self.assertEqual(lines[0], "(Alice, Keyboard, 2)")
        self.assertEqual(lines[1], "(Bob, Monitor, 1)")

    def test_four_table_hash_join_with_left_join(self):
        # Setup users, orders, products, suppliers
        cmds = [
            "CREATE TABLE users (id INT, name TEXT);",
            "CREATE TABLE suppliers (id INT, sname TEXT);",
            "CREATE TABLE products (id INT, title TEXT, supplier_id INT);",
            "CREATE TABLE orders (id INT, user_id INT, product_id INT);",
            "INSERT INTO users VALUES (1, 'Alice');",
            "INSERT INTO users VALUES (2, 'Bob');",
            "INSERT INTO users VALUES (3, 'Charlie');",
            "INSERT INTO suppliers VALUES (500, 'TechCorp');",
            "INSERT INTO products VALUES (10, 'Mouse', 500);",
            "INSERT INTO products VALUES (20, 'Stand', NULL);",
            "INSERT INTO orders VALUES (100, 1, 10);",
            "INSERT INTO orders VALUES (101, 2, 20);",
            "SELECT users.name, products.title, suppliers.sname FROM users JOIN orders ON users.id = orders.user_id JOIN products ON orders.product_id = products.id LEFT JOIN suppliers ON products.supplier_id = suppliers.id ORDER BY users.name ASC;"
        ]
        rc, lines, out = self.run_db("test_joins.db", cmds)
        self.assertEqual(rc, 0)
        self.assertEqual(len(lines), 2)
        self.assertEqual(lines[0], "(Alice, Mouse, TechCorp)")
        self.assertEqual(lines[1], "(Bob, Stand, NULL)")

if __name__ == "__main__":
    unittest.main()
