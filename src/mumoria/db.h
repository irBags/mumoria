// Mumoria - the database (one SQLite file).
//
// Every call takes the database lock, so sessions, the timer and the IRC
// bridge can share one connection. A transaction holds that lock from
// mu_db_begin() to mu_db_commit()/mu_db_rollback(), so nothing else can slip
// a statement into it. Parameters are bound by a format string:
// 'i' int64, 's' text, 'b' blob (pointer then int length), 'n' null.

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MuRows MuRows;

int mu_db_open(const char *path);
void mu_db_close(void);

int mu_db_begin(void);    // BEGIN IMMEDIATE; 0 on success
int mu_db_commit(void);   // 0 on success (rolls back on failure)
void mu_db_rollback(void);

// Runs a statement; returns the number of rows changed, or -1.
int mu_db_run(const char *sql, const char *fmt, ...);
// First column of the first row, or dflt.
int64_t mu_db_int(const char *sql, int64_t dflt, const char *fmt, ...);
int64_t mu_db_last_id(void);

// Row-by-row reading. Holds the database lock until mu_db_done().
MuRows *mu_db_query(const char *sql, const char *fmt, ...);
int mu_db_next(MuRows *rows); // 1 while there is a row
int64_t mu_db_col_int(MuRows *rows, int col);
const char *mu_db_col_text(MuRows *rows, int col);
const void *mu_db_col_blob(MuRows *rows, int col, int *len);
void mu_db_done(MuRows *rows);

// --- accounts ---------------------------------------------------------------

typedef struct {
    int64_t id;
    char name[33];
    int role; // 0 player, 1 moderator, 2 admin
    int64_t banned_until;
    char ban_reason[128];
    int64_t muted_until;
} MuAccount;

enum { MU_LOGIN_OK = 0, MU_LOGIN_BAD = 1, MU_LOGIN_BANNED = 2, MU_LOGIN_THROTTLED = 3 };

void mu_fold(const char *name, char *out, int max); // lower case, for lookups
int mu_valid_name(const char *name);                 // 3-16 letters, digits, spaces
int64_t mu_db_account_create(const char *name, const char *password, int role); // id, 0 if taken, -1 error
int mu_db_account_find(const char *name, MuAccount *out); // 1 if found
int mu_db_account_by_id(int64_t id, MuAccount *out);
int mu_db_login(const char *name, const char *password, const char *ip, MuAccount *out);
int mu_db_set_password(int64_t account, const char *password);

void mu_db_audit(const char *actor, const char *action, const char *target, const char *detail);
void mu_db_ledger(int64_t account, const char *who, const char *kind, int64_t gold, const char *item, const char *detail);

#ifdef __cplusplus
}
#endif
