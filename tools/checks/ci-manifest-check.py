#!/usr/bin/env python3
"""Every row of the ci-suite.sh manifest splits into exactly mode|shard|name|command.

A `|` inside a check's name (8.3.0's `userrun gen | userrun wc`) cut the name short and handed
the rest of the sentence to bash as the command, which died on an unmatched backtick. Each row's
command must start with a runner the suite uses, and each name must have its backticks paired.
"""
import pathlib
import re
import sys

src = pathlib.Path(__file__).with_name("ci-suite.sh").read_text()
body = re.search(r"^manifest\(\) \{\ncat <<'EOF'\n(.*?)\nEOF$", src, re.S | re.M).group(1)
RUNNERS = ("python3 ", "node ", "sh ", "bash ", "cc ", "./")
bad = []
for line in body.splitlines():
    if not line.strip() or line.lstrip().startswith("#"):
        continue
    parts = line.split("|", 3)
    if len(parts) < 4:
        bad.append(f"too few fields: {line[:80]}")
        continue
    name, command = parts[2], parts[3].lstrip()
    if name.count("`") % 2:
        bad.append(f"unpaired backtick in the name (a | inside it?): {name[:80]}")
    if not command.startswith(RUNNERS):
        bad.append(f"command does not start with a runner: {command[:80]}")
for b in bad:
    print("FAIL:", b)
if bad:
    sys.exit(1)
print(f"PASS: all {sum(1 for l in body.splitlines() if l.strip() and not l.lstrip().startswith('#'))} ci-suite rows split into mode, shard, name and a runnable command")
