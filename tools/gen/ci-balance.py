#!/usr/bin/env python3
"""Rebalance the CI shards. ci-suite.sh assigns every check to a shard by hand (the number after `once|` or
`retry|`), and over time one shard fills up: the slowest shard sets how long every CI run takes.

This reads how long each check really took in a finished CI run, deals them out to the shards longest first so
every shard gets about the same total, and rewrites the shard numbers in tools/checks/ci-suite.sh.

ARM checks (QEMU raspi4b, the ubuntu-26.04 runner) stay on the ARM shards and are packed across those; the rest
are packed across the others.

Usage:  python3 tools/gen/ci-balance.py <run-id> [shards]     (needs `gh`; a green run is best)
        python3 tools/gen/ci-balance.py --check <run-id>      (print the numbers, change nothing)
"""
import json, re, subprocess, sys

args = [a for a in sys.argv[1:] if not a.startswith("--")]
dry = "--check" in sys.argv
if not args:
    sys.exit(__doc__)
run_id = args[0]
K = int(args[1]) if len(args) > 1 else 11
ARM = [6, 8, 9, 10]   # .github/workflows/check.yml gives these the ubuntu-26.04 runner; tools/ci-local.sh runs them one after another
if K <= max(ARM):
    sys.exit("need at least %d shards: %s hold the ARM checks" % (max(ARM) + 1, ARM))
SUITE = "tools/checks/ci-suite.sh"

def gh(*a):
    return subprocess.run(["gh", *a], capture_output=True, text=True).stdout

jobs = json.loads(gh("run", "view", run_id, "--json", "jobs"))["jobs"]
dur = {}
for j in jobs:
    if not j["name"].startswith("suite ("):
        continue
    log = gh("api", "--allow-escape-sequences", f"repos/{{owner}}/{{repo}}/actions/jobs/{j['databaseId']}/logs")
    for line in re.sub(r"\x1b\[[0-9;]*m", "", log).splitlines():
        m = re.search(r" (?:PASS|FLAKY) +(.*) \((?:[^0-9)]*)(\d+)s[^)]*\)$", line)
        if m:
            dur[m.group(1).strip()] = max(dur.get(m.group(1).strip(), 0), int(m.group(2)))

lines = open(SUITE).read().split("\n")
entries = []
for i, l in enumerate(lines):
    m = re.match(r"^(once|retry)\s*\|\s*(\d+)\s*\|([^|]*)\|", l)
    if m:
        entries.append((i, m.group(3), dur.get(m.group(3), 30), int(m.group(2))))   # a check with no timing gets 30 s
loads = [0] * K
assign = {}
# Longest first into the lightest shard of its kind: ARM checks across ARM, the rest across the others.
for i, name, d, old in sorted(entries, key=lambda e: -e[2]):
    pool = ARM if old in ARM else [s for s in range(K) if s not in ARM]
    s = min(pool, key=lambda s: loads[s]); loads[s] += d; assign[i] = s
before = {}
for i, name, d, old in entries:
    s = int(re.match(r"^(?:once|retry)\s*\|\s*(\d+)", lines[i]).group(1)); before[s] = before.get(s, 0) + d
print(f"{len(entries)} checks, {sum(e[2] for e in entries)} s of checks, {len(dur)} timed")
print("before, seconds per shard:", dict(sorted(before.items())))
print("after,  seconds per shard:", dict(enumerate(loads)))
if dry:
    sys.exit(0)
for i, s in assign.items():
    lines[i] = re.sub(r"^(once|retry)(\s*\|\s*)\d+(\s*\|)", lambda m: f"{m.group(1)}{m.group(2)}{s}{m.group(3)}", lines[i], count=1)
open(SUITE, "w").write("\n".join(lines))
print("rewrote", SUITE)
