// Mumoria - boss encounters (docs/MUMORIA_DESIGN.md 2.16).

#pragma once

struct MuLevel;
struct MuSession;

bool mu_boss_level(int depth);
// A new level, loaded: put its boss on it (if it is a boss level).
void mu_boss_place(MuLevel *level);
// Says so when a player arrives on a level whose boss is alive.
void mu_boss_announce(MuSession *s, MuLevel *level);
// An admin's event: a boss, awake, beside the loaded player (lock held).
bool mu_boss_spawn_beside(MuLevel *level);
