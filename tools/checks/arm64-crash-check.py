#!/usr/bin/env python3
"""ARM64 crash screen: an unexpected exception at EL1 ends in a report on the UART and a red panel on the screen, then a quiet halt.

Builds `make -C arch/arm64 crashtest` (main.c's CRASH_TEST hook stores to unmapped memory right after the framebuffer is
up) and boots it on QEMU virt with ramfb and on the raspi4b model. For each, it checks:
  - the UART carries the report in order: "KERNEL CRASH: data abort, translation fault level 1", the ESR with its class
    (0x25), the FAR (the address stored to), the ELR, "last console lines:" with the newest lines, then the halt line;
  - the kernel stays halted: nothing is printed after it, and the boot banner appears once (no reset loop, no silent hang
    before the report);
  - a QMP screendump shows the panel: its red background, white text in the header row and the log rows, and the desktop
    around it untouched (so the panel was drawn over the screen, not a wipe of it).
  - source: the crash path cleans the data cache over the panel (a real GPU reads RAM) and uses neither the TrueType
    rasterizer nor the console (integer 8x16 VGA font only).
Skips (exit 0) when clang's aarch64 target, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-crash-check.py   (from the repo root)
"""
import json, os, shutil, socket, subprocess, sys, tempfile, time

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
if subprocess.run(["make", "-C", arch, "crashtest"], capture_output=True).returncode:
    print("FAIL: arch/arm64 `make crashtest` does not build"); sys.exit(1)
fails = []

main_c = open(os.path.join(arch, "main.c")).read()
i = main_c.find("static void crash(const struct frame *f) {"); body = main_c[i:main_c.find("\n}\n", i)] if i >= 0 else ""
if "fb_flush(" not in body: fails.append("main.c crash() does not clean the data cache over the panel: a real Pi would not show it")
elif "text_draw(" in body or "console_putc(" in body or "uart_puts(" in body: fails.append("main.c crash() uses the rasterizer, the console or uart_puts: it must stay integer-only and independent of them")
else: print("  ok: crash() cleans the cache over the panel and uses only the VGA font and the raw UART")

def run(name, qemu_args, image, size=(800, 600)):
    tmp = tempfile.mkdtemp()
    log, sock, shot = tmp + "/uart", tmp + "/qmp", tmp + "/shot.ppm"
    q = subprocess.Popen(["qemu-system-aarch64", *qemu_args, "-display", "none", "-serial", "file:" + log,
                          "-qmp", "unix:%s,server,nowait" % sock, "-kernel", os.path.join(arch, image)],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        out = ""
        for _ in range(150):
            time.sleep(0.1)
            out = open(log, errors="replace").read() if os.path.exists(log) else ""
            if "halted:" in out: break
        time.sleep(1.5)   # a kernel that keeps running (or resets) after the report would print more
        out2 = open(log, errors="replace").read()
        lines = out2.splitlines()
        want = ["M1c fb ok", "CRASHTEST: storing to unmapped memory", "KERNEL CRASH: data abort, translation fault level 1"]
        at = -1
        for w in want:
            nxt = next((k for k in range(at + 1, len(lines)) if lines[k] == w), -1)
            if nxt < 0: fails.append(f"{name}: missing or out of order UART line {w!r}, got {out2[-500:]!r}"); return
            at = nxt
        tail = lines[at + 1:]
        if not (tail and tail[0].startswith("ESR 0x") and "EC 0x0000000000000025" in tail[0]): fails.append(f"{name}: no ESR line with class 0x25 after the headline: {tail[:2]}"); return
        if "FAR 0x0000000300000000" not in tail: fails.append(f"{name}: FAR line missing or wrong (stored to 0x300000000): {tail[:4]}"); return
        if not any(t.startswith("ELR 0x") for t in tail): fails.append(f"{name}: no ELR line"); return
        j = tail.index("last console lines:") if "last console lines:" in tail else -1
        if j < 0 or "M1c fb ok" not in tail[j:] or "CRASHTEST: storing to unmapped memory" not in tail[j:]: fails.append(f"{name}: the last console lines are missing from the report: {tail}"); return
        if not tail[-1].startswith("halted:"): fails.append(f"{name}: the kernel kept printing after the crash report: {tail[-3:]}"); return
        if out2.count("Joshua Tree on ARM64") != 1: fails.append(f"{name}: the boot banner printed {out2.count('Joshua Tree on ARM64')} times: the kernel reset"); return
        print(f"  ok: {name} UART report: class, ESR, FAR, ELR, last console lines, halt line, then silence")
        s = socket.socket(socket.AF_UNIX); s.connect(sock); f = s.makefile("rw")
        f.readline()
        def cmd(c, **a):
            f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
            while True:
                r = json.loads(f.readline())
                if "return" in r or "error" in r: return r
        cmd("qmp_capabilities")
        r = cmd("screendump", filename=shot)
        if "error" in r: fails.append(f"{name}: screendump failed: {r['error']}"); return
        time.sleep(0.3)
        parts = open(shot, "rb").read().split(b"\n", 3); w, h = map(int, parts[1].split()); px = parts[3]
        if (w, h) != size: fails.append(f"{name}: screen is {w}x{h}, want {size[0]}x{size[1]}"); return
        pix = lambda x, y: tuple(px[(y * w + x) * 3:(y * w + x) * 3 + 3])
        s_ = max(1, (h * 10 // 600 + 5) // 10)                      # the kernel's panel rule: 16s, 32s, W-32s wide, 14 rows of 16s
        pxl, pyl, pw, ph = 16 * s_, 32 * s_, w - 32 * s_, 16 * s_ * 14
        for what, (x, y), c in (("panel background", (pxl + pw - 4, pyl + ph - 4), (0x90, 0, 0)), ("panel corner", (pxl + 1, pyl + 1), (0x90, 0, 0)),
                                ("desktop beside the panel", (8, pyl + ph // 2), (0x20, 0x30, 0x40))):
            got = pix(x, y)
            if c and got != c: fails.append(f"{name}: {what} at {(x, y)}: got {got}, want {c}"); return
        def white(y0, y1): return sum(1 for y in range(y0, y1) for x in range(pxl, pxl + pw) if pix(x, y) == (255, 255, 255))
        head = white(pyl + 8 * s_, pyl + 24 * s_)                   # row 0: the headline
        logr = white(pyl + 8 * s_ + 5 * 16 * s_, pyl + ph - 8 * s_) # rows 5 on: the log lines and the halt line
        if head < 40 or logr < 100: fails.append(f"{name}: panel text not drawn ({head} white pixels in the headline row, {logr} in the log rows)"); return
        print(f"  ok: {name} screendump shows the red panel over the desktop with its text ({head} headline pixels, {logr} log pixels)")
    finally:
        q.kill(); q.wait()
        shutil.rmtree(tmp, ignore_errors=True)

run("virt ramfb", ["-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-device", "ramfb"], "crash-kernel8.elf")
if "raspi4b" in subprocess.run(["qemu-system-aarch64", "-machine", "help"], capture_output=True, text=True).stdout:
    run("pi mailbox", ["-machine", "raspi4b"], "crash-kernel8.img")
else:
    print("  QEMU here has no raspi4b model (needs QEMU 9 or newer), Pi crash screen skipped")
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
for m in fails: print("FAIL: " + m)
if fails: sys.exit(1)
print("PASS: an unexpected EL1 fault prints a crash report on the UART (class, ESR, FAR, ELR, the last console lines), draws it as a panel on the framebuffer, and halts quietly, on virt and on the Pi 4B model")
