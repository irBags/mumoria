// Mumoria - raids (docs/MUMORIA_DESIGN.md 2.20).
//
// On every fifth level below the town a raid gate ('0') stands somewhere.
// Stepping on it shows the raid; entering makes a level for the party: no
// stairs, packs of themed monsters all through it, two captains with their
// guards, and the raid's lord at the far end, who calls reinforcements as it
// weakens. The raid grows with the party that enters it. The lord's death
// spills treasure for the party to roll for, and gives everyone inside a
// reward of their own once per MU_RAID_LOCKOUT_HOURS. The way out ('0', where
// the party came in) leads back beside the gate.

#include "raid.h"

#include "items.h"
#include "loot.h"
#include "mu_config.h"
#include "script.h"
#include "session.h"
#include "social.h"

namespace {

struct Theme {
    const char *name;    // "the Orc Warrens"
    const char *label;   // on the depth line
    const char *letters; // the monsters it is made of
    int lo, hi;          // their levels
    int lord_max;        // the lord: the strongest of them up to this level
    const char *about;
};

const Theme THEMES[] = {
    {"the Orc Warrens", "Orc Warrens", "okzsj", 1, 13, 18, "Orcs hold the warrens: dozens of them, their captains and their lord."},
    {"the Ogre Hold", "Ogre Hold", "oP", 9, 18, 20, "Ogres and giants hold this keep, and a giant lord rules them."},
    {"the Troll Caves", "Troll Caves", "TPo", 13, 25, 28, "Trolls by the score fill the caves; the oldest waits deepest in."},
    {"the Barrow Downs", "Barrow Downs", "zsGWV", 12, 30, 34, "The dead walk the barrows. Something older commands them."},
    {"the Lich Crypts", "Lich Crypts", "WVLGs", 24, 36, 37, "Wraiths and vampires guard the crypts of a lich."},
    {"the Dragon Nests", "Dragon Nests", "d", 29, 37, 38, "Young dragons nest here, and their mother watches over them."},
    {"the Dragons' Deep", "Dragons Deep", "dD", 35, 40, 40, "Ancient dragons sleep here on their hoards. Not all of them sleep."},
};
const int THEME_COUNT = (int) (sizeof(THEMES) / sizeof(THEMES[0]));

const uint8_t GATE = 110, WAY_OUT = 111; // store door sub-categories

int theme_of(int gate) {
    int t = gate / MU_RAID_INTERVAL - 1;
    return t < 0 ? 0 : t >= THEME_COUNT ? THEME_COUNT - 1 : t;
}

bool themed(Creature_t const &c, Theme const &t) {
    return c.level > 0 && strchr(t.letters, c.sprite) != nullptr && (c.movement & config::monsters::move::CM_WIN) == 0;
}

std::vector<int> creatures(Theme const &t, int lo, int hi) {
    std::vector<int> out;
    for (int i = 0; i < MON_MAX_CREATURES; i++) {
        if (themed(creatures_list[i], t) && creatures_list[i].level >= lo && creatures_list[i].level <= hi) {
            out.push_back(i);
        }
    }
    return out;
}

int pick(std::vector<int> const &from) {
    return from.empty() ? -1 : from[(size_t) (randomNumber((int) from.size()) - 1)];
}

// The lord: the strongest of the theme's creatures up to lord_max.
int lord_of(Theme const &t) {
    int best = -1;
    std::vector<int> top;
    for (int i : creatures(t, 1, t.lord_max)) {
        int level = creatures_list[i].level;
        if (level > best) {
            best = level;
            top.clear();
        }
        if (level == best) {
            top.push_back(i);
        }
    }
    return pick(top);
}

bool free_floor(Coord_t const &c) {
    if (c.y <= 0 || c.x <= 0 || c.y >= dg.height - 1 || c.x >= dg.width - 1) {
        return false;
    }
    Tile_t const &t = dg.floor[c.y][c.x];
    return t.feature_id < MIN_CLOSED_SPACE && t.creature_id == 0 && t.treasure_id == 0;
}

Coord_t random_floor() {
    for (int tries = 0; tries < 20000; tries++) {
        Coord_t c{(int32_t) randomNumber(dg.height - 2), (int32_t) randomNumber(dg.width - 2)};
        if (free_floor(c)) {
            return c;
        }
    }
    return Coord_t{1, 1};
}

// A free square at least `near` and at most `far` from `from`.
bool floor_between(Coord_t const &from, int near, int far, Coord_t &out) {
    for (int tries = 0; tries < 4000; tries++) {
        Coord_t c = random_floor();
        int d = coordDistanceBetween(from, c);
        if (d >= near && d <= far) {
            out = c;
            return true;
        }
    }
    return false;
}

// A monster placed and, for a raid, toughened; returns its id or 0.
int spawn(Coord_t const &c, int creature, int sleep, int hp_times10) {
    if (creature < 0 || !free_floor(c) || !monsterPlaceNew(c, creature, sleep > 0)) {
        return 0;
    }
    int id = dg.floor[c.y][c.x].creature_id;
    Monster_t &m = monsters[id];
    if (sleep > 0) {
        m.sleep_count = (int16_t) sleep;
    }
    int hp = m.hp * hp_times10 / 10;
    m.hp = (int16_t) (hp > 30000 ? 30000 : hp < 1 ? 1 : hp);
    return id;
}

// Up to n of the theme's creatures in free squares around `center`.
int spawn_around(Coord_t const &center, int n, std::vector<int> const &kinds, int sleep, int hp_times10) {
    int placed = 0;
    for (int tries = 0; placed < n && tries < n * 30; tries++) {
        int r = 1 + tries / 15;
        Coord_t c{center.y + (int32_t) randomNumber(2 * r + 1) - r - 1, center.x + (int32_t) randomNumber(2 * r + 1) - r - 1};
        if (next_free_monster_id >= MON_TOTAL_ALLOCATIONS - 2) {
            break;
        }
        if (spawn(c, pick(kinds), sleep, hp_times10) != 0) {
            placed++;
        }
    }
    return placed;
}

void place_door(Coord_t const &c, uint8_t sub) {
    int id = popt();
    dg.floor[c.y][c.x].treasure_id = (uint8_t) id;
    inventoryItemCopyTo(config::dungeon::objects::OBJ_STORE_DOOR, game.treasure.list[id]);
    game.treasure.list[id].sub_category_id = sub;
    game.treasure.list[id].sprite = '0';
}

// Raid treasure is gear: arms, armour, rings, amulets, staves and wands.
bool gear(Inventory_t const &item) {
    uint8_t c = item.category_id;
    return (c >= TV_MIN_WEAR && c <= TV_MAX_WEAR && c != TV_SPIKE && c != TV_DIGGING) || c == TV_STAFF || c == TV_WAND;
}

// Gear rolled at `level`: the best (by value) of `rolls` pieces.
void make_item(int treasure_id, int level, int rolls) {
    if (level > TREASURE_MAX_LEVELS) {
        level = TREASURE_MAX_LEVELS;
    }
    Inventory_t best{};
    bool have = false;
    for (int found = 0, tries = 0; found < rolls && tries < rolls * 40; tries++) {
        inventoryItemCopyTo(sorted_objects[itemGetRandomObjectId(level, false)], game.treasure.list[treasure_id]);
        if (!gear(game.treasure.list[treasure_id])) {
            continue;
        }
        found++;
        magicTreasureMagicalAbility(treasure_id, level);
        if (!have || game.treasure.list[treasure_id].cost > best.cost) {
            best = game.treasure.list[treasure_id];
            have = true;
        }
    }
    if (have) {
        game.treasure.list[treasure_id] = best;
    }
}

bool loot_spot(Coord_t const &center, Coord_t &out) {
    for (int tries = 0; tries < 80; tries++) {
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

// Treasure on the floor around `center`, and gold.
void hoard(Coord_t const &center, int items, int level, int gold) {
    for (int i = 0; i < items; i++) {
        Coord_t c;
        if (!loot_spot(center, c)) {
            break;
        }
        int id = popt();
        dg.floor[c.y][c.x].treasure_id = (uint8_t) id;
        make_item(id, level, 3);
        dungeonLiteSpot(c);
    }
    for (int i = 0; i < gold; i++) {
        Coord_t c;
        if (loot_spot(center, c)) {
            dungeonPlaceGold(c);
            dungeonLiteSpot(c);
        }
    }
}

MuRaid *raid_here() {
    MuSession *s = mu_self;
    return s != nullptr && s->level != nullptr ? s->level->raid : nullptr;
}

void tell_level(MuSession *s, std::string const &line) {
    for (MuSession *p : s->level->players) {
        mu_system(p, line);
    }
}

// Every player in the raid not locked out: a reward of their own.
void rewards(MuSession *s, MuRaid const &raid, Coord_t const &center) {
    Theme const &t = THEMES[raid.theme];
    int64_t now = (int64_t) time(nullptr);
    std::vector<MuSession *> inside = s->level->players;
    for (MuSession *p : inside) {
        int64_t last = mu_db_int("SELECT at FROM raid_clear WHERE character = ? AND raid = ?", 0, "ii", p->char_id, (int64_t) raid.theme);
        if (now - last < (int64_t) MU_RAID_LOCKOUT_HOURS * 3600) {
            int64_t left = (int64_t) MU_RAID_LOCKOUT_HOURS * 3600 - (now - last);
            mu_system(p, "[Raid] You had " + std::string(t.name) + "'s reward " + std::to_string((now - last) / 3600) + "h ago; the next in " +
                             std::to_string((left + 3599) / 3600) + "h.");
            continue;
        }
        mu_db_run("INSERT INTO raid_clear (character, raid, at) VALUES (?, ?, ?) ON CONFLICT(character, raid) DO UPDATE SET at = excluded.at", "iii",
                  p->char_id, (int64_t) raid.theme, now);

        Coord_t c;
        if (!loot_spot(center, c)) {
            continue;
        }
        int id = popt();
        dg.floor[c.y][c.x].treasure_id = (uint8_t) id;
        make_item(id, raid.gate + 25, 5);
        // Rare at least; a character's first raid reward of the day, Epic at least
        bool daily = mu_db_int("SELECT daily_reward_at FROM character WHERE id = ?", 0, "i", p->char_id) / 86400 != now / 86400;
        if (mu_items_drop(game.treasure.list[id], MU_LOOT_REWARD, raid.gate) < 0) {
            mu_loot_bless(game.treasure.list[id], MU_LOOT_REWARD, raid.gate, raid.gate, p, daily ? MU_EPIC : MU_RARE);
        }
        if (daily) {
            mu_db_run("UPDATE character SET daily_reward_at = ? WHERE id = ?", "ii", now, p->char_id);
            mu_system(p, "[Daily] Your first raid reward today: Epic or better.");
        }
        Inventory_t item = game.treasure.list[id];
        int32_t gold = 150 * (raid.theme + 1) + randomNumber(150 * (raid.theme + 1));

        bool taken = false;
        mu_with_player(s, p, [&] {
            py.misc.au += gold;
            if (inventoryCanCarryItemCount(item)) {
                Inventory_t copy = item;
                inventoryCarryItem(copy);
                taken = true;
            }
            printCharacterGoldValue();
        });
        if (taken) {
            (void) dungeonDeleteObject(c);
        } else {
            dungeonLiteSpot(c);
        }
        obj_desc_t desc;
        itemDescription(desc, item, true);
        mu_system(p, std::string("[Raid] Your reward: ") + desc + (taken ? "" : " It lies by the lord: your pack is full.") + " And " +
                         std::to_string(gold) + " gold.");
        if (p != s) {
            mu_conn_poke(p->conn);
        }
    }
}

} // namespace

bool mu_raid_gate_level(int depth) {
    return depth > 0 && depth % MU_RAID_INTERVAL == 0;
}

MuRaid *mu_raid_build(int gate, int party) {
    auto *raid = new MuRaid();
    raid->gate = gate;
    raid->theme = theme_of(gate);
    raid->party = party < 1 ? 1 : party > MU_PARTY_MAX ? MU_PARTY_MAX : party;
    Theme const &t = THEMES[raid->theme];
    int n = raid->party;

    // no stairs: the way in is the way out
    for (int y = 0; y < dg.height; y++) {
        for (int x = 0; x < dg.width; x++) {
            int id = dg.floor[y][x].treasure_id;
            if (id != 0 && (game.treasure.list[id].category_id == TV_UP_STAIR || game.treasure.list[id].category_id == TV_DOWN_STAIR)) {
                (void) dungeonDeleteObject(Coord_t{y, x});
            }
            if (dg.floor[y][x].creature_id > 1) {
                dg.floor[y][x].creature_id = 0;
            }
        }
    }
    next_free_monster_id = config::monsters::MON_MIN_INDEX_ID;
    monster_multiply_total = 0;

    raid->entry = random_floor();
    place_door(raid->entry, WAY_OUT);

    std::vector<int> mobs = creatures(t, t.lo, t.hi);
    std::vector<int> strong = creatures(t, (t.lo + t.hi) / 2, t.hi);
    if (strong.empty()) {
        strong = mobs;
    }
    int mob_hp = 10 + 2 * (n - 1); // a little tougher for each member after the first

    // the lord, as far from the way in as can be found
    Coord_t lair = raid->entry;
    for (int i = 0; i < 400; i++) {
        Coord_t c = random_floor();
        if (coordDistanceBetween(c, raid->entry) > coordDistanceBetween(lair, raid->entry)) {
            lair = c;
        }
    }
    int lord = spawn(lair, lord_of(t), 40, 10 * (6 + 3 * n));
    if (lord != 0) {
        monsters[lord].mu_boss = 3;
        monsters[lord].speed++;
        raid->boss_hp = monsters[lord].hp;
        spawn_around(lair, 3 + n, strong, 40, mob_hp);
    }

    // two captains with guards, between the two
    for (int i = 0; i < 2; i++) {
        Coord_t c;
        if (floor_between(raid->entry, 25, 200, c) && coordDistanceBetween(c, lair) >= 15) {
            int captain = spawn(c, pick(strong), 30, 10 * (3 + n));
            if (captain != 0) {
                monsters[captain].mu_boss = 2;
                spawn_around(c, 3 + n / 2, mobs, 30, mob_hp);
            }
        }
    }

    // packs everywhere but the doorstep
    int want = std::min(MON_TOTAL_ALLOCATIONS - 15, 45 + 15 * n);
    for (int tries = 0; next_free_monster_id - config::monsters::MON_MIN_INDEX_ID < want && tries < 200; tries++) {
        Coord_t c;
        if (!floor_between(raid->entry, 12, 400, c)) {
            break;
        }
        spawn_around(c, 3 + randomNumber(4), mobs, 1 + randomNumber(25), mob_hp);
    }
    return raid;
}

bool mu_raid_find_gate(Coord_t &out) {
    for (int y = 0; y < dg.height; y++) {
        for (int x = 0; x < dg.width; x++) {
            int id = dg.floor[y][x].treasure_id;
            if (id != 0 && game.treasure.list[id].category_id == TV_STORE_DOOR && game.treasure.list[id].sub_category_id == GATE) {
                out = Coord_t{y, x};
                return true;
            }
        }
    }
    return false;
}

void mu_raid_place_gate(MuLevel *level) {
    Coord_t c;
    if (!mu_raid_gate_level(level->depth) || level->raid != nullptr || mu_raid_find_gate(c)) {
        return;
    }
    place_door(random_floor(), GATE);
    level->dirty = true;
}

void mu_raid_announce(MuSession *s, MuLevel *level) {
    if (level->raid != nullptr) {
        Theme const &t = THEMES[level->raid->theme];
        int lord = -1;
        for (int i = config::monsters::MON_MIN_INDEX_ID; i < next_free_monster_id; i++) {
            if (monsters[i].mu_boss == 3) {
                lord = monsters[i].creature_id;
            }
        }
        mu_system(s, std::string("[Raid] You are in ") + t.name + "." +
                         (level->raid->won ? " Its lord is dead." : lord >= 0 ? std::string(" Deep within waits ") + creatures_list[lord].name + ", its lord." : ""));
        return;
    }
    if (mu_raid_gate_level(level->depth)) {
        mu_system(s, std::string("A raid gate (0) hums somewhere on this level: it leads into ") + THEMES[theme_of(level->depth)].name + ".");
    }
}

int mu_raid_party_here(MuSession *s) {
    if (s->party == nullptr) {
        return 1;
    }
    int n = 0;
    for (MuSession *m : s->party->members) {
        n += m->level == s->level;
    }
    return n < 1 ? 1 : n;
}

MuLevel *mu_raid_of_party(MuSession *s, int gate) {
    if (s->party == nullptr) {
        return nullptr;
    }
    for (MuSession *m : s->party->members) {
        if (m != s && m->level != nullptr && m->level->raid != nullptr && m->level->raid->gate == gate) {
            return m->level;
        }
    }
    return nullptr;
}

void mu_raid_monster_dies(int monster_id) {
    MuRaid *raid = raid_here();
    Monster_t &m = monsters[monster_id];
    int kind = m.mu_boss;
    m.mu_boss = 0;
    if (raid == nullptr) {
        return;
    }
    MuSession *s = mu_self;
    Coord_t center{m.pos.y, m.pos.x};
    const char *name = creatures_list[m.creature_id].name;
    if (kind == 2) {
        hoard(center, 2, raid->gate + 12, 2);
        printMessage("The captain falls, and its hoard spills out!");
        tell_level(s, "[Raid] " + s->char_name + " has slain a captain, " + std::string(name) + ".");
        return;
    }
    raid->won = true;
    hoard(center, 3 + raid->party, raid->gate + 18, 4);
    printMessage("The lord falls! Its treasure spills across the floor!");
    tell_level(s, "[Raid] " + std::string(name) + " is dead! The party rolls for its treasure; each of you has a reward of your own.");
}

void mu_raid_rewards(Coord_t const &center) {
    MuRaid *raid = raid_here();
    if (raid == nullptr) {
        return;
    }
    MuSession *s = mu_self;
    rewards(s, *raid, center);
    tell_level(s, "[Raid] The way out (0) is where you came in.");
    mu_system_all(s->char_name + (s->party != nullptr ? "'s party has" : " has") + " conquered " + THEMES[raid->theme].name + "!");
}

std::string mu_raid_admin(MuSession *s, std::string const &arg) {
    MuRaid *raid = s->level != nullptr ? s->level->raid : nullptr;
    if (arg == "lord") {
        if (raid == nullptr) {
            return "Not in a raid.";
        }
        for (int i = config::monsters::MON_MIN_INDEX_ID; i < next_free_monster_id; i++) {
            if (monsters[i].mu_boss == 3) {
                spellTeleportPlayerTo(Coord_t{monsters[i].pos.y, monsters[i].pos.x});
                return std::string("Beside ") + creatures_list[monsters[i].creature_id].name + ".";
            }
        }
        return "Its lord is dead.";
    }
    if (raid != nullptr || !mu_raid_gate_level(s->level->depth)) {
        return "Raid gates stand on every " + std::to_string(MU_RAID_INTERVAL) + "th level below the town.";
    }
    s->pending_raid = s->level->depth;
    dg.generate_new_level = true;
    return std::string("Into ") + THEMES[theme_of(s->level->depth)].name + ".";
}

// --- engine hooks -------------------------------------------------------------

void muMonsterHurt(int monster_id) {
    mu_script_hurt(monster_id); // script.cpp: a script's boss
    Monster_t &m = monsters[monster_id];
    MuRaid *raid = raid_here();
    if (m.mu_boss != 3 || raid == nullptr || raid->boss_hp <= 0) {
        return;
    }
    // at three quarters, half and a quarter: its guard comes running
    while (raid->waves < 3 && m.hp * 100 <= raid->boss_hp * (75 - 25 * raid->waves)) {
        raid->waves++;
        Theme const &t = THEMES[raid->theme];
        int n = spawn_around(Coord_t{m.pos.y, m.pos.x}, 2 + raid->party, creatures(t, (t.lo + t.hi) / 2, t.hi), 0, 10);
        if (n > 0) {
            std::string line = std::string(creatures_list[m.creature_id].name) + " roars, and its guard rushes in!";
            printMessage(line.c_str());
            tell_level(mu_self, "[Raid] " + line);
        }
    }
}

const char *mu_raid_title(MuLevel const *level) {
    return level != nullptr && level->raid != nullptr ? THEMES[level->raid->theme].name : nullptr;
}

const char *muRaidName() {
    MuLevel *level = mu_level_loaded();
    return level != nullptr && level->raid != nullptr ? THEMES[level->raid->theme].label : nullptr;
}

void muRaidGate(int sub_category_id) {
    MuSession *s = mu_self;
    MuSafePrompt safe;
    if (sub_category_id == WAY_OUT) {
        if (getInputConfirmation("Leave the raid through the gate?")) {
            s->to_gate = true;
            mu_send_to_depth(s->level->depth);
        }
        return;
    }

    int gate = s->level->depth;
    Theme const &t = THEMES[theme_of(gate)];
    MuLevel *inside = mu_raid_of_party(s, gate);
    int64_t last = mu_db_int("SELECT at FROM raid_clear WHERE character = ? AND raid = ?", 0, "ii", s->char_id, (int64_t) theme_of(gate));
    int64_t wait = (int64_t) MU_RAID_LOCKOUT_HOURS * 3600 - ((int64_t) time(nullptr) - last);

    std::string party;
    if (s->party != nullptr) {
        for (MuSession *m : s->party->members) {
            party += (party.empty() ? "" : ", ") + m->char_name + (m->level == s->level ? "" : m->level != nullptr && m->level->raid != nullptr ? " (inside)" : " (away)");
        }
    }
    char line[100];
    clearScreen();
    snprintf(line, sizeof(line), "The Raid Gate: %s", t.name);
    putString(line, Coord_t{2, 20});
    putString("A ring of cold stone, humming with power. Beyond it:", Coord_t{5, 4});
    putString(t.about, Coord_t{6, 4});
    putString(party.empty() ? "You stand alone. A raid grows with the party that enters it." : ("Your party: " + party).c_str(), Coord_t{9, 4});
    putString("More of you means more foes, a tougher lord, and more treasure.", Coord_t{10, 4});
    if (wait > 0) {
        snprintf(line, sizeof(line), "Your reward here is spent: the next in %dh %dm.", (int) (wait / 3600), (int) (wait % 3600 / 60));
    } else {
        snprintf(line, sizeof(line), "Its lord's death gives each of you a reward (once every %d hours).", MU_RAID_LOCKOUT_HOURS);
    }
    putString(line, Coord_t{12, 4});
    putString("No stairs lead out: only this gate, a scroll, or death.", Coord_t{13, 4});
    putString(inside != nullptr ? "e) Join your party inside" : "e) Enter the raid", Coord_t{16, 4});
    putString("ESC) Step back", Coord_t{17, 4});
    char key = getKeyInput();
    if (key == 'e' || key == 'E') {
        s->pending_raid = gate;
        dg.generate_new_level = true;
    }
    drawCavePanel();
}
