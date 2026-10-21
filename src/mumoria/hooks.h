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
const char *muOtherPlayerNameAt(Coord_t const &coord);
// Is another player on this level closer to coord than `distance`?
bool muNearerPlayer(Coord_t const &coord, int distance);

// Experience for a kill (shared out in a party).
void muKillExperience(Creature_t const &creature);
// A monster is about to die (bosses drop their loot here).
void muMonsterDies(int monster_id);

// Keys Umoria leaves unused ('): true if handled.
bool muCommand(char key);
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
