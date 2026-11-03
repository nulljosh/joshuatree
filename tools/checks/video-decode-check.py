#!/usr/bin/env python3
"""13.4.0, Blackcomb's own video decoders (user/libjt/mp4.c, h264.c with h264_cabac.c, aac.c), on the host against ffmpeg.

ffmpeg makes three clips and decodes them for reference; our code must match:
  1. H.264 Constrained Baseline (CAVLC), 640x360 (cropped from 368), rotating content, four reference frames, deblocking:
     every frame byte for byte.
  2. H.264 Main profile with CABAC and no B frames, the shape of YouTube's 360p file: every frame byte for byte.
  3. AAC-LC stereo at 44.1 kHz, a chord with noise: within one sample step of ffmpeg everywhere, over 60 dB, once the
     edit list's encoder warm-up (1024 samples) is dropped, as ffmpeg drops it.
mp4.c's sample tables feed all three. Skips (exit 0) when clang or ffmpeg is missing.
Usage: tools/checks/video-decode-check.py   (from the repo root)
"""
import math, shutil, struct, subprocess, sys
from pathlib import Path
from scratch import scratch_dir

ROOT = Path(__file__).resolve().parents[2]
LIB = ROOT / "user/libjt"
TMP = Path(scratch_dir("video-decode"))
if not all(shutil.which(t) for t in ("clang", "ffmpeg")):
    print("SKIP: clang or ffmpeg not installed"); sys.exit(0)

HARNESS = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mp4.h"
#include "h264.h"
#include "aac.h"
static unsigned char b[1 << 24];
static void *al(unsigned long n) { return calloc(1, n); }
int main(int argc, char **v) {
    if (argc < 4) return 2;
    FILE *f = fopen(v[2], "rb"); unsigned n = (unsigned)fread(b, 1, sizeof b, f); fclose(f);
    FILE *o = fopen(v[3], "wb");
    struct mp4 m; if (mp4_open(&m, b, n)) { puts("mp4 refused"); return 1; }
    struct mp4_iter it; unsigned long long off, dts; unsigned sz;
    if (!strcmp(v[1], "video")) {
        struct h264 *d = h264_new(al);
        const unsigned char *a = m.video.cfg, *p = a + 6; unsigned ns = a[5] & 31;
        for (unsigned i = 0; i < ns; i++) { unsigned l = p[0] << 8 | p[1]; if (h264_nal(d, p + 2, l) < 0) { printf("sps: %s\n", h264_error(d)); return 1; } p += 2 + l; }
        unsigned np = *p++;
        for (unsigned i = 0; i < np; i++) { unsigned l = p[0] << 8 | p[1]; if (h264_nal(d, p + 2, l) < 0) { printf("pps: %s\n", h264_error(d)); return 1; } p += 2 + l; }
        mp4_iter_init(&it, &m.video);
        while (mp4_iter_next(&it, &off, &sz, &dts)) {
            unsigned char *q = b + off, *e = q + sz;
            while (e - q >= 4) { unsigned l = (unsigned)q[0] << 24 | q[1] << 16 | q[2] << 8 | q[3]; q += 4; if (h264_nal(d, q, l) < 0) { printf("slice: %s\n", h264_error(d)); return 1; } q += l; }
            struct h264_pic pc;
            if (h264_end(d, &pc)) {
                for (unsigned y = 0; y < pc.h; y++) fwrite(pc.y + (pc.y0 + y) * pc.stride + pc.x, 1, pc.w, o);
                for (unsigned y = 0; y < pc.h / 2; y++) fwrite(pc.u + (pc.y0 / 2 + y) * pc.cstride + pc.x / 2, 1, pc.w / 2, o);
                for (unsigned y = 0; y < pc.h / 2; y++) fwrite(pc.v + (pc.y0 / 2 + y) * pc.cstride + pc.x / 2, 1, pc.w / 2, o);
            }
        }
    } else {
        static struct aac a; short pcm[2048];
        if (aac_init(&a, m.audio.cfg, m.audio.cfglen)) { printf("aac: %s\n", a.err); return 1; }
        mp4_iter_init(&it, &m.audio);
        unsigned skip = m.audio.skip;   /* the edit list's warm-up, as a player drops it */
        while (mp4_iter_next(&it, &off, &sz, &dts)) {
            if (aac_frame(&a, b + off, sz, pcm) < 0) { printf("aac: %s\n", a.err); return 1; }
            unsigned from = skip > 1024 ? 1024 : skip; skip -= from;
            fwrite(pcm + from * (unsigned)a.channels, 2, (1024 - from) * (unsigned)a.channels, o);
        }
    }
    fclose(o); return 0;
}
'''
(TMP / "h.c").write_text(HARNESS)
exe = TMP / "h"
r = subprocess.run(["clang", "-O2", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-I", str(LIB), str(TMP / "h.c"),
                    str(LIB / "mp4.c"), str(LIB / "h264.c"), str(LIB / "aac.c"), "-o", str(exe), "-lm"], capture_output=True, text=True)
assert r.returncode == 0, r.stderr[-1500:]
ff = lambda *a: subprocess.run(["ffmpeg", "-v", "error", "-y", *a], check=True)
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + str(detail)[-300:]), flush=True)
    if not ok: fails.append(name)

def video(name, args, w, h):
    clip, ours, ref = TMP / (name + ".mp4"), TMP / (name + ".ours"), TMP / (name + ".ref")
    ff("-f", "lavfi", "-i", "mandelbrot=size=%dx%d:rate=24" % (w, h), "-f", "lavfi", "-i", "testsrc2=size=%dx%d:rate=24" % (w, h),
       "-filter_complex", "[0][1]blend=all_mode=average,rotate=t*0.3", "-t", "3", "-pix_fmt", "yuv420p", "-c:v", "libx264", *args, "-an", str(clip))
    ff("-i", str(clip), "-f", "rawvideo", "-pix_fmt", "yuv420p", str(ref))
    out = subprocess.run([str(exe), "video", str(clip), str(ours)], capture_output=True, text=True, timeout=120)
    a, b = ours.read_bytes() if ours.exists() else b"", ref.read_bytes()
    check("%s: %d frames, every one byte for byte" % (name, len(b) // (w * h * 3 // 2)), out.returncode == 0 and a == b, out.stdout + " sizes %d %d" % (len(a), len(b)))

video("baseline", ["-profile:v", "baseline", "-x264-params", "ref=4"], 640, 360)
video("main-cabac", ["-profile:v", "main", "-bf", "0", "-x264-params", "ref=3"], 640, 360)

clip, ours, ref = TMP / "sound.mp4", TMP / "sound.ours", TMP / "sound.ref"
ff("-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100", "-f", "lavfi", "-i", "sine=frequency=659:sample_rate=44100",
   "-f", "lavfi", "-i", "anoisesrc=color=pink:amplitude=0.05:sample_rate=44100", "-filter_complex", "[0][1][2]amix=inputs=3,pan=stereo|c0=c0|c1=0.6*c0",
   "-t", "6", "-c:a", "aac", "-b:a", "128k", str(clip))
ff("-i", str(clip), "-f", "s16le", "-ac", "2", str(ref))
out = subprocess.run([str(exe), "audio", str(clip), str(ours)], capture_output=True, text=True, timeout=120)
a, b = (ours.read_bytes() if ours.exists() else b""), ref.read_bytes()
n = min(len(a), len(b)) // 2
A, B = struct.unpack("<%dh" % n, a[:2 * n]), struct.unpack("<%dh" % n, b[:2 * n])
err = sum((x - y) ** 2 for x, y in zip(A, B)); sig = sum(y * y for y in B) or 1; worst = max((abs(x - y) for x, y in zip(A, B)), default=99)
snr = 10 * math.log10(sig / max(err, 1))
check("AAC-LC stereo: %.1f s, %.1f dB against ffmpeg, never more than %d apart" % (n / 2 / 44100, snr, worst),
      out.returncode == 0 and abs(len(a) - len(b)) <= 4096 and snr > 60 and worst <= 1, out.stdout)
if fails: print("FAIL: %d of the video decode checks failed" % len(fails)); sys.exit(1)
print("PASS: our MP4 reader, H.264 (Baseline and Main with CABAC) and AAC-LC decoders match ffmpeg on fresh clips", flush=True)
