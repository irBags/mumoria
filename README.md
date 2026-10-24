# Mumoria: multiplayer Umoria

The Dungeons of Moria is a single-player dungeon adventure written by Robert
Alan Koeneke, first released in 1983. It was written in VMS Pascal, then
ported to C by James E. Wilson in 1988 and released as Umoria.

Mumoria turns Umoria 5.7 into an online world: one persistent town and
dungeon shared by every player, played over telnet. The game underneath is
still Umoria (its monsters, items, spells, combat and permadeath), with an
MMORPG built around it: parties and guilds, banks, an auction house, trade,
chat, bosses, raids, rare loot, and Lua scripting for new encounters and
items.

2026, Mike Killingbeck and Claude AI.

## What it adds

* **A shared, persistent world.** Everyone on 250 ft is on the same level,
  and it stays as it was left. The town's people move for everyone at once;
  in the dungeon, monsters move on each player's turn and everyone who can
  see them sees it. Characters, levels and the world live in one SQLite
  database.
* **Accounts.** Log in with a name and password; one account holds several
  characters, plus its bank and guild, which outlive a character's death.
  AOL members of the P3OL server can be handed in with a token.
* **Parties** of up to five: shared experience and gold, loot rolled for
  (d100), arriving together below the town, and optional battle turns.
* **Guilds** with ranks and permissions, a guild bank, a message of the
  day and a shield colour on the map.
* **The town's buildings 7, 8 and 9:** a personal Bank, the Guild Hall and
  a global Auction House (bids, buyouts, 8 to 48 hours). The six stores never
  keep anyone waiting: each player has their own stock, and a shared supply
  can run out for everyone.
* **Chat:** the level, world, guild, party and private messages, in a chat
  pane under the map; world chat can be relayed to an IRC channel.
* **Trade** between two players, both confirming, in one database
  transaction.
* **Player against player**, only between characters that both chose it,
  never in the town.
* **Bosses** on every second level, coming back two hours after they die,
  with better loot for each one a character has slain.
* **Raids:** a raid gate (`0`) on every fifth level opens an instance made
  for the party: packs of themed monsters, two captains, and a lord who calls
  reinforcements as he weakens. Seven raids from the Orc Warrens (250 ft) to
  the Dragons' Deep.
* **Loot rarity:** gear is Common, Uncommon, Rare, Epic or Legendary, with
  bigger bonuses, powers and names the rarer it is, a level to wear it, and
  bad-luck protection on boss and raid rolls. Rare items glow on the map;
  great finds are told to the server. `/loot` and `/legends` keep score.
* **Animations** for spells, missiles and blows, for players and monsters.
* **Lua 5.4 scripting:** new items, boss fights, events, timers, portals
  and commands, loaded without a restart. The Demon Breach, a raid that
  tears open in the town, is one script (`scripts/examples/40_demon_breach.lua`).
* **Admin tools:** roles, bans, mutes, an audit log, `/makeitem` (an
  in-game item maker for the drop tables), `/makeloot`, `/spawnboss`,
  `/raid`, `/reload`, and Umoria's wizard mode for admins.
* **Anti-cheat by design:** the client sends only keys and the server draws
  everything; item and gold moves are single transactions, so nothing can be
  duplicated.

## Building

Linux (or WSL) with CMake, a C++ compiler and the SQLite runtime library:

    cmake -S . -B build && cmake --build build

That builds `build/umoria/mumoria-server`, `build/umoria/mumoria-client`
and, when ncurses is found, the single-player `build/umoria/umoria`. Lua comes
with the source (`src/lua`). Without root, ncurses can be built into
`~/local`, where CMake looks for it by itself (see `docs/MUMORIA_DESIGN.md`,
section 4).

Or `./build.sh`, which does that and also builds the client for Windows
(`build/windows/mumoria-client.exe`, cross-compiled with Zig;
`./build.sh get-zig` fetches Zig without root). `./build.sh dist` makes the
packages to hand out: `dist/mumoria-linux-x86_64.tar.gz` and
`dist/mumoria-client-windows-x86_64.zip`. The server runs on Linux (on
Windows, in WSL).

## Running a server

    cd build/umoria
    ./mumoria-server --create-admin YourName     # once: the first admin
    ./mumoria-server --port 4000 --db mumoria.db

Settings are in `mumoria.conf` next to the server (`mumoria.conf.example`
lists them): the port, the database, the guild cost, shared gold, the
stores' supply, the IRC relay, the AOL token secret and the scripts
directory. Scripts are loaded from `scripts/` where the server runs (copy the
repository's `scripts/` folder there).

## Playing

    telnet yourserver 4000

or `mumoria-client yourserver 4000` (Linux, WSL, macOS, or
`mumoria-client.exe` on Windows), which sets up the terminal and plays the
game's sounds (`-q` for none; Ctrl-] quits; see `client/sounds/README.md`).
What goes wrong for it is told on leaving and kept in `mumoria-client.log`
beside it. Type `new` at the account prompt to make an account, then a
character. The keys are Umoria's (`?` lists them). The multiplayer ones:

* `'` then text: talk; `'` then a `/` command, such as `/help`, `/who`,
  `/invite`, `/guild`, `/trade`, `/loot`.
* `"` then a direction: attack a player (both with PvP on).

Open the terminal 120 columns wide (or more) and the chat sits in a box
beside the map; narrower, it is under it. `/panel` changes the layout and the
box's outline.

Death is permanent, as in Umoria. Your bank and guild belong to the account.

## Where things are

| Path | What |
|---|---|
| `src/` | the Umoria 5.7 engine, with `#ifdef MUMORIA` hooks |
| `src/mumoria/` | the server: sessions, world, database, social, stores, bosses, raids, loot, items, scripts |
| `src/lua/` | Lua 5.4.9, vendored |
| `scripts/` | Lua scripts; `scripts/README.md` is the guide, `scripts/examples/` the examples |
| `client/` | the small terminal client |
| `data/` | the help, welcome, version and death screens |
| `docs/MUMORIA_DESIGN.md` | the design: every feature, its files, its tables, what was tested |
| `historical/` | Umoria's own history and documents |

## Credits and license

Moria by Robert Alan Koeneke and the VMS Moria team; Umoria by James E.
Wilson, David Grabiner, Michael R. Cook and many others (see `AUTHORS`).
Mumoria by Mike Killingbeck and Claude AI.

Mumoria, like Umoria, is free software under the GNU General Public License
v3.0 (`LICENSE`), and comes with ABSOLUTELY NO WARRANTY. Lua is MIT licensed
(`src/lua/README.md`).
