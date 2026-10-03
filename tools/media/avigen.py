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

out = sys.argv[1]; os.makedirs(out, exist_ok=True)
open(f'{out}/av8.avi', 'wb').write(avi(64, 48, 10, 10, audio=(1, 8, 11025)))
open(f'{out}/av16s.avi', 'wb').write(avi(48, 32, 6, 10, audio=(2, 16, 22050), rec=True))
open(f'{out}/silent.avi', 'wb').write(avi(32, 32, 4, 10))
