#!/usr/bin/env python3
"""Builds the disk image tools/checks/music-check.py boots with: musicfix.py IMG [KEEPDIR] [--set NAME].

A 16MB FAT16 disk (tools/mkdisk.sh) holding three songs, so no media rides in the kernel image:

  ALPHA.WAV   in the root:   440Hz tone, 8-bit mono 8000Hz, 30 s (240KB), long enough that it cannot end on its own during the check
  MUSIC/BRAVO.WAV:           660Hz tone, 16-bit stereo 11025Hz, 12 s (529KB), exercises the downmix
  MUSIC/CHARLIE.MP3:         880Hz, 22050Hz mono, 12 s (a committed fixture), so the MP3 path plays through the real decoder
  MUSIC/ZHUGE.WAV:           a valid WAV of 3.3MB, over the player's 3MB cap, so it must be refused

Needs mtools (mmd, mcopy), the same as tools/checks/ring3burrow-check.py.

`--set NAME` builds a different library, for tools/checks/music-qa-check.py:

  qa     ALPHA.WAV (3 s, 8 kHz), BAD.WAV (not a WAV), EMPTY.WAV (0 bytes), FINAL.WAV (2 s) in the root, and in MUSIC/
         CORRUPT.MP3 (one frame's side info damaged) and DELTA.MP3 (the 12 s fixture seventeen times over: 3:26 at
         22.05 kHz, past the 97 s a 32-bit frames*1000 holds at 44.1 kHz and past 195 s at this rate)
  many   fourteen 1 s songs, so the list scrolls and a shuffle round is long enough to show a repeat
  empty  no songs at all
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


FIXTURES = os.path.join(ROOT, "tools", "media", "fixtures")


def damage_frame(mp3, k):
    """The MP3 with the side info of its k-th frame (the first is 0) overwritten: the header is still good, so a
    header scan counts the frame but the decoder cannot decode it."""
    pos = 0
    if mp3[:3] == b"ID3":
        pos = 10 + ((mp3[6] << 21) | (mp3[7] << 14) | (mp3[8] << 7) | mp3[9])
    br1 = [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320]
    br2 = [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160]
    for n in range(k + 1):
        h = mp3[pos:pos + 4]
        assert len(h) == 4 and h[0] == 0xFF and (h[1] & 0xE0) == 0xE0, "lost sync walking the MP3"
        mpeg1 = bool(h[1] & 0x08)
        br = (br1 if mpeg1 else br2)[h[2] >> 4] * 1000
        sr = ([44100, 48000, 32000] if mpeg1 else [22050, 24000, 16000])[(h[2] >> 2) & 3]
        size = (144 if mpeg1 else 72) * br // sr + ((h[2] >> 1) & 1)
        if n == k:
            out = bytearray(mp3)
            for i in range(4, 14): out[pos + i] = 0xFF
            return bytes(out)
        pos += size
    raise AssertionError("unreachable")


def qa_songs(which):
    tone = open(os.path.join(FIXTURES, "tone880.mp3"), "rb").read()
    if which == "qa":
        return {
            "ALPHA.WAV": ("::", wav(1, 8, 8000, 3, 440)),
            "BAD.WAV": ("::", bytes(range(256)) * 8),
            "EMPTY.WAV": ("::", b""),
            "FINAL.WAV": ("::", wav(1, 8, 8000, 2, 660)),
            "CORRUPT.MP3": ("::MUSIC/", damage_frame(open(os.path.join(FIXTURES, "mono22.mp3"), "rb").read(), 10)),
            "DELTA.MP3": ("::MUSIC/", tone * 17),
        }
    if which == "tag":   # NEVERG~1 is all FAT shows; the title and artist live in the ID3 tag
        def frame(i, t): return i + (len(t.encode()) + 2).to_bytes(4, "big") + b"\0\0" + b"\0" + t.encode() + b"\0"
        body = frame(b"TIT2", "Never Gonna Give You Up") + frame(b"TPE1", "Rick Astley")
        n = len(body)
        tag = b"ID3\x03\x00\x00" + bytes([(n >> 21) & 127, (n >> 14) & 127, (n >> 7) & 127, n & 127]) + body
        return {"NEVERG~1.MP3": ("::MUSIC/", tag + tone)}
    if which == "many":
        return {f"SONG{i:02d}.WAV": ("::", wav(1, 8, 8000, 1, 300 + 20 * i)) for i in range(1, 15)}
    return {}   # empty


def main():
    args = sys.argv[1:]
    which = "std"
    if "--set" in args:
        i = args.index("--set"); which = args[i + 1]; del args[i:i + 2]
    if not args or which not in ("std", "qa", "many", "empty", "tag"):
        sys.exit("usage: musicfix.py IMG [KEEPDIR] [--set std|qa|many|empty|tag]")
    img = args[0]
    subprocess.run(["bash", os.path.join(ROOT, "tools", "mkdisk.sh"), img], check=True, stdout=subprocess.DEVNULL)
    work = args[1] if len(args) > 1 else tempfile.mkdtemp(prefix="jt-musicfix-")
    os.makedirs(work, exist_ok=True)
    subprocess.run(["mmd", "-i", img, "::MUSIC"], check=True)
    if which != "std":
        songs = qa_songs(which)
    else:
        songs = {
            "ALPHA.WAV": ("::", wav(1, 8, 8000, 30, 440)),
            "BRAVO.WAV": ("::MUSIC/", wav(2, 16, 11025, 12, 660)),
            "ZHUGE.WAV": ("::MUSIC/", riff(1, 8, 8000, b"\x80" * 3_300_000)),
        }
        songs["CHARLIE.MP3"] = ("::MUSIC/", open(os.path.join(FIXTURES, "tone880.mp3"), "rb").read())
    for name, (dest, blob) in songs.items():
        path = os.path.join(work, name)
        open(path, "wb").write(blob)
        subprocess.run(["mcopy", "-i", img, path, dest + name], check=True)
        print(f"musicfix: {dest}{name} {len(blob)} bytes")


if __name__ == "__main__":
    main()
