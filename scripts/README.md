# Mumoria scripts

Plugins for the server, in [Lua 5.4](https://www.lua.org/manual/5.4/). Every
`*.lua` file in the scripts directory (`scripts_dir` in `mumoria.conf`,
default `scripts` next to where the server runs) is loaded at start, in name
order. An admin's `/reload` loads them all again without a restart, and
`/scripts` lists what loaded and the last errors.

`examples/` holds scripts to copy from (they are not loaded where they are):

* `10_items.lua`: custom items, from a Legendary sword to a Rare ring.
* `20_orc_chieftain.lua`: a boss fight with guards, phases, healing and loot.
* `30_bounties.lua`: kill counting, rewards, a `/bounty` command.
* `40_demon_breach.lua`: a raid in the town: a portal, waves of named
  demons, two Pit Fiends, a Demon Lord with phases, rewards, a cool-down.
* `garbage_man.lua`: an NPC who sweeps up the town's litter every half hour
  and cannot be killed by blows.
* `50_dungeon_warden.lua` with `51_rat_sewer.lua`, `52_bandit_fort.lua`,
  `53_lich_crypt.lua` and `54_dragon_forge.lua`: four dungeons drawn by
  their scripts, and the quest giver in the town who sends parties in.
* Raids (`mu.raid`), each with its gate `0` in the dungeon:
  `60_kobold_den.lua` (levels 1-3, gate at 50 ft), `61_old_crypt.lua`
  (2-5, 100 ft, a drawn crypt), `62_snake_pit.lua` (3-5, 150 ft); and
  deeper ones on several floors: `70_orc_hold.lua` (8-14, 350 ft, three
  floors), `71_sunken_temple.lua` (15-22, 600 ft, two),
  `72_giant_stair.lua` (18-28, 800 ft, four).
* `80_inn.lua`: the Rusty Tankard, an inn in the town to hang out in,
  drink and sleep (a shared, peaceful dungeon with furniture and NPCs).

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
mu.on("hurt", function(player, monster) end) -- a script's monster (spawned with boss= or tag=) was hurt and lives
mu.on("portal", function(player, key) end) -- a player stepped on a script's portal
mu.on("talk", function(player, monster) end) -- a player walked into an NPC (mu.npc)
mu.every(seconds, function(player, level) end, depth) -- a timer; depth (optional) limits it to one level
mu.command("name", function(player, args) end, "help") -- a /name chat command (if the game has none)
```

Timers run at the end of some player's turn, at most once a second a level,
so on a level where nobody is acting time stands still (as it does for its
monsters). `mu.every(1, fn, 0)` is a clock for the town.

A **player** is `{name, level, class, race, x, y, depth, hp, max_hp, mana,
max_mana, gold, admin, in_party}`. A **monster** is `{id, name, creature, letter, level, x,
y, hp, max_hp, awake, phase, kind, boss, tag}`: `name` is the name it goes
by (its own, if a script gave it one), `creature` what it really is, `kind`
is `boss`, `captain` or `lord` for the game's own bosses, `boss` a script
boss's key, `tag` a script's tag, `phase` the script's count (0 until set).
A **level** is `{depth, players, raid}`, and in a script's dungeon
`{depth, players, dungeon, id, party, floor, run}`. All are copies: read them, then act
through `mu`.

## Doing things

These work inside an event or command (on the level of the player in it):

| Call | What it does |
|---|---|
| `mu.tell(player, text)` | a line in that player's chat |
| `mu.say_level(text)` / `mu.say_world(text)` | to the level, or to everyone |
| `mu.spawn{creature=, name=, at= / near= / far=true, count=, hp=, speed=, awake=, boss= / tag=}` | monsters: a creature's name ("Black Orc") or a letter ("o"); `at` on that square (asleep unless `awake`), `near` around it; `name` is what the game calls them ("Imp"); `hp` multiplies, `speed` adds; `boss=` makes them a boss (a boss's loot), `tag=` only marks them; returns the monster, or a list when `count` is given |
| `mu.npc(tag, letter)` | monsters with this tag are NPCs: they never move or fight, take no harm, and walking into one is the `talk` event; `letter` (optional) is what they are drawn as |
| `mu.letter(tag, letter)` | monsters with this tag are drawn as `letter` (one printable character) rather than their creature's |
| `mu.monsters()` / `mu.tagged(tag)` | every monster on the level / those with that tag or boss key |
| `mu.find_boss(key)` | the first with that key, if alive here |
| `mu.set_phase(monster, n)` / `mu.heal(monster, n)` | its phase; hit points (negative takes them) |
| `mu.hurt(monster, n)` | as if the player struck it: true if it died (the kill and its events are theirs) |
| `mu.despawn(monster)` | gone, no loot, no kill |
| `mu.portal{key=, at=, glyph=, colour=, gate=}` | a portal (`0`, red by default; an xterm colour) at a place, or somewhere open away from the doors; its place, or nil. `gate = true`: the gate of the dungeon or raid `key` (at most 11 letters), which shows it and lets a party in |
| `mu.find_portal(key)` / `mu.close_portal(key)` | where it is / gone |
| `mu.drop(what, place)` | a custom item by key, or a random item of a rarity ("epic") |
| `mu.give(player, what)` | a custom item by key, or a random one of a rarity, into a pack (false if full) |
| `mu.teleport(player or monster, place)` | moves a player on this level (onto that square if it is free, else near it), or a monster onto it (false if it is taken) |
| `mu.pay(player, gold)` | gold into their purse (negative takes it) |
| `mu.restore(player, {hp=, mana=, food=, cure=})` | hit points and mana back (a number, or `"full"`); `food = "full"` feeds them (never bloated); `cure = true` ends poison, confusion, blindness and fear |
| `mu.buff(player, what, turns)` | a timed effect, as a potion's: `hero`, `bless`, `speed`, `slow`, `confused`, `poisoned`, `afraid`, `see_invisible`, `infravision`, `resist_fire`, `resist_cold` |
| `mu.creatures{letters=, min=, max=}` | the names of the creatures drawn as those letters (any, if none given), of those levels |
| `mu.spot{from=, near=, far=}` / `mu.spot{from=, farthest=true}` | a free open square (no wall, monster, item or player) that many squares from a place (the event's player if none), or the farthest found; nil if none |
| `mu.decor(place, glyph, name, colour)` | furniture there: it blocks the way, cannot be dug, and walking into it is a dungeon's `bump` (`name`: at most 12 letters) |
| `mu.level()` | the event's level, as events give it |
| `mu.items()` / `mu.take(place)` | the items on the floor, `{x, y, name, age}` (`age`: seconds it has lain there, town only, else 0) / the one there is gone: its name, or nil |
| `mu.hold(tag)` | monsters with this tag get no turns of their own: they neither move nor attack |
| `mu.walk(monster, place, steps)` | up to `steps` (1-10) squares along the shortest way there, waiting if someone is in it; where it is now, or nil if it cannot get there |
| `mu.town_expiry(on)` | `false`: the game stops clearing items left in the town for 30 minutes (a script does it instead); back on at `/reload` |
| `mu.glow(place, tier)` | the loot glow animation |
| `mu.players()` / `mu.player(name)` | players on this level / anyone online |
| `mu.random(n)`, `mu.now()`, `mu.depth()` | 1..n; the time in seconds; this level |
| `mu.set(key, value)` / `mu.get(key)` | kept in the database across restarts (`get` gives a string or nil) |
| `mu.log(text)`, `print(...)` | to the server log |

Monsters a script tags (boss= or tag=) count apart from the town's people:
the town never sends them away. Their names, tags and keys are numbered
for good in the database, so a saved level keeps them after a restart.

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

## Dungeons

`mu.dungeon{}` is a whole dungeon: its map, what is in it and what happens.
Each party (or lone player) that goes in gets a copy of its own, gone a
minute after the last one leaves. See `examples/51_rat_sewer.lua`.

```lua
mu.dungeon{
  key = "rat_sewer", name = "the Rat-King's Sewer", label = "Rat-King's Sewer", -- label: the depth line
  depth = 6,            -- the depth it plays as (random items, a letter's monsters)
  level = {5, 10},      -- the character levels it suits (the Warden refuses below the first)
  about = "vermin in the dark...",
  map = [[ ... ]],      -- rows of text, at most 64 by 196
  legend = {            -- what the map's letters are
    G = "#",                                  -- drawn as this tile (else as the floor around it)
    r = {spawn = {creature = "Giant White Rat"}}, -- a monster there (asleep)
    z = {item = "uncommon"},                  -- an item there
  },
  build = function(d) end,           -- the copy is drawn: place its boss, its loot
  enter = function(p, d) end,        -- a player arrives
  step = function(p, d, letter, place) end, -- a player stepped onto a letter's square
  bump = function(p, d, letter, place) end, -- walked into a letter's decor (return false: not handled)
  talk = function(p, m, d) end,      -- walked into an NPC here
  hurt = function(p, m, d) end,      -- a tagged monster here was hurt
  kill = function(p, m, d) end,      -- a monster here died
  tick = function(p, d) end,         -- each second (`every =` seconds) while someone acts
}
```

Map tiles: `#` and space rock, `=` rock nothing can dig, `%` magma, `.` lit
floor, `,` dark floor, `+` a door, `'` an open door, `&` a locked door, `$` a
secret door, `:` rubble, `^` a trap, `<` the way out (on a deeper floor, the
stairs up), `>` the stairs down, `@` where players arrive. Letters and
digits are lit floor (dark beside dark floor) unless the legend says
otherwise, and are places the script can use.

A legend letter can be **decor**: `t = {decor = "=", name = "a table",
colour = 137}` draws furniture (that glyph, that xterm colour) which blocks
the way and cannot be dug; walking into it fires `bump` (else its name is
told: "There is a table in the way.").

**Floors.** `floors = {{...}, {...}}` makes a dungeon several floors deep,
each `{name=, label=, depth=, map=, legend=, build=}`, or `generate = true`
for a cave the game makes at that depth (without its stairs or monsters: a
way in, and stairs down as far from it as can be found, unless it is the
last floor). Stepping on `>` or `<` asks to go down or up. The floors a
party has open stay open while any of them is on any floor, so the way back
up leads where they came from; a member who comes in later starts on the
first floor. The handlers serve every floor: `d.floor` says which, and
`d.flags` is one table for all of a party's floors.

`shared = true` makes one copy for everyone (an inn) rather than one a
party; `safe = true` keeps players from fighting there; `hidden = true` keeps
it out of `mu.dungeons()`.

`d` is the copy's own table: `d.key`, `d.id`, `d.party` (how many it was
made for), `d.places` (`d.places.K[1]` is the first `K`), and `d.flags`
for the script's own notes. Its handlers run only for that dungeon. Plain
`mu.on` handlers run inside dungeons too; `mu.every` timers do not.

| Call | What it does |
|---|---|
| `mu.dungeons()` | every dungeon: `{key, name, level, about, depth, floors, kind, gate}` (`kind`: `"dungeon"` or `"raid"`) |
| `mu.here()` | in a dungeon, its floor's `d` (for commands, which are not given it); else nil |
| `mu.enter_dungeon(player, key)` | the player in the event goes in (to their party's copy if one is open) |
| `mu.leave_dungeon(player)` | out to where they came in from; false if not in one |
| `mu.tile(place, tile)` | one square drawn again (a gate opens); false where someone stands in a wall's way |
| `mu.phases(m, {{at = 66, run = fn(m)}, ...})` | in a boss's hurt handler: each phase it has fallen to runs once |
| `mu.reward_once(player, what, hours)` | true (and noted) once every `hours` (12) per character |
| `mu.reward_party{key=, title=, tier=, gold=, items={{key=, name=, one_in=}}}` | everyone on the level has their reward once every 12 hours |

A copy's `d` is lost if the scripts are reloaded while it is open.

`/escape` (in the prelude, so always there) leaves any dungeon or raid for
where the player came in from.

## Raids

`mu.raid{}` is a dungeon filled the way the game's own raids are: sleeping
packs all through it but not by the way in, captains with their guards, and
a lord on the last floor who calls its guard at three quarters, half and a
quarter of its hit points; all of it grows with the party that goes in. Its
gate (`gate =` a depth, 0 the town) stands somewhere on that level and
shows the raid, the party and the reward; the Dungeon Warden lists raids
too. See `examples/60_kobold_den.lua` (one generated floor) and
`examples/70_orc_hold.lua` (two generated floors and a drawn hall).

```lua
mu.raid{
  key = "orc_hold",              -- at most 11 letters (it is the gate's key)
  name = "the Orc Stronghold", label = "Orc Stronghold",
  level = {8, 14},               -- the gate refuses below the first
  about = "...", gate = 7, colour = 166,
  floors = {
    {name = "the Outer Caves", depth = 7, generate = true,
     mobs = {"Orc", "Orc Shaman"},           -- or {letters = "o", min = 5, max = 12}
     packs = 8, pack = {3, 5},               -- packs (a third more per member after the first) of 3 to 5
     captains = 1, captain = {creature = "Orc Warrior", name = "Orc Sergeant", hp = 3}},
    {name = "the War Chief's Hall", depth = 11, map = HALL, legend = {...}},
  },
  lord = {creature = "Black Orc", name = "Orc War Chief", hp = 6, speed = 1,
          at = "L",                         -- a map letter; else the farthest square from the way in
          guard = {"Orc Warrior", "Black Orc"},
          phases = nil},                    -- or a mu.phases list, instead of its guard calls
  loot = {tier = "rare", count = 3},        -- the lord drops these, one more per member
  captain_loot = {tier = "uncommon", count = 2},
  reward = {tier = "rare", gold = 400, items = {{key = "...", name = "...", one_in = 4}}, hours = 12},
  enter = fn, step = fn, bump = fn, talk = fn, hurt = fn, kill = fn, tick = fn, build = fn,
}
```

`hp` multiplies the creature's hit points; every monster has a fifth more
per member after the first, the lord half its `hp` more. The lord's and the
captains' loot rolls as a boss's. The lord's death drops its loot, gives
everyone on that floor their reward (once every 12 hours each, as
`mu.reward_party`) and tells the world.
