"""Regression tests for B-tree overflow cells.

1. A record whose serialized size is exactly BTREE_MAX_LOCAL_PAYLOAD (1024) used
   to be indistinguishable from an overflow stub. cursor_value() then followed a
   garbage page number (segfault / 16 GB realloc). Body length 1001 for
   (id INT, body TEXT) produces such a record; we sweep a wide window instead of
   hard-coding the row overhead.
2. leaf_node_insert() serialized a row (allocating its overflow chain) and then
   leaf_node_split_and_insert() serialized it again, orphaning the first chain.
   Orphaned pages are neither referenced nor on the freelist, so the file grows
   ~30% more than necessary. We assert an upper bound on file size.
"""
import ctypes
import os
import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parent.parent
DB_BIN = ROOT / "db"
LIB = ROOT / "libdbms.so"
PAGE = 4096


def run_cli(db_path, cmds):
    env = os.environ.copy()
    env["ASAN_OPTIONS"] = "detect_leaks=1"
    if os.path.exists("/usr/lib/libasan.so"):
        env["LD_PRELOAD"] = "/usr/lib/libasan.so"
    p = subprocess.run(
        [str(DB_BIN), str(db_path)],
        input="\n".join(cmds) + "\n.exit\n",
        capture_output=True, text=True, timeout=120,
        env=env,
    )
    return p



@pytest.mark.parametrize("n", range(990, 1040))
def test_inline_stub_boundary_roundtrip(tmp_path, n):
    """Insert / select / count / delete for bodies straddling the 1024-byte cell limit."""
    db = tmp_path / "b.db"
    body = "x" * n
    p = run_cli(db, [
        "CREATE TABLE t (id INT, body TEXT);",
        f"INSERT INTO t VALUES (2, '{body}');",
        "SELECT LENGTH(body) FROM t WHERE id = 2;",
        "SELECT COUNT(*) FROM t;",
        "DELETE FROM t WHERE id = 2;",
        "SELECT COUNT(*) FROM t;",
    ])
    assert p.returncode == 0, f"crashed (rc={p.returncode}): {p.stderr[:300]}"
    assert f"({n})" in p.stdout
    assert p.stdout.count("(1)") == 1 and "(0)" in p.stdout


class Api:
    def __init__(self, path):
        c = ctypes
        L = self.lib = c.CDLL(str(LIB))
        L.dbms_open.argtypes = [c.c_char_p, c.POINTER(c.c_void_p)]
        L.dbms_close.argtypes = [c.c_void_p]
        L.dbms_prepare_v2.argtypes = [c.c_void_p, c.c_char_p, c.c_int,
                                      c.POINTER(c.c_void_p), c.POINTER(c.c_char_p)]
        L.dbms_bind_int.argtypes = [c.c_void_p, c.c_int, c.c_int]
        L.dbms_bind_text.argtypes = [c.c_void_p, c.c_int, c.c_char_p, c.c_int]
        L.dbms_step.argtypes = [c.c_void_p]
        L.dbms_column_text.argtypes = [c.c_void_p, c.c_int]
        L.dbms_column_text.restype = c.c_char_p
        L.dbms_finalize.argtypes = [c.c_void_p]
        self.path = str(path)
        self.db = c.c_void_p()
        assert L.dbms_open(self.path.encode(), c.byref(self.db)) == 0

    def prep(self, sql):
        s = ctypes.c_void_p()
        b = sql.encode()
        assert self.lib.dbms_prepare_v2(self.db, b, len(b), ctypes.byref(s), None) == 0, sql
        return s

    def exec(self, sql):
        s = self.prep(sql)
        rc = self.lib.dbms_step(s)
        self.lib.dbms_finalize(s)
        return rc

    def insert(self, i, data: bytes):
        s = self.prep("INSERT INTO t VALUES (?, ?);")
        self.lib.dbms_bind_int(s, 1, i)
        self.lib.dbms_bind_text(s, 2, data, len(data))
        rc = self.lib.dbms_step(s)
        self.lib.dbms_finalize(s)
        return rc

    def get(self, i):
        s = self.prep(f"SELECT data FROM t WHERE id = {i};")
        v = self.lib.dbms_column_text(s, 0) if self.lib.dbms_step(s) == 100 else None
        self.lib.dbms_finalize(s)
        return v

    def pages(self):
        """Close + reopen so the file on disk is fully flushed, then return its size in pages."""
        self.lib.dbms_close(self.db)
        n = os.path.getsize(self.path) / PAGE
        assert self.lib.dbms_open(self.path.encode(), ctypes.byref(self.db)) == 0
        return n

    def close(self):
        self.lib.dbms_close(self.db)


def payload(i, n):
    return (f"R{i}_" * (n // 4 + 2))[:n].encode()


def test_no_orphaned_overflow_pages_on_split(tmp_path):
    ROWS, SIZE = 96, 20000
    api = Api(tmp_path / "leak.db")
    try:
        api.exec("CREATE TABLE t (id INT, data TEXT);")
        for i in range(1, ROWS + 1):
            assert api.insert(i, payload(i, SIZE)) == 101
        used = api.pages()
        for i in range(1, ROWS + 1):
            assert api.get(i) == payload(i, SIZE)

        # Each 20 KB row needs ceil((20000+hdr-1016)/4092) = 5 overflow pages, and a
        # leaf holds at most 3 stubs. Generous ceiling that still fails on the old
        # double-serialize behaviour (787 pages on the pre-fix code, 555 after).
        ceiling = 34 + ROWS * 5 + ROWS // 2 + 20
        assert used <= ceiling, f"{used} pages > {ceiling}: overflow chains are leaking"
    finally:
        api.close()


def test_vacuum_preserves_multi_page_overflow_rows(tmp_path):
    """VACUUM rewrites every row through deserialize/serialize; chains of 5 pages must survive
    intact and the rewritten file must not be larger than the original."""
    ROWS, SIZE = 40, 20000
    api = Api(tmp_path / "vac.db")
    try:
        api.exec("CREATE TABLE t (id INT, data TEXT);")
        for i in range(1, ROWS + 1):
            assert api.insert(i, payload(i, SIZE)) == 101
        before = api.pages()
        api.exec("VACUUM;")
        after = api.pages()
        assert after <= before
        for i in range(1, ROWS + 1):
            assert api.get(i) == payload(i, SIZE), f"row {i} corrupted by VACUUM"
    finally:
        api.close()


@pytest.mark.xfail(strict=True, reason="pre-existing: execute_vacuum copies logically deleted "
                                        "(xmax != 0) row versions back in as live rows")
def test_vacuum_does_not_resurrect_deleted_rows(tmp_path):
    p = run_cli(tmp_path / "r.db", [
        "CREATE TABLE t (id INT, name TEXT);",
        "INSERT INTO t VALUES (1, 'a');",
        "INSERT INTO t VALUES (2, 'b');",
        "DELETE FROM t;",
        "VACUUM;",
        "SELECT COUNT(*) FROM t;",
    ])
    assert "(0)" in p.stdout.split("Database vacuumed")[-1]
