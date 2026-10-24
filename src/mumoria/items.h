// Mumoria - custom items: made by admins (/makeitem) or by scripts
// (mu.item), and dropped like any loot. docs/MUMORIA_DESIGN.md 2.22.

#pragma once

#include "../headers.h"

#include <vector>

struct MuSession;

// Where a custom item may drop from.
enum MuItemFrom { MU_FROM_ANYWHERE, MU_FROM_BOSSES, MU_FROM_LORDS, MU_FROM_NEVER };

struct MuCustomItem {
    int64_t id = 0;
    std::string key;    // unique: "item<id>" for an admin's, the script's own otherwise
    std::string name;   // "Thunderfury" ("'Thunderfury'" on the item), or "of the Storm"
    int base = -1;      // game_objects index
    int tier = 0;       // MuRarity
    int to_hit = 0, to_damage = 0, to_ac = 0;
    int dice = 0, sides = 0; // 0: the base item's
    int bonus = 0;           // misc_use: the amount of its stats, speed, stealth...
    uint32_t flags = 0;      // powers (TR_*)
    int min_depth = 1, max_depth = 127;
    int from = MU_FROM_BOSSES;
    int one_in = 0;     // drops from 1 in this many gear drops it may come from; 0 never
    int slot = 0;       // its name's special_name_id
    bool enabled = true;
    std::string source; // "admin" or the script file
    bool live = false;  // a script item: defined by the scripts now loaded
};

// At start: the items and their names, from the database.
void mu_items_load();
// The registry (read only).
std::vector<MuCustomItem> const &mu_items();
MuCustomItem const *mu_item_by_key(std::string const &key);
MuCustomItem const *mu_item_by_id(int64_t id);

// A script's item (mu.item): made or updated by key. "" on success, else why not.
std::string mu_items_define(MuCustomItem const &item);
// Before scripts (re)load: their items are not live until defined again.
void mu_items_scripts_unloaded();

// Turns item into a custom item.
void mu_items_make(MuCustomItem const &def, Inventory_t &item);
// Places one on the loaded level at or near coord; false if no room.
bool mu_items_place(MuCustomItem const &def, Coord_t const &near);
// A new gear drop (MuLootSource source) at depth: may become a custom item.
// Returns its tier, or -1 when it stays what it is.
int mu_items_drop(Inventory_t &item, int source, int depth);

// The base item's plain name ("Long Sword", "Ring of Strength").
std::string mu_items_base_name(int base);
// A base item by name, exact (case aside) or else the only one containing it; -1 if none.
int mu_items_find_base(std::string const &name);
// Power names (str, slay_evil, free_action...) and their flags.
uint32_t mu_items_power(std::string const &name); // 0 if unknown
std::string mu_items_power_names(uint32_t flags);

// Admin: /makeitem [id] (the form), /items [drop|on|off id]. True if handled.
bool mu_items_command(MuSession *s, std::string const &cmd, std::string const &arg);
