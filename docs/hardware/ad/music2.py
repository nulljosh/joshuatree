# Original bed: upbeat but clean, Apple-ad style. Claps, round bass, plucked piano hook. 120 BPM. numpy only.
import numpy as np, wave
SR, BPM, DUR = 44100, 120, 37.0
B = 60 / BPM; BAR = 4 * B
N = int(SR * DUR); t_all = np.arange(N) / SR
mix = np.zeros(N)
DROP = 3.1          # groove starts on the first OS shot
END = 30.9          # end card: drums out, chord rings
def hz(n): return 440 * 2 ** ((n - 69) / 12)
def add(start, sig, gain):
    i = int(round(start * SR))
    if i < 0: sig, i = sig[-i:], 0
    j = min(N, i + len(sig))
    if i < N and j > i: mix[i:j] += gain * sig[:j - i]
def saw(f, d, det=0.0):
    t = np.arange(int(d * SR)) / SR
    return sum(2 * ((t * f * 2 ** (k / 1200) + 0.3 * k) % 1) - 1 for k in (-det, det)) / 2
def lp(x, a):  # one-pole lowpass, a in (0,1): smaller = darker
    y = np.empty_like(x); acc = 0.0
    for i, v in enumerate(x): acc += a * (v - acc); y[i] = acc
    return y
rng = np.random.default_rng(3)
kick_t = np.arange(int(0.35 * SR)) / SR
KICK = np.sin(2 * np.pi * (50 * kick_t + 90 * (1 - np.exp(-kick_t * 30)) / 30)) * np.exp(-kick_t * 9)
CLAP = lp(lp(rng.standard_normal(int(0.18 * SR)), 0.3), 0.3) * np.exp(-np.arange(int(0.18 * SR)) / SR * 22)
HAT = np.diff(rng.standard_normal(int(0.06 * SR) + 1)) * np.exp(-np.arange(int(0.06 * SR)) / SR * 70)
def pluck(notes, d):
    tt = np.arange(int(d * SR)) / SR
    return sum(np.sin(2 * np.pi * hz(n) * tt) + 0.35 * np.sin(4 * np.pi * hz(n) * tt) * np.exp(-tt * 6)
               + 0.12 * np.sin(6 * np.pi * hz(n) * tt) * np.exp(-tt * 10) for n in notes) / len(notes) * np.exp(-tt * 5) * np.minimum(1, tt / 0.004)
HOOK = [[64, 67, 69, 72], [72, 71, 67, 64]]
def epiano(notes, d):
    # warm Rhodes-ish chord: sine + soft bell partial, slow tremolo, fills the 300-1500 Hz middle
    tt = np.arange(int(d * SR)) / SR
    trem = 1 + 0.12 * np.sin(2 * np.pi * 4.5 * tt)
    s = sum(np.sin(2 * np.pi * hz(n) * tt) + 0.18 * np.sin(2 * np.pi * hz(n) * 7.0 * tt) * np.exp(-tt * 8) for n in notes) / len(notes)
    return s * trem * np.exp(-tt * 1.1) * np.minimum(1, tt / 0.01)
SHAKE = lp(np.diff(rng.standard_normal(int(0.05 * SR) + 1)), 0.6) * np.hanning(int(0.05 * SR))
prog = [(57, [57, 60, 64, 67]), (53, [53, 57, 60, 64]), (48, [55, 60, 64, 67]), (55, [55, 59, 62, 67])]  # Am7 Fmaj7 C G
OFF = DROP % BAR - BAR  # grid origin: downbeats at ..., DROP - BAR, DROP, DROP + BAR, ...
bars = int((DUR - OFF) / BAR) + 1
for b in range(bars):
    t0 = OFF + b * BAR; root, ch = prog[b % 4]
    groove = DROP - 0.01 <= t0 < END - 0.3
    # pad under everything, filtered darker before the drop
    pad = sum(saw(hz(n), BAR + 0.3, 9) for n in ch) / 4
    env = np.minimum(1, np.arange(len(pad)) / SR / 0.25) * np.minimum(1, (len(pad) / SR - np.arange(len(pad)) / SR) / 0.3)
    add(t0, lp(pad * env, 0.04 if not groove else 0.06), 0.12)
    if not groove and t0 >= END - 0.01:
        add(t0, lp(sum(saw(hz(n + 12), 5.5, 7) for n in ch) / 4 * np.exp(-np.arange(int(5.5 * SR)) / SR * 0.8), 0.15), 0.25)
        continue
    if not groove:
        for j, n in enumerate(HOOK[b % 2]):
            add(t0 + j * B * 0.75 + (B if j > 1 else 0), pluck([n + 12], 0.9), 0.20)
        continue
    add(t0, epiano([n + 12 for n in ch], 2 * B), 0.30)
    add(t0 + 2 * B + B / 2, epiano([n + 12 for n in ch], 1.5 * B), 0.20)
    for k in range(4):
        bt = t0 + k * B
        for q in range(4): add(bt + q * B / 4, SHAKE, 0.05 if q % 2 else 0.03)
        add(bt, KICK, 0.9)
        if k in (1, 3): add(bt, CLAP, 0.35)
        add(bt + B / 2, lp(HAT, 0.5), 0.07)
        # octave-bounce bass on eighths
        for e in range(2):
            n = root - 24 + (12 if e else 0)
            tt = np.arange(int(B / 2 * 0.9 * SR)) / SR; s = np.sin(2 * np.pi * hz(n) * tt) + 0.2 * np.sin(4 * np.pi * hz(n) * tt)
            add(bt + e * B / 2, s * np.exp(-tt * 5), 0.45)
        # offbeat chord stabs, bright
        if k in (0, 2):
            add(bt + B / 2, pluck(ch, 0.5), 0.22)
        # handclap layer: two quick bursts, the Apple-ad clap
        if k in (1, 3): add(bt + 0.012, CLAP, 0.25)
    # hook: a four-note plucked motif each bar, up an octave
    for j, n in enumerate(HOOK[b % 2]):
        add(t0 + j * B * 0.75 + (B if j > 1 else 0), pluck([n + 12], 0.6), 0.30)
# gentle master: soft clip + fade tail
mix = mix - np.convolve(mix, np.ones(2205) / 2205, mode='same')  # ~20 Hz highpass, kills DC
mix = np.tanh(mix * 1.1)
mix[-int(2.5 * SR):] *= np.linspace(1, 0, int(2.5 * SR))
mix /= np.max(np.abs(mix)); mix *= 0.85
with wave.open("music2.wav", "w") as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
    w.writeframes((mix * 32767).astype("<i2").tobytes())
print("MUSIC2", DUR, "bars", bars)
