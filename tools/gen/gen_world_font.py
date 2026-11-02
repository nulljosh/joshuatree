#!/usr/bin/env python3
"""9.2.0: the Pi's fonts for the eleven languages past English (arch/arm64/lang.c), and Hindi shaped ahead of time.

Reads every string in arch/arm64/lang.c and writes:
- arch/arm64/world_font.h: five TrueType subsets text.c falls back to for a codepoint Tree Sans lacks. Latin Extended,
  Greek and Vietnamese from Inter 4 at Tree Sans's own instance (opsz 14, wght 400; Tree Sans is cut to Latin-1); Japanese, Chinese and Korean from Noto Sans JP, SC and KR; Hindi from
  Noto Sans Devanagari. Each holds only the characters lang.c uses (a whole CJK face is 10 to 17 MB). A `covers:` line
  lists them for tools/checks/lang-fonts-check.py.
- arch/arm64/lang_hi.h: the Hindi row. Devanagari reorders and joins letters (the i sign goes before its consonant,
  न्द is one conjunct), and the kernel does no shaping, so HarfBuzz shapes each run here and every shaped glyph gets a
  private-use codepoint (U+E000 up) in the Devanagari subset's cmap. The row's hash is kept so the check sees an edit.
Noto is SIL OFL 1.1 (tools/fonts/Noto-OFL.txt). The sources are fetched from google/fonts into ~/.cache/jt-noto.
Run: uv run --with fonttools --with uharfbuzz python3 tools/gen/gen_world_font.py
"""
import hashlib, io, os, re, urllib.request
from fontTools.ttLib import TTFont
from fontTools.subset import Subsetter, Options
from fontTools.varLib.instancer import instantiateVariableFont
from fontTools.ttLib.tables._c_m_a_p import CmapSubtable
import uharfbuzz as hb

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ARCH = os.path.join(ROOT, "arch", "arm64")
CACHE = os.path.expanduser("~/.cache/jt-noto")
NOTO = {"inter": "inter/Inter%5Bopsz,wght%5D.ttf", "jp": "notosansjp/NotoSansJP%5Bwght%5D.ttf", "sc": "notosanssc/NotoSansSC%5Bwght%5D.ttf",
        "kr": "notosanskr/NotoSansKR%5Bwght%5D.ttf", "deva": "notosansdevanagari/NotoSansDevanagari%5Bwdth,wght%5D.ttf"}

def noto(key):
    path = os.path.join(CACHE, key + ".ttf")
    if not os.path.exists(path):
        os.makedirs(CACHE, exist_ok=True)
        urllib.request.urlretrieve("https://github.com/google/fonts/raw/main/ofl/" + NOTO[key], path)
    f = TTFont(path)
    if "fvar" in f: f = instantiateVariableFont(f, {a.axisTag: {"wght": 400, "opsz": 14}.get(a.axisTag, a.defaultValue) for a in f["fvar"].axes})
    return f

def c_strings(block):
    return [m.replace("\\\"", "\"") for m in re.findall(r'"((?:[^"\\]|\\.)*)"', block)]

src = open(os.path.join(ARCH, "lang.c"), encoding="utf-8").read()
table = src[src.index("L[LANG_COUNT][S_COUNT] = {"):]
table = table[table.index("{") + 1: table.index("\n};")]
rows = [c_strings(r) for r in re.findall(r"\{([^{}]*)\}", table)]
hi_block = src[src.index("L_hi_src[S_COUNT] = {"):]
hi = c_strings(hi_block[: hi_block.index("};")])
assert len(rows) == 12 and len(hi) == 28 and all(len(r) in (0, 28) for r in rows), (len(rows), len(hi))

def is_deva(c): return 0x0900 <= ord(c) <= 0x097F or c in "\u200c\u200d"
want = {"latin": set(), "jp": set(), "sc": set(), "kr": set()}
for lang, row in zip(["en", "es", "fr", "de", "pt", "it", "vi", "ja", "zh", "ko", "hi", "el"], rows):
    for s in row:
        for c in s:
            if ord(c) < 0x100: continue
            want["jp" if lang == "ja" else "sc" if lang == "zh" else "kr" if lang == "ko" else "latin"].add(ord(c))
for s in hi:
    for c in s:
        if ord(c) >= 0x100 and not is_deva(c): want["latin"].add(ord(c))   # the danda and the like are Devanagari's

def subset(font, cps=None, gids=None):
    o = Options(); o.layout_features = []; o.hinting = False; o.notdef_outline = True; o.name_IDs = ["*"]; o.drop_tables += ["GSUB", "GPOS", "GDEF", "BASE"]
    s = Subsetter(o)
    if gids is not None: s.populate(glyphs=[font.getGlyphOrder()[g] for g in gids])
    else: s.populate(unicodes=sorted(cps))
    s.subset(font)
    return font

# Hindi: shape each Devanagari run, give every glyph it uses a private-use codepoint.
deva_path = os.path.join(CACHE, "deva-static.ttf")
deva = noto("deva"); deva.save(deva_path)
hbfont = hb.Font(hb.Face(open(deva_path, "rb").read()))
order = deva.getGlyphOrder()
pua, shaped = {}, []
for s in hi:
    out = ""
    for run in re.findall(r"[\u0900-\u097f\u200c\u200d]+|[^\u0900-\u097f\u200c\u200d]+", s):
        if not is_deva(run[0]): out += run; continue
        buf = hb.Buffer(); buf.add_str(run); buf.guess_segment_properties(); hb.shape(hbfont, buf)
        for info in buf.glyph_infos:
            if info.codepoint not in pua: pua[info.codepoint] = 0xE000 + len(pua)
            out += chr(pua[info.codepoint])
    shaped.append(out)
deva_names = {order[g]: cp for g, cp in pua.items()}
deva = subset(deva, gids=[0] + sorted(pua))
cmap = CmapSubtable.newSubtable(4); cmap.platformID, cmap.platEncID, cmap.language = 3, 1, 0
cmap.cmap = {cp: name for name, cp in deva_names.items()}
deva["cmap"].tables = [cmap]

faces = [("latin", subset(noto("inter"), want["latin"]), want["latin"]),
         ("jp", subset(noto("jp"), want["jp"]), want["jp"]), ("sc", subset(noto("sc"), want["sc"]), want["sc"]),
         ("kr", subset(noto("kr"), want["kr"]), want["kr"]), ("deva", deva, set(pua.values()))]

def c_bytes(name, data):
    lines = [", ".join("0x%02x" % b for b in data[i:i + 20]) for i in range(0, len(data), 20)]
    return "static const unsigned char world_%s[%d] = {\n    %s\n};\n" % (name, len(data), ",\n    ".join(lines))

h = ["/* Generated by tools/gen/gen_world_font.py from the strings in lang.c; do not edit. Inter (SIL OFL 1.1) and",
     "   Noto Sans JP, SC, KR and Devanagari (SIL OFL 1.1, tools/fonts/Noto-OFL.txt), each cut to the characters lang.c uses. */"]
for name, font, cps in faces:
    b = io.BytesIO(); font.save(b); data = b.getvalue()
    font = TTFont(io.BytesIO(data)); cm = font.getBestCmap() or {}
    for cp in cps: assert cp in cm, (name, hex(cp))
    h.append("/* covers %s: %s */" % (name, " ".join("%04X" % c for c in sorted(cps))))
    h.append(c_bytes(name, data))
h.append("static const unsigned char *const world_faces[] = { %s };" % ", ".join("world_" + n for n, _, _ in faces))
open(os.path.join(ARCH, "world_font.h"), "w").write("\n".join(h) + "\n")

def c_lit(s): return '"' + "".join(chr(b) if 32 <= b < 127 and chr(b) not in '"\\' else "\\x%02x\"\"" % b for b in s.encode("utf-8")) + '"'
digest = hashlib.sha256("\x00".join(hi).encode()).hexdigest()[:16]
open(os.path.join(ARCH, "lang_hi.h"), "w").write(
    "/* Generated by tools/gen/gen_world_font.py: lang.c's Hindi row, shaped by HarfBuzz into private-use codepoints, one per\n"
    "   glyph of world_deva (world_font.h). Source row hash %s. */\n" % digest +
    "static const char *const L_hi[S_COUNT] = {\n" + ",\n".join("    " + c_lit(s) for s in shaped) + "\n};\n")
print("world_font.h: " + ", ".join("%s %d chars" % (n, len(c)) for n, _, c in faces) + "; lang_hi.h hash " + digest)
