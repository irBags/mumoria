# Mumoria's sounds

`mumoria-client` plays these; a plain telnet never hears anything. The build
copies them beside the client (`build/umoria/sounds/`).

The server sends a sound's name, a volume (1-100: full beside you, fainter
with distance, muffled by walls, out of hearing past 20 squares) and a
pitch. The client plays `<name>.wav`; a missing file is silence. So to
change a sound, drop in a `.wav` of the same name; to silence one, delete it.

**Variants.** Beside `<name>.wav` there may be `<name>2.wav` up to
`<name>9.wav`: the client plays one of them at random each time. Add one to
give a sound more variety.

**Falling back.** A name with no file of its own plays its last part
instead: `orc_hurt`, else `hurt`; `thrown_hit`, else `hit`. So a family or a
weapon needs only the files that should differ.

**Pitch.** Voices come with a pitch (a percentage, 100 as recorded). Each
creature has its own: bigger and higher-level ones deeper, each a little
apart from the rest of its family. A player's goes by their race (a
Half-Troll deeper, a Halfling higher) and name.

### Whose sounds

| Name | When |
|------|------|
| `<voice>_attack` | it attacks (a player swings, a monster strikes) |
| `<voice>_hurt` | it is hurt and lives |
| `<voice>_crit` | it takes a critical blow |
| `<voice>_die` | it dies |
| `<voice>_roar` | a monster wakes, or draws breath to breathe (else `roar`) |

A player's voice is `male` or `female`: the man's lower, the woman's
higher. A monster's is its family, by its letter (`src/mumoria/fx.cpp`
`creature_voice`): `human` (p), `goblin` (kobolds, yeeks, the hobgoblin),
`orc`, `ogre` (ogres, yetis, umber hulks), `troll`, `giant`, `dragon` (d,
D), `zombie` (zombies, mummies), `skeleton`, `ghost` (ghosts, wights,
wraiths, liches), `vampire`, `insect` (ants, beetles, centipedes, ticks,
lice, flies, ant lions, scorpions), `bat`, `rodent`, `snake` (snakes,
nagas), `canine`, `slime` (jellies, oozes, icky things, worms, molds,
mushrooms, the cube), `eye`, `frog`, `golem` (golems, the xorn), `spirit`
(elementals), `demon` (quasits, quylthulgs, the Balrog), `harpy`, `coins`
(creeping coins) and `beast` (anything else).

### Weapons

| Name | When |
|------|------|
| `<weapon>_swing` | a melee blow, hit or miss (else `swing`) |
| `<weapon>_hit` / `<weapon>_crit` | it lands; a critical (else `hit`, `crit`) |
| `bow_shoot`, `crossbow_shoot`, `sling_shoot` | a missile fired from its launcher |
| `throw` | anything thrown by hand |
| `arrow_`, `bolt_`, `stone_`, `thrown_` + `hit` / `miss` | the missile lands, or flies by |
| `blow_bite`, `_claw`, `_sting`, `_spit`, `_slime`, `_crush`, `_touch`, `_hit` | a monster's blow lands |

Weapons are `sword`, `dagger`, `axe`, `mace` (maces, hammers, flails,
clubs), `spear` (spears, pikes, lances, javelins), `pole` (halberds,
glaives and the like), `whip`, `pick` (picks and shovels) and `fist`.

### The rest

| Name         | When                                                    |
|--------------|---------------------------------------------------------|
| `hit`, `crit`, `miss` | a spell's bolt lands; a blow when nothing more fitting is there |
| `cast`       | a spell is cast (by a player or a monster)              |
| `fizzle`     | a spell fails                                           |
| `blast`      | a ball spell or a breath bursts                         |
| `heal`       | healing                                                 |
| `loot`       | a Rare or better item appears                           |
| `door_open`  | a door is opened                                        |
| `door_close` | a door is closed                                        |
| `door_bash`  | a door is smashed open                                  |
| `your_turn`  | battle turns: it is your turn (only you hear it)        |
| `hurt`, `die`, `roar`, `attack`, `swing` | when a voice or a weapon has none of its own |

(`kill` is no longer sent: each monster dies in its own voice.)

The sounds come from free CC0 packs (Kenney, OpenGameArt; see
`CREDITS.md`), except `fizzle`, which is synthesised.

* `python3 fetch_voices.py` makes the voices' and weapons' sounds and lists
  where each came from in `candidates/VOICES.md`. Its table says which
  recording (and which pitch) each name is made from; change it and run it
  again (it needs ffmpeg, with rubberband, and 7z).
* `python3 fetch_candidates.py` remakes `candidates/<name>/` for the rest:
  other choices for each sound, the synthesised one (`0-synth.wav`) among
  them, listed in `candidates/README.md`. To change one, copy a candidate
  over `<name>.wav`. `--use` puts each first pick in use.
* `python3 make_sounds.py` remakes the synthesised ones.

New `.wav` files are seen by the build after `cmake .` is run again in the
build folder (it lists them when it is set up).

## Playing them

On Windows, `mumoria-client.exe` plays them itself (all read at the start,
mixed into one stream, so they overlap). Any 16-bit PCM `.wav` will do.

On Linux, WSL and macOS the client runs a command for each sound:

* Linux and WSL: `paplay --volume=%p %f` (PulseAudio; WSLg has it)
* macOS: `afplay -v %a %f`
* anything else: set `MUMORIA_PLAYER`, e.g. `MUMORIA_PLAYER='aplay -q %f'`

PulseAudio (WSLg's too) puts the sound card to sleep after a few quiet
seconds, and the next sound then waits while it wakes, heard late. So the
client holds a silent stream open (`pacat`, gone when the client ends) to
keep it awake; `MUMORIA_KEEPALIVE=off` leaves that out. It also reads every
sound once at start, so a first play is not slowed by the disk.

In the command `%f` is the file, `%v` the volume 0-100, `%p` the volume for paplay
(0-65536), `%a` for afplay (0.00-1.00) and `%r` the rate to play the file
at for its pitch. paplay takes a file's rate from the file, so a pitched
sound goes to it as raw samples at the new rate; afplay plays every sound
at its own pitch. Windows pitches them in its own mixer. `MUMORIA_SOUNDS=/some/dir` plays
another folder's sounds. `mumoria-client -q` (or `MUMORIA_SOUND=off`) tells
the server not to send any.

## Adding a sound

Call `muSound(coord, "name")` in the server (`src/mumoria/hooks.h`), or
`a.sound(at_ms, coord, "name")` inside an animation in `fx.cpp` to time it
with the frames; `muVoice(coord, "event")` is the cry of whoever is there.
Names are lower-case letters, digits and `_`, at most 15 of them. Then add
`name.wav` here (or a line to `fetch_voices.py`) and a line to the table.
