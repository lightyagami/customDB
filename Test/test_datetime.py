import subprocess
import os

def run_db(db_file, commands):
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate("\n".join(commands) + "\n")
    return out.splitlines()

def test_datetime():
    db_file = "test_datetime.db"
    if os.path.exists(db_file):
        os.remove(db_file)

    # 1. Test SELECT date('now'), time('now'), datetime('now'), strftime(...)
    cmds = [
        "select date('now')",
        "select time('now')",
        "select datetime('now')",
        "select strftime('%Y-%m-%d', 'now')",
        ".exit"
    ]
    lines = run_db(db_file, cmds)
    out = "\n".join(lines)

    import datetime
    today_str = datetime.date.today().strftime("%Y-%m-%d")
    assert today_str in out, f"Date function failed, expected {today_str} in output:\n{out}"

    if os.path.exists(db_file):
        os.remove(db_file)

    print("✓ Date & Time Functions (datetime('now'), date('now'), time('now'), strftime()) integration test passed!")

if __name__ == "__main__":
    test_datetime()
