#!/usr/bin/env python3
"""The SBC encoder (arch/arm64/sbc.c, A2DP's mandatory codec) makes frames that ffmpeg's own SBC decoder reads back as the tone
that went in: 2 seconds, 1 kHz on the left and 440 Hz on the right, 44100 Hz, bitpool 35.

The C encoder is built with the host cc and a small driver in a scratch directory, so nothing lands in arch/arm64. The frames go
out back to back as a raw .sbc stream, and ffmpeg decodes that to s16le stereo.
  1. ffmpeg decodes the stream with no error and nothing on stderr.
  2. ffprobe reads 44100 Hz and 2 channels.
  3. Every frame is 82 bytes, starts with 0x9C, has the header of 44.1 kHz STEREO with 16 blocks and 8 subbands, and its CRC-8
     (checked here in Python, not with the encoder's code) matches the bytes it covers.
  4. The decoded length is 88320 samples a channel, within 128 (690 frames of 128 samples).
  5. The left channel's dominant frequency is 1000 Hz and the right's is 440 Hz, each within 2 percent (Goertzel, no numpy).
  6. Each channel's SNR against the original tone, after aligning by the lag of best correlation (0 to 300 samples), is above 20 dB.
Discriminating: swap the two channels in sbc.c (the analysis reads `ch` where it should read `1 - ch`) and steps 5 to 8 fail,
because each channel decodes to the other's tone: 440 Hz on the left, 1000 Hz on the right, SNR near -3 dB.
Skips (exit 0) without cc, ffmpeg or ffprobe.
Usage: tools/checks/sbc-encode-check.py   (from the repo root)
"""
import array, math, shutil, subprocess, sys
from pathlib import Path

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
from scratch import scratch_dir
if not (shutil.which("cc") and shutil.which("ffmpeg") and shutil.which("ffprobe")):
    print("SKIP: cc, ffmpeg or ffprobe not installed"); sys.exit(0)

RATE, BITPOOL, FRAMES = 44100, 35, 690          # 690 * 128 = 88320 samples, the 2 seconds padded to a whole frame
FRAME_BYTES = 12 + 2 * BITPOOL                  # header 4, scale factors 8, bits 2 * bitpool
F_LEFT, F_RIGHT, AMP = 1000.0, 440.0, 16384     # half scale, so the codec is not near clipping
WIN_START, FREQ_WIN, SNR_WIN, MAX_LAG = 30000, 8192, 4096, 300

tmp = Path(scratch_dir("sbc-encode"))
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-300:]))
    if not ok: fails.append(name)

DRIVER = r"""
#include <stdio.h>
#include <stdlib.h>
#include "sbc.h"
int main(int argc, char **argv)
{
    if (argc != 5) return 2;
    FILE *in = fopen(argv[1], "rb"), *out = fopen(argv[2], "wb");
    if (!in || !out) return 1;
    struct sbc_enc e;
    sbc_init(&e, (unsigned)strtoul(argv[3], 0, 10), (unsigned)strtoul(argv[4], 0, 10));
    short pcm[256];
    unsigned char frame[128];
    while (fread(pcm, sizeof pcm[0], 256, in) == 256) {
        unsigned len = sbc_encode(&e, pcm, frame);
        if (fwrite(frame, 1, len, out) != len) return 1;
    }
    return fclose(out) || fclose(in) ? 1 : 0;
}
"""
drv_c, drv = tmp / "driver.c", tmp / "sbc-driver"
drv_c.write_text(DRIVER)
cc = subprocess.run(["cc", "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror", "-I", str(root / "arch/arm64"),
                     str(drv_c), str(root / "arch/arm64/sbc.c"), "-o", str(drv)], capture_output=True, text=True)
if cc.returncode != 0:
    print("FAIL: sbc.c does not compile with cc -Wall -Wextra -Werror\n" + cc.stderr[-800:]); sys.exit(1)

# The input: 2 seconds of the two tones, then silence to the end of the last frame, as interleaved s16 little-endian.
n = FRAMES * 128
left = [round(AMP * math.sin(2 * math.pi * F_LEFT * i / RATE)) for i in range(n)]
right = [round(AMP * math.sin(2 * math.pi * F_RIGHT * i / RATE)) for i in range(n)]
for i in range(88200, n):
    left[i] = right[i] = 0
pcm = array.array("h", [0]) * (2 * n)
pcm[0::2] = array.array("h", left)
pcm[1::2] = array.array("h", right)
if sys.byteorder != "little": pcm.byteswap()
raw, sbc_file = tmp / "tone.raw", tmp / "x.sbc"
raw.write_bytes(pcm.tobytes())
run = subprocess.run([str(drv), str(raw), str(sbc_file), str(RATE), str(BITPOOL)], capture_output=True, text=True)
if run.returncode != 0:
    print("FAIL: the encoder driver exited %d" % run.returncode); sys.exit(1)

# 3. Framing and CRC, recomputed here.
def crc8(data):
    crc = 0x0F
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1D) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc
stream = sbc_file.read_bytes()
frames_ok = len(stream) == FRAMES * FRAME_BYTES
bad = None
for k in range(FRAMES):
    f = stream[k * FRAME_BYTES:(k + 1) * FRAME_BYTES]
    if len(f) != FRAME_BYTES or f[0] != 0x9C or f[1] != 0xB9 or f[2] != BITPOOL or crc8(bytes([f[1], f[2]]) + f[4:12]) != f[3]:
        bad = k; break
check("%d frames of %d bytes, each with the 44.1 kHz STEREO header and a valid CRC-8" % (FRAMES, FRAME_BYTES),
      frames_ok and bad is None, (len(stream), bad))

# 1 and 2. ffmpeg's decoder and ffprobe.
dec_raw = tmp / "out.raw"
dec = subprocess.run(["ffmpeg", "-nostdin", "-v", "error", "-y", "-f", "sbc", "-i", str(sbc_file), "-f", "s16le", "-ac", "2",
                      str(dec_raw)], capture_output=True, text=True)
check("ffmpeg decodes the stream without error", dec.returncode == 0 and dec.stderr.strip() == "", dec.stderr)
probe = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "a:0", "-show_entries", "stream=sample_rate,channels",
                        "-of", "csv=p=0", str(sbc_file)], capture_output=True, text=True)
check("ffprobe reads 44100 Hz, 2 channels", probe.stdout.strip() == "44100,2", probe.stdout + probe.stderr)

# 4. The decoded length.
data = dec_raw.read_bytes() if dec_raw.exists() else b""
out = array.array("h")
out.frombytes(data[:len(data) - len(data) % 4])
if sys.byteorder != "little": out.byteswap()
m = len(out) // 2
check("decoded length is %d samples a channel (within 128)" % n, abs(m - n) <= 128, m)
L, R = out[0::2], out[1::2]

def goertzel_power(x, freq):
    c = 2 * math.cos(2 * math.pi * freq / RATE)
    s1 = s2 = 0.0
    for v in x:
        s1, s2 = v + c * s1 - s2, s1
    return s1 * s1 + s2 * s2 - c * s1 * s2

def dominant(x):
    seg = x[WIN_START:WIN_START + FREQ_WIN]
    best = max(range(20, 4001, 25), key=lambda f: goertzel_power(seg, f))
    return max((best + d / 2 for d in range(-50, 51)), key=lambda f: goertzel_power(seg, f))

def snr_and_lag(dec, ref):
    ref_win = ref[WIN_START:WIN_START + SNR_WIN]
    lag = max(range(MAX_LAG + 1), key=lambda g: sum(a * b for a, b in zip(ref_win, dec[WIN_START + g:WIN_START + g + SNR_WIN])))
    seg = dec[WIN_START + lag:WIN_START + lag + SNR_WIN]
    sig = sum(a * a for a in ref_win)
    err = sum((a - b) ** 2 for a, b in zip(ref_win, seg))
    gain = sum(a * b for a, b in zip(ref_win, seg)) / sig
    snr = 10 * math.log10(sig / err) if err else 999.0
    return snr, lag, gain

f_left, f_right = dominant(L), dominant(R)
check("left channel dominant at 1000 Hz (within 2%)", abs(f_left - F_LEFT) <= 0.02 * F_LEFT, "%.1f Hz" % f_left)
check("right channel dominant at 440 Hz (within 2%)", abs(f_right - F_RIGHT) <= 0.02 * F_RIGHT, "%.1f Hz" % f_right)

snr_l, lag_l, gain_l = snr_and_lag(L, left)
snr_r, lag_r, gain_r = snr_and_lag(R, right)
check("left SNR above 20 dB (lag %d, gain %.3f, %.1f dB)" % (lag_l, gain_l, snr_l), snr_l > 20, "%.2f dB" % snr_l)
check("right SNR above 20 dB (lag %d, gain %.3f, %.1f dB)" % (lag_r, gain_r, snr_r), snr_r > 20, "%.2f dB" % snr_r)

if fails: print("FAIL: " + "; ".join(fails)); sys.exit(1)
print("PASS: the SBC encoder's frames decode in ffmpeg to the tones that went in: 1000 and 440 Hz, above 20 dB SNR, valid frames")
