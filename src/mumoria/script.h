// Mumoria - Lua scripts: plugins that add items, bosses, events and
// commands (docs/MUMORIA_DESIGN.md 2.23; the API is scripts/README.md).

#pragma once

#include "../headers.h"

struct MuSession;
struct MuLevel;

// At start (world lock held): loads every *.lua in dir, sorted by name.
void mu_script_init(std::string const &dir);
// Admin /reload: a fresh Lua, every script loaded again. Returns a summary.
std::string mu_script_reload();

// Events (world lock held, the player loaded).
void mu_script_enter(MuSession *s, MuLevel *level);
void mu_script_kill(int monster_id); // the loaded player killed it; still on the level
void mu_script_hurt(int monster_id); // a script's boss was hurt and lives
// The end of a player's turn: due mu.every timers run (once a second a level at most).
void mu_script_tick(MuSession *s);
// A monster's tag (Monster_t::mu_script) is a boss's: it drops a boss's loot.
bool mu_script_is_boss(int tag);

// Script dungeons (mu.dungeon): the depth `key` plays at (-1: no such dungeon);
// drawing a new instance (loaded, level->raid set); a step on a marked
// square; an instance deleted.
int mu_script_dungeon_depth(std::string const &key);
bool mu_script_dungeon_build(MuLevel *level);
void mu_script_step(MuSession *s, char letter, Coord_t const &at);
void mu_script_dungeon_gone(int id);
// A chat command no one else knew: true if a script took it.
bool mu_script_command(MuSession *s, std::string const &cmd, std::string const &args);
// "/a /b" of the scripts' commands, for /help ("" if none).
std::string mu_script_command_list();

// Admin /scripts: the files loaded and the last errors.
bool mu_script_admin(MuSession *s, std::string const &cmd);
