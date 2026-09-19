import subprocess
import os
import math
import unittest

def l2_dist(v1, v2):
    return math.sqrt(sum((a - b) ** 2 for a, b in zip(v1, v2)))

def format_vector(v):
    return "[" + ", ".join(f"{x:.4f}" for x in v) + "]"

class TestVectorKnnDifferential(unittest.TestCase):
    DB_FILE = "test_knn_diff.db"

    def setUp(self):
        self._cleanup()

    def tearDown(self):
        self._cleanup()

    def _cleanup(self):
        for f in [self.DB_FILE, f"{self.DB_FILE}-journal", f"{self.DB_FILE}-wal"]:
            if os.path.exists(f):
                try:
                    os.remove(f)
                except OSError:
                    pass

    def run_db(self, commands):
        env = os.environ.copy()
        env["ASAN_OPTIONS"] = "detect_leaks=1"
        if os.path.exists("/usr/lib/libasan.so"):
            env["LD_PRELOAD"] = "/usr/lib/libasan.so"
        p = subprocess.Popen(
            ["./db", self.DB_FILE],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            env=env
        )
        inp = "\n".join(commands) + "\n.exit\n"
        out, err = p.communicate(inp)
        return out, err

    def test_768_dim_differential_knn(self):
        # Construct 768-dim query vector
        query_vec = [0.1000] * 25 + [0.5000] * 743

        # Construct candidate vectors:
        # vec1 is an adversarial trap: slightly closer in the first 25 dims (diff=0.0001),
        # but vastly further in dims 25..768 (diff=0.4000).
        # Any truncation <= 25 dims will falsely pick vec1.
        vec1 = [0.1001] * 25 + [0.9000] * 743

        # vec2 is the true nearest neighbor: slightly further in first 25 dims (diff=0.0010),
        # but exact match in dims 25..768 (diff=0.0000).
        vec2 = [0.1010] * 25 + [0.5000] * 743

        # vec3, vec4: decoys
        vec3 = [0.1005] * 25 + [0.7000] * 743
        vec4 = [0.2000] * 25 + [0.6000] * 743

        vectors = {1: vec1, 2: vec2, 3: vec3, 4: vec4}

        # Python ground-truth calculation across ALL 768 dimensions:
        distances_768 = {row_id: l2_dist(query_vec, v) for row_id, v in vectors.items()}
        ground_truth_id = min(distances_768, key=distances_768.get)

        # Confirm Python ground truth: vec2 MUST be closest in 768 dims
        self.assertEqual(ground_truth_id, 2)

        # Confirm that a truncated 25-dim prefix would have produced vec1 (the wrong answer)
        distances_25 = {row_id: l2_dist(query_vec[:25], v[:25]) for row_id, v in vectors.items()}
        truncated_trap_id = min(distances_25, key=distances_25.get)
        self.assertEqual(truncated_trap_id, 1)

        # Now execute against the actual database engine
        q_str = format_vector(query_vec)
        cmds = [
            "CREATE TABLE embeddings (id INT, vec VECTOR(768));",
            f"INSERT INTO embeddings VALUES (1, '{format_vector(vec1)}');",
            f"INSERT INTO embeddings VALUES (2, '{format_vector(vec2)}');",
            f"INSERT INTO embeddings VALUES (3, '{format_vector(vec3)}');",
            f"INSERT INTO embeddings VALUES (4, '{format_vector(vec4)}');",
            f"SELECT id FROM embeddings ORDER BY l2_distance(vec, '{q_str}') ASC LIMIT 1;"
        ]

        out, err = self.run_db(cmds)
        self.assertNotIn("Error", out)
        self.assertNotIn("Syntax error", out)

        # The query output must return row id 2 (the true 768-dim neighbor), NOT row id 1!
        self.assertIn("(2)", out, f"Expected nearest neighbor id 2, but output was:\n{out}")
        self.assertNotIn("(1)", out)

    def test_768_dim_differential_knn_http(self):
        import urllib.request
        import json
        import time

        query_vec = [0.1000] * 25 + [0.5000] * 743
        vec1 = [0.1001] * 25 + [0.9000] * 743
        vec2 = [0.1010] * 25 + [0.5000] * 743
        vec3 = [0.1005] * 25 + [0.7000] * 743
        vec4 = [0.2000] * 25 + [0.6000] * 743

        env = os.environ.copy()
        env["ASAN_OPTIONS"] = "detect_leaks=1"
        if os.path.exists("/usr/lib/libasan.so"):
            env["LD_PRELOAD"] = "/usr/lib/libasan.so"

        server_port = 8899
        p = subprocess.Popen(
            ["./db_server", str(server_port), self.DB_FILE],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env
        )
        time.sleep(0.3)

        try:
            def query_http(sql):
                req = urllib.request.Request(
                    f"http://127.0.0.1:{server_port}/query",
                    data=json.dumps({"sql": sql}).encode("utf-8"),
                    headers={"Content-Type": "application/json"}
                )
                with urllib.request.urlopen(req) as resp:
                    return json.loads(resp.read().decode("utf-8"))

            query_http("CREATE TABLE embeddings (id INT, vec VECTOR(768));")
            query_http(f"INSERT INTO embeddings VALUES (1, '{format_vector(vec1)}');")
            query_http(f"INSERT INTO embeddings VALUES (2, '{format_vector(vec2)}');")
            query_http(f"INSERT INTO embeddings VALUES (3, '{format_vector(vec3)}');")
            query_http(f"INSERT INTO embeddings VALUES (4, '{format_vector(vec4)}');")

            q_str = format_vector(query_vec)
            res = query_http(f"SELECT id FROM embeddings ORDER BY l2_distance(vec, '{q_str}') ASC LIMIT 1;")
            self.assertEqual(res.get("columns"), ["id"])
            self.assertEqual(res.get("rows"), [["2"]])
        finally:
            p.terminate()
            p.wait()

if __name__ == "__main__":
    unittest.main()

