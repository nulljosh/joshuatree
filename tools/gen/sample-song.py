#!/usr/bin/env python3
"""Writes HELLO.WAV, the sample song tools/flash-pi.sh puts in JT/MUSIC/ when that folder is empty: sample-song.py OUT.WAV.

Self-made and deterministic (pure arithmetic, no random, no third-party audio): 8 bars of 4/4 at 120 bpm, 16 s, in C major over
C, Am, F, G twice, with three voices (a bass on the root, a soft arpeggio pad, a lead melody). 16-bit PCM mono at 22050 Hz, about
690 KB, the format user/libjt/wav.c reads. A WAV carries no tag, so Music shows the file name as the title: HELLO.WAV is "Hello".
tools/checks/arm64-music-check.py plays it through the same path. Usage: python3 tools/gen/sample-song.py build/HELLO.WAV
"""
import math, struct, sys

RATE, BPM = 22050, 120
BEAT = 60.0 / BPM
NOTE = {"C": 0, "D": 2, "E": 4, "F": 5, "G": 7, "A": 9, "B": 11}

def hz(name, octave):
    """Equal temperament, A4 = 440."""
    return 440.0 * 2 ** ((NOTE[name] + 12 * (octave - 4) - 9) / 12)

CHORDS = [("C", ["C", "E", "G"]), ("A", ["A", "C", "E"]), ("F", ["F", "A", "C"]), ("G", ["G", "B", "D"])] * 2
# the lead: (note, octave, beats) per bar, four beats each
LEAD = [
    [("E", 5, 1), ("G", 5, 1), ("E", 5, 1), ("C", 5, 1)],
    [("C", 5, 1.5), ("E", 5, 0.5), ("A", 5, 1), ("E", 5, 1)],
    [("F", 5, 1), ("A", 5, 1), ("G", 5, 1), ("F", 5, 1)],
    [("D", 5, 1), ("G", 5, 1), ("B", 4, 1), ("G", 4, 1)],
    [("E", 5, 1), ("G", 5, 1), ("C", 6, 1), ("G", 5, 1)],
    [("A", 5, 1.5), ("G", 5, 0.5), ("E", 5, 1), ("C", 5, 1)],
    [("F", 5, 1), ("G", 5, 1), ("A", 5, 1), ("C", 6, 1)],
    [("B", 5, 1), ("G", 5, 1), ("D", 5, 1), ("C", 5, 1)],
]

def add(buf, start, length, freq, amp, kind):
    """One note: a sine (plus a quiet octave for the lead) under a short attack and an exponential release."""
    i0, n = int(start * RATE), int(length * RATE)
    atk, rel = int(0.01 * RATE), max(int(0.25 * RATE), 1)
    for k in range(n):
        if i0 + k >= len(buf): break
        t = k / RATE
        s = math.sin(2 * math.pi * freq * t)
        if kind == "lead": s = 0.8 * s + 0.2 * math.sin(4 * math.pi * freq * t)
        env = min(k / atk, 1.0) * (math.exp(-k / (0.9 * RATE)) if kind != "lead" else 1.0)
        if k > n - rel: env *= (n - k) / rel
        buf[i0 + k] += amp * s * env

def build():
    bars = len(CHORDS)
    total = int(bars * 4 * BEAT * RATE)
    buf = [0.0] * total
    for b, (root, tones) in enumerate(CHORDS):
        t0 = b * 4 * BEAT
        add(buf, t0, 2 * BEAT, hz(root, 2), 0.38, "bass")                   # voice 1: bass, two half-bar notes
        add(buf, t0 + 2 * BEAT, 2 * BEAT, hz(root, 2), 0.30, "bass")
        for j in range(8):                                                  # voice 2: eighth-note arpeggio
            add(buf, t0 + j * BEAT / 2, BEAT, hz(tones[j % 3], 4), 0.12, "pad")
        t = t0
        for name, octv, beats in LEAD[b]:                                   # voice 3: the melody
            add(buf, t, beats * BEAT, hz(name, octv), 0.30, "lead")
            t += beats * BEAT
    peak = max(abs(v) for v in buf)
    gain = 0.8 * 32767 / peak
    return b"".join(struct.pack("<h", int(round(v * gain))) for v in buf)

def wav_bytes():
    data = build()
    fmt = struct.pack("<HHIIHH", 1, 1, RATE, RATE * 2, 2, 16)
    body = b"WAVE" + b"fmt " + struct.pack("<I", 16) + fmt + b"data" + struct.pack("<I", len(data)) + data
    return b"RIFF" + struct.pack("<I", len(body)) + body

if __name__ == "__main__":
    if len(sys.argv) != 2: sys.exit(__doc__)
    open(sys.argv[1], "wb").write(wav_bytes())
