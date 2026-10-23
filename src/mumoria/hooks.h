// Mumoria - what the Umoria engine calls when built as the server
// (MUMORIA defined). headers.h includes this; each call site is behind
// #ifdef MUMORIA, so the single-player build never sees any of it.

#pragma once

// A key for getKeyInput(): gives the world to others while waiting.
int muGetKey();
// checkForNonBlockingKeyPress(): lets others take a turn, then says whether
// a key is waiting (and takes it, as Umoria does).
bool muPollKey();
void muBell();
// exitProgram()/abortProgram(): ends this player's session, not the server.
[[noreturn]] void muExitProgram(const char *why);
// endGame(), after the tomb: records the death and ends the session.
[[noreturn]] void muEndGame();

// Other players on the loaded level.
bool muOtherPlayerAt(Coord_t const &coord);
// How the "@" on coord is drawn (colour.cpp): 0 when it is not a player.
uint32_t muTileAttr(Coord_t const &coord);
const char *muOtherPlayerNameAt(Coord_t const &coord);
// Is another player on this level closer to coord than `distance`?
bool muNearerPlayer(Coord_t const &coord, int distance);

// Experience for a kill (shared out in a party).
void muKillExperience(Creature_t const &creature);
// A monster is about to die: bosses drop loot, a party or guild rolls for the
// drops (items placed since treasure_before).
void muMonsterDies(int monster_id, int treasure_before);
// Gold picked up: shared in a party or guild. Returns the line to print.
std::string muGoldFound(int32_t gold, const char *description);

// Keys Umoria leaves unused (' and "): true if handled. It sets
// game.player_free_turn: chat is free, an attack takes a turn.
bool muCommand(char key);
// One round of Umoria's melee against another player (player.cpp): says
// each hit and miss, returns the damage done.
int muPlayerMeleeDamage(int target_ac, const char *target_name, Coord_t target);
// The end of each game turn (others on the level are told to redraw).
void muTurnDone();
// Town buildings 7-9 (store door sub-categories 107-109).
void muEnterBuilding(int sub_category_id);
// Only one player at a time in a store; false if it is busy.
bool muStoreEnter(int store_id);
void muStoreLeave(int store_id);
// Three more buildings in the town (Bank, Guild Hall, Auction House).
void muPlaceTownBuildings();
bool muWizardAllowed();
// A free floor square, not under anyone.
void muFindSpot(Coord_t &coord);
// Umoria's 1 in MON_CHANCE_OF_NEW, divided by the number of players and
// parties exploring the level (never below 1 in 40). The town keeps its own.
int muNewMonsterChance();
// Each turn in the town: while it has more people than a new town has (4 by
// day, 8 by night), someone nobody can see leaves; while it has fewer, 1 in
// MON_CHANCE_OF_NEW someone new arrives, asleep and out of everyone's sight.
void muTownPeople();

// Animations (fx.cpp), shown to everyone on the level who can see them. One
// player's animations play one after another.
enum MuFxElement {
    // the first seven are MagicSpellFlags
    MU_FX_MISSILE, MU_FX_LIGHTNING, MU_FX_GAS, MU_FX_ACID, MU_FX_FROST, MU_FX_FIRE, MU_FX_HOLY,
    MU_FX_MIND, MU_FX_DRAIN, MU_FX_MUD, MU_FX_LIGHT, MU_FX_ARCANE, MU_FX_HEAL, MU_FX_FIZZLE, MU_FX_PHYSICAL, MU_FX_DARK,
};
// How a ray ends at its last square. MU_FX_HIT becomes a kill when the square is empty after.
enum MuFxEnd { MU_FX_NONE, MU_FX_HIT, MU_FX_KILL, MU_FX_AFFECT, MU_FX_RESIST, MU_FX_MISS, MU_FX_WALL };
void muFxCast(Coord_t at, int element);
void muFxHeal(Coord_t at);
// From `from` in `direction` up to `end`. glyph 0: the element's own (an arrow's: its point).
void muFxRay(Coord_t from, int direction, Coord_t end, int element, int outcome, char glyph = 0);
void muFxBall(Coord_t from, int direction, Coord_t center, int element);
void muFxBreath(Coord_t from, Coord_t center, int element);
// A monster's spell, square to square: MU_FX_HIT, MU_FX_AFFECT or MU_FX_RESIST.
void muFxBolt(Coord_t from, Coord_t to, int element, int outcome);
// A monster's blow (monster_attacks description id). seen: the monster is lit.
void muFxMonsterBlow(Coord_t from, Coord_t target, int description, bool hit, bool seen);
// One melee blow; weapon is the wielded item's category (TV_*).
void muFxSwing(Coord_t from, Coord_t target, int weapon, int outcome, bool critical);
// While one is alive, animations play at the same time (Starlite's eight beams).
struct MuFxTogether {
    MuFxTogether();
    ~MuFxTogether();
};
// drawDungeonPanel(): where this screen's map is, and that it is being drawn.
void muMapPanel(bool begin);
void muMapTile(bool begin);

// While one of these is alive, a prompt does not hold the player's level:
// it is a menu or a question before anything has changed.
struct MuSafePrompt {
    MuSafePrompt();
    ~MuSafePrompt();
};

// Marks the main command prompt (where the map can be redrawn when others move).
struct MuTopPrompt {
    MuTopPrompt();
    ~MuTopPrompt();
};

// Engine entry points the server calls (game_run.cpp).
void muEngineInit(uint32_t seed);
void muEngineNewCharacter();
void muEnginePlay();
// The player took the stairs (or fell, or recalled): the world deals the level.
void muNewLevel();
void muRunStateGet(int &prevdir, int &direction);
void muRunStateSet(int prevdir, int direction);
