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
    int floor = 1;            // which of its floors (1 the first)
    int run = 0;              // the floors made for one party share it (the first floor's id)
    bool shared = false;      // one copy for everyone (an inn), not one per party
    bool safe = false;        // players do not fight here
    bool lit = false;         // all of it lit and known on arrival: nothing to explore
    Coord_t up{0, 0}, down{0, 0}; // its stairs to the floor above (or the way out) and below
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
// Stairs between a dungeon's floors: store doors 113 (down) and 114 (up).
void mu_dungeon_place_stairs(Coord_t const &c, bool down);
// A script's raid gate: store door 115, the dungeon's key in the inscription.
const uint8_t MU_SCRIPT_GATE = 115;
// The loaded level, as Umoria made it, becomes a script dungeon's floor: no
// stairs or monsters; a way in (and out, or up) and, if `down`, stairs down
// as far from it as can be found.
void mu_dungeon_generated(MuRaid *d, bool down);
// A free open square (no wall, monster, item or player) between `near` and
// `far` squares from `from`; false if none was found.
bool mu_raid_spot(Coord_t const &from, int near, int far, Coord_t &out);
// Of many free squares, the one farthest from `from`.
Coord_t mu_raid_farthest(Coord_t const &from);
// Decor: a script's furniture, an object that blocks the way (a table, the
// bar). Rubble underneath (sub-category MU_DECOR), its name in the
// inscription, its colour in to_ac.
const uint8_t MU_DECOR = 200;
bool mu_is_decor(Inventory_t const &item);
void mu_place_decor(Coord_t const &c, char glyph, std::string const &name, int colour);
