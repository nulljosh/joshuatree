#!/usr/bin/env python3
"""Compute the landing page's real facts straight from the source and
rewrite the `data-fact="..."` spans in landing/index.html in place.

Every number on the page that describes the project must come from the
same place a human would look to check it, the way the headline already
comes from docs/roadmap.md's **Latest** line (inject-landing-headline.sh)
and the version comes from VERSION (landing/version.txt). This does the
same job for four more facts:

  apps    - real apps in kernel/kernel.c's GUI_LABELS, minus the Apps
            folder tile and Trash (neither is a real app).
  checks  - regression checks in tools/checks/ci-suite.sh's manifest,
            counted the same way the suite itself counts them: one line
            per `once` or `retry` entry.
  lines   - hand-authored kernel/driver source, counted with the same
            real-code filter tools/gen/progress.sh uses (same excluded
            dirs, generated-data headers, and app_* port prefix), summed
            over the files on disk right now rather than replayed from
            git history. progress.sh's own number comes from a full
            `git log --numstat` replay instead, because it needs a value
            at every historical commit for the chart, not just today's;
            the two can differ by a handful of lines on a rename-heavy
            history (a known, harmless edge in that replay, not a bug
            here) since this one just reads the files that exist right
            now.
  version - the latest tagged release, straight from VERSION.

Run this before wrangler deploy (like inject-landing-headline.sh) to keep
the landing page's fact row honest. tools/checks/landing-facts-check.py
fails CI if a marked span ever disagrees with the computed value.
"""
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# Same real-code filter tools/gen/progress.sh uses, kept in sync by hand:
# comments there explain why each entry is excluded (vendored v86 build,
# raster/glyph data tables baked into headers, ported-app HTML byte
# arrays). Only .c/.h/.S files hand-authored for the kernel/drivers count.
EXCLUDE_DIRS = ("node_modules/", ".claude/", "landing/v86/")
EXCLUDE_BASENAMES = {"wallpaper.h", "editor_fonts.h", "vgafont.h", "icon_art.h"}
EXCLUDE_PREFIX = ("drivers/app_",)


def counts_as_real(rel_path):
    if any(rel_path.startswith(d) for d in EXCLUDE_DIRS):
        return False
    if not rel_path.endswith((".c", ".h", ".S")):
        return False
    if rel_path.rsplit("/", 1)[-1] in EXCLUDE_BASENAMES:
        return False
    if any(rel_path.startswith(p) for p in EXCLUDE_PREFIX):
        return False
    return True


def count_apps():
    src = (ROOT / "kernel/kernel.c").read_text()
    m = re.search(r"GUI_LABELS\[GUI_APP_COUNT\]\s*=\s*\{(.*?)\};", src, re.S)
    if not m:
        raise ValueError("Could not find GUI_LABELS in kernel/kernel.c")
    labels = re.findall(r'"([^"]*)"', m[1])
    if not labels:
        raise ValueError("GUI_LABELS parsed with no labels")
    real = [l for l in labels if l not in ("Apps", "Trash")]
    return len(real)


def count_checks():
    src = (ROOT / "tools/checks/ci-suite.sh").read_text()
    m = re.search(r"manifest\(\) \{\ncat <<'EOF'\n(.*?)\nEOF\n\}", src, re.S)
    if not m:
        raise ValueError("Could not find the manifest heredoc in tools/checks/ci-suite.sh")
    lines = [l for l in m[1].splitlines() if re.match(r"^(once|retry)\s*\|", l)]
    if not lines:
        raise ValueError("Manifest heredoc parsed with no once/retry lines")
    return len(lines)


def count_lines():
    # Read the same number the progress chart already plots as its most
    # recent point (landing/progress.svg's caption, e.g. "49,454 lines"),
    # rather than recomputing it a second way. The stat row and the chart
    # used to come from two different counts (a live filesystem walk here
    # vs. a git-log replay in tools/gen/progress.sh) that could legitimately
    # disagree by a handful of lines on a rename-heavy history. Scraping
    # the chart's own generated text guarantees the two numbers on the page
    # can never drift apart: run tools/gen/progress.sh first to refresh
    # progress.svg, then this script picks up whatever it just wrote.
    svg = (ROOT / "landing/progress.svg").read_text()
    m = re.search(r"([\d,]+) lines", svg)
    if not m:
        raise ValueError("Could not find a 'N lines' caption in landing/progress.svg; run tools/gen/progress.sh first")
    return m[1]


def read_version():
    v = (ROOT / "VERSION").read_text().strip()
    if not v:
        raise ValueError("VERSION is empty")
    return v


def compute_facts():
    return {
        "apps": str(count_apps()),
        "checks": str(count_checks()),
        "lines": count_lines(),
        "version": read_version(),
    }


def render(page, facts):
    def sub_one(name, value):
        nonlocal page
        pattern = re.compile(r'(<span data-fact="' + re.escape(name) + r'">)[^<]*(</span>)')
        new_page, n = pattern.subn(lambda m: m[1] + value + m[2], page)
        if n == 0:
            raise ValueError(f'No <span data-fact="{name}"> found in landing/index.html')
        page = new_page

    for name, value in facts.items():
        sub_one(name, value)
    return page


def main():
    page_path = ROOT / "landing/index.html"
    page = page_path.read_text()
    facts = compute_facts()
    updated = render(page, facts)
    if updated != page:
        page_path.write_text(updated)
    print("Landing facts: " + ", ".join(f"{k}={v}" for k, v in facts.items()))


if __name__ == "__main__":
    main()
