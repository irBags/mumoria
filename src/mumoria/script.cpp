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
const int STEP_HOOK = 1000; // instructions between counts
const int STEP_MAX = 5000;  // counts in one call: 5 million instructions

std::vector<std::string> boss_keys;     // Monster_t::mu_script - 1
std::map<std::string, int> boss_max_hp; // "depth:key" -> hit points when made

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
    steps = 0;
    int rc = lua_pcall(L, nargs, nresults, base);
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
    set_str(l, "name", c.name);
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
    int max_hp = m.hp;
    if (m.mu_script > 0 && m.mu_script <= boss_keys.size()) {
        std::string const &key = boss_keys[m.mu_script - 1];
        set_str(l, "boss", key);
        int &known = boss_max_hp[std::to_string(dg.current_level) + ":" + key];
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
    for (size_t i = 0; i < boss_keys.size(); i++) {
        if (boss_keys[i] == key) {
            return (int) i + 1;
        }
    }
    if (boss_keys.size() >= 255) {
        return 0;
    }
    boss_keys.push_back(key);
    return (int) boss_keys.size();
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
    std::string boss = str_field(l, 1, "boss", "");
    int tag = boss.empty() ? 0 : boss_index(boss);

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
        if (tag != 0) {
            boss_max_hp[std::to_string(dg.current_level) + ":" + boss] = m.hp;
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
        if (m.hp >= 0 && m.mu_script > 0 && m.mu_script <= boss_keys.size() && boss_keys[m.mu_script - 1] == key) {
            push_monster(l, i);
            return 1;
        }
    }
    lua_pushnil(l);
    return 1;
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
    if (d == nullptr) {
        return luaL_error(l, "mu.give: no item '%s'", key.c_str());
    }
    MuCustomItem def = *d;
    bool given = false;
    mu_with_player(mu_self, p, [&] {
        Inventory_t item{};
        mu_items_make(def, item);
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
    {"monsters", l_monsters}, {"find_boss", l_find_boss}, {"set_phase", l_set_phase}, {"heal", l_heal},   {"drop", l_drop},
    {"give", l_give},         {"teleport", l_teleport}, {"player", l_player}, {"players", l_players},     {"glow", l_glow},           {"random", l_random},
    {"now", l_now},           {"depth", l_depth},   {"log", l_log},             {"set", l_set},             {"get", l_get},
    {"_error", l_error},      {"_boss_key", l_boss_key}, {nullptr, nullptr},
};

// mu.on, mu.command and mu.boss, in Lua.
const char *PRELUDE = R"LUA(
local handlers = {enter = {}, kill = {}, hurt = {}}
local commands = {}

function mu.on(event, fn)
  assert(handlers[event], "mu.on: no event '" .. tostring(event) .. "' (enter, kill, hurt)")
  assert(type(fn) == "function", "mu.on needs a function")
  table.insert(handlers[event], fn)
end

function mu._fire(event, ...)
  for _, fn in ipairs(handlers[event]) do
    local ok, err = pcall(fn, ...)
    if not ok then mu._error(tostring(err)) end
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

void mu_script_enter(MuSession *s, MuLevel *level) {
    if (!begin_fire("enter")) {
        return;
    }
    push_player(L, s);
    lua_newtable(L);
    set_int(L, "depth", level->depth);
    if (mu_raid_title(level) != nullptr) {
        set_str(L, "raid", mu_raid_title(level));
    }
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
