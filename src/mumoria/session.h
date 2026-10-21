// Mumoria - a connected player.

#pragma once

#include "world.h"

#include <string>

struct MuParty;
struct MuTrade;

// What muGetKey() returns when something happened that the player's screen
// must act on (a trade opened, a screen's other side changed). ^] is unused.
#define MU_KEY_EVENT 29

struct MuSession {
    MuConn *conn = nullptr;
    MuAccount account{};
    int64_t char_id = 0;
    std::string char_name;

    MuPlayerCtx ctx;
    MuLevel *level = nullptr;
    bool in_game = false;
    bool closing = false; // saving on the way out: prompts answer themselves

    int safe_depth = 0;     // inside MuSafePrompt
    bool wake_on_poke = false; // a screen that redraws when others change it
    bool event_pending = false; // set by others: handle at the main prompt
    bool at_top = false;    // waiting at the main command prompt
    bool did_act = false;   // took a turn since the lock was last given back
    bool view_stale = false;
    time_t last_save = 0, last_key = 0;

    // set by others, read by the session's own thread
    bool kicked = false;
    std::string kick_reason;
    int64_t pending_exp = 0;

    MuParty *party = nullptr;
    int64_t invited_by = 0;  // account that invited us to a party
    int64_t trade_asked = 0; // account we asked to trade
    int pending_depth = -1;  // /goto, /summon: the level to go to
    std::string reply_to;
    time_t chat_window = 0;
    int chat_count = 0;
    MuTrade *trade = nullptr;
    int64_t guild_id = 0;
    int guild_rank = 0;
    MuSession *arrive_near = nullptr; // /goto, /summon: arrive beside them
};

// The session of the calling thread.
extern thread_local MuSession *mu_self;

void mu_session_run(MuConn *conn);

// Every session that has logged in (in the game or not), under mu_sessions_lock.
extern pthread_mutex_t mu_sessions_lock;
extern std::vector<MuSession *> mu_sessions;
MuSession *mu_find_online(const char *name); // by character or account name; lock held

void mu_system(MuSession *s, const std::string &line);      // a line in s's chat pane
void mu_system_all(const std::string &line);                 // to everyone
