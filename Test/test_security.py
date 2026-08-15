import os
import subprocess
import unittest

class TestSecurityRobustness(unittest.TestCase):
    def setUp(self):
        for f in ["test_sec.db", "test_sec.db-journal", "test_sec.db-wal"]:
            if os.path.exists(f):
                try: os.remove(f)
                except OSError: pass

    def tearDown(self):
        for f in ["test_sec.db", "test_sec.db-journal", "test_sec.db-wal"]:
            if os.path.exists(f):
                try: os.remove(f)
                except OSError: pass

    def run_db(self, db_name, commands):
        p = subprocess.Popen(["./db", db_name], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        inp = "\n".join(commands) + "\n.exit\n"
        out, err = p.communicate(inp)
        return p.returncode, out, err

    def test_oversized_string_input_handling(self):
        # Verify parser and executor bounds-check oversized input strings without crashing
        huge_name = "A" * 5000
        cmds = [
            "CREATE TABLE users (id INT, name VARCHAR(50));",
            f"INSERT INTO users VALUES (1, '{huge_name}');",
            "SELECT * FROM users;"
        ]
        rc, out, err = self.run_db("test_sec.db", cmds)
        self.assertEqual(rc, 0, "Process crashed on oversized string input")
        self.assertIn("Executed.", out)

    def test_sql_quote_escaping_and_injection_resilience(self):
        # Verify single-quote escaping correctly handles inputs that mimic SQL injection payloads
        payload = "admin'' OR ''1''=''1"
        cmds = [
            "CREATE TABLE auth (id INT, username TEXT, role TEXT);",
            f"INSERT INTO auth VALUES (1, '{payload}', 'guest');",
            "SELECT * FROM auth WHERE username = 'admin'' OR ''1''=''1';"
        ]
        rc, out, err = self.run_db("test_sec.db", cmds)
        self.assertEqual(rc, 0, "Process crashed during injection-style string handling")
        # Ensure the string was treated literally and stored safely
        self.assertIn(f"(1, admin' OR '1'='1, guest)", out)

    def test_syntax_fuzzing_and_malformed_queries(self):
        # Verify engine gracefully returns syntax error without crashing on malformed ASTs
        malformed = [
            "SELECT",
            "INSERT INTO",
            "WHERE id = 1",
            "CREATE TABLE ((((",
            "SELECT * FROM tbl WHERE col = ",
            "DROP TABLE ;;;;",
            "UPDATE SET = WHERE",
            "SELECT 1 FROM WHERE AND OR NOT",
            "PRAGMA ;;;",
            "ATTACH '' AS ''",
            "DETACH ''"
        ]
        rc, out, err = self.run_db("test_sec.db", malformed)
        self.assertEqual(rc, 0, "Process crashed on malformed SQL syntax fuzzing")

    def test_deep_condition_nesting(self):
        # Verify deep condition chaining terminates cleanly
        nested_where = " AND ".join([f"id >= {i}" for i in range(50)])
        cmds = [
            "CREATE TABLE test_nest (id INT);",
            "INSERT INTO test_nest VALUES (100);",
            f"SELECT * FROM test_nest WHERE {nested_where};"
        ]
        rc, out, err = self.run_db("test_sec.db", cmds)
        self.assertEqual(rc, 0, "Process crashed on nested condition chain")

    def test_type_boundary_extremes(self):
        # Test integer overflow limits and scientific notation parsing
        cmds = [
            "CREATE TABLE bounds (id INT, price DOUBLE);",
            "INSERT INTO bounds VALUES (2147483647, 1e300);",
            "INSERT INTO bounds VALUES (1, -1e300);",
            "SELECT * FROM bounds ORDER BY id ASC;"
        ]
        rc, out, err = self.run_db("test_sec.db", cmds)
        self.assertEqual(rc, 0, "Process crashed on extreme numeric boundaries")
        self.assertIn("(1, -1e+300)", out)
        self.assertIn("(2147483647, 1e+300)", out)

if __name__ == "__main__":
    unittest.main()
