#!/usr/bin/env python3
"""Proof that a crash report names the actual function that faulted, not
just the exception kind: boots with the "panictest" multiboot command-line
flag (kernel/kernel.c's kmain), which -- and only which -- calls
kernel/backtrace.c's jt_panic_test_target() right after idt_install(). That
function's sole job is to int3 itself. A normal boot never sets this flag,
so this path can never fire outside this check.

Asserts the serial log's crash report (kernel/idt.c's isr_handler) names
jt_panic_test_target from the symbol table built into the kernel by the
two-pass build (Makefile's kernel.elf.pass1 -> tools/gen/gen_symtab.py ->
kernel/symtab.c), not just "CPU exception: breakpoint".

Usage: tools/checks/panic-symbols-check.py   (from the repo root, after
make kernel.elf)
"""
import subprocess, sys, time

log = "/tmp/jt-panic-symbols-serial.log"

def main():
    q = subprocess.Popen(
        ["qemu-system-i386", "-name", "jt-panicsymtest", "-kernel", "kernel.elf",
         "-append", "panictest", "-display", "none", "-no-reboot",
         "-serial", "file:" + log],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    serial = ""
    try:
        for _ in range(50):
            time.sleep(0.2)
            try:
                serial = open(log, errors="replace").read()
            except FileNotFoundError:
                continue
            if "exception: ring-0" in serial:
                break
    finally:
        q.terminate()
        try:
            q.wait(timeout=5)
        except subprocess.TimeoutExpired:
            q.kill()
            q.wait(timeout=5)

    fails = []
    if "exception: ring-0 breakpoint, halted" not in serial:
        fails.append("no ring-0 breakpoint exception on serial (panictest flag never fired)")
    if "panic in jt_panic_test_target" not in serial:
        fails.append("crash report did not name jt_panic_test_target (symbol table lookup failed)")
    if "at jt_panic_test_target+0x" not in serial:
        fails.append("backtrace's own first frame did not name jt_panic_test_target")
    # A second frame naming kmain (the real caller) proves the EBP chain
    # walk works, not just the direct EIP lookup.
    if "at kmain+0x" not in serial:
        fails.append("backtrace did not walk up to kmain, the real caller (EBP chain walk broken)")

    if fails:
        print("FAIL: " + "; ".join(fails))
        print("--- serial log ---")
        print(serial[-2000:])
        return 1
    print("PASS: crash report names jt_panic_test_target by symbol, with a real caller frame above it")
    return 0

if __name__ == "__main__":
    sys.exit(main())
