#!/usr/bin/env python3
"""Downloads free (CC0) sound packs and makes candidates/<name>/ from them.

    python3 fetch_candidates.py [--use] [cache_dir]

Every pack used is CC0 (public domain): no credit needed, no conditions;
CREDITS.md names them anyway. Needs ffmpeg. The packs are kept in cache_dir
(default /tmp/mumoria-sfx), so a second run downloads nothing.

Each candidate is cut to its sound's longest length, faded out, made mono
22050 Hz 16-bit, and evened out in loudness. candidates/<name>/1-*.wav is
the pick; 0-synth.wav is make_sounds.py's own. --use copies each 1-*.wav up
to <name>.wav, the file the client plays (do that, or copy any candidate
up by hand, after listening).
"""

import glob
import os
import re
import shutil
import subprocess
import sys
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "candidates")

# pack id: (download, its page, what it is). All CC0, checked on each page.
PACKS = {
    "kenney-rpg": ("https://kenney.nl/media/pages/assets/rpg-audio/8e99002d76-1677590336/kenney_rpg-audio.zip",
                   "https://kenney.nl/assets/rpg-audio", "RPG Audio by Kenney"),
    "kenney-impact": ("https://kenney.nl/media/pages/assets/impact-sounds/87b4ddecda-1677589768/kenney_impact-sounds.zip",
                      "https://kenney.nl/assets/impact-sounds", "Impact Sounds by Kenney"),
    "kenney-ui": ("https://kenney.nl/media/pages/assets/interface-sounds/fa43c1dd4d-1677589452/kenney_interface-sounds.zip",
                  "https://kenney.nl/assets/interface-sounds", "Interface Sounds by Kenney"),
    "oga-rpg80": ("https://opengameart.org/sites/default/files/80-CC0-RPG-SFX_0.zip",
                  "https://opengameart.org/content/80-cc0-rpg-sfx", "80 CC0 RPG SFX (rubberduck)"),
    "oga-creature80": ("https://opengameart.org/sites/default/files/80-CC0-creature-SFX_0.zip",
                       "https://opengameart.org/content/80-cc0-creature-sfx", "80 CC0 creature SFX (rubberduck)"),
    "oga-creature80b": ("https://opengameart.org/sites/default/files/80-CC0-creature-sfx-2.zip",
                        "https://opengameart.org/content/80-cc0-creture-sfx-2", "80 CC0 creature SFX #2 (rubberduck)"),
    "oga-death": ("https://opengameart.org/sites/default/files/death%20sound%20effect.zip",
                  "https://opengameart.org/content/hurt-death-sound-effect-for-character", "Hurt / Death sound effect for character"),
    "oga-monster": ("https://opengameart.org/sites/default/files/monster-sounds-volume-2.zip",
                    "https://opengameart.org/content/monster-sound-pack-volume-1", "Monster Sound Pack"),
    "oga-magic": ("https://opengameart.org/sites/default/files/magical_{1_0,2,3,4,5,6_0,7_0}.ogg",
                  "https://opengameart.org/content/magic-spell-sfx", "Magic Spell SFX"),
    "oga-explosion": ("https://opengameart.org/sites/default/files/Chunky%20Explosion.mp3",
                      "https://opengameart.org/content/chunky-explosion", "Chunky Explosion"),
    "oga-roar": ("https://opengameart.org/sites/default/files/monster_roar.wav",
                 "https://opengameart.org/content/cc0-deep-monster-roar", "CC0 Deep Monster Roar"),
    # for fetch_voices.py: creatures, men's and women's voices, weapons
    "oga-rpgpack": ("https://opengameart.org/sites/default/files/rpg_sound_pack.zip",
                    "https://opengameart.org/content/rpg-sound-pack", "RPG Sound Pack (artisticdude)"),
    "oga-yelling": ("https://opengameart.org/sites/default/files/yelling%20sounds.zip",
                    "https://opengameart.org/content/male-gruntyelling-sounds", "Male Grunt/Yelling sounds (HaelDB; CC0 of its two)"),
    "oga-femscream": ("https://opengameart.org/sites/default/files/female_screams.zip",
                      "https://opengameart.org/content/female-screams", "Female screams (congusbongus; files 2-4 CC0 per its license.txt)"),
    "oga-femdying": ("https://opengameart.org/sites/default/files/Dying-Voices-FEMALE-RPG.zip",
                     "https://opengameart.org/node/99640","Dying Voices - Female - RPG (Galacti-Chron; CC0 of its two)"),
    "oga-battle": ("https://opengameart.org/sites/default/files/battle_sound_effects_0.zip",
                   "https://opengameart.org/content/battle-sound-effects", "Battle Sound Effects (artisticdude; CC0 of its five)"),
    "oga-medieval": ("https://opengameart.org/sites/default/files/medieval_sfx_textures_{1,2}_of_2.7z",
                     "https://opengameart.org/content/medieval-sound-effects-weapon-textures",
                     "Medieval sound effects - Weapon Textures (Ben Jaszczak & Brian Nelson)"),
}

# name: (longest seconds, [(pack, file name in it), ...]); the first is the
# pick. ("synth", "") first: the pick is 0-synth.wav.
PICKS = {
    "hit": (0.6, [("kenney-impact", "impactPunch_medium_000"), ("kenney-impact", "impactPunch_medium_002"),
                  ("kenney-impact", "impactPunch_heavy_000"), ("oga-rpg80", "blade_01"), ("kenney-impact", "impactSoft_medium_000")]),
    "crit": (0.8, [("kenney-impact", "impactPlate_heavy_000"), ("kenney-impact", "impactMetal_heavy_000"),
                   ("kenney-impact", "impactPunch_heavy_002"), ("oga-rpg80", "blade_03")]),
    "miss": (0.6, [("kenney-rpg", "knifeSlice"), ("kenney-rpg", "knifeSlice2"), ("kenney-rpg", "drawKnife1"), ("kenney-rpg", "cloth1")]),
    "kill": (1.2, [("oga-creature80b", "die_01"), ("oga-creature80b", "die_02"), ("oga-rpg80", "creature_die_01"),
                   ("oga-monster", "Monster-4"), ("kenney-impact", "impactSoft_heavy_000")]),
    "hurt": (0.8, [("oga-creature80", "hurt_02"), ("oga-creature80", "hurt_05"), ("oga-rpg80", "creature_hurt_01"),
                   ("oga-death", "4"), ("kenney-impact", "impactPunch_heavy_001")]),
    "die": (2.5, [("oga-death", "15"), ("oga-death", "2"), ("oga-death", "7"), ("oga-death", "12"), ("oga-creature80", "scream_01")]),
    "roar": (1.6, [("oga-rpg80", "creature_roar_03"), ("oga-rpg80", "creature_roar_01"), ("oga-creature80b", "roar_04"),
                   ("oga-creature80b", "roar_05"), ("oga-roar", "monster_roar")]),
    "cast": (1.6, [("oga-magic", "magical_3"), ("oga-rpg80", "spell_01"), ("oga-rpg80", "spell_02"), ("oga-magic", "magical_1_0")]),
    # nothing free sounded like a spell going wrong: the synthesised crackle stays
    "fizzle": (1.0, [("synth", ""), ("kenney-ui", "error_005"), ("kenney-ui", "error_003"), ("oga-rpg80", "spell_fire_07"),
                     ("oga-magic", "magical_6_0")]),
    "blast": (2.0, [("oga-explosion", "Chunky Explosion"), ("oga-rpg80", "spell_fire_03"), ("oga-rpg80", "spell_fire_06")]),
    "heal": (1.8, [("oga-magic", "magical_1_0"), ("oga-magic", "magical_4"), ("oga-magic", "magical_2"), ("kenney-ui", "maximize_006")]),
    "loot": (1.2, [("oga-rpg80", "item_gem_04"), ("oga-rpg80", "item_coins_02"), ("kenney-rpg", "handleCoins"),
                   ("kenney-ui", "confirmation_002")]),
    "door_open": (1.2, [("kenney-rpg", "doorOpen_1"), ("kenney-rpg", "doorOpen_2"), ("kenney-rpg", "creak1")]),
    "door_close": (1.0, [("kenney-rpg", "doorClose_1"), ("kenney-rpg", "doorClose_2"), ("kenney-rpg", "doorClose_3")]),
    "door_bash": (1.0, [("kenney-impact", "impactWood_heavy_000"), ("kenney-impact", "impactWood_heavy_001"),
                        ("kenney-impact", "impactPlank_medium_000"), ("oga-rpg80", "wood_01")]),
    "your_turn": (1.2, [("kenney-ui", "confirmation_001"), ("kenney-ui", "bong_001"), ("kenney-ui", "question_001"),
                        ("kenney-ui", "pluck_001")]),
}


def fetch(url, dest):
    req = urllib.request.Request(url, headers={"User-Agent": "mumoria-sounds"})
    with urllib.request.urlopen(req, timeout=120) as r, open(dest, "wb") as f:
        shutil.copyfileobj(r, f)


def unpack(cache):
    """Each pack into cache/<pack id>/ (once)."""
    for pack, (url, _, _) in PACKS.items():
        folder = os.path.join(cache, pack)
        if os.path.isdir(folder):
            continue
        os.makedirs(folder)
        brace = re.search(r"\{([^}]*)\}", url)
        urls = [url[:brace.start()] + part + url[brace.end():] for part in brace.group(1).split(",")] if brace else [url]
        for u in urls:
            name = urllib.request.unquote(u.rsplit("/", 1)[1])
            print(f"downloading {name}")
            data_path = os.path.join(folder, name)
            fetch(u, data_path)
            if name.endswith(".zip"):
                with zipfile.ZipFile(data_path) as z:
                    z.extractall(folder)
                os.remove(data_path)
            elif name.endswith(".7z"):
                subprocess.run(["7z", "x", "-y", f"-o{folder}", data_path], check=True, capture_output=True)
                os.remove(data_path)


def find(cache, pack, stem):
    for path in glob.glob(os.path.join(cache, pack, "**", "*"), recursive=True):
        base = os.path.basename(path)
        if base.startswith("._"):
            continue
        root, ext = os.path.splitext(base)
        if root == stem and ext.lower() in (".ogg", ".wav", ".mp3", ".flac"):
            return path
    raise SystemExit(f"{pack}: no {stem}")


def loudness(path):
    err = subprocess.run(["ffmpeg", "-hide_banner", "-i", path, "-af", "volumedetect", "-f", "null", "-"],
                         capture_output=True, text=True).stderr
    mean = float(re.search(r"mean_volume: (-?[\d.]+)", err).group(1))
    peak = float(re.search(r"max_volume: (-?[\d.]+)", err).group(1))
    return mean, peak


def convert(src, dest, seconds, start=0.0, effect=""):
    """From `start` seconds in, leading silence off, then `effect` (ffmpeg
    filters, at 22050 Hz: a change of pitch), cut and faded to `seconds`,
    mono 22050 Hz, then louder or quieter so its peak is -1 dB and its
    average at most -18 dB."""
    fade = 0.15
    shape = (f"atrim=start={start},asetpts=PTS-STARTPTS,aresample=22050,"
             f"silenceremove=start_periods=1:start_threshold=-50dB,{effect + ',' if effect else ''}"
             f"atrim=0:{seconds},afade=t=out:st={max(0.0, seconds - fade)}:d={fade}")
    tmp = dest + ".tmp.wav"
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", src, "-af", shape,
                    "-ac", "1", "-ar", "22050", "-sample_fmt", "s16", tmp], check=True)
    mean, peak = loudness(tmp)
    gain = min(-1.0 - peak, -18.0 - mean)
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-i", tmp, "-af", f"volume={gain:.2f}dB",
                    "-ac", "1", "-ar", "22050", "-sample_fmt", "s16", dest], check=True)
    os.remove(tmp)


def slug(text):
    return re.sub(r"[^a-z0-9]+", "_", text.lower()).strip("_")


def main():
    args = [a for a in sys.argv[1:] if a != "--use"]
    use = "--use" in sys.argv[1:]
    cache = args[0] if args else "/tmp/mumoria-sfx"
    os.makedirs(cache, exist_ok=True)
    unpack(cache)
    lines = ["# Sound candidates", "",
             "Each folder holds the choices for one sound. `1-*` is the one in use",
             "(copied up to `../<name>.wav`) unless `0-synth.wav`, the synthesised",
             "one (`../make_sounds.py`), is marked in use. To change a sound, copy",
             "any of them up over `../<name>.wav`. All are CC0: see `../CREDITS.md`.",
             "Made by `../fetch_candidates.py`.", ""]
    for name, (seconds, picks) in PICKS.items():
        folder = os.path.join(OUT, name)
        os.makedirs(folder, exist_ok=True)
        for old in glob.glob(os.path.join(folder, "[1-9]-*.wav")):
            os.remove(old)
        lines.append(f"## {name}")
        lines.append("")
        synth = picks[0][0] == "synth"
        if synth:
            picks = picks[1:]
        lines.append("* `0-synth.wav`: synthesised (make_sounds.py)" + (" - **in use**" if synth else ""))
        for n, (pack, stem) in enumerate(picks, 1):
            dest = os.path.join(folder, f"{n}-{pack}-{slug(stem)}.wav")
            convert(find(cache, pack, stem), dest, seconds)
            lines.append(f"* `{os.path.basename(dest)}`: \"{stem}\" from {PACKS[pack][2]}, {PACKS[pack][1]}")
            print(os.path.relpath(dest, HERE))
        lines.append("")
        if use:
            first = os.path.join(folder, "0-synth.wav") if synth else sorted(glob.glob(os.path.join(folder, "1-*.wav")))[0]
            shutil.copyfile(first, os.path.join(HERE, name + ".wav"))
    with open(os.path.join(OUT, "README.md"), "w") as f:
        f.write("\n".join(lines))


if __name__ == "__main__":
    main()
