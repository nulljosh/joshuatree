#!/usr/bin/env python3
"""Every tool this OS answers locally must be a tool Turing's picker can name, or an
explicit OS-only one below. Turing renaming a tool otherwise silently stops the OS
handling it. Snapshot: docs/turing-tools.txt (python3 tools/gen/sync_turing.py)."""
import os, re, sys
root = os.path.join(os.path.dirname(__file__), "..", "..")
snap = [l.strip() for l in open(os.path.join(root, "docs", "turing-tools.txt")) if l.strip()]
version = snap[0].split()[2] if snap[0].startswith("#") else "?"
turing = {l for l in snap if not l.startswith("#")}
# The OS's own Mail/Notes/say/open_app: Turing's picker never names these, the OS keyword path does.
OS_ONLY = {"read_mail", "read_notes", "send_mail", "say", "open_app", "list_reminders"}
handled = set()
for p in ("kernel/chat.h", "user/samantha.c"):
    path = os.path.join(root, p)
    if os.path.exists(path):
        handled |= set(re.findall(r'(?:strcmp|streq)\(tool, "([a-z_]+)"\)', open(path).read()))
bad = sorted(handled - turing - OS_ONLY)
if bad:
    print(f"FAIL: handled locally but not in Turing {version}'s tool list: {', '.join(bad)} -- renamed upstream? run tools/gen/sync_turing.py or add to OS_ONLY")
    sys.exit(1)
print(f"PASS: {len(handled)} local tools all exist in Turing {version} or are OS-only")
