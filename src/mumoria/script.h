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
// A chat command no one else knew: true if a script took it.
bool mu_script_command(MuSession *s, std::string const &cmd, std::string const &args);
// "/a /b" of the scripts' commands, for /help ("" if none).
std::string mu_script_command_list();

// Admin /scripts: the files loaded and the last errors.
bool mu_script_admin(MuSession *s, std::string const &cmd);
