# Mumoria scripts

Plugins for the server, in [Lua 5.4](https://www.lua.org/manual/5.4/). Every
`*.lua` file in the scripts directory (`scripts_dir` in `mumoria.conf`,
default `scripts` next to where the server runs) is loaded at start, in name
order. An admin's `/reload` loads them all again without a restart, and
`/scripts` lists what loaded and the last errors.

`examples/` holds three to copy from (they are not loaded where they are):

* `10_items.lua`: custom items, from a Legendary sword to a Rare ring.
* `20_orc_chieftain.lua`: a boss fight with guards, phases, healing and loot.
* `30_bounties.lua`: kill counting, rewards, a `/bounty` command.

## What a script may do

Scripts run while every player waits, so they are kept on a short leash:
there is no `io`, `os`, `require`, `load` or `dofile`; a script may use 64 MB
at most; and one call is stopped after about 5 million instructions. An
error is written to the server log and shown to the admins online; the
script's other handlers and every other script carry on.

Places are map coordinates: any table with `x` and `y` will do, so a player
or a monster can be passed where a place is wanted.

## Items

```lua
mu.item{
  key = "thunderfury",          -- unique; how scripts refer to it
  name = "Thunderfury",         -- shown as 'Thunderfury'; a name starting "of " reads "Longsword of ..."
  base = "Longsword",           -- an Umoria base item (/makeitem lists them by name)
  tier = "legendary",           -- common, uncommon, rare, epic, legendary (the level it needs follows)
  to_hit = 14, to_dam = 16, to_ac = 0,
  dice = "3d6",                 -- omit for the base item's
  powers = {"slay_evil", "flame_tongue", "free_action", "dex"},
  bonus = 2,                    -- the amount of its stats, speed, stealth, searching, infravision
  depth = {20, 127},            -- the levels it drops on (50 ft each)
  from = "lords",               -- anywhere, bosses (bosses and raids), lords (raid lords), never
  one_in = 40,                  -- 1 in this many gear drops there; 0 or never: only given
}
```

Powers: `str int wis dex con chr speed stealth searching infravision
slay_evil slay_undead slay_animal slay_dragon frost_brand flame_tongue
resist_fire resist_cold resist_acid resist_lightning free_action
see_invisible regeneration slow_digestion feather_fall`.

An item is stored in the database by its key, so items already found keep
their name when the script changes or goes. Changing a script changes the
items made from then on, not those already found. There are 184 names in
all for custom items (admins' and scripts' together), and a name once given
is kept. Admins can make items in the game too: `/makeitem`.

## Events

```lua
mu.on("enter", function(player, level) end) -- a player arrives on a level; level = {depth=, raid=}
mu.on("kill", function(player, monster) end) -- the player killed it (before its loot is shared)
mu.on("hurt", function(player, monster) end) -- a script's boss (spawned with boss=) was hurt and lives
mu.command("name", function(player, args) end, "help") -- a /name chat command (if the game has none)
```

A **player** is `{name, level, class, race, x, y, depth, hp, max_hp, gold,
admin, in_party}`. A **monster** is `{id, name, letter, level, x, y, hp,
max_hp, awake, phase, kind, boss}`: `kind` is `boss`, `captain` or `lord`
for the game's own bosses, `boss` is a script boss's key, `phase` its count
(0 until set). Both are copies: read them, then act through `mu`.

## Doing things

These work inside an event or command (on the level of the player in it):

| Call | What it does |
|---|---|
| `mu.tell(player, text)` | a line in that player's chat |
| `mu.say_level(text)` / `mu.say_world(text)` | to the level, or to everyone |
| `mu.spawn{creature=, near= / far=true, count=, hp=, speed=, awake=, boss=}` | monsters: a name ("Black Orc") or a letter ("o"); `hp` multiplies, `speed` adds; returns the monster, or a list when `count` is given |
| `mu.monsters()` | every monster on the level |
| `mu.find_boss(key)` | that script boss, if alive here |
| `mu.set_phase(monster, n)` / `mu.heal(monster, n)` | a boss's phase; hit points (negative hurts) |
| `mu.drop(what, place)` | a custom item by key, or a random item of a rarity ("epic") |
| `mu.give(player, key)` | a custom item into a pack (false if full) |
| `mu.teleport(player, place)` | moves a player on this level |
| `mu.glow(place, tier)` | the loot glow animation |
| `mu.players()` / `mu.player(name)` | players on this level / anyone online |
| `mu.random(n)`, `mu.now()`, `mu.depth()` | 1..n; the time in seconds; this level |
| `mu.set(key, value)` / `mu.get(key)` | kept in the database across restarts (`get` gives a string or nil) |
| `mu.log(text)`, `print(...)` | to the server log |

## Bosses

`mu.boss{}` is a whole fight in one table (see `examples/20_orc_chieftain.lua`):

```lua
mu.boss{
  key = "grashnak", depth = 6, creature = "Black Orc",
  hp = 10, speed = 1, respawn_hours = 6,
  announce = "War drums echo...",            -- told to each player arriving while it lives
  guards = {creature = "Orc Warrior", count = 4},
  phases = {{at = 70, say = "...", spawn = {creature = "Orc Warrior", count = 3}},
            {at = 35, say = "...", spawn = {creature = "Orc Shaman", count = 2}, heal = 60}},
  loot = {items = {{key = "chieftains_cleaver", one_in = 3}}, tier = "epic", count = 2},
  defeat = "{player} has slain Grashnak the Chieftain!",
}
```

It appears (asleep, far from the player) when someone arrives on its level
and it is due, carries its key, and is a boss for the loot: its drops are
Rare or better. In the game it has its creature's name. It comes back
`respawn_hours` after it dies. `mu.boss` is written in Lua over the calls
above, in `src/mumoria/script.cpp`'s prelude, if a fight needs more.
