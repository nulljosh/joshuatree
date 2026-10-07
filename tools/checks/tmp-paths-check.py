#!/usr/bin/env python3
"""No check may hard-code a fixed /tmp/jt-* path or a fixed QMP socket path.

Two suites running at once share /tmp, so a fixed path means one run
overwrites the other's QEMU dump (see scratch.py for the fix). This scans
every file under tools/checks for `/tmp/jt-`, `unix:/tmp/...` and
`/tmp/*.sock`. Files that still do it are listed in tmp-paths-baseline.txt.
That list may only shrink: a new offender fails, and so does a listed file
that no longer offends (delete its line).

Usage: python3 tools/checks/tmp-paths-check.py [checks_dir baseline_file]
"""
import os, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
SELF = {"tmp-paths-check.py", "tmp-paths-baseline.txt"}
BAD = re.compile(r"/tmp/jt-|unix:/tmp/|/tmp/[\w.-]*\.sock\b")


def offenders(checks_dir):
    out = {}
    for name in sorted(os.listdir(checks_dir)):
        path = os.path.join(checks_dir, name)
        if name in SELF or not os.path.isfile(path):
            continue
        try:
            text = open(path, errors="replace").read()
        except OSError:
            continue
        lines = [i + 1 for i, l in enumerate(text.splitlines()) if BAD.search(l)]
        if lines:
            out[name] = lines
    return out


def load_baseline(path):
    names = set()
    for line in open(path):
        line = line.split("#", 1)[0].strip()
        if line:
            names.add(line)
    return names


def main():
    checks_dir = sys.argv[1] if len(sys.argv) > 2 else HERE
    baseline_file = sys.argv[2] if len(sys.argv) > 2 else os.path.join(HERE, "tmp-paths-baseline.txt")
    found = offenders(checks_dir)
    baseline = load_baseline(baseline_file)
    fails = []
    for name, lines in found.items():
        if name not in baseline:
            fails.append(f"{name}:{lines[0]} hard-codes a fixed /tmp path or socket; use scratch_dir() from scratch.py (or mktemp -d in shell)")
    for name in sorted(baseline - set(found)):
        fails.append(f"{name} is in tmp-paths-baseline.txt but is clean now; delete its line (the list only shrinks)")
    if fails:
        print("FAIL: tmp-paths-check")
        for f in fails:
            print("  " + f)
        return 1
    print(f"OK: no new fixed /tmp paths ({len(baseline)} files still on the baseline)")
    return 0


sys.exit(main())
