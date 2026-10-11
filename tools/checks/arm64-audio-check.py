#!/usr/bin/env python3
"""ARM64 audio: the polled PWM driver for the Pi's 3.5 mm jack (arch/arm64/audio.c, the `beep` command in cmd.c).

Three parts, no hardware:
1. Static: audio.c never waits on `ticks`, never touches the GPIO set/clear registers (GPIO 42 is the green light), and
   main.c, ask.c and cmd.c reach it only from `beep` (never at boot).
2. Host harness: audio.c compiled with a fake PWM, clock manager and GPIO. A tone and a stereo buffer reach the FIFO in order
   and scaled into the range, the ring checksum matches a tone computed here in Python, GPIO 42's bits are left alone, and a
   clock that never starts, a FIFO stuck full and a FIFO that never empties each end in -1 with one `audio:` line and the PWM
   switched off again (the reads are counted, so a hang would fail).
3. QEMU virt, the audiotest build: `beep` runs at boot, and the UART shows the tone, its frame count and the same ring
   checksum as the Python tone. virt has no PWM, so it also says so and returns an error. The jack itself is NOT proved here.
Skips (exit 0) where clang or qemu is missing.
"""
import os, re, shutil, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch/arm64")
fails = []


def tone_sum(freq, ms, vol=70, rate=44100, amp=6000):
    """The generator's ring checksum, computed independently: FNV-1a over each sample's low then high byte, L then R, after
    the master volume (default 70) scales it toward zero. Returns frames and checksum."""
    step = (freq << 32) // rate
    phase, h, frames = 0, 2166136261, rate * ms // 1000
    for _ in range(frames):
        t = abs((phase >> 18) - 8192)
        a = (t - 4096) * amp
        s = a // 4096 if a >= 0 else -((-a) // 4096)   # C division truncates toward zero
        s = s * vol // 100 if s >= 0 else -((-s * vol) // 100)
        phase = (phase + step) & 0xFFFFFFFF
        for _ in range(2):
            for b in (s & 0xFF, (s >> 8) & 0xFF):
                h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return frames, h


# 1. static
src = open(os.path.join(arch, "audio.c")).read()
code = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
if re.search(r"\bticks\b", code): fails.append("audio.c waits on ticks")
if re.search(r"GPIO_BASE \+ 0x(1[CDEF]|2[0-9A-F]|30)\b", code): fails.append("audio.c touches the GPIO set/clear registers (pin 42 is the green light)")
for f in ("main.c", "ask.c", "cmd.c", "xhci.c", "wifi.c"):
    t = open(os.path.join(arch, f)).read()
    users = len(re.findall(r"^[^\n]*\baudio_(?:tone|play)\b", re.sub(r"^int audio_\w+\(.*$", "", t, flags=re.M), flags=re.M))
    if f != "cmd.c" and users: fails.append(f"{f} calls into audio.c (only cmd.c may, through `beep` and the volume keys)")
    if f == "cmd.c" and users != 1: fails.append("cmd.c must reach audio_tone from exactly one place (its beep helper)")
if not fails: print("  ok: audio.c has no ticks wait and no GPIO set/clear write; only `beep` calls it")

# 2. host harness
HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define AUDIO_HOST_TEST
#define PI_BUILD
#include "arch/arm64/audio.c"
static int mode;
static unsigned cm_ctl, gpfsel4 = 0x040, pup = 0xFFFFFFFF, pwm_ctl = 0xDEAD, rng1, rng2, fifo_len, fifo_n, minv = ~0u, maxv, first[8];
static unsigned long reads;
void kputs(const char *s) { fputs(s, stdout); }
unsigned audio_rd(unsigned long a) {
    reads++;
    if (a == CM_BASE + 0xA0) return (cm_ctl & 0x7F) | (mode != 1 && (cm_ctl & 0x10) ? 0x80 : 0);
    if (a == GPIO_BASE + 0x10) return gpfsel4;
    if (a == GPIO_BASE + 0xE8) return pup;
    if (a == PWM1_BASE + 0x00) return pwm_ctl;
    if (a == PWM1_BASE + 0x04) {
        if (mode == 2) return 1;
        if (fifo_len > (mode == 3 ? 1u : 0u) && (reads & 3) == 0) fifo_len--;
        return (fifo_len >= 16 ? 1 : 0) | (fifo_len == 0 ? 2 : 0);
    }
    fprintf(stderr, "read of %lx\n", a); exit(3);
}
void audio_wr(unsigned long a, unsigned v) {
    if (a == CM_BASE + 0xA0) { if (v >> 24 != 0x5A) exit(4); cm_ctl = v & 0xFFFFFF; }
    else if (a == CM_BASE + 0xA4) { if (v >> 24 != 0x5A) exit(4); }
    else if (a == GPIO_BASE + 0x10) gpfsel4 = v;
    else if (a == GPIO_BASE + 0xE8) pup = v;
    else if (a == PWM1_BASE + 0x00) { pwm_ctl = v; if (v & 0x40) fifo_len = 0; }
    else if (a == PWM1_BASE + 0x04) { }
    else if (a == PWM1_BASE + 0x10) rng1 = v;
    else if (a == PWM1_BASE + 0x20) rng2 = v;
    else if (a == PWM1_BASE + 0x18) {
        if (pwm_ctl != 0x2121 || fifo_len >= 16 || v > rng1) { fprintf(stderr, "bad FIFO write %u ctl %x len %u\n", v, pwm_ctl, fifo_len); exit(5); }
        fifo_len++; if (fifo_n < 8) first[fifo_n] = v; fifo_n++;
        if (v < minv) minv = v;
        if (v > maxv) maxv = v;
    } else { fprintf(stderr, "write of %lx\n", a); exit(6); }
}
int main(int argc, char **argv) {
    mode = atoi(argv[1]);
    int r;
    if (atoi(argv[2]) >= 0) audio_set_volume(atoi(argv[2]));   /* -1: leave the default */
    if (!strcmp(argv[3], "tone")) r = audio_tone(atoi(argv[4]), atoi(argv[5]));
    else {
        static const short s[6] = {-32768, 32767, 0, 0, 100, -100};
        r = audio_play(s, 3, 44100);
    }
    printf("RESULT ret=%d fifo=%u min=%u max=%u range=%u reads=%lu ctl=%x low=%x pin42=%x clk=%x first=%u,%u,%u,%u,%u,%u\n",
           r, fifo_n, minv, maxv, rng1, reads, pwm_ctl, gpfsel4 & 0x3F, gpfsel4 & 0x1C0, cm_ctl & 0x10,
           first[0], first[1], first[2], first[3], first[4], first[5]);
    return 0;
}
'''
if not shutil.which("clang"):
    print("SKIP: clang not installed"); sys.exit(0)
tmp = scratch_dir("audio")
hc, exe = os.path.join(tmp, "harness.c"), os.path.join(tmp, "harness")
open(hc, "w").write(HARNESS)
r = subprocess.run(["clang", "-std=c11", "-Wall", "-Wextra", "-fsanitize=address,undefined", "-I", root, hc, "-o", exe],
                   capture_output=True, text=True)
if r.returncode:
    print("FAIL: the host harness does not build:\n" + r.stderr); sys.exit(1)


def run(mode, *args, vol=100):   # vol 100 keeps the FIFO numbers below at full scale; vol -1 is the build's default
    p = subprocess.run([exe, str(mode), str(vol), *map(str, args)], capture_output=True, text=True, timeout=60)
    if p.returncode: fails.append(f"harness {args} exited {p.returncode}: {p.stderr.strip()}")
    res = re.search(r"RESULT (.*)", p.stdout)
    return p.stdout, dict(kv.split("=") for kv in res.group(1).split()) if res else {}


frames, h = tone_sum(440, 100, 100)
out, d = run(0, "tone", 440, 100)
if f"audio: {frames} frames, ring sum 0x{h:08x}," not in out: fails.append(f"host tone line wrong: {out!r}, wanted {frames} frames sum {h:08x}")
if f"audio: played {frames} frames" not in out: fails.append("host tone did not report playing every frame")
if d.get("ret") != "0" or d.get("fifo") != str(frames * 2): fails.append(f"host tone: ret/fifo wrong {d}")
if d.get("range") != "612" or int(d.get("max", 0)) > 612: fails.append(f"host tone: PWM range or sample scale wrong {d}")
if d.get("ctl") != "0" or d.get("low") != "0" or d.get("clk") != "0": fails.append(f"host tone left the PWM, pins or clock on {d}")
if d.get("pin42") != "40": fails.append(f"host tone changed GPIO 42's function bits {d}")
if int(d.get("reads", 1 << 30)) > 600000: fails.append(f"host tone polled too much {d}")
if not fails: print(f"  ok: a 440 Hz tone of {frames} frames reaches the FIFO in order, ring sum 0x{h:08x} matches the Python tone, pins and clock end off, pin 42 untouched")

frames, h70 = tone_sum(440, 100)
out, d = run(0, "tone", 440, 100, vol=-1)
if f"audio: {frames} frames, ring sum 0x{h70:08x}," not in out or ", volume 70" not in out: fails.append(f"host tone at the default volume is not volume 70's tone: {out!r}")
elif h70 == h: fails.append("volume 70 and volume 100 give the same ring")
else: print(f"  ok: the default volume is 70 and its ring sum 0x{h70:08x} matches the Python tone scaled by 0.70")

out, d = run(0, "tone", 440, 100, vol=0)
if d.get("min") != "306" or d.get("max") != "306" or ", abs 0," not in out: fails.append(f"host volume 0: the FIFO is not flat at the PWM midpoint {d} {out!r}")
else: print("  ok: volume 0 sends only the PWM midpoint (306 of 612): silence, and nothing outside the range")

out, d = run(0, "play")
want = ",".join(str((s + 32768) * 612 >> 16) for s in (-32768, 32767, 0, 0, 100, -100))
if d.get("ret") != "0" or d.get("first") != want: fails.append(f"host stereo play: FIFO order or scaling wrong {d}, wanted first={want}")
else: print(f"  ok: a stereo buffer goes out left then right, scaled into the range ({want})")

for mode, why in ((1, "clock will not start"), (2, "PWM FIFO stays full"), (3, "PWM FIFO never emptied")):
    out, d = run(mode, "tone", 440, 20)
    lines = [l for l in out.splitlines() if l.startswith("audio: ") and why in l]
    if d.get("ret") != "-1" or len(lines) != 1: fails.append(f"host mode {mode}: wanted -1 and one {why!r} line, got {d} {out!r}")
    elif d.get("ctl") != "0" or d.get("low") != "0" or d.get("clk") != "0": fails.append(f"host mode {mode}: hardware left on {d}")
    elif int(d["reads"]) > 6_000_000: fails.append(f"host mode {mode}: too many polls, the wait is not bounded {d}")
    else: print(f"  ok: {why}: returns -1 with one line after {d['reads']} polls and the hardware is switched off")
out, d = run(0, "tone", 5, 100)
if d.get("ret") != "-1" or "audio: tone needs" not in out or d.get("fifo") != "0": fails.append("a 5 Hz tone is not refused")

# 3. QEMU virt, audiotest build
if shutil.which("qemu-system-aarch64") and shutil.which("ld.lld"):
    if subprocess.run(["make", "-C", arch, "audiotest"], capture_output=True).returncode:
        fails.append("`make -C arch/arm64 audiotest` does not build")
    else:
        log = os.path.join(tmp, "uart")
        frames, h = tone_sum(440, 200)   # at the default volume, 70
        want = ["audio: tone 440 Hz 200 ms", f"audio: {frames} frames, ring sum 0x{h:08x}, abs ", "audio: no PWM on this machine"]
        q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-display", "none",
                              "-device", "ramfb", "-serial", "file:" + log, "-kernel", os.path.join(arch, "audio-kernel8.elf")],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        text = ""
        try:
            for _ in range(300):   # at most 30 s
                time.sleep(0.1)
                text = open(log, errors="replace").read() if os.path.exists(log) else ""
                if want[-1] in text: break
        finally:
            q.kill(); q.wait()
        missing = [w for w in want if w not in text]
        if missing: fails.append(f"QEMU: missing {missing}, UART tail {text[-300:]!r}")
        else: print(f"  ok: on QEMU `beep` fills the ring with {frames} frames, ring sum 0x{h:08x} (volume 70) matches the Python tone, and says there is no PWM")
else:
    print("  skip: QEMU part (qemu-system-aarch64 or ld.lld missing)")

for m in fails: print("FAIL: " + m)
if fails: sys.exit(1)
print("PASS: the ARM audio driver's tone reaches the ring and the PWM FIFO with the right checksum, every hardware wait gives up, and the jack stays untouched at boot (the real jack is unverified)")
