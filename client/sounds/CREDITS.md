# Where the sounds come from

Every sound here is either synthesised by `make_sounds.py` (written for
Mumoria) or taken from a pack released under **CC0 1.0** (public domain,
https://creativecommons.org/publicdomain/zero/1.0/): free to use, change and
share, with no credit required. They are credited anyway, with thanks. Each
pack's licence was checked on its page on 2026-10-24.

| Pack | By | Page |
|------|----|------|
| RPG Audio | Kenney (kenney.nl) | https://kenney.nl/assets/rpg-audio |
| Impact Sounds | Kenney (kenney.nl) | https://kenney.nl/assets/impact-sounds |
| Interface Sounds | Kenney (kenney.nl) | https://kenney.nl/assets/interface-sounds |
| 80 CC0 RPG SFX | rubberduck | https://opengameart.org/content/80-cc0-rpg-sfx |
| 80 CC0 creature SFX | rubberduck | https://opengameart.org/content/80-cc0-creature-sfx |
| 80 CC0 creature SFX #2 | rubberduck | https://opengameart.org/content/80-cc0-creture-sfx-2 |
| Hurt / Death sound effect for character | (OpenGameArt) | https://opengameart.org/content/hurt-death-sound-effect-for-character |
| Monster Sound Pack | (OpenGameArt) | https://opengameart.org/content/monster-sound-pack-volume-1 |
| Magic Spell SFX | (OpenGameArt) | https://opengameart.org/content/magic-spell-sfx |
| Chunky Explosion | (OpenGameArt) | https://opengameart.org/content/chunky-explosion |
| CC0 Deep Monster Roar | (OpenGameArt) | https://opengameart.org/content/cc0-deep-monster-roar |
| RPG Sound Pack | artisticdude | https://opengameart.org/content/rpg-sound-pack |
| Medieval sound effects - Weapon Textures | Ben Jaszczak & Brian Nelson | https://opengameart.org/content/medieval-sound-effects-weapon-textures |
| Male Grunt/Yelling sounds | HaelDB | https://opengameart.org/content/male-gruntyelling-sounds |
| Dying Voices - Female - RPG | Galacti-Chron (voice: Sky Rae) | https://opengameart.org/node/99640 |
| Female screams (files 2-4) | congusbongus, from Freesound: tcrocker68, pushkin, Archeos | https://opengameart.org/content/female-screams |
| Battle Sound Effects | artisticdude | https://opengameart.org/content/battle-sound-effects |

The last block (checked 2026-10-26) is for the creatures', the players' and
the weapons' sounds (`fetch_voices.py`). Three of those packs offer more
than one licence and CC0 among them, and are used under CC0: Male
Grunt/Yelling (CC0 or OGA-BY), Dying Voices (CC0 or CC-BY) and Battle
Sound Effects (CC0 among five). The female screams pack lists each file's
source in its `license.txt`: one recording is CC-BY, the rest CC0, and
since that list is numbered from 0 and the files from 1 it is not certain
which file the CC-BY one is; files 2-4 are CC0 either way, and only they
are used (never `1.ogg`).

Which file came from which pack: `candidates/README.md` and
`candidates/VOICES.md`. The files were trimmed, faded, made mono and evened
out in loudness (`fetch_candidates.py`), and some moved in pitch
(`fetch_voices.py`).

Packs that were looked at and left out because they are not CC0: "Big
Explosion" (CC-BY 3.0) and "Spell Sounds Starter Pack" (CC-BY-SA / GPL).
Anything added later must be CC0 too, or be credited as its licence asks.
