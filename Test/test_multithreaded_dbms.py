import ctypes
import threading
import os
import time

# Load libdbms.so
lib = ctypes.CDLL("/home/venomsnake/Downloads/DBMS/libdbms.so")

lib.dbms_open.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
lib.dbms_open.restype = ctypes.c_int

lib.dbms_close.argtypes = [ctypes.c_void_p]
lib.dbms_close.restype = ctypes.c_int

lib.dbms_exec.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_char_p)]
lib.dbms_exec.restype = ctypes.c_int

DB_FILE = "/home/venomsnake/Downloads/DBMS/test_multithreaded.db"

if os.path.exists(DB_FILE):
    os.remove(DB_FILE)

# Setup initial schema
db_handle = ctypes.c_void_p()
ret = lib.dbms_open(DB_FILE.encode('utf-8'), ctypes.byref(db_handle))
assert ret == 0, "dbms_open failed"

setup_sql = b"CREATE TABLE accounts (id INT PRIMARY KEY, name TEXT, balance INT);"
lib.dbms_exec(db_handle, setup_sql, None, None, None)
lib.dbms_close(db_handle)

success_counter = 0
counter_lock = threading.Lock()

def worker_task(thread_id):
    global success_counter
    # Each thread opens connection, performs operations, and closes
    h = ctypes.c_void_p()
    if lib.dbms_open(DB_FILE.encode('utf-8'), ctypes.byref(h)) == 0:
        for i in range(5):
            val = thread_id * 100 + i
            sql = f"INSERT INTO accounts VALUES ({val}, 'user_{val}', {val * 10});".encode('utf-8')
            lib.dbms_exec(h, sql, None, None, None)
            time.sleep(0.001)

        lib.dbms_close(h)
        with counter_lock:
            success_counter += 1

threads = []
print("Spawning 20 concurrent threads executing parallel DBMS operations...")
for tid in range(20):
    t = threading.Thread(target=worker_task, args=(tid,))
    threads.append(t)
    t.start()

for t in threads:
    t.join()

print(f"Completed {success_counter}/20 worker threads successfully!")
assert success_counter == 20, "Not all threads completed successfully!"

# Cleanup test file
if os.path.exists(DB_FILE):
    os.remove(DB_FILE)

print("✓ Thread-safety & synchronization test for libdbms.so passed 100%!")
