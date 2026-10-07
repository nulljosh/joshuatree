#!/usr/bin/env python3
"""One private scratch directory per check run.

Checks that boot QEMU write a serial log and a framebuffer dump. When those
lived at fixed /tmp paths, two suites running at once overwrote each other's
files and failed at random. scratch_dir() makes a fresh tempfile.mkdtemp()
that only this process knows about and removes it on exit, even after a
SystemExit. Set JT_KEEP_TMP=1 to keep it (the path is printed) for debugging.
"""
import atexit, os, shutil, sys, tempfile


def scratch_dir(name):
    """Return a new private directory for the check called `name`."""
    path = tempfile.mkdtemp(prefix=f"jt-{name}-")
    if os.environ.get("JT_KEEP_TMP") == "1":
        print(f"JT_KEEP_TMP=1: keeping {path}", file=sys.stderr)
    else:
        atexit.register(shutil.rmtree, path, ignore_errors=True)
    return path
