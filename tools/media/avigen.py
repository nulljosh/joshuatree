#!/usr/bin/env python3
"""Builds the AVI fixtures for the host test: avigen.py OUTDIR. MJPG video from PIL, PCM audio,
written by hand so the test needs no ffmpeg."""
import io, os, struct, sys, math
from PIL import Image

def chunk(cid, data):
    return cid + struct.pack('<I', len(data)) + data + (b'\0' if len(data) & 1 else b'')
def lst(kind, body):
    return b'LIST' + struct.pack('<I', len(body) + 4) + kind + body

def frame(i, w, h):
    im = Image.new('RGB', (w, h))
    px = im.load()
    for y in range(h):
        for x in range(w):
            px[x, y] = ((x * 4 + i * 20) & 255, (y * 4) & 255, (i * 40) & 255)
    b = io.BytesIO(); im.save(b, 'JPEG', quality=80); return b.getvalue()

def avi(w, h, nframes, fps, audio=None, rec=False):
    us = 1000000 // fps
    streams = [chunk(b'strh', b'vids' + b'MJPG' + struct.pack('<IHHIIIIIIIIhhhh', 0, 0, 0, 0, 1, fps, 0, nframes, 0, 0, 0, 0, 0, w, h))
               + chunk(b'strf', struct.pack('<IiiHHIIiiII', 40, w, h, 1, 24, 0x47504A4D, w * h * 3, 0, 0, 0, 0))]
    hdrl = lst(b'strl', streams[0])
    if audio:
        ch, bits, rate = audio
        hdrl += lst(b'strl', chunk(b'strh', b'auds' + b'\0\0\0\0' + struct.pack('<IHHIIIIIIIIhhhh', 0, 0, 0, 0, 1, rate, 0, 0, 0, 0, 0, 0, 0, 0, 0))
                    + chunk(b'strf', struct.pack('<HHIIHHH', 1, ch, rate, rate * ch * bits // 8, ch * bits // 8, bits, 0)))
    avih = chunk(b'avih', struct.pack('<IIIIIIIIIIIIII', us, 0, 0, 0x10, nframes, 0, 2 if audio else 1, 0, w, h, 0, 0, 0, 0))
    movi = b''
    per = (audio[2] // fps) if audio else 0
    for i in range(nframes):
        group = chunk(b'00dc', frame(i, w, h))
        if audio:
            ch, bits, rate = audio
            pcm = bytearray()
            for k in range(per):
                v = math.sin(2 * math.pi * 440 * (i * per + k) / rate)
                for _ in range(ch):
                    pcm += bytes([int(128 + v * 100)]) if bits == 8 else struct.pack('<h', int(v * 20000))
            group += chunk(b'01wb', bytes(pcm))
        if i == 0 and not rec: group += chunk(b'02wb', b'\x55' * 100)   # a stream the file never declared
        movi += lst(b'rec ', group) if rec else group
    body = b'AVI ' + lst(b'hdrl', avih + hdrl) + chunk(b'JUNK', b'\0' * 7) + lst(b'movi', movi) + chunk(b'idx1', b'\0' * 16)
    return b'RIFF' + struct.pack('<I', len(body)) + body

def bitframe(i, w, h):
    """Frame i for the Movies check: six big black/white squares spell i in binary, left to right, most significant
    first, in the middle of a mid-grey picture with a dark border. Flat blocks survive JPEG and scaling."""
    im = Image.new('RGB', (w, h), (96, 96, 96))
    px = im.load()
    sq = w // 8
    for bit in range(6):
        on = (i >> (5 - bit)) & 1
        x0 = sq // 2 + bit * sq + (w - 7 * sq) // 2
        for y in range(h // 2 - sq // 2, h // 2 + sq // 2):
            for x in range(x0, x0 + sq - 2):
                px[x, y] = (255, 255, 255) if on else (0, 0, 0)
    for x in range(w):
        for y in (0, 1, h - 2, h - 1): px[x, y] = (181, 80, 44)
    b = io.BytesIO(); im.save(b, 'JPEG', quality=92, subsampling=0); return b.getvalue()

def movie_avi(w, h, nframes, fps, rate, kind='ok'):
    """MJPG video plus 8-bit mono PCM at `rate`: a 440 Hz tone. kind 'garbage' keeps the container valid but fills every
    frame with noise, so the player has to notice the frames do not decode."""
    us = 1000000 // fps
    strl_v = lst(b'strl', chunk(b'strh', b'vids' + b'MJPG' + struct.pack('<IHHIIIIIIIIhhhh', 0, 0, 0, 0, 1, fps, 0, nframes, 0, 0, 0, 0, 0, w, h))
                 + chunk(b'strf', struct.pack('<IiiHHIIiiII', 40, w, h, 1, 24, 0x47504A4D, w * h * 3, 0, 0, 0, 0)))
    strl_a = lst(b'strl', chunk(b'strh', b'auds' + b'\0\0\0\0' + struct.pack('<IHHIIIIIIIIhhhh', 0, 0, 0, 0, 1, rate, 0, 0, 0, 0, 0, 0, 0, 0, 0))
                 + chunk(b'strf', struct.pack('<HHIIHHH', 1, 1, rate, rate, 1, 8, 0)))
    avih = chunk(b'avih', struct.pack('<IIIIIIIIIIIIII', us, 0, 0, 0x10, nframes, 0, 2, 0, w, h, 0, 0, 0, 0))
    movi = b''
    per = rate // fps
    for i in range(nframes):
        if kind == 'garbage':
            jpg = b'\xff\xd8\xff\xe0' + bytes((i * 31 + k * 17) & 255 for k in range(900))
        else:
            jpg = bitframe(i, w, h)
        pcm = bytes(int(128 + 100 * math.sin(2 * math.pi * 440 * (i * per + k) / rate)) for k in range(per))
        movi += chunk(b'00dc', jpg) + chunk(b'01wb', pcm)
    body = b'AVI ' + lst(b'hdrl', avih + strl_v + strl_a) + lst(b'movi', movi)
    return b'RIFF' + struct.pack('<I', len(body)) + body

out = sys.argv[1]; os.makedirs(out, exist_ok=True)
if '--movie' in sys.argv:
    # the Movies check's disk: a 2 s clip at 20 fps, a damaged one, a non-AVI, and one over the 6 MB cap
    open(f'{out}/CLIP.AVI', 'wb').write(movie_avi(160, 120, 40, 20, 11025))
    open(f'{out}/BAD.AVI', 'wb').write(movie_avi(160, 120, 10, 20, 11025, 'garbage'))
    open(f'{out}/JUNK.AVI', 'wb').write(bytes((k * 7 + 3) & 255 for k in range(4000)))
    big = movie_avi(160, 120, 4, 20, 11025)
    pad = chunk(b'JUNK', b'\0' * (6 * 1024 * 1024 + 1000))
    big = b'RIFF' + struct.pack('<I', len(big) - 8 + len(pad)) + big[8:] + pad
    open(f'{out}/BIG.AVI', 'wb').write(big)
    sys.exit(0)
open(f'{out}/av8.avi', 'wb').write(avi(64, 48, 10, 10, audio=(1, 8, 11025)))
open(f'{out}/av16s.avi', 'wb').write(avi(48, 32, 6, 10, audio=(2, 16, 22050), rec=True))
open(f'{out}/silent.avi', 'wb').write(avi(32, 32, 4, 10))
