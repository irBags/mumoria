// Mumoria - loot rarity (docs/MUMORIA_DESIGN.md 2.21).
//
// Gear (arms, armour, jewellery, staves and wands) comes in five tiers:
// Common, Uncommon, Rare, Epic and Legendary. The tier is rolled when the
// item appears: rarely from the floor and from monsters (better from monsters
// above the killer's level, never better than Uncommon from one far below),
// always Rare or better from a level boss, a raid captain or lord, or a raid
// reward. A higher tier means bigger bonuses, more powers (resistances,
// slays, sight), stat bonuses and a name from Epic up, and a character level
// to wear it. Boss and raid rolls carry a bad-luck counter per character.
//
// A rare item glows on the map and in every list in its tier's colour, and
// its first finder is told (Epic: the party and level; Legendary: the world)
// and logged (/loot, /legends).

#include "loot.h"

#include <algorithm>

#include "items.h"
#include "mu_config.h"
#include "session.h"
#include "social.h"

namespace {

using namespace config::treasure::flags;

struct Tier {
    const char *name;
    int colour;    // xterm 256
    int level;     // to wear it
    int power;     // how much its bonuses grow
    int powers;    // resistances, slays and the like
    int32_t worth; // added to its value
};

const Tier TIERS[MU_RARITY_COUNT] = {
    {"Common", 0, 0, 0, 0, 0},
    {"Uncommon", 40, 0, 1, 0, 100},
    {"Rare", 33, MU_LOOT_RARE_LEVEL, 2, 1, 500},
    {"Epic", 135, MU_LOOT_EPIC_LEVEL, 3, 2, 2500},
    {"Legendary", 208, MU_LOOT_LEGENDARY_LEVEL, 5, 3, 10000},
};

// Chances in 100000 by MuLootSource. A rare chance of 100000 means Rare at least.
struct Odds {
    int legendary, epic, rare, uncommon;
    int level_bonus; // item levels above the monster's
};
const Odds ODDS[] = {
    {10, 150, 2000, 10000, 0},    // the floor
    {20, 300, 3000, 12000, 0},    // a monster
    {1000, 20000, 100000, 0, 10}, // a level boss
    {2000, 30000, 100000, 0, 12}, // a raid captain
    {4000, 45000, 100000, 0, 18}, // a raid lord
    {3000, 35000, 100000, 0, 25}, // a raid reward
};
// Bad luck: each boss or raid roll without one raises the next one's chance.
const int EPIC_DROUGHT = 3000, EPIC_DROUGHT_MAX = 45000;
const int LEGEND_DROUGHT = 300, LEGEND_DROUGHT_MAX = 15000;

bool dropping = false;

int roll(int n) {
    return n <= 0 ? 0 : randomNumber(n);
}

bool weapon(uint8_t c) {
    return c == TV_BOW || c == TV_HAFTED || c == TV_POLEARM || c == TV_SWORD || c == TV_DIGGING;
}

bool armour(uint8_t c) {
    return c == TV_BOOTS || c == TV_GLOVES || c == TV_CLOAK || c == TV_HELM || c == TV_SHIELD || c == TV_HARD_ARMOR || c == TV_SOFT_ARMOR;
}

bool jewellery(uint8_t c) {
    return c == TV_RING || c == TV_AMULET;
}

bool eligible(Inventory_t const &item) {
    uint8_t c = item.category_id;
    bool gear = weapon(c) || armour(c) || jewellery(c) || c == TV_STAFF || c == TV_WAND;
    // a ring of teleportation and the like stays what it is
    // a custom item (items.cpp) is what its maker made it
    return gear && mu_rarity(item) == MU_COMMON && item.special_name_id < SN_ARRAY_SIZE && item.id < MAX_OBJECTS_IN_GAME &&
           (game_objects[item.id].flags & TR_CURSED) == 0;
}

int level_of(MuSession *p) {
    return p == mu_player_loaded() ? py.misc.level : p->ctx.py.misc.level;
}

// A rare item is never cursed: its curse and its bad name go.
void cleanse(Inventory_t &item) {
    static const uint8_t BAD[] = {SN_CLUMSINESS, SN_WEAKNESS, SN_SLOWNESS, SN_NOISE,    SN_GREAT_MASS, SN_STUPIDITY, SN_DULLNESS,
                                  SN_BLINDNESS,  SN_TIMIDNESS, SN_TELEPORTATION, SN_UGLINESS, SN_IRRITATION, SN_VULNERABILITY};
    for (uint8_t bad : BAD) {
        if (item.special_name_id == bad) {
            item.special_name_id = SN_NULL;
        }
    }
    item.flags &= ~(TR_CURSED | TR_AGGRAVATE | TR_TELEPORT | TR_BLIND | TR_TIMID);
    item.to_hit = (int16_t) std::abs(item.to_hit);
    item.to_damage = (int16_t) std::abs(item.to_damage);
    item.to_ac = (int16_t) std::abs(item.to_ac);
    if (item.category_id != TV_STAFF && item.category_id != TV_WAND) {
        item.misc_use = (int16_t) std::abs(item.misc_use);
    }
    item.weight = game_objects[item.id].weight; // boots of great mass
    if (item.cost < 0) {
        item.cost = 0;
    }
}

void add_powers(Inventory_t &item, int count) {
    const uint32_t ARMS[] = {TR_SLAY_EVIL, TR_SLAY_UNDEAD, TR_SLAY_ANIMAL, TR_SLAY_DRAGON, TR_FROST_BRAND, TR_FLAME_TONGUE,
                             TR_SEE_INVIS, TR_FREE_ACT,    TR_REGEN,       TR_RES_FIRE,    TR_RES_COLD};
    const uint32_t WARDS[] = {TR_RES_FIRE, TR_RES_COLD, TR_RES_ACID, TR_RES_LIGHT, TR_FREE_ACT, TR_SEE_INVIS, TR_REGEN, TR_SLOW_DIGEST, TR_FFALL};
    bool arms = item.category_id == TV_HAFTED || item.category_id == TV_POLEARM || item.category_id == TV_SWORD;
    const uint32_t *pool = arms ? ARMS : WARDS;
    int size = arms ? (int) (sizeof(ARMS) / sizeof(ARMS[0])) : (int) (sizeof(WARDS) / sizeof(WARDS[0]));
    for (int tries = 0; count > 0 && tries < 30; tries++) {
        uint32_t power = pool[randomNumber(size) - 1];
        if ((item.flags & power) == 0) {
            item.flags |= power;
            count--;
        }
    }
}

// Epic and up: a stat bonus (or, rarely, speed) on arms and armour that have no such bonus yet.
void add_stats(Inventory_t &item, int tier) {
    const uint32_t P1 = TR_STATS | TR_SEARCH | TR_STEALTH | TR_INFRA | TR_SPEED | TR_TUNNEL;
    uint8_t c = item.category_id;
    if (tier < MU_EPIC || item.misc_use != 0 || (item.flags & P1) != 0 || c == TV_BOW || c == TV_DIGGING || !(weapon(c) || armour(c))) {
        return;
    }
    if (tier == MU_LEGENDARY && randomNumber(5) == 1) {
        item.flags |= TR_SPEED;
        item.misc_use = 1;
    } else {
        int stats = tier == MU_EPIC ? 1 : 2;
        for (int tries = 0; stats > 0 && tries < 20; tries++) {
            uint32_t stat = 1u << (randomNumber(6) - 1);
            if ((item.flags & stat) == 0) {
                item.flags |= stat;
                stats--;
            }
        }
        item.misc_use = (int16_t) (tier == MU_EPIC ? randomNumber(2) : 1 + randomNumber(2));
    }
    item.identification |= config::identification::ID_SHOW_P1;
}

void make_rare(Inventory_t &item, int tier, int level) {
    Tier const &t = TIERS[tier];
    cleanse(item);
    int base = 1 + level / 8;
    auto bonus = [&] { return (int16_t) (base * t.power / 2 + roll(base * t.power / 2 + 1)); };
    uint8_t c = item.category_id;
    if (weapon(c)) {
        item.to_hit += bonus();
        item.to_damage += bonus();
        item.identification |= config::identification::ID_SHOW_HIT_DAM;
        if (c == TV_BOW) {
            item.misc_use += tier == MU_LEGENDARY ? 1 : 0; // its multiplier
        } else if (tier == MU_EPIC) {
            item.damage.sides++;
        } else if (tier == MU_LEGENDARY) {
            item.damage.dice++;
        }
    } else if (armour(c)) {
        item.to_ac += bonus();
    } else if (jewellery(c)) {
        if (item.misc_use > 0) {
            item.misc_use += (int16_t) ((tier + 1) / 2);
        }
        if (item.to_hit > 0) {
            item.to_hit += bonus();
        }
        if (item.to_damage > 0) {
            item.to_damage += bonus();
        }
        item.to_ac += (int16_t) (bonus() / 2);
    } else {
        item.misc_use += (int16_t) (2 * tier + roll(3 * tier)); // charges
    }
    add_powers(item, (weapon(c) || armour(c) || jewellery(c)) ? t.powers : 0);
    add_stats(item, tier);
    if (item.special_name_id == SN_NULL && (weapon(c) || armour(c))) {
        if (tier == MU_EPIC) {
            item.special_name_id = (uint8_t) (SN_VALOUR + randomNumber(8) - 1);
        } else if (tier == MU_LEGENDARY) {
            item.special_name_id = (uint8_t) (SN_VALAR + randomNumber(8) - 1);
        }
    }
    item.cost = item.cost * (1 + tier) + t.worth;
    item.mu_rarity = (uint8_t) tier;
}

std::string describe(Inventory_t const &item) {
    obj_desc_t desc;
    itemDescription(desc, item, true);
    std::string out = desc;
    if (!out.empty() && out.back() == '.') {
        out.pop_back();
    }
    return out;
}

std::string ago(int64_t seconds) {
    if (seconds < 3600) {
        return std::to_string(seconds / 60) + "m ago";
    }
    if (seconds < 86400 * 2) {
        return std::to_string(seconds / 3600) + "h ago";
    }
    return std::to_string(seconds / 86400) + "d ago";
}

// What an identified item does, beyond its name.
std::string powers_of(Inventory_t const &item) {
    struct Named {
        uint32_t flag;
        const char *name;
    };
    const Named POWERS[] = {
        {TR_SLAY_EVIL, "slays evil"},       {TR_SLAY_UNDEAD, "slays undead"},   {TR_SLAY_ANIMAL, "slays animals"},
        {TR_SLAY_DRAGON, "slays dragons"},  {TR_FROST_BRAND, "frost brand"},    {TR_FLAME_TONGUE, "flame tongue"},
        {TR_RES_FIRE, "resist fire"},       {TR_RES_COLD, "resist cold"},       {TR_RES_ACID, "resist acid"},
        {TR_RES_LIGHT, "resist lightning"}, {TR_FREE_ACT, "free action"},       {TR_SEE_INVIS, "see invisible"},
        {TR_REGEN, "regeneration"},         {TR_SLOW_DIGEST, "slow digestion"}, {TR_FFALL, "feather fall"},
        {TR_SUST_STAT, "sustains a stat"},
    };
    const char *STATS[] = {"Str", "Int", "Wis", "Dex", "Con", "Chr"};
    std::string out;
    auto add = [&](std::string const &what) { out += (out.empty() ? "" : ", ") + what; };
    std::string sign = item.misc_use >= 0 ? "+" : "";
    if ((item.flags & TR_STATS) != 0) {
        std::string stats;
        for (int i = 0; i < 6; i++) {
            if ((item.flags & (1u << i)) != 0) {
                stats += (stats.empty() ? "" : "/") + std::string(STATS[i]);
            }
        }
        add(sign + std::to_string(item.misc_use) + " " + stats);
    }
    if ((item.flags & TR_SPEED) != 0) {
        add(sign + std::to_string(item.misc_use) + " speed");
    }
    if ((item.flags & TR_STEALTH) != 0) {
        add(sign + std::to_string(item.misc_use) + " stealth");
    }
    if ((item.flags & TR_SEARCH) != 0) {
        add(sign + std::to_string(item.misc_use) + " searching");
    }
    if ((item.flags & TR_INFRA) != 0) {
        add(sign + std::to_string(item.misc_use) + " infravision");
    }
    for (Named const &p : POWERS) {
        if ((item.flags & p.flag) != 0) {
            add(p.name);
        }
    }
    if ((item.flags & TR_CURSED) != 0) {
        add("CURSED");
    }
    return out.empty() ? "no special powers" : out;
}

void inspect(MuSession *s, Inventory_t const &item, char letter) {
    int tier = mu_rarity(item);
    std::string line = std::string("[Item] ") + letter + ") " + describe(item);
    mu_system(s, line);
    std::string about = std::string("  ") + TIERS[tier].name;
    bool worn = item.category_id >= TV_MIN_WEAR && item.category_id <= TV_MAX_WEAR;
    if (worn && TIERS[tier].level > 0) {
        about += ", needs level " + std::to_string(TIERS[tier].level) + (py.misc.level < TIERS[tier].level ? " (too high for you)" : "");
    }
    if (worn) {
        about += spellItemIdentified(item) ? ". " + powers_of(item) + "." : ". Identify it to learn its powers.";
    }
    mu_system(s, about);
}

void tell_party_and_level(MuSession *s, std::string const &line) {
    std::vector<MuSession *> told;
    if (s->level != nullptr) {
        told = s->level->players;
    }
    if (s->party != nullptr) {
        for (MuSession *m : s->party->members) {
            if (std::find(told.begin(), told.end(), m) == told.end()) {
                told.push_back(m);
            }
        }
    }
    for (MuSession *p : told) {
        mu_system(p, line);
        if (p != mu_self) {
            mu_conn_poke(p->conn);
        }
    }
}

} // namespace

int mu_rarity(Inventory_t const &item) {
    int tier = item.mu_rarity & MU_RARITY_TIER;
    return tier < MU_RARITY_COUNT ? tier : MU_COMMON;
}

const char *mu_rarity_name(int tier) {
    return TIERS[tier >= 0 && tier < MU_RARITY_COUNT ? tier : 0].name;
}

int mu_rarity_level(int tier) {
    return TIERS[tier >= 0 && tier < MU_RARITY_COUNT ? tier : 0].level;
}

int32_t mu_rarity_worth(int tier) {
    return TIERS[tier >= 0 && tier < MU_RARITY_COUNT ? tier : 0].worth;
}

bool mu_loot_spot_near(Coord_t const &at, int radius, Coord_t &out) {
    for (int r = 1; r <= radius; r++) {
        for (int dy = -r; dy <= r; dy++) {
            for (int dx = -r; dx <= r; dx++) {
                Coord_t c{at.y + dy, at.x + dx};
                if (c.y > 0 && c.x > 0 && c.y < dg.height - 1 && c.x < dg.width - 1 && dg.floor[c.y][c.x].feature_id <= MAX_CAVE_FLOOR &&
                    dg.floor[c.y][c.x].treasure_id == 0 && dg.floor[c.y][c.x].creature_id == 0 && los(at, c)) {
                    out = c;
                    return true;
                }
            }
        }
    }
    return false;
}

int mu_loot_bless(Inventory_t &item, int source, int item_level, int creature_level, MuSession *owner, int min_tier) {
    if (!eligible(item)) {
        return MU_COMMON;
    }
    Odds odds = ODDS[source >= 0 && source <= MU_LOOT_REWARD ? source : MU_LOOT_FLOOR];
    bool big = source >= MU_LOOT_BOSS;
    int64_t epic_drought = 0, legend_drought = 0;
    if (!big) {
        // deeper finds are rarer finds; a monster above you gives more, one far below nothing rare
        int scale = 100 + 4 * item_level;
        int challenge = 100;
        if (owner != nullptr) {
            int diff = creature_level - level_of(owner);
            challenge = diff <= -MU_LOOT_GREY_LEVELS ? 0 : std::max(50, std::min(200, 100 + 5 * diff));
            if (challenge == 0) {
                odds.uncommon /= 2;
            }
        }
        odds.rare = odds.rare * scale / 100 * challenge / 100;
        odds.epic = odds.epic * scale / 100 * challenge / 100;
        odds.legendary = odds.legendary * scale / 100 * challenge / 100;
    } else if (owner != nullptr) {
        MuRows *r = mu_db_query("SELECT epic_drought, legend_drought FROM character WHERE id = ?", "i", owner->char_id);
        if (r != nullptr && mu_db_next(r)) {
            epic_drought = mu_db_col_int(r, 0);
            legend_drought = mu_db_col_int(r, 1);
        }
        mu_db_done(r);
        odds.epic += (int) std::min<int64_t>(EPIC_DROUGHT_MAX, EPIC_DROUGHT * epic_drought);
        odds.legendary += (int) std::min<int64_t>(LEGEND_DROUGHT_MAX, LEGEND_DROUGHT * legend_drought);
    }

    int r = randomNumber(100000);
    int tier = MU_COMMON;
    if (r <= odds.legendary) {
        tier = MU_LEGENDARY;
    } else if (r <= odds.legendary + odds.epic) {
        tier = MU_EPIC;
    } else if (r <= odds.legendary + odds.epic + odds.rare) {
        tier = MU_RARE;
    } else if (r <= odds.legendary + odds.epic + odds.rare + odds.uncommon) {
        tier = MU_UNCOMMON;
    }
    if (tier < min_tier) {
        tier = min_tier;
    }

    if (big && owner != nullptr) {
        epic_drought = tier >= MU_EPIC ? 0 : epic_drought + 1;
        legend_drought = tier >= MU_LEGENDARY ? 0 : legend_drought + 1;
        mu_db_run("UPDATE character SET epic_drought = ?, legend_drought = ? WHERE id = ?", "iii", epic_drought, legend_drought, owner->char_id);
    }
    if (tier > MU_COMMON) {
        make_rare(item, tier, item_level + odds.level_bonus);
    }
    return tier;
}

void mu_loot_drops(int treasure_before, Coord_t const &center, int source, int creature_level) {
    MuSession *s = mu_self;
    if (game.treasure.current_id <= treasure_before) {
        return;
    }
    int depth = dg.current_level;
    int item_level = std::max(depth, creature_level);
    bool first = source == MU_LOOT_LORD; // a lord's first piece is Epic at least
    for (int y = center.y - 4; y <= center.y + 4; y++) {
        for (int x = center.x - 4; x <= center.x + 4; x++) {
            if (y <= 0 || x <= 0 || y >= dg.height - 1 || x >= dg.width - 1) {
                continue;
            }
            int id = dg.floor[y][x].treasure_id;
            if (id < treasure_before || id >= game.treasure.current_id || !eligible(game.treasure.list[id])) {
                continue;
            }
            int tier = mu_items_drop(game.treasure.list[id], source, depth); // a custom item (items.cpp)?
            if (tier < 0) {
                tier = mu_loot_bless(game.treasure.list[id], source, item_level, creature_level, s, first ? MU_EPIC : MU_COMMON);
            }
            first = false;
            if (tier >= MU_RARE) {
                // it glows: seen (and remembered) even in the dark
                dg.floor[y][x].field_mark = true;
                muFxLoot(Coord_t{y, x}, tier);
            }
            if (tier > MU_COMMON) {
                dungeonLiteSpot(Coord_t{y, x}); // redrawn in its colour
            }
            if (tier >= MU_EPIC && s != nullptr && s->level != nullptr) {
                std::string line = std::string("[Loot] ") + (tier == MU_LEGENDARY ? "A blinding orange light: a Legendary" : "A purple glow: an Epic") +
                                   " item lies on the floor!";
                for (MuSession *p : s->level->players) {
                    mu_system(p, line);
                }
            }
        }
    }
}

void mu_loot_found(MuSession *s, Inventory_t const &item) {
    int tier = mu_rarity(item);
    if (s == nullptr || tier < MU_RARE) {
        return;
    }
    std::string what = describe(item);
    mu_db_run("INSERT INTO loot_log (character, name, tier, item, depth, at) VALUES (?, ?, ?, ?, ?, ?)", "isisii", s->char_id,
              s->char_name.c_str(), (int64_t) tier, what.c_str(), (int64_t) dg.current_level, (int64_t) time(nullptr));
    if (tier == MU_LEGENDARY) {
        mu_system_all("[Legendary] " + s->char_name + " has found " + what + "!");
    } else if (tier == MU_EPIC) {
        tell_party_and_level(s, "[Epic] " + s->char_name + " looted " + what + "!");
    }
    if (tier >= MU_EPIC) {
        muFxLoot(py.pos, tier);
    }
}

uint32_t mu_loot_tile_attr(Coord_t const &coord, char ch) {
    if (ch == ' ' || isalpha((unsigned char) ch) || coord.y < 0 || coord.x < 0 || coord.y >= MAX_HEIGHT || coord.x >= MAX_WIDTH) {
        return 0;
    }
    int id = dg.floor[coord.y][coord.x].treasure_id;
    if (id == 0) {
        return 0;
    }
    Inventory_t const &item = game.treasure.list[id];
    int tier = mu_rarity(item);
    if (tier == MU_COMMON || item.sprite != (uint8_t) ch) {
        return 0;
    }
    return MU_ATTR_BOLD | MU_ATTR_FG(TIERS[tier].colour);
}

bool mu_loot_command(MuSession *s, std::string const &cmd, std::string const &arg) {
    int64_t now = (int64_t) time(nullptr);
    if (cmd == "loot") {
        int64_t counts[MU_RARITY_COUNT] = {0};
        MuRows *r = mu_db_query("SELECT tier, COUNT(*) FROM loot_log WHERE character = ? GROUP BY tier", "i", s->char_id);
        while (r != nullptr && mu_db_next(r)) {
            int64_t tier = mu_db_col_int(r, 0);
            if (tier >= 0 && tier < MU_RARITY_COUNT) {
                counts[tier] = mu_db_col_int(r, 1);
            }
        }
        mu_db_done(r);
        int64_t epic = 0, legend = 0;
        r = mu_db_query("SELECT epic_drought, legend_drought FROM character WHERE id = ?", "i", s->char_id);
        if (r != nullptr && mu_db_next(r)) {
            epic = mu_db_col_int(r, 0);
            legend = mu_db_col_int(r, 1);
        }
        mu_db_done(r);
        epic = std::min<int64_t>(EPIC_DROUGHT_MAX, EPIC_DROUGHT * epic);
        legend = std::min<int64_t>(LEGEND_DROUGHT_MAX, LEGEND_DROUGHT * legend);
        char luck[120];
        snprintf(luck, sizeof(luck), "Luck on your next boss or raid roll: +%d.%d%% Epic, +%d.%02d%% Legendary.", (int) (epic / 1000),
                 (int) (epic / 100 % 10), (int) (legend / 1000), (int) (legend / 10 % 100));
        mu_system(s, "[Loot] Your finds: " + std::to_string(counts[MU_RARE]) + " Rare, " + std::to_string(counts[MU_EPIC]) + " Epic, " +
                         std::to_string(counts[MU_LEGENDARY]) + " Legendary. " + luck);
        r = mu_db_query("SELECT tier, item, depth, at FROM loot_log WHERE character = ? ORDER BY at DESC, id DESC LIMIT 6", "i", s->char_id);
        while (r != nullptr && mu_db_next(r)) {
            char where[32];
            mu_system(s, std::string("  ") + mu_db_col_text(r, 1) + " (" + mu_depth_name((int) mu_db_col_int(r, 2), where, sizeof(where)) + ", " +
                             ago(now - mu_db_col_int(r, 3)) + ")");
        }
        mu_db_done(r);
        return true;
    }
    if (cmd == "legends") {
        mu_system(s, "[Legends] The latest Epic and Legendary finds:");
        MuRows *r = mu_db_query("SELECT name, item, at FROM loot_log WHERE tier >= ? ORDER BY at DESC, id DESC LIMIT 10", "i", (int64_t) MU_EPIC);
        int n = 0;
        while (r != nullptr && mu_db_next(r)) {
            mu_system(s, std::string("  ") + mu_db_col_text(r, 0) + ": " + mu_db_col_text(r, 1) + " (" + ago(now - mu_db_col_int(r, 2)) + ")");
            n++;
        }
        mu_db_done(r);
        if (n == 0) {
            mu_system(s, "  None yet. Be the first.");
        }
        return true;
    }
    if (cmd == "item") {
        std::string a = arg;
        for (char &c : a) {
            c = (char) tolower((unsigned char) c);
        }
        if (a == "worn") {
            char letter = 'a'; // as the equipment list letters them
            for (int i = PlayerEquipment::Wield; i < PLAYER_INVENTORY_SIZE; i++) {
                if (py.inventory[i].category_id != TV_NOTHING) {
                    inspect(s, py.inventory[i], letter++);
                }
            }
            if (letter == 'a') {
                mu_system(s, "You are not wearing anything.");
            }
            return true;
        }
        if (a.size() == 1 && a[0] >= 'a' && a[0] - 'a' < py.pack.unique_items) {
            inspect(s, py.inventory[a[0] - 'a'], a[0]);
            return true;
        }
        mu_system(s, "Use: /item <pack letter>, or /item worn");
        return true;
    }
    return false;
}

int mu_rarity_by_name(std::string const &name) {
    for (int t = MU_COMMON; t < MU_RARITY_COUNT; t++) {
        std::string have = TIERS[t].name;
        for (char &c : have) {
            c = (char) tolower((unsigned char) c);
        }
        if (name == have || name == std::to_string(t)) {
            return t;
        }
    }
    return -1;
}

bool mu_loot_place_tier(int tier, Coord_t const &near) {
    Coord_t spot{0, 0};
    if (tier < MU_COMMON || tier >= MU_RARITY_COUNT || !mu_loot_spot_near(near, 3, spot)) {
        return false;
    }
    int id = popt();
    Inventory_t &item = game.treasure.list[id];
    for (int tries = 0; tries < 500; tries++) {
        inventoryItemCopyTo(sorted_objects[itemGetRandomObjectId(dg.current_level, false)], item);
        if (eligible(item)) {
            break;
        }
    }
    if (!eligible(item)) {
        pusht((uint8_t) id);
        return false;
    }
    magicTreasureMagicalAbility(id, dg.current_level);
    if (tier > MU_COMMON) {
        make_rare(item, tier, dg.current_level);
    }
    dg.floor[spot.y][spot.x].treasure_id = (uint8_t) id;
    dg.floor[spot.y][spot.x].field_mark = true;
    dungeonLiteSpot(spot);
    if (tier > MU_COMMON) {
        muFxLoot(spot, tier);
    }
    return true;
}

std::string mu_loot_admin(MuSession *s, std::string const &arg) {
    int tier = mu_rarity_by_name(arg);
    if (tier <= MU_COMMON || s->level == nullptr || s->level->depth == 0) {
        return "Use: /makeloot uncommon|rare|epic|legendary (in the dungeon)";
    }
    if (!mu_loot_place_tier(tier, py.pos)) {
        return "No room beside you (or nothing to make here); try again.";
    }
    return std::string(tier == MU_EPIC || tier == MU_UNCOMMON ? "An " : "A ") + TIERS[tier].name + " item appears beside you.";
}

// --- engine hooks ---------------------------------------------------------------

void muItemRarityTag(Inventory_t const &item, char *description) {
    int tier = mu_rarity(item);
    if (tier != MU_COMMON) {
        (void) strcat(description, " <");
        (void) strcat(description, TIERS[tier].name);
        (void) strcat(description, ">");
    }
}

void muItemColour(Inventory_t const *item) {
    if (mu_vt_current == nullptr) {
        return;
    }
    int tier = item != nullptr ? mu_rarity(*item) : MU_COMMON;
    mu_vt_set_attr(mu_vt_current, tier == MU_COMMON ? 0 : MU_ATTR_FG(TIERS[tier].colour) | (tier >= MU_EPIC ? MU_ATTR_BOLD : 0));
}

bool muCanWear(Inventory_t const &item) {
    int tier = mu_rarity(item);
    if (py.misc.level >= TIERS[tier].level) {
        return true;
    }
    std::string line = "You must be level " + std::to_string(TIERS[tier].level) + " to use " + (tier == MU_EPIC ? "an " : "a ") + TIERS[tier].name + " item.";
    // (Uncommon needs no level, so never reaches here)
    printMessage(line.c_str());
    return false;
}

void muItemCarried(Inventory_t &item) {
    if (mu_rarity(item) >= MU_RARE && (item.mu_rarity & MU_RARITY_TOLD) == 0) {
        item.mu_rarity |= MU_RARITY_TOLD;
        mu_loot_found(mu_player_loaded(), item);
    }
}

void muFloorItem(Coord_t const &coord, int treasure_id) {
    if (dropping || dg.current_level == 0 || !eligible(game.treasure.list[treasure_id])) {
        return;
    }
    int tier = mu_items_drop(game.treasure.list[treasure_id], MU_LOOT_FLOOR, dg.current_level);
    if (tier < 0) {
        tier = mu_loot_bless(game.treasure.list[treasure_id], MU_LOOT_FLOOR, dg.current_level, dg.current_level, nullptr);
    }
    if (tier >= MU_RARE) {
        dg.floor[coord.y][coord.x].field_mark = true; // it glows in the dark
    }
}

void muLootDropping(bool on) {
    dropping = on;
}
