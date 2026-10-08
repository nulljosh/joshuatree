#!/usr/bin/env python3
"""ARM64 Calculator: arch/arm64/calc.c's evaluator and keypad, on a table of sums.

Compiles the same calc.c the kernel links (freestanding, -fno-builtin, no libm) on the host with a tiny driver, and
checks each expression's displayed result, in radians and in degrees. Then drives the keypad itself (calc_press):
memory keys, the Deg toggle, Del and C. Also builds the kernel and checks calc.o asks for nothing outside itself.
Skips (exit 0) when clang is missing.
Usage: tools/checks/arm64-calc-check.py   (from the repo root)
"""
import os, shutil, subprocess, sys, tempfile

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
arch = os.path.join(root, "arch/arm64")
if not shutil.which("clang"):
    print("SKIP: clang not installed"); sys.exit(0)

# (expression, degrees mode, what the display must say)
TABLE = [
    ("sin(30deg)", 0, "0.5"), ("sin(30)", 1, "0.5"), ("cos(60)", 1, "0.5"), ("tan(45)", 1, "1"),
    ("asin(0.5)", 1, "30"), ("acos(0)", 1, "90"), ("atan(1)*4", 0, "3.14159265"), ("sin(pi)", 0, "0"),
    ("5!", 0, "120"), ("2^10", 0, "1024"), ("2^3^2", 0, "512"), ("-2^2", 0, "-4"), ("2^-1", 0, "0.5"), ("2^0.5", 0, "1.41421356"),
    ("1/0", 0, "Error"), ("0/0", 0, "Error"), ("5%0", 0, "Error"), ("7%3", 0, "1"),
    ("12*(3+4)", 0, "84"), ("10/2", 0, "5"), ("1+2*3", 0, "7"), ("(1+2)*3", 0, "9"), ("0.1+0.2", 0, "0.3"),
    ("ln(e)", 0, "1"), ("log(1000)", 0, "3"), ("exp(1)", 0, "2.71828183"), ("exp(0)", 0, "1"), ("sqrt(2)", 0, "1.41421356"),
    ("sqrt(-1)", 0, "Error"), ("ln(0)", 0, "Error"), ("asin(2)", 0, "Error"), ("tan(90)", 1, "Error"), ("171!", 0, "Error"),
    ("(1+2", 0, "Error"), ("1+2)", 0, "Error"), ("2 $ 3", 0, "Error"), ("foo(1)", 0, "Error"),
    ("2^60", 0, "1.152922e18"), ("1/3", 0, "0.33333333"), ("0.0000001", 0, "1e-7"),
    ("6*7", 0, "42"), ("ans+1", 0, "43"),
]
DRIVER = r'''
#include <stdio.h>
#include <string.h>
int calc_eval_str(const char *src, int deg, char *out);
int calc_press(const char *k);
int calc_type(int ch);
const char *calc_input(void);
const char *calc_output(void);
int calc_flags(void);
int main(int argc, char **argv) {
    char out[32];
    for (int i = 1; i + 1 < argc; i += 2) { calc_eval_str(argv[i + 1], argv[i][0] == '1', out); printf("%s\n", out); }
    /* the keypad: 9 M+ C 4 M- C MR = shows 5; MC drops the M flag; Deg flips the mode; typed keys and Del */
    const char *keys[] = {"9", "M+", "C", "4", "M-", "C", "MR", "=", 0};
    for (int i = 0; keys[i]; i++) calc_press(keys[i]);
    printf("mem %s %d\n", calc_output(), calc_flags() >> 2 & 1);
    calc_press("MC"); printf("mc %d\n", calc_flags() >> 2 & 1);
    calc_press("C"); calc_press("Sci"); calc_press("Deg"); calc_press("sin"); calc_type('9'); calc_type('0'); calc_type('1');
    calc_type('\b'); calc_type(')'); calc_type('\n');
    printf("deg %s = %s %d\n", calc_input(), calc_output(), calc_flags() & 3);
    return 0;
}
'''
fails = []

def boot_calctest():
    """The calctest build on QEMU virt, headless: the Calculator opens at boot and four sums are typed into it. The
    kernel's own FPU must give the same answers on the UART, and a QMP screendump must show the window's terracotta
    "=" key. A path as the first argument keeps the screendump (a .ppm)."""
    import json, socket, time
    want = ["calc: sin(30deg) = 0.5", "calc: 5! = 120", "calc: 2^10 = 1024", "calc: 1/0 = Error"]
    with tempfile.TemporaryDirectory() as t:
        log, sock, shot = t + "/uart", t + "/qmp", (sys.argv[1] if len(sys.argv) > 1 else t + "/shot.ppm")
        # bounded: the finally below kills it, 30 s at most
        q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256",
                              "-display", "none", "-device", "ramfb", "-serial", "file:" + log, "-qmp", f"unix:{sock},server,nowait",
                              "-kernel", os.path.join(arch, "calc-kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        out = ""
        try:
            for _ in range(300):
                time.sleep(0.1)
                out = open(log, errors="replace").read() if os.path.exists(log) else ""
                if want[-1] in out: break
            missing = [w for w in want if w not in out]
            if missing:
                q.kill()
                fails.append(f"calctest boot: missing {missing}, UART tail {out[-300:]!r}, qemu said {q.stderr.read()[-300:]!r}"); return
            print("  ok: on QEMU the kernel's own FPU gives " + ", ".join(w[6:] for w in want))
            s = socket.socket(socket.AF_UNIX); s.connect(sock); f = s.makefile("rw"); f.readline()
            for c in ({"execute": "qmp_capabilities"}, {"execute": "screendump", "arguments": {"filename": shot}}):
                f.write(json.dumps(c) + "\n"); f.flush(); f.readline()
            time.sleep(0.3)
            data = open(shot, "rb").read()
            parts = data.split(b"\n", 3)   # P6, size, maxval, pixels
            px = parts[3]
            accent = sum(1 for i in range(0, len(px) - 2, 3) if abs(px[i] - 0xb5) < 6 and abs(px[i + 1] - 0x50) < 6 and abs(px[i + 2] - 0x2c) < 6)
            if accent < 300: fails.append(f"calctest screendump: only {accent} terracotta pixels, the Calculator window is not on screen")
            else: print(f"  ok: the screendump shows the Calculator ({accent} pixels of its terracotta = key and error text)")
        finally:
            q.kill(); q.wait()

with tempfile.TemporaryDirectory() as t:
    drv, exe = os.path.join(t, "drv.c"), os.path.join(t, "calc")
    open(drv, "w").write(DRIVER)
    cc = ["clang", "-O2", "-ffreestanding", "-fno-builtin", "-fno-math-errno", "-Wall", "-Wextra", "-Werror", "-c",
          os.path.join(arch, "calc.c"), "-o", os.path.join(t, "calc.o")]
    r = subprocess.run(cc, capture_output=True, text=True)
    if r.returncode: print("FAIL: calc.c does not compile on the host:\n" + r.stderr); sys.exit(1)
    r = subprocess.run(["clang", drv, os.path.join(t, "calc.o"), "-o", exe], capture_output=True, text=True)
    if r.returncode: print("FAIL: the driver does not link:\n" + r.stderr); sys.exit(1)
    args = [a for e, d, _ in TABLE for a in (str(d), e)]
    lines = subprocess.run([exe] + args, capture_output=True, text=True, timeout=30).stdout.splitlines()
for (e, d, want), got in zip(TABLE, lines):
    if got != want: fails.append(f"{e}{' (degrees)' if d else ''} shows {got!r}, wanted {want!r}")
    else: print(f"  ok: {e}{' (degrees)' if d else ''} = {got}")
tail = lines[len(TABLE):]
for want in ("mem 5 1", "mc 0", "deg sin(90) = 1 3"):
    if want in tail: print("  ok: keypad " + want)
    else: fails.append(f"keypad: wanted {want!r}, got {tail!r}")

# The kernel build: calc.o must link into the real image and need nothing outside itself (no libm, no stray libc call).
if shutil.which("ld.lld") and subprocess.run(["clang", "-target", "aarch64-none-elf", "-c", "-x", "c", "/dev/null", "-o", "/dev/null"], capture_output=True).returncode == 0:
    if subprocess.run(["make", "-C", arch, "kernel8.elf"], capture_output=True).returncode: fails.append("arch/arm64 `make` does not build")
    else:
        nm = shutil.which("llvm-nm") or shutil.which("nm")
        und = subprocess.run([nm, "-u", os.path.join(arch, "calc.o")], capture_output=True, text=True).stdout.split()
        und = [u for u in und if u not in ("U",)]
        if und: fails.append(f"calc.o needs symbols from outside: {und}")
        else: print("  ok: the kernel links calc.o and it needs no outside symbol (no libm)")
    if shutil.which("qemu-system-aarch64") and not subprocess.run(["make", "-C", arch, "calctest"], capture_output=True).returncode:
        boot_calctest()
for m in fails: print("FAIL: " + m)
if fails: sys.exit(1)
print(f"PASS: the ARM Calculator gets all {len(TABLE)} sums right (sin(30deg)=0.5, 5!=120, 2^10=1024, 1/0 is an error) and its memory, Deg and edit keys work")
