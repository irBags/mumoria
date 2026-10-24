# Mumoria's sounds

`mumoria-client` plays these; a plain telnet never hears anything. The build
copies them beside the client (`build/umoria/sounds/`).

The server sends a sound's name and a volume (1-100: full beside you,
fainter with distance, muffled by walls, out of hearing past 20 squares).
The client plays `<name>.wav`; a missing file is silence. So to change a
sound, drop in a `.wav` of the same name; to silence one, delete it.

| Name         | When                                                    |
|--------------|---------------------------------------------------------|
| `hit`        | a blow, a missile or a bolt lands                       |
| `crit`       | a critical hit                                          |
| `miss`       | a blow or missile misses, or a spell is shrugged off   |
| `kill`       | a monster dies                                          |
| `hurt`       | a monster's blow lands on a player                      |
| `die`        | a player dies                                           |
| `roar`       | a monster wakes up, or draws breath to breathe          |
| `cast`       | a spell is cast (by a player or a monster)              |
| `fizzle`     | a spell fails                                           |
| `blast`      | a ball spell or a breath bursts                         |
| `heal`       | healing                                                 |
| `loot`       | a Rare or better item appears                           |
| `door_open`  | a door is opened                                        |
| `door_close` | a door is closed                                        |
| `door_bash`  | a door is smashed open                                  |
| `your_turn`  | battle turns: it is your turn (only you hear it)        |

The ones in use come from free CC0 packs (Kenney, OpenGameArt; see
`CREDITS.md`), except `fizzle`, which is synthesised. `candidates/<name>/`
holds the other choices for each sound, the synthesised one
(`0-synth.wav`) among them; `candidates/README.md` says where each came
from. To change a sound, copy a candidate over `<name>.wav` and rebuild.

* `python3 fetch_candidates.py` downloads the packs again and remakes the
  candidates (it needs ffmpeg); `--use` also puts each first pick in use.
* `python3 make_sounds.py` remakes the synthesised ones.

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
(0-65536) and `%a` for afplay (0.00-1.00). `MUMORIA_SOUNDS=/some/dir` plays
another folder's sounds. `mumoria-client -q` (or `MUMORIA_SOUND=off`) tells
the server not to send any.

## Adding a sound

Call `muSound(coord, "name")` in the server (`src/mumoria/hooks.h`), or
`a.sound(at_ms, coord, "name")` inside an animation in `fx.cpp` to time it
with the frames. Names are lower-case letters, digits and `_`. Then add
`name.wav` here and a line to the table.
