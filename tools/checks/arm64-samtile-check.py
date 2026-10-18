#!/usr/bin/env python3
"""ARM64 Samantha dock tile (3.9.8): a click on the Samantha tile (slot 7) opens the Terminal with `sam ` already typed on the ask> line.

Joshua's board test: the tile did nothing he could see (the empty Terminal looks like the Terminal tile's). Headless QEMU virt with
a USB keyboard and mouse behind a hub, like arm64-mouse-check.py. It clicks the Terminal tile (slot 6) and measures the ink on the
prompt row, then clicks the Samantha tile: the row must carry more ink (`sam ` is typed there), Enter must run `sam` with no question
(UART `usage: sam QUESTION`), and a click on the Terminal tile afterwards must not clear what is typed.
Skips (exit 0) when clang, ld.lld or qemu-system-aarch64 is missing. Usage: tools/checks/arm64-samtile-check.py
"""
import json, os, shutil, socket, subprocess, sys, tempfile, time

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
if subprocess.run(["make", "-C", arch], capture_output=True).returncode:
    print("FAIL: arch/arm64 `make` does not build"); sys.exit(1)
fails = []


def geom(w, h):
    s = 2 if h >= 1080 else 1; lw, lh = w // s, h // s
    icon = max(16, min(lh * 7 // 100, (min(740, lw - 40) - 2 * 10 - 10 * 6) // 11))
    dock_w = 11 * icon + 10 * 6 + 2 * 10; x0 = (lw - dock_w) // 2; y0 = lh - icon - 2 * 10 - 24
    return s, icon, x0, y0


def slot_centre(w, h, slot):
    s, icon, x0, y0 = geom(w, h)
    return (x0 + 10 + slot * (icon + 6) + icon // 2) * s, (y0 + 10 + icon // 2) * s


tmp = tempfile.mkdtemp(prefix="jt-samtile-")
log, sock = tmp + "/uart", tmp + "/qmp"
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-nic", "none", "-device", "ramfb",
                      "-device", "pcie-root-port,id=rp,chassis=1", "-device", "qemu-xhci,id=x,bus=rp",
                      "-device", "usb-hub,id=h,bus=x.0,port=1",
                      "-device", "usb-kbd,bus=x.0,port=1.1", "-device", "usb-mouse,id=m1,bus=x.0,port=1.2",
                      "-display", "none", "-serial", "file:" + log, "-qmp", "unix:%s,server,nowait" % sock,
                      "-kernel", os.path.join(arch, "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def uart(): return open(log, errors="replace").read() if os.path.exists(log) else ""


def wait_for(text, tries=100, start=0):
    for _ in range(tries):
        if text in uart()[start:]: return True
        time.sleep(0.1)
    return False


try:
    if not wait_for("usb ready: 1 kbd, 1 mouse", 300):
        fails.append(f"the keyboard and mouse did not come up, got {uart()[-600:]!r}")
    else:
        s_ = socket.socket(socket.AF_UNIX); s_.connect(sock); f = s_.makefile("rw"); f.readline()

        def cmd(c, **a):
            f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
            while True:
                r = json.loads(f.readline())
                if "return" in r or "error" in r: return r
        cmd("qmp_capabilities"); time.sleep(0.5)

        def shot():
            p = tmp + "/s%d.ppm" % time.monotonic_ns(); cmd("screendump", filename=p); time.sleep(0.4)
            parts = open(p, "rb").read().split(b"\n", 3); w, h = map(int, parts[1].split()); return w, h, parts[3]

        def px(img, x, y): i = (y * img[0] + x) * 3; return tuple(img[2][i:i + 3])
        base = shot(); W, H = base[0], base[1]; pos = [W // 2, H // 2]

        def move_to(x, y):
            while (pos[0], pos[1]) != (x, y):
                dx = max(-100, min(100, x - pos[0])); dy = max(-100, min(100, y - pos[1]))
                cmd("input-send-event", events=[{"type": "rel", "data": {"axis": "x", "value": dx}}, {"type": "rel", "data": {"axis": "y", "value": dy}}])
                pos[0] += dx; pos[1] += dy
                if not wait_for(f"mouse {pos[0]},{pos[1]}\n", 40): return False
            return True

        def click():
            cmd("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": "left"}}])
            cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": "left"}}])
        win_w, win_h = 500 * H // 600, 350 * H // 600; wx, wy = (W - win_w) // 2, 100 * H // 600
        sc = lambda v: v * H // 600

        def prompt_ink():
            img = shot()
            return sum(1 for y in range(wy + sc(32 + 18 * 16), wy + sc(32 + 19 * 16)) for x in range(wx + 8, wx + win_w - 4) if max(px(img, x, y)) < 0x90)

        n0 = len(uart())
        if not move_to(*slot_centre(W, H, 6)): fails.append("could not move onto the Terminal tile")
        else:
            click()
            if not wait_for("terminal open", 30, n0): fails.append("the Terminal tile printed no 'terminal open'")
            else:
                empty = prompt_ink()
                n1 = len(uart())
                if not move_to(*slot_centre(W, H, 7)): fails.append("could not move onto the Samantha tile")
                else:
                    click()
                    if not wait_for("dock Samantha: sam ready", 30, n1):
                        fails.append(f"the Samantha tile did not bring up the Terminal, got {uart()[-300:]!r}")
                    else:
                        typed = prompt_ink()
                        if typed < empty + 15: fails.append(f"the Samantha tile left the prompt empty ({empty} ink pixels before, {typed} after)")
                        else: print(f"  ok: the Samantha tile opens the Terminal with `sam ` typed ({empty} -> {typed} ink pixels on the prompt row)")
                        n2 = len(uart())
                        cmd("input-send-event", events=[{"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "ret"}}}])
                        cmd("input-send-event", events=[{"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "ret"}}}])
                        if not wait_for("usage: sam QUESTION", 40, n2): fails.append(f"Enter on the prefilled line did not run `sam`, got {uart()[-300:]!r}")
                        else: print("  ok: Enter on the prefilled line runs `sam` (its usage line)")
    if "KERNEL CRASH" in uart(): fails.append("the kernel crashed")
finally:
    q.kill(); q.wait()
print("PASS" if not fails else "FAIL: " + "; ".join(fails))
sys.exit(1 if fails else 0)
