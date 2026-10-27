#!/usr/bin/env python3
"""Makes the creatures', the players' and the weapons' sounds from free (CC0) packs.

    python3 fetch_voices.py [cache_dir]

The server names a sound for whose it is: a monster's family and what it
did (orc_hurt), a player's voice (male_die, female_die), a weapon's kind
(axe_swing, bow_shoot). See README.md. Each entry below is one of those
names; its first source becomes <name>.wav and any more <name>2.wav,
<name>3.wav, which the client picks among at random.

A source is (pack, file stem, options): "take" is which of a recording's
takes to use (the weapon packs hold several in one file, 0 the first);
"st" moves its pitch by semitones, keeping its length; "speed" plays it
faster or slower, pitch and all (a small rat's squeak from a bigger one).
Men's voices go down three semitones and women's up two, so a man
sounds deeper than a woman whatever the take. The server pitches each
creature a little apart on top of that (bigger, deeper).

Shares the packs and the cutting with fetch_candidates.py (needs ffmpeg
with rubberband, and 7z). Only CC0 packs: see CREDITS.md.
"""

import os
import re
import subprocess
import sys

from fetch_candidates import HERE, PACKS, convert, find, unpack

RS, YL, FS, FD, BT, MD = "oga-rpgpack", "oga-yelling", "oga-femscream", "oga-femdying", "oga-battle", "oga-medieval"
C1, C2, R80, MON, KI, KR, ROAR, DE = ("oga-creature80", "oga-creature80b", "oga-rpg80", "oga-monster", "kenney-impact",
                                     "kenney-rpg", "oga-roar", "oga-death")

MAN = {"st": -3}  # every man's voice a little deeper
WOMAN = {"st": 2}  # every woman's a little higher

# name: (longest seconds, [(pack, stem, options), ...])
VOICES = {
    # --- players: a man's voice, a woman's ---
    "male_attack": (0.6, [(YL, "yell3", MAN), (YL, "2yell6", MAN), (YL, "3grunt3", MAN)]),
    "male_hurt": (0.7, [(YL, "yell6", MAN), (YL, "2yell9", MAN), (YL, "3yell10", MAN)]),
    "male_crit": (0.9, [(YL, "3yell2", MAN), (YL, "3yell5", MAN), (YL, "yell8", MAN)]),
    "male_die": (1.8, [(YL, "3yell13", MAN), (YL, "3yell16", MAN), (YL, "yell1", MAN)]),
    "female_attack": (0.45, [(FD, "DEATH -- High ugh ouch!", WOMAN), (FD, "DEATH -- Medium -- Ugh! Im Hit!", WOMAN),
                             (FD, "DEATH -- Ugh ! you got me!", WOMAN)]),
    "female_hurt": (0.6, [(FD, "DEATH -- lower Ugh that sucks", WOMAN), (FD, "DEATH -- Quick ah shit", WOMAN),
                          (FD, "DEATH -- Medium Ughh this hurts!", WOMAN)]),
    "female_crit": (0.9, [(FS, "4", WOMAN), (FS, "2", WOMAN)]),
    "female_die": (1.8, [(FS, "3", WOMAN), (FD, "DEATH -- HIgh dying noise", WOMAN)]),

    # --- monsters, by family (fx.cpp creature_voice) ---
    "human_attack": (0.7, [(C2, "human_01", {}), (C2, "human_05", {})]),
    "human_hurt": (0.7, [(C2, "human_02", {}), (C2, "human_06", {})]),
    "human_crit": (0.9, [(C2, "human_03", {})]),
    "human_die": (1.5, [(DE, "7", {}), (DE, "10", {})]),
    "goblin_attack": (0.6, [(RS, "mnstr2", {"st": 6}), (RS, "mnstr8", {"st": 6})]),
    "goblin_hurt": (0.6, [(RS, "mnstr1", {"st": 7}), (RS, "mnstr12", {"st": 7})]),
    "goblin_crit": (0.7, [(RS, "mnstr10", {"st": 6})]),
    "goblin_die": (1.2, [(C2, "die_02", {"st": 5})]),
    "orc_attack": (0.7, [(RS, "mnstr3", {}), (RS, "mnstr9", {})]),
    "orc_hurt": (0.6, [(RS, "mnstr1", {}), (RS, "mnstr15", {})]),
    "orc_crit": (0.9, [(RS, "mnstr5", {})]),
    "orc_die": (1.2, [(RS, "mnstr14", {})]),
    "orc_roar": (1.0, [(RS, "mnstr11", {})]),
    "ogre_attack": (0.7, [(RS, "ogre2", {}), (RS, "ogre3", {})]),
    "ogre_hurt": (0.6, [(RS, "ogre1", {}), (RS, "ogre5", {})]),
    "ogre_crit": (0.7, [(RS, "ogre4", {})]),
    "ogre_die": (1.2, [(C1, "grunt_03", {"st": -3})]),
    "ogre_roar": (1.4, [(C2, "roar_05", {"st": -2})]),
    "troll_attack": (0.8, [(C1, "troll_01", {}), (C1, "troll_02", {})]),
    "troll_hurt": (0.7, [(C1, "troll_03", {})]),
    "troll_crit": (0.8, [(RS, "ogre4", {"st": -3})]),
    "troll_die": (1.4, [(C2, "die_03", {"st": -4})]),
    "troll_roar": (1.4, [(C1, "roar_01", {"st": -3})]),
    "giant_attack": (0.8, [(RS, "giant1", {}), (RS, "giant3", {})]),
    "giant_hurt": (0.7, [(RS, "giant4", {}), (RS, "giant5", {})]),
    "giant_crit": (0.9, [(RS, "giant2", {})]),
    "giant_die": (1.6, [(C2, "die_04", {"st": -6})]),
    "giant_roar": (1.4, [(C1, "roar_02", {"st": -5})]),
    "dragon_attack": (0.9, [(R80, "creature_roar_01", {}), (C2, "roar_06", {})]),
    "dragon_hurt": (0.8, [(R80, "creature_hurt_02", {"st": -4})]),
    "dragon_crit": (1.0, [(C2, "monster_10", {"st": -3})]),
    "dragon_die": (2.0, [(C2, "die_01", {"st": -6})]),
    "dragon_roar": (1.8, [(ROAR, "monster_roar", {}), (R80, "creature_roar_03", {})]),
    "zombie_attack": (0.8, [(RS, "shade3", {"st": -5}), (RS, "shade5", {"st": -5})]),
    "zombie_hurt": (0.7, [(RS, "shade1", {"st": -5})]),
    "zombie_crit": (0.8, [(RS, "shade14", {"st": -4})]),
    "zombie_die": (1.5, [(RS, "shade9", {"st": -6})]),
    "zombie_roar": (1.3, [(RS, "shade12", {"st": -5})]),
    "skeleton_attack": (0.4, [(KI, "impactWood_light_001", {"st": 3})]),
    "skeleton_hurt": (0.4, [(KI, "impactWood_light_003", {"st": 2})]),
    "skeleton_crit": (0.6, [(KI, "impactWood_medium_002", {})]),
    "skeleton_die": (1.0, [(R80, "wood_03", {}), (R80, "stones_02", {})]),
    "ghost_attack": (0.8, [(RS, "shade4", {}), (RS, "shade10", {})]),
    "ghost_hurt": (0.7, [(RS, "shade2", {}), (RS, "shade15", {})]),
    "ghost_crit": (1.0, [(RS, "shade7", {})]),
    "ghost_die": (1.6, [(RS, "shade8", {})]),
    "ghost_roar": (1.3, [(RS, "shade13", {})]),
    "vampire_attack": (0.8, [(C1, "breath", {})]),
    "vampire_hurt": (0.7, [(RS, "shade6", {"st": -2})]),
    "vampire_crit": (0.9, [(C2, "monster_12", {})]),
    "vampire_die": (1.4, [(RS, "shade11", {"st": -3})]),
    "insect_attack": (0.5, [(C2, "bug_05", {}), (C2, "bug_06", {}), (RS, "bite-small", {})]),
    "insect_hurt": (0.5, [(C1, "bug_01", {}), (C1, "bug_02", {})]),
    "insect_crit": (0.6, [(C2, "bug_09", {})]),
    "insect_die": (0.8, [(C1, "bug_03", {}), (C1, "bug_04", {})]),
    "bat_attack": (0.4, [(C1, "cute_01", {"st": 5})]),
    "bat_hurt": (0.4, [(C1, "cute_02", {"st": 6})]),
    "bat_crit": (0.5, [(C1, "cute_03", {"st": 6})]),
    "bat_die": (0.7, [(C1, "cute_04", {"st": 4})]),
    "rodent_attack": (0.4, [(C1, "cute_05", {"st": 3})]),
    "rodent_hurt": (0.4, [(C1, "cute_06", {"st": 3})]),
    "rodent_crit": (0.5, [(C1, "cute_07", {"st": 3})]),
    "rodent_die": (0.7, [(C1, "cute_08", {"st": 2})]),
    "snake_attack": (0.6, [(C1, "spit_01", {}), (C1, "breath", {"st": 3})]),
    "snake_hurt": (0.5, [(C1, "spit_02", {})]),
    "snake_crit": (0.6, [(C1, "spit_03", {"st": -2})]),
    "snake_die": (1.0, [(C1, "breath", {"st": -3})]),
    "canine_attack": (0.6, [(C1, "barking_01", {}), (C1, "barking_02", {})]),
    "canine_hurt": (0.5, [(RS, "wolfman", {"st": 3})]),
    "canine_crit": (0.6, [(C1, "hurt_02", {})]),
    "canine_die": (1.2, [(C1, "howl", {"st": -2})]),
    "canine_roar": (1.5, [(C1, "howl", {})]),
    "slime_attack": (0.5, [(RS, "slime1", {}), (RS, "slime4", {})]),
    "slime_hurt": (0.5, [(RS, "slime2", {}), (C2, "slime_01", {})]),
    "slime_crit": (0.6, [(RS, "slime8", {})]),
    "slime_die": (1.0, [(R80, "creature_slime_01", {}), (C2, "slime_05", {})]),
    "eye_attack": (0.7, [(C1, "weird_01", {})]),
    "eye_hurt": (0.6, [(C1, "weird_02", {})]),
    "eye_crit": (0.7, [(C1, "weird_03", {})]),
    "eye_die": (1.0, [(C1, "weird_04", {})]),
    "frog_attack": (0.6, [(C1, "burp_01", {})]),
    "frog_hurt": (0.5, [(C1, "burble_01", {})]),
    "frog_crit": (0.6, [(C1, "burp_02", {"st": -2})]),
    "frog_die": (1.0, [(C1, "burble_02", {"st": -3})]),
    "golem_attack": (0.7, [(C2, "stomp_01", {})]),
    "golem_hurt": (0.5, [(KI, "impactMining_001", {})]),
    "golem_crit": (0.6, [(KI, "impactMining_003", {})]),
    "golem_die": (1.4, [(R80, "stones_01", {})]),
    "golem_roar": (1.2, [(RS, "giant2", {"st": -7})]),
    "spirit_attack": (0.8, [(C2, "breath_02", {})]),
    "spirit_hurt": (0.6, [(C2, "weird_06", {})]),
    "spirit_crit": (0.8, [(C2, "weird_07", {})]),
    "spirit_die": (1.2, [(C2, "weird_08", {})]),
    "demon_attack": (0.8, [(C2, "monster_08", {}), (C2, "monster_09", {})]),
    "demon_hurt": (0.7, [(C2, "monster_11", {}), (C2, "monster_13", {})]),
    "demon_crit": (0.9, [(C2, "monster_14", {})]),
    "demon_die": (1.6, [(C2, "monster_15", {"st": -3})]),
    "demon_roar": (1.6, [(C2, "monster_16", {"st": -4})]),
    "harpy_attack": (0.6, [(C1, "scream_01", {"st": 2})]),
    "harpy_hurt": (0.6, [(C1, "scream_02", {})]),
    "harpy_crit": (0.8, [(FS, "2", {"st": 4})]),
    "harpy_die": (1.2, [(C1, "hurt_02", {"st": -1})]),
    "coins_attack": (0.5, [(R80, "item_coins_01", {})]),
    "coins_hurt": (0.5, [(R80, "item_coins_02", {})]),
    "coins_crit": (0.6, [(R80, "item_coins_03", {})]),
    "coins_die": (1.0, [(KR, "handleCoins", {})]),
    "beast_attack": (0.7, [(C1, "monster_01", {}), (C1, "monster_02", {})]),
    "beast_hurt": (0.6, [(C1, "monster_03", {})]),
    "beast_crit": (0.8, [(C1, "monster_04", {})]),
    "beast_die": (1.4, [(C1, "monster_05", {})]),
    "beast_roar": (1.5, [(C1, "roar_03", {})]),
    # a family's own file missing: these (the client drops the family's name)
    "attack": (0.6, [(C1, "grunt_01", {})]),
    "swing": (0.4, [(RS, "swing", {})]),

    # --- weapons (fx.cpp weapon_sound): its swish, its blow, a critical blow ---
    "sword_swing": (0.5, [(MD, "Sabre Swing", {"take": 0}), (MD, "Katana Swing", {"take": 0}), (MD, "Katana Swing", {"take": 1})]),
    "sword_hit": (0.6, [(R80, "blade_01", {}), (R80, "blade_02", {}), (KR, "knifeSlice", {})]),
    "sword_crit": (0.8, [(R80, "blade_03", {}), (KI, "impactMetal_heavy_001", {})]),
    "dagger_swing": (0.4, [(MD, "Dagger Swing", {"take": 0}), (MD, "Dagger Swing", {"take": 1})]),
    "dagger_hit": (0.5, [(KR, "knifeSlice2", {}), (KR, "knifeSlice", {"st": 2})]),
    "dagger_crit": (0.6, [(R80, "blade_01", {"st": 3})]),
    "axe_swing": (0.6, [(MD, "Axe Swing", {"take": 0}), (MD, "Axe Swing", {"take": 2})]),
    "axe_hit": (0.6, [(KR, "chop", {}), (KI, "impactWood_heavy_002", {})]),
    "axe_crit": (0.8, [(KR, "chop", {"st": -3}), (KI, "impactPlank_medium_002", {"st": -2})]),
    "mace_swing": (0.5, [(RS, "swing", {"st": -5}), (BT, "swish_3", {"st": -4})]),
    "mace_hit": (0.6, [(KI, "impactPunch_heavy_000", {}), (KI, "impactPlate_medium_001", {})]),
    "mace_crit": (0.8, [(KI, "impactPlate_heavy_001", {}), (KI, "impactMetal_heavy_002", {})]),
    "spear_swing": (0.6, [(MD, "Spear Swing", {"take": 0})]),
    "spear_hit": (0.6, [(KI, "impactSoft_heavy_001", {}), (KR, "knifeSlice2", {"st": -3})]),
    "spear_crit": (0.8, [(KI, "impactSoft_heavy_002", {}), (R80, "blade_02", {"st": -3})]),
    "pole_swing": (0.7, [(MD, "Spear Swing", {"take": 0, "speed": 0.85})]),
    "pole_hit": (0.6, [(KR, "chop", {"st": -2}), (R80, "blade_02", {"st": -2})]),
    "pole_crit": (0.8, [(KI, "impactMetal_heavy_003", {})]),
    "whip_swing": (0.4, [(BT, "swish_2", {"st": 3})]),
    "whip_hit": (0.5, [(KI, "impactSoft_medium_001", {"st": 5})]),
    "whip_crit": (0.6, [(KI, "impactPunch_heavy_003", {"st": 4})]),
    "pick_swing": (0.5, [(RS, "swing", {"st": -4})]),
    "pick_hit": (0.6, [(KI, "impactMining_000", {}), (KI, "impactMining_002", {})]),
    "pick_crit": (0.8, [(KI, "impactMining_004", {"st": -2})]),
    "fist_swing": (0.3, [(RS, "swing3", {})]),
    "fist_hit": (0.5, [(KI, "impactPunch_medium_000", {}), (KI, "impactPunch_medium_001", {}), (KI, "impactPunch_medium_002", {})]),
    "fist_crit": (0.6, [(KI, "impactPunch_heavy_002", {})]),
    # missiles: the launcher as it looses, then what it is as it lands or flies by
    "bow_shoot": (0.6, [(MD, "English Longbow Shoot", {"take": 0}), (MD, "Scythian Recurve Shoot", {"take": 0}), (BT, "Bow", {})]),
    "crossbow_shoot": (0.6, [(MD, "Crossbow Shoot", {"take": 0}), (MD, "Crossbow Shoot", {"take": 1})]),
    "sling_shoot": (0.5, [(BT, "swish_3", {"speed": 0.8}), (RS, "swing2", {"st": -6})]),
    "throw": (0.4, [(BT, "swish_2", {}), (RS, "swing2", {})]),
    "arrow_hit": (0.5, [(KI, "impactWood_light_002", {}), (KI, "impactSoft_medium_002", {})]),
    "arrow_miss": (0.7, [(MD, "Scythian Recurve Arrow Passby", {"take": 0}), (MD, "Scythian Recurve Arrow Pass", {"take": 0})]),
    "bolt_hit": (0.5, [(KI, "impactSoft_heavy_000", {}), (KI, "impactWood_medium_001", {})]),
    "bolt_miss": (0.7, [(MD, "Crossbow Bolt Passby", {"take": 0})]),
    "stone_hit": (0.5, [(KI, "impactGeneric_light_000", {}), (R80, "item_stone_01", {})]),
    "stone_miss": (0.6, [(R80, "item_stone_02", {}), (R80, "item_stone_03", {})]),
    # a monster's blow landing (fx.cpp blow_sound)
    "blow_bite": (0.5, [(RS, "bite-small", {}), (RS, "bite-small2", {}), (C1, "eat_01", {})]),
    "blow_claw": (0.5, [(KR, "knifeSlice2", {"st": -2}), (KI, "impactSoft_medium_003", {})]),
    "blow_sting": (0.4, [(RS, "bite-small3", {"st": 5})]),
    "blow_spit": (0.5, [(C1, "spit_03", {})]),
    "blow_slime": (0.5, [(RS, "slime6", {})]),
    "blow_crush": (0.6, [(KI, "impactSoft_heavy_003", {"st": -3}), (KI, "impactPunch_heavy_004", {"st": -3})]),
    "blow_touch": (0.5, [(KR, "cloth1", {}), (KR, "cloth3", {})]),
    "blow_hit": (0.5, [(KI, "impactPunch_heavy_001", {}), (KI, "impactPunch_medium_003", {})]),
}


def takes(path):
    """Where each take in a recording starts, in seconds (gaps of a quarter second or more)."""
    err = subprocess.run(["ffmpeg", "-hide_banner", "-i", path, "-af", "silencedetect=n=-35dB:d=0.25", "-f", "null", "-"],
                         capture_output=True, text=True).stderr
    return [0.0] + [max(0.0, float(t) - 0.02) for t in re.findall(r"silence_end: ([\d.]+)", err)]


def effect(options):
    parts = []
    if options.get("st"):
        parts.append(f"rubberband=pitch={2 ** (options['st'] / 12):.4f}")
    if options.get("speed"):
        parts.append(f"asetrate={22050 * options['speed']:.0f},aresample=22050")
    return ",".join(parts)


def main():
    cache = sys.argv[1] if len(sys.argv) > 1 else "/tmp/mumoria-sfx"
    os.makedirs(cache, exist_ok=True)
    unpack(cache)
    lines = ["# Creature, voice and weapon sounds", "",
             "Made by `../fetch_voices.py` (which says how). `<name>.wav` is the first",
             "source, `<name>2.wav` the second and so on; the client plays one of them",
             "at random. All are CC0: see `../CREDITS.md`.", ""]
    for name, (seconds, sources) in VOICES.items():
        for n, (pack, stem, options) in enumerate(sources, 1):
            src = find(cache, pack, stem)
            start = 0.0
            if options.get("take"):
                starts = takes(src)
                start = starts[min(options["take"], len(starts) - 1)]
            dest = os.path.join(HERE, f"{name}{n if n > 1 else ''}.wav")
            convert(src, dest, seconds, start, effect(options))
            how = ", ".join(f"{k} {v}" for k, v in options.items())
            lines.append(f"* `{os.path.basename(dest)}`: \"{stem}\"{f' ({how})' if how else ''} from {PACKS[pack][2]}")
            print(os.path.basename(dest))
    with open(os.path.join(HERE, "candidates", "VOICES.md"), "w") as f:
        f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
