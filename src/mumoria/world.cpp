// Mumoria - the shared world. See world.h and docs/MUMORIA_DESIGN.md.

#include "world.h"

#include "auth.h"
#include "boss.h"
#include "raid.h"
#include "mu_config.h"
#include "session.h"
#include "social.h"

#include <map>
#include <pthread.h>
#include <unistd.h>

MuWorldSeeds mu_seeds{0, 0};
std::vector<MuSession *> mu_players;

static pthread_mutex_t world_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t world_cond = PTHREAD_COND_INITIALIZER;
static unsigned long next_ticket = 0, serving = 0;

static MuSession *loaded_player = nullptr;
static MuLevel *loaded_level = nullptr; // null: the globals hold nothing worth keeping
static std::map<int, MuLevel *> levels;

// Thread-of-execution state Umoria keeps in globals across a prompt.
extern int hack_monptr;
static thread_local int my_hack_monptr = -1;

static bool *const options[11] = {
    &config::options::display_counts,   &config::options::find_bound,       &config::options::run_cut_corners,
    &config::options::run_examine_corners, &config::options::run_ignore_doors, &config::options::run_print_self,
    &config::options::highlight_seams,  &config::options::prompt_to_pickup, &config::options::use_roguelike_keys,
    &config::options::show_inventory_weights, &config::options::error_beep_sound,
};

// --- the lock ----------------------------------------------------------------

void mu_world_lock(MuSession *s) {
    pthread_mutex_lock(&world_mutex);
    for (;;) {
        unsigned long ticket = next_ticket++;
        while (ticket != serving) {
            pthread_cond_wait(&world_cond, &world_mutex);
        }
        MuLevel *level = s != nullptr ? s->level : nullptr;
        if (level == nullptr || level->busy == nullptr || level->busy == s) {
            break;
        }
        // someone is mid-action on this level: let the queue move and wait
        serving++;
        level->waiters++;
        pthread_cond_broadcast(&world_cond);
        while (s->level == level && level->busy != nullptr && level->busy != s) {
            pthread_cond_wait(&world_cond, &world_mutex);
        }
        level->waiters--;
    }
    pthread_mutex_unlock(&world_mutex);

    if (s != nullptr) {
        mu_swap_in(s);
    }
    hack_monptr = my_hack_monptr;
}

void mu_world_unlock(MuSession *s, bool hold_level) {
    my_hack_monptr = hack_monptr;
    pthread_mutex_lock(&world_mutex);
    if (s != nullptr && s->level != nullptr) {
        if (hold_level) {
            s->level->busy = s;
        } else if (s->level->busy == s) {
            s->level->busy = nullptr;
        }
    }
    serving++;
    pthread_cond_broadcast(&world_cond);
    pthread_mutex_unlock(&world_mutex);

    if (s != nullptr && s->did_act && s->level != nullptr) {
        s->did_act = false;
        mu_poke_level(s->level, s);
    }
}

// --- the context swap ----------------------------------------------------------

static void save_level(MuLevel *L) {
    L->height = dg.height;
    L->width = dg.width;
    L->game_turn = dg.game_turn;
    memcpy(L->floor, dg.floor, sizeof(L->floor));
    memcpy(L->monsters, monsters, sizeof(L->monsters));
    L->next_free_monster_id = next_free_monster_id;
    L->monster_multiply_total = monster_multiply_total;
    L->treasure_current_id = game.treasure.current_id;
    memcpy(L->treasure, game.treasure.list, sizeof(L->treasure));
    L->dirty = true;
}

static void load_level(MuLevel *L) {
    dg.height = L->height;
    dg.width = L->width;
    dg.game_turn = L->game_turn;
    dg.current_level = (int16_t) L->depth;
    memcpy(dg.floor, L->floor, sizeof(dg.floor));
    memcpy(monsters, L->monsters, sizeof(L->monsters));
    next_free_monster_id = L->next_free_monster_id;
    monster_multiply_total = L->monster_multiply_total;
    game.treasure.current_id = L->treasure_current_id;
    memcpy(game.treasure.list, L->treasure, sizeof(L->treasure));
    loaded_level = L;
}

static bool in_level(Coord_t const &c) {
    return c.y > 0 && c.x > 0 && c.y < dg.height - 1 && c.x < dg.width - 1;
}

static void save_player(MuSession *s) {
    mu_shops_put_away(s);
    MuPlayerCtx &c = s->ctx;
    c.py = py;
    c.game = game; // its floor items are the level's; load_player leaves them
    c.panel = dg.panel;
    c.generate_new_level = dg.generate_new_level;
    memcpy(c.objects_identified, objects_identified, sizeof(c.objects_identified));
    memcpy(c.creature_recall, creature_recall, sizeof(c.creature_recall));
    memcpy(c.messages, messages, sizeof(c.messages));
    c.last_message_id = last_message_id;
    c.message_ready_to_print = message_ready_to_print;
    c.screen_has_changed = screen_has_changed;
    muRunStateGet(c.run_prevdir, c.run_direction);
    c.eof = eof_flag;
    for (int i = 0; i < 11; i++) {
        c.opt[i] = *options[i];
    }

    if (s->level != nullptr && s->level == loaded_level) {
        size_t bytes = (size_t) MAX_HEIGHT * MAX_WIDTH / 8 + 1;
        c.marks.assign(bytes, 0);
        c.lit.assign(bytes, 0);
        for (int y = 0; y < dg.height; y++) {
            for (int x = 0; x < dg.width; x++) {
                int bit = y * MAX_WIDTH + x;
                if (dg.floor[y][x].field_mark) {
                    c.marks[bit >> 3] |= (uint8_t) (1 << (bit & 7));
                }
                if (dg.floor[y][x].permanent_light) {
                    c.lit[bit >> 3] |= (uint8_t) (1 << (bit & 7));
                }
            }
        }
        c.marks_depth = s->level->depth;
        c.shown.clear();
        for (int i = config::monsters::MON_MIN_INDEX_ID; i < next_free_monster_id; i++) {
            if (monsters[i].lit) {
                c.shown.push_back(monsters[i].pos);
            }
        }
        if (in_level(py.pos) && dg.floor[py.pos.y][py.pos.x].creature_id == 1) {
            dg.floor[py.pos.y][py.pos.x].creature_id = 0;
        }
    }
}

// Puts the loaded player on their square, moving them if a monster took it.
static void stand_player() {
    if (!in_level(py.pos) || dg.floor[py.pos.y][py.pos.x].creature_id > 1 || dg.floor[py.pos.y][py.pos.x].feature_id >= MIN_CLOSED_SPACE) {
        Coord_t spot = py.pos;
        muFindSpot(spot);
        py.pos = spot;
    }
    dg.floor[py.pos.y][py.pos.x].creature_id = 1;
}

// Every lamp on the level lights the squares around its bearer, for all to
// see; where a player was is dark again once they move (it is relit from
// where they are each time someone is loaded). Your own lamp is Umoria's
// sub1MoveLight 3x3; others' light shows but is not remembered.
static void light_around(Coord_t const &c) {
    for (int y = c.y - 1; y <= c.y + 1; y++) {
        for (int x = c.x - 1; x <= c.x + 1; x++) {
            if (y >= 0 && x >= 0 && y < dg.height && x < dg.width) {
                dg.floor[y][x].temporary_light = true;
            }
        }
    }
}

static void light_lamps(MuSession *s) {
    if (py.carrying_light && py.flags.blind == 0) {
        light_around(py.pos);
    }
    for (MuSession *p : s->level->players) {
        if (p != s && !p->in_building && p->ctx.py.carrying_light && in_level(p->ctx.py.pos)) {
            light_around(p->ctx.py.pos);
        }
    }
}

// Monsters are lit (drawn) as this player's screen shows them. A monster that
// moved since is still drawn where it was: that square is drawn again, and
// the monster is drawn where it is now by the next updateMonsters().
static void show_monsters(MuSession *s, bool same_level) {
    if (!same_level) {
        return;
    }
    MuScreen *screen = mu_vt_current;
    mu_vt_current = mu_screen_of(s); // mu_with_player() sets it only after the swap
    for (Coord_t const &at : s->ctx.shown) {
        if (!in_level(at)) {
            continue;
        }
        int id = dg.floor[at.y][at.x].creature_id;
        if (id > 1) {
            monsters[id].lit = true; // still there (or another took its place)
        }
        dungeonLiteSpot(at);
    }
    mu_vt_current = screen;
}

static void load_player(MuSession *s) {
    mu_shops_bring_back(s);
    MuPlayerCtx &c = s->ctx;
    py = c.py;
    // everything but the floor items, which belong to the level
    auto treasure = game.treasure.current_id;
    Inventory_t keep[LEVEL_MAX_OBJECTS];
    memcpy(keep, game.treasure.list, sizeof(keep));
    game = c.game;
    memcpy(game.treasure.list, keep, sizeof(keep));
    game.treasure.current_id = treasure;
    dg.panel = c.panel;
    dg.generate_new_level = c.generate_new_level;
    memcpy(objects_identified, c.objects_identified, sizeof(c.objects_identified));
    memcpy(creature_recall, c.creature_recall, sizeof(c.creature_recall));
    memcpy(messages, c.messages, sizeof(c.messages));
    last_message_id = c.last_message_id;
    message_ready_to_print = c.message_ready_to_print;
    screen_has_changed = c.screen_has_changed;
    muRunStateSet(c.run_prevdir, c.run_direction);
    eof_flag = c.eof;
    for (int i = 0; i < 11; i++) {
        *options[i] = c.opt[i];
    }

    if (s->level != nullptr && s->level == loaded_level) {
        bool have = c.marks_depth == s->level->depth && !c.marks.empty();
        bool dungeon = s->level->depth > 0; // the town's light is daylight, shared
        for (int y = 0; y < dg.height; y++) {
            for (int x = 0; x < dg.width; x++) {
                int bit = y * MAX_WIDTH + x;
                dg.floor[y][x].field_mark = have && (c.marks[bit >> 3] & (1 << (bit & 7)));
                if (dungeon) {
                    dg.floor[y][x].permanent_light = have && (c.lit[bit >> 3] & (1 << (bit & 7)));
                }
                dg.floor[y][x].temporary_light = false;
            }
        }
        stand_player();
        light_lamps(s);
        for (int i = config::monsters::MON_MIN_INDEX_ID; i < next_free_monster_id; i++) {
            monsters[i].distance_from_player = (uint8_t) coordDistanceBetween(py.pos, monsters[i].pos);
            monsters[i].lit = false;
        }
        loaded_player = s; // show_monsters() draws as this player
        show_monsters(s, c.marks_depth == s->level->depth);
    }
    loaded_player = s;
}

void mu_swap_in(MuSession *s) {
    if (loaded_player == s) {
        return;
    }
    if (loaded_player != nullptr) {
        save_player(loaded_player);
    }
    if (s->level != nullptr && loaded_level != s->level) {
        if (loaded_level != nullptr) {
            save_level(loaded_level);
        }
        load_level(s->level);
    }
    load_player(s);
}

void mu_ctx_init(MuPlayerCtx &ctx) {
    ctx = MuPlayerCtx{};
    ctx.game.magic_seed = mu_seeds.magic_seed;
    ctx.game.town_seed = mu_seeds.town_seed;
    for (int i = 0; i < 11; i++) {
        ctx.opt[i] = *options[i];
    }
    ctx.opt[8] = false; // original keys
}

// --- levels ----------------------------------------------------------------

struct LevelHeader {
    char magic[4];
    uint32_t version, tile_size, monster_size, item_size;
    int32_t depth, height, width, game_turn;
    int32_t next_free, multiply, treasure_id;
};

static std::vector<uint8_t> level_blob(MuLevel *L) {
    LevelHeader h{{'M', 'U', 'L', '1'}, 1, sizeof(Tile_t), sizeof(Monster_t), sizeof(Inventory_t),
                  L->depth, L->height, L->width, L->game_turn, L->next_free_monster_id, L->monster_multiply_total, L->treasure_current_id};
    std::vector<uint8_t> blob(sizeof(h) + sizeof(L->floor) + sizeof(L->monsters) + sizeof(L->treasure));
    uint8_t *p = blob.data();
    memcpy(p, &h, sizeof(h));
    p += sizeof(h);
    memcpy(p, L->floor, sizeof(L->floor));
    p += sizeof(L->floor);
    memcpy(p, L->monsters, sizeof(L->monsters));
    p += sizeof(L->monsters);
    memcpy(p, L->treasure, sizeof(L->treasure));
    return blob;
}

static MuLevel *level_from_blob(const void *data, int len) {
    LevelHeader h{};
    if (len < (int) sizeof(h)) {
        return nullptr;
    }
    memcpy(&h, data, sizeof(h));
    auto *L = new MuLevel();
    size_t want = sizeof(h) + sizeof(L->floor) + sizeof(L->monsters) + sizeof(L->treasure);
    if (memcmp(h.magic, "MUL1", 4) != 0 || h.version != 1 || h.tile_size != sizeof(Tile_t) || h.monster_size != sizeof(Monster_t) ||
        h.item_size != sizeof(Inventory_t) || (size_t) len != want) {
        delete L;
        return nullptr; // another build's layout: the level is made anew
    }
    const uint8_t *p = (const uint8_t *) data + sizeof(h);
    memcpy(L->floor, p, sizeof(L->floor));
    p += sizeof(L->floor);
    memcpy(L->monsters, p, sizeof(L->monsters));
    p += sizeof(L->monsters);
    memcpy(L->treasure, p, sizeof(L->treasure));
    L->depth = h.depth;
    L->height = (int16_t) h.height;
    L->width = (int16_t) h.width;
    L->game_turn = h.game_turn;
    L->next_free_monster_id = (int16_t) h.next_free;
    L->monster_multiply_total = (int16_t) h.multiply;
    L->treasure_current_id = (int16_t) h.treasure_id;
    return L;
}

static void store_level(MuLevel *L) {
    if (L == loaded_level) {
        save_level(L);
    }
    std::vector<uint8_t> blob = level_blob(L);
    time_t now = time(nullptr);
    mu_db_run("INSERT INTO level (depth, blob, created_at, saved_at, boss_dead_at) VALUES (?, ?, ?, ?, ?) "
              "ON CONFLICT(depth) DO UPDATE SET blob = excluded.blob, saved_at = excluded.saved_at, boss_dead_at = excluded.boss_dead_at",
              "ibiii", (int64_t) L->depth, blob.data(), (int) blob.size(), (int64_t) L->created_at, (int64_t) now, (int64_t) L->boss_dead_at);
    L->saved_at = now;
    L->dirty = false;
}

// The level for a depth: in memory, else from the DB, else made now (and
// then loaded). Lock held.
static MuLevel *get_level(int depth) {
    auto it = levels.find(depth);
    if (it != levels.end()) {
        return it->second;
    }

    time_t now = time(nullptr);
    MuRows *r = mu_db_query("SELECT blob, created_at, saved_at, boss_dead_at FROM level WHERE depth = ?", "i", (int64_t) depth);
    MuLevel *L = nullptr;
    if (mu_db_next(r)) {
        int len = 0;
        const void *blob = mu_db_col_blob(r, 0, &len);
        time_t saved_at = (time_t) mu_db_col_int(r, 2);
        // the town never resets; a dungeon level left alone long enough does
        if (depth == 0 || now - saved_at < MU_LEVEL_RESET_HOURS * 3600) {
            L = level_from_blob(blob, len);
        }
        if (L != nullptr) {
            L->created_at = (time_t) mu_db_col_int(r, 1);
            L->saved_at = saved_at;
            L->boss_dead_at = (time_t) mu_db_col_int(r, 3);
        }
    }
    mu_db_done(r);

    if (L == nullptr) {
        if (loaded_level != nullptr) {
            save_level(loaded_level);
        }
        loaded_level = nullptr;
        dg.current_level = (int16_t) depth;
        generateCave();
        if (depth == 0) {
            mu_shops_take_catalog(); // the new town stocked its stores
        }
        L = new MuLevel();
        L->depth = depth;
        L->created_at = now;
        save_level(L);
        loaded_level = L;
        mu_boss_place(L);
        save_level(L);
        levels[depth] = L;
        store_level(L);
        return L;
    }
    levels[depth] = L;
    return L;
}

void mu_poke_level(MuLevel *level, MuSession *except) {
    for (MuSession *p : level->players) {
        if (p != except) {
            p->view_stale = true;
            mu_conn_poke(p->conn);
        }
    }
}

int mu_level_waiters(MuLevel *level) {
    pthread_mutex_lock(&world_mutex);
    int n = level->waiters;
    pthread_mutex_unlock(&world_mutex);
    return n;
}

void mu_send_to_depth(int depth) {
    mu_self->pending_depth = depth;
    dg.generate_new_level = true;
}

void mu_forget_level(int depth) {
    auto it = levels.find(depth);
    if (it != levels.end()) {
        if (!it->second->players.empty()) {
            return;
        }
        if (it->second == loaded_level) {
            loaded_level = nullptr;
        }
        delete it->second;
        levels.erase(it);
    }
    mu_db_run("DELETE FROM level WHERE depth = ?", "i", (int64_t) depth);
}

// The town as Umoria makes it on each visit: new townspeople, asleep. Done
// when someone enters a town nobody is in (lock held, the town loaded).
static void fresh_townspeople() {
    for (int y = 0; y < dg.height; y++) {
        for (int x = 0; x < dg.width; x++) {
            if (dg.floor[y][x].creature_id > 1) {
                dg.floor[y][x].creature_id = 0;
            }
        }
    }
    next_free_monster_id = config::monsters::MON_MIN_INDEX_ID;
    monster_multiply_total = 0;
    bool night = (dg.game_turn / 5000) & 1; // dungeon_generate.cpp isNighTime()
    monsterPlaceNewWithinDistance(night ? config::monsters::MON_MIN_TOWNSFOLK_NIGHT : config::monsters::MON_MIN_TOWNSFOLK_DAY, 3, true);
}

static std::vector<MuLevel *> raids; // raid instances: never saved

// A new level made for a raid from the gate on `gate` (raid.cpp fills it).
static MuLevel *new_raid(int gate, int party) {
    if (loaded_level != nullptr) {
        save_level(loaded_level);
    }
    loaded_level = nullptr;
    dg.current_level = (int16_t) gate;
    generateCave();
    auto *L = new MuLevel();
    L->depth = gate;
    L->created_at = time(nullptr);
    loaded_level = L;
    L->raid = mu_raid_build(gate, party);
    save_level(L);
    raids.push_back(L);
    return L;
}

static void enter_level(MuSession *s, MuLevel *L);

void mu_enter_game(MuSession *s, int depth) {
    enter_level(s, get_level(depth));
}

static void enter_level(MuSession *s, MuLevel *L) {
    int depth = L->depth;
    if (loaded_level != L) {
        if (loaded_level != nullptr) {
            save_level(loaded_level);
        }
        load_level(L);
    }

    // as generateCave() sets the panel up
    dg.panel.top = dg.panel.bottom = dg.panel.left = dg.panel.right = 0;
    dg.panel.max_rows = (int16_t) ((dg.height / SCREEN_HEIGHT) * 2 - 2);
    dg.panel.max_cols = (int16_t) ((dg.width / SCREEN_WIDTH) * 2 - 2);
    dg.panel.row = dg.panel.max_rows;
    dg.panel.col = dg.panel.max_cols;
    dg.generate_new_level = false;

    for (int y = 0; y < dg.height; y++) {
        for (int x = 0; x < dg.width; x++) {
            dg.floor[y][x].field_mark = false;
            dg.floor[y][x].temporary_light = false;
            if (depth > 0) {
                dg.floor[y][x].permanent_light = false; // a new arrival has lit nothing yet
            }
            if (dg.floor[y][x].creature_id == 1) {
                dg.floor[y][x].creature_id = 0; // stale, from whoever was loaded when it was saved
            }
        }
    }
    py.pos = Coord_t{-1, -1};
    if (depth == 0 && L->players.empty()) {
        fresh_townspeople();
    }
    mu_raid_place_gate(L);
    Coord_t gate;
    if (L->raid != nullptr) {
        py.pos = L->raid->entry; // in through the gate
    } else if (s->to_gate && mu_raid_find_gate(gate)) {
        py.pos = gate; // back out of a raid
        s->arrive_near = nullptr;
    }
    s->to_gate = false;
    // below the town a party arrives together: beside a member already there.
    // Anyone else drops in at random.
    if (s->arrive_near == nullptr && depth > 0 && s->party != nullptr && L->raid == nullptr && py.pos.y < 0) {
        for (MuSession *m : s->party->members) {
            if (m != s && m->level == L && !m->in_building) {
                s->arrive_near = m;
                break;
            }
        }
    }
    if (s->arrive_near != nullptr && s->arrive_near->level == L) {
        py.pos = s->arrive_near->ctx.py.pos;
    }
    Coord_t door = py.pos; // the gate or way out they arrive by, if any
    s->arrive_near = nullptr;
    muFindSpot(py.pos);
    dg.floor[py.pos.y][py.pos.x].creature_id = 1;
    if (door.y > 0 && dg.floor[door.y][door.x].treasure_id != 0) {
        dg.floor[door.y][door.x].field_mark = true; // they know where it is
    }

    s->level = L;
    mu_vt_fx_clear(mu_screen_of(s)); // the last level's animations
    s->ctx.marks.clear();
    s->ctx.lit.clear();
    s->ctx.shown.clear();
    s->ctx.marks_depth = depth;
    L->players.push_back(s);
    light_lamps(s);
    L->empty_since = 0;
    loaded_player = s;
    if (std::find(mu_players.begin(), mu_players.end(), s) == mu_players.end()) {
        mu_players.push_back(s);
    }
    s->in_game = true;
    if (L->raid == nullptr) {
        mu_boss_announce(s, L);
    }
    mu_raid_announce(s, L);
    mu_poke_level(L, s);
}

static void leave_level(MuSession *s) {
    MuLevel *L = s->level;
    if (L == nullptr) {
        return;
    }
    if (in_level(py.pos) && dg.floor[py.pos.y][py.pos.x].creature_id == 1) {
        dg.floor[py.pos.y][py.pos.x].creature_id = 0;
    }
    L->players.erase(std::remove(L->players.begin(), L->players.end(), s), L->players.end());
    if (L->players.empty()) {
        L->empty_since = time(nullptr);
    }
    pthread_mutex_lock(&world_mutex);
    if (L->busy == s) {
        L->busy = nullptr;
    }
    pthread_cond_broadcast(&world_cond);
    pthread_mutex_unlock(&world_mutex);
    save_level(L);
    s->level = nullptr;
    mu_poke_level(L, s);
}

void mu_change_level(MuSession *s) {
    if (s->pending_raid >= 0) {
        // through a raid gate: into the party's raid, or a new one made for
        // the members standing at the gate
        int gate = s->pending_raid;
        s->pending_raid = -1;
        int party = mu_raid_party_here(s);
        MuLevel *raid = mu_raid_of_party(s, gate);
        dg.current_level = (int16_t) s->level->depth;
        leave_level(s);
        enter_level(s, raid != nullptr ? raid : new_raid(gate, party));
        mu_char_store(s);
        return;
    }
    int new_depth = s->pending_depth >= 0 ? s->pending_depth : dg.current_level;
    s->pending_depth = -1;
    dg.current_level = (int16_t) s->level->depth;
    leave_level(s);
    mu_enter_game(s, new_depth);
    mu_char_store(s);
}

void mu_leave_game(MuSession *s) {
    leave_level(s);
    mu_players.erase(std::remove(mu_players.begin(), mu_players.end(), s), mu_players.end());
    s->in_game = false;
    if (loaded_player == s) {
        save_player(s);
        loaded_player = nullptr;
    }
}

void mu_world_maintain(bool force_all) {
    time_t now = time(nullptr);
    for (auto it = raids.begin(); it != raids.end();) {
        MuLevel *L = *it;
        if (L->players.empty() && L->empty_since != 0 && now - L->empty_since >= MU_RAID_EMPTY_SECONDS) {
            if (L == loaded_level) {
                loaded_level = nullptr;
            }
            delete L->raid;
            delete L;
            it = raids.erase(it);
            continue;
        }
        ++it;
    }
    for (auto it = levels.begin(); it != levels.end();) {
        MuLevel *L = it->second;
        if (L == loaded_level) {
            save_level(L);
        }
        if (L->dirty && (force_all || now - L->saved_at >= MU_LEVEL_SAVE_SECONDS || L->players.empty())) {
            store_level(L);
        }
        if (L->players.empty() && L->empty_since != 0 && now - L->empty_since >= MU_LEVEL_UNLOAD_SECONDS && !L->dirty) {
            if (L == loaded_level) {
                loaded_level = nullptr;
            }
            delete L;
            it = levels.erase(it);
            continue;
        }
        ++it;
    }
    mu_db_run("INSERT INTO world (key, blob) VALUES ('stores', ?) ON CONFLICT(key) DO UPDATE SET blob = excluded.blob", "b", mu_catalog,
              (int) sizeof(mu_catalog));
}

// --- characters ------------------------------------------------------------

static std::string temp_save_path() {
    return "mumoria_tmp_" + std::to_string((long) getpid()) + ".sav";
}

bool mu_char_serialize(std::vector<uint8_t> &out) {
    std::string path = temp_save_path();
    (void) unlink(path.c_str());
    config::files::save_game = path;

    int32_t turn = dg.game_turn;
    bool was_saved = game.character_saved;
    game.character_saved = false;
    bool ok = saveGame();
    game.character_saved = was_saved;
    dg.game_turn = turn;
    // saveChar() takes the pack's weight off the speed; have it put back
    py.flags.status |= config::player::status::PY_STR_WGT;

    out.clear();
    FILE *f = fopen(path.c_str(), "rb");
    if (f != nullptr) {
        uint8_t buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
            out.insert(out.end(), buf, buf + n);
        }
        fclose(f);
    }
    (void) unlink(path.c_str());
    return ok && !out.empty();
}

bool mu_char_deserialize(const void *blob, int len) {
    // loadGame() also restores a level, the stores and the seeds: keep ours
    if (loaded_level != nullptr) {
        save_level(loaded_level);
        loaded_level = nullptr;
    }
    auto *keep_stores = new Store_t[MAX_STORES];
    memcpy(keep_stores, stores, sizeof(stores));

    std::string path = temp_save_path();
    FILE *f = fopen(path.c_str(), "wb");
    if (f == nullptr) {
        delete[] keep_stores;
        return false;
    }
    fwrite(blob, 1, (size_t) len, f);
    fclose(f);
    config::files::save_game = path;

    dg.game_turn = -1;
    bool generate = false;
    bool ok = loadGame(generate);
    (void) unlink(path.c_str());

    memcpy(stores, keep_stores, sizeof(stores));
    delete[] keep_stores;
    game.magic_seed = mu_seeds.magic_seed;
    game.town_seed = mu_seeds.town_seed;
    game.character_saved = false;
    game.to_be_wizard = false;
    game.wizard_mode = false;
    return ok;
}

bool mu_char_store(MuSession *s) {
    std::vector<uint8_t> blob;
    if (s->char_id == 0 || !mu_char_serialize(blob)) {
        return false;
    }
    int depth = s->level != nullptr ? s->level->depth : 0;
    int changed = mu_db_run("UPDATE character SET save = ?, level = ?, depth = ?, saved_at = ? WHERE id = ? AND alive = 1", "biiii",
                            blob.data(), (int) blob.size(), (int64_t) py.misc.level, (int64_t) depth, (int64_t) time(nullptr), s->char_id);
    s->last_save = time(nullptr);
    return changed == 1;
}

// --- the world --------------------------------------------------------------

void mu_world_init() {
    int64_t magic = mu_db_int("SELECT num FROM world WHERE key = 'magic_seed'", 0, "");
    int64_t town = mu_db_int("SELECT num FROM world WHERE key = 'town_seed'", 0, "");
    uint32_t fresh = 0;
    mu_random_bytes((uint8_t *) &fresh, sizeof(fresh));
    muEngineInit(fresh & 0x7fffffff);
    if (magic == 0 || town == 0) {
        magic = game.magic_seed;
        town = game.town_seed;
        mu_db_run("INSERT OR REPLACE INTO world (key, num) VALUES ('magic_seed', ?)", "i", magic);
        mu_db_run("INSERT OR REPLACE INTO world (key, num) VALUES ('town_seed', ?)", "i", town);
    }
    mu_seeds.magic_seed = (uint32_t) magic;
    mu_seeds.town_seed = (uint32_t) town;
    game.magic_seed = mu_seeds.magic_seed;
    game.town_seed = mu_seeds.town_seed;
    magicInitializeItemNames();

    MuRows *r = mu_db_query("SELECT blob FROM world WHERE key = 'stores'", "");
    if (mu_db_next(r)) {
        int len = 0;
        const void *blob = mu_db_col_blob(r, 0, &len);
        if (len == (int) sizeof(stores)) {
            memcpy(stores, blob, sizeof(stores));
        }
    }
    mu_db_done(r);
    mu_shops_take_catalog();
}

void mu_world_shutdown() {
    mu_world_lock(nullptr);
    mu_world_maintain(true);
    mu_world_unlock(nullptr, false);
}

const char *mu_depth_name(int depth, char *buf, size_t max) {
    if (depth == 0) {
        snprintf(buf, max, "the town");
    } else {
        snprintf(buf, max, "%d ft", depth * 50);
    }
    return buf;
}

// --- engine hooks about other players ------------------------------------------

MuLevel *mu_level_loaded() {
    return loaded_level;
}

MuScreen *mu_screen_of(MuSession *s) {
    return &s->conn->screen;
}

MuSession *mu_player_at(Coord_t const &coord) {
    if (loaded_level == nullptr) {
        return nullptr;
    }
    for (MuSession *p : loaded_level->players) {
        if (p != loaded_player && !p->in_building && p->ctx.py.pos.y == coord.y && p->ctx.py.pos.x == coord.x) {
            return p;
        }
    }
    return nullptr;
}

bool muOtherPlayerAt(Coord_t const &coord) {
    return mu_player_at(coord) != nullptr;
}

const char *muOtherPlayerNameAt(Coord_t const &coord) {
    MuSession *p = mu_player_at(coord);
    return p != nullptr ? p->char_name.c_str() : nullptr;
}

bool muNearerPlayer(Coord_t const &coord, int distance) {
    if (loaded_level == nullptr) {
        return false;
    }
    for (MuSession *p : loaded_level->players) {
        if (p != loaded_player && coordDistanceBetween(coord, p->ctx.py.pos) < distance) {
            return true;
        }
    }
    return false;
}

int muNewMonsterChance() {
    int chance = config::monsters::MON_CHANCE_OF_NEW;
    if (loaded_level == nullptr || loaded_level->depth == 0) {
        return chance;
    }
    // a party, or guild-mates together, count once; each lone player counts
    std::vector<const void *> teams;
    for (MuSession *p : loaded_level->players) {
        const void *team = p->party != nullptr ? (const void *) p->party : (const void *) p;
        if (p->party == nullptr && p->guild_id != 0) {
            for (MuSession *q : loaded_level->players) {
                if (q != p && q->party == nullptr && q->guild_id == p->guild_id) {
                    team = (const void *) (intptr_t) p->guild_id;
                }
            }
        }
        if (std::find(teams.begin(), teams.end(), team) == teams.end()) {
            teams.push_back(team);
        }
    }
    chance /= teams.empty() ? 1 : (int) teams.size();
    return chance < MU_NEW_MONSTER_MIN ? MU_NEW_MONSTER_MIN : chance;
}

// Can any player on the loaded level see this square?
static bool seen_by_a_player(Coord_t const &c) {
    for (MuSession *p : loaded_level->players) {
        Coord_t eye = p == loaded_player ? py.pos : p->ctx.py.pos;
        if (!p->in_building && coordDistanceBetween(eye, c) <= config::monsters::MON_MAX_SIGHT && los(eye, c)) {
            return true;
        }
    }
    return false;
}

void muTownPeople() {
    if (loaded_level == nullptr) {
        return;
    }
    int people = next_free_monster_id - config::monsters::MON_MIN_INDEX_ID;
    bool night = (dg.game_turn / 5000) & 1; // dungeon_generate.cpp isNighTime()
    int want = night ? config::monsters::MON_MIN_TOWNSFOLK_NIGHT : config::monsters::MON_MIN_TOWNSFOLK_DAY;
    if (people <= want && randomNumber(config::monsters::MON_CHANCE_OF_NEW) != 1) {
        return;
    }
    if (people > want) { // too many: one leaves each turn
        for (int i = next_free_monster_id - 1; i >= config::monsters::MON_MIN_INDEX_ID; i--) {
            if (!seen_by_a_player(monsters[i].pos)) {
                dungeonDeleteMonster(i);
                return;
            }
        }
        return;
    }
    if (people == want) {
        return;
    }
    for (int tries = 0; tries < 200; tries++) {
        Coord_t c{(int32_t) randomNumber(dg.height - 2), (int32_t) randomNumber(dg.width - 2)};
        Tile_t const &t = dg.floor[c.y][c.x];
        if (t.feature_id < MIN_CLOSED_SPACE && t.creature_id == 0 && !muOtherPlayerAt(c) && !seen_by_a_player(c)) {
            (void) monsterPlaceNew(c, randomNumber(monster_levels[0]) - 1, true); // a townsperson, as monsterGetOneSuitableForLevel(0)
            return;
        }
    }
}

// In the town, an awake townsperson within 8 squares: nobody arrives there.
static bool town_danger(Coord_t const &c) {
    if (dg.current_level != 0) {
        return false;
    }
    for (int i = config::monsters::MON_MIN_INDEX_ID; i < next_free_monster_id; i++) {
        if (monsters[i].sleep_count == 0 && coordDistanceBetween(c, monsters[i].pos) <= 8) {
            return true;
        }
    }
    return false;
}

void muFindSpot(Coord_t &coord) {
    for (int tries = 0; tries < 20000; tries++) {
        Coord_t c{(int32_t) randomNumber(dg.height - 2), (int32_t) randomNumber(dg.width - 2)};
        if (tries < 200 && coord.y > 0) {
            // first look close to where they were
            c.y = coord.y + randomNumber(5) - 3;
            c.x = coord.x + randomNumber(5) - 3;
            if (!in_level(c)) {
                continue;
            }
        }
        Tile_t const &t = dg.floor[c.y][c.x];
        if (t.feature_id < MIN_CLOSED_SPACE && t.creature_id == 0 && t.treasure_id == 0 && !muOtherPlayerAt(c) && (tries > 10000 || !town_danger(c))) {
            coord = c;
            return;
        }
    }
    coord = Coord_t{1, 1};
}
