// Mumoria - the database. See db.h; the schema is SCHEMA_V1 below.

#include "db.h"

#include "auth.h"
#include "sqlite3_min.h"

#include <ctype.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static sqlite3 *db = NULL;
static pthread_mutex_t db_lock;

struct MuRows {
    sqlite3_stmt *stmt;
};

// Items are Umoria's Inventory_t as a blob; `item` text columns hold the
// name as described when it was stored, for listings and the ledger.
static const char *SCHEMA_V1 =
    "CREATE TABLE IF NOT EXISTS account ("
    " id INTEGER PRIMARY KEY, name TEXT NOT NULL, folded TEXT NOT NULL UNIQUE,"
    " pw_hash TEXT NOT NULL, salt TEXT NOT NULL, role INTEGER NOT NULL DEFAULT 0,"
    " created INTEGER NOT NULL, last_login INTEGER NOT NULL DEFAULT 0,"
    " banned_until INTEGER NOT NULL DEFAULT 0, ban_reason TEXT NOT NULL DEFAULT '',"
    " muted_until INTEGER NOT NULL DEFAULT 0);"
    "CREATE TABLE IF NOT EXISTS login_attempt (ip TEXT NOT NULL, at INTEGER NOT NULL);"
    "CREATE INDEX IF NOT EXISTS login_attempt_ip ON login_attempt(ip, at);"
    "CREATE TABLE IF NOT EXISTS character ("
    " id INTEGER PRIMARY KEY, account INTEGER NOT NULL, name TEXT NOT NULL, folded TEXT NOT NULL,"
    " race INTEGER NOT NULL DEFAULT 0, class INTEGER NOT NULL DEFAULT 0, level INTEGER NOT NULL DEFAULT 1,"
    " depth INTEGER NOT NULL DEFAULT 0, alive INTEGER NOT NULL DEFAULT 1, save BLOB,"
    " boss_kills INTEGER NOT NULL DEFAULT 0, created INTEGER NOT NULL, saved_at INTEGER NOT NULL DEFAULT 0);"
    "CREATE UNIQUE INDEX IF NOT EXISTS character_name ON character(folded) WHERE alive = 1;"
    "CREATE INDEX IF NOT EXISTS character_account ON character(account);"
    "CREATE TABLE IF NOT EXISTS graveyard ("
    " id INTEGER PRIMARY KEY, account INTEGER NOT NULL, name TEXT NOT NULL, race INTEGER, class INTEGER,"
    " level INTEGER, depth INTEGER, exp INTEGER, died_from TEXT, died_at INTEGER NOT NULL);"
    "CREATE TABLE IF NOT EXISTS level ("
    " depth INTEGER PRIMARY KEY, blob BLOB NOT NULL, created_at INTEGER NOT NULL, saved_at INTEGER NOT NULL,"
    " boss_dead_at INTEGER NOT NULL DEFAULT 0);"
    "CREATE TABLE IF NOT EXISTS world (key TEXT PRIMARY KEY, num INTEGER, blob BLOB);"
    "CREATE TABLE IF NOT EXISTS bank (account INTEGER PRIMARY KEY, gold INTEGER NOT NULL DEFAULT 0 CHECK (gold >= 0));"
    "CREATE TABLE IF NOT EXISTS bank_item ("
    " id INTEGER PRIMARY KEY, account INTEGER NOT NULL, blob BLOB NOT NULL, item TEXT NOT NULL, at INTEGER NOT NULL);"
    "CREATE INDEX IF NOT EXISTS bank_item_account ON bank_item(account);"
    "CREATE TABLE IF NOT EXISTS guild ("
    " id INTEGER PRIMARY KEY, name TEXT NOT NULL, folded TEXT NOT NULL UNIQUE, tag TEXT NOT NULL DEFAULT '',"
    " leader INTEGER NOT NULL, motd TEXT NOT NULL DEFAULT '', gold INTEGER NOT NULL DEFAULT 0 CHECK (gold >= 0),"
    " created INTEGER NOT NULL);"
    "CREATE TABLE IF NOT EXISTS guild_rank ("
    " guild INTEGER NOT NULL, rank INTEGER NOT NULL, title TEXT NOT NULL, perms INTEGER NOT NULL,"
    " PRIMARY KEY (guild, rank));"
    "CREATE TABLE IF NOT EXISTS guild_member ("
    " account INTEGER PRIMARY KEY, guild INTEGER NOT NULL, rank INTEGER NOT NULL, joined INTEGER NOT NULL);"
    "CREATE INDEX IF NOT EXISTS guild_member_guild ON guild_member(guild);"
    "CREATE TABLE IF NOT EXISTS guild_invite ("
    " guild INTEGER NOT NULL, account INTEGER NOT NULL, by TEXT NOT NULL, at INTEGER NOT NULL,"
    " PRIMARY KEY (guild, account));"
    "CREATE TABLE IF NOT EXISTS guild_vault ("
    " id INTEGER PRIMARY KEY, guild INTEGER NOT NULL, blob BLOB NOT NULL, item TEXT NOT NULL,"
    " deposited_by TEXT NOT NULL, at INTEGER NOT NULL);"
    "CREATE INDEX IF NOT EXISTS guild_vault_guild ON guild_vault(guild);"
    "CREATE TABLE IF NOT EXISTS auction ("
    " id INTEGER PRIMARY KEY, seller INTEGER NOT NULL, seller_name TEXT NOT NULL, blob BLOB NOT NULL,"
    " item TEXT NOT NULL, start INTEGER NOT NULL, buyout INTEGER NOT NULL DEFAULT 0,"
    " high_bid INTEGER NOT NULL DEFAULT 0, bidder INTEGER NOT NULL DEFAULT 0, bidder_name TEXT NOT NULL DEFAULT '',"
    " ends_at INTEGER NOT NULL, state INTEGER NOT NULL DEFAULT 0);"
    "CREATE INDEX IF NOT EXISTS auction_open ON auction(state, ends_at);"
    "CREATE TABLE IF NOT EXISTS claim ("
    " id INTEGER PRIMARY KEY, account INTEGER NOT NULL, blob BLOB, item TEXT NOT NULL DEFAULT '',"
    " gold INTEGER NOT NULL DEFAULT 0, reason TEXT NOT NULL, at INTEGER NOT NULL);"
    "CREATE INDEX IF NOT EXISTS claim_account ON claim(account);"
    "CREATE TABLE IF NOT EXISTS ledger ("
    " id INTEGER PRIMARY KEY, at INTEGER NOT NULL, account INTEGER NOT NULL, who TEXT NOT NULL,"
    " kind TEXT NOT NULL, gold INTEGER NOT NULL DEFAULT 0, item TEXT NOT NULL DEFAULT '', detail TEXT NOT NULL DEFAULT '');"
    "CREATE TABLE IF NOT EXISTS audit ("
    " id INTEGER PRIMARY KEY, at INTEGER NOT NULL, actor TEXT NOT NULL, action TEXT NOT NULL,"
    " target TEXT NOT NULL DEFAULT '', detail TEXT NOT NULL DEFAULT '');";

static int exec(const char *sql) {
    char *err = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "mumoria db: %s\n", err != NULL ? err : "?");
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

int mu_db_open(const char *path) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&db_lock, &attr);

    if (sqlite3_open(path, &db) != SQLITE_OK) {
        fprintf(stderr, "mumoria db: cannot open %s\n", path);
        return -1;
    }
    sqlite3_busy_timeout(db, 5000);
    if (exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA foreign_keys=ON;") != 0) {
        return -1;
    }
    int version = (int) mu_db_int("PRAGMA user_version", 0, "");
    if (version < 1) {
        if (exec("BEGIN") != 0 || exec(SCHEMA_V1) != 0 || exec("PRAGMA user_version = 1") != 0 || exec("COMMIT") != 0) {
            return -1;
        }
    }
    return 0;
}

void mu_db_close(void) {
    if (db != NULL) {
        sqlite3_close(db);
        db = NULL;
    }
}

int mu_db_begin(void) {
    pthread_mutex_lock(&db_lock);
    if (exec("BEGIN IMMEDIATE") != 0) {
        pthread_mutex_unlock(&db_lock);
        return -1;
    }
    return 0;
}

int mu_db_commit(void) {
    int rc = exec("COMMIT");
    if (rc != 0) {
        exec("ROLLBACK");
    }
    pthread_mutex_unlock(&db_lock);
    return rc;
}

void mu_db_rollback(void) {
    exec("ROLLBACK");
    pthread_mutex_unlock(&db_lock);
}

static sqlite3_stmt *prepare(const char *sql, const char *fmt, va_list ap) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "mumoria db: %s in: %s\n", sqlite3_errmsg(db), sql);
        return NULL;
    }
    int idx = 1;
    for (const char *f = fmt; *f != '\0'; f++, idx++) {
        switch (*f) {
            case 'i':
                sqlite3_bind_int64(stmt, idx, va_arg(ap, int64_t));
                break;
            case 's':
                sqlite3_bind_text(stmt, idx, va_arg(ap, const char *), -1, SQLITE_TRANSIENT);
                break;
            case 'b': {
                const void *p = va_arg(ap, const void *);
                int n = va_arg(ap, int);
                sqlite3_bind_blob(stmt, idx, p, n, SQLITE_TRANSIENT);
                break;
            }
            default:
                sqlite3_bind_null(stmt, idx);
                break;
        }
    }
    return stmt;
}

int mu_db_run(const char *sql, const char *fmt, ...) {
    pthread_mutex_lock(&db_lock);
    va_list ap;
    va_start(ap, fmt);
    sqlite3_stmt *stmt = prepare(sql, fmt, ap);
    va_end(ap);
    int result = -1;
    if (stmt != NULL) {
        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE || rc == SQLITE_ROW) {
            result = sqlite3_changes(db);
        } else {
            fprintf(stderr, "mumoria db: %s in: %s\n", sqlite3_errmsg(db), sql);
        }
        sqlite3_finalize(stmt);
    }
    pthread_mutex_unlock(&db_lock);
    return result;
}

int64_t mu_db_int(const char *sql, int64_t dflt, const char *fmt, ...) {
    pthread_mutex_lock(&db_lock);
    va_list ap;
    va_start(ap, fmt);
    sqlite3_stmt *stmt = prepare(sql, fmt, ap);
    va_end(ap);
    int64_t result = dflt;
    if (stmt != NULL) {
        if (sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_type(stmt, 0) != SQLITE_NULL) {
            result = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
    }
    pthread_mutex_unlock(&db_lock);
    return result;
}

int64_t mu_db_last_id(void) {
    return sqlite3_last_insert_rowid(db);
}

MuRows *mu_db_query(const char *sql, const char *fmt, ...) {
    pthread_mutex_lock(&db_lock);
    va_list ap;
    va_start(ap, fmt);
    sqlite3_stmt *stmt = prepare(sql, fmt, ap);
    va_end(ap);
    MuRows *rows = calloc(1, sizeof(MuRows));
    rows->stmt = stmt;
    return rows;
}

int mu_db_next(MuRows *rows) {
    return rows->stmt != NULL && sqlite3_step(rows->stmt) == SQLITE_ROW;
}

int64_t mu_db_col_int(MuRows *rows, int col) {
    return sqlite3_column_int64(rows->stmt, col);
}

const char *mu_db_col_text(MuRows *rows, int col) {
    const unsigned char *t = sqlite3_column_text(rows->stmt, col);
    return t != NULL ? (const char *) t : "";
}

const void *mu_db_col_blob(MuRows *rows, int col, int *len) {
    *len = sqlite3_column_bytes(rows->stmt, col);
    return sqlite3_column_blob(rows->stmt, col);
}

void mu_db_done(MuRows *rows) {
    if (rows->stmt != NULL) {
        sqlite3_finalize(rows->stmt);
    }
    free(rows);
    pthread_mutex_unlock(&db_lock);
}

// --- accounts ---------------------------------------------------------------

void mu_fold(const char *name, char *out, int max) {
    int n = 0;
    for (; *name != '\0' && n < max - 1; name++) {
        if (*name != ' ') {
            out[n++] = (char) tolower((unsigned char) *name);
        }
    }
    out[n] = '\0';
}

int mu_valid_name(const char *name) {
    int len = (int) strlen(name);
    if (len < 3 || len > 16 || name[0] == ' ' || name[len - 1] == ' ') {
        return 0;
    }
    for (const char *p = name; *p != '\0'; p++) {
        if (!isalnum((unsigned char) *p) && *p != ' ') {
            return 0;
        }
    }
    return 1;
}

static void hash_password(const char *password, const uint8_t *salt, char *hex_out) {
    uint8_t hash[MU_HASH_LEN];
    mu_pbkdf2(password, salt, MU_SALT_LEN, MU_PBKDF2_ROUNDS, hash);
    mu_hex(hash, MU_HASH_LEN, hex_out);
}

int64_t mu_db_account_create(const char *name, const char *password, int role) {
    char folded[33], hash_hex[MU_HASH_LEN * 2 + 1], salt_hex[MU_SALT_LEN * 2 + 1];
    uint8_t salt[MU_SALT_LEN];
    mu_fold(name, folded, sizeof(folded));
    if (mu_db_int("SELECT id FROM account WHERE folded = ?", 0, "s", folded) != 0) {
        return 0;
    }
    if (mu_random_bytes(salt, sizeof(salt)) != 0) {
        return -1;
    }
    hash_password(password, salt, hash_hex);
    mu_hex(salt, sizeof(salt), salt_hex);
    if (mu_db_run("INSERT INTO account (name, folded, pw_hash, salt, role, created) VALUES (?, ?, ?, ?, ?, ?)", "ssssii", name,
                  folded, hash_hex, salt_hex, (int64_t) role, (int64_t) time(NULL)) != 1) {
        return 0; // lost a race for the name
    }
    return mu_db_int("SELECT id FROM account WHERE folded = ?", -1, "s", folded);
}

static int read_account(MuRows *r, MuAccount *out) {
    if (!mu_db_next(r)) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    out->id = mu_db_col_int(r, 0);
    snprintf(out->name, sizeof(out->name), "%s", mu_db_col_text(r, 1));
    out->role = (int) mu_db_col_int(r, 2);
    out->banned_until = mu_db_col_int(r, 3);
    snprintf(out->ban_reason, sizeof(out->ban_reason), "%s", mu_db_col_text(r, 4));
    out->muted_until = mu_db_col_int(r, 5);
    return 1;
}

#define ACCOUNT_COLS "id, name, role, banned_until, ban_reason, muted_until"

int mu_db_account_find(const char *name, MuAccount *out) {
    char folded[33];
    mu_fold(name, folded, sizeof(folded));
    MuRows *r = mu_db_query("SELECT " ACCOUNT_COLS " FROM account WHERE folded = ?", "s", folded);
    int found = read_account(r, out);
    mu_db_done(r);
    return found;
}

int mu_db_account_by_id(int64_t id, MuAccount *out) {
    MuRows *r = mu_db_query("SELECT " ACCOUNT_COLS " FROM account WHERE id = ?", "i", id);
    int found = read_account(r, out);
    mu_db_done(r);
    return found;
}

// 5 failures in 10 minutes from one address locks it out for the rest of them.
int mu_db_login(const char *name, const char *password, const char *ip, MuAccount *out) {
    int64_t now = (int64_t) time(NULL);
    mu_db_run("DELETE FROM login_attempt WHERE at < ?", "i", now - 600);
    if (mu_db_int("SELECT COUNT(*) FROM login_attempt WHERE ip = ?", 0, "s", ip) >= 5) {
        return MU_LOGIN_THROTTLED;
    }

    char folded[33], salt_hex[MU_SALT_LEN * 2 + 1] = "", stored[MU_HASH_LEN * 2 + 1] = "";
    mu_fold(name, folded, sizeof(folded));
    MuRows *r = mu_db_query("SELECT salt, pw_hash FROM account WHERE folded = ?", "s", folded);
    int found = mu_db_next(r);
    if (found) {
        snprintf(salt_hex, sizeof(salt_hex), "%s", mu_db_col_text(r, 0));
        snprintf(stored, sizeof(stored), "%s", mu_db_col_text(r, 1));
    }
    mu_db_done(r);

    // hash even for a missing name, so timing does not say which names exist
    uint8_t salt[MU_SALT_LEN] = {0};
    char hash_hex[MU_HASH_LEN * 2 + 1];
    if (found) {
        mu_unhex(salt_hex, salt, sizeof(salt));
    }
    hash_password(password, salt, hash_hex);

    if (!found || !mu_equal((const uint8_t *) hash_hex, (const uint8_t *) stored, MU_HASH_LEN * 2)) {
        mu_db_run("INSERT INTO login_attempt (ip, at) VALUES (?, ?)", "si", ip, now);
        return MU_LOGIN_BAD;
    }
    if (!mu_db_account_find(name, out)) {
        return MU_LOGIN_BAD;
    }
    if (out->banned_until > now) {
        return MU_LOGIN_BANNED;
    }
    mu_db_run("UPDATE account SET last_login = ? WHERE id = ?", "ii", now, out->id);
    return MU_LOGIN_OK;
}

int mu_db_set_password(int64_t account, const char *password) {
    uint8_t salt[MU_SALT_LEN];
    char hash_hex[MU_HASH_LEN * 2 + 1], salt_hex[MU_SALT_LEN * 2 + 1];
    if (mu_random_bytes(salt, sizeof(salt)) != 0) {
        return -1;
    }
    hash_password(password, salt, hash_hex);
    mu_hex(salt, sizeof(salt), salt_hex);
    return mu_db_run("UPDATE account SET pw_hash = ?, salt = ? WHERE id = ?", "ssi", hash_hex, salt_hex, account) == 1 ? 0 : -1;
}

void mu_db_audit(const char *actor, const char *action, const char *target, const char *detail) {
    mu_db_run("INSERT INTO audit (at, actor, action, target, detail) VALUES (?, ?, ?, ?, ?)", "issss", (int64_t) time(NULL), actor,
              action, target != NULL ? target : "", detail != NULL ? detail : "");
}

void mu_db_ledger(int64_t account, const char *who, const char *kind, int64_t gold, const char *item, const char *detail) {
    mu_db_run("INSERT INTO ledger (at, account, who, kind, gold, item, detail) VALUES (?, ?, ?, ?, ?, ?, ?)", "iississ",
              (int64_t) time(NULL), account, who, kind, gold, item != NULL ? item : "", detail != NULL ? detail : "");
}
