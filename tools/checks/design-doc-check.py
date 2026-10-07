#!/usr/bin/env python3
"""docs/DESIGN.md states numbers, colours and names; this fails when one of them is not what the source says.

Every `NAME = value` code span in the doc is a fact. FACTS below says, for each NAME, which source file defines it
and how to read it. The check fails (and prints both values) when:

  - a fact in the doc disagrees with the source,
  - a fact in FACTS is missing from the doc (a rule was deleted without the check noticing),
  - the doc states a `NAME = value` that FACTS does not know (a number nobody is checking),
  - a source pattern stops matching (the code was refactored; update the pattern, do not delete the fact),
  - a check named under "Run in the suite" is not in ci-suite.sh, or one named under "Run by hand" is,
  - the doc has an em dash, or the section headings are not the agreed ones, in order.

Same shape as landing-facts-check.py: the doc is the claim, the source is the oracle. Hex colours compare as numbers,
so `#b5502c` and `0x00B5502C` are equal. A few facts are measured over art/icons (text, purple and teal, gradient hue).

No QEMU, no build, under a second. Usage: python3 tools/checks/design-doc-check.py   (from anywhere)
Proof it discriminates: change one value in docs/DESIGN.md or one constant in the source and it prints FAIL with both.
"""
import ast
import colorsys
import glob
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
DOC = os.path.join(ROOT, "docs", "DESIGN.md")
HEADINGS = ["What the OS never does", "Icons", "Type", "Colour and light", "Windows and dock", "Motion",
            "How it is enforced"]


class Gone(Exception):
    """A source pattern no longer matches: the code moved, the fact has to be re-pointed."""


def text(path):
    with open(os.path.join(ROOT, path), errors="replace") as f:
        return f.read()


def rx(path, pattern, group=1, flags=re.M):
    m = re.search(pattern, text(path), flags)
    if not m:
        raise Gone("%s: pattern not found: %s" % (path, pattern))
    return m.group(group) if group is not None else m


def rxs(path, pattern, flags=re.M):
    m = re.search(pattern, text(path), flags)
    if not m:
        raise Gone("%s: pattern not found: %s" % (path, pattern))
    return m.groups()


def func_body(path, header):
    """The text of one C function, from its header line to the first closing brace at column 0."""
    m = re.search(re.escape(header) + r".*?\n\}\n", text(path), re.S)
    if not m:
        raise Gone("%s: function not found: %s" % (path, header))
    return m.group(0)


def hexval(s):
    s = s.strip()
    if s.startswith("#"):
        return int(s[1:], 16)
    if s.lower().startswith("0x"):
        return int(s, 16)
    raise ValueError(s)


def hexs(n):
    return "#%06x" % n


# ---- readers: each returns the source's value as the same kind of thing the doc parser returns ----

def squircle_n():
    return float(rx("tools/gen/restyle_icons.py", r"^SQUIRCLE_N = ([\d.]+)"))


def hl(i):
    return float(rxs("tools/gen/restyle_icons.py", r"^HL_WIDTH, HL_ALPHA, HL_FADE = ([\d.]+), ([\d.]+), ([\d.]+)")[i])


def glyph_margin(i):
    top, bot = rxs("tools/gen/restyle_icons.py", r"keeps out of the top (\d+) and bottom (\d+) units")
    # iconinset samples rows 8 and 118; both must sit inside the bands the doc names.
    r_top, r_bot = rxs("tools/checks/iconinset-check.py", r"^TOP_ROW, BOT_ROW = (\d+), (\d+)")
    if not (int(r_top) < int(top) and 128 - int(r_bot) <= int(bot)):
        raise Gone("iconinset-check.py samples rows %s and %s, outside the %s/%s unit margins" % (r_top, r_bot, top, bot))
    return int(top) if i == 0 else int(bot)


def depth(i):
    return int(rxs("tools/checks/iconlight-check.py", r"^DEPTH_MIN, DEPTH_MAX = (\d+), (\d+)")[i])


def accent():
    """The landing page's --accent, which the Music and Hamurapi tiles (restyle_icons.py) and the Movies art must match.
    Apps keep their own window accents (Epiphany and Windgate are blue today), so those are not read here."""
    want = hexval(rx("landing/index.html", r":root \{[^}]*?--accent:\s*(#[0-9a-fA-F]{6})", flags=re.S))
    seen = {"landing/index.html": want}
    for name in ("music", "hamurabi"):
        seen["restyle_icons.py " + name] = hexval(rx("tools/gen/restyle_icons.py", r'^    "%s": \("(#[0-9A-Fa-f]{6})"' % name))
    seen["art/icons/movies.svg"] = want if hexs(want) in text("art/icons/movies.svg").lower() else 0
    if len(set(seen.values())) != 1:
        return "; ".join("%s %s" % (k, hexs(v)) for k, v in sorted(seen.items()))
    return want


def lexly_blue():
    tile = hexval(rx("tools/gen/import_fleet_icons.py", r'"tonchi": "(#[0-9A-Fa-f]{6})"'))
    svg = text("art/icons/tonchi.svg").lower()
    if hexs(tile) not in svg:
        return "import_fleet_icons.py %s, but art/icons/tonchi.svg does not contain it" % hexs(tile)
    return tile


def fleet_count():
    body = rx("tools/gen/import_fleet_icons.py", r"^FLEET = (\{.*?\})", flags=re.M | re.S)
    return len(ast.literal_eval(re.sub(r"#[^\n]*", "", body)))


def icon_svgs():
    return sorted(glob.glob(os.path.join(ROOT, "art", "icons", "*.svg")))


def icon_files():
    return len(icon_svgs())


def margin_tiles():
    """The icons iconinset-check.py measures the glyph margin on: the ones that carry restyle_icons.py's tile header."""
    return sum(1 for f in icon_svgs() if re.search(r"Tile #[0-9A-Fa-f]{6} -> #[0-9A-Fa-f]{6}", open(f).read()))


def icon_text_elements():
    return sum(len(re.findall(r"<text\b", open(f).read())) for f in icon_svgs())


def hls_of(hexstr):
    r, g, b = [int(hexstr[i:i + 2], 16) / 255 for i in (1, 3, 5)]
    h, l, s = colorsys.rgb_to_hls(r, g, b)
    return h * 360, l, s


def icon_purple_teal():
    bad = set()
    for f in icon_svgs():
        for c in set(re.findall(r"#[0-9a-fA-F]{6}\b", open(f).read())):
            h, l, s = hls_of(c)
            if s > 0.25 and 0.12 < l < 0.9 and (255 <= h <= 320 or 160 <= h <= 200):
                bad.add("%s in %s" % (c.lower(), os.path.basename(f)))
    return len(bad) if not bad else "%d (%s)" % (len(bad), ", ".join(sorted(bad)))


def gradient_hue_spread():
    worst = 0.0
    for f in icon_svgs():
        for m in re.finditer(r"<(linear|radial)Gradient\b([^>]*)>(.*?)</\1Gradient>", open(f).read(), re.S):
            if re.search(r'id="hl"', m.group(2)):
                continue  # the white top lip is not a colour ramp
            hues = [h for h, l, s in (hls_of(c) for c in re.findall(r'stop-color="(#[0-9A-Fa-f]{6})"', m.group(3)))
                    if s > 0.15 and 0.1 < l < 0.92]
            for a in hues:
                for b in hues:
                    worst = max(worst, min(abs(a - b), 360 - abs(a - b)))
    return round(worst, 1)


def dejavu_faces():
    members = re.findall(r"^\s*(TTF_FACE_[A-Z_]+)\b", rx("drivers/ttf.h", r"typedef enum \{(.*?)\}", flags=re.S), re.M)
    members = [m for m in members if m != "TTF_FACE_COUNT"]
    have = 0
    for m in members:
        blob = rx("drivers/ttf.c", r"case %s:\s+return ttf_load\((\w+)\);" % m)
        header = blob[:-len("_data")] + ".h"
        if blob.endswith("_data") and os.path.isfile(os.path.join(ROOT, "drivers", header)):
            have += 1
    return have if have == len(members) else "%d faces in ttf.h, only %d have a header in drivers/" % (len(members), have)


def gui_text_px():
    fam, wt, size = (int(x) for x in rxs("kernel/kernel.c", r"GUI_AA_GLYPH\(c\) \(&editor_glyphs\[\(\((\d+) \* 2 \+ (\d+)\) \* 4 \+ (\d+)\)"))
    families = ast.literal_eval(rx("tools/gen/gen_editor_fonts.py", r"for family in (\(.*?\)):"))
    sizes = ast.literal_eval(rx("tools/gen/gen_editor_fonts.py", r"for size in (\(.*?\)):"))
    if families[fam] != "DejaVuSans" or wt != 0:
        return "the desktop text is %s weight %d, not DejaVuSans regular" % (families[fam], wt)
    return sizes[size]


def mono_apps():
    names = []
    for f in sorted(glob.glob(os.path.join(ROOT, "user", "*.c"))):
        if "jt_mono_draw(" in open(f, errors="replace").read():
            names.append(os.path.basename(f)[:-2])
    return ", ".join(names)


def serif_uses():
    n = 0
    for d in ("kernel", "user", "drivers"):
        for f in glob.glob(os.path.join(ROOT, d, "**", "*"), recursive=True):
            rel = os.path.relpath(f, ROOT)
            if not os.path.isfile(f) or not f.endswith((".c", ".h", ".S")) or rel in ("drivers/ttf.c", "drivers/ttf.h"):
                continue
            s = open(f, errors="replace").read() if os.path.getsize(f) < 3_000_000 else ""
            n += s.count("TTF_FACE_SERIF")
            # editor_glyphs[((family * 2 + bold) * 4 + size) * 95 + ...]: family 0 is Sans, 1 is Serif, 2 is Mono
            n += sum(1 for fam in re.findall(r"editor_glyphs\[\(\((\d+) \*", s) if fam != "0")
    return n


def ui_glyph_range():
    body = func_body("kernel/kernel.c", "static void gui_aa_char(unsigned char c")
    lo, hi = re.search(r"if \(c < (\d+) \|\| c > (\d+)\)", body).groups()
    return "%s-%s" % (lo, hi)


def desktop():
    w, h, s = rxs("kernel/kernel.c", r"&& !window_open_scaled\((\d+), (\d+), 32, (\d+)\)")
    return "%sx%s" % (w, h)


def pixel_scale():
    return int(rxs("kernel/kernel.c", r"&& !window_open_scaled\((\d+), (\d+), 32, (\d+)\)")[2])


def night_hours():
    return "%s-%s" % (rx("kernel/kernel.c", r"hour >= 18 && hour < (\d+)\) night"),
                      rx("kernel/kernel.c", r"hour >= (\d+) && hour < 8\) night"))


def day_hours():
    a, b = rxs("kernel/kernel.c", r"hour >= (\d+) && hour < (\d+)\) day = \d+;")
    return "%s-%s" % (a, b)


def dock_icon():
    w, h, scale = rxs("kernel/kernel.c", r"&& !window_open_scaled\((\d+), (\d+), 32, (\d+)\)")
    pct = int(rx("kernel/kernel.c", r"^int dock_scale_pct = (\d+);"))
    n = int(rx("kernel/dock_geom.h", r"#define GUI_ICON_COUNT\s+(\d+)"))
    d = {k: int(rx("kernel/dock_geom.h", r"#define %s\s+(\d+)" % k)) for k in ("DOCK_BUDGET", "DOCK_GAP", "DOCK_PAD")}
    if "window_height() * dock_scale_pct / 100" not in text("kernel/dock_geom.c"):
        raise Gone("kernel/dock_geom.c: gui_dock_icon no longer sizes from window_height() * dock_scale_pct / 100")
    by_height = int(h) * pct // 100
    budget = min(d["DOCK_BUDGET"], int(w) - 40)
    by_width = (budget - 2 * d["DOCK_PAD"] - (n - 1) * d["DOCK_GAP"]) // n
    return max(16, min(by_height, by_width))


def dock_order():
    k = text("kernel/kernel.c")
    labels = re.findall(r'\{"([^"]+)",', re.search(r"struct app APPS\[GUI_APP_COUNT\] = \{(.*?)\n\};", k, re.S).group(1))
    sym = {"GUI_APPS_FOLDER": labels.index("Apps"), "GUI_TRASH": labels.index("Trash")}
    raw = re.search(r"#define GUI_DOCK_DEFAULT_ORDER \{(.*?)\}", text("kernel/gui_paint.h")).group(1).replace(" ", "").split(",")
    return ", ".join(labels[sym[t] if t in sym else int(t)] for t in raw)


def window_frame(i):
    body = func_body("kernel/gui_paint.c", "void gui_draw_window_frame(")
    body_col, radius = re.search(r"gui_rounded_rect_on_wallpaper\(x, y, w, h, (0x[0-9A-Fa-f]+), (\d+)\)", body).groups()
    rule = re.search(r"gui_hairline_h\(x \+ 8, y \+ 29, w - 16, (0x[0-9A-Fa-f]+)\)", body).group(1)
    close = re.search(r"gui_fill_circle\(x \+ 24, y \+ 16, 7, (0x[0-9A-Fa-f]+)", body).group(1)
    ink = re.search(r"gui_text\(name, .*?, (0x[0-9A-Fa-f]+)\);", body).group(1)
    return [hexval(body_col), int(radius), hexval(rule), hexval(close), hexval(ink)][i]


def menubar_white():
    a, b = rxs("kernel/kernel.c", r"gui_lerp\(gui_wallpaper_color\(row\), 0x00FFFFFF, (\d+), (\d+)\)")
    return int(a) * 100 // int(b)


def tray(i):
    t = text("kernel/gui_paint.c")
    radius = re.search(r"gui_rounded_rect_on_wallpaper\(dock_x, y0, dock_w, dock_h, DOCK_TRAY_COLOR, (\d+)\)", t)
    edge = re.search(r"gui_hairline_h\(dock_x \+ 18, y0, dock_w - 36, (0x[0-9A-Fa-f]+)\)", t)
    rows = re.search(r"rows = (\d+) \* sc;", t)
    if not (radius and edge and rows):
        raise Gone("kernel/gui_paint.c: gui_draw_dock_tray no longer matches the tray patterns")
    return [int(radius.group(1)), hexval(edge.group(1)), int(rows.group(1))][i]


def launchpad(i):
    vals = rxs("tools/checks/launchpad-centered-check.py", r"^MIN_V_MARGIN, MIN_SIDE_MARGIN, MIN_LABEL_GAP = (\d+), (\d+), (\d+)")
    return int(vals[i])


def apps_tile():
    tile = int(rx("kernel/apps_geom.h", r"#define APPS_TILE\s+(\d+)"))
    check = int(rx("tools/checks/launchpad-centered-check.py", r"^NEW_TILE = (\d+)"))
    return tile if tile == check else "%d in apps_geom.h but launchpad-centered-check.py allows %d" % (tile, check)


def ticks_per_second():
    return int(rx("kernel/irq.c", r"^\s+pit_init\((\d+)\);"))


def secs(name):
    return int(rx("user/samantha.c", r"#define %s\s+(\d+)" % name)) / ticks_per_second()


def cap(name):
    return int(rx("user/samantha.c", r"#define %s\s+(\d+)" % name))


def cap_ease():
    return "quadratic out" if re.search(r"rem = u \* u / 256", text("user/samcaps.h")) else "no square found in samcaps.h"


# kind: hex, num, secs, str, max (the doc value is a ceiling the measured value must stay under)
FACTS = {
    "DESKTOP": ("str", desktop, "kernel/kernel.c"),
    "PIXEL_SCALE": ("num", pixel_scale, "kernel/kernel.c"),
    "UI_GLYPH_RANGE": ("str", ui_glyph_range, "kernel/kernel.c"),
    "SQUIRCLE_N": ("num", squircle_n, "tools/gen/restyle_icons.py"),
    "TOL_SQUIRCLE": ("num", lambda: float(rx("tools/checks/iconinset-check.py", r"^TOL_SQUIRCLE = ([\d.]+)")), "tools/checks/iconinset-check.py"),
    "GLYPH_TOP_MARGIN": ("num", lambda: glyph_margin(0), "tools/gen/restyle_icons.py"),
    "GLYPH_BOTTOM_MARGIN": ("num", lambda: glyph_margin(1), "tools/gen/restyle_icons.py"),
    "DEPTH_MIN": ("num", lambda: depth(0), "tools/checks/iconlight-check.py"),
    "DEPTH_MAX": ("num", lambda: depth(1), "tools/checks/iconlight-check.py"),
    "EDGE_MAX": ("num", lambda: int(rx("tools/checks/iconlight-check.py", r"^EDGE_MAX = (\d+)")), "tools/checks/iconlight-check.py"),
    "HL_WIDTH": ("num", lambda: hl(0), "tools/gen/restyle_icons.py"),
    "HL_ALPHA": ("num", lambda: hl(1), "tools/gen/restyle_icons.py"),
    "HL_FADE": ("num", lambda: hl(2), "tools/gen/restyle_icons.py"),
    "GRADIENT_HUE_MAX": ("max", gradient_hue_spread, "art/icons/*.svg"),
    "ICON_FILES": ("num", icon_files, "art/icons/*.svg"),
    "MARGIN_TILES": ("num", margin_tiles, "art/icons/*.svg (the Tile header iconinset-check.py keys on)"),
    "ICON_TEXT_ELEMENTS": ("num", icon_text_elements, "art/icons/*.svg"),
    "ICON_PURPLE_TEAL": ("num", icon_purple_teal, "art/icons/*.svg"),
    "ACCENT": ("hex", accent, "landing/index.html, tools/gen/restyle_icons.py, art/icons/movies.svg"),
    "LEXLY_BLUE": ("hex", lexly_blue, "tools/gen/import_fleet_icons.py and art/icons/tonchi.svg"),
    "FLEET_ICONS": ("num", fleet_count, "tools/gen/import_fleet_icons.py"),
    "DEJAVU_FACES": ("num", dejavu_faces, "drivers/ttf.h, drivers/ttf.c"),
    "GUI_TEXT_PX": ("num", gui_text_px, "kernel/kernel.c, tools/gen/gen_editor_fonts.py"),
    "BODY_PX": ("num", lambda: float(rx("tools/gen/gen_user_text.c", r"#define BODY_PX ([\d.]+)f")), "tools/gen/gen_user_text.c"),
    "DISP_PX": ("num", lambda: float(rx("tools/gen/gen_user_text.c", r"#define DISP_PX ([\d.]+)f")), "tools/gen/gen_user_text.c"),
    "MONO_PX": ("num", lambda: float(rx("tools/gen/gen_user_text.c", r"#define MONO_PX ([\d.]+)f")), "tools/gen/gen_user_text.c"),
    "JT_MONO_ADV": ("num", lambda: int(rx("user/libjt/text.h", r"#define JT_MONO_ADV (\d+)")), "user/libjt/text.h"),
    "MONO_APPS": ("str", mono_apps, "user/*.c (who calls jt_mono_draw)"),
    "SERIF_USES": ("num", serif_uses, "kernel/, user/, drivers/ (TTF_FACE_SERIF, editor_glyphs family 1)"),
    "DEFAULT_WALLPAPER": ("str", lambda: rx("kernel/kernel.c", r"^static int wall_theme = (\w+);"), "kernel/kernel.c"),
    "NIGHT_FLOOR": ("hex", lambda: hexval(rx("kernel/kernel.c", r"return gui_lerp\(rgb, (0x[0-9A-Fa-f]+), night, 100\)")), "kernel/kernel.c"),
    "NIGHT_PCT": ("num", lambda: int(rx("kernel/kernel.c", r"else night = (\d+);")), "kernel/kernel.c"),
    "NIGHT_HOURS": ("str", night_hours, "kernel/kernel.c"),
    "DAY_TINT": ("hex", lambda: hexval(rx("kernel/kernel.c", r"return gui_lerp\(rgb, (0x[0-9A-Fa-f]+), day, 100\)")), "kernel/kernel.c"),
    "DAY_PCT": ("num", lambda: int(rxs("kernel/kernel.c", r"hour >= (\d+) && hour < (\d+)\) day = (\d+);")[2]), "kernel/kernel.c"),
    "DAY_HOURS": ("str", day_hours, "kernel/kernel.c"),
    "GUI_MENUBAR_H": ("num", lambda: int(rx("kernel/kernel.c", r"#define GUI_MENUBAR_H\s+(\d+)")), "kernel/kernel.c"),
    "MENUBAR_WHITE": ("num", menubar_white, "kernel/kernel.c"),
    "MENUBAR_RULE": ("hex", lambda: hexval(rx("kernel/kernel.c", r"gui_hairline_h\(0, GUI_MENUBAR_H - 1, \(int\)window_width\(\), (0x[0-9A-Fa-f]+)\)")), "kernel/kernel.c"),
    "DOCK_TRAY_COLOR": ("hex", lambda: hexval(rx("kernel/dock_geom.h", r"#define DOCK_TRAY_COLOR\s+(0x[0-9A-Fa-f]+)")), "kernel/dock_geom.h"),
    "WINDOW_BODY": ("hex", lambda: window_frame(0), "kernel/gui_paint.c"),
    "WINDOW_RADIUS": ("num", lambda: window_frame(1), "kernel/gui_paint.c"),
    "WINDOW_RULE": ("hex", lambda: window_frame(2), "kernel/gui_paint.c"),
    "CLOSE_DOT": ("hex", lambda: window_frame(3), "kernel/gui_paint.c"),
    "WINDOW_INK": ("hex", lambda: window_frame(4), "kernel/gui_paint.c"),
    "SNAP_EDGE": ("num", lambda: int(rxs("kernel/kernel.c", r"const int corner = (\d+), edge = (\d+);")[1]), "kernel/kernel.c"),
    "SNAP_CORNER": ("num", lambda: int(rxs("kernel/kernel.c", r"const int corner = (\d+), edge = (\d+);")[0]), "kernel/kernel.c"),
    "DOCK_SLOTS": ("num", lambda: int(rx("kernel/dock_geom.h", r"#define GUI_ICON_COUNT\s+(\d+)")), "kernel/dock_geom.h"),
    "DOCK_ORDER": ("str", dock_order, "kernel/kernel.c"),
    "dock_scale_pct": ("num", lambda: int(rx("kernel/kernel.c", r"^int dock_scale_pct = (\d+);")), "kernel/kernel.c"),
    "DOCK_ICON": ("num", dock_icon, "kernel/dock_geom.[ch], kernel/kernel.c"),
    "DOCK_GAP": ("num", lambda: int(rx("kernel/dock_geom.h", r"#define DOCK_GAP\s+(\d+)")), "kernel/dock_geom.h"),
    "DOCK_PAD": ("num", lambda: int(rx("kernel/dock_geom.h", r"#define DOCK_PAD\s+(\d+)")), "kernel/dock_geom.h"),
    "DOCK_MARGIN_BOT": ("num", lambda: int(rx("kernel/dock_geom.h", r"#define DOCK_MARGIN_BOT\s+(\d+)")), "kernel/dock_geom.h"),
    "TRAY_RADIUS": ("num", lambda: tray(0), "kernel/gui_paint.c"),
    "TRAY_EDGE": ("hex", lambda: tray(1), "kernel/gui_paint.c"),
    "TRAY_SHADOW_ROWS": ("num", lambda: tray(2), "kernel/gui_paint.c"),
    "DOCK_LABEL_BG": ("hex", lambda: hexval(rx("kernel/gui_paint.h", r"#define DOCK_LABEL_BG\s+(0x[0-9A-Fa-f]+)")), "kernel/gui_paint.h"),
    "DOCK_LABEL_EDGE": ("hex", lambda: hexval(rx("kernel/gui_paint.h", r"#define DOCK_LABEL_EDGE\s+(0x[0-9A-Fa-f]+)")), "kernel/gui_paint.h"),
    "APPS_COLS": ("num", lambda: int(rx("kernel/apps_geom.h", r"#define APPS_COLS\s+(\d+)")), "kernel/apps_geom.h"),
    "APPS_TILE": ("num", apps_tile, "kernel/apps_geom.h, tools/checks/launchpad-centered-check.py"),
    "LAUNCHPAD_MIN_TOP": ("num", lambda: launchpad(0), "tools/checks/launchpad-centered-check.py"),
    "LAUNCHPAD_MIN_SIDE": ("num", lambda: launchpad(1), "tools/checks/launchpad-centered-check.py"),
    "LABEL_MIN_GAP": ("num", lambda: launchpad(2), "tools/checks/launchpad-centered-check.py"),
    "TICK_HZ": ("num", ticks_per_second, "kernel/irq.c"),
    "CAP_IN": ("secs", lambda: secs("CAP_IN"), "user/samantha.c"),
    "CAP_QUIET": ("secs", lambda: secs("CAP_QUIET"), "user/samantha.c"),
    "CAP_OUT": ("secs", lambda: secs("CAP_OUT"), "user/samantha.c"),
    "CAP_SHOW": ("num", lambda: cap("CAP_SHOW"), "user/samantha.c"),
    "CAP_ALPHA": ("num", lambda: cap("CAP_ALPHA"), "user/samantha.c"),
    "CAP_EASE": ("str", cap_ease, "user/samcaps.h"),
}


def parse_doc(kind, raw):
    raw = raw.strip()
    if kind == "hex":
        return hexval(raw)
    if kind == "secs":
        return float(re.sub(r"\s*s$", "", raw))
    if kind in ("num", "max"):
        return float(raw)
    return raw


def same(kind, doc, src):
    if kind == "max":
        return isinstance(src, (int, float)) and src <= doc
    if kind == "hex":
        return isinstance(src, int) and doc == src
    if kind in ("num", "secs"):
        return isinstance(src, (int, float)) and abs(float(src) - doc) < 1e-9
    return doc == src


def show(kind, v):
    if kind == "hex" and isinstance(v, int):
        return "%s (0x%08X)" % (hexs(v), v)
    return str(v)


def section(doc, title):
    m = re.search(r"^## %s\n(.*?)(?=^## |\Z)" % re.escape(title), doc, re.S | re.M)
    return m.group(1) if m else ""


def suite_checks():
    return set(re.findall(r"tools/checks/([\w.-]+-check\.\w+)", text("tools/checks/ci-suite.sh")))


def main():
    doc = open(DOC, encoding="utf-8").read()
    bad = []

    if "\u2014" in doc:
        bad.append("docs/DESIGN.md has an em dash; the house voice never uses one")
    heads = re.findall(r"^## (.+)$", doc, re.M)
    if heads != HEADINGS:
        bad.append("docs/DESIGN.md sections are %s, want %s" % (heads, HEADINGS))

    stated = {}
    for name, value in re.findall(r"`([A-Za-z][A-Za-z0-9_]*) = ([^`]+)`", doc):
        if name in stated:
            bad.append("%s is stated twice in the doc (%s and %s)" % (name, stated[name], value))
        stated[name] = value

    for name in stated:
        if name not in FACTS:
            bad.append("the doc says `%s = %s` but design-doc-check.py has no source for it" % (name, stated[name]))

    ok = 0
    for name, (kind, read, where) in FACTS.items():
        if name not in stated:
            bad.append("%s is missing from docs/DESIGN.md (source: %s)" % (name, where))
            continue
        try:
            doc_v = parse_doc(kind, stated[name])
        except ValueError:
            bad.append("%s: the doc says %r, which is not a %s" % (name, stated[name], kind))
            continue
        try:
            src_v = read()
        except Gone as e:
            bad.append("%s: %s" % (name, e))
            continue
        if same(kind, doc_v, src_v):
            ok += 1
        else:
            note = ", measured, and it must stay at or under the doc's ceiling" if kind == "max" else ""
            bad.append("%s: the doc says %s, the source says %s (%s%s)" % (
                name, stated[name].strip(), show(kind, src_v), where, note))

    # The checks the doc names must be real, and the CI/manual split must match ci-suite.sh.
    enforced = section(doc, "How it is enforced")
    in_suite = suite_checks()
    for label, want_in in (("Run in the suite on every pull request:", True), ("Run by hand, not in CI yet:", False)):
        m = re.search(re.escape(label) + r"(.*?)(?:\n\n|\Z)", enforced, re.S)
        if not m:
            bad.append("'How it is enforced' has no paragraph starting %r" % label)
            continue
        names = re.findall(r"`([\w.-]+-check\.\w+)`", m.group(1))
        if not names:
            bad.append("%r names no checks" % label)
        for n in names:
            if not os.path.isfile(os.path.join(ROOT, "tools", "checks", n)):
                bad.append("%s is named in the doc but tools/checks/%s does not exist" % (n, n))
            elif (n in in_suite) != want_in:
                bad.append("%s is %s ci-suite.sh but the doc lists it under %r" % (
                    n, "in" if n in in_suite else "not in", label.rstrip(":")))

    if bad:
        print("FAIL: docs/DESIGN.md and the source disagree:")
        for b in bad:
            print("  - " + b)
        return 1
    print("PASS: docs/DESIGN.md matches the source (%d facts, %d checks named)" % (ok, len(re.findall(r"`[\w.-]+-check\.\w+`", enforced))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
