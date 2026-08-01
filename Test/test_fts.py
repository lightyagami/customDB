import subprocess
import os

def run_db(db_file, commands):
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate("\n".join(commands) + "\n")
    return out.splitlines()

def test_fts():
    db_file = "test_fts.db"
    if os.path.exists(db_file):
        os.remove(db_file)

    # 1. Create FTS5 virtual table and insert documents
    cmds = [
        "create virtual table articles using fts5(title, body)",
        "insert into articles values (1, 'SQLite Architecture', 'SQLite is a C-language library that implements a small, fast, self-contained SQL database engine.')",
        "insert into articles values (2, 'DBMS Storage Engine', 'B+Tree data structures provide efficient disk block index lookup and linear range scans.')",
        "select * from articles where body match 'SQLite'",
        "select * from articles where body match 'B+Tree'",
        ".exit"
    ]
    lines = run_db(db_file, cmds)
    out = "\n".join(lines)

    assert "FTS5 Virtual Table 'articles' created successfully" in out, f"CREATE VIRTUAL TABLE FTS5 failed:\n{out}"
    assert "SQLite Architecture" in out, f"FTS MATCH 'SQLite' query failed:\n{out}"
    assert "DBMS Storage Engine" in out, f"FTS MATCH 'B+Tree' query failed:\n{out}"

    if os.path.exists(db_file):
        os.remove(db_file)

    print("✓ Full-Text Search Engine (FTS5 Virtual Table & MATCH) integration test passed!")

if __name__ == "__main__":
    test_fts()
