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
// How the map square coord, showing ch, is drawn (colour.cpp): a player's
// "@" in its colours, an item in its rarity's; 0 for plain.
uint32_t muTileAttr(Coord_t const &coord, char ch);
const char *muOtherPlayerNameAt(Coord_t const &coord);
// Another player at coord: their name, and male and race for their voice; null if none.
const char *muOtherPlayerVoiceAt(Coord_t const &coord, bool &male, int &race);
// Moving onto coord: the name of a player there who is not in the mover's
// party (they block it); null if the way is clear (party members walk through).
const char *muPlayerInTheWay(Coord_t const &coord);
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
// each hit and miss, returns the damage done (critical: a blow of it was).
int muPlayerMeleeDamage(int target_ac, const char *target_name, Coord_t target, bool &critical);
// The end of each game turn (others on the level are told to redraw).
void muTurnDone();
// Town buildings 7-9 (store door sub-categories 107-109).
void muEnterBuilding(int sub_category_id);
// The stores are shared (shops.cpp): each customer shops in their own view
// of one. muStoreEnter makes it (false: the doors are locked to them).
bool muStoreEnter(int store_id);
void muStoreLeave(int store_id);
// Before paying: false (and says so) when the shared supply has just run out.
bool muStoreCanBuy(int store_id, Inventory_t const &item);
void muStoreBought(int store_id, Inventory_t const &item);
// Umoria's store turnover, for the shared stores: at most every few minutes.
void muStoreTurnover();
// Three more buildings in the town (Bank, Guild Hall, Auction House).
void muPlaceTownBuildings();
bool muWizardAllowed();
// A free floor square, not under anyone.
void muFindSpot(Coord_t &coord);
// Moves the loaded player's map panel to show coord (ui.cpp); draws nothing.
void muPanelFollow(Coord_t coord);
// Umoria drew off the screen (ui_io.cpp), where it would abort: logged with
// a backtrace, and the drawing is skipped rather than ending the server.
void muOffScreen(const char *what, Coord_t coord);
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
// A missile's sounds: shoot as it goes ("bow_shoot", "throw"), and what it
// is ("arrow": arrow_hit, arrow_miss) as it lands.
void muFxRay(Coord_t from, int direction, Coord_t end, int element, int outcome, char glyph = 0, const char *shoot = nullptr,
             const char *missile = nullptr);
void muFxBall(Coord_t from, int direction, Coord_t center, int element);
void muFxBreath(Coord_t from, Coord_t center, int element);
// A monster's spell, square to square: MU_FX_HIT, MU_FX_AFFECT or MU_FX_RESIST.
void muFxBolt(Coord_t from, Coord_t to, int element, int outcome);
// A monster's blow (monster_attacks description id). seen: the monster is lit.
void muFxMonsterBlow(Coord_t from, Coord_t target, int description, bool hit, bool seen);
// One melee blow; weapon is the wielded item's category (TV_*), object_id
// its game_objects[] entry (the sound is by kind: a sword's, an axe's).
void muFxSwing(Coord_t from, Coord_t target, int weapon, int object_id, int outcome, bool critical);
// Damage as a number over the map, for the player whose fight it is: dealt
// over what they hit, taken over their own @, healing and mana as +N. It
// shows at the next animation's moment of impact, or by muFxDamageShow()
// (the turn's end) if no animation comes. Its colour: white dealt, gold a
// critical, red taken, bright red a critical taken, green healing, blue
// mana; a spell's damage orange (fire), cyan (frost) or purple (the rest),
// by the animation it comes with.
enum MuFxNumber { MU_NUM_DEALT, MU_NUM_TAKEN, MU_NUM_HEAL, MU_NUM_MANA };
void muFxDamage(Coord_t at, int amount, int kind);
// The next damage dealt or taken is a critical blow's.
void muFxDamageCritical();
void muFxDamageShow();
// A chat bubble over the player at `at` who just spoke.
void muFxChat(Coord_t at);
// A sound from square at, for everyone near enough to hear it (fx.cpp);
// the animations above make their own. Names: client/sounds/README.md.
void muSound(Coord_t at, const char *name);
// The cry of whoever is at `at` (a monster's own, a player's man's or
// woman's): "attack", "hurt", "crit", "die" or "roar".
void muVoice(Coord_t at, const char *event);
// A monster died at `at` (muMonsterDies): its death cry comes with the blow after.
void muFxDeath(Coord_t at, int creature_id);
// While one is alive, animations play at the same time (Starlite's eight beams).
struct MuFxTogether {
    MuFxTogether();
    ~MuFxTogether();
};
// drawDungeonPanel(): where this screen's map is, and that it is being drawn.
void muMapPanel(bool begin);
void muMapTile(bool begin);

// Loot rarity (loot.cpp). Appends " <Epic>" and the like to a description.
void muItemRarityTag(Inventory_t const &item, char *description);
// Item lists: the next line in the item's rarity colour; null for plain again.
void muItemColour(Inventory_t const *item);
// Wearing it: false (and says so) below the level its rarity asks.
bool muCanWear(Inventory_t const &item);
// Going into the loaded player's pack: a rare find is told and logged, once.
void muItemCarried(Inventory_t &item);
// dungeonPlaceRandomObjectAt(): a new item on the floor may be rare.
void muFloorItem(Coord_t const &coord, int treasure_id);
// Around monsterDeath(): its drops are rolled by muMonsterDies(), not as floor items.
void muLootDropping(bool dropping);
// A rare item appears (or is found): a glow in its rarity's colour.
void muFxLoot(Coord_t at, int tier);

// Scripts (script.cpp): a monster's name (its own, if a script gave it one);
// a script's portal (store door 112, its key in the inscription) stepped on.
const char *muMonsterName(Monster_t const &monster);
// The letter it is drawn as: its tag's own (mu.letter), else its creature's.
uint8_t muMonsterSprite(Monster_t const &monster);
void muScriptPortal(const char *key);
// A monster a script moves itself (mu.hold): the engine gives it no turns.
bool muMonsterHeld(Monster_t const &monster);
// A script's NPC (mu.spawn{npc=true}): held, unharmed, and talked to, not hit.
bool muMonsterFriendly(Monster_t const &monster);
// The player walked into monster creature_id: true if it is an NPC (its
// script's talk event fires) rather than something to attack.
bool muMonsterTalk(int creature_id);

// Raids (raid.cpp): a raid gate (110), a raid's way out (111) or a script
// dungeon's stairs (113 down, 114 up) stepped on; a script raid's gate (115,
// its key in the inscription).
void muRaidGate(int sub_category_id);
void muScriptGate(const char *key);
// The player walked into a script's decor (raid.h MU_DECOR): true if it was
// decor (its script hears of it, or its name is told), else false.
bool muDecorBump(Coord_t const &coord);
bool muIsDecor(Coord_t const &coord);
// A monster was hurt and lives (a raid's lord calls its guard).
void muMonsterHurt(int monster_id);
// The loaded level's raid, for the depth line; null when not in one.
const char *muRaidName();

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
