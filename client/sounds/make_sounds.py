#!/usr/bin/env python3
"""Mumoria's starter sounds, synthesised: no samples, no licences to track.

    python3 make_sounds.py        # writes the .wav files beside this script

Each is 22050 Hz, 16-bit mono. They are simple on purpose; replace any of
them with a recorded .wav of the same name (see README.md).
"""

import math
import os
import random
import struct
import wave

RATE = 22050
random.seed(1981)  # the same files every time


def silence(seconds):
    return [0.0] * int(RATE * seconds)


def tone(freq_from, freq_to, seconds, wave_kind="sine", vibrato=0.0, vib_rate=6.0):
    """A tone sliding from freq_from to freq_to."""
    out = []
    phase = 0.0
    n = int(RATE * seconds)
    for i in range(n):
        f = freq_from + (freq_to - freq_from) * i / n
        f *= 1.0 + vibrato * math.sin(2 * math.pi * vib_rate * i / RATE)
        phase += 2 * math.pi * f / RATE
        if wave_kind == "saw":
            v = (phase / math.pi) % 2.0 - 1.0
        elif wave_kind == "square":
            v = 1.0 if math.sin(phase) >= 0 else -1.0
        else:
            v = math.sin(phase)
        out.append(v)
    return out


def noise(seconds):
    return [random.uniform(-1.0, 1.0) for _ in range(int(RATE * seconds))]


def lowpass(samples, cutoff_from, cutoff_to=None):
    """One-pole low-pass, its cutoff sliding from cutoff_from to cutoff_to."""
    cutoff_to = cutoff_from if cutoff_to is None else cutoff_to
    out, y, n = [], 0.0, len(samples)
    for i, x in enumerate(samples):
        fc = cutoff_from + (cutoff_to - cutoff_from) * i / max(1, n)
        a = 1.0 - math.exp(-2 * math.pi * fc / RATE)
        y += a * (x - y)
        out.append(y)
    return out


def envelope(samples, attack, decay_power=1.0):
    """Rises over `attack` seconds, then falls away to nothing by the end."""
    n = len(samples)
    a = max(1, int(RATE * attack))
    out = []
    for i, x in enumerate(samples):
        if i < a:
            g = i / a
        else:
            g = (1.0 - (i - a) / max(1, n - a)) ** decay_power
        out.append(x * g)
    return out


def mix(*parts):
    """Lays (offset_seconds, samples, gain) parts over each other."""
    end = max(int(off * RATE) + len(s) for off, s, _ in parts)
    out = [0.0] * end
    for off, s, gain in parts:
        start = int(off * RATE)
        for i, x in enumerate(s):
            out[start + i] += x * gain
    return out


def write(name, samples, peak=0.85):
    top = max(abs(x) for x in samples) or 1.0
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), name + ".wav")
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", int(x / top * peak * 32767)) for x in samples))
    print(f"{name}.wav  {len(samples) / RATE:.2f} s")


def thump(freq, seconds):
    return envelope(tone(freq, freq * 0.5, seconds), 0.002, 2.0)


def whoosh(seconds, lo, hi):
    return envelope(lowpass(noise(seconds), lo, hi), seconds * 0.45, 1.5)


def bell(freq, seconds):
    return envelope(mix((0, tone(freq, freq, seconds), 1.0), (0, tone(freq * 2.01, freq * 2.01, seconds), 0.35)), 0.004, 2.5)


SOUNDS = {
    # a blow lands
    "hit": lambda: mix((0, thump(150, 0.14), 1.0), (0, envelope(lowpass(noise(0.07), 2500), 0.001, 2.0), 0.6)),
    # a critical: the blow and a ring of steel
    "crit": lambda: mix((0, thump(140, 0.16), 1.0), (0, envelope(lowpass(noise(0.08), 3000), 0.001, 2.0), 0.6),
                        (0.03, envelope(tone(1900, 1850, 0.35), 0.002, 3.0), 0.35)),
    # through the air
    "miss": lambda: whoosh(0.22, 400, 3000),
    # a monster dies
    "kill": lambda: mix((0, thump(110, 0.18), 1.0), (0.05, envelope(tone(320, 70, 0.35, "saw"), 0.01, 1.5), 0.35)),
    # a monster's blow lands on you
    "hurt": lambda: mix((0, envelope(lowpass(tone(170, 110, 0.22, "saw"), 1200), 0.01, 1.2), 1.0),
                        (0, thump(120, 0.12), 0.8)),
    # a player dies
    "die": lambda: mix((0, envelope(lowpass(tone(420, 55, 1.2, "saw", vibrato=0.03, vib_rate=5), 1500), 0.02, 1.2), 1.0),
                       (0, thump(90, 0.4), 0.6)),
    # a monster wakes, or breathes
    "roar": lambda: mix((0, envelope(lowpass(tone(85, 65, 0.8, "saw", vibrato=0.06, vib_rate=11), 900), 0.15, 1.3), 1.0),
                        (0, envelope(lowpass(noise(0.8), 700), 0.2, 1.3), 0.5)),
    # a spell is cast
    "cast": lambda: envelope(mix((0, tone(600, 1400, 0.4), 1.0), (0, tone(900, 2100, 0.4), 0.5),
                                 (0, tone(605, 1405, 0.4), 0.7)), 0.05, 1.5),
    # a spell fails
    "fizzle": lambda: mix((0, envelope(lowpass(noise(0.35), 4000), 0.01, 1.5), 0.3),
                          *[(random.uniform(0, 0.3), envelope(noise(0.006), 0.0005), 1.0) for _ in range(14)]),
    # a ball or a breath bursts
    "blast": lambda: mix((0, envelope(lowpass(noise(0.8), 1800, 200), 0.004, 1.8), 1.0), (0, thump(70, 0.5), 0.8)),
    # healing
    "heal": lambda: mix((0.00, bell(523, 0.35), 1.0), (0.09, bell(659, 0.35), 1.0),
                        (0.18, bell(784, 0.35), 1.0), (0.27, bell(1047, 0.45), 1.0)),
    # a rare item appears
    "loot": lambda: mix((0.00, bell(1319, 0.5), 1.0), (0.12, bell(1760, 0.6), 0.9), (0.24, bell(2093, 0.7), 0.7)),
    # a door creaks open
    "door_open": lambda: mix((0, envelope(lowpass(tone(230, 330, 0.5, "saw", vibrato=0.08, vib_rate=17), 1100), 0.05, 0.8), 1.0),
                             (0.45, thump(110, 0.12), 0.6)),
    # a door shuts
    "door_close": lambda: mix((0, thump(95, 0.25), 1.0), (0, envelope(lowpass(noise(0.05), 2000), 0.001, 2.0), 0.7)),
    # a door is smashed open
    "door_bash": lambda: mix((0, thump(80, 0.35), 1.0), (0, envelope(lowpass(noise(0.45), 3500, 600), 0.002, 1.6), 1.0),
                             (0.08, thump(120, 0.2), 0.6)),
    # battle turns: your turn
    "your_turn": lambda: mix((0.00, bell(784, 0.3), 1.0), (0.15, bell(1047, 0.5), 1.0)),
}

if __name__ == "__main__":
    for name, make in SOUNDS.items():
        write(name, make())
