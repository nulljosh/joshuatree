#!/usr/bin/env python3
"""ARM64 Launchpad (3.8.2): the dock's first tile, Apps, opens a centred cream card with a grid of every app, not the Calculator.

QEMU's virt machine with ramfb and a USB keyboard and mouse behind a hub (the focus check's setup). Through QMP it:
  - clicks the Apps tile: "launchpad open 16", a cream card (388x352 at scale 1) appears, every one of its 16 cells has icon art
    in it and a name under it, the six names with no ARM app yet (Burrow, Mail, Reminders, Weather, Stocks, Trash) are greyed
    (no dark ink in the name strip), and no "calc open" is printed (the Apps tile used to open the Calculator);
  - the arrow keys walk the selection over all 16 names (Right, Left, Down, Up, edges stop), and every name the grid shows is a
    name Spotlight finds when typed ("spotlight: NAME n", n >= 1), so one list drives both; "apps" finds nothing in Spotlight;
  - Esc closes the card and the pixels under it are the desktop again, pixel for pixel; so does a click outside the card;
  - a click on the Calculator cell closes the card and opens the Calculator window (red dot on screen), whose red dot closes it;
  - Enter on Calendar (Right, Right) closes the card and starts the Calendar at EL0; Spotlight still opens the Calculator.
Skips (exit 0) when clang's aarch64 target, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-launchpad-check.py   (from the repo root)
"""
import json, os, re, shutil, socket, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
if subprocess.run(["make", "-C", arch], capture_output=True).returncode:
    print("FAIL: arch/arm64 `make` does not build"); sys.exit(1)
fails = []

W, H = 800, 600                                            # scale 1, the same dock math as the focus check
icon = max(16, min(H * 7 // 100, (min(740, W - 40) - 2 * 10 - 10 * 6) // 11))
dock_x0, dock_y0 = (W - (11 * icon + 10 * 6 + 2 * 10)) // 2, H - icon - 2 * 10 - 24
def slot_centre(slot): return dock_x0 + 10 + slot * (icon + 6) + icon // 2, dock_y0 + 10 + icon // 2
CREAM, RED = (0xfa, 0xf8, 0xf4), (0xff, 0x5f, 0x57)
CARD_W, CARD_H, PAD, TOP, CW, CH, COLS, N = 388, 352, 22, 20, 86, 78, 4, 16
GREYED = ["Burrow", "Mail", "Reminders", "Weather", "Stocks", "Trash"]

tmp = scratch_dir("arm64-launchpad")
log, sock = tmp + "/uart", tmp + "/qmp"
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-nic", "none", "-device", "ramfb",
                      "-device", "pcie-root-port,id=rp,chassis=1", "-device", "qemu-xhci,id=x,bus=rp",
                      "-device", "usb-hub,id=h,bus=x.0,port=1",
                      "-device", "usb-kbd,bus=x.0,port=1.1", "-device", "usb-mouse,id=m1,bus=x.0,port=1.2",
                      "-display", "none", "-serial", "file:" + log, "-qmp", "unix:%s,server,nowait" % sock,
                      "-kernel", os.path.join(arch, "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def uart(): return open(log, errors="replace").read() if os.path.exists(log) else ""
def wait_for(pred, tries=60):
    for _ in range(tries):
        if pred(uart()): return True
        time.sleep(0.1)
    return False
def has(text, after=0): return lambda u: text in u[after:]
try:
    if not wait_for(has("usb ready: 1 kbd, 1 mouse"), 300):
        fails.append(f"the keyboard and mouse behind the hub did not come up, got {uart()[-600:]!r}"); raise SystemExit
    s_ = socket.socket(socket.AF_UNIX); s_.connect(sock); f = s_.makefile("rw"); f.readline()
    def cmd(c, **a):
        f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    cmd("qmp_capabilities")
    time.sleep(0.5)
    def shot():
        path = tmp + "/shot%d.ppm" % time.monotonic_ns()
        cmd("screendump", filename=path)
        for _ in range(30):
            if os.path.exists(path) and os.path.getsize(path) >= W * H * 3: break
            time.sleep(0.1)
        parts = open(path, "rb").read().split(b"\n", 3)
        img = (int(parts[1].split()[0]), parts[3]); os.remove(path); return img
    def px(img, x, y): i = (y * img[0] + x) * 3; return tuple(img[1][i:i + 3])
    def region(img, x, y, w, h): return [px(img, i, j) for j in range(y, y + h) for i in range(x, x + w)]
    def ink(img, box): return sum(1 for c in region(img, *box) if max(c) < 0x80)
    def card_box(img):   # the cream card: the bounding box of cream pixels above the dock
        xs, ys = [], []
        for y in range(26, dock_y0 - 4):
            row = [x for x in range(W) if px(img, x, y) == CREAM]
            if len(row) > 100: xs += [row[0], row[-1]]; ys.append(y)
        if not xs: return None
        h = max(ys) - min(ys) + 1   # the rounded corners trim a row or two off the top and bottom
        return (min(xs), min(ys) - (CARD_H - h) // 2, max(xs) - min(xs) + 1, CARD_H if CARD_H - 4 <= h <= CARD_H else h)
    def cell(box, k): return box[0] + PAD + (k % COLS) * CW, box[1] + TOP + (k // COLS) * CH
    base = shot()
    pos = [W // 2, H // 2]
    def move_to(x, y):
        while (pos[0], pos[1]) != (x, y):
            dx = max(-100, min(100, x - pos[0])); dy = max(-100, min(100, y - pos[1]))
            cmd("input-send-event", events=[{"type": "rel", "data": {"axis": "x", "value": dx}}, {"type": "rel", "data": {"axis": "y", "value": dy}}])
            pos[0] += dx; pos[1] += dy
            if not wait_for(has(f"mouse {pos[0]},{pos[1]}\n"), 40): return False
        return True
    def click(x, y, want):
        n = len(uart())
        if not move_to(x, y): fails.append(f"could not move the pointer to {x},{y}"); return False
        cmd("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": "left"}}])
        cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": "left"}}])
        if not wait_for(has(want, n), 40): fails.append(f"a click at {x},{y} printed no {want!r}, got {uart()[n:][-300:]!r}"); return False
        time.sleep(0.3); return True
    def key(*qcodes):
        cmd("send-key", keys=[{"type": "qcode", "data": k} for k in qcodes]); time.sleep(0.25)
    def sel(want_n, want_name, label):
        if not wait_for(lambda u: re.findall(r"launchpad: (\d+) (\w+)\n", u)[-1:] == [(str(want_n), want_name)], 30):
            fails.append(f"{label}: expected selection {want_n} {want_name}, got {re.findall(r'launchpad: (.*)', uart())[-1:]}"); return False
        return True

    n0 = len(uart())
    ok = click(*slot_centre(0), "launchpad open 16")
    if ok:
        a = shot(); box = card_box(a)
        if not box or (box[2], box[3]) != (CARD_W, CARD_H): fails.append(f"the Launchpad card is not {CARD_W}x{CARD_H} cream, got {box}")
        elif abs((box[0] + box[2] // 2) - W // 2) > 2: fails.append(f"the card is not centred: x {box[0]}, width {box[2]}")
        else:
            print(f"  ok: the Apps tile opens a centred cream card, {box[2]}x{box[3]} at {box[0]},{box[1]}")
            if "calc open" in uart()[n0:]: fails.append("the Apps tile still opens the Calculator")
            else: print("  ok: the Apps tile no longer opens the Calculator")
            bad = []
            for k in range(N):
                cx, cy = cell(box, k)
                if sum(1 for c in region(a, cx + 19, cy + 6, 48, 48) if c != CREAM) < 300: bad.append(k)
            if bad: fails.append(f"cells with no icon art: {bad}")
            else: print(f"  ok: all {N} cells carry icon art")
            names = []
            def step_key(*ks):   # one key at a time (a QMP chord would press them together); wait for the selection line it prints
                for k_ in ks:
                    c0 = len(re.findall(r"launchpad: ", uart())); key(k_); wait_for(lambda u: len(re.findall(r"launchpad: ", u)) > c0, 20)
            for k in range(N):   # walk the selection over every cell: Right along each row, Down and back to the left edge at its end
                if k and k % COLS == 0: step_key("down", *(["left"] * (COLS - 1)))
                elif k: step_key("right")
                got = re.findall(r"launchpad: (\d+) (\w+)\n", uart())[-1:]
                names.append(got[0] if got else None)
            nm = [g[1] for g in names if g]
            if [g[0] for g in names if g] != [str(i) for i in range(1, N + 1)] or len(set(nm)) != N: fails.append(f"the arrow walk did not visit 16 distinct cells in order: {names}")
            else: print("  ok: Right and Down walk the selection over 16 distinct names: " + ", ".join(nm))
            greyed = [nm[k] for k in range(N) if nm[k] in GREYED]
            if sorted(greyed) != sorted(GREYED): fails.append(f"the greyed names are not Burrow, Mail, Reminders, Weather, Stocks, Trash: {greyed}")
            else:
                wrong = []
                a2 = a   # the first frame had cell 0 selected (white name on the accent); judge the rest from it
                for k in range(1, N):
                    cx, cy = cell(box, k); d = ink(a2, (cx + 4, cy + 6 + 48 + 4, CW - 8, 18))
                    if (nm[k] in GREYED) != (d == 0): wrong.append((nm[k], d))
                if wrong: fails.append(f"greyed names must have no dark ink, live ones some: {wrong}")
                else: print("  ok: the six names with no ARM app are greyed, the live ones are dark")
            # every name the grid shows is one Spotlight finds
            key("esc"); wait_for(has("launchpad close", n0), 30)
            missing = []
            for name in nm:
                n = len(uart()); key("f1"); wait_for(has("spotlight open", n), 30)
                for ch in name.lower(): key(ch)
                time.sleep(0.2)
                hit = re.findall(r"spotlight: (\w+) (\d+)\n", uart()[n:])[-1:]
                if not hit or hit[0][0] != name.lower() or int(hit[0][1]) < 1: missing.append(name)
                key("esc"); wait_for(has("spotlight close", n), 30)
            if missing: fails.append(f"names the Launchpad shows that Spotlight does not find: {missing}")
            else: print("  ok: all 16 names are found by Spotlight: one list drives both")
            n = len(uart()); key("f1"); wait_for(has("spotlight open", n), 30)
            for ch in "apps": key(ch)
            time.sleep(0.2)
            if re.findall(r"spotlight: apps (\d+)\n", uart()[n:])[-1:] != ["0"]: fails.append("Spotlight lists Apps, which is the Launchpad itself")
            key("esc"); wait_for(has("spotlight close", n), 30)
            box2 = card_box(shot())
            if box2: fails.append(f"a cream card is still on screen after Esc: {box2}")
            elif region(base, *box) != region(shot(), *box): fails.append("Esc did not put the desktop back pixel for pixel under the card")
            else: print("  ok: Esc closes the card and the pixels under it are the desktop again")
            # edges: selection stops at the corners
            if click(*slot_centre(0), "launchpad open"):
                key("left"); key("up"); k0 = re.findall(r"launchpad: (\d+) ", uart())[-1:]
                for _ in range(8): key("down")
                for _ in range(5): key("right")
                k1 = re.findall(r"launchpad: (\d+) (\w+)\n", uart())[-1:]
                if k0 != ["1"] or k1 != [("16", "Calculator")]: fails.append(f"selection did not stop at the edges: {k0} {k1}")
                else: print("  ok: the selection stops at the first and last cell")
                if not click(30, 100, "launchpad close"): pass
                elif region(base, *box) != region(shot(), *box): fails.append("a click outside the card did not put the desktop back")
                else: print("  ok: a click outside the card closes it, desktop back pixel for pixel")
            # a click on the Calculator cell
            if click(*slot_centre(0), "launchpad open"):
                cx, cy = cell(box, N - 1)
                nn = len(uart())
                if click(cx + 43, cy + 30, "calc open"):
                    c = shot()
                    reds = [(x, y) for y in range(26, dock_y0) for x in range(W // 2, W) if px(c, x, y) == RED]   # the Console's dot is on the left
                    if "launchpad close" not in uart()[nn:] or not reds: fails.append("the Calculator cell did not close the card and open the Calculator")
                    else:
                        print("  ok: a click on the Calculator cell closes the card and opens the Calculator")
                        click(reds[0][0], reds[0][1], "calc closed")
            # Enter on Calendar
            if click(*slot_centre(0), "launchpad open"):
                nn = len(uart())
                key("right"); key("right"); sel(3, "Calendar", "Right, Right")
                key("ret")
                if not wait_for(has("M3c window calendar first frame", nn), 100): fails.append(f"Enter on Calendar did not start it, got {uart()[nn:][-300:]!r}")
                else:
                    print("  ok: Enter on Calendar closes the card and starts it at EL0")
                    key("esc"); wait_for(has("M3c app calendar exit 0", nn), 100)
            # Spotlight still reaches the Calculator
            nn = len(uart()); key("f1"); wait_for(has("spotlight open", nn), 30)
            for ch in "calc": key(ch)
            key("ret")
            if not wait_for(has("calc open", nn), 30): fails.append("Spotlight no longer opens the Calculator")
            else: print("  ok: Spotlight still opens the Calculator")
    if "KERNEL CRASH" in uart(): fails.append("the kernel crashed")
except SystemExit:
    pass
finally:
    q.kill(); q.wait()
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
for m in fails: print("FAIL: " + m)
if fails: sys.exit(1)
print("PASS: the Apps tile opens a Launchpad of every app Spotlight lists; arrows, Enter, Esc and clicks work; the Calculator is one click away")
