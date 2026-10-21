// Mumoria - the part of the SQLite C API the server uses.
//
// The build machine has the SQLite library (libsqlite3.so.0) but not its
// development header. These declarations are copied from sqlite3.h, whose C
// API is stable; with the real header installed, it is used instead.

#pragma once

#if defined(__has_include)
#if __has_include(<sqlite3.h>)
#include <sqlite3.h>
#define MU_HAVE_SQLITE_HEADER 1
#endif
#endif

#ifndef MU_HAVE_SQLITE_HEADER

#ifdef __cplusplus
extern "C" {
#endif

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
typedef long long sqlite3_int64;
typedef void (*sqlite3_destructor_type)(void *);

#define SQLITE_OK 0
#define SQLITE_BUSY 5
#define SQLITE_ROW 100
#define SQLITE_DONE 101
#define SQLITE_NULL 5
#define SQLITE_TRANSIENT ((sqlite3_destructor_type) -1)

int sqlite3_open(const char *filename, sqlite3 **db);
int sqlite3_close(sqlite3 *db);
int sqlite3_exec(sqlite3 *db, const char *sql, int (*callback)(void *, int, char **, char **), void *arg, char **errmsg);
int sqlite3_busy_timeout(sqlite3 *db, int ms);
const char *sqlite3_errmsg(sqlite3 *db);
void sqlite3_free(void *p);
int sqlite3_prepare_v2(sqlite3 *db, const char *sql, int nbyte, sqlite3_stmt **stmt, const char **tail);
int sqlite3_bind_int64(sqlite3_stmt *stmt, int idx, sqlite3_int64 value);
int sqlite3_bind_text(sqlite3_stmt *stmt, int idx, const char *value, int n, sqlite3_destructor_type d);
int sqlite3_bind_blob(sqlite3_stmt *stmt, int idx, const void *value, int n, sqlite3_destructor_type d);
int sqlite3_bind_null(sqlite3_stmt *stmt, int idx);
int sqlite3_step(sqlite3_stmt *stmt);
int sqlite3_finalize(sqlite3_stmt *stmt);
int sqlite3_column_type(sqlite3_stmt *stmt, int col);
sqlite3_int64 sqlite3_column_int64(sqlite3_stmt *stmt, int col);
const unsigned char *sqlite3_column_text(sqlite3_stmt *stmt, int col);
const void *sqlite3_column_blob(sqlite3_stmt *stmt, int col);
int sqlite3_column_bytes(sqlite3_stmt *stmt, int col);
int sqlite3_changes(sqlite3 *db);
sqlite3_int64 sqlite3_last_insert_rowid(sqlite3 *db);

#ifdef __cplusplus
}
#endif

#endif
