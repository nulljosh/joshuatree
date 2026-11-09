#!/usr/bin/env python3
"""lldb on QEMU's gdb stub, both ports (13.5.0): each kernel boots paused, lldb stops it at its first C function and reads the
program counter there. To debug by hand, run the same three commands: QEMU with -S -gdb tcp:127.0.0.1:1234, then in lldb
`target create kernel.elf` and `gdb-remote 127.0.0.1:1234`.

Skips (exit 0) when lldb is missing, as on the CI runners. Discriminating: a kernel with no symbols, or a stub that never stops,
leaves no breakpoint stop and no program counter inside the entry function.
"""
import os, re, shutil, subprocess, sys
from freeport import free_port

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
if not shutil.which("lldb"):
    print("SKIP: lldb not installed"); sys.exit(0)

subprocess.run(["make", "-s", "-C", ROOT, "kernel.elf"], check=True)
subprocess.run(["make", "-s", "-C", os.path.join(ROOT, "arch/arm64"), "kernel8.elf"], check=True)

PORTS = (
    ("i386", ["qemu-system-i386", "-kernel", "kernel.elf"], "kernel.elf", "kmain", "eip"),
    ("arm64", ["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-kernel", "arch/arm64/kernel8.elf"],
     "arch/arm64/kernel8.elf", "main", "pc"),
)
fails = []
for name, qemu, elf, entry, reg in PORTS:
    port = free_port()
    q = subprocess.Popen(qemu + ["-display", "none", "-S", "-gdb", f"tcp:127.0.0.1:{port}"], cwd=ROOT,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        try:
            out = subprocess.run(["lldb", "-b", "-o", f"target create {elf}", "-o", f"gdb-remote 127.0.0.1:{port}",
                                  "-o", f"breakpoint set -n {entry}", "-o", "continue", "-o", f"register read {reg}"],
                                 cwd=ROOT, capture_output=True, text=True, timeout=60).stdout
        except subprocess.TimeoutExpired:
            out = "lldb never came back: the kernel did not stop at the breakpoint"
    finally:
        q.kill(); q.wait()
    stopped = "stop reason = breakpoint" in out
    read = re.search(rf"\b{reg} = 0x0*[1-9a-f][0-9a-f]* .*{entry}", out)
    print(f"{name}: stopped at {entry}={'yes' if stopped else 'NO'}  {reg} reads inside it={'yes' if read else 'NO'}")
    if not (stopped and read):
        fails.append(name); print(out[-800:])
if fails:
    print("FAIL:", ", ".join(fails)); sys.exit(1)
print("PASS: lldb stops both kernels at their entry through QEMU's gdb stub and reads the program counter")
