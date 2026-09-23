#!/usr/bin/env python3
"""Fail if any `data-fact` span in landing/index.html disagrees with what
tools/gen/inject-landing-facts.py computes straight from the source, the
same drift guard versionsync-check.sh runs for VERSION/version.txt and
landing-headline-check.sh runs for the roadmap headline.
"""
import importlib.util
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location(
    "inject_landing_facts", ROOT / "tools/gen/inject-landing-facts.py"
)
gen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen)


def main():
    page = (ROOT / "landing/index.html").read_text()
    expected = gen.compute_facts()

    found = dict(re.findall(r'<span data-fact="([a-z]+)">([^<]*)</span>', page))
    missing_marks = [name for name in expected if name not in found]
    if missing_marks:
        print("FAIL: landing/index.html is missing data-fact spans for: " + ", ".join(missing_marks))
        return 1

    mismatches = [
        f'{name}: page says "{found[name]}", source says "{value}"'
        for name, value in expected.items()
        if found[name] != value
    ]
    if mismatches:
        print("FAIL: landing facts are stale, run: python3 ./tools/gen/inject-landing-facts.py")
        for line in mismatches:
            print("  " + line)
        return 1

    print("PASS: landing facts match the source (" + ", ".join(f"{k}={v}" for k, v in expected.items()) + ")")
    return 0


if __name__ == "__main__":
    sys.exit(main())
