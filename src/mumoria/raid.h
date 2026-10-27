// Mumoria - raids (docs/MUMORIA_DESIGN.md 2.20).

#pragma once

#include "../headers.h"

#include <string>
#include <vector>

struct MuSession;
struct MuLevel;

// A raid instance: a level made for the party that went through a gate.
struct MuRaid {
    int gate = 0;     // the depth of the gate it was entered from
    int theme = 0;    // index in raid.cpp's THEMES
    int party = 1;    // how many it was made for
    Coord_t entry{0, 0};
    int boss_hp = 0;  // the lord's full hit points
    int waves = 0;    // reinforcements it has called
    bool won = false;

    // A script's dungeon (script.cpp mu.dungeon) rather than a gate's raid.
    std::string script;       // its key; empty for a raid
    std::string title, label; // "the Rat-King's Sewer"; on the depth line
    int id = 0;               // this instance, for the script's state
    std::vector<uint8_t> marks; // the map's letter on each square (y * MAX_WIDTH + x), 0 for none
};

// A script dungeon instance (null for a raid or a plain level).
MuRaid *mu_dungeon_of(MuLevel const *level);

bool mu_raid_gate_level(int depth);
// The loaded level was just made for a raid from `gate`: fill it.
MuRaid *mu_raid_build(int gate, int party);
// On a gate level (loaded): a gate somewhere, if it has none.
void mu_raid_place_gate(MuLevel *level);
// The raid gate on the loaded level, if any.
bool mu_raid_find_gate(Coord_t &out);
void mu_raid_announce(MuSession *s, MuLevel *level);
// A captain (mu_boss 2) or the lord (3) dies: its hoard, before the party's
// loot rolls. Then, for the lord, mu_raid_rewards(): everyone's own reward.
void mu_raid_monster_dies(int monster_id);
void mu_raid_rewards(Coord_t const &center);
// How many of s's party stand on s's level (the raid is made for them).
int mu_raid_party_here(MuSession *s);
// A raid from this gate that one of s's party is in, or null.
MuLevel *mu_raid_of_party(MuSession *s, int gate);
// Admin /raid: into this level's raid at once; /raid lord: beside its lord.
std::string mu_raid_admin(MuSession *s, std::string const &arg);
// "the Orc Warrens", or null when the level is not a raid.
const char *mu_raid_title(MuLevel const *level);
// A script dungeon `key` that one of s's party is in, or null.
MuLevel *mu_dungeon_of_party(MuSession *s, std::string const &key);
// s (loaded) leaves its script dungeon for where it came from.
void mu_dungeon_leave(MuSession *s);
// Its way out: a store door sub-category 111, as a raid's.
void mu_dungeon_place_exit(Coord_t const &c);
