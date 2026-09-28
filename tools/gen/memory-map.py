#!/usr/bin/env python3
"""Draw docs/memory-map.svg from the built kernel.elf: where each part of
the kernel sits in memory, and how much room is left before the area
programs load into. The picture behind tools/checks/bss-margin-check.py.

Usage: tools/gen/memory-map.py [kernel.elf]
"""
import os, struct, sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "kernel.elf")
data = open(path, "rb").read()
shoff, = struct.unpack_from("<I", data, 0x20)
shentsize, shnum, shstrndx = struct.unpack_from("<HHH", data, 0x2E)
secs = [struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize) for i in range(shnum)]
strtab = secs[shstrndx][4]
name = lambda s: data[strtab + s[0]:data.index(b"\0", strtab + s[0])].decode()
by = {name(s): (s[3], s[5]) for s in secs if s[3]}
PLAIN = {".text": "code", ".rodata": "fixed data (fonts, images, text)", ".data": "starting values",
         ".bss": "working memory", ".userimg": "where programs load", ".dmabuf": "sound buffer"}
parts = [(n, a, z) for n, (a, z) in sorted(by.items(), key=lambda kv: kv[1][0]) if n in PLAIN and z]
lo = parts[0][1]; hi = max(a + z for _, a, z in parts)
gap = by[".userimg"][0] - (by[".bss"][0] + by[".bss"][1])
W, X0, BAR = 640, 40, 560
rows = []
y = 70
for n, a, z in parts:
    x = X0 + BAR * (a - lo) / (hi - lo); w = max(2, BAR * z / (hi - lo))
    fill = "#2f6b3a" if n in (".userimg", ".dmabuf") else "#8fb996"
    rows.append(f'<rect x="{x:.1f}" y="{y}" width="{w:.1f}" height="18" rx="3" fill="{fill}"/>')
    rows.append(f'<text x="{X0}" y="{y + 36}" font-size="12" fill="#333">{PLAIN[n]}: {z // 1024:,} KB at 0x{a:08X}</text>')
    y += 58
svg = f'''<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{y + 40}" viewBox="0 0 {W} {y + 40}" style="background:#fff;font-family:-apple-system,BlinkMacSystemFont,sans-serif">
<text x="{W // 2}" y="30" font-size="16" font-weight="600" fill="#333" text-anchor="middle">Joshua Tree memory map</text>
<text x="{W // 2}" y="50" font-size="12" fill="#666" text-anchor="middle">Room between working memory and program area: {gap // 1024} KB (the check fails under 16 KB)</text>
{chr(10).join(rows)}
</svg>
'''
out = os.path.join(ROOT, "docs", "memory-map.svg")
open(out, "w").write(svg)
print(f"memory-map: {len(parts)} parts, {gap // 1024}KB free, wrote {out}")
