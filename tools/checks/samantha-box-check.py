#!/usr/bin/env python3
"""The Samantha box CAD builds, every fit assert in it passes, and all five files come out.

docs/hardware/samantha_cad.py checks its own geometry before it writes anything: the screw bosses clear the
Pi, both port windows, the card hole, the speaker and the screen; the bezel's screw holes stay off the screen;
the grille leaves a printable web and the shell is one solid; the screen drops into the bezel and the fit
test; and the box, standing in its stand, drops into the slot, shows its whole lit area and leaves room for
the power plug. This check runs it into a scratch folder and fails if it stops, or if any of back_shell.stl,
front_bezel.stl, stand.stl, fit_test.stl or samantha-box.step is missing or empty.

Then it runs a copy with the old stand slot (130.7 mm for a 170.7 mm box) and wants that copy to stop and
write nothing, so the asserts keep running before the export.

It needs uv and build123d in uv's cache. It never downloads: without them (a fresh CI runner) it prints SKIP
and passes.

Revert test, run by hand once (2026-10-11): putting the old line back in samantha_cad.py,
    SL_W, SL_D = OUT_W - 40.0, DEPTH + 0.8
makes this check FAIL with "the slot must take the box's full width with play", and no parts are written.
"""
import os, shutil, subprocess, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
HW = os.path.join(ROOT, "docs", "hardware")
EXPECTED = ["back_shell.stl", "front_bezel.stl", "stand.stl", "fit_test.stl", "samantha-box.step"]
UV = ["uv", "run", "--offline", "--with", "build123d", "python3"]
SLOT = "SL_W, SL_D = OUT_W + 2 * SLOT_CLR, DEPTH + 0.8"
OLD_SLOT = "SL_W, SL_D = OUT_W - 40.0, DEPTH + 0.8"


def tail(text, n=6):
    return "\n".join(text.strip().splitlines()[-n:])


def main():
    if not shutil.which("uv"):
        print("SKIP: samantha-box-check needs uv")
        return 0
    if subprocess.run(UV + ["-c", "import build123d"], capture_output=True).returncode != 0:
        print("SKIP: samantha-box-check needs build123d in uv's cache (run uv run --with build123d once, online)")
        return 0
    tmp = scratch_dir("samantha-box")

    out = os.path.join(tmp, "parts")
    r = subprocess.run(UV + [os.path.join(HW, "samantha_cad.py"), out], capture_output=True, text=True)
    if r.returncode != 0:
        print(tail(r.stderr))
        print("FAIL: samantha_cad.py stopped, see the assert above")
        return 1
    print(r.stdout.strip())
    bad = [f for f in EXPECTED if not os.path.isfile(os.path.join(out, f)) or os.path.getsize(os.path.join(out, f)) == 0]
    if bad:
        print("FAIL: samantha_cad.py did not write", ", ".join(bad))
        return 1
    print("PASS: every fit assert holds and all five files are written")

    # the old slot in a copy: it must stop at its assert, before any file is written
    copy = os.path.join(tmp, "old-slot")
    os.makedirs(copy)
    src = open(os.path.join(HW, "samantha_cad.py")).read()
    if src.count(SLOT) != 1:
        print("FAIL: samantha_cad.py no longer has the line this check swaps for the old slot; update SLOT here")
        return 1
    open(os.path.join(copy, "samantha_cad.py"), "w").write(src.replace(SLOT, OLD_SLOT))
    shutil.copy(os.path.join(HW, "pi4b.py"), copy)
    out = os.path.join(copy, "parts")
    r = subprocess.run(UV + ["samantha_cad.py", out], capture_output=True, text=True, cwd=copy)
    if r.returncode == 0 or "the slot must take the box's full width" not in r.stderr:
        print(tail(r.stderr) or r.stdout.strip())
        print("FAIL: the old 130.7 mm stand slot got past samantha_cad.py's asserts")
        return 1
    if os.path.exists(out):
        print("FAIL: samantha_cad.py wrote parts before its asserts stopped it")
        return 1
    print("PASS: the old stand slot stops the build and nothing is written")
    return 0


if __name__ == "__main__":
    sys.exit(main())
