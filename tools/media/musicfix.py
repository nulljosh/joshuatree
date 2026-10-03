#!/usr/bin/env python3
"""Builds the disk image tools/checks/music-check.py boots with: musicfix.py IMG [KEEPDIR].

A 16MB FAT16 disk (tools/mkdisk.sh) holding three songs, so no media rides in the kernel image:

  ALPHA.WAV   in the root:   440Hz tone, 8-bit mono 8000Hz, 14 s (112KB)
  MUSIC/BRAVO.WAV:           660Hz tone, 16-bit stereo 11025Hz, 12 s (529KB), exercises the downmix
  MUSIC/ZHUGE.WAV:           a valid WAV of 3.3MB, over the player's 3MB cap, so it must be refused

Needs mtools (mmd, mcopy), the same as tools/checks/ring3burrow-check.py.
"""
import math, os, struct, subprocess, sys, tempfile

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")


def wav(ch, bits, rate, secs, tone):
    n = rate * secs
    data = bytearray()
    for i in range(n):
        s = math.sin(2 * math.pi * tone * i / rate) * 0.6
        for _ in range(ch):
            data += bytes([int(128 + s * 127)]) if bits == 8 else struct.pack("<h", int(s * 32767))
    return riff(ch, bits, rate, bytes(data))


def riff(ch, bits, rate, data):
    fmt = struct.pack("<HHIIHH", 1, ch, rate, rate * ch * bits // 8, ch * bits // 8, bits)
    body = b"WAVE" + b"fmt " + struct.pack("<I", 16) + fmt + b"data" + struct.pack("<I", len(data)) + data
    return b"RIFF" + struct.pack("<I", len(body)) + body


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: musicfix.py IMG [KEEPDIR]")
    img = sys.argv[1]
    subprocess.run(["bash", os.path.join(ROOT, "tools", "mkdisk.sh"), img], check=True, stdout=subprocess.DEVNULL)
    songs = {
        "ALPHA.WAV": ("::", wav(1, 8, 8000, 14, 440)),
        "BRAVO.WAV": ("::MUSIC/", wav(2, 16, 11025, 12, 660)),
        "ZHUGE.WAV": ("::MUSIC/", riff(1, 8, 8000, b"\x80" * 3_300_000)),
    }
    work = sys.argv[2] if len(sys.argv) > 2 else tempfile.mkdtemp(prefix="jt-musicfix-")
    os.makedirs(work, exist_ok=True)
    subprocess.run(["mmd", "-i", img, "::MUSIC"], check=True)
    for name, (dest, blob) in songs.items():
        path = os.path.join(work, name)
        open(path, "wb").write(blob)
        subprocess.run(["mcopy", "-i", img, path, dest + name], check=True)
        print(f"musicfix: {dest}{name} {len(blob)} bytes")


if __name__ == "__main__":
    main()
