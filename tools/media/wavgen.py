#!/usr/bin/env python3
"""Builds the WAV fixtures the host test reads: wavgen.py OUTDIR."""
import struct, sys, math, os
def wav(ch, bits, rate, frames, tone=440):
    data = bytearray()
    for i in range(frames):
        s = math.sin(2 * math.pi * tone * i / rate)
        for c in range(ch):
            v = s * (0.5 if c == 0 else 0.25)
            data += bytes([int(128 + v * 127)]) if bits == 8 else struct.pack('<h', int(v * 32767))
    fmt = struct.pack('<HHIIHH', 1, ch, rate, rate * ch * bits // 8, ch * bits // 8, bits)
    body = b'WAVE' + b'fmt ' + struct.pack('<I', 16) + fmt + b'data' + struct.pack('<I', len(data)) + bytes(data)
    return b'RIFF' + struct.pack('<I', len(body)) + body
out = sys.argv[1]; os.makedirs(out, exist_ok=True)
for name, args in {'m8': (1, 8, 8000, 800), 'm16': (1, 16, 22050, 2205), 's8': (2, 8, 11025, 1102), 's16': (2, 16, 44100, 4410)}.items():
    open(f'{out}/{name}.wav', 'wb').write(wav(*args))
