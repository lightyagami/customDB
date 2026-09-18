import subprocess
import os
import pytest

def run_db(db_path, commands):
    proc = subprocess.Popen(
        ["./db", db_path],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    stdout, stderr = proc.communicate("\n".join(commands) + "\n")
    lines = [line.strip() for line in stdout.splitlines() if line.strip()]
    return lines

def test_vector_type_and_distance():
    db_file = "test_vector.db"
    if os.path.exists(db_file):
        os.remove(db_file)

    # 1. Create table with VECTOR(3)
    cmds = [
        "create table items (id INT, embedding VECTOR(3))",
        "insert into items values (1, '[1.0, 2.0, 3.0]')",
        "insert into items values (2, '[4.0, 6.0, 3.0]')",
        ".exit"
    ]
    out = "\n".join(run_db(db_file, cmds))
    assert "Executed." in out

    # 2. Select basic rows
    select_cmds = [
        "select * from items",
        ".exit"
    ]
    out_sel = "\n".join(run_db(db_file, select_cmds))
    assert "(1, [1.0, 2.0, 3.0])" in out_sel or "(1, [1.0,2.0,3.0])" in out_sel or "(1, [1" in out_sel

    # 3. Test l2_distance
    # dist(v1, v1) = 0.0
    # dist(v2, v1) = sqrt((4-1)^2 + (6-2)^2 + (3-3)^2) = sqrt(9 + 16) = 5.0
    l2_cmds = [
        "select id, l2_distance(embedding, '[1.0, 2.0, 3.0]') from items",
        ".exit"
    ]
    out_l2 = "\n".join(run_db(db_file, l2_cmds))
    assert "(1, 0)" in out_l2 or "(1, 0.0)" in out_l2, f"Expected 0 distance for row 1: {out_l2}"
    assert "(2, 5)" in out_l2 or "(2, 5.0)" in out_l2, f"Expected 5.0 distance for row 2: {out_l2}"

    # 4. Test cosine_similarity
    # cosine(v1, v1) = 1.0
    cos_cmds = [
        "select id, cosine_similarity(embedding, '[1.0, 2.0, 3.0]') from items",
        ".exit"
    ]
    out_cos = "\n".join(run_db(db_file, cos_cmds))
    assert "(1, 1)" in out_cos or "(1, 1.0)" in out_cos, f"Expected 1.0 similarity for row 1: {out_cos}"

    # 5. Test raw bracket token without quotes
    insert_raw = [
        "insert into items values (3, [1.0, 2.0, 3.0])",
        "select id, l2_distance(embedding, '[1.0, 2.0, 3.0]') from items where id = 3",
        ".exit"
    ]
    out_raw = "\n".join(run_db(db_file, insert_raw))
    assert "(3, 0)" in out_raw or "(3, 0.0)" in out_raw, f"Expected raw bracket vector to insert & match: {out_raw}"

    # 6. Test vector similarity search: ORDER BY l2_distance(...) ASC LIMIT 1
    knn_cmds = [
        "select id from items order by l2_distance(embedding, '[1.0, 2.0, 3.0]') asc limit 1",
        ".exit"
    ]
    lines_knn = run_db(db_file, knn_cmds)
    result_rows = []
    for l in lines_knn:
        s = l.replace("db >", "").strip()
        if s.startswith("(") and s.endswith(")"):
            result_rows.append(s)
    assert len(result_rows) == 1, f"Expected exactly 1 row returned with LIMIT 1, got: {result_rows}"
    assert result_rows[0] == "(1)" or result_rows[0] == "(3)", f"Expected nearest row, got: {result_rows[0]}"

    # Clean up
    if os.path.exists(db_file):
        os.remove(db_file)
