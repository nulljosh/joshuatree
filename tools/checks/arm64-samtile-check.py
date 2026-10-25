#!/usr/bin/env python3
"""ARM64 Samantha dock tile (3.9.8): a click on the Samantha tile (slot 7) opens the Terminal with `sam ` already typed on the ask> line;
since 4.20.0 the Terminal fills the screen from the menu bar to the dock, so the chat view draws her big face.

Joshua's board test: the tile did nothing he could see (the empty Terminal looks like the Terminal tile's). Headless QEMU virt with
a USB keyboard and mouse behind a hub, like arm64-mouse-check.py. It clicks the Terminal tile (slot 6) and measures the ink on the
prompt row, then clicks the Samantha tile: the row must carry more ink (`sam ` is typed there), Enter must run `sam` with no question
(UART `tui| open`, the chat view; it was a usage line before 4.2.0), deleting `sam ` changes one text row, and `sam` draws the big face (only a window of 21 rows or more gets it).
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

        def key(*names):
            for k in names:
                for down in (True, False):
                    cmd("input-send-event", events=[{"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": k}}}])
                time.sleep(0.1)

        n0 = len(uart())
        if not move_to(*slot_centre(W, H, 6)): fails.append("could not move onto the Terminal tile")
        else:
            click()
            if not wait_for("terminal open", 30, n0): fails.append("the Terminal tile printed no 'terminal open'")
            else:
                n1 = len(uart())
                if not move_to(*slot_centre(W, H, 7)): fails.append("could not move onto the Samantha tile")
                else:
                    click()
                    if not wait_for("dock Samantha: sam ready", 30, n1):
                        fails.append(f"the Samantha tile did not bring up the Terminal, got {uart()[-300:]!r}")
                    elif "dock Samantha: full" not in uart()[n1:]:
                        fails.append("the Samantha tile did not open the Terminal across the screen (no 'dock Samantha: full')")
                    else:
                        time.sleep(0.5); typed = shot(); key("backspace", "backspace", "backspace", "backspace"); time.sleep(0.5); empty = shot()
                        ys = [y for y in range(H) for x in range(0, W, 2) if px(typed, x, y) != px(empty, x, y)]
                        if len(ys) < 15: fails.append(f"the Samantha tile left the prompt empty ({len(ys)} pixels changed by deleting it)")
                        elif max(ys) - min(ys) > sc(20): fails.append(f"deleting `sam ` changed more than one row (y {min(ys)} to {max(ys)})")
                        else: print(f"  ok: the Samantha tile opens the Terminal across the screen with `sam ` typed (prompt row at y {min(ys)})")
                        n2 = len(uart())
                        key("s", "a", "m", "ret")
                        if not wait_for("tui| open", 40, n2): fails.append(f"Enter on the prompt did not run `sam`, got {uart()[-300:]!r}")
                        elif not wait_for("face: on, big", 40, n2): fails.append(f"the full-screen chat view did not draw the big face, got {uart()[-300:]!r}")
                        else: print("  ok: `sam` opens the chat view with the big face")
                        if os.environ.get("SAMTILE_SHOT"):   # a PPM of the chat view, for the release notes
                            time.sleep(1); cmd("screendump", filename=os.environ["SAMTILE_SHOT"]); time.sleep(0.4)
    if "KERNEL CRASH" in uart(): fails.append("the kernel crashed")
finally:
    q.kill(); q.wait()
print("PASS" if not fails else "FAIL: " + "; ".join(fails))
sys.exit(1 if fails else 0)
