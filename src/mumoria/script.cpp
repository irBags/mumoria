// Mumoria - Lua scripts (docs/MUMORIA_DESIGN.md 2.23, scripts/README.md).
//
// One Lua 5.4 state for the server, used only under the world lock. Every
// *.lua in the scripts directory is run at start (and again on /reload); a
// script defines items (mu.item), bosses (mu.boss), commands (mu.command)
// and event handlers (mu.on: enter, kill, hurt). The prelude below writes
// mu.on, mu.command and mu.boss in Lua over the C functions.
//
// Scripts run while every player waits, so they are fenced in: no io, os,
// package, debug, load or dofile; at most MEMORY_MAX bytes; a call stopped
// after STEP_MAX thousand instructions. An error is logged, shown to the
// admins online, and the rest of the server carries on.

#include "script.h"

#include <dirent.h>

#include <algorithm>
#include <map>
#include <set>

#include "../lua/lua.hpp"
#include "items.h"
#include "loot.h"
#include "raid.h"
#include "session.h"
#include "social.h"

namespace {

lua_State *L = nullptr;
std::string script_dir = "scripts";
std::vector<std::string> loaded;
std::vector<std::string> errors; // the last few
std::string current_file;

size_t memory = 0;
const size_t MEMORY_MAX = 64u << 20;
int steps = 0;
int depth = 0;              // calls into Lua under way (an event can come from inside mu.hurt)
const int STEP_HOOK = 1000; // instructions between counts
const int STEP_MAX = 5000;  // counts in one call: 5 million instructions

// Numbers a monster carries (Monster_t::mu_script, mu_name), kept for good
// in script_state ("<kind>:<n>" = the key or name), so a monster saved on a
// level means the same after a restart or a /reload.
struct Registry {
    const char *kind;
    std::vector<std::string> keys; // [n - 1]

    void load() {
        keys.clear();
        std::string like = std::string(kind) + ":%";
        MuRows *r = mu_db_query("SELECT key, value FROM script_state WHERE key LIKE ?", "s", like.c_str());
        while (r != nullptr && mu_db_next(r)) {
            int n = atoi(mu_db_col_text(r, 0) + strlen(kind) + 1);
            if (n >= 1 && n <= 255) {
                if ((int) keys.size() < n) {
                    keys.resize((size_t) n);
                }
                keys[(size_t) n - 1] = mu_db_col_text(r, 1);
            }
        }
        mu_db_done(r);
    }

    int find(std::string const &key) const {
        for (size_t i = 0; i < keys.size(); i++) {
            if (keys[i] == key) {
                return (int) i + 1;
            }
        }
        return 0;
    }

    // its number, given now if it has none (0 when all 255 are taken)
    int get(std::string const &key) {
        int n = find(key);
        if (n != 0 || keys.size() >= 255) {
            return n;
        }
        keys.push_back(key);
        n = (int) keys.size();
        std::string row = std::string(kind) + ":" + std::to_string(n);
        mu_db_run("INSERT INTO script_state (key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value", "ss", row.c_str(),
                  key.c_str());
        return n;
    }

    std::string const *at(int n) const {
        return n >= 1 && n <= (int) keys.size() && !keys[(size_t) n - 1].empty() ? &keys[(size_t) n - 1] : nullptr;
    }
};

Registry tags{"tag", {}};               // Monster_t::mu_script: a boss's key or a tag
Registry names{"monster_name", {}};     // Monster_t::mu_name
std::vector<std::string> boss_tags;     // the tags mu.boss and boss= made bosses of (they drop a boss's loot)
std::map<std::string, int> boss_max_hp; // "depth:key" -> hit points when made
std::map<MuLevel *, time_t> ticked;     // when each level's timers last ran
std::set<int> held;                     // tags whose monsters the script moves (mu.hold)

void *alloc(void *, void *ptr, size_t osize, size_t nsize) {
    if (ptr == nullptr) {
        osize = 0; // then osize is the kind of object, not a size
    }
    if (nsize == 0) {
        memory -= osize;
        free(ptr);
        return nullptr;
    }
    if (memory - osize + nsize > MEMORY_MAX) {
        return nullptr;
    }
    void *p = realloc(ptr, nsize);
    if (p != nullptr) {
        memory = memory - osize + nsize;
    }
    return p;
}

void count_hook(lua_State *l, lua_Debug *) {
    if (++steps > STEP_MAX) {
        luaL_error(l, "the script ran too long and was stopped");
    }
}

void report(std::string const &what) {
    fprintf(stderr, "mumoria script: %s\n", what.c_str());
    errors.push_back(what);
    if (errors.size() > 10) {
        errors.erase(errors.begin());
    }
    for (MuSession *p : mu_players) {
        if (p->account.role >= 2) {
            mu_system(p, "[Script] " + what.substr(0, 200));
        }
    }
}

int traceback(lua_State *l) {
    const char *msg = lua_tostring(l, 1);
    luaL_traceback(l, l, msg != nullptr ? msg : "(an error that is not a string)", 1);
    return 1;
}

// Calls the function under its nargs arguments; false (and reported) on an error.
bool call(int nargs, int nresults) {
    int base = lua_gettop(L) - nargs;
    lua_pushcfunction(L, traceback);
    lua_insert(L, base);
    if (depth++ == 0) {
        steps = 0; // the budget is for the whole call, events inside it included
    }
    int rc = lua_pcall(L, nargs, nresults, base);
    depth--;
    lua_remove(L, base);
    if (rc != LUA_OK) {
        report(lua_tostring(L, -1) != nullptr ? lua_tostring(L, -1) : "error");
        lua_pop(L, 1);
        return false;
    }
    return true;
}

// mu.<name> onto the stack; false (nothing pushed) if it is not a function.
bool get_mu(const char *name) {
    if (L == nullptr) {
        return false;
    }
    lua_getglobal(L, "mu");
    lua_getfield(L, -1, name);
    lua_remove(L, -2);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1);
        return false;
    }
    return true;
}

std::string lower(std::string s) {
    for (char &c : s) {
        c = (char) tolower((unsigned char) c);
    }
    return s;
}

void need_world(lua_State *l) {
    if (mu_self == nullptr || mu_level_loaded() == nullptr) {
        luaL_error(l, "this works only inside an event or a command");
    }
}

// --- tables ----------------------------------------------------------------------

void set_int(lua_State *l, const char *k, lua_Integer v) {
    lua_pushinteger(l, v);
    lua_setfield(l, -2, k);
}

void set_str(lua_State *l, const char *k, std::string const &v) {
    lua_pushstring(l, v.c_str());
    lua_setfield(l, -2, k);
}

void set_bool(lua_State *l, const char *k, bool v) {
    lua_pushboolean(l, v ? 1 : 0);
    lua_setfield(l, -2, k);
}

void push_player(lua_State *l, MuSession *p) {
    Player_t const &P = p == mu_player_loaded() ? py : p->ctx.py;
    lua_newtable(l);
    set_str(l, "name", p->char_name);
    set_int(l, "level", P.misc.level);
    set_str(l, "class", classes[P.misc.class_id].title);
    set_str(l, "race", character_races[P.misc.race_id].name);
    set_int(l, "x", P.pos.x);
    set_int(l, "y", P.pos.y);
    set_int(l, "depth", p->level != nullptr ? p->level->depth : 0);
    set_int(l, "hp", P.misc.current_hp);
    set_int(l, "max_hp", P.misc.max_hp);
    set_int(l, "gold", P.misc.au);
    set_bool(l, "admin", p->account.role >= 2);
    set_bool(l, "in_party", p->party != nullptr);
}

void push_monster(lua_State *l, int id) {
    Monster_t const &m = monsters[id];
    Creature_t const &c = creatures_list[m.creature_id];
    lua_newtable(l);
    set_int(l, "id", id);
    set_int(l, "_c", m.creature_id);
    set_str(l, "name", muMonsterName(m));
    set_str(l, "letter", std::string(1, (char) c.sprite));
    set_int(l, "level", c.level);
    set_int(l, "x", m.pos.x);
    set_int(l, "y", m.pos.y);
    set_int(l, "hp", m.hp);
    set_bool(l, "awake", m.sleep_count == 0);
    set_int(l, "phase", m.mu_phase);
    static const char *KIND[] = {nullptr, "boss", "captain", "lord"};
    if (m.mu_boss >= 1 && m.mu_boss <= 3) {
        set_str(l, "kind", KIND[m.mu_boss]);
    }
    set_str(l, "creature", c.name);
    int max_hp = m.hp;
    if (std::string const *key = tags.at(m.mu_script)) {
        bool boss = std::find(boss_tags.begin(), boss_tags.end(), *key) != boss_tags.end();
        set_str(l, boss ? "boss" : "tag", *key);
        int &known = boss_max_hp[std::to_string(dg.current_level) + ":" + *key];
        known = std::max(known, (int) m.hp);
        max_hp = known;
    }
    set_int(l, "max_hp", max_hp);
}

// A monster table still meaning the same monster: its id, else -1.
int monster_arg(lua_State *l, int idx) {
    luaL_checktype(l, idx, LUA_TTABLE);
    lua_getfield(l, idx, "id");
    lua_getfield(l, idx, "_c");
    int id = (int) lua_tointeger(l, -2), c = (int) lua_tointeger(l, -1);
    lua_pop(l, 2);
    if (id < config::monsters::MON_MIN_INDEX_ID || id >= next_free_monster_id || monsters[id].creature_id != c || monsters[id].hp < 0) {
        return -1;
    }
    return id;
}

// {x=, y=} (a player or a monster will do)
Coord_t coord_arg(lua_State *l, int idx) {
    luaL_checktype(l, idx, LUA_TTABLE);
    lua_getfield(l, idx, "x");
    lua_getfield(l, idx, "y");
    Coord_t c{(int32_t) luaL_checkinteger(l, -1), (int32_t) luaL_checkinteger(l, -2)};
    lua_pop(l, 2);
    if (c.y <= 0 || c.x <= 0 || c.y >= dg.height - 1 || c.x >= dg.width - 1) {
        luaL_error(l, "the place (%d, %d) is off the level", (int) c.x, (int) c.y);
    }
    return c;
}

// A player table or a name: the player, online; else an error.
MuSession *player_arg(lua_State *l, int idx) {
    std::string name;
    if (lua_istable(l, idx)) {
        lua_getfield(l, idx, "name");
        name = luaL_checkstring(l, -1);
        lua_pop(l, 1);
    } else {
        name = luaL_checkstring(l, idx);
    }
    pthread_mutex_lock(&mu_sessions_lock);
    MuSession *p = mu_find_online(name.c_str());
    pthread_mutex_unlock(&mu_sessions_lock);
    if (p == nullptr || !p->in_game) {
        luaL_error(l, "%s is not in the game", name.c_str());
    }
    return p;
}

int int_field(lua_State *l, int idx, const char *k, int dflt) {
    lua_getfield(l, idx, k);
    int v = lua_isnil(l, -1) ? dflt : (int) luaL_checkinteger(l, -1);
    lua_pop(l, 1);
    return v;
}

double num_field(lua_State *l, int idx, const char *k, double dflt) {
    lua_getfield(l, idx, k);
    double v = lua_isnil(l, -1) ? dflt : luaL_checknumber(l, -1);
    lua_pop(l, 1);
    return v;
}

std::string str_field(lua_State *l, int idx, const char *k, std::string const &dflt) {
    lua_getfield(l, idx, k);
    std::string v = lua_isnil(l, -1) ? dflt : luaL_checkstring(l, -1);
    lua_pop(l, 1);
    return v;
}

bool bool_field(lua_State *l, int idx, const char *k, bool dflt) {
    lua_getfield(l, idx, k);
    bool v = lua_isnil(l, -1) ? dflt : lua_toboolean(l, -1) != 0;
    lua_pop(l, 1);
    return v;
}

// --- the mu functions ------------------------------------------------------------

int l_item(lua_State *l) {
    luaL_checktype(l, 1, LUA_TTABLE);
    MuCustomItem d;
    d.key = str_field(l, 1, "key", "");
    d.name = str_field(l, 1, "name", "");
    if (d.key.empty() || d.name.empty()) {
        return luaL_error(l, "mu.item needs a key and a name");
    }
    std::string base = str_field(l, 1, "base", "");
    d.base = mu_items_find_base(base);
    if (d.base < 0) {
        return luaL_error(l, "mu.item %s: no base item '%s' (or more than one has that in its name)", d.key.c_str(), base.c_str());
    }
    std::string tier = lower(str_field(l, 1, "tier", "rare"));
    d.tier = mu_rarity_by_name(tier);
    if (d.tier < 0) {
        return luaL_error(l, "mu.item %s: no rarity '%s'", d.key.c_str(), tier.c_str());
    }
    d.to_hit = int_field(l, 1, "to_hit", 0);
    d.to_damage = int_field(l, 1, "to_dam", 0);
    d.to_ac = int_field(l, 1, "to_ac", 0);
    d.bonus = int_field(l, 1, "bonus", 0);
    std::string dice = str_field(l, 1, "dice", "");
    if (!dice.empty() && sscanf(dice.c_str(), "%dd%d", &d.dice, &d.sides) != 2) {
        return luaL_error(l, "mu.item %s: dice like \"3d6\", not '%s'", d.key.c_str(), dice.c_str());
    }
    lua_getfield(l, 1, "powers");
    if (lua_istable(l, -1)) {
        for (lua_Integer i = 1; lua_rawgeti(l, -1, i) != LUA_TNIL; i++) {
            std::string name = luaL_checkstring(l, -1);
            lua_pop(l, 1);
            uint32_t flag = mu_items_power(name);
            if (flag == 0) {
                return luaL_error(l, "mu.item %s: no power '%s'", d.key.c_str(), name.c_str());
            }
            d.flags |= flag;
        }
        lua_pop(l, 1);
    }
    lua_pop(l, 1);
    lua_getfield(l, 1, "depth");
    if (lua_istable(l, -1)) {
        lua_rawgeti(l, -1, 1);
        lua_rawgeti(l, -2, 2);
        d.min_depth = (int) luaL_optinteger(l, -2, 1);
        d.max_depth = (int) luaL_optinteger(l, -1, 127);
        lua_pop(l, 2);
    } else if (lua_isinteger(l, -1)) {
        d.min_depth = d.max_depth = (int) lua_tointeger(l, -1);
    }
    lua_pop(l, 1);
    std::string from = lower(str_field(l, 1, "from", "bosses"));
    const char *FROM[] = {"anywhere", "bosses", "lords", "never"};
    d.from = -1;
    for (int i = 0; i < 4; i++) {
        if (from == FROM[i]) {
            d.from = i;
        }
    }
    if (d.from < 0) {
        return luaL_error(l, "mu.item %s: from is anywhere, bosses, lords or never, not '%s'", d.key.c_str(), from.c_str());
    }
    d.one_in = int_field(l, 1, "one_in", 0);
    d.source = current_file;
    std::string why = mu_items_define(d);
    if (!why.empty()) {
        return luaL_error(l, "mu.item %s: %s", d.key.c_str(), why.c_str());
    }
    lua_pushstring(l, d.key.c_str());
    return 1;
}

int l_tell(lua_State *l) {
    MuSession *p = player_arg(l, 1);
    mu_system(p, luaL_checkstring(l, 2));
    if (p != mu_self) {
        mu_conn_poke(p->conn);
    }
    return 0;
}

int l_say_level(lua_State *l) {
    need_world(l);
    std::string line = luaL_checkstring(l, 1);
    MuLevel *level = mu_self->level;
    if (level != nullptr) {
        for (MuSession *p : level->players) {
            mu_system(p, line);
            if (p != mu_self) {
                mu_conn_poke(p->conn);
            }
        }
    }
    return 0;
}

int l_say_world(lua_State *l) {
    mu_system_all(luaL_checkstring(l, 1));
    return 0;
}

int find_creature(std::string const &want) {
    std::string w = lower(want);
    for (int i = 0; i < MON_MAX_CREATURES; i++) {
        if (lower(creatures_list[i].name) == w) {
            return i;
        }
    }
    if (want.size() == 1) {
        // a letter: one of its creatures near this depth
        std::vector<int> found;
        for (int spread = 3; found.empty() && spread < 100; spread += 5) {
            for (int i = 0; i < MON_MAX_CREATURES; i++) {
                Creature_t const &c = creatures_list[i];
                if (c.sprite == (uint8_t) want[0] && c.level > 0 && std::abs(c.level - dg.current_level) <= spread) {
                    found.push_back(i);
                }
            }
        }
        return found.empty() ? -1 : found[(size_t) randomNumber((int) found.size()) - 1];
    }
    int partial = -1;
    for (int i = 0; i < MON_MAX_CREATURES; i++) {
        if (lower(creatures_list[i].name).find(w) != std::string::npos) {
            if (partial >= 0 && strcmp(creatures_list[i].name, creatures_list[partial].name) != 0) {
                return -2; // more than one
            }
            partial = i;
        }
    }
    return partial;
}

bool spawn_spot(Coord_t const &near, bool far, Coord_t &out) {
    for (int tries = 0; tries < 3000; tries++) {
        Coord_t c;
        if (far) {
            c = Coord_t{(int32_t) randomNumber(dg.height - 2), (int32_t) randomNumber(dg.width - 2)};
        } else {
            int r = 1 + tries / 40;
            if (r > 8) {
                return false;
            }
            c = Coord_t{near.y + (int32_t) randomNumber(2 * r + 1) - r - 1, near.x + (int32_t) randomNumber(2 * r + 1) - r - 1};
        }
        if (c.y <= 0 || c.x <= 0 || c.y >= dg.height - 1 || c.x >= dg.width - 1) {
            continue;
        }
        Tile_t const &t = dg.floor[c.y][c.x];
        if (t.feature_id > MAX_OPEN_SPACE || t.creature_id != 0 || muOtherPlayerAt(c)) {
            continue;
        }
        if (far && coordDistanceBetween(c, py.pos) < 20) {
            continue;
        }
        out = c;
        return true;
    }
    return false;
}

int boss_index(std::string const &key) {
    if (std::find(boss_tags.begin(), boss_tags.end(), key) == boss_tags.end()) {
        boss_tags.push_back(key);
    }
    return tags.get(key);
}

// mu.spawn{creature=, near= | far=true, count=, hp=, speed=, awake=, boss=}
int l_spawn(lua_State *l) {
    need_world(l);
    luaL_checktype(l, 1, LUA_TTABLE);
    std::string name = str_field(l, 1, "creature", "");
    int creature = find_creature(name);
    if (creature < 0) {
        return luaL_error(l, creature == -2 ? "mu.spawn: more than one creature has '%s' in its name" : "mu.spawn: no creature '%s'", name.c_str());
    }
    bool far = bool_field(l, 1, "far", false);
    Coord_t near = py.pos;
    lua_getfield(l, 1, "near");
    if (!lua_isnil(l, -1)) {
        near = coord_arg(l, lua_gettop(l));
    }
    lua_pop(l, 1);
    bool many = int_field(l, 1, "count", 0) > 0; // then a list; else the monster itself (or nil)
    int count = std::max(1, std::min(50, int_field(l, 1, "count", 1)));
    double hp = num_field(l, 1, "hp", 1.0);
    int speed = int_field(l, 1, "speed", 0);
    bool awake = bool_field(l, 1, "awake", !far);
    std::string boss = str_field(l, 1, "boss", ""), tag_name = str_field(l, 1, "tag", "");
    std::string shown = str_field(l, 1, "name", "");
    int tag = !boss.empty() ? boss_index(boss) : !tag_name.empty() ? tags.get(tag_name) : 0;
    int name_no = shown.empty() ? 0 : names.get(shown);
    std::string key = !boss.empty() ? boss : tag_name;

    lua_newtable(l);
    int made = 0;
    for (int i = 0; i < count; i++) {
        Coord_t c;
        if (!spawn_spot(near, far, c) || !monsterPlaceNew(c, creature, !awake)) {
            break;
        }
        int id = dg.floor[c.y][c.x].creature_id;
        Monster_t &m = monsters[id];
        m.hp = (int16_t) std::max(1.0, std::min(30000.0, m.hp * hp));
        m.speed = (int16_t) (m.speed + speed);
        m.mu_script = (uint8_t) tag;
        m.mu_phase = 0;
        m.mu_name = (uint8_t) name_no;
        if (tag != 0) {
            boss_max_hp[std::to_string(dg.current_level) + ":" + key] = m.hp;
        }
        monsterUpdateVisibility(id);
        push_monster(l, id);
        lua_rawseti(l, -2, ++made);
    }
    if (mu_level_loaded() != nullptr) {
        mu_level_loaded()->dirty = true;
    }
    if (!many) {
        lua_rawgeti(l, -1, 1);
    }
    return 1;
}

int l_monsters(lua_State *l) {
    need_world(l);
    lua_newtable(l);
    int n = 0;
    for (int i = config::monsters::MON_MIN_INDEX_ID; i < next_free_monster_id; i++) {
        if (monsters[i].hp >= 0) {
            push_monster(l, i);
            lua_rawseti(l, -2, ++n);
        }
    }
    return 1;
}

int l_find_boss(lua_State *l) {
    need_world(l);
    std::string key = luaL_checkstring(l, 1);
    for (int i = config::monsters::MON_MIN_INDEX_ID; i < next_free_monster_id; i++) {
        Monster_t const &m = monsters[i];
        if (m.hp >= 0 && tags.at(m.mu_script) != nullptr && *tags.at(m.mu_script) == key) {
            push_monster(l, i);
            return 1;
        }
    }
    lua_pushnil(l);
    return 1;
}

// mu.tagged(tag): every live monster here with that tag (or boss key)
int l_tagged(lua_State *l) {
    need_world(l);
    int n = tags.find(luaL_checkstring(l, 1)), count = 0;
    lua_newtable(l);
    for (int i = config::monsters::MON_MIN_INDEX_ID; n != 0 && i < next_free_monster_id; i++) {
        if (monsters[i].hp >= 0 && monsters[i].mu_script == n) {
            push_monster(l, i);
            lua_rawseti(l, -2, ++count);
        }
    }
    return 1;
}

int l_despawn(lua_State *l) {
    need_world(l);
    int id = monster_arg(l, 1);
    if (id >= 0) {
        Coord_t at{monsters[id].pos.y, monsters[id].pos.x};
        dungeonDeleteMonster(id);
        dungeonLiteSpot(at);
        mu_level_loaded()->dirty = true;
    }
    lua_pushboolean(l, id >= 0);
    return 1;
}

// --- portals: a store door (sub-category 112) with its key in the inscription

const uint8_t PORTAL = 112;

bool find_portal(std::string const &key, Coord_t &out) {
    for (int y = 0; y < dg.height; y++) {
        for (int x = 0; x < dg.width; x++) {
            int id = dg.floor[y][x].treasure_id;
            if (id != 0 && game.treasure.list[id].category_id == TV_STORE_DOOR && game.treasure.list[id].sub_category_id == PORTAL &&
                key == game.treasure.list[id].inscription) {
                out = Coord_t{y, x};
                return true;
            }
        }
    }
    return false;
}

// Open floor with nothing on it, nobody within three squares, no door near.
bool portal_spot(Coord_t &out) {
    for (int tries = 0; tries < 4000; tries++) {
        Coord_t c{(int32_t) randomNumber(dg.height - 2), (int32_t) randomNumber(dg.width - 2)};
        Tile_t const &t = dg.floor[c.y][c.x];
        if (t.feature_id > MAX_CAVE_FLOOR || t.treasure_id != 0 || t.creature_id != 0 || coordDistanceBetween(c, py.pos) < 4) {
            continue;
        }
        bool clear = true;
        for (int y = c.y - 3; clear && y <= c.y + 3; y++) {
            for (int x = c.x - 3; clear && x <= c.x + 3; x++) {
                if (y <= 0 || x <= 0 || y >= dg.height - 1 || x >= dg.width - 1) {
                    clear = false;
                } else if (dg.floor[y][x].treasure_id != 0 && game.treasure.list[dg.floor[y][x].treasure_id].category_id == TV_STORE_DOOR) {
                    clear = false;
                } else if (muOtherPlayerAt(Coord_t{y, x})) {
                    clear = false;
                }
            }
        }
        if (clear) {
            out = c;
            return true;
        }
    }
    return false;
}

void push_place(lua_State *l, Coord_t const &c) {
    lua_newtable(l);
    set_int(l, "x", c.x);
    set_int(l, "y", c.y);
}

// mu.portal{key=, at=, glyph=, colour=}: its place (nil if no room)
int l_portal(lua_State *l) {
    need_world(l);
    luaL_checktype(l, 1, LUA_TTABLE);
    std::string key = str_field(l, 1, "key", "");
    if (key.empty() || key.size() > INSCRIP_SIZE - 2) {
        return luaL_error(l, "mu.portal needs a key of 1 to %d letters", INSCRIP_SIZE - 2);
    }
    std::string glyph = str_field(l, 1, "glyph", "0");
    int colour = int_field(l, 1, "colour", 199);
    Coord_t at;
    if (find_portal(key, at)) {
        push_place(l, at); // already open
        return 1;
    }
    lua_getfield(l, 1, "at");
    bool given = !lua_isnil(l, -1);
    if (given) {
        at = coord_arg(l, lua_gettop(l));
    }
    lua_pop(l, 1);
    if ((given && dg.floor[at.y][at.x].treasure_id != 0) || (!given && !portal_spot(at))) {
        lua_pushnil(l);
        return 1;
    }
    int id = popt();
    Inventory_t &door = game.treasure.list[id];
    inventoryItemCopyTo(config::dungeon::objects::OBJ_STORE_DOOR, door);
    door.sub_category_id = PORTAL;
    door.sprite = (uint8_t) (glyph.empty() ? '0' : glyph[0]);
    door.to_ac = (int16_t) colour;
    snprintf(door.inscription, sizeof(door.inscription), "%s", key.c_str());
    dg.floor[at.y][at.x].treasure_id = (uint8_t) id;
    dg.floor[at.y][at.x].field_mark = true;
    dungeonLiteSpot(at);
    mu_level_loaded()->dirty = true;
    push_place(l, at);
    return 1;
}

int l_find_portal(lua_State *l) {
    need_world(l);
    Coord_t at;
    if (find_portal(luaL_checkstring(l, 1), at)) {
        push_place(l, at);
    } else {
        lua_pushnil(l);
    }
    return 1;
}

int l_close_portal(lua_State *l) {
    need_world(l);
    Coord_t at;
    bool found = find_portal(luaL_checkstring(l, 1), at);
    if (found) {
        (void) dungeonDeleteObject(at);
        dungeonLiteSpot(at);
        mu_level_loaded()->dirty = true;
    }
    lua_pushboolean(l, found ? 1 : 0);
    return 1;
}

// mu.hurt(monster, damage): as if the player in the event struck it (the
// kill is theirs, its events fire). True if it died.
int l_hurt(lua_State *l) {
    need_world(l);
    int id = monster_arg(l, 1);
    int damage = (int) luaL_checkinteger(l, 2);
    if (id < 0 || damage <= 0) {
        lua_pushboolean(l, 0);
        return 1;
    }
    Coord_t at{monsters[id].pos.y, monsters[id].pos.x};
    std::string name = muMonsterName(monsters[id]);
    bool died = monsterTakeHit(id, damage) >= 0;
    if (died) {
        printMessage(("The " + name + " dies.").c_str());
        dungeonLiteSpot(at);
    }
    mu_level_loaded()->dirty = true;
    lua_pushboolean(l, died ? 1 : 0);
    return 1;
}

// --- floor items, and monsters a script walks ----------------------------------

bool pickable(Coord_t const &c) {
    int id = dg.floor[c.y][c.x].treasure_id;
    return id != 0 && game.treasure.list[id].category_id <= TV_MAX_PICK_UP;
}

// mu.items(): every item on this level's floor, {x, y, name, age}
int l_items(lua_State *l) {
    need_world(l);
    lua_newtable(l);
    int n = 0;
    for (int y = 1; y < dg.height - 1; y++) {
        for (int x = 1; x < dg.width - 1; x++) {
            Coord_t c{y, x};
            if (!pickable(c)) {
                continue;
            }
            obj_desc_t name;
            itemDescription(name, game.treasure.list[dg.floor[y][x].treasure_id], true);
            push_place(l, c);
            set_str(l, "name", name);
            set_int(l, "age", std::max(0, mu_town_item_age(c)));
            lua_rawseti(l, -2, ++n);
        }
    }
    return 1;
}

// mu.take(place): the item there is gone; its name, or nil
int l_take(lua_State *l) {
    need_world(l);
    Coord_t at = coord_arg(l, 1);
    if (!pickable(at)) {
        lua_pushnil(l);
        return 1;
    }
    obj_desc_t name;
    itemDescription(name, game.treasure.list[dg.floor[at.y][at.x].treasure_id], true);
    (void) dungeonDeleteObject(at);
    dungeonLiteSpot(at);
    mu_level_loaded()->dirty = true;
    lua_pushstring(l, name);
    return 1;
}

// Open floor a walker may cross: no wall, door or stairs (items are fine).
bool walkable(Coord_t const &c) {
    Tile_t const &t = dg.floor[c.y][c.x];
    return c.y > 0 && c.x > 0 && c.y < dg.height - 1 && c.x < dg.width - 1 && t.feature_id <= MAX_OPEN_SPACE &&
           (t.treasure_id == 0 || pickable(c));
}

// mu.walk(monster, place, steps): up to `steps` squares along the shortest way
// there, waiting when someone is in it. Where it is now, or nil if it cannot get there.
int l_walk(lua_State *l) {
    need_world(l);
    int id = monster_arg(l, 1);
    Coord_t to = coord_arg(l, 2);
    int steps = std::max(1, std::min(10, (int) luaL_optinteger(l, 3, 1)));
    if (id < 0) {
        lua_pushnil(l);
        return 1;
    }
    // squares from `to`, breadth first
    static std::vector<int16_t> dist;
    dist.assign((size_t) MAX_HEIGHT * MAX_WIDTH, -1);
    std::vector<Coord_t> queue{to};
    dist[(size_t) (to.y * MAX_WIDTH + to.x)] = 0;
    for (size_t q = 0; q < queue.size(); q++) {
        Coord_t c = queue[q];
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                Coord_t n{c.y + dy, c.x + dx};
                if (walkable(n) && dist[(size_t) (n.y * MAX_WIDTH + n.x)] < 0) {
                    dist[(size_t) (n.y * MAX_WIDTH + n.x)] = (int16_t) (dist[(size_t) (c.y * MAX_WIDTH + c.x)] + 1);
                    queue.push_back(n);
                }
            }
        }
    }
    Monster_t &m = monsters[id];
    if (dist[(size_t) (m.pos.y * MAX_WIDTH + m.pos.x)] < 0) {
        lua_pushnil(l);
        return 1;
    }
    for (int s = 0; s < steps; s++) {
        Coord_t here{m.pos.y, m.pos.x}, best = here;
        int best_d = dist[(size_t) (here.y * MAX_WIDTH + here.x)];
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                Coord_t n{here.y + dy, here.x + dx};
                int d = walkable(n) ? dist[(size_t) (n.y * MAX_WIDTH + n.x)] : -1;
                if (d >= 0 && d < best_d && dg.floor[n.y][n.x].creature_id == 0 && !muOtherPlayerAt(n)) {
                    best = n;
                    best_d = d;
                }
            }
        }
        if (best.y == here.y && best.x == here.x) {
            break; // there, or someone is in the way
        }
        dungeonMoveCreatureRecord(here, best);
        if (m.lit) {
            m.lit = false;
            dungeonLiteSpot(here);
        }
        m.pos.y = best.y;
        m.pos.x = best.x;
        m.distance_from_player = (uint8_t) coordDistanceBetween(py.pos, best);
        monsterUpdateVisibility(id);
    }
    mu_level_loaded()->dirty = true;
    push_place(l, Coord_t{m.pos.y, m.pos.x});
    return 1;
}

// mu.hold(tag): the engine gives monsters with this tag no turns of their own
int l_hold(lua_State *l) {
    int tag = tags.get(luaL_checkstring(l, 1));
    if (tag != 0) {
        held.insert(tag);
    }
    return 0;
}

// mu.town_expiry(on): false leaves clearing the town's floor to the script
int l_town_expiry(lua_State *l) {
    mu_town_expiry = lua_toboolean(l, 1) != 0;
    return 0;
}

int l_set_phase(lua_State *l) {
    int id = monster_arg(l, 1);
    if (id >= 0) {
        monsters[id].mu_phase = (uint8_t) luaL_checkinteger(l, 2);
    }
    lua_pushboolean(l, id >= 0);
    return 1;
}

int l_heal(lua_State *l) {
    int id = monster_arg(l, 1);
    if (id >= 0) {
        monsters[id].hp = (int16_t) std::min<lua_Integer>(30000, monsters[id].hp + luaL_checkinteger(l, 2));
    }
    lua_pushboolean(l, id >= 0);
    return 1;
}

int l_drop(lua_State *l) {
    need_world(l);
    std::string what = luaL_checkstring(l, 1);
    Coord_t at = lua_isnoneornil(l, 2) ? py.pos : coord_arg(l, 2);
    MuCustomItem const *d = mu_item_by_key(what);
    bool ok;
    if (d != nullptr) {
        ok = mu_items_place(*d, at);
    } else {
        int tier = mu_rarity_by_name(lower(what));
        if (tier < 0) {
            return luaL_error(l, "mu.drop: no item or rarity '%s'", what.c_str());
        }
        ok = mu_loot_place_tier(tier, at);
    }
    lua_pushboolean(l, ok ? 1 : 0);
    return 1;
}

int l_give(lua_State *l) {
    need_world(l);
    MuSession *p = player_arg(l, 1);
    std::string key = luaL_checkstring(l, 2);
    MuCustomItem const *d = mu_item_by_key(key);
    Inventory_t item{};
    if (d != nullptr) {
        mu_items_make(*d, item);
    } else {
        int tier = mu_rarity_by_name(lower(key));
        if (tier < 0) {
            return luaL_error(l, "mu.give: no item or rarity '%s'", key.c_str());
        }
        if (!mu_loot_make_tier(tier, item)) {
            lua_pushboolean(l, 0);
            return 1;
        }
    }
    bool given = false;
    mu_with_player(mu_self, p, [&] {
        if (inventoryCanCarryItemCount(item)) {
            inventoryCarryItem(item);
            given = true;
        }
    });
    lua_pushboolean(l, given ? 1 : 0);
    return 1;
}

int l_teleport(lua_State *l) {
    need_world(l);
    MuSession *p = player_arg(l, 1);
    Coord_t at = coord_arg(l, 2);
    if (p->level != mu_self->level) {
        return luaL_error(l, "mu.teleport: %s is on another level", p->char_name.c_str());
    }
    mu_with_player(mu_self, p, [&] { spellTeleportPlayerTo(at); });
    return 0;
}

int l_player(lua_State *l) {
    std::string name = luaL_checkstring(l, 1);
    pthread_mutex_lock(&mu_sessions_lock);
    MuSession *p = mu_find_online(name.c_str());
    pthread_mutex_unlock(&mu_sessions_lock);
    if (p == nullptr || !p->in_game) {
        lua_pushnil(l);
    } else {
        push_player(l, p);
    }
    return 1;
}

int l_players(lua_State *l) {
    need_world(l);
    lua_newtable(l);
    int n = 0;
    if (mu_self->level != nullptr) {
        for (MuSession *p : mu_self->level->players) {
            push_player(l, p);
            lua_rawseti(l, -2, ++n);
        }
    }
    return 1;
}

int l_glow(lua_State *l) {
    need_world(l);
    Coord_t at = coord_arg(l, 1);
    int tier = mu_rarity_by_name(lower(luaL_optstring(l, 2, "epic")));
    muFxLoot(at, tier < 0 ? MU_EPIC : tier);
    return 0;
}

int l_random(lua_State *l) {
    lua_Integer n = luaL_checkinteger(l, 1);
    lua_pushinteger(l, n <= 1 ? 1 : randomNumber((int) std::min<lua_Integer>(n, 1 << 30)));
    return 1;
}

int l_now(lua_State *l) {
    lua_pushinteger(l, (lua_Integer) time(nullptr));
    return 1;
}

int l_depth(lua_State *l) {
    lua_pushinteger(l, mu_level_loaded() != nullptr ? dg.current_level : 0);
    return 1;
}

int l_log(lua_State *l) {
    fprintf(stderr, "mumoria script %s: %s\n", current_file.c_str(), luaL_tolstring(l, 1, nullptr));
    return 0;
}

int l_print(lua_State *l) {
    std::string line;
    for (int i = 1; i <= lua_gettop(l); i++) {
        line += (i > 1 ? "\t" : "") + std::string(luaL_tolstring(l, i, nullptr));
        lua_pop(l, 1);
    }
    fprintf(stderr, "mumoria script: %s\n", line.c_str());
    return 0;
}

int l_set(lua_State *l) {
    std::string key = luaL_checkstring(l, 1);
    if (lua_isnoneornil(l, 2)) {
        mu_db_run("DELETE FROM script_state WHERE key = ?", "s", key.c_str());
    } else {
        std::string value = luaL_tolstring(l, 2, nullptr);
        mu_db_run("INSERT INTO script_state (key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value", "ss", key.c_str(),
                  value.c_str());
    }
    return 0;
}

int l_get(lua_State *l) {
    std::string key = luaL_checkstring(l, 1);
    MuRows *r = mu_db_query("SELECT value FROM script_state WHERE key = ?", "s", key.c_str());
    if (r != nullptr && mu_db_next(r)) {
        lua_pushstring(l, mu_db_col_text(r, 0));
    } else {
        lua_pushnil(l);
    }
    mu_db_done(r);
    return 1;
}

int l_error(lua_State *l) {
    report(luaL_checkstring(l, 1));
    return 0;
}

int l_boss_key(lua_State *l) {
    lua_pushboolean(l, boss_index(luaL_checkstring(l, 1)) != 0);
    return 1;
}

const luaL_Reg MU[] = {
    {"item", l_item},         {"tell", l_tell},     {"say_level", l_say_level}, {"say_world", l_say_world}, {"spawn", l_spawn},
    {"monsters", l_monsters}, {"find_boss", l_find_boss}, {"tagged", l_tagged}, {"despawn", l_despawn},
    {"portal", l_portal},     {"find_portal", l_find_portal}, {"close_portal", l_close_portal}, {"set_phase", l_set_phase}, {"hurt", l_hurt}, {"heal", l_heal},   {"drop", l_drop},
    {"items", l_items},       {"take", l_take},     {"walk", l_walk},           {"hold", l_hold},           {"town_expiry", l_town_expiry},
    {"give", l_give},         {"teleport", l_teleport}, {"player", l_player}, {"players", l_players},     {"glow", l_glow},           {"random", l_random},
    {"now", l_now},           {"depth", l_depth},   {"log", l_log},             {"set", l_set},             {"get", l_get},
    {"_error", l_error},      {"_boss_key", l_boss_key}, {nullptr, nullptr},
};

// mu.on, mu.command and mu.boss, in Lua.
const char *PRELUDE = R"LUA(
local handlers = {enter = {}, kill = {}, hurt = {}, portal = {}}
local commands = {}
local timers = {}

function mu.on(event, fn)
  assert(handlers[event], "mu.on: no event '" .. tostring(event) .. "' (enter, kill, hurt, portal)")
  assert(type(fn) == "function", "mu.on needs a function")
  table.insert(handlers[event], fn)
end

function mu._fire(event, ...)
  for _, fn in ipairs(handlers[event]) do
    local ok, err = pcall(fn, ...)
    if not ok then mu._error(tostring(err)) end
  end
end

-- fn(player, level) every `seconds` (on some player's turn), on `depth` only if given
function mu.every(seconds, fn, depth)
  assert(type(seconds) == "number" and type(fn) == "function", "mu.every(seconds, fn, depth)")
  table.insert(timers, {every = seconds, fn = fn, depth = depth, due = 0})
end

function mu._tick(p, level)
  local now = mu.now()
  for _, t in ipairs(timers) do
    if (t.depth == nil or t.depth == level.depth) and now >= t.due then
      t.due = now + t.every
      local ok, err = pcall(t.fn, p, level)
      if not ok then mu._error(tostring(err)) end
    end
  end
end

function mu.command(name, fn, help)
  assert(type(name) == "string" and type(fn) == "function", "mu.command(name, fn, help)")
  commands[name:lower()] = {fn = fn, help = help or ""}
end

function mu._command(name, player, args)
  local c = commands[name]
  if not c then return false end
  c.fn(player, args)
  return true
end

function mu._command_list()
  local names = {}
  for name in pairs(commands) do names[#names + 1] = "/" .. name end
  table.sort(names)
  return table.concat(names, " ")
end

-- mu.boss{key=, depth=, creature=, title=, hp=, speed=, respawn_hours=,
--         guards={creature=, count=}, phases={{at=, say=, spawn={creature=, count=}, heal=}},
--         loot={items={...}, tier=, count=}, announce=, defeat=}
local bosses = {}

function mu.boss(b)
  assert(type(b.key) == "string", "mu.boss needs a key")
  assert(type(b.creature) == "string", "mu.boss " .. b.key .. " needs a creature")
  b.depth = b.depth or 1
  bosses[b.key] = b
  mu._boss_key(b.key)
end

local function due(b)
  return mu.now() >= tonumber(mu.get("boss:" .. b.key) or "0")
end

mu.on("enter", function(p, level)
  for key, b in pairs(bosses) do
    if level.depth == b.depth and not level.raid then
      local m = mu.find_boss(key)
      if not m and due(b) then
        m = mu.spawn{creature = b.creature, far = true, hp = b.hp or 5, speed = b.speed or 1, boss = key}
        if m and b.guards then
          mu.spawn{creature = b.guards.creature, near = m, count = b.guards.count or 3, hp = b.guards.hp or 1}
        end
      end
      if m and b.announce then mu.tell(p, b.announce) end
    end
  end
end)

mu.on("hurt", function(p, m)
  local b = m.boss and bosses[m.boss]
  if not b or not b.phases then return end
  local pct = m.hp * 100 // math.max(1, m.max_hp)
  local phase = b.phases[m.phase + 1]
  while phase and pct <= phase.at do
    m.phase = m.phase + 1
    mu.set_phase(m, m.phase)
    if phase.say then mu.say_level(phase.say) end
    if phase.spawn then
      mu.spawn{creature = phase.spawn.creature, near = m, count = phase.spawn.count or 2, hp = phase.spawn.hp or 1}
    end
    if phase.heal then mu.heal(m, phase.heal) end
    phase = b.phases[m.phase + 1]
  end
end)

mu.on("kill", function(p, m)
  local b = m.boss and bosses[m.boss]
  if not b then return end
  mu.set("boss:" .. b.key, mu.now() + math.floor((b.respawn_hours or 2) * 3600))
  local loot = b.loot or {}
  for _, it in ipairs(loot.items or {}) do
    local key, one_in = it, 1
    if type(it) == "table" then key, one_in = it.key, it.one_in or 1 end
    if mu.random(one_in) == 1 then mu.drop(key, m) end
  end
  for _ = 1, loot.count or 0 do mu.drop(loot.tier or "rare", m) end
  if b.defeat then mu.say_world((b.defeat:gsub("{player}", p.name))) end
end)
)LUA";

void open_state() {
    memory = 0;
    held.clear();
    mu_town_expiry = true; // until a script takes the job
    L = lua_newstate(alloc, nullptr);
    lua_atpanic(L, [](lua_State *l) -> int {
        fprintf(stderr, "mumoria script: panic: %s\n", lua_tostring(l, -1));
        return 0;
    });
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1);
    lua_pop(L, 6);
    // no files, no loading code from strings or bytecode
    for (const char *name : {"dofile", "loadfile", "load", "require"}) {
        lua_pushnil(L);
        lua_setglobal(L, name);
    }
    lua_getglobal(L, "string");
    lua_pushnil(L);
    lua_setfield(L, -2, "dump");
    lua_pop(L, 1);
    lua_register(L, "print", l_print);
    luaL_newlib(L, MU);
    lua_setglobal(L, "mu");
    lua_sethook(L, count_hook, LUA_MASKCOUNT, STEP_HOOK);

    current_file = "(prelude)";
    if (luaL_loadbufferx(L, PRELUDE, strlen(PRELUDE), "=prelude", "t") != LUA_OK) {
        report(lua_tostring(L, -1));
        lua_pop(L, 1);
    } else {
        (void) call(0, 0);
    }
}

void load_all() {
    loaded.clear();
    std::vector<std::string> files;
    DIR *d = opendir(script_dir.c_str());
    if (d != nullptr) {
        for (struct dirent *e = readdir(d); e != nullptr; e = readdir(d)) {
            std::string name = e->d_name;
            if (name.size() > 4 && name.compare(name.size() - 4, 4, ".lua") == 0 && name[0] != '.') {
                files.push_back(name);
            }
        }
        closedir(d);
    }
    std::sort(files.begin(), files.end());
    for (std::string const &name : files) {
        current_file = name;
        std::string path = script_dir + "/" + name;
        if (luaL_loadfilex(L, path.c_str(), "t") != LUA_OK) {
            report(lua_tostring(L, -1));
            lua_pop(L, 1);
            continue;
        }
        if (call(0, 0)) {
            loaded.push_back(name);
        }
    }
    current_file.clear();
}

// stack: mu._fire, the event's name, its nargs arguments
void fire(int nargs) {
    (void) call(nargs + 1, 0);
}

bool begin_fire(const char *event) {
    if (!get_mu("_fire")) {
        return false;
    }
    lua_pushstring(L, event);
    return true;
}

} // namespace

void mu_script_init(std::string const &dir) {
    script_dir = dir.empty() ? "scripts" : dir;
    tags.load();
    names.load();
    errors.clear();
    mu_items_scripts_unloaded();
    open_state();
    load_all();
    fprintf(stderr, "mumoria: %d script(s) loaded from %s\n", (int) loaded.size(), script_dir.c_str());
}

std::string mu_script_reload() {
    if (L != nullptr) {
        lua_close(L);
        L = nullptr;
    }
    errors.clear();
    mu_items_scripts_unloaded();
    open_state();
    load_all();
    std::string out = std::to_string(loaded.size()) + " script(s) loaded from " + script_dir;
    if (!errors.empty()) {
        out += ", " + std::to_string(errors.size()) + " error(s): /scripts shows them";
    }
    return out + ".";
}

void push_level(MuLevel *level) {
    lua_newtable(L);
    set_int(L, "depth", level->depth);
    set_int(L, "players", (lua_Integer) level->players.size());
    if (mu_raid_title(level) != nullptr) {
        set_str(L, "raid", mu_raid_title(level));
    }
}

void mu_script_enter(MuSession *s, MuLevel *level) {
    if (!begin_fire("enter")) {
        return;
    }
    push_player(L, s);
    push_level(level);
    fire(2);
}

void mu_script_tick(MuSession *s) {
    if (L == nullptr || s->level == nullptr || mu_level_loaded() != s->level) {
        return;
    }
    time_t now = time(nullptr);
    time_t &last = ticked[s->level];
    if (now == last || !get_mu("_tick")) {
        return;
    }
    last = now;
    push_player(L, s);
    push_level(s->level);
    (void) call(2, 0);
}

bool mu_script_is_boss(int tag) {
    std::string const *key = tags.at(tag);
    return key != nullptr && std::find(boss_tags.begin(), boss_tags.end(), *key) != boss_tags.end();
}

bool muMonsterHeld(Monster_t const &monster) {
    return monster.mu_script != 0 && held.count(monster.mu_script) != 0;
}

const char *muMonsterName(Monster_t const &monster) {
    std::string const *name = names.at(monster.mu_name);
    return name != nullptr ? name->c_str() : creatures_list[monster.creature_id].name;
}

void muScriptPortal(const char *key) {
    if (mu_self == nullptr || !begin_fire("portal")) {
        return;
    }
    push_player(L, mu_self);
    lua_pushstring(L, key);
    fire(2);
}

void mu_script_kill(int monster_id) {
    if (mu_self == nullptr || !begin_fire("kill")) {
        return;
    }
    push_player(L, mu_self);
    push_monster(L, monster_id);
    fire(2);
}

void mu_script_hurt(int monster_id) {
    if (mu_self == nullptr || monsters[monster_id].mu_script == 0 || !begin_fire("hurt")) {
        return;
    }
    push_player(L, mu_self);
    push_monster(L, monster_id);
    fire(2);
}

bool mu_script_command(MuSession *s, std::string const &cmd, std::string const &args) {
    if (!get_mu("_command")) {
        return false;
    }
    lua_pushstring(L, cmd.c_str());
    push_player(L, s);
    lua_pushstring(L, args.c_str());
    if (!call(3, 1)) {
        return true; // it was theirs, and it failed (reported)
    }
    bool handled = lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return handled;
}

std::string mu_script_command_list() {
    if (!get_mu("_command_list") || !call(0, 1)) {
        return "";
    }
    std::string out = lua_tostring(L, -1) != nullptr ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    return out;
}

bool mu_script_admin(MuSession *s, std::string const &cmd) {
    if (cmd == "reload") {
        mu_system(s, "[Script] " + mu_script_reload());
        mu_db_audit(s->account.name, "reload", script_dir.c_str(), "");
        return true;
    }
    if (cmd == "scripts") {
        std::string files;
        for (std::string const &f : loaded) {
            files += (files.empty() ? "" : ", ") + f;
        }
        mu_system(s, "[Script] " + std::to_string(loaded.size()) + " loaded from " + script_dir + (files.empty() ? "." : ": " + files) +
                         " Memory " + std::to_string(memory / 1024) + " KB.");
        for (std::string const &e : errors) {
            mu_system(s, "  " + e.substr(0, 200));
        }
        if (errors.empty()) {
            mu_system(s, "  No errors.");
        }
        return true;
    }
    return false;
}
