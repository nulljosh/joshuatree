#!/usr/bin/env python3
"""9.2.0: the Pi's world fonts and shaped Hindi stay in step with arch/arm64/lang.c.

tools/gen/gen_world_font.py cuts the Noto and Inter faces to the characters lang.c uses and shapes its Hindi row; it needs
the network and HarfBuzz, so CI does not run it. This check does the cheap half with no dependencies:
1. every character past Latin-1 in the eleven plain rows is on a `covers` line of arch/arm64/world_font.h;
2. the Hindi source row hashes to the value arch/arm64/lang_hi.h was shaped from.
Fails when a word is added or changed in lang.c without running the generator: on the Pi it would draw as an empty box.
Usage: tools/checks/lang-fonts-check.py   (from the repo root)
"""
import hashlib, os, re, sys

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch", "arm64")
src = open(os.path.join(arch, "lang.c"), encoding="utf-8").read()
def strings(block): return [m.replace('\\"', '"') for m in re.findall(r'"((?:[^"\\]|\\.)*)"', block)]
table = src[src.index("L[LANG_COUNT][S_COUNT] = {"):]
table = table[table.index("{") + 1: table.index("\n};")]
hi_block = src[src.index("L_hi_src[S_COUNT] = {"):]
hi = strings(hi_block[: hi_block.index("};")])
font = open(os.path.join(arch, "world_font.h")).read()
covered = {int(h, 16) for line in re.findall(r"/\* covers \w+: ([0-9A-F ]+) \*/", font) for h in line.split()}
missing = sorted({c for s in strings(table) for c in s if ord(c) >= 0x100 and ord(c) not in covered})
fails = 0
if missing: print("  FAIL: lang.c uses characters world_font.h lacks: %s" % " ".join(missing)); fails += 1
else: print("  ok: every character of the plain rows is in world_font.h")
want = hashlib.sha256("\x00".join(hi).encode()).hexdigest()[:16]
have = re.search(r"Source row hash ([0-9a-f]{16})", open(os.path.join(arch, "lang_hi.h")).read())
if not have or have.group(1) != want: print("  FAIL: lang_hi.h was shaped from another Hindi row (%s, now %s)" % (have and have.group(1), want)); fails += 1
else: print("  ok: lang_hi.h is shaped from the Hindi row lang.c has")
if fails: print("FAIL: run uv run --with fonttools --with uharfbuzz python3 tools/gen/gen_world_font.py"); sys.exit(1)
print("PASS: the world fonts cover every word in lang.c and the Hindi is shaped from the current row")
