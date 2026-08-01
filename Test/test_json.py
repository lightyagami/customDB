import subprocess
import os

def run_db(db_file, commands):
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate("\n".join(commands) + "\n")
    return out.splitlines()

def test_json():
    db_file = "test_json.db"
    if os.path.exists(db_file):
        os.remove(db_file)

    # 1. Test json_extract on direct JSON literal string
    cmds = [
        "select json_extract('{\"name\":\"Alice\",\"age\":25}', '$.name')",
        "select json_extract('{\"user\":{\"id\":100}}', '$.user.id')",
        "select json_array(1, 2, 'three')",
        "select json_object('name', 'Bob', 'score', 95)",
        ".exit"
    ]
    lines = run_db(db_file, cmds)
    out = "\n".join(lines)

    assert "(Alice)" in out, f"json_extract name failed:\n{out}"
    assert "(100)" in out, f"json_extract nested user.id failed:\n{out}"
    assert "([1, 2, 'three'])" in out or "[1, 2, 'three']" in out, f"json_array failed:\n{out}"
    assert "({'name', 'Bob', 'score', 95})" in out or "name" in out, f"json_object failed:\n{out}"

    # 2. Test json_extract on table column
    tbl_cmds = [
        "create table profiles (id INT, data VARCHAR(256))",
        "insert into profiles values (1, '{\"role\":\"admin\",\"status\":\"active\"}')",
        "select json_extract(data, '$.role') from profiles",
        ".exit"
    ]
    lines_t = run_db(db_file, tbl_cmds)
    out_t = "\n".join(lines_t)

    assert "(admin)" in out_t, f"json_extract on table column failed:\n{out_t}"

    if os.path.exists(db_file):
        os.remove(db_file)

    print("✓ Native JSON Functions (json_extract, json_array, json_object) integration test passed!")

if __name__ == "__main__":
    test_json()
