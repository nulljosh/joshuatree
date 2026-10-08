#!/usr/bin/env python3
"""Every vendored BearSSL .c file compiles for aarch64 freestanding.

Static: no QEMU. Fails on the first clang error. Guards the Pi TLS client
subset in third_party/bearssl against a header or shim regression.
"""
import glob
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, "third_party", "bearssl", "src")
CMD = ["clang", "-target", "aarch64-none-elf", "-ffreestanding",
       "-fno-stack-protector", "-mgeneral-regs-only", "-O2",
       "-Ithird_party/bearssl/inc", "-Ithird_party/bearssl/src", "-Ilib",
       "-Ithird_party/bearssl/shim", "-c", "-o", os.devnull]

files = sorted(glob.glob(os.path.join(SRC, "*.c")))
if len(files) < 60:
    sys.exit(f"bearssl-tls-check: only {len(files)} .c files, expected the TLS subset")
bad = 0
for f in files:
    r = subprocess.run(CMD + [os.path.relpath(f, ROOT)], cwd=ROOT,
                       capture_output=True, text=True)
    if r.returncode:
        bad += 1
        print(f"FAIL {os.path.relpath(f, ROOT)}\n{r.stderr.strip()}")
if bad:
    sys.exit(f"bearssl-tls-check: {bad}/{len(files)} files failed")
print(f"bearssl-tls-check: {len(files)} files compile for aarch64 freestanding")
