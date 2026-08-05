#ifndef DBMS_H
#define DBMS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

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
