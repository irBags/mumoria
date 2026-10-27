// Mumoria - a connected player.

#pragma once

#include "shops.h"
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
    bool guild_turns = false; // the guild runs turn-based battles
    bool acted = false;       // took this battle round's action
    bool turn_shown = false;  // "YOUR TURN!" is on their screen
    int64_t invited_by = 0;  // account that invited us to a party
    int64_t trade_asked = 0; // account we asked to trade
    int pending_depth = -1;  // /goto, /summon: the level to go to
    std::string reply_to;
    time_t chat_window = 0;
    int chat_count = 0;
    MuTrade *trade = nullptr;
    int64_t guild_id = 0;
    int guild_rank = 0;
    int guild_shield = 0;     // xterm colour of the guild's shield
    bool in_building = false; // inside a shop or hall: not drawn, blocks nobody
    bool gold_dirty = false;  // others credited shared gold: redraw it
    int64_t gold_fraction = 0; // shared gold below one piece, in hundredths
    bool pvp = false;
    int64_t pvp_changed_at = 0;
    int pending_pvp_damage = 0; // struck by another player: applied on our turn
    std::string pvp_attacker;
    MuSession *arrive_near = nullptr; // /goto, /summon: arrive beside them
    int pending_raid = -1;   // stepped through a raid gate on this depth (raid.cpp)
    bool to_gate = false;    // leaving a raid: arrive beside its gate
    std::string pending_dungeon; // a script's dungeon to go into (script.cpp mu.enter_dungeon)
    Coord_t arrive_at{-1, -1};   // leaving a script dungeon: arrive here (near it)
    int dungeon_back_depth = 0;  // in a script dungeon: where its way out (and a reconnect) leads
    Coord_t dungeon_back{-1, -1};
    int last_step = -1;          // the square a dungeon's step event last fired for
    int store_in = -1;       // the store this player is shopping in (shops.cpp)
    Store_t store_view{};    // their own view of it, while someone else is loaded
    MuStoreTerms store_terms[MAX_STORES]{};
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
