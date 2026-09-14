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
set -euo pipefail
cd "$(dirname "$0")"

python3 << 'PYEOF'
import subprocess, re

EXCLUDE_DIRS = ("node_modules/", ".claude/", "landing/v86/")
EXCLUDE_BASENAMES = {"wallpaper.h", "editor_fonts.h", "vgafont.h"}
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

pad_l, pad_r, pad_t, pad_b = 34, 10, 26, 28
plot_w, plot_h = 420, 140
width = pad_l + plot_w + pad_r
height = pad_t + plot_h + pad_b

def xf(i): return pad_l + i * plot_w // (n - 1 if n > 1 else 1)
def yf(v): return pad_t + plot_h - v * plot_h // max_v
def yf_pct(v): return pad_t + plot_h - v * plot_h // 100  # right axis is always 0-100%

points_attr = " ".join(f"{xf(i)},{yf(cum[i])}" for i in range(n))
dots = "".join(f'<circle cx="{xf(i)}" cy="{yf(cum[i])}" r="3" fill="var(--bg)" stroke="var(--line)" stroke-width="2"/>' for i in range(n))
last_x = xf(n - 1)
area_points = f"{pad_l},{pad_t+plot_h} {points_attr} {last_x},{pad_t+plot_h}"
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
    if x - last_x_used >= min_gap or i == uniq_date_idx[-1]:
        shown.append(i)
        last_x_used = x

def short_date(d):
    # "2026-09-14" -> "Sep 14"
    y, mo, da = d.split("-")
    months = ["", "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"]
    return f"{months[int(mo)]} {int(da)}"

svg = []
svg.append(f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">')
# v52.4: real dark-mode support, direct feedback ("white graph on dark
# mode... should be dynamic and native as code, not a screenshot"). A
# plain pre-rendered SVG with hardcoded colors can't respond to the
# viewer's theme at all, exactly the "screenshot" complaint. The real
# fix: an SVG loaded via <img> still evaluates its OWN <style> block's
# @media queries against the browser's color-scheme preference, a real,
# supported platform feature, not a hack, so this doesn't need inlining
# into the page or a second dark-mode image generated alongside it.
# CSS custom properties defined once here, redefined under
# prefers-color-scheme:dark, same technique index.html's own :root
# already uses for the rest of the page, every fill/stroke below
# reads via var(--x) instead of a literal hex.
svg.append('''<style>
  :root {
    --bg: #faf8f6; --grid: #e8e2da; --axis: #ded6ca; --muted: #a39c92;
    --label: #75726e; --strong: #1c1c1e; --line: #884b16; --line2: #4c2e13; --line2-pct: #b6a08a;
  }
  @media (prefers-color-scheme: dark) {
    :root { --bg: #161412; --grid: #2c2724; --axis: #3a332e; --muted: #8a8177;
             --label: #b3aa9f; --strong: #f2f0ee; --line: #d99a5b; --line2: #e8b98a; --line2-pct: #c3a58a; }
  }
  text { font-family: -apple-system, Helvetica, Arial, sans-serif; }
</style>''')
svg.append('<defs><linearGradient id="area" x1="0" y1="0" x2="0" y2="1">'
            '<stop offset="0%" stop-color="var(--line)" stop-opacity="0.35"/>'
            '<stop offset="100%" stop-color="var(--line)" stop-opacity="0"/></linearGradient></defs>')
svg.append('<rect width="100%" height="100%" fill="var(--bg)"/>')
# Single legend row, one series. The old second dashed line (doc
# coverage over time) got cut entirely, third real attempt at this:
# relabeling wasn't enough, plotting the right metric wasn't enough
# either, the metric itself is fundamentally lumpy (jumps in one pass,
# not a smooth trend) and just reads as a noisy, ugly zigzag as a line
# chart, direct feedback ("still looks retarded"), fair. Doc coverage
# is a real, current, mostly-binary fact, not a time series worth
# fighting a chart to show, so it's a plain stat in the caption instead.
svg.append(f'<line x1="{pad_l}" y1="8" x2="{pad_l+14}" y2="8" stroke="var(--line)" stroke-width="2.5"/>')
svg.append(f'<text x="{pad_l+19}" y="11" font-size="10" fill="var(--label)">Lines of real code</text>')
svg.append(f'<line x1="{pad_l}" y1="{pad_t}" x2="{pad_l+plot_w}" y2="{pad_t}" stroke="var(--grid)"/>')
svg.append(f'<text x="2" y="{pad_t+3}" font-size="9" fill="var(--muted)">{max_v}</text>')
svg.append(f'<line x1="{pad_l}" y1="{pad_t+plot_h//2}" x2="{pad_l+plot_w}" y2="{pad_t+plot_h//2}" stroke="var(--grid)"/>')
svg.append(f'<text x="2" y="{pad_t+plot_h//2+3}" font-size="9" fill="var(--muted)">{half_v}</text>')
svg.append(f'<line x1="{pad_l}" y1="{pad_t}" x2="{pad_l}" y2="{pad_t+plot_h}" stroke="var(--axis)"/>')
svg.append(f'<line x1="{pad_l}" y1="{pad_t+plot_h}" x2="{pad_l+plot_w}" y2="{pad_t+plot_h}" stroke="var(--axis)"/>')
svg.append(f'<text x="2" y="{pad_t+plot_h+3}" font-size="9" fill="var(--muted)">0</text>')
# Left axis title, rotated, its own color matching the solid line
svg.append(f'<text x="10" y="{pad_t+plot_h//2}" font-size="8" fill="var(--line)" text-anchor="middle" transform="rotate(-90 10 {pad_t+plot_h//2})">Lines of code</text>')
svg.append(f'<polygon points="{area_points}" fill="url(#area)"/>')
svg.append(f'<polyline points="{points_attr}" fill="none" stroke="var(--line)" stroke-width="2.5" stroke-linejoin="round" stroke-linecap="round"/>')
svg.append(dots)
for i in shown:
    svg.append(f'<text x="{xf(i)}" y="{pad_t+plot_h+16}" font-size="10" fill="var(--label)" text-anchor="middle">{short_date(labels[i])}</text>')
svg.append(f'<text x="{pad_l}" y="{height-4}" font-size="10" font-weight="600" fill="var(--strong)">{max_v:,} lines &#183; {doc_pct[-1]}% documented &#183; {commit_count} commits since {short_date(points[0][2])}</text>')
svg.append('</svg>')

out = "".join(svg)
with open("progress.svg", "w") as f:
    f.write(out)
import shutil, os
os.makedirs("landing", exist_ok=True)
shutil.copy("progress.svg", "landing/progress.svg")
print(f"wrote progress.svg: {max_v:,} real lines, {doc_pct[-1]}% documented (real architecture-doc coverage), across {n} sampled points, {commit_count} total commits")
PYEOF
