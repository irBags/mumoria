// Mumoria - the shared world: the lock, the context swap, the levels.
//
// See docs/MUMORIA_DESIGN.md section 1. In short: Umoria's globals hold ONE
// player and ONE level. Each session keeps its own copy of the player's
// globals (MuPlayerCtx) and each level its own copy of the level's (MuLevel).
// Whoever holds the world lock has theirs loaded.

#pragma once

#include "../headers.h"

#include "db.h"
#include "net.h"

#include <algorithm>
#include <string>
#include <vector>

struct MuSession;
struct MuRaid;

struct MuPlayerCtx {
    Player_t py{};
    Game_t game{};
    Panel_t panel{};
    bool generate_new_level = false;
    uint8_t objects_identified[OBJECT_IDENT_SIZE]{};
    Recall_t creature_recall[MON_MAX_CREATURES]{};
    vtype_t messages[MESSAGE_HISTORY_SIZE]{};
    int16_t last_message_id = 0;
    bool message_ready_to_print = false;
    bool screen_has_changed = false;
    int run_prevdir = 0, run_direction = 0;
    int eof = 0;
    bool opt[11]{};
    // What the player remembers of their level, one bit a square: field
    // marks, and (below the town, where Umoria uses it only for what the
    // player has lit) permanent light. Lamp light is not kept: load_player
    // relights everyone's lamp where they stand.
    std::vector<uint8_t> marks, lit;
    int marks_depth = -1;
    // Where this player's screen shows a monster. Umoria's monster.lit means
    // "drawn on the screen", but monsters are the level's: it is saved and
    // set again per player, or a monster that moves leaves its letter behind
    // on the screens of players it was not lit for when they were loaded.
    std::vector<Coord_t> shown;
};

struct MuLevel {
    int depth = 0;
    int16_t height = 0, width = 0;
    int32_t game_turn = 0;
    Tile_t floor[MAX_HEIGHT][MAX_WIDTH]{};
    Monster_t monsters[MON_TOTAL_ALLOCATIONS]{};
    int16_t next_free_monster_id = 0, monster_multiply_total = 0;
    int16_t treasure_current_id = 0;
    Inventory_t treasure[LEVEL_MAX_OBJECTS]{};

    std::vector<MuSession *> players;
    MuSession *busy = nullptr; // a player in the middle of an action here
    int waiters = 0;           // players waiting for `busy`
    time_t created_at = 0, saved_at = 0, empty_since = 0;
    time_t boss_dead_at = 0;
    bool dirty = false;
    MuRaid *raid = nullptr; // a raid instance (raid.cpp): never saved
};

// Thrown to end a session from deep inside the engine.
struct MuSessionEnd {
    enum Why { Disconnect, Quit, Died, Kicked } why;
    std::string text;
};

struct MuWorldSeeds {
    uint32_t magic_seed, town_seed;
};

extern MuWorldSeeds mu_seeds;

void mu_world_init();
void mu_world_shutdown();

// The world lock. s may be null (the timer): then nothing is swapped.
void mu_world_lock(MuSession *s);
// hold_level: the player stopped mid-action; others on that level wait.
void mu_world_unlock(MuSession *s, bool hold_level);

// Runs fn with `other` loaded (lock held by the caller), then swaps back.
// The engine draws on the thread's screen: fn draws on other's.
template <typename F> void mu_with_player(MuSession *self, MuSession *other, F fn);
MuScreen *mu_screen_of(MuSession *s);
MuLevel *mu_level_loaded(); // the level the globals hold
MuSession *mu_player_loaded(); // the player the globals hold (py)
// Another player standing on coord, drawn and in the way (not in a building).
MuSession *mu_player_at(Coord_t const &coord);
void mu_swap_in(MuSession *s);

// Sessions in the game, under the world lock.
extern std::vector<MuSession *> mu_players;

void mu_ctx_init(MuPlayerCtx &ctx);
void mu_enter_game(MuSession *s, int depth);
void mu_change_level(MuSession *s);
void mu_leave_game(MuSession *s);
void mu_poke_level(MuLevel *level, MuSession *except);
int mu_level_waiters(MuLevel *level);
// A level change asked for from outside the engine's own stairs (lock held, s loaded).
void mu_send_to_depth(int depth);
void mu_forget_level(int depth); // /resetlevel: only when nobody is on it

// Characters as Umoria save files (lock held, s loaded).
bool mu_char_serialize(std::vector<uint8_t> &out);
bool mu_char_deserialize(const void *blob, int len);
// Writes the loaded character to the DB. Inside a transaction if one is open.
bool mu_char_store(MuSession *s);

// Saves levels that changed and drops long-empty ones (lock held).
void mu_world_maintain(bool force_all);

// Town litter: items on the town's floor go after MU_TOWN_ITEM_MINUTES
// (checked once a second, on a turn in the town). A script can take the job
// over (mu_town_expiry = false). Age in seconds, -1 if none is known there.
void mu_town_litter();
int mu_town_item_age(Coord_t const &coord);
extern bool mu_town_expiry;

const char *mu_depth_name(int depth, char *buf, size_t max);

// --- implementation of the template ----------------------------------------

template <typename F> void mu_with_player(MuSession *self, MuSession *other, F fn) {
    if (other == self) {
        fn();
        return;
    }
    MuScreen *screen = mu_vt_current;
    mu_swap_in(other);
    mu_vt_current = mu_screen_of(other);
    fn();
    mu_vt_current = screen;
    mu_swap_in(self);
}
