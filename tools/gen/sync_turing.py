#!/usr/bin/env python3
"""Snapshot the tool names Turing's picker can return into docs/turing-tools.txt.
Run after Turing changes its tool list: python3 tools/gen/sync_turing.py [path/to/turing]
tools/checks/turing-sync-check.py keeps this OS's local handlers honest against the snapshot."""
import os, sys
turing = os.path.expanduser(sys.argv[1] if len(sys.argv) > 1 else "~/Documents/Code/turing")
sys.path[:0] = [turing, os.path.join(turing, "app")]
from app import tools
version = open(os.path.join(turing, "VERSION")).read().strip()
out = os.path.join(os.path.dirname(__file__), "..", "..", "docs", "turing-tools.txt")
with open(out, "w") as f:
    f.write(f"# Turing v{version} tool names (model_tools). Regenerate: python3 tools/gen/sync_turing.py\n")
    f.write("\n".join(sorted(tools.model_tools())) + "\n")
print(f"wrote {len(tools.model_tools())} tools for Turing v{version}")
