// Mumoria - a connected player: login, the character menu, and the engine's
// input hooks (how a player's thread gives the world to others).

#include "session.h"

#include "auth.h"
#include "mu_config.h"
#include "social.h"

#include <sched.h>
#include <unistd.h>

thread_local MuSession *mu_self = nullptr;
pthread_mutex_t mu_sessions_lock = PTHREAD_MUTEX_INITIALIZER;
std::vector<MuSession *> mu_sessions;

extern const char *mu_aol_secret; // server_main.cpp

static void record_death(MuSession *s);

// --- screen helpers before the game (no world lock: the engine is not used) ----

static void put(int y, int x, const std::string &text) {
    mu_vt_move(mu_vt_current, y, x);
    mu_vt_clrtoeol(mu_vt_current);
    mu_vt_addstr(mu_vt_current, text.c_str());
}

static void blank() {
    mu_vt_clear(mu_vt_current);
}

[[noreturn]] static void hang_up(MuSession *s) {
    throw MuSessionEnd{s->kicked ? MuSessionEnd::Kicked : MuSessionEnd::Disconnect, s->kick_reason};
}

static int key(MuSession *s) {
    mu_vt_flush(mu_vt_current);
    int k = mu_conn_key(s->conn, MU_IDLE_SECONDS * 1000, 0);
    if (k < 0) {
        hang_up(s);
    }
    return k;
}

// A line typed at (y, x); false on ESC. `hide` draws stars.
static bool read_line(MuSession *s, int y, int x, std::string &out, size_t max, bool hide) {
    out.clear();
    for (;;) {
        mu_vt_move(mu_vt_current, y, x);
        mu_vt_clrtoeol(mu_vt_current);
        mu_vt_addstr(mu_vt_current, hide ? std::string(out.size(), '*').c_str() : out.c_str());
        int k = key(s);
        if (k == 27) {
            return false;
        }
        if (k == '\r') {
            return true;
        }
        if ((k == 8 || k == 127) && !out.empty()) {
            out.pop_back();
        } else if (k >= 32 && k < 127 && out.size() < max) {
            out.push_back((char) k);
        }
    }
}

static void title() {
    blank();
    put(1, 20, "M U M O R I A");
    put(2, 12, "The Dungeons of Moria, together");
    put(4, 4, "Umoria 5.7 by Koeneke, Wilson and the Umoria team. Mumoria (c) 2026.");
}

// --- login ----------------------------------------------------------------

static bool create_account(MuSession *s) {
    std::string name, pw1, pw2;
    title();
    put(7, 4, "A new account. Names are 3-16 letters, digits or spaces.");
    put(9, 4, "Account name: ");
    if (!read_line(s, 9, 18, name, 16, false)) {
        return false;
    }
    if (!mu_valid_name(name.c_str())) {
        put(15, 4, "That name will not do.");
        return false;
    }
    put(10, 4, "Password:     ");
    if (!read_line(s, 10, 18, pw1, 64, true)) {
        return false;
    }
    put(11, 4, "Again:        ");
    if (!read_line(s, 11, 18, pw2, 64, true)) {
        return false;
    }
    if (pw1 != pw2 || pw1.size() < 6) {
        put(15, 4, pw1 != pw2 ? "The passwords differ." : "Use at least 6 characters.");
        return false;
    }
    put(15, 4, "Creating the account...");
    mu_vt_flush(mu_vt_current);
    int64_t id = mu_db_account_create(name.c_str(), pw1.c_str(), 0);
    if (id <= 0) {
        put(15, 4, "That name is taken.");
        return false;
    }
    mu_db_account_by_id(id, &s->account);
    mu_db_audit(s->account.name, "account-create", s->conn->ip, "");
    return true;
}

static bool login(MuSession *s) {
    for (int attempt = 0; attempt < 5; attempt++) {
        std::string name, pw;
        title();
        put(7, 4, "Type an account name, or \"new\" to make one.");
        put(9, 4, "Account name: ");
        if (attempt > 0) {
            put(15, 4, "Not recognised. Try again.");
        }
        if (!read_line(s, 9, 18, name, 200, false) || name.empty()) {
            continue;
        }
        if (name == "new") {
            if (create_account(s)) {
                return true;
            }
            put(16, 4, "[press a key]");
            key(s);
            continue;
        }
        // the AOL hand-off: the P3OL server signs the screen name
        if (name.rfind("token:", 0) == 0) {
            char who[33];
            if (mu_token_check(name.c_str() + 6, mu_aol_secret, (long) time(nullptr), who, sizeof(who))) {
                if (!mu_db_account_find(who, &s->account)) {
                    uint8_t rnd[16];
                    char pw_hex[33];
                    mu_random_bytes(rnd, sizeof(rnd));
                    mu_hex(rnd, sizeof(rnd), pw_hex); // they log in by token; the password is never shown
                    if (mu_db_account_create(who, pw_hex, 0) <= 0 || !mu_db_account_find(who, &s->account)) {
                        continue;
                    }
                }
                if (s->account.banned_until > time(nullptr)) {
                    put(15, 4, "That account is banned.");
                    key(s);
                    return false;
                }
                mu_db_audit(s->account.name, "login-token", s->conn->ip, "");
                return true;
            }
            continue;
        }
        put(10, 4, "Password:     ");
        if (!read_line(s, 10, 18, pw, 64, true)) {
            continue;
        }
        put(12, 4, "Checking...");
        mu_vt_flush(mu_vt_current);
        int rc = mu_db_login(name.c_str(), pw.c_str(), s->conn->ip, &s->account);
        if (rc == MU_LOGIN_OK) {
            mu_db_audit(s->account.name, "login", s->conn->ip, "");
            return true;
        }
        if (rc == MU_LOGIN_BANNED) {
            put(15, 4, "That account is banned: " + std::string(s->account.ban_reason));
            key(s);
            return false;
        }
        if (rc == MU_LOGIN_THROTTLED) {
            put(15, 4, "Too many tries from your address. Wait ten minutes.");
            key(s);
            return false;
        }
    }
    return false;
}

MuSession *mu_find_online(const char *name) {
    char want[33], have[33];
    mu_fold(name, want, sizeof(want));
    for (MuSession *p : mu_sessions) {
        mu_fold(p->char_name.c_str(), have, sizeof(have));
        if (p->in_game && strcmp(want, have) == 0) {
            return p;
        }
    }
    for (MuSession *p : mu_sessions) {
        mu_fold(p->account.name, have, sizeof(have));
        if (strcmp(want, have) == 0) {
            return p;
        }
    }
    return nullptr;
}

// One session per account: an older one is saved and dropped.
static void take_over(MuSession *s) {
    for (int wait = 0; wait < 100; wait++) {
        bool found = false;
        pthread_mutex_lock(&mu_sessions_lock);
        for (MuSession *p : mu_sessions) {
            if (p != s && p->account.id == s->account.id) {
                found = true;
                if (!p->kicked) {
                    p->kicked = true;
                    p->kick_reason = "logged in from elsewhere";
                    mu_conn_kill(p->conn);
                }
            }
        }
        pthread_mutex_unlock(&mu_sessions_lock);
        if (!found) {
            return;
        }
        usleep(100000);
    }
}

// --- the character menu -----------------------------------------------------------

struct CharRow {
    int64_t id;
    std::string name;
    int level, depth, race, cls;
};

static std::vector<CharRow> characters(MuSession *s) {
    std::vector<CharRow> out;
    MuRows *r = mu_db_query("SELECT id, name, level, depth, race, class FROM character WHERE account = ? AND alive = 1 ORDER BY id", "i",
                            s->account.id);
    while (mu_db_next(r)) {
        out.push_back(CharRow{mu_db_col_int(r, 0), mu_db_col_text(r, 1), (int) mu_db_col_int(r, 2), (int) mu_db_col_int(r, 3),
                              (int) mu_db_col_int(r, 4), (int) mu_db_col_int(r, 5)});
    }
    mu_db_done(r);
    return out;
}

// Returns a character id, 0 for a new one, -1 to quit.
static int64_t character_menu(MuSession *s) {
    for (;;) {
        std::vector<CharRow> rows = characters(s);
        blank();
        put(1, 4, "Mumoria - " + std::string(s->account.name));
        put(3, 4, "Your characters:");
        char line[120], depth[32];
        for (size_t i = 0; i < rows.size(); i++) {
            CharRow const &c = rows[i];
            snprintf(line, sizeof(line), "%c) %-16s level %2d %s %s, %s", (char) ('a' + i), c.name.c_str(), c.level,
                     character_races[c.race].name, classes[c.cls].title, mu_depth_name(c.depth, depth, sizeof(depth)));
            put(5 + (int) i, 6, line);
        }
        if (rows.empty()) {
            put(5, 6, "(none yet)");
        }
        int y = 7 + (int) (rows.size() > 0 ? rows.size() : 1);
        if ((int) rows.size() < MU_MAX_CHARACTERS) {
            put(y, 6, "n) Make a new character");
        }
        put(y + 1, 6, "q) Quit");
        put(y + 3, 4, "Death is permanent; your bank and guild belong to the account.");

        int k = key(s);
        if (k == 'q' || k == 'Q' || k == 27) {
            return -1;
        }
        if ((k == 'n' || k == 'N') && (int) rows.size() < MU_MAX_CHARACTERS) {
            return 0;
        }
        if (k >= 'a' && k < 'a' + (int) rows.size()) {
            return rows[(size_t) (k - 'a')].id;
        }
    }
}

// Creation ends with Umoria's own name prompt; a name already in use is asked again.
static bool name_character(MuSession *s) {
    for (int tries = 0; tries < 10; tries++) {
        std::string name = py.misc.name;
        char folded[33];
        mu_fold(name.c_str(), folded, sizeof(folded));
        if (mu_valid_name(name.c_str()) &&
            mu_db_int("SELECT COUNT(*) FROM character WHERE folded = ? AND alive = 1", 1, "s", folded) == 0) {
            int64_t now = (int64_t) time(nullptr);
            if (mu_db_run("INSERT INTO character (account, name, folded, race, class, level, depth, created) VALUES (?, ?, ?, ?, ?, 1, 0, ?)",
                          "issiii", s->account.id, name.c_str(), folded, (int64_t) py.misc.race_id, (int64_t) py.misc.class_id, now) == 1) {
                s->char_id = mu_db_int("SELECT id FROM character WHERE folded = ? AND alive = 1", 0, "s", folded);
                s->char_name = name;
                return s->char_id != 0;
            }
        }
        putStringClearToEOL("That name is taken or will not do (3-16 letters). Name:", Coord_t{0, 0});
        char buf[24] = {0};
        if (!getStringInput(buf, Coord_t{0, 57}, 16)) {
            continue;
        }
        snprintf(py.misc.name, sizeof(py.misc.name), "%s", buf);
    }
    return false;
}

static void play(MuSession *s, int64_t char_id) {
    mu_ctx_init(s->ctx);
    s->closing = false;
    s->last_key = time(nullptr);
    s->char_id = 0;
    s->char_name.clear();
    mu_world_lock(s);
    try {
        int depth = 0;
        if (char_id == 0) {
            muEngineNewCharacter();
            if (!name_character(s)) {
                throw MuSessionEnd{MuSessionEnd::Quit, "no name"};
            }
            // PvP is the character's choice, made here; /pvp changes it once a day
            s->pvp = getInputConfirmation("Allow other players to fight this character (PvP)?");
            s->pvp_changed_at = 0;
            mu_db_run("UPDATE character SET pvp = ? WHERE id = ?", "ii", (int64_t) s->pvp, s->char_id);
        } else {
            MuRows *r = mu_db_query("SELECT save, depth, name, pvp, pvp_changed_at FROM character WHERE id = ? AND account = ? AND alive = 1",
                                    "ii", char_id, s->account.id);
            std::vector<uint8_t> blob;
            if (mu_db_next(r)) {
                int len = 0;
                const uint8_t *p = (const uint8_t *) mu_db_col_blob(r, 0, &len);
                if (p != nullptr) {
                    blob.assign(p, p + len);
                }
                depth = (int) mu_db_col_int(r, 1);
                s->char_name = mu_db_col_text(r, 2);
                s->pvp = mu_db_col_int(r, 3) != 0;
                s->pvp_changed_at = mu_db_col_int(r, 4);
            }
            mu_db_done(r);
            playerInitializeBaseExperienceLevels();
            if (blob.empty() || !mu_char_deserialize(blob.data(), (int) blob.size())) {
                throw MuSessionEnd{MuSessionEnd::Quit, "that character could not be read"};
            }
            s->char_id = char_id;
        }

        s->last_save = s->last_key = time(nullptr);
        mu_enter_game(s, depth);
        mu_char_store(s);
        mu_social_entered(s);
        muEnginePlay(); // leaves by an exception: death, quit, hang-up
    } catch (MuSessionEnd &end) {
        // the world lock is held here, with this player loaded. Saving may
        // still reach a -more- prompt: it must not wait or throw again.
        s->closing = true;
        fprintf(stderr, "mumoria: %s left the game (%d %s)\n", s->account.name, (int) end.why, end.text.c_str());
        // a blow from another player still lands when its target hangs up
        if (end.why != MuSessionEnd::Died && s->in_game && s->pending_pvp_damage > 0) {
            py.misc.current_hp -= s->pending_pvp_damage;
            s->pending_pvp_damage = 0;
            if (py.misc.current_hp < 0) {
                game.character_is_dead = true;
                snprintf(game.character_died_from, sizeof(game.character_died_from), "%s", s->pvp_attacker.c_str());
                record_death(s);
            }
        }
        if (end.why != MuSessionEnd::Died && s->in_game && !game.character_is_dead) {
            mu_char_store(s);
        }
        mu_social_left(s, end.why == MuSessionEnd::Died);
        mu_leave_game(s); // also when never in the game: the world forgets this player

        mu_world_unlock(s, false);
        if (end.why == MuSessionEnd::Disconnect || end.why == MuSessionEnd::Kicked) {
            throw;
        }
        if (!end.text.empty() && end.why == MuSessionEnd::Quit && end.text != "exit") {
            blank();
            put(10, 4, end.text);
            put(12, 4, "[press a key]");
            key(s);
        }
    }
}

void mu_session_run(MuConn *conn) {
    auto *s = new MuSession();
    s->conn = conn;
    mu_self = s;
    mu_vt_current = &conn->screen;

    try {
        if (login(s)) {
            take_over(s);
            pthread_mutex_lock(&mu_sessions_lock);
            mu_sessions.push_back(s);
            pthread_mutex_unlock(&mu_sessions_lock);

            for (;;) {
                int64_t id = character_menu(s);
                if (id < 0) {
                    break;
                }
                play(s, id);
            }
        }
    } catch (MuSessionEnd &) {
        // hung up or kicked; the game (if any) is already saved
    }

    blank();
    put(10, 4, s->kicked ? "Disconnected: " + s->kick_reason : "Farewell.");
    mu_vt_flush(mu_vt_current);

    pthread_mutex_lock(&mu_sessions_lock);
    mu_sessions.erase(std::remove(mu_sessions.begin(), mu_sessions.end(), s), mu_sessions.end());
    pthread_mutex_unlock(&mu_sessions_lock);
    mu_conn_kill(conn);
    delete s;
    mu_self = nullptr;
}

void mu_system(MuSession *s, const std::string &line) {
    mu_vt_chat(&s->conn->screen, line.c_str());
}

void mu_system_all(const std::string &line) {
    pthread_mutex_lock(&mu_sessions_lock);
    for (MuSession *p : mu_sessions) {
        mu_vt_chat(&p->conn->screen, line.c_str());
    }
    pthread_mutex_unlock(&mu_sessions_lock);
}

// --- the engine's hooks -------------------------------------------------------

MuSafePrompt::MuSafePrompt() {
    if (mu_self != nullptr) {
        mu_self->safe_depth++;
    }
}

MuSafePrompt::~MuSafePrompt() {
    if (mu_self != nullptr) {
        mu_self->safe_depth--;
    }
}

MuTopPrompt::MuTopPrompt() {
    if (mu_self != nullptr) {
        mu_self->safe_depth++;
        mu_self->at_top = true;
    }
}

MuTopPrompt::~MuTopPrompt() {
    if (mu_self != nullptr) {
        mu_self->safe_depth--;
        mu_self->at_top = false;
    }
}

// The map again, after others moved (lock held, at the main prompt).
static void refresh_view(MuSession *s) {
    s->view_stale = false;
    updateMonsters(false);
    drawDungeonPanel();
    panelMoveCursor(py.pos);
    putQIO();
}

// Experience and boss kills others credited to this player, applied when it
// can be shown (lock held, at the main prompt).
static void apply_pending(MuSession *s) {
    if (s->pending_exp > 0) {
        py.misc.exp += (int32_t) s->pending_exp;
        s->pending_exp = 0;
        displayCharacterExperience();
    }
    if (s->gold_dirty) {
        s->gold_dirty = false;
        printCharacterGoldValue();
    }
}

int muGetKey() {
    MuSession *s = mu_self;
    if (s->closing) {
        return ESCAPE;
    }
    // struck by a player, or asked into a trade, while busy elsewhere
    if (s->at_top && (s->event_pending || s->pending_pvp_damage > 0)) {
        return MU_KEY_EVENT;
    }
    bool safe = s->safe_depth > 0 || s->level == nullptr;
    long waited_ms = 0;

    if (s->at_top && s->level != nullptr) {
        apply_pending(s);
        if (s->view_stale) {
            refresh_view(s);
        }
        // a turn-based battle: wait for the others in it to take their turns
        time_t since = time(nullptr);
        bool said = false;
        for (MuSession *w = mu_round_waits_for(s); w != nullptr; w = mu_round_waits_for(s)) {
            putStringClearToEOL("Waiting for " + w->char_name + " to take their turn...", Coord_t{0, 0});
            putQIO();
            said = true;
            mu_world_unlock(s, false);
            int r = mu_conn_wait(s->conn, 500);
            mu_world_lock(s);
            if (r == MU_KEY_CLOSED) {
                hang_up(s);
            }
            if (s->view_stale) {
                refresh_view(s);
            }
            if (time(nullptr) - since >= MU_ROUND_TIMEOUT) {
                mu_round_skip(w);
                since = time(nullptr);
            }
        }
        if (said) {
            messageLineClear();
            putQIO();
        }
    }
    mu_world_unlock(s, !safe);

    int k;
    for (;;) {
        k = mu_conn_key(s->conn, 250, s->at_top || s->wake_on_poke);
        if (k >= 0) {
            break;
        }
        if (k == MU_KEY_CLOSED) {
            mu_world_lock(s);
            hang_up(s);
        }
        if (k == MU_KEY_POKED) {
            if (s->wake_on_poke) {
                k = MU_KEY_EVENT; // a screen waiting on someone else redraws
                break;
            }
            if (s->event_pending) {
                k = MU_KEY_EVENT;
                break;
            }
            if (s->level != nullptr) {
                mu_world_lock(s);
                refresh_view(s);
                apply_pending(s);
                mu_world_unlock(s, false);
            }
            continue;
        }

        waited_ms += 250;
        time_t now = time(nullptr);
        // stopped mid-action while others on the level wait: carry on for
        // them - at once, unless this is a turn-based group's battle. What
        // the message line said is kept in the chat pane.
        long hold_ms = mu_turn_based(s) ? MU_BUSY_TIMEOUT_MS : 0;
        if (!safe && s->level != nullptr && waited_ms >= hold_ms && mu_level_waiters(s->level) > 0) {
            std::string said(mu_vt_current->cell[0], MU_COLS);
            said.erase(said.find_last_not_of(' ') + 1);
            if (!said.empty()) {
                mu_system(s, said);
            }
            k = ESCAPE;
            break;
        }
        if (s->at_top && s->level != nullptr && now - s->last_save >= MU_AUTOSAVE_SECONDS) {
            mu_world_lock(s);
            mu_char_store(s);
            mu_world_unlock(s, false);
        }
        if (now - s->last_key >= MU_IDLE_SECONDS) {
            s->kicked = true;
            s->kick_reason = "idle too long";
            mu_world_lock(s);
            hang_up(s);
        }
    }

    mu_world_lock(s);
    s->last_key = time(nullptr);
    return k;
}

bool muPollKey() {
    MuSession *s = mu_self;
    if (s->closing) {
        return false;
    }
    // between two turns of a run or a rest: a safe point to let others play
    mu_world_unlock(s, false);
    sched_yield();
    mu_world_lock(s);
    if (!mu_conn_has_key(s->conn)) {
        return false;
    }
    int k = mu_conn_key(s->conn, 0, 0);
    if (k == MU_KEY_CLOSED) {
        hang_up(s);
    }
    return true;
}

void muBell() {
    mu_vt_bell(mu_vt_current);
}

void muExitProgram(const char *why) {
    throw MuSessionEnd{MuSessionEnd::Quit, why != nullptr ? why : ""};
}

// The graveyard, the character gone, and the news (lock held, s loaded).
static void record_death(MuSession *s) {
    int depth = s->level != nullptr ? s->level->depth : 0;
    const char *how = game.total_winner ? "retired a Total Winner" : game.character_died_from;
    mu_db_run("INSERT INTO graveyard (account, name, race, class, level, depth, exp, died_from, died_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)",
              "isiiiiisi", s->account.id, s->char_name.c_str(), (int64_t) py.misc.race_id, (int64_t) py.misc.class_id,
              (int64_t) py.misc.level, (int64_t) depth, (int64_t) py.misc.max_exp, how, (int64_t) time(nullptr));
    mu_db_run("UPDATE character SET alive = 0, save = NULL WHERE id = ?", "i", s->char_id);
    char where[32], line[160];
    snprintf(line, sizeof(line), "%s the level %d %s %s %s at %s.", s->char_name.c_str(), py.misc.level, classes[py.misc.class_id].title,
             game.total_winner ? "has" : "was killed by", game.total_winner ? "won" : game.character_died_from,
             mu_depth_name(depth, where, sizeof(where)));
    mu_system_all(line);
    fprintf(stderr, "mumoria: %s\n", line);
}

void muEndGame() {
    record_death(mu_self);
    throw MuSessionEnd{MuSessionEnd::Died, ""};
}

void muTurnDone() {
    mu_self->did_act = true;
    mu_round_acted(mu_self);
}

void muNewLevel() {
    mu_change_level(mu_self);
}

bool muWizardAllowed() {
    if (mu_self->account.role < 2) {
        return false;
    }
    mu_db_audit(mu_self->account.name, "wizard-mode", mu_self->char_name.c_str(), "");
    return true;
}
