#!/usr/bin/env python3
"""A landing tile must not be older than the code that draws it.

Each tile in landing/shots/ is a still of one app. tools/landing-shots.py records a hash of
that app's sources (and the shared drawing code, and its own script) in landing/shots/sources.json
every time it retakes a tile. This check recomputes the hashes and fails when one differs, which
means the app changed and the tile was not retaken. It also fails on a tile with no entry and an
entry with no tile.

Why hashes and not a retake-and-compare: the Weather and Stocks tiles come from live data, and a
QEMU boot per tile (eleven of them) on a shared runner is the kind of wall-clock wait the suite
already retries. A hash of the sources is a pure function of the tree: no QEMU, no network,
under a second, and it cannot flake.

Fix a failure by running `python3 tools/landing-shots.py <name>` and looking at the new tile.
If the change cannot alter the picture (a comment), `python3 tools/landing-shots.py --record`.

Usage: python3 tools/checks/landing-shots-fresh-check.py   (from anywhere)
"""
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import landing_shots_manifest as M

shots = os.path.join(M.ROOT, "landing", "shots")
tiles = sorted(f[:-5] for f in os.listdir(shots) if f.endswith(".webp"))
have = M.load()
fails = []
for t in tiles:
    if t not in M.PROGRAM:
        fails.append(f"{t}.webp has no source mapping in tools/landing_shots_manifest.py")
    elif t not in have:
        fails.append(f"{t}.webp was never recorded: run tools/landing-shots.py {t}")
    elif have[t] != M.source_hash(t):
        fails.append(f"{t}.webp is stale: {M.PROGRAM[t]}.c, the shared drawing code or the shot script changed. "
                     f"Retake it: python3 tools/landing-shots.py <name>")
for t in M.PROGRAM:
    if t not in tiles: fails.append(f"{t}.webp is mapped but missing from landing/shots/")
for t in have:
    if t not in M.PROGRAM: fails.append(f"sources.json lists {t}, which is not a tile")
if fails:
    print("FAIL:"); [print("  - " + f) for f in fails]; sys.exit(1)
print(f"PASS: all {len(tiles)} landing tiles match the sources that draw them")
