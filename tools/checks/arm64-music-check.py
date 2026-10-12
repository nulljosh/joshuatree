#!/usr/bin/env python3
"""ARM64 M3e: user/music.c, unchanged, plays a WAV and an MP3 from the card on the ARM kernel.

The musictest kernel (the real one, except that JT_SYS_TIME ticks from a fixed date so Music's once-a-second `music: at`
lines come on virt, which has no network clock) on QEMU's virt machine, a virtio keyboard, and the check card
(tools/checks/sdcard.py) as a virtio disk with JT/MUSIC/ALPHA.WAV (440 Hz, 8-bit mono 8000 Hz, 6 s, made by
tools/media/musicfix.py's own generator) and JT/MUSIC/CHARLIE.MP3 (the 880 Hz fixture the i386 music check plays). Through QMP:
  - F1 and "music" in Spotlight, Enter: the kernel starts Music at EL0 in its own window; it lists both songs (its library
    line and, on screen, ink in the first two rows and none in the third);
  - Space plays ALPHA.WAV: its `music: at playing pos=` lines advance about a second a second, never past the frames the
    kernel has fed, so its clock is the sound;
  - n moves to CHARLIE.MP3. The WAV's stream ends with the kernel's `audio: app stream` line, and its ring sum and its sum of
    |sample| match the first N bytes of the file through the kernel's conversion at volume 70 exactly (played <= N);
  - the MP3 decodes at EL0 (minimp3 in floating point, its registers kept across every call and park): its position
    advances and its stream's sums are not zero;
  - F11 lowers the kernel's volume to 60 while it plays (`notice: Volume 60`; the tick is refused, the PWM is the app's);
  - n wraps to ALPHA.WAV again, Esc closes Music (`music: closed`, exit 0) and its stream's sums match the file at volume 60,
    lower per frame than at 70: Music's volume and the kernel's compose;
  - F2 and `vol`: the Terminal answers `volume 60`. No fault, no crash, no oom.
JT_SYS_BRK (Music reads each song whole into malloc'd memory) and JT_SYS_AUDIO are what it rests on. The Pi's DMA path is
not exercised here (virt has no PWM); the real jack is unverified.
Skips (exit 0) when clang, ld.lld, qemu-system-aarch64 or mtools is missing.
Usage: tools/checks/arm64-music-check.py   (from the repo root)
"""
import json, os, re, shutil, socket, struct, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
from sdcard import PART, build_card

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch/arm64")
sys.path.insert(0, os.path.join(root, "tools", "media"))
from musicfix import wav   # the i386 music check's own song generator

if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mmd", "mcopy")):
    print("SKIP: clang, ld.lld, qemu-system-aarch64 or mtools not installed"); sys.exit(0)
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok or not detail else "  (" + str(detail)[-600:] + ")"))
    if not ok: fails.append(name)

def app_sum(data, vol):
    """The kernel's app stream sums over 8-bit unsigned mono bytes: each becomes (b - 128) * 256 scaled by the volume toward
    zero (C division), sent left and right; FNV-1a over each sample's low then high byte, and the sum of |sample|."""
    h, a = 2166136261, 0
    for b in data:
        s = (b - 128) * 256
        v = s * vol // 100 if s >= 0 else -((-s * vol) // 100)
        a += 2 * abs(v)
        for _ in range(2):
            for byte in (v & 0xFF, (v >> 8) & 0xFF): h = ((h ^ byte) * 16777619) & 0xFFFFFFFF
    return h, a & 0xFFFFFFFF

tmp = scratch_dir("arm64-music")

# ---- the Pi's half, on the host: audio.c built PI_BUILD against fake registers, with a DMA engine that takes `step` frames
# of the PWM ring every time the pump looks at its source address and records the words it took. What it heard must be
# silence, then every byte the app queued through the kernel's conversion in order, then silence; the control block must
# point PWM1's DREQ (1) at FIF1 and loop on itself; the CPU must never write the FIFO; and when the stream ends the DMA is
# reset, the PWM and its DMA requests are off and the pins are inputs again, GPIO 42 untouched. ----
HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#define AUDIO_HOST_TEST
#define PI_BUILD
#include "arch/arm64/audio.c"
static unsigned cm_ctl, gpfsel4 = 0x040, pup, pwm_ctl, dmac, dmac_run, dma_cs, cs_run, conblk, dma_en, fifo, resets;
static unsigned long pos, nheard, cap;
static unsigned *heard;
void kputs(const char *s) { fputs(s, stdout); }
unsigned audio_rd(unsigned long a) {
    if (a == CM_BASE + 0xA0) return (cm_ctl & 0x7F) | (cm_ctl & 0x10 ? 0x80 : 0);
    if (a == GPIO_BASE + 0x10) return gpfsel4;
    if (a == GPIO_BASE + 0xE8) return pup;
    if (a == DMA_ENABLE) return dma_en;
    if (a == DMA_REG(0x0C)) {
        if (!(dma_cs & 1)) return 0;
        for (unsigned k = 0; k < 37; k++, pos++) { unsigned i = (unsigned)(pos % PCM_FRAMES) * 2; if (nheard + 2 <= cap) { heard[nheard++] = pcm[i]; heard[nheard++] = pcm[i + 1]; } }
        return BUS(pcm) + (unsigned)(pos % PCM_FRAMES) * 8;
    }
    fprintf(stderr, "read of %lx\n", a); exit(3);
}
void audio_wr(unsigned long a, unsigned v) {
    if (a == CM_BASE + 0xA0 || a == CM_BASE + 0xA4) { if (v >> 24 != 0x5A) exit(4); if (a == CM_BASE + 0xA0) cm_ctl = v & 0xFFFFFF; }
    else if (a == GPIO_BASE + 0x10) gpfsel4 = v;
    else if (a == GPIO_BASE + 0xE8) pup = v;
    else if (a == PWM1_BASE + 0x00) pwm_ctl = v;
    else if (a == PWM1_BASE + 0x04 || a == PWM1_BASE + 0x10 || a == PWM1_BASE + 0x20) { }
    else if (a == PWM1_BASE + 0x08) { dmac = v; if (v) dmac_run = v; }
    else if (a == PWM1_BASE + 0x18) fifo++;
    else if (a == DMA_ENABLE) dma_en = v;
    else if (a == DMA_REG(0x00)) { if (v >> 31) { resets++; dma_cs = 0; } else dma_cs = cs_run = v; }
    else if (a == DMA_REG(0x04)) conblk = v;
    else { fprintf(stderr, "write of %lx\n", a); exit(6); }
}
int main(int argc, char **argv) {
    unsigned n = (unsigned)atoi(argv[1]); int stop_at = atoi(argv[2]);
    unsigned char *buf = malloc(n);
    for (unsigned i = 0; i < n; i++) buf[i] = (unsigned char)(28 + (i * 7) % 200);
    cap = 2UL * (n + 4 * PCM_FRAMES); heard = malloc(cap * sizeof *heard);
    unsigned taken = 0; long pumps = 0;
    while (taken < n && pumps < 100000) { unsigned k = n - taken > 8192 ? 8192 : n - taken; int r = audio_app_queue(buf + taken, k, 8000, taken + k == n); taken += (unsigned)r; if (r < (int)k) { audio_pump(); pumps++; } }
    while (audio_busy() && pumps < 100000) { audio_pump(); if (++pumps == stop_at) audio_app_stop(); }
    FILE *f = fopen(argv[3], "wb"); fwrite(heard, sizeof *heard, nheard, f); fclose(f);
    printf("RESULT ti=%u src_ok=%d dst=%x len=%u loop=%d conblk_ok=%d dmac=%x cs=%x en=%x fifo=%u resets=%u dmac_end=%x ctl_end=%x pins=%x pin42=%x busy=%d range=%u silence=%u pumps=%ld\n",
           dma_cb.ti, dma_cb.src == BUS(pcm), dma_cb.dst, dma_cb.len, dma_cb.next == BUS(&dma_cb), conblk == BUS(&dma_cb), dmac_run, cs_run, dma_en,
           fifo, resets, dmac, pwm_ctl, gpfsel4 & 0x3F, gpfsel4 & 0x1C0, audio_busy(), range, silence, pumps);
    return 0;
}
'''
hc, exe = os.path.join(tmp, "harness.c"), os.path.join(tmp, "harness")
open(hc, "w").write(HARNESS)
r = subprocess.run(["clang", "-std=c11", "-Wall", "-Wextra", "-fsanitize=address,undefined", "-I", root, hc, "-o", exe], capture_output=True, text=True)
if r.returncode: print("FAIL: the Pi DMA host harness does not build:\n" + r.stderr[-2000:]); sys.exit(1)
def word(b, vol=70, rng=27000000 // 8000):
    s = (b - 128) * 256
    v = s * vol // 100 if s >= 0 else -((-s * vol) // 100)
    return (v + 32768) * rng >> 16
def host(n, stop_at):
    out = os.path.join(tmp, "heard.bin")
    p = subprocess.run([exe, str(n), str(stop_at), out], capture_output=True, text=True, timeout=120)
    res = re.search(r"RESULT (.*)", p.stdout)
    d = dict(kv.split("=") for kv in res.group(1).split()) if res else {}
    raw = open(out, "rb").read() if os.path.exists(out) else b""
    words = struct.unpack("<%dI" % (len(raw) // 4), raw)
    return p, d, words, streams_of(p.stdout)
def streams_of(text): return [m.groups() for m in re.finditer(r"audio: app stream (\d+) frames at (\d+) Hz, ring sum 0x([0-9a-f]{8}), abs (\d+), volume (\d+), played (\d+)\n", text)]
def heard_ok(words, data, silence):
    pairs = list(zip(words[0::2], words[1::2]))
    if any(l != r for l, r in pairs): return "left and right differ"
    i = 0
    while i < len(pairs) and pairs[i][0] == silence: i += 1
    want = [word(b) for b in data]
    got = [l for l, _ in pairs[i:i + len(want)]]
    if got != want: return "the sound after the leading silence is not the queued bytes in order (first difference at %s)" % next((k for k, (a, b) in enumerate(zip(got, want)) if a != b), len(got))
    if any(l != silence for l, _ in pairs[i + len(want):]): return "something other than silence after the sound"
    return ""
N = 20000
data = [28 + (i * 7) % 200 for i in range(N)]
p, d, words, st = host(N, -1)
check("host, Pi build: the harness ran clean (no stray register, no sanitizer report)", p.returncode == 0 and d, (p.returncode, p.stderr[-400:]))
if d:
    check("the control block: no wide bursts, PERMAP 1 (PWM1's DREQ), source increment, DEST_DREQ, WAIT_RESP; from the PWM ring at its bus "
          "address into FIF1 (0x7e20c818), the whole ring, then itself again", d["ti"] == str((1 << 26) | (1 << 16) | (1 << 8) | (1 << 6) | (1 << 3))
          and d["src_ok"] == "1" and d["dst"] == "7e20c818" and d["len"] == str(16384 * 8) and d["loop"] == "1" and d["conblk_ok"] == "1", d)
    check("DMA channel 5 enabled and started (CS active, priorities 8), PWM1's DMA requests on at 7 words (DMAC 0x80000707), and the CPU "
          "never wrote the FIFO itself", d["en"] == "20" and d["cs"] == "%x" % ((1 << 28) | (8 << 20) | (8 << 16) | 1) and d["dmac"] == "80000707" and d["fifo"] == "0", d)
    why = heard_ok(words, data, int(d["silence"]))
    check("what the DMA took: silence, then all %d queued bytes as PWM words (range 3375, volume 70) in order, then silence" % N, not why and len(words) > 2 * N, why)
    h, a = app_sum(bytes(data), 70)
    check("the stream's line: %d frames fed and played, its sums the bytes' own" % N, st and st[0][0] == str(N) and st[0][5] == str(N) and st[0][2] == "%08x" % h and st[0][3] == str(a), st)
    check("at its end the channel is reset, DMA requests and the PWM are off, GPIO 40 and 41 are inputs again and GPIO 42's bits are as they were",
          d["resets"] == "2" and d["dmac_end"] == "0" and d["ctl_end"] == "0" and d["pins"] == "0" and d["pin42"] == "40" and d["busy"] == "0", d)
p, d, words, st = host(N, 40)
if d and st:
    fed, played = int(st[0][0]), int(st[0][5])
    why = heard_ok(words, data[:fed], int(d["silence"]))
    check("stop part way: what was queued goes, the %d frames already in the PWM ring play out (%d played) and nothing after them; "
          "the stream ends in at most an eighth of a second more" % (fed, played),
          0 < fed < N and played == fed and not why and int(d["pumps"]) - 40 <= (8000 // 8 + 64) // 37 + 2, (why, st, d.get("pumps")))
else: check("host stop run", False, (p.returncode, p.stderr[-300:], p.stdout[-300:]))

subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
r = subprocess.run(["make", "-C", arch, "musictest"], capture_output=True, text=True, timeout=900)
if r.returncode: print("FAIL: arch/arm64 `make musictest` does not build:\n" + (r.stdout + r.stderr)[-1500:]); sys.exit(1)

# ---- the card: sdcard.py's FAT32 card, with JT/MUSIC/ and two songs put there by mtools, as a Mac would ----
alpha = wav(1, 8, 8000, 6, 440)
pcm = alpha[44:]                                     # the RIFF header is 44 bytes: fmt then data
assert alpha[36:40] == b"data" and struct.unpack_from("<I", alpha, 40)[0] == len(pcm) == 48000
charlie = open(os.path.join(root, "tools", "media", "fixtures", "tone880.mp3"), "rb").read()
card = os.path.join(tmp, "card.img")
build_card(card, False)
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
def mt(*a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *a[1:]], capture_output=True, env=MT)
for name, blob in (("ALPHA.WAV", alpha), ("CHARLIE.MP3", charlie)):
    open(os.path.join(tmp, name), "wb").write(blob)
for args in (("mmd", "::JT"), ("mmd", "::JT/MUSIC"), ("mcopy", os.path.join(tmp, "ALPHA.WAV"), "::JT/MUSIC/ALPHA.WAV"),
             ("mcopy", os.path.join(tmp, "CHARLIE.MP3"), "::JT/MUSIC/CHARLIE.MP3")):
    if mt(*args).returncode: print("FAIL: mtools could not write %r onto the check card" % (args,)); sys.exit(1)

log, sock = os.path.join(tmp, "uart"), os.path.join(tmp, "qmp")
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                      "-nic", "none", "-device", "ramfb", "-device", "virtio-keyboard-device",
                      "-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "virtio-blk-device,drive=card",
                      "-display", "none", "-serial", "file:" + log, "-qmp", "unix:%s,server,nowait" % sock,
                      "-kernel", os.path.join(arch, "music-kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def uart(): return open(log, errors="replace").read() if os.path.exists(log) else ""
def wait_re(pat, secs, after=0):
    end = time.time() + secs
    while time.time() < end:
        m = re.search(pat, uart()[after:])
        if m: return m
        time.sleep(0.05)
    return None
def wait_for(text, secs, after=0): return wait_re(re.escape(text), secs, after)
try:
    if not wait_for("M2 input ready", 60) or not wait_for("sd fat32 partition at block 2048", 20):
        fails.append("the desktop or the card did not come up: %r" % uart()[-600:]); raise SystemExit
    time.sleep(0.5)
    s_ = socket.socket(socket.AF_UNIX); s_.settimeout(20); s_.connect(sock); f = s_.makefile("rw"); f.readline()
    def cmd(c, **a):
        f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    cmd("qmp_capabilities")
    def key(*qcodes): cmd("send-key", keys=[{"type": "qcode", "data": k} for k in qcodes]); time.sleep(0.15)
    def shot():
        path = os.path.join(tmp, "shot%d.ppm" % time.monotonic_ns())
        cmd("screendump", filename=path)
        for _ in range(30):
            if os.path.exists(path) and os.path.getsize(path) >= 800 * 600 * 3: break
            time.sleep(0.1)
        parts = open(path, "rb").read().split(b"\n", 3)
        return int(parts[1].split()[0]), parts[3]
    def ink(img, x, y, w, h): return sum(1 for j in range(y, y + h) for i in range(x, x + w) if max(img[1][(j * img[0] + i) * 3:(j * img[0] + i) * 3 + 3]) < 0x60)
    def streams(text): return [m.groups() for m in re.finditer(r"audio: app stream (\d+) frames at (\d+) Hz, ring sum 0x([0-9a-f]{8}), abs (\d+), volume (\d+), played (\d+)\n", text)]
    def positions(text): return [int(p) for p in re.findall(r"music: at playing pos=(\d+)(?: ahead=\d+)?\n", text)]

    # ---- Spotlight opens Music ----
    n0 = len(uart())
    key("f1")
    if not wait_for("spotlight open", 5, n0): fails.append("F1 did not open Spotlight: %r" % uart()[n0:][-300:]); raise SystemExit
    for k in "music": key(k)
    check("Spotlight lists Music, though it has no dock tile", wait_for("spotlight: music 1\n", 5, n0), uart()[n0:][-300:])
    key("ret")
    m = wait_re(r"M3c window music (\d+)x(\d+) at (\d+),(\d+)\n", 10, n0)
    check("Enter starts user/music.c at EL0 in its own window", m and "M3c app music starts" in uart()[n0:], uart()[n0:][-600:])
    if not m: raise SystemExit
    cw, ch, cx, cy = map(int, m.groups())
    lib = wait_for("music: library n=2\n", 10, n0)
    check("it reads JT/MUSIC/ through readdir and lists both songs", lib, uart()[n0:][-400:])
    wait_for("M3c window music first frame", 5, n0); time.sleep(0.5)
    img = shot()
    rows = [ink(img, cx + 30, cy + 46 + i * 26 + 2, 140, 20) for i in range(3)]
    check("on screen, the list has ink in its first two rows (Alpha, Charlie) and none in the third (%s)" % rows, rows[0] > 20 and rows[1] > 20 and rows[2] == 0, rows)

    # ---- Space plays the WAV; its clock is the sound ----
    n1 = len(uart())
    key("spc")
    check("Space plays ALPHA.WAV", wait_for("music: playing ALPHA.WAV\n", 8, n1), uart()[n1:][-400:])
    wait_re(r"(music: at playing pos=\d+(?: ahead=\d+)?\n(.|\n)*){3}", 8, n1)
    n2 = len(uart())
    key("n")
    m = wait_re(r"audio: app stream \d+ .*\n", 2, n2)
    check("n stops the WAV and the kernel ends its stream with one line within 2 s (the PWM ring holds an eighth of a second)", m, uart()[n2:][-400:])
    pos = positions(uart()[n1:n2])
    st = streams(uart()[n2:])
    if st:
        fed, rate, h, a, vol, played = int(st[0][0]), int(st[0][1]), st[0][2], int(st[0][3]), int(st[0][4]), int(st[0][5])
        steps = [b - a_ for a_, b in zip(pos, pos[1:])]   # the first line lands anywhere inside a second: only later steps are whole ones
        check("the WAV's position lines advance about a second a second (%s ms)" % pos,
              len(pos) >= 3 and all(0 < d <= 1500 for d in steps) and all(700 <= d <= 1300 for d in steps[1:]), (pos, steps))
        check("and never run ahead of the sound: the last one (%d ms) is within what the kernel had played when it stopped (%d frames, %d ms)"
              % (pos[-1] if pos else -1, played, played * 1000 // max(rate, 1)), pos and pos[-1] <= played * 1000 // rate + 5 and played <= fed, st[0])
        wh, wa = app_sum(pcm[:fed], 70)
        check("the WAV stream: %d frames at 8000 Hz fed, ring sum 0x%s and abs %d match the file's first %d bytes at volume 70 exactly"
              % (fed, h, a, fed), rate == 8000 and vol == 70 and 0 < fed < len(pcm) and h == "%08x" % wh and a == wa, (st[0], "%08x" % wh, wa))
        per70 = a / fed
    else: per70 = 0

    # ---- the MP3 decodes at EL0, F11 turns the kernel down ----
    check("the next song is the MP3", wait_for("music: playing CHARLIE.MP3\n", 8, n2), uart()[n2:][-400:])
    wait_re(r"(music: at playing pos=\d+(?: ahead=\d+)?\n(.|\n)*){2}", 8, n2)
    n3 = len(uart())
    key("f11")
    check("F11 lowers the kernel's volume to 60 while it plays", wait_for("notice: Volume 60\n", 4, n3), uart()[n3:][-300:])
    check("and its tick is refused: the PWM is the app's while its stream runs", wait_for("audio: busy, an app is playing\n", 4, n3), uart()[n3:][-300:])
    wait_re(r"music: at playing pos=\d+(?: ahead=\d+)?\n", 4, n3)
    mpos = positions(uart()[n2:])
    n4 = len(uart())
    key("n")
    m = wait_re(r"audio: app stream \d+ .*\n", 2, n4)
    st = streams(uart()[n4:])
    check("the MP3 decodes: its position advances (%s ms) and its stream at 22050 Hz is not silence" % mpos,
          len(mpos) >= 2 and mpos[-1] > mpos[0] and st and st[0][1] == "22050" and int(st[0][0]) > 22050 and int(st[0][3]) > 0, (mpos, st))

    # ---- the WAV again at volume 60, Esc ----
    check("n wraps to ALPHA.WAV again", wait_for("music: playing ALPHA.WAV\n", 8, n4), uart()[n4:][-300:])
    wait_re(r"music: at playing pos=\d+(?: ahead=\d+)?\n(.|\n)*music: at playing pos=\d+(?: ahead=\d+)?\n", 6, n4)
    n5 = len(uart())
    key("esc")
    check("Esc closes Music: it stops its sound and exits 0", wait_for("M3c app music exit 0", 6, n5) and "music: closed\n" in uart()[n5:], uart()[n5:][-300:])
    wait_re(r"audio: app stream \d+ .*\n", 2, n5)
    st = streams(uart()[n5:])
    if st:
        fed, h, a, vol = int(st[0][0]), st[0][2], int(st[0][3]), int(st[0][4])
        wh, wa = app_sum(pcm[:fed], 60)
        check("the WAV again: %d frames fed, its sums match the file at volume 60 exactly, and per frame they are lower than at 70 (%.0f < %.0f)"
              % (fed, a / max(fed, 1), per70), vol == 60 and 0 < fed and h == "%08x" % wh and a == wa and a / fed < per70 * 0.9, (st[0], "%08x" % wh, wa))
    else: check("the WAV's stream ended with its line after Esc", False, uart()[n5:][-300:])

    # ---- the Terminal ----
    n6 = len(uart())
    key("f2")
    if wait_for("terminal open", 6, n6):
        time.sleep(0.4)
        for k in "vol": key(k)
        key("ret")
        check("the Terminal answers: `vol` says volume 60", wait_for("volume 60\n", 6, n6), uart()[n6:][-300:])
        # ---- an app's FP registers are its own: fpkeep, with this kernel wiping the FPU in every call and before every resume ----
        n7 = len(uart())
        for k in "userrun fpkeep": key({" ": "spc"}.get(k, k))
        key("ret")
        m = wait_re(r"M3c app fpkeep exit (\d+), ran (\d+) times\n", 10, n7)
        check("`userrun fpkeep`: q0-q31, FPCR and FPSR came back whole after 40 parks and 40 calls (exit %s, ran %s times)"
              % (m.group(1), m.group(2)) if m else "`userrun fpkeep` ends", m and m.group(1) == "0" and int(m.group(2)) > 40, uart()[n7:][-300:])
    else: check("F2 opens the Terminal", False, uart()[n6:][-300:])
    u = uart()
    after = u.partition("M3c app music starts")[2]
    for bad in ("M3 EL0 fault", "KERNEL CRASH", "oom", "M3c load FAIL", "audio: app sound did not start"):
        check("%r never printed once Music started" % bad, bad not in after, after[-300:])
except SystemExit:
    pass
finally:
    q.kill(); q.wait()
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
if fails:
    for msg in fails: print("FAIL: " + str(msg)[:400])
    sys.exit(1)
print("PASS: on the ARM kernel Spotlight opens the unchanged user/music.c, which lists JT/MUSIC/, plays a WAV whose clock follows the "
      "frames the kernel fed and whose ring sums match the file exactly, decodes an MP3 at EL0 with the FPU, and plays the WAV again "
      "quieter after F11 (Music's volume and the kernel's compose); Esc closes it and the Terminal answers (the real jack is unverified)")
