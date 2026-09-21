#!/usr/bin/env python3
"""Render up to three open priorities from the ordered session task queue."""
import argparse
import html
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
START = "<!-- roadmap-summary:start -->"
END = "<!-- roadmap-summary:end -->"


def summarize(roadmap):
    section = re.search(
        r"^## Session task queue[^\n]*\n(.*?)(?=^## |\Z)",
        roadmap, re.M | re.S,
    )
    if not section:
        raise ValueError("Missing Session task queue section")
    titles = []
    for line in section[1].splitlines():
        # Completed entries are struck through. Only numbered, bold task
        # titles are public copy; the diagnostic prose stays in the roadmap.
        match = re.match(r"^\d+\. \*\*(.+?)\*\*", line)
        if match:
            title = re.sub(r"[`*_]", "", match[1]).strip().rstrip(".")
            if title and title not in titles:
                titles.append(title)
    if not titles:
        return "The current task queue is complete. More plans soon."
    return "On the roadmap: " + "; ".join(titles[:3]) + "."


def render(roadmap, page):
    if page.count(START) != 1 or page.count(END) != 1:
        raise ValueError("Expected exactly one roadmap summary marker pair")
    before, rest = page.split(START)
    old, after = rest.split(END)
    return before + START + "<p>" + html.escape(summarize(roadmap)) + "</p>" + END + after


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="Fail if generated copy is stale")
    args = parser.parse_args()
    page_path = ROOT / "landing/index.html"
    page = page_path.read_text()
    updated = render((ROOT / "docs/roadmap.md").read_text(), page)
    if args.check:
        if page != updated:
            parser.exit(1, "Landing roadmap summary is stale; run python3 tools/gen/landing-roadmap.py\n")
    elif page != updated:
        page_path.write_text(updated)


if __name__ == "__main__":
    main()
