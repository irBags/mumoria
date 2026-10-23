// Mumoria - boss encounters (docs/MUMORIA_DESIGN.md 2.16).
//
// Every MU_BOSS_INTERVAL levels a boss rules the level: a creature from a
// little deeper, much tougher, that comes back MU_BOSS_RESPAWN_HOURS after it
// dies. Its loot is rolled deeper than the level, and deeper again for each
// boss the killer has slain before (character.boss_kills, capped), so each
// boss a character beats makes the next one's drops better.

#include "boss.h"

#include "mu_config.h"
#include "raid.h"
#include "session.h"
#include "social.h"

bool mu_boss_level(int depth) {
    return depth > 0 && depth % MU_BOSS_INTERVAL == 0;
}

static int live_boss() {
    for (int i = config::monsters::MON_MIN_INDEX_ID; i < next_free_monster_id; i++) {
        if (monsters[i].mu_boss != 0 && monsters[i].hp >= 0) {
            return i;
        }
    }
    return 0;
}

// A creature from depth to depth + 5: never a town monster or the Balrog.
static int pick_creature(int depth) {
    std::vector<int> found;
    for (int spread = 5; found.empty() && spread < 100; spread += 5) {
        for (int i = 0; i < MON_MAX_CREATURES; i++) {
            Creature_t const &c = creatures_list[i];
            if (c.level > 0 && c.level >= depth && c.level <= depth + spread && (c.movement & config::monsters::move::CM_WIN) == 0) {
                found.push_back(i);
            }
        }
    }
    return found.empty() ? -1 : found[(size_t) (randomNumber((int) found.size()) - 1)];
}

// beside: next to the loaded player (an admin's /spawnboss); else far from them.
static bool place_boss(MuLevel *level, bool beside = false) {
    int creature = pick_creature(level->depth);
    if (creature < 0) {
        return false;
    }
    for (int tries = 0; tries < 5000; tries++) {
        Coord_t c{(int32_t) randomNumber(dg.height - 2), (int32_t) randomNumber(dg.width - 2)};
        if (beside) {
            c = Coord_t{py.pos.y + (int32_t) randomNumber(3) - 2, py.pos.x + (int32_t) randomNumber(3) - 2};
        }
        Tile_t const &t = dg.floor[c.y][c.x];
        if (t.feature_id >= MIN_CLOSED_SPACE || t.creature_id != 0 || t.treasure_id != 0 || muOtherPlayerAt(c)) {
            continue;
        }
        if (!beside && py.pos.y > 0 && coordDistanceBetween(c, py.pos) < 20) {
            continue;
        }
        if (!monsterPlaceNew(c, creature, !beside)) {
            return false;
        }
        Monster_t &m = monsters[dg.floor[c.y][c.x].creature_id];
        m.mu_boss = 1;
        int hp = m.hp * (3 + level->depth / 10);
        m.hp = (int16_t) (hp > 30000 ? 30000 : hp);
        if (level->depth > 20) {
            m.speed++;
        }
        level->boss_dead_at = 0;
        level->dirty = true;
        return true;
    }
    return false;
}

bool mu_boss_spawn_beside(MuLevel *level) {
    if (level->depth == 0 || !place_boss(level, true)) {
        return false;
    }
    updateMonsters(false);
    return true;
}

void mu_boss_place(MuLevel *level) {
    if (mu_boss_level(level->depth) && live_boss() == 0) {
        place_boss(level);
    }
}

void mu_boss_announce(MuSession *s, MuLevel *level) {
    if (!mu_boss_level(level->depth)) {
        return;
    }
    int boss = live_boss();
    if (boss == 0 && level->boss_dead_at != 0 && time(nullptr) - level->boss_dead_at >= MU_BOSS_RESPAWN_HOURS * 3600) {
        place_boss(level);
        boss = live_boss();
    }
    if (boss != 0) {
        mu_system(s, std::string("You sense a powerful presence: ") + creatures_list[monsters[boss].creature_id].name + " rules this level.");
    }
}

// A loot square near the boss: open floor with nothing on it.
static bool loot_spot(Coord_t const &center, Coord_t &out) {
    for (int tries = 0; tries < 60; tries++) {
        int r = 1 + tries / 20;
        Coord_t c{center.y + (int32_t) randomNumber(2 * r + 1) - r - 1, center.x + (int32_t) randomNumber(2 * r + 1) - r - 1};
        if (c.y <= 0 || c.x <= 0 || c.y >= dg.height - 1 || c.x >= dg.width - 1) {
            continue;
        }
        Tile_t const &t = dg.floor[c.y][c.x];
        if (t.feature_id <= MAX_CAVE_FLOOR && t.treasure_id == 0) {
            out = c;
            return true;
        }
    }
    return false;
}

static void boss_dies(int monster_id) {
    Monster_t &m = monsters[monster_id];
    m.mu_boss = 0;
    MuSession *s = mu_self;
    int depth = dg.current_level;
    int tier = (int) mu_db_int("SELECT boss_kills FROM character WHERE id = ?", 0, "i", s->char_id);
    if (tier > MU_BOSS_TIER_CAP) {
        tier = MU_BOSS_TIER_CAP;
    }
    int loot_level = depth + 10 + 2 * tier;
    if (loot_level > TREASURE_MAX_LEVELS) {
        loot_level = TREASURE_MAX_LEVELS;
    }

    Coord_t center{m.pos.y, m.pos.x};
    int items = 2 + depth / 10;
    for (int i = 0; i < items; i++) {
        Coord_t c;
        if (!loot_spot(center, c)) {
            break;
        }
        // dungeonPlaceRandomObjectAt(), rolled at the boss's loot level
        int free_id = popt();
        dg.floor[c.y][c.x].treasure_id = (uint8_t) free_id;
        int object_id = itemGetRandomObjectId(loot_level, false);
        inventoryItemCopyTo(sorted_objects[object_id], game.treasure.list[free_id]);
        magicTreasureMagicalAbility(free_id, loot_level);
        dungeonLiteSpot(c);
    }
    for (int i = 0; i < 3; i++) {
        Coord_t c;
        if (loot_spot(center, c)) {
            dungeonPlaceGold(c);
            dungeonLiteSpot(c);
        }
    }

    const char *name = creatures_list[m.creature_id].name;
    printMessage("A great treasure spills across the floor!");
    mu_db_run("UPDATE character SET boss_kills = boss_kills + 1 WHERE id = ?", "i", s->char_id);
    if (s->level != nullptr) {
        s->level->boss_dead_at = time(nullptr);
        s->level->dirty = true;
    }
    char where[32];
    mu_system_all(s->char_name + " has slain " + name + ", the boss of " + mu_depth_name(depth, where, sizeof(where)) + "!");
}

void muMonsterDies(int monster_id, int treasure_before) {
    Monster_t &m = monsters[monster_id];
    Coord_t center{m.pos.y, m.pos.x};
    bool lord = m.mu_boss == 3;
    if (m.mu_boss == 1) {
        boss_dies(monster_id);
    } else if (m.mu_boss >= 2) {
        mu_raid_monster_dies(monster_id); // raid.cpp
    }
    mu_loot_rolls(treasure_before, center);
    if (lord) {
        mu_raid_rewards(center); // after the rolls: nobody else rolls for a reward left on the floor
    }
}
