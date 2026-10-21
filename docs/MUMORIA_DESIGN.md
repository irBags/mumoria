# Mumoria - server design

Mumoria is Umoria 5.7.15 turned into a shared, persistent, multiplayer world.
The game rules are Umoria's own C/C++ source in `src/`, run unchanged by a
server that holds every character and level. The python port in
`python-umoria/` is a reference for the multiplayer rules it already worked
out (parties, shared monsters, rewards). None of its FDO or AOL window code is
used.

## 1. The core idea: one engine, many players, a context swap

Umoria keeps the whole game in globals: `py` (the character), `dg` (the level),
`monsters[]`, `game.treasure` (the items on the floor), `objects_identified[]`,
the message history, and a few statics. Rewriting all of that into per-player
objects would touch every one of its 31,000 lines and would put the original
gameplay at risk.

Instead, the server keeps one copy of those globals for each **player** and
one for each **level**, and swaps them in and out:

* Each connected player has a thread that runs Umoria's own main loop
  (`playDungeon`).
* Exactly one thread runs the engine at a time, holding the **world lock**
  (a fair ticket lock, `src/mumoria/world.cpp`).
* When a thread takes the lock and the loaded player or level is not its own,
  the world saves the loaded copies and loads the thread's own
  (`mu_world_enter`).
* A thread gives the lock back whenever Umoria waits for a key
  (`getKeyInput`), and at every turn while it runs or rests
  (`checkForNonBlockingKeyPress`). So a player thinking about a prompt never
  stops anyone else.

Every rule is still Umoria's: combat, spells, monsters, stores, items, death.
The single-player `umoria` target builds exactly as before. Every engine
change sits behind `#ifdef MUMORIA`.

**Language.** The engine stays the C++14 that Umoria 5.7 ships as. The new
server layers are C: the network and telnet code (`net.c`), the virtual
terminal (`vterm.c`), the database (`db.c`), password hashing and tokens
(`auth.c`), the IRC bridge (`irc.c`) and the client
(`client/mumoria_client.c`). The glue that has to touch the engine's globals is
C++ (`world.cpp`, `session.cpp`, `social.cpp`, `screens.cpp`, `boss.cpp`).

### When a player stops in the middle of an action

A prompt that comes **before** anything has changed is safe to wait at. That
covers the command prompt, item and direction pickers, the spell list,
shops, the Mumoria screens and the chat line (`MuSafePrompt`). Anywhere
else, most often a `-more-` while monsters are acting, the player is stopped
part-way through changing their level. Their level is **held**: players on
other levels carry on, and players on the same level wait. A held prompt
answers itself (ESC) after `MU_BUSY_TIMEOUT_MS` (5 s), but only when
somebody is waiting. Pausing there is safe because the world restores that
level byte for byte before the player resumes.

Two further rules:

* **Shops:** one customer at a time per shop. Another customer's purchase
  would move the item letters.
* **Leaving:** while a player who has hung up is being saved, a prompt that
  turns up answers itself instead of waiting (`MuSession::closing`).

### What is per player and what is per level

| Per player (`MuPlayerCtx`) | Per level (`MuLevel`) |
| --- | --- |
| `py`, `game` (except the floor items), `dg.panel` | `dg.floor`, height, width, game turn |
| `objects_identified`, `creature_recall` | `monsters[]`, `next_free_monster_id`, `monster_multiply_total` |
| message history, run state, options | `game.treasure` (the items on the floor) |
| field marks and lamp light (as bitmaps) | the boss state |

Field marks (what a player remembers seeing) and lamp light are per player.
They are written into the shared floor when that player is loaded. Only the
loaded player's square carries `creature_id == 1`, so a monster can only ever
strike the player whose turn it is.

### Monsters with more than one player

This is the python port's rule (`python-umoria/mp_monsters.py`): a monster
acts only on the turns of the player **nearest** to it. A monster chasing
player A moves on A's turns. A player who stands still is not hunted in the
meantime, which is what Umoria's turn-based pace already means. Other players
block both monsters and players. Bumping into one prints "Name is in your way."

## 2. Requirements, one by one

Each entry gives the rationale, the files, the database, the networking, the
steps, and what stays compatible.

### 2.1 Persistent, server-authoritative world

* **Why:** the client sends nothing but keystrokes. Everything the player sees
  is drawn by the server, so no client state can be trusted or forged.
* **Files:** `src/mumoria/world.cpp` (lock, context swap, levels),
  `src/mumoria/session.cpp` (a player's thread), `src/mumoria/vterm.c` (a
  virtual 80x30 screen per session), `src/mumoria/mu_curses.h` (curses for the
  engine, writing to that screen), `src/ui_io.cpp` (`#ifdef MUMORIA` input).
* **DB:** `level` (one row per depth, the level as a blob), and `world` (the
  magic seed, the town seed, the store stock).
* **Network:** see 2.13.
* **Steps:** (1) build the engine against `mu_curses.h`; (2) per-session
  threads plus the world lock; (3) swap the contexts in `mu_world_enter`;
  (4) send the screen diff on every `putQIO`.
* **Compatibility:** `umoria` (single player, ncurses) is unchanged.

### 2.2 Database-backed characters and world

* **Why:** one durable store, in a single file, with transactions. That is
  what the anti-dupe design (2.14) needs.
* **Files:** `src/mumoria/db.c`, `db.h`, `sqlite3_min.h` (the subset of the
  SQLite API, so the build needs only the system's `libsqlite3.so.0`).
* **DB:** `mumoria.db`, WAL mode, `PRAGMA user_version` migrations. The
  schema is in `db.c` (`SCHEMA_V1`).
* **Characters** are Umoria's own save format (`saveGame`/`loadGame`),
  written through a temporary file into `character.save`. An existing
  `game.sav` from single player can be imported, which is the backward
  compatibility path.
* **Steps:** saves happen on a level change, every `MU_AUTOSAVE_SECONDS`, on
  disconnect, and inside every economy transaction.

### 2.3 Global persistent maps

* **Why:** everyone at 250 ft is in the same level, and it stays as it was
  left (doors, kills, items on the floor).
* **Files:** `world.cpp` (`mu_level_get`, `mu_level_save`), the
  `dungeon_generate.cpp` hooks.
* **DB:** `level(depth, blob, saved_at, created_at, boss_*)`. The blob is the
  raw floor, monsters and items, with a size and version header. A header that
  does not match means the level is generated again.
* **Steps:** a level is made the first time anyone enters its depth. It is
  saved every `MU_LEVEL_SAVE_SECONDS` while occupied and when the last player
  leaves. A level left empty for `MU_LEVEL_RESET_HOURS` is generated anew,
  which repopulates the dungeon. The town is one shared level, made from the
  world's `town_seed`.
* **Compatibility:** Umoria's level generator is used as is.

### 2.4 Accounts and authentication

* **Files:** `src/mumoria/auth.c` (PBKDF2-HMAC-SHA256, 100,000 rounds, a
  16-byte random salt), `sha256.c`, `session.cpp` (the login screens).
* **DB:** `account(id, name, folded, pw_hash, salt, role, created, last_login,
  banned_until, ban_reason, muted_until)`. `login_attempt(ip, at)` for
  throttling.
* **Network:** login runs over the same telnet stream, with echo off for the
  password. An **AOL hand-off token**
  `name:expiry:hex(HMAC-SHA256(secret, name:expiry))` logs a screen name in
  without a password, when the P3OL server launches the game for an AOL
  member (2.11).
* **Steps:** 5 failures in 10 minutes from one address locks that address
  out. One session per account: a second login saves and drops the first.

### 2.5 Parties

* **Why:** play together on one level, with Umoria's own rules.
* **Files:** `src/mumoria/social.cpp` (`/invite`, `/accept`, `/decline`,
  `/leave`, `/kick`, `/p`).
* **DB:** none. Parties live for as long as the session does, as in the python
  port.
* **Rules (from the python port):** five members at most. A kill shares its
  experience between the members on that level, at
  `exp * (100 + 6n) / (100n)` each. Whoever finds gold keeps it.
* **Steps:** the share is credited straight into each member's context, and
  their level-up is run when they are next loaded.

### 2.6 to 2.8 Guilds, ranks and permissions, guild banks

* **Files:** `social.cpp` (commands), `screens.cpp` (the Guild Hall screen),
  `db.c`.
* **DB:**
  * `guild(id, name, folded, tag, leader_account, motd, gold, created)`
  * `guild_rank(guild, rank, title, perms)`, rank 0..4 (Leader, Officer,
    Veteran, Member, Recruit)
  * `guild_member(account, guild, rank, joined)`
  * `guild_invite(guild, account, by, at)`
  * `guild_vault(id, guild, item blob, deposited_by, at)`
* **Permissions** (bits): invite, kick, promote, demote, deposit, withdraw,
  set the MOTD, edit the ranks. The leader holds them all and can disband. A
  member can only act on lower ranks.
* **Guild bank:** gold and up to `MU_GUILD_VAULT_SLOTS` items, in the Guild
  Hall (town building `8`). Every movement is written to `ledger`.
* Guilds belong to **accounts**, so they outlive Umoria's permadeath.
* Founding a guild costs `guild_cost` gold (`mumoria.conf`, 1000 by default),
  paid from what the character carries. Founded at the Guild Hall (`c`) or
  with `/guild create <name>`.
* Commands: `/guild invite|join|leave|kick|promote|demote|motd|rank|members|disband`.
  `/guild rank <1-4> <title> [permission bits]` renames a rank and sets
  its permissions.

### 2.9 Personal banks

* **Files:** `screens.cpp` (the Bank, town building `7`), `db.c`.
* **DB:** `bank(account, gold)`, `bank_item(id, account, item blob, at)`, up
  to `MU_BANK_SLOTS` items. These belong to the account, so they outlive a
  dead character (as in the python port's East End).

### 2.10 Global auction house

* **Files:** `screens.cpp` (the Auction House, town building `9`), `db.c`.
* **DB:** `auction(id, seller, item blob, start, buyout, high_bid, bidder,
  ends_at, state)` and `claim(id, account, item blob, gold, reason, at)`.
* **Rules:** the seller picks 8, 24 or 48 hours, plus an optional buyout.
  A bid takes its gold at once (held in escrow). An outbid player's gold goes
  back to their bank. When an auction ends, the winner gets a claim for the
  item, the seller gets the gold in their bank (minus `MU_AH_CUT_PERCENT`),
  and an unsold item goes back to the seller as a claim. Claims are collected
  at the Auction House.
* **Steps:** auctions are settled on any Auction House visit and every minute
  by a timer.

### 2.11 Chat: world, guild, party, private, and the AOL rooms

* **Files:** `social.cpp` (`mu_chat_*`), `vterm.c` (the chat pane), `irc.c`.
* **UI:** rows 24-29, below Umoria's own 24-line screen, are the chat pane, so
  no Umoria line moves. `'` opens the chat line on the message line. Plain
  text goes to the level ("say"). `/w` world, `/g` guild, `/p` party,
  `/t name` private, `/r` reply.
* **AOL:** `irc.c` joins a configured IRC channel (the P3OL rooms are IRC, on
  the Ergo server) and relays world chat both ways. AOL members in that chat
  room talk with the players in the game.
* **Limits:** at most 5 lines in 5 seconds and 200 characters a line. Mutes
  are enforced (2.15).

### 2.12 Trading between players

* **Files:** `screens.cpp` (the trade screen, drawn like Umoria's store
  screen), `social.cpp` (`/trade name`).
* **Rules:** both players are on the same level. Each offers up to 8 pack
  items and some gold. Changing an offer clears both confirmations. When both
  confirm, the swap runs under the world lock and both characters are saved
  **in one DB transaction**. A trade with no room in a pack fails without
  changing anything.

### 2.13 Networking and concurrency

* TCP, telnet (`IAC WILL ECHO`, `WILL SGA`, NAWS read), and ANSI output.
  Any terminal works (PuTTY, telnet). `client/mumoria_client.c` is a small C
  client that puts the terminal in raw mode.
* One thread per connection. Output is the diff of the session's virtual
  screen, so a moved `@` costs a few bytes.
* Input: at most `MU_MAX_KEYS_PER_SEC` keys a second. Anything above that is
  dropped. A session idle for `MU_IDLE_SECONDS` is saved and dropped.

### 2.14 Anti-cheat and anti-duplication

* **Server-authoritative by construction:** the client sends keys, and the
  server draws the screen. There is nothing to forge.
* **Anti-duplication:** an item or gold movement between a character and the
  database (bank, vault, auction, claim, trade) is **one SQLite transaction
  that also writes the character's save**. A crash either keeps both
  changes or loses both, so no item can be left in both places. The world
  lock means no two such moves interleave. Conditional updates
  (`... WHERE state = 0`, `... AND high_bid = ?`) with their row counts
  checked settle any races.
* **No save-scumming:** saves are the server's, and death deletes the
  character (Umoria's permadeath), recorded in `graveyard`.
* Wizard mode and debug commands need the admin role, and are logged.
* The `ledger` table records every gold or item movement (who, what, how much,
  why).

### 2.15 Administration and moderation

* **Roles:** 0 player, 1 moderator, 2 admin (`account.role`).
* **Moderator:** `/who`, `/kick`, `/mute name minutes`, `/goto name`.
* **Admin:** `/ban name days reason`, `/unban`, `/role name n`,
  `/broadcast text`, `/summon name`, `/inspect name`, `/save`,
  `/shutdown minutes`, `/resetlevel depth`, Umoria's wizard mode (`^W`).
* **DB:** `audit(id, at, actor, action, target, detail)`.
* **CLI:** `mumoria-server --create-admin NAME` sets up the first admin.

### 2.16 Boss encounters

* **Rule:** on every `MU_BOSS_INTERVAL` (2) levels, a boss is placed when the
  level is made, and comes back `MU_BOSS_RESPAWN_HOURS` after it dies.
* **The boss:** a creature from depth to depth + 5 (never a town monster or
  the Balrog), with `3 + depth / 10` times its hit points and +1 speed below
  1000 ft. Entering the level says "You sense a powerful presence..."
* **Loot, persistently better:** `2 + depth / 10` items, rolled at
  `depth + 10 + 2 * tier`. The tier is the character's boss kills, capped at
  10 and kept in `character.boss_kills`. Plus gold piles. Each boss a
  character kills makes the next one's drops better, and that progress is
  saved.
* **Files:** `src/mumoria/boss.cpp`, plus the hooks in `monster.cpp`
  (`monsterTakeHit`) and `world.cpp` (level creation).
* **DB:** `level.boss_dead_at`, `character.boss_kills`.

## 3. Menus: drawn the way Umoria draws

There are no new windows. Bank, Guild Hall, Auction House and Trade are drawn
exactly like Umoria's stores (`store.cpp`): a full-screen page with lettered
lines `a)`, `b)`..., the prompt on the message line, `ESC` to leave, and then
`drawCavePanel()`. The inventory, spells and character sheet are Umoria's own
overlays, untouched. The town gets three more buildings, `7` Bank, `8` Guild
Hall and `9` Auction House, as store doors with sub-categories 107 to 109.
They stand in the free margins left and right of the six stores. A shop's
doorway can hold more than one player, because whoever is inside a shop is
standing on its door.

## 4. Building and running

    cmake -S . -B build && cmake --build build
    ./build/umoria/mumoria-server --create-admin YourName
    ./build/umoria/mumoria-server --port 4000 --db mumoria.db
    telnet localhost 4000

Settings are in `mumoria.conf` (key = value). `mumoria.conf.example` lists
them: `port`, `db`, `guild_cost`, `irc_host`, `irc_port`, `irc_channel`,
`irc_nick`, `aol_token_secret`.

The build needs only the SQLite runtime library (`libsqlite3.so.0`), not its
development header (`src/mumoria/sqlite3_min.h`). Without the ncurses
development files, CMake skips the single-player `umoria` target with a
warning, and the server still builds.

## 5. What has been tested

Scripted telnet bots (a screen emulator over the real protocol) have run all
of the following against the built server:

* account creation and login, the character menu, Umoria's own character
  creation
* the shared town (each player sees the other's `@` move), say and world
  chat, parties, `/who`
* the Bank: deposit, withdraw, store an item
* the Guild Hall: found a guild, guild gold, then invite, join, guild chat
  and promote
* the Auction House: list, buy out, bid, settle when the time is up, collect
  the claim, with the seller's bank credited and every step in `ledger`
* trading between two players
* `/broadcast` and `/mute`
* admin wizard mode
* stairs, then reconnecting and continuing on the same persistent level
* a boss announced on 100 ft
* a 6-player stress run in the town and on 50 ft (moves, rests, chat, a
  drop and reconnect), with clean saving at shutdown

## 6. Not done yet

* **Boss kill and loot** is written (`boss.cpp`), but no test has killed a
  boss yet.
* The **IRC bridge** has not been run against a live IRC server.
* **PvP** is off. The python port's public-dungeon PvP can follow later.
* **Party gold** is not shared. The finder keeps it.
* No **colour client**. Umoria has no colours.
* **The AOL launch itself:** the token login is built. The P3OL side, a
  terminal relay to this server, is not.
* Umoria's look command keeps a little state in statics
  (`dungeon_los.cpp`), and so does its haggling (`store.cpp`). Two players
  doing those at the same moment can disturb each other's display, but not
  the game state.
