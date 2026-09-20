#!/usr/bin/env bash
# Regenerate progress.svg.
#
# v51: real rewrite, not a tweak, direct and repeated feedback ("still
# unimpressive... more accurate and relevant, not just how many features
# shipped, now we're focusing on polish"). The old version plotted
# cumulative checked-off roadmap items per tagged version. Two real
# problems with that, not just a styling complaint: git tags stopped
# being cut after jt-v27 (confirmed: `git tag -l 'jt-v*'` has nothing past
# v27, though roadmap.md is well past v50), so everything after that had
# no real per-version data to plot, only a live re-parse of roadmap.md's
# current (post-prune) state; and a checked-item count actively
# undersells a polish phase, a version that fixes three real bugs and
# ships zero new checkboxes looks like a flat line even though real work
# happened.
#
# Real fix: plot actual lines of hand-authored kernel/driver code over
# real commit history instead, computed from `git log --numstat`, which
# never depends on tagging discipline and never flattens during a polish
# pass (a bug fix still changes real lines). EXCLUDE_BASENAMES/PREFIX
# below strip the embedded data blobs (wallpaper.h's raw RGB dump,
# editor_fonts.h's raster glyph tables, the ported-app HTML byte arrays)
# that would otherwise pad the number with content nobody wrote by hand,
# a first draft of this script that included them briefly put the total
# near 90,000 and made "written from nothing" read as a rounding error.
#
# A second series, rewritten twice now. First attempt (comment/blank-line
# density %) got real, repeated pushback: Joshua kept reading the line as
# "how much of the codebase is documented" (a real, true, already-100%
# fact: every real file has a row in docs/ARCHITECTURE.md) when it was
# actually plotting something else entirely (comment density inside the
# code, a genuine but different metric). Relabeling it wasn't enough,
# the substance was wrong for what he wanted shown. Real fix: the second
# series now plots the real thing, % of real source files that have an
# actual documented row in docs/ARCHITECTURE.md at that commit (a file's
# basename found in the doc's own text), not comment density. Before
# docs/ARCHITECTURE.md existed, this is honestly 0%, that's real history,
# not a bug.
#
# v53: fixed a real layout bug ("Make it less funny looking" -- the
# rotated y-axis title collided with the tick numbers, and the x-axis
# date labels crowded together at the right edge) by giving the title
# and the tick-number column real separate space (dynamic, sized off
# max_v's own digit count, not a fixed guess) and by no longer force-
# showing the final date label when it collides with the one before it
# (replace it instead of stacking both). While verifying the fix against
# a real render, found a second real bug the same pass surfaced: commit
# 6c169c0 ("icons: the remaining thirteen...") temporarily added ~53,000
# lines to kernel/icon_art.h, a GENERATED PNG-byte data blob (the file's
# own header says "do not edit"), before the very next commit shrank it
# back down. Since max_v was read straight off the final sampled point,
# that intermediate spike rendered off the top of the chart entirely,
# real visual breakage, not just an axis-label collision. icon_art.h is
# exactly the class of file EXCLUDE_BASENAMES already exists to strip
# (same shape as wallpaper.h/editor_fonts.h/vgafont.h, a generated data
# table, not hand-authored code); added to the set instead of writing a
# one-off special case for the spike.
set -euo pipefail
cd "$(dirname "$0")/../.."

python3 << 'PYEOF'
import subprocess, re

EXCLUDE_DIRS = ("node_modules/", ".claude/", "landing/v86/")
EXCLUDE_BASENAMES = {"wallpaper.h", "editor_fonts.h", "vgafont.h", "icon_art.h"}
EXCLUDE_PREFIX = ("drivers/app_",)

def counts_as_real(path):
    if any(path.startswith(d) for d in EXCLUDE_DIRS):
        return False
    if not path.endswith((".c", ".h", ".S")):
        return False
    base = path.rsplit("/", 1)[-1]
    if base in EXCLUDE_BASENAMES:
        return False
    if any(path.startswith(p) for p in EXCLUDE_PREFIX):
        return False
    return True

log = subprocess.run(
    ["git", "log", "--reverse", "--numstat", "--pretty=format:@@%H|%ad", "--date=short"],
    capture_output=True, text=True
).stdout

commit_count = subprocess.run(["git", "rev-list", "--count", "HEAD"], capture_output=True, text=True).stdout.strip()

lines_now = {}
points = []  # (commit_index, sha, date, total)
idx = 0
cur_sha = cur_date = None
rename_re = re.compile(r'^(.*)\{(.*) => (.*)\}(.*)$')
for line in log.split("\n"):
    if line.startswith("@@"):
        idx += 1
        cur_sha, cur_date = line[2:].split("|", 1)
        continue
    if not line.strip():
        continue
    parts = line.split("\t")
    if len(parts) != 3:
        continue
    add, dele, path = parts
    m = rename_re.match(path)
    if m:
        path = m.group(1) + m.group(3) + m.group(4)
    if not counts_as_real(path) or add == "-" or dele == "-":
        continue
    lines_now[path] = lines_now.get(path, 0) + int(add) - int(dele)
    total = sum(v for v in lines_now.values() if v > 0)
    points.append((idx, cur_sha, cur_date, total))

if not points:
    raise SystemExit("no real-code commits found")

# Downsample to a real point every few commits rather than all ~300 raw
# diff-lines: keeps the SVG polyline legible without inventing data,
# every kept point is a real value at a real commit, never interpolated.
MAX_POINTS = 40
step = max(1, len(points) // MAX_POINTS)
sampled = points[::step]
if sampled[-1] != points[-1]:
    sampled.append(points[-1])

# Real bug, found live: `points` only gets an entry on a commit that
# touches real .c/.h/.S code, so a docs-only commit after the last code
# change (exactly what just happened, adding contacts.h/calculator.h's
# missing rows) never appears here, and doc_pct_at would read docs as of
# that STALE older code commit instead of the real current state. Force
# the final sample to the true current HEAD sha, real line count carried
# forward unchanged (it genuinely hasn't moved), doc coverage computed
# fresh against what's actually on disk right now.
head_sha = subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()
if sampled[-1][1] != head_sha:
    last_idx, _, _, last_total = sampled[-1]
    head_date = subprocess.run(["git", "log", "-1", "--format=%ad", "--date=short", head_sha], capture_output=True, text=True).stdout.strip()
    sampled[-1] = (last_idx, head_sha, head_date, last_total)

def doc_units_at(sha):
    # A "documentable unit" matches how docs/ARCHITECTURE.md's own table is
    # organized: one row per .c file (its paired .h is implicitly covered,
    # e.g. ata.c's row documents ata.h too, they're declared/implemented in
    # the same breath) plus one row per header-only subsystem (no paired .c
    # in the same dir: mail.h, reminders.h, the app_*.h ports, etc). A raw
    # per-file count (82 files) double-counts every .c/.h pair as two
    # separate "undocumented" items when one real row covers both, the
    # exact wrong number that first triggered this whole rewrite.
    files = subprocess.run(["git", "ls-tree", "-r", "--name-only", sha], capture_output=True, text=True).stdout.splitlines()
    files = [f for f in files if counts_as_real(f)]
    stems = {f[:-2] for f in files if f.endswith(".c")}
    units = []
    for f in files:
        if f.endswith(".c") or f.endswith(".S"):
            units.append(f)
        elif f.endswith(".h") and f[:-2] not in stems:
            units.append(f)
    return units

def doc_pct_at(sha):
    units = doc_units_at(sha)
    if not units:
        return 0
    arch = subprocess.run(["git", "show", f"{sha}:docs/ARCHITECTURE.md"], capture_output=True, text=True).stdout
    if not arch:
        return 0  # honest: the doc didn't exist yet at this point in history
    documented = sum(1 for f in units if f.rsplit("/", 1)[-1] in arch)
    return documented * 100 // len(units)

labels = [p[2] for p in sampled]
cum = [p[3] for p in sampled]
doc_pct = [doc_pct_at(p[1]) for p in sampled]
n = len(cum)
max_v = cum[-1] or 1

# v53: real layout fix, not a tweak. The rotated y-axis title ("Lines of
# code") and the numeric tick labels ("34912", "17456", "0") were both
# drawn starting at x=2, so the rotated title's own vertical span (its
# rotated width, which for a 13-character string is ~55-60px, not the
# ~8px font-size someone eyeballing the tag would guess) landed directly
# on top of whichever tick sits near mid-height, every single time, not
# just for today's numbers -- confirmed by rendering, the "17456" tick
# collided even though the bug report screenshot showed a different
# value ("18814") from an earlier data snapshot; same root cause both
# times, two columns sharing the same x instead of sitting side by side.
# Real fix: give the title and the tick numbers their own separate
# columns. Tick label width is sized from max_v's real digit count (not
# a hardcoded guess) so this stays correct as the line count grows past
# today's ~35k into six digits, and pad_l is derived from those two
# column widths plus real gaps instead of a fixed magic number.
TICK_CHAR_W = 5.5  # approx glyph advance at font-size 9
TITLE_COL_W = 12    # rotated title's own thickness (font-size 8) + margin
COL_GAP = 4          # real gap between the title column and the tick column
AXIS_GAP = 4         # real gap between the tick column and the axis line
tick_digits = len(str(max_v))
tick_col_w = max(10, int(tick_digits * TICK_CHAR_W) + 2)
pad_l = TITLE_COL_W + COL_GAP + tick_col_w + AXIS_GAP
title_x = TITLE_COL_W // 2
tick_x = pad_l - AXIS_GAP  # tick text is right-aligned (anchor=end) here
# pad_b needs room for two stacked text rows below the plot (the date
# labels, then the bold summary caption) -- 28 only gave them an 8px
# baseline gap, not enough for two font-size-10 rows (~14px needed
# before descenders/ascenders start touching), and rendering it for
# real showed exactly that: "Aug 31" clipping into "25,650 lines...".
pad_r, pad_t, pad_b = 10, 26, 34
plot_w, plot_h = 420, 140
width = pad_l + plot_w + pad_r
height = pad_t + plot_h + pad_b

def xf(i): return pad_l + i * plot_w // (n - 1 if n > 1 else 1)
def yf(v): return pad_t + plot_h - v * plot_h // max_v
def yf_pct(v): return pad_t + plot_h - v * plot_h // 100  # right axis is always 0-100%

points_attr = " ".join(f"{xf(i)},{yf(cum[i])}" for i in range(n))
# Direct feedback: 40+ dots on a smooth curve is visual noise, not more
# information, the line itself already carries every sampled value.
# Thin markers to ~10 total, always keeping the first and last real point.
DOT_TARGET = 10
dot_step = max(1, (n - 1) // (DOT_TARGET - 1)) if n > 1 else 1
dot_idx = sorted(set(list(range(0, n, dot_step)) + [n - 1]))
dots = "".join(f'<circle cx="{xf(i)}" cy="{yf(cum[i])}" r="3" fill="var(--bg)" stroke="var(--line)" stroke-width="2" data-version="{labels[i]}" data-lines="{cum[i]}"/>' for i in dot_idx)
half_v = max_v // 2

# x-axis: real calendar dates, deduplicated (many commits share a day),
# thinned the same way the old script thinned version labels, by a
# minimum pixel gap so 8+ dates across a narrow mobile viewport never
# overlap into mush.
uniq_date_idx = []
seen = set()
for i, lab in enumerate(labels):
    if lab not in seen:
        seen.add(lab)
        uniq_date_idx.append(i)
min_gap = 46
shown = []
last_x_used = -min_gap
for i in uniq_date_idx:
    x = xf(i)
    if x - last_x_used >= min_gap:
        shown.append(i)
        last_x_used = x
# Real bug, found live: the old version force-appended the very last
# unique date's label unconditionally ("or i == uniq_date_idx[-1]"),
# ignoring min_gap entirely for it. That's exactly what crammed "Sep 15
# Sep 16" together at the right edge whenever the most recent commits
# landed close together in the sample index -- the forced label had no
# room to breathe next to whatever the gap scan had already placed.
# Real fix: still guarantee the most recent real date is always shown
# (dropping the newest label silently would be its own bug), but if it
# collides with the last label the gap scan kept, REPLACE that label
# instead of adding a second one on top of it. Every real date range
# gets at least min_gap between adjacent labels now, not just the ones
# that happened to land far enough apart on their own.
last_i = uniq_date_idx[-1]
if not shown or shown[-1] != last_i:
    if shown and xf(last_i) - xf(shown[-1]) < min_gap:
        shown[-1] = last_i
    else:
        shown.append(last_i)

def short_date(d):
    # "2026-09-14" -> "Sep 14"
    y, mo, da = d.split("-")
    months = ["", "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"]
    return f"{months[int(mo)]} {int(da)}"

# Shared markup pieces: the plain static file (progress.svg, what
# README.md embeds, no JS available there) and the landing page's real
# interactive version plot the exact same axes off the exact same real
# values, computed once, so the two can never quietly drift apart. The
# only real difference between them is the dots layer and the CSS scope
# selector (see build_svg below) -- the interactive one needs real hit
# targets and hover/focus state, the static one doesn't.
bg_rect = '<rect width="100%" height="100%" fill="var(--bg)"/>'
# Single legend row, one series. The old second dashed line (doc
# coverage over time) got cut entirely, third real attempt at this:
# relabeling wasn't enough, plotting the right metric wasn't enough
# either, the metric itself is fundamentally lumpy (jumps in one pass,
# not a smooth trend) and just reads as a noisy, ugly zigzag as a line
# chart, direct feedback ("still looks retarded"), fair. Doc coverage
# is a real, current, mostly-binary fact, not a time series worth
# fighting a chart to show, so it's a plain stat in the caption instead.
legend = (
    f'<line x1="{pad_l}" y1="8" x2="{pad_l+14}" y2="8" stroke="var(--line)" stroke-width="2.5"/>'
    f'<text x="{pad_l+19}" y="11" font-size="10" fill="var(--label)">Lines of real code</text>'
)
grid_lines = (
    f'<line x1="{pad_l}" y1="{pad_t}" x2="{pad_l+plot_w}" y2="{pad_t}" stroke="var(--grid)"/>'
    f'<line x1="{pad_l}" y1="{pad_t+plot_h//2}" x2="{pad_l+plot_w}" y2="{pad_t+plot_h//2}" stroke="var(--grid)"/>'
)
tick_labels = (
    f'<text x="{tick_x}" y="{pad_t+3}" font-size="9" fill="var(--muted)" text-anchor="end">{max_v}</text>'
    f'<text x="{tick_x}" y="{pad_t+plot_h//2+3}" font-size="9" fill="var(--muted)" text-anchor="end">{half_v}</text>'
    f'<text x="{tick_x}" y="{pad_t+plot_h+3}" font-size="9" fill="var(--muted)" text-anchor="end">0</text>'
)
axis_lines = (
    f'<line x1="{pad_l}" y1="{pad_t}" x2="{pad_l}" y2="{pad_t+plot_h}" stroke="var(--axis)"/>'
    f'<line x1="{pad_l}" y1="{pad_t+plot_h}" x2="{pad_l+plot_w}" y2="{pad_t+plot_h}" stroke="var(--axis)"/>'
)
# Left axis title, rotated, its own dedicated column (title_x), well clear
# of the tick-number column (tick_x, right-aligned into the axis line) so
# the two never share pixels regardless of how many digits max_v has.
title_label = f'<text x="{title_x}" y="{pad_t+plot_h//2}" font-size="8" fill="var(--line)" text-anchor="middle" transform="rotate(-90 {title_x} {pad_t+plot_h//2})">Lines of code</text>'
polyline = f'<polyline points="{points_attr}" fill="none" stroke="var(--line)" stroke-width="2.5" stroke-linejoin="round" stroke-linecap="round"/>'
date_labels = "".join(f'<text x="{xf(i)}" y="{pad_t+plot_h+16}" font-size="10" fill="var(--label)" text-anchor="middle">{short_date(labels[i])}</text>' for i in shown)
caption = f'<text x="{pad_l}" y="{height-4}" font-size="10" font-weight="600" fill="var(--strong)">{max_v:,} lines &#183; {doc_pct[-1]}% documented &#183; {commit_count} commits since {short_date(points[0][2])}</text>'

# v52.4: real dark-mode support, direct feedback ("white graph on dark
# mode... should be dynamic and native as code, not a screenshot"). A
# plain pre-rendered SVG with hardcoded colors can't respond to the
# viewer's theme at all, exactly the "screenshot" complaint. The real
# fix: an SVG loaded via <img> still evaluates its OWN <style> block's
# @media queries against the browser's color-scheme preference, a real,
# supported platform feature, not a hack, so this doesn't need inlining
# into the page or a second dark-mode image generated alongside it. CSS
# custom properties defined once here, redefined under
# prefers-color-scheme:dark, same technique index.html's own :root
# already uses for the rest of the page, every fill/stroke reads via
# var(--x) instead of a literal hex.
def color_vars(scope):
    return f'''
  {scope} {{
    --bg: #faf8f6; --grid: #e8e2da; --axis: #ded6ca; --muted: #a39c92;
    --label: #75726e; --strong: #1c1c1e; --line: #884b16; --line2: #4c2e13; --line2-pct: #b6a08a;
  }}
  @media (prefers-color-scheme: dark) {{
    {scope} {{ --bg: #161412; --grid: #2c2724; --axis: #3a332e; --muted: #8a8177;
             --label: #b3aa9f; --strong: #f2f0ee; --line: #d99a5b; --line2: #e8b98a; --line2-pct: #c3a58a; }}
  }}'''

def build_svg(dots_markup, svg_id=None, extra_style="", extra_root_attrs=""):
    # svg_id=None -> the plain, standalone progress.svg (README embed via
    # <img>, a fully separate SVG document, so a bare ":root"/"text"
    # selector only ever touches that document). svg_id set -> the
    # landing page's version, inlined directly into index.html; an inlined
    # SVG's own <style> rules are NOT scoped to it by the browser (a real
    # gotcha found while building this: a bare ":root { --bg: ... }" in
    # here would silently override the whole page's --bg custom property,
    # since :root means the HTML document root once this is inlined, not
    # the svg element), so every selector below is scoped by #svg_id
    # instead of relying on document structure.
    scope = f"#{svg_id}" if svg_id else ":root"
    id_attr = f' id="{svg_id}"' if svg_id else ""
    text_rule = f'{scope} text' if svg_id else 'text'
    style = f'<style>{color_vars(scope)}\n  {text_rule} {{ font-family: -apple-system, Helvetica, Arial, sans-serif; }}\n{extra_style}</style>'
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}"{id_attr}{extra_root_attrs}>'
        + style + bg_rect + legend + grid_lines + tick_labels + axis_lines + title_label
        + polyline + dots_markup + date_labels + caption + '</svg>'
    )

plain_svg = build_svg(dots)

with open("progress.svg", "w") as f:
    f.write(plain_svg)
import shutil, os
os.makedirs("landing", exist_ok=True)
shutil.copy("progress.svg", "landing/progress.svg")

# Real interactive version for the landing page: same points, wrapped in
# a <g> with a bigger invisible hit target (real dots are only r=3, too
# small to reliably hover/tap on their own) and a real data-tooltip
# string baked in at generation time (real date + real, comma-formatted
# line count, computed from the same git history the chart itself
# plots -- never fabricated at hover time in JS).
SVG_ID = "progress-chart-live"
dots_interactive = "".join(
    f'<g class="progress-pt" tabindex="0" role="img" '
    f'aria-label="{short_date(labels[i])}, {cum[i]:,} lines of real code" '
    f'data-tooltip="{short_date(labels[i])} &#183; {cum[i]:,} lines">'
    f'<circle class="progress-hit" cx="{xf(i)}" cy="{yf(cum[i])}" r="11" fill="transparent"/>'
    f'<circle class="progress-dot" cx="{xf(i)}" cy="{yf(cum[i])}" r="3" fill="var(--bg)" stroke="var(--line)" stroke-width="2"/>'
    f'</g>'
    for i in dot_idx
)
interactive_style = f'''
  #{SVG_ID} .progress-pt {{ cursor: pointer; }}
  #{SVG_ID} .progress-dot {{ transition: r 0.15s ease, stroke-width 0.15s ease; }}
  #{SVG_ID} .progress-pt:hover .progress-dot,
  #{SVG_ID} .progress-pt:focus .progress-dot,
  #{SVG_ID} .progress-pt.is-active .progress-dot {{ r: 5.5; stroke-width: 2.5; }}
  #{SVG_ID} .progress-pt:focus {{ outline: none; }}
'''
interactive_svg = build_svg(
    dots_interactive, svg_id=SVG_ID, extra_style=interactive_style,
    extra_root_attrs=f' role="img" aria-label="Real lines of code over time, {max_v:,} lines as of {short_date(points[-1][2])}"',
)

# Splice into landing/index.html between markers, the same inject-
# between-comments pattern tools/gen/landing-roadmap.py already uses for
# the roadmap summary card, so one script run keeps README's plain chart
# and the landing page's real interactive one both current together.
landing_path = "landing/index.html"
with open(landing_path) as f:
    html = f.read()
start_marker, end_marker = "<!-- progress-chart:start -->", "<!-- progress-chart:end -->"
si = html.index(start_marker)
ei = html.index(end_marker)
if si == -1 or ei == -1:
    raise SystemExit(f"{landing_path} is missing the progress-chart:start/end markers")
html = html[:si + len(start_marker)] + "\n" + interactive_svg + "\n" + html[ei:]
with open(landing_path, "w") as f:
    f.write(html)

print(f"wrote progress.svg: {max_v:,} real lines, {doc_pct[-1]}% documented (real architecture-doc coverage), across {n} sampled points, {commit_count} total commits")
print(f"spliced interactive chart into {landing_path}")
PYEOF
