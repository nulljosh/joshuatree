#!/usr/bin/env python3
"""Original demo songs for the Music app: demo-music.py OUTDIR. Synthesized from scratch (no samples,
no copied tunes), so there is nothing to license. Output is 8-bit mono 11025 Hz WAV, the format
SYS_AUDIO takes, about 40 seconds and 430KB a song."""
import math, os, random, struct, sys

RATE = 11025
NOTE = {n: i for i, n in enumerate(['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'])}

def hz(name):                      # 'A4' -> 440.0
    n, o = name[:-1], int(name[-1])
    return 440.0 * 2 ** ((NOTE[n] + 12 * (o + 1) - 69) / 12)

def wave(kind, ph):
    ph %= 1.0
    if kind == 'sq': return 1.0 if ph < 0.5 else -1.0
    if kind == 'pulse': return 1.0 if ph < 0.25 else -1.0
    if kind == 'tri': return 4 * abs(ph - 0.5) - 1
    if kind == 'saw': return 2 * ph - 1
    return math.sin(2 * math.pi * ph)

class Mix:
    def __init__(self, secs): self.b = [0.0] * int(secs * RATE)
    def note(self, t, name, dur, kind='tri', vol=0.3, a=0.01, r=0.08, vib=0.0):
        f = hz(name); s0 = int(t * RATE); n = int((dur + r) * RATE)
        for i in range(n):
            j = s0 + i
            if j >= len(self.b): break
            x = i / RATE
            env = min(1.0, x / a) if x < dur else max(0.0, 1 - (x - dur) / r)
            env *= 0.75 + 0.25 * math.exp(-x * 3)           # a little pluck, then a held body
            fm = f * (1 + vib * math.sin(2 * math.pi * 5.5 * x))
            self.b[j] += vol * env * wave(kind, fm * x)
    def kick(self, t, vol=0.7):
        s0 = int(t * RATE)
        for i in range(int(0.16 * RATE)):
            if s0 + i >= len(self.b): break
            x = i / RATE; f = 45 + 110 * math.exp(-x * 38)
            self.b[s0 + i] += vol * math.exp(-x * 17) * math.sin(2 * math.pi * f * x)
    def noise(self, t, dur, vol, hp=0.0):
        s0 = int(t * RATE); last = 0.0
        for i in range(int(dur * RATE)):
            if s0 + i >= len(self.b): break
            v = random.uniform(-1, 1); y = v - last * hp; last = v
            self.b[s0 + i] += vol * y * math.exp(-i / RATE * (6 / dur))
    def snare(self, t, vol=0.4): self.noise(t, 0.14, vol)
    def hat(self, t, vol=0.12): self.noise(t, 0.04, vol, hp=0.9)
    def finish(self, echo=0.27, gain=0.3):
        d = int(echo * RATE)
        for i in range(d, len(self.b)): self.b[i] += gain * self.b[i - d]
        peak = max(abs(v) for v in self.b) or 1.0
        out = bytearray()
        for v in self.b:
            q = v / peak * 0.88 * 127 + random.uniform(-0.5, 0.5)   # tiny dither: 8 bits is coarse
            out.append(max(0, min(255, int(round(128 + q)))))
        return bytes(out)

def wav(pcm):
    fmt = struct.pack('<HHIIHH', 1, 1, RATE, RATE, 1, 8)
    body = b'WAVE' + b'fmt ' + struct.pack('<I', 16) + fmt + b'data' + struct.pack('<I', len(pcm)) + pcm
    return b'RIFF' + struct.pack('<I', len(body)) + body

def desert_road():                 # A minor, 92 bpm: soft arpeggios over a walking bass
    random.seed(1); bpm = 92; beat = 60 / bpm; bars = 16; m = Mix(bars * 4 * beat + 2)
    prog = [('A', ['A3', 'C4', 'E4']), ('F', ['F3', 'A3', 'C4']), ('C', ['C4', 'E4', 'G4']), ('G', ['G3', 'B3', 'D4'])]
    root = {'A': 'A2', 'F': 'F2', 'C': 'C3', 'G': 'G2'}
    lead = ['E5', 'D5', 'C5', 'A4', 'C5', 'E5', 'G5', 'E5']
    for bar in range(bars):
        name, ch = prog[bar % 4]; t0 = bar * 4 * beat
        for k in range(8):
            m.note(t0 + k * beat / 2, ch[k % 3] if k % 2 == 0 else ch[(k + 1) % 3], beat / 2 * 0.9, 'pulse', 0.13)
        m.note(t0, root[name], beat * 1.8, 'tri', 0.34); m.note(t0 + 2 * beat, root[name], beat * 1.8, 'tri', 0.3)
        if bar >= 4 and bar % 4 != 3:
            for k in range(4): m.note(t0 + k * beat, lead[(bar + k) % 8], beat * 0.9, 'sq', 0.09, vib=0.01)
        if bar >= 2:
            for k in range(4): m.kick(t0 + k * beat) if k % 2 == 0 else m.snare(t0 + k * beat)
            for k in range(8): m.hat(t0 + k * beat / 2)
    return m.finish()

def sunrise():                     # C major, 72 bpm: slow pads, a singing line on top
    random.seed(2); bpm = 72; beat = 60 / bpm; bars = 12; m = Mix(bars * 4 * beat + 3)
    prog = [['C3', 'G3', 'E4'], ['A2', 'E3', 'C4'], ['F2', 'C3', 'A3'], ['G2', 'D3', 'B3']]
    mel = [['E5', 'G5', 'E5', 'D5'], ['C5', 'E5', 'D5', 'C5'], ['A4', 'C5', 'F5', 'E5'], ['D5', 'G5', 'B4', 'D5']]
    for bar in range(bars):
        ch = prog[bar % 4]; t0 = bar * 4 * beat
        for n in ch: m.note(t0, n, beat * 3.9, 'tri', 0.2, a=0.25, r=0.4)
        if bar >= 2:
            for k, n in enumerate(mel[bar % 4]): m.note(t0 + k * beat, n, beat * 0.95, 'sine', 0.3, a=0.03, r=0.2, vib=0.008)
        if bar >= 6:
            for k in range(8): m.note(t0 + k * beat / 2, ch[k % 3].replace('3', '5').replace('2', '4'), beat / 2, 'sine', 0.07)
        if bar >= 8: m.kick(t0, 0.45); m.kick(t0 + 2 * beat, 0.45)
    return m.finish(echo=0.4, gain=0.38)

def boot_sequence():               # E minor chiptune, 128 bpm: fast, bright, a bit cheeky
    random.seed(3); bpm = 128; beat = 60 / bpm; bars = 20; m = Mix(bars * 4 * beat + 2)
    prog = [['E3', 'G3', 'B3'], ['C3', 'E3', 'G3'], ['D3', 'F#3', 'A3'], ['B2', 'D#3', 'F#3']]
    hook = ['B4', 'E5', 'G5', 'F#5', 'E5', 'B4', 'D5', 'E5']
    for bar in range(bars):
        ch = prog[bar % 4]; t0 = bar * 4 * beat
        for k in range(16): m.note(t0 + k * beat / 4, ch[k % 3], beat / 4 * 0.8, 'pulse', 0.1)
        for k in range(8): m.note(t0 + k * beat / 2, ch[0].replace('3', '2'), beat / 2 * 0.9, 'saw', 0.22)
        if bar >= 4:
            for k in range(8): m.note(t0 + k * beat / 2, hook[(k + bar) % 8], beat / 2 * 0.85, 'sq', 0.11)
        for k in range(4): m.kick(t0 + k * beat, 0.6)
        for k in (1, 3): m.snare(t0 + k * beat, 0.35)
        for k in range(8): m.hat(t0 + k * beat / 2, 0.1)
    return m.finish(echo=0.18, gain=0.22)

if __name__ == '__main__':
    out = sys.argv[1] if len(sys.argv) > 1 else '.'; os.makedirs(out, exist_ok=True)
    for name, fn in (('DESERT.WAV', desert_road), ('SUNRISE.WAV', sunrise), ('BOOT.WAV', boot_sequence)):
        open(os.path.join(out, name), 'wb').write(wav(fn())); print(name)
