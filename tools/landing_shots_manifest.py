"""Which source files draw each landing tile, and a hash of them.

tools/landing-shots.py writes landing/shots/sources.json after it retakes a tile.
tools/checks/landing-shots-fresh-check.py fails when the hash of an app's sources no
longer matches, which means the app changed and its tile was not retaken.
"""
import hashlib, json, os

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
MANIFEST = os.path.join(ROOT, "landing", "shots", "sources.json")

# tile file stem -> the user program that draws it
PROGRAM = {
    "app-notes": "notes", "app-calendar": "calendar", "app-weather": "weather", "app-files": "burrow",
    "app-terminal": "terminal", "app-stocks": "stocks", "samantha-chat": "samantha",
    "app-bookrank": "bookrank", "app-tonchi": "tonchi", "app-curbfind": "curbfind", "app-epiphany": "epiphany",
}
# Shared drawing code every program links in. Changing it can change every tile.
SHARED = ["user/jtsys.h", "user/libjt/text.c", "user/libjt/text.h", "user/libjt/aafont.h",
          "user/libjt/stdio.c", "user/libjt/stdio.h", "user/libjt/string.c", "user/libjt/stdlib.c"]
# The shot script itself: a new crop, size or clock changes every tile.
SCRIPT = "tools/landing-shots.py"


def sources(tile):
    return [f"user/{PROGRAM[tile]}.c"] + SHARED


def source_hash(tile):
    h = hashlib.sha256()
    for rel in sources(tile) + [SCRIPT]:
        h.update(rel.encode() + b"\0")
        with open(os.path.join(ROOT, rel), "rb") as f:
            h.update(f.read())
    return h.hexdigest()[:16]


def load():
    try:
        with open(MANIFEST) as f:
            return json.load(f)
    except FileNotFoundError:
        return {}


def record(tile):
    m = load(); m[tile] = source_hash(tile)
    with open(MANIFEST, "w") as f:
        json.dump(dict(sorted(m.items())), f, indent=2); f.write("\n")
