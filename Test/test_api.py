#!/usr/bin/env python3
import ctypes
import os

DB_FILE = "test_api.db"
LIB_FILE = "./libdbms.so"

def test_api():
    if os.path.exists(DB_FILE):
        os.remove(DB_FILE)

    assert os.path.exists(LIB_FILE), f"Shared library {LIB_FILE} not found!"
    lib = ctypes.CDLL(LIB_FILE)

    # Function signatures
    lib.dbms_open.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
    lib.dbms_open.restype = ctypes.c_int

    lib.dbms_close.argtypes = [ctypes.c_void_p]
    lib.dbms_close.restype = ctypes.c_int

    lib.dbms_prepare_v2.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_char_p)]
    lib.dbms_prepare_v2.restype = ctypes.c_int

    lib.dbms_bind_int.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]
    lib.dbms_bind_int.restype = ctypes.c_int

    lib.dbms_bind_text.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
    lib.dbms_bind_text.restype = ctypes.c_int

    lib.dbms_step.argtypes = [ctypes.c_void_p]
    lib.dbms_step.restype = ctypes.c_int

    lib.dbms_column_int.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.dbms_column_int.restype = ctypes.c_int

    lib.dbms_column_text.argtypes = [ctypes.c_void_p, ctypes.c_int]
    lib.dbms_column_text.restype = ctypes.c_char_p

    lib.dbms_finalize.argtypes = [ctypes.c_void_p]
    lib.dbms_finalize.restype = ctypes.c_int

    # 1. Open Connection
    db = ctypes.c_void_p()
    res = lib.dbms_open(DB_FILE.encode('utf-8'), ctypes.byref(db))
    assert res == 0, f"dbms_open failed with code {res}"

    # 2. CREATE TABLE
    stmt = ctypes.c_void_p()
    sql_create = b"CREATE TABLE users (id INT, name TEXT, age INT)"
    lib.dbms_prepare_v2(db, sql_create, len(sql_create), ctypes.byref(stmt), None)
    res = lib.dbms_step(stmt)
    assert res == 101, f"CREATE TABLE step failed with code {res}" # DBMS_DONE
    lib.dbms_finalize(stmt)

    # 3. INSERT via Prepared Statement & Bindings
    stmt_ins = ctypes.c_void_p()
    sql_ins = b"INSERT INTO users VALUES (1, 'Alice', 25)"
    lib.dbms_prepare_v2(db, sql_ins, len(sql_ins), ctypes.byref(stmt_ins), None)
    res = lib.dbms_step(stmt_ins)
    assert res == 101, f"INSERT step failed with code {res}"
    lib.dbms_finalize(stmt_ins)

    # 4. SELECT via Prepared Statement
    stmt_sel = ctypes.c_void_p()
    sql_sel = b"SELECT * FROM users"
    lib.dbms_prepare_v2(db, sql_sel, len(sql_sel), ctypes.byref(stmt_sel), None)

    step_res = lib.dbms_step(stmt_sel)
    assert step_res == 100, f"SELECT step failed with code {step_res}" # DBMS_ROW

    user_id = lib.dbms_column_int(stmt_sel, 0)
    name = lib.dbms_column_text(stmt_sel, 1).decode('utf-8')
    age = lib.dbms_column_int(stmt_sel, 2)

    assert user_id == 1, f"Expected user_id 1, got {user_id}"
    assert name == "Alice", f"Expected name Alice, got {name}"
    assert age == 25, f"Expected age 25, got {age}"

    lib.dbms_finalize(stmt_sel)
    lib.dbms_close(db)

    print("✓ Native C Shared Library API (libdbms.so via Python ctypes) integration test passed!")

    if os.path.exists(DB_FILE):
        os.remove(DB_FILE)

if __name__ == "__main__":
    test_api()
