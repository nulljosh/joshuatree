#!/usr/bin/env python3
"""The relay token only ships on a dev build. claude_cfg.sh reads the token file (default ~/.claude-relay-token) only when
JT_WIFI_DEV=1 (tools/flash-pi.sh) or CLAUDE_RELAY_TOKEN_FILE is set explicitly; a plain `make` on the Mac that holds the
real token must produce a kernel8.img without it.

Builds twice with HOME pointed at a temp dir holding a known 24-byte throwaway token in .claude-relay-token:
  1. flag unset: the 24 bytes must not appear in kernel8.img, and neither must the default relay host 10.0.2.2.
  2. JT_WIFI_DEV=1: the 24 bytes must appear.
Discriminating: drop the JT_WIFI_DEV guard in claude_cfg.sh and build 1 fails.
Skips (exit 0) when clang's aarch64 target or ld.lld is missing.
Usage: tools/checks/arm64-token-gate-check.py   (from the repo root)
"""
import os, shutil, subprocess, sys, tempfile

if not all(shutil.which(t) for t in ("clang", "ld.lld")):
    print("SKIP: clang or ld.lld missing"); sys.exit(0)
root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
arch = os.path.join(root, "arch", "arm64")
TOKEN = b"gate-check-throwaway-0x1"   # 24 bytes, never a real token
assert len(TOKEN) == 24
tmp = tempfile.mkdtemp()
with open(os.path.join(tmp, ".claude-relay-token"), "wb") as f: f.write(TOKEN + b"\n")

def build(dev):
    env = {k: v for k, v in os.environ.items() if not k.startswith("CLAUDE_RELAY") and k != "JT_WIFI_DEV"}
    env["HOME"] = tmp
    if dev: env["JT_WIFI_DEV"] = "1"
    r = subprocess.run(["make", "-B", "-C", arch, "kernel8.img"], env=env, capture_output=True, text=True, timeout=600)
    if r.returncode: print("FAIL: arch/arm64 does not build:\n" + r.stdout[-2000:] + r.stderr[-2000:]); sys.exit(1)
    return open(os.path.join(arch, "kernel8.img"), "rb").read()

img = build(dev=False)
if TOKEN in img: print("FAIL: release build (JT_WIFI_DEV unset) carries the relay token"); sys.exit(1)
if b"10.0.2.2" in img: print("FAIL: release build carries the relay host"); sys.exit(1)
img = build(dev=True)
if TOKEN not in img: print("FAIL: dev build (JT_WIFI_DEV=1) lacks the relay token"); sys.exit(1)
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)   # drop the throwaway-token header and image
shutil.rmtree(tmp)
print("PASS: relay token absent from the release image, present on the dev build")
