// Mumoria - custom items (docs/MUMORIA_DESIGN.md 2.22).
//
// An admin makes one with /makeitem (a form), or a script with mu.item{}.
// Each is one of Umoria's base items (saves refer to those by index) with
// its own name, rarity, bonuses and powers, and where and how often it
// drops. Its name takes one of the special-name slots past Umoria's own
// (SN_ARRAY_SIZE..255), kept for good once given, so an item already made
// keeps its name. Every custom item is in table custom_item; a script's is
// written there too, by its key, and drops only while its script is loaded.

#include "items.h"

#include <algorithm>

#include "loot.h"
#include "session.h"
#include "social.h"

namespace {

using namespace config::treasure::flags;

std::vector<MuCustomItem> registry;
std::string names[SN_TABLE_SIZE]; // special_item_names[] points into these

struct Power {
    const char *key, *label;
    uint32_t flag;
};

// The order of the form's power screen.
const Power POWERS[] = {
    {"str", "Strength", TR_STR},
    {"int", "Intelligence", TR_INT},
    {"wis", "Wisdom", TR_WIS},
    {"dex", "Dexterity", TR_DEX},
    {"con", "Constitution", TR_CON},
    {"chr", "Charisma", TR_CHR},
    {"speed", "Speed", TR_SPEED},
    {"stealth", "Stealth", TR_STEALTH},
    {"searching", "Searching", TR_SEARCH},
    {"infravision", "Infravision", TR_INFRA},
    {"slay_evil", "Slay evil", TR_SLAY_EVIL},
    {"slay_undead", "Slay undead", TR_SLAY_UNDEAD},
    {"slay_animal", "Slay animal", TR_SLAY_ANIMAL},
    {"slay_dragon", "Slay dragon", TR_SLAY_DRAGON},
    {"frost_brand", "Frost brand", TR_FROST_BRAND},
    {"flame_tongue", "Flame tongue", TR_FLAME_TONGUE},
    {"resist_fire", "Resist fire", TR_RES_FIRE},
    {"resist_cold", "Resist cold", TR_RES_COLD},
    {"resist_acid", "Resist acid", TR_RES_ACID},
    {"resist_lightning", "Resist lightning", TR_RES_LIGHT},
    {"free_action", "Free action", TR_FREE_ACT},
    {"see_invisible", "See invisible", TR_SEE_INVIS},
    {"regeneration", "Regeneration", TR_REGEN},
    {"slow_digestion", "Slow digestion", TR_SLOW_DIGEST},
    {"feather_fall", "Feather fall", TR_FFALL},
};

const Power &power(int i) {
    return POWERS[i];
}

int power_count() {
    return (int) (sizeof(POWERS) / sizeof(POWERS[0]));
}

bool gear_category(uint8_t c) {
    return c == TV_BOW || c == TV_HAFTED || c == TV_POLEARM || c == TV_SWORD || c == TV_DIGGING || c == TV_BOOTS || c == TV_GLOVES ||
           c == TV_CLOAK || c == TV_HELM || c == TV_SHIELD || c == TV_HARD_ARMOR || c == TV_SOFT_ARMOR || c == TV_RING || c == TV_AMULET ||
           c == TV_STAFF || c == TV_WAND;
}

bool weapon(uint8_t c) {
    return c == TV_BOW || c == TV_HAFTED || c == TV_POLEARM || c == TV_SWORD || c == TV_DIGGING;
}

std::string lower(std::string s) {
    for (char &c : s) {
        c = (char) tolower((unsigned char) c);
    }
    return s;
}

std::string shown_name(std::string const &name) {
    return name.rfind("of ", 0) == 0 ? name : "'" + name + "'";
}

void set_name(int slot, std::string const &name) {
    if (slot >= SN_ARRAY_SIZE && slot < SN_TABLE_SIZE) {
        names[slot] = shown_name(name);
        special_item_names[slot] = names[slot].c_str();
    }
}

const char *FROM_NAMES[] = {"anywhere", "bosses and raids", "raid lords", "never (given only)"};

MuCustomItem read_row(MuRows *r) {
    MuCustomItem d;
    d.id = mu_db_col_int(r, 0);
    d.key = mu_db_col_text(r, 1);
    d.name = mu_db_col_text(r, 2);
    d.base = (int) mu_db_col_int(r, 3);
    d.tier = (int) mu_db_col_int(r, 4);
    d.to_hit = (int) mu_db_col_int(r, 5);
    d.to_damage = (int) mu_db_col_int(r, 6);
    d.to_ac = (int) mu_db_col_int(r, 7);
    d.dice = (int) mu_db_col_int(r, 8);
    d.sides = (int) mu_db_col_int(r, 9);
    d.bonus = (int) mu_db_col_int(r, 10);
    d.flags = (uint32_t) mu_db_col_int(r, 11);
    d.min_depth = (int) mu_db_col_int(r, 12);
    d.max_depth = (int) mu_db_col_int(r, 13);
    d.from = (int) mu_db_col_int(r, 14);
    d.one_in = (int) mu_db_col_int(r, 15);
    d.slot = (int) mu_db_col_int(r, 16);
    d.enabled = mu_db_col_int(r, 17) != 0;
    d.source = mu_db_col_text(r, 18);
    return d;
}

const char *COLUMNS = "id, key, name, base, tier, to_hit, to_damage, to_ac, dice, sides, bonus, flags, min_depth, max_depth, drops_from, "
                      "one_in, slot, enabled, source";

int next_slot() {
    int slot = (int) mu_db_int("SELECT MAX(slot) FROM custom_item", 0, "");
    return slot < SN_ARRAY_SIZE ? SN_ARRAY_SIZE : slot + 1;
}

// Inserts (id 0) or updates; the registry is read again. "" or why not.
std::string save(MuCustomItem &d, std::string const &by) {
    if (d.base < 0 || d.base >= MAX_OBJECTS_IN_GAME || !gear_category(game_objects[d.base].category_id)) {
        return "Pick a base item (arms, armour, a ring, an amulet, a staff or a wand).";
    }
    if (d.name.empty()) {
        return "Give it a name.";
    }
    if (d.id == 0) {
        d.slot = next_slot();
        if (d.slot >= SN_TABLE_SIZE) {
            return "No names are left for custom items (" + std::to_string(SN_TABLE_SIZE - SN_ARRAY_SIZE) + " in all).";
        }
        std::string key = d.key.empty() ? "slot" + std::to_string(d.slot) : d.key; // "item<id>" once it has an id
        if (mu_db_run("INSERT INTO custom_item (key, name, base, tier, slot, source, created_by, created_at) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                      "ssiiissi", key.c_str(), d.name.c_str(), (int64_t) d.base, (int64_t) d.tier, (int64_t) d.slot, d.source.c_str(),
                      by.c_str(), (int64_t) time(nullptr)) != 1) {
            return "Could not save it (is the key " + key + " taken?).";
        }
        d.id = mu_db_last_id();
        if (d.key.empty()) {
            d.key = "item" + std::to_string((long) d.id);
        }
    }
    mu_db_run("UPDATE custom_item SET key = ?, name = ?, base = ?, tier = ?, to_hit = ?, to_damage = ?, to_ac = ?, dice = ?, sides = ?, bonus = ?,"
              " flags = ?, min_depth = ?, max_depth = ?, drops_from = ?, one_in = ?, enabled = ?, source = ? WHERE id = ?",
              "ssiiiiiiiiiiiiiisi", d.key.c_str(), d.name.c_str(), (int64_t) d.base, (int64_t) d.tier, (int64_t) d.to_hit,
              (int64_t) d.to_damage, (int64_t) d.to_ac, (int64_t) d.dice, (int64_t) d.sides, (int64_t) d.bonus, (int64_t) d.flags,
              (int64_t) d.min_depth, (int64_t) d.max_depth, (int64_t) d.from, (int64_t) d.one_in, (int64_t) (d.enabled ? 1 : 0),
              d.source.c_str(), d.id);
    bool live = d.live;
    for (MuCustomItem &old : registry) {
        if (old.id == d.id) {
            old = d;
            old.live = live;
            set_name(d.slot, d.name);
            return "";
        }
    }
    registry.push_back(d);
    registry.back().live = live;
    set_name(d.slot, d.name);
    return "";
}

std::string describe(Inventory_t const &item) {
    obj_desc_t desc;
    itemDescription(desc, item, true);
    return desc;
}

std::string summary(MuCustomItem const &d) {
    char line[160];
    static const char *FROM_SHORT[] = {"anywhere", "bosses", "lords", "given"};
    snprintf(line, sizeof(line), "%3ld %-20.20s %-18.18s %-9s %3d-%-3d %-8s %s", (long) d.id, d.name.c_str(), mu_items_base_name(d.base).c_str(),
             mu_rarity_name(d.tier), d.min_depth, d.max_depth, FROM_SHORT[d.from & 3],
             d.one_in > 0 ? ("1 in " + std::to_string(d.one_in) + (d.enabled ? "" : " (off)")).c_str() : (d.enabled ? "-" : "(off)"));
    return line;
}

// --- the form ------------------------------------------------------------------

bool ask(const char *prompt, std::string &out, int len) {
    putStringClearToEOL(prompt, Coord_t{22, 0});
    char buf[100] = {0};
    bool ok = getStringInput(buf, Coord_t{22, (int) strlen(prompt)}, std::min(len, 60));
    eraseLine(Coord_t{22, 0});
    out = buf;
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return ok;
}

bool ask_int(const char *prompt, int &value, int lo, int hi) {
    std::string s;
    if (!ask(prompt, s, 8) || s.empty()) {
        return false;
    }
    value = std::max(lo, std::min(hi, atoi(s.c_str())));
    return true;
}

void pick_base(MuCustomItem &d) {
    std::string part;
    if (!ask("Base item (part of its name): ", part, 30) || part.empty()) {
        return;
    }
    std::vector<int> found;
    std::vector<std::string> seen;
    for (int i = 0; i < MAX_DUNGEON_OBJECTS && found.size() < 20; i++) {
        if (!gear_category(game_objects[i].category_id) || (game_objects[i].flags & TR_CURSED) != 0) {
            continue;
        }
        std::string name = mu_items_base_name(i);
        if (lower(name).find(lower(part)) != std::string::npos && std::find(seen.begin(), seen.end(), name) == seen.end()) {
            seen.push_back(name);
            found.push_back(i);
        }
    }
    if (found.empty()) {
        putStringClearToEOL("No base item has that in its name.", Coord_t{22, 0});
        (void) getKeyInput();
        return;
    }
    clearScreen();
    putString("Which base item?", Coord_t{0, 0});
    for (size_t i = 0; i < found.size(); i++) {
        DungeonObject_t const &o = game_objects[found[i]];
        char line[100];
        snprintf(line, sizeof(line), " %c) %-34s %dd%d  [%d]  level %d", (char) ('a' + i), seen[i].c_str(), o.damage.dice, o.damage.sides, o.ac,
                 o.depth_first_found);
        putString(line, Coord_t{(int) i + 2, 0});
    }
    char key = getKeyInput();
    int i = key - 'a';
    if (i >= 0 && i < (int) found.size()) {
        d.base = found[i];
    }
}

void pick_powers(MuCustomItem &d) {
    for (;;) {
        clearScreen();
        putString("Powers (a letter turns one on or off; ESC when done)", Coord_t{0, 0});
        for (int i = 0; i < power_count(); i++) {
            Power const &p = power(i);
            char line[60];
            snprintf(line, sizeof(line), "%c) [%c] %s", (char) ('a' + i), (d.flags & p.flag) != 0 ? 'x' : ' ', p.label);
            putString(line, Coord_t{2 + i % 13, i < 13 ? 2 : 40});
        }
        putString("Stats, speed, stealth, searching and infravision all get the bonus amount.", Coord_t{17, 2});
        char key = getKeyInput();
        int i = key - 'a';
        if (key == ESCAPE) {
            return;
        }
        if (i >= 0 && i < power_count()) {
            d.flags ^= power(i).flag;
        }
    }
}

void form(MuSession *s, MuCustomItem d) {
    MuSafePrompt safe;
    std::string note = d.id == 0 ? "A new item." : "Editing item " + std::to_string((long) d.id) + " (" + d.key + ").";
    for (;;) {
        clearScreen();
        putString("The Item Maker", Coord_t{0, 30});
        putString(note.c_str(), Coord_t{1, 2});
        Inventory_t preview{};
        std::string shown = "(no base item yet)";
        if (d.base >= 0) {
            mu_items_make(d, preview);
            preview.identification |= config::identification::ID_KNOWN2;
            shown = describe(preview);
        }
        auto line = [](int y, std::string const &text) { putString(text.c_str(), Coord_t{y, 2}); };
        line(3, "It is: " + shown.substr(0, 70));
        line(5, "a) Base item:    " + (d.base >= 0 ? mu_items_base_name(d.base) : std::string("-")));
        line(6, "b) Name:         " + (d.name.empty() ? std::string("-") : shown_name(d.name)));
        line(7, std::string("c) Rarity:       ") + mu_rarity_name(d.tier) +
                    (mu_rarity_level(d.tier) > 0 ? " (needs level " + std::to_string(mu_rarity_level(d.tier)) + ")" : ""));
        line(8, "d) To-hit:       " + std::to_string(d.to_hit) + "        e) Damage bonus: " + std::to_string(d.to_damage));
        line(9, "f) Armour bonus: " + std::to_string(d.to_ac) + "        g) Damage dice:  " +
                    (d.dice > 0 ? std::to_string(d.dice) + "d" + std::to_string(d.sides) : std::string("the base item's")));
        line(10, "h) Bonus amount: " + std::to_string(d.bonus) + "  (stats, speed, stealth...; charges on staves and wands)");
        line(11, "i) Powers:       " + mu_items_power_names(d.flags).substr(0, 58));
        line(12, "j) Depth:        " + std::to_string(d.min_depth) + " to " + std::to_string(d.max_depth) + " (levels, 50 ft each)");
        line(13, std::string("k) Drops from:   ") + FROM_NAMES[d.from & 3]);
        line(14, "l) How often:    " + (d.one_in > 0 ? "1 in " + std::to_string(d.one_in) + " gear drops there" : std::string("never")));
        line(15, std::string("m) In the drops: ") + (d.enabled ? "yes" : "no (off)"));
        line(17, "s) Save    p) Save and make one beside you    ESC) Leave");
        if (!d.source.empty() && d.source != "admin") {
            line(19, "This item is defined by the script " + d.source + ": edit the script.");
        }
        char key = getKeyInput();
        std::string text;
        switch (key) {
            case 'a':
                pick_base(d);
                break;
            case 'b':
                if (ask("Name (\"of the Storm\", or a name like Thunderfury): ", text, 30) && !text.empty()) {
                    d.name = text;
                }
                break;
            case 'c':
                d.tier = (d.tier + 1) % MU_RARITY_COUNT;
                break;
            case 'd':
                (void) ask_int("To-hit: ", d.to_hit, -50, 99);
                break;
            case 'e':
                (void) ask_int("Damage bonus: ", d.to_damage, -50, 99);
                break;
            case 'f':
                (void) ask_int("Armour bonus: ", d.to_ac, -50, 99);
                break;
            case 'g':
                if (ask("Damage dice (like 3d6; 0 for the base item's): ", text, 8)) {
                    int n = 0, m = 0;
                    if (sscanf(text.c_str(), "%dd%d", &n, &m) == 2 && n > 0 && m > 0 && n <= 20 && m <= 50) {
                        d.dice = n;
                        d.sides = m;
                    } else {
                        d.dice = d.sides = 0;
                    }
                }
                break;
            case 'h':
                (void) ask_int("Bonus amount: ", d.bonus, -10, 30);
                break;
            case 'i':
                pick_powers(d);
                break;
            case 'j':
                (void) ask_int("Shallowest level it drops on: ", d.min_depth, 1, 127);
                (void) ask_int("Deepest level it drops on: ", d.max_depth, d.min_depth, 127);
                break;
            case 'k':
                d.from = (d.from + 1) % 4;
                break;
            case 'l':
                (void) ask_int("Drops from 1 in how many gear drops (0: never)? ", d.one_in, 0, 1000000);
                break;
            case 'm':
                d.enabled = !d.enabled;
                break;
            case 's':
            case 'p': {
                if (!d.source.empty() && d.source != "admin") {
                    note = "A script's item: edit " + d.source + " instead.";
                    break;
                }
                d.source = "admin";
                bool fresh = d.id == 0;
                std::string why = save(d, s->account.name);
                if (!why.empty()) {
                    note = why;
                    break;
                }
                mu_db_audit(s->account.name, fresh ? "makeitem" : "edititem", d.key.c_str(), d.name.c_str());
                note = "Saved as item " + std::to_string((long) d.id) + ".";
                if (key == 'p') {
                    note += mu_items_place(d, py.pos) ? " One lies beside you." : " No room beside you.";
                }
                break;
            }
            case ESCAPE:
                drawCavePanel();
                return;
            default:
                break;
        }
    }
}

void list_screen() {
    MuSafePrompt safe;
    size_t at = 0;
    do {
        clearScreen();
        putString(" id name                 base               rarity    depth   from     how often", Coord_t{0, 0});
        if (registry.empty()) {
            putString("No custom items yet: /makeitem makes one.", Coord_t{2, 1});
        }
        int y = 1;
        for (; at < registry.size() && y < 22; at++, y++) {
            putString(summary(registry[at]).c_str(), Coord_t{y, 0});
        }
        putString("/makeitem <id> edits; /items drop|on|off <id>", Coord_t{22, 0});
        waitForContinueKey(23);
    } while (at < registry.size());
    drawCavePanel();
}

} // namespace


void mu_items_load() {
    registry.clear();
    MuRows *r = mu_db_query((std::string("SELECT ") + COLUMNS + " FROM custom_item ORDER BY id").c_str(), "");
    while (r != nullptr && mu_db_next(r)) {
        registry.push_back(read_row(r));
    }
    mu_db_done(r);
    for (MuCustomItem const &d : registry) {
        set_name(d.slot, d.name);
    }
}

std::vector<MuCustomItem> const &mu_items() {
    return registry;
}

MuCustomItem const *mu_item_by_key(std::string const &key) {
    for (MuCustomItem const &d : registry) {
        if (d.key == key) {
            return &d;
        }
    }
    return nullptr;
}

MuCustomItem const *mu_item_by_id(int64_t id) {
    for (MuCustomItem const &d : registry) {
        if (d.id == id) {
            return &d;
        }
    }
    return nullptr;
}

std::string mu_items_define(MuCustomItem const &item) {
    MuCustomItem d = item;
    MuCustomItem const *old = mu_item_by_key(d.key);
    if (old != nullptr) {
        if (old->source == "admin") {
            return "the key " + d.key + " belongs to an admin's item";
        }
        d.id = old->id;
        d.slot = old->slot;
        d.enabled = old->enabled; // an admin's /items off stands
    }
    d.live = true;
    return save(d, "script");
}

void mu_items_scripts_unloaded() {
    for (MuCustomItem &d : registry) {
        d.live = false;
    }
}

void mu_items_make(MuCustomItem const &d, Inventory_t &item) {
    inventoryItemCopyTo(d.base, item);
    item.to_hit = (int16_t) d.to_hit;
    item.to_damage = (int16_t) d.to_damage;
    item.to_ac = (int16_t) d.to_ac;
    if (d.dice > 0 && d.sides > 0) {
        item.damage.dice = (uint8_t) d.dice;
        item.damage.sides = (uint8_t) d.sides;
    }
    item.flags |= d.flags;
    if (d.bonus != 0) {
        item.misc_use = (int16_t) (item.category_id == TV_STAFF || item.category_id == TV_WAND ? item.misc_use + d.bonus : d.bonus);
        if ((d.flags & (TR_STATS | TR_SPEED | TR_STEALTH | TR_SEARCH | TR_INFRA)) != 0) {
            item.identification |= config::identification::ID_SHOW_P1;
        }
    }
    if (weapon(item.category_id)) {
        item.identification |= config::identification::ID_SHOW_HIT_DAM;
    }
    item.special_name_id = (uint8_t) d.slot;
    item.cost = item.cost * (1 + d.tier) + mu_rarity_worth(d.tier) + 100 * (d.to_hit + d.to_damage + d.to_ac);
    if (item.cost < 0) {
        item.cost = 0;
    }
    item.mu_rarity = (uint8_t) d.tier;
}

bool mu_items_place(MuCustomItem const &d, Coord_t const &near) {
    Coord_t spot;
    if (!mu_loot_spot_near(near, 3, spot)) {
        return false;
    }
    int id = popt();
    mu_items_make(d, game.treasure.list[id]);
    dg.floor[spot.y][spot.x].treasure_id = (uint8_t) id;
    dg.floor[spot.y][spot.x].field_mark = true;
    dungeonLiteSpot(spot);
    if (d.tier >= MU_RARE) {
        muFxLoot(spot, d.tier);
    }
    return true;
}

int mu_items_drop(Inventory_t &item, int source, int depth) {
    if (registry.empty()) {
        return -1;
    }
    size_t start = (size_t) randomNumber((int) registry.size()) - 1;
    for (size_t n = 0; n < registry.size(); n++) {
        MuCustomItem const &d = registry[(start + n) % registry.size()];
        bool from = d.from == MU_FROM_ANYWHERE || (d.from == MU_FROM_BOSSES && source >= MU_LOOT_BOSS) || (d.from == MU_FROM_LORDS && source == MU_LOOT_LORD);
        bool usable = d.enabled && (d.source == "admin" || d.live) && d.one_in > 0;
        if (usable && from && depth >= d.min_depth && depth <= d.max_depth && randomNumber(d.one_in) == 1) {
            mu_items_make(d, item);
            return d.tier;
        }
    }
    return -1;
}

std::string mu_items_base_name(int base) {
    if (base < 0 || base >= MAX_OBJECTS_IN_GAME) {
        return "?";
    }
    DungeonObject_t const &o = game_objects[base];
    std::string out;
    for (const char *p = o.name; *p != '\0'; p++) {
        if (*p == '&' && p[1] == ' ') {
            p++;
        } else if (*p != '~' && *p != '^') {
            out += *p;
        }
    }
    switch (o.category_id) {
        case TV_RING:
            return "Ring of " + out;
        case TV_AMULET:
            return "Amulet of " + out;
        case TV_STAFF:
            return "Staff of " + out;
        case TV_WAND:
            return "Wand of " + out;
        default:
            return out;
    }
}

int mu_items_find_base(std::string const &name) {
    std::string want = lower(name);
    int partial = -1, partials = 0;
    std::string partial_name;
    for (int i = 0; i < MAX_DUNGEON_OBJECTS; i++) {
        if (!gear_category(game_objects[i].category_id) || (game_objects[i].flags & TR_CURSED) != 0) {
            continue;
        }
        std::string have = lower(mu_items_base_name(i));
        if (have == want) {
            return i;
        }
        if (have.find(want) != std::string::npos && have != partial_name) {
            partial = i;
            partial_name = have;
            partials++;
        }
    }
    return partials == 1 ? partial : -1;
}

uint32_t mu_items_power(std::string const &name) {
    std::string want = lower(name);
    for (int i = 0; i < power_count(); i++) {
        if (want == POWERS[i].key) {
            return POWERS[i].flag;
        }
    }
    return 0;
}

std::string mu_items_power_names(uint32_t flags) {
    std::string out;
    for (int i = 0; i < power_count(); i++) {
        if ((flags & POWERS[i].flag) != 0) {
            out += (out.empty() ? "" : ", ") + std::string(POWERS[i].label);
        }
    }
    return out.empty() ? "none" : out;
}

bool mu_items_command(MuSession *s, std::string const &cmd, std::string const &arg) {
    if (cmd == "makeitem") {
        MuCustomItem d;
        if (!arg.empty()) {
            MuCustomItem const *old = mu_item_by_id(atoi(arg.c_str()));
            if (old == nullptr) {
                old = mu_item_by_key(arg);
            }
            if (old == nullptr) {
                mu_system(s, "No custom item " + arg + ": /items lists them.");
                return true;
            }
            d = *old;
        } else {
            d.tier = MU_RARE;
            d.min_depth = s->level != nullptr && s->level->depth > 0 ? s->level->depth : 1;
            d.one_in = 200;
        }
        form(s, d);
        return true;
    }
    if (cmd != "items") {
        return false;
    }
    std::string what = arg.substr(0, arg.find(' '));
    std::string which = arg.find(' ') != std::string::npos ? arg.substr(arg.find(' ') + 1) : "";
    if (what.empty()) {
        list_screen();
        return true;
    }
    MuCustomItem const *found = mu_item_by_id(atoi(which.c_str()));
    if (found == nullptr) {
        found = mu_item_by_key(which);
    }
    if (found == nullptr) {
        mu_system(s, "Use: /items, or /items drop|on|off <id>");
        return true;
    }
    MuCustomItem d = *found;
    if (what == "drop") {
        if (s->level == nullptr || s->level->depth == 0) {
            mu_system(s, "Not in the town.");
        } else {
            mu_system(s, mu_items_place(d, py.pos) ? d.name + " lies beside you." : "No room beside you.");
            mu_db_audit(s->account.name, "dropitem", d.key.c_str(), "");
        }
    } else if (what == "on" || what == "off") {
        d.enabled = what == "on";
        (void) save(d, s->account.name);
        mu_system(s, d.name + (d.enabled ? " is in the drops." : " is out of the drops."));
        mu_db_audit(s->account.name, ("item" + what).c_str(), d.key.c_str(), "");
    } else {
        mu_system(s, "Use: /items, or /items drop|on|off <id>");
    }
    return true;
}
