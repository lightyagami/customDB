import subprocess
import os

def run_db(db_file, commands):
    p = subprocess.Popen(["./db", db_file], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    out, err = p.communicate("\n".join(commands) + "\n")
    return out.splitlines()

def test_attach_detach():
    main_db = "test_main.db"
    aux_db = "test_aux.db"
    for f in [main_db, aux_db]:
        if os.path.exists(f):
            os.remove(f)

    # 1. ATTACH DATABASE aux_db AS aux
    cmds = [
        f"attach database '{aux_db}' as aux",
        "detach database aux",
        ".exit"
    ]
    lines = run_db(main_db, cmds)
    out = "\n".join(lines)
    assert "Attached" in out or "Executed" in out, f"ATTACH DATABASE failed:\n{out}"
    assert "Detached" in out or "Executed" in out, f"DETACH DATABASE failed:\n{out}"

    for f in [main_db, aux_db]:
        if os.path.exists(f):
            os.remove(f)

    print("✓ ATTACH / DETACH Database (Multi-Database Connection Namespace) integration test passed!")

if __name__ == "__main__":
    test_attach_detach()
