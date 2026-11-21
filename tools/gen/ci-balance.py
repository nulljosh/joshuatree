#!/usr/bin/env python3
"""Rebalance the CI shards. ci-suite.sh assigns every check to a shard by hand (the number after `once|` or
`retry|`), and over time one shard fills up: the slowest shard sets how long every CI run takes.

This reads how long each check really took in finished CI runs (the median across the runs given), deals them out
to the shards longest first so every shard gets about the same total, and rewrites the shard numbers in
tools/checks/ci-suite.sh. It refuses to pack when it could not time almost every check: until 13.21.4 it called gh
with a flag gh does not have, read no logs at all, priced every check at the 30 s default and still rewrote the
shards, so the packing was fiction and shard 7 ran 2.4 times longer than shard 1.

A check is ARM when it runs qemu-system-aarch64 or builds arch/arm64 (its command, or the source of the
tools/checks script it runs); those are packed across the ARM shards (the ubuntu-26.04 runner, whose QEMU has raspi4b)
and the rest across the others. The ARM shards and the shard count are read from .github/workflows/check.yml, the one
place they are written.

Usage:  python3 tools/gen/ci-balance.py <run-id> [<run-id> ...]     (needs `gh`; green full runs are best)
        python3 tools/gen/ci-balance.py --check <run-id> [...]      (print the numbers, change nothing)
"""
import json, re, subprocess, sys

import statistics
args = [a for a in sys.argv[1:] if not a.startswith("--")]
dry = "--check" in sys.argv
if not args:
    sys.exit(__doc__)
WF = open(".github/workflows/check.yml").read()
ARM = [int(x) for x in re.search(r"contains\(fromJSON\('\[([0-9, ]+)\]'\), matrix\.shard\)", WF).group(1).split(",")]
K = len(re.search(r"shard: \[([0-9, ]+)\]", WF).group(1).split(","))
NEEDS = re.compile(r"[\"']qemu-system-aarch64[\"']|qemu-system-aarch64 +-|make[^\n]{0,60}arch/arm64|\"-C\",\s*(arch|ARCH)\b|make (-s )?-C \S*arm64")
def needs_arm(cmd):
    """The check runs the ARM QEMU or builds arch/arm64, in its command or in the tools/checks script it runs."""
    if "qemu-system-aarch64" in cmd: return True
    for f in re.findall(r"tools/checks/[\w.-]+", cmd):
        try: src = open(f).read()
        except OSError: continue
        if NEEDS.search(src): return True
    return False
SUITE = "tools/checks/ci-suite.sh"

def gh(*a):
    r = subprocess.run(["gh", *a], capture_output=True, text=True)
    if r.returncode != 0:   # a failed call must stop the packing, never read as an empty log
        sys.exit("gh %s failed: %s" % (" ".join(a), r.stderr.strip()))
    return r.stdout

runs = {}   # check name -> its time in each run
for run_id in args:
    jobs = json.loads(gh("run", "view", run_id, "--json", "jobs"))["jobs"]
    seen = {}
    for j in jobs:
        if not j["name"].startswith("suite ("):
            continue
        log = gh("api", f"repos/{{owner}}/{{repo}}/actions/jobs/{j['databaseId']}/logs")
        for line in re.sub(r"\x1b\[[0-9;]*m", "", log).splitlines():
            m = re.search(r" (?:PASS|FLAKY) +(.*) \((?:[^0-9)]*)(\d+)s[^)]*\)$", line)
            if m:
                seen[m.group(1).strip()] = max(seen.get(m.group(1).strip(), 0), int(m.group(2)))
    for name, d in seen.items():
        runs.setdefault(name, []).append(d)
dur = {name: statistics.median(ds) for name, ds in runs.items()}

lines = open(SUITE).read().split("\n")
entries = []
for i, l in enumerate(lines):
    m = re.match(r"^(once|retry)\s*\|\s*(\d+)\s*\|([^|]*)\|(.*)$", l)
    if m:
        entries.append((i, m.group(3), dur.get(m.group(3), 30), needs_arm(m.group(4))))   # a check with no timing gets 30 s
untimed = [name for _, name, _, _ in entries if name not in dur]
print(f"{len(entries) - len(untimed)} of {len(entries)} checks timed from {len(args)} run(s)")
if len(untimed) > max(1, len(entries) // 20):   # a few new checks may have no timing yet; most must
    sys.exit("refusing to pack: %d checks have no timing in those runs (a run that skipped the suite, or logs gh could not "
             "read), first few: %s" % (len(untimed), "; ".join(n[:60] for n in untimed[:5])))
loads = [0] * K
assign = {}
# Longest first into the lightest shard of its kind: ARM checks across ARM, the rest across the others.
for i, name, d, arm in sorted(entries, key=lambda e: -e[2]):
    pool = ARM if arm else [s for s in range(K) if s not in ARM]
    s = min(pool, key=lambda s: loads[s]); loads[s] += d; assign[i] = s
before = {}
for i, name, d, arm in entries:
    s = int(re.match(r"^(?:once|retry)\s*\|\s*(\d+)", lines[i]).group(1)); before[s] = before.get(s, 0) + d
print(f"{len(entries)} checks, {sum(e[2] for e in entries):.0f} s of checks")
print("before, seconds per shard:", dict(sorted(before.items())))
print("after,  seconds per shard:", {s: round(l) for s, l in enumerate(loads)})
if dry:
    sys.exit(0)
for i, s in assign.items():
    lines[i] = re.sub(r"^(once|retry)(\s*\|\s*)\d+(\s*\|)", lambda m: f"{m.group(1)}{m.group(2)}{s}{m.group(3)}", lines[i], count=1)
open(SUITE, "w").write("\n".join(lines))
print("rewrote", SUITE)
