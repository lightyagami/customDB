# Universal Native C API & Cross-Platform Specification (`libdbms`)

This document outlines the architectural specification and implementation plan for transforming our DBMS into a **universal, multi-platform, cross-language embedded database engine library** (`libdbms.so` / `libdbms.dylib` / `dbms.dll`).

---

## 1. Cross-Platform & Cross-Architecture Target Matrix

`libdbms` is designed to be compiled into a single lightweight shared library across all major operating systems and CPU architectures:

| Operating System | Binary Output | Architectures Supported | Compiler Chain |
| :--- | :--- | :--- | :--- |
| **Linux** | `libdbms.so` | `x86_64`, `aarch64` (ARM64), `riscv64` | `gcc`, `clang` |
| **macOS** | `libdbms.dylib` | `arm64` (Apple Silicon M1/M2/M3/M4), `x86_64` (Intel) | `clang` (Xcode CLI tools) |
| **Windows** | `dbms.dll` | `x86_64` (AMD64), `arm64` | `MSVC`, `MinGW-w64` |
| **WebAssembly** | `dbms.wasm` | Web Browsers & Edge Runtimes (Node, Deno, Bun) | `Emscripten` (`emcc`) |

---

## 2. Universal C API Header Interface (`include/dbms.h`)

The library header provides a clean, thread-safe, POSIX/Win32 compliant interface using standard `C89/C99` ABI:

```c
#ifndef DBMS_H
#define DBMS_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
  #ifdef DBMS_BUILD_DLL
    #define DBMS_API __declspec(dllexport)
  #else
    #define DBMS_API __declspec(dllimport)
  #endif
#else
  #define DBMS_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ── Return Status Codes ──────────────────────────────────────────────────── */
#define DBMS_OK           0   /* Successful result */
#define DBMS_ERROR        1   /* SQL error or missing table */
#define DBMS_BUSY         5   /* Database engine file locked */
#define DBMS_MISUSE      21   /* Invalid API call sequencing */
#define DBMS_ROW         100  /* dbms_step() has another row ready */
#define DBMS_DONE        101  /* dbms_step() has finished executing */

/* ── Opaque Connection & Statement Handles ────────────────────────────────── */
typedef struct dbms dbms;
typedef struct dbms_stmt dbms_stmt;

/* ── Connection Management ────────────────────────────────────────────────── */
DBMS_API int dbms_open(const char* filename, dbms** ppDb);
DBMS_API int dbms_close(dbms* pDb);
DBMS_API const char* dbms_errmsg(dbms* pDb);

/* ── Direct Execution ─────────────────────────────────────────────────────── */
DBMS_API int dbms_exec(
    dbms* pDb,
    const char* sql,
    int (*callback)(void* param, int num_cols, char** col_vals, char** col_names),
    void* arg,
    char** errmsg
);

/* ── Prepared Statement API ───────────────────────────────────────────────── */
DBMS_API int dbms_prepare_v2(dbms* pDb, const char* zSql, int nByte, dbms_stmt** ppStmt, const char** pzTail);
DBMS_API int dbms_step(dbms_stmt* pStmt);
DBMS_API int dbms_reset(dbms_stmt* pStmt);
DBMS_API int dbms_finalize(dbms_stmt* pStmt);

/* ── Parameter Binding Functions ─────────────────────────────────────────── */
DBMS_API int dbms_bind_int(dbms_stmt* pStmt, int index, int value);
DBMS_API int dbms_bind_double(dbms_stmt* pStmt, int index, double value);
DBMS_API int dbms_bind_text(dbms_stmt* pStmt, int index, const char* text, int len);
DBMS_API int dbms_bind_blob(dbms_stmt* pStmt, int index, const void* blob, int len);
DBMS_API int dbms_bind_null(dbms_stmt* pStmt, int index);

/* ── Column Result Extraction Functions ──────────────────────────────────── */
DBMS_API int         dbms_column_count(dbms_stmt* pStmt);
DBMS_API const char* dbms_column_name(dbms_stmt* pStmt, int col);
DBMS_API int         dbms_column_type(dbms_stmt* pStmt, int col);
DBMS_API int         dbms_column_int(dbms_stmt* pStmt, int col);
DBMS_API double      dbms_column_double(dbms_stmt* pStmt, int col);
DBMS_API const char* dbms_column_text(dbms_stmt* pStmt, int col);
DBMS_API const void* dbms_column_blob(dbms_stmt* pStmt, int col);
DBMS_API int         dbms_column_bytes(dbms_stmt* pStmt, int col);

/* ── Utilities ────────────────────────────────────────────────────────────── */
DBMS_API int64_t dbms_last_insert_rowid(dbms* pDb);
DBMS_API int     dbms_changes(dbms* pDb);

#ifdef __cplusplus
}
#endif

#endif /* DBMS_H */
```

---

## 3. Multi-Language Binding Examples (FFI)

Because `libdbms` exports a standard C ABI, it can be loaded natively into any programming language:

###  Python (`ctypes` / `cffi`)
```python
import ctypes

# Load native shared library
lib = ctypes.CDLL("./libdbms.so") # or dbms.dll / libdbms.dylib

# Open database connection
db = ctypes.c_void_p()
lib.dbms_open(b"app.db", ctypes.byref(db))

# Prepare and execute statement
stmt = ctypes.c_void_p()
lib.dbms_prepare_v2(db, b"SELECT * FROM users WHERE age > ?", -1, ctypes.byref(stmt), None)
lib.dbms_bind_int(stmt, 1, 21)

while lib.dbms_step(stmt) == 100: # DBMS_ROW
    user_id = lib.dbms_column_int(stmt, 0)
    name = ctypes.string_at(lib.dbms_column_text(stmt, 1)).decode('utf-8')
    print(f"User: {user_id}, Name: {name}")

lib.dbms_finalize(stmt)
lib.dbms_close(db)
```

### ⚡ Node.js / JavaScript (N-API / `ffi-napi`)
```javascript
const ffi = require('ffi-napi');
const ref = require('ref-napi');

const dbPtr = ref.refType(ref.types.void);
const stmtPtr = ref.refType(ref.types.void);

const lib = ffi.Library('./libdbms.so', {
  'dbms_open': ['int', ['string', dbPtr]],
  'dbms_prepare_v2': ['int', [dbPtr, 'string', 'int', stmtPtr, 'pointer']],
  'dbms_step': ['int', [stmtPtr]],
  'dbms_column_text': ['string', [stmtPtr, 'int']],
  'dbms_close': ['int', [dbPtr]]
});

// Usage
const db = ref.alloc(dbPtr);
lib.dbms_open("web_app.db", db);
```

### 🦀 Rust (`bindgen` / FFI)
```rust
extern "C" {
    fn dbms_open(filename: *const i8, db: *mut *mut std::ffi::c_void) -> i32;
    fn dbms_close(db: *mut std::ffi::c_void) -> i32;
}

fn main() {
    unsafe {
        let mut db: *mut std::ffi::c_void = std::ptr::null_mut();
        let filename = std::ffi::CString::new("rust_app.db").unwrap();
        dbms_open(filename.as_ptr(), &mut db);
        dbms_close(db);
    }
}
```

### 💙 C# / .NET Core (`P/Invoke`)
```csharp
using System;
using System.Runtime.InteropServices;

class Program {
    [DllImport("dbms.dll", CallingConvention = CallingConvention.Cdecl)]
    public static extern int dbms_open(string filename, out IntPtr db);

    [DllImport("dbms.dll", CallingConvention = CallingConvention.Cdecl)]
    public static extern int dbms_close(IntPtr db);

    static void Main() {
        IntPtr db;
        dbms_open("dotnet_app.db", out db);
        dbms_close(db);
    }
}
```

---

## 4. Cross-Platform Compilation Commands

### 🐧 Building on Linux (x86_64 / ARM64)
```bash
gcc -shared -fPIC -O3 -Iinclude src/*.c -o libdbms.so
```

### 🍎 Building on macOS (Apple Silicon M1/M2/M3 & Intel)
```bash
# Universal binary (Apple Silicon + Intel)
clang -dynamiclib -O3 -arch arm64 -arch x86_64 -Iinclude src/*.c -o libdbms.dylib
```

### 🪟 Building on Windows (MinGW-w64 or MSVC)
```cmd
gcc -shared -O3 -DDBMS_BUILD_DLL -Iinclude src/*.c -o dbms.dll -Wl,--out-implib,libdbms.a
```

### 🌐 Building for WebAssembly (Web Browsers)
```bash
emcc -O3 -s WASM=1 -s EXPORTED_FUNCTIONS="['_dbms_open','_dbms_prepare_v2','_dbms_step','_dbms_close']" -Iinclude src/*.c -o dbms.js
```
