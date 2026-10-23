// Mumoria - loot rarity (docs/MUMORIA_DESIGN.md 2.21).

#pragma once

#include "../headers.h"

struct MuSession;

enum MuRarity { MU_COMMON, MU_UNCOMMON, MU_RARE, MU_EPIC, MU_LEGENDARY, MU_RARITY_COUNT };

// Where an item came from: how its rarity is rolled.
enum MuLootSource { MU_LOOT_FLOOR, MU_LOOT_MONSTER, MU_LOOT_BOSS, MU_LOOT_CAPTAIN, MU_LOOT_LORD, MU_LOOT_REWARD };

// Inventory_t::mu_rarity: the tier in the low three bits; MU_RARITY_TOLD once
// a finder has been told of it (and it was logged).
#define MU_RARITY_TIER 0x07
#define MU_RARITY_TOLD 0x08

int mu_rarity(Inventory_t const &item);
const char *mu_rarity_name(int tier);
int mu_rarity_level(int tier); // the character level it needs to be worn

// Rolls a new item's rarity and makes it so: better bonuses, powers, a name.
// owner: whose luck (and level) counts; null for a floor item. Returns the tier.
int mu_loot_bless(Inventory_t &item, int source, int item_level, int creature_level, MuSession *owner, int min_tier = MU_COMMON);
// A monster died (the loaded player killed it): the items it dropped, placed
// since treasure_before around center, are rolled. source: MU_LOOT_MONSTER,
// _BOSS, _CAPTAIN or _LORD.
void mu_loot_drops(int treasure_before, Coord_t const &center, int source, int creature_level);
// A rare item's first finder. Called by muItemCarried().
void mu_loot_found(MuSession *s, Inventory_t const &item);

// The map: a rare item glows in its rarity's colour (0 when plain).
uint32_t mu_loot_tile_attr(Coord_t const &coord, char ch);

// Admin /makeloot <tier>: an item of that rarity beside the loaded player.
std::string mu_loot_admin(MuSession *s, std::string const &arg);

// Chat commands: /loot, /legends, /item. True if cmd was one of them.
bool mu_loot_command(MuSession *s, std::string const &cmd, std::string const &arg);
