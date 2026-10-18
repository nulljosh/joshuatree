#!/usr/bin/env python3
"""ARM64 tree menu (the mark and "Joshua Tree" at the left of the menu bar): About, Restart, Shut Down, Log Out.

Part 1, QEMU virt with ramfb, a USB keyboard and mouse behind a hub, virtio-net and a slow fake relay. Through QMP it:
  - clicks the tree: the cream card opens under the bar (pixels), four rows hold text, row 0 is terracotta;
  - Down moves the terracotta to row 1; Esc puts the desktop back pixel for pixel; F10 and Ctrl+Option+T open it again;
    a click outside dismisses it; a click on the mark while it is open closes it;
  - About: Enter shows the version, any key closes it;
  - Restart by a mouse click and Shut Down by Up/Down/Enter reach the same Enter-confirms prompt as the chords, Esc cancels;
    Enter on a confirmed Restart reaches power_apply (the virt build has no watchdog and says so);
  - Log Out with a Terminal and a Calculator open closes both and leaves the bare wallpaper;
  - mid-question (`sam` waiting on the relay) Restart and Shut Down refuse politely and arm nothing; after the answer they ask.
Part 2, QEMU raspi4b (no USB; the TREEMENU_SELFTEST builds click the tree and press the keys through the board's own input
path): Restart reboots the model (the banner prints twice) and Shut Down halts it (QEMU exits 0).
Skips (exit 0) when clang, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-treemenu-check.py   (from the repo root)
"""
import http.server, json, os, shutil, socket, subprocess, sys, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
from freeport import free_port

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + (("  (" + str(detail)[-300:] + ")") if detail and not ok else ""))
    if not ok: fails.append(name)

W, H = 800, 600
icon = max(16, min(H * 7 // 100, (min(740, W - 40) - 2 * 10 - 10 * 6) // 11))
dock_x0, dock_y0 = (W - (11 * icon + 10 * 6 + 2 * 10)) // 2, H - icon - 2 * 10 - 24
def slot_centre(slot): return dock_x0 + 10 + slot * (icon + 6) + icon // 2, dock_y0 + 10 + icon // 2
MB = 26 * H // 540                       # the menu bar, sg(26)
MARK = (12 * H // 540, MB // 2)          # the mark's centre
CX, CY, CW = 8, MB + 2, 220              # the card on the logical grid (main.c TREE_W)
ROW_Y = [CY + 6, CY + 37, CY + 59, CY + 81]
CREAM, TERRA = (0xfa, 0xf8, 0xf4), (0xb5, 0x50, 0x2c)
WALL = (0x6e, 0x6e, 0x6e)   # the graphite grey wallpaper, the default since 3.9.0; terracotta is the selection, not the wall
wx, wy, ww, wh = (W - 500) // 2, 100, 500, 350   # the Console's and the Terminal's frame

tmp = scratch_dir("arm64-treemenu")
TOKEN = "arm-treemenu-check-token-0123456789"
token_file = tmp + "/token"
with open(token_file, "w") as f: f.write(TOKEN + "\n")
os.chmod(token_file, 0o600)

# a relay that sits on every question until released: the Terminal is then busy with a `sam` request
release = threading.Event()
class Relay(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        self.rfile.read(int(self.headers.get("Content-Length", "0")))
        release.wait(60)
        body = b"S 12345678-1234-1234-1234-123456789abc\nAll done."
        self.send_response(200); self.send_header("Content-Type", "text/plain"); self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close"); self.end_headers(); self.wfile.write(body)
    def log_message(self, *a): pass
class Quiet(http.server.ThreadingHTTPServer):
    daemon_threads = True
relay = Quiet(("127.0.0.1", free_port()), Relay)
threading.Thread(target=relay.serve_forever, daemon=True).start()

def build(*targets, env=None):
    r = subprocess.run(["make", "-C", arch, *targets], env=env, capture_output=True, text=True, timeout=600)
    if r.returncode: print("FAIL: arch/arm64 does not build %s:\n%s" % (targets, (r.stdout + r.stderr)[-1500:])); sys.exit(1)

q = None
try:
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
    env = dict({k: v for k, v in os.environ.items() if not k.startswith("CLAUDE_RELAY")}, CLAUDE_RELAY_HOST="10.0.2.2",
               CLAUDE_RELAY_PORT=str(relay.server_address[1]), CLAUDE_RELAY_TOKEN_FILE=token_file)
    build(env=env)
    log, sock = tmp + "/uart", tmp + "/qmp"
    q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256",
                          "-global", "virtio-mmio.force-legacy=false", "-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n",
                          "-device", "ramfb", "-device", "pcie-root-port,id=rp,chassis=1", "-device", "qemu-xhci,id=x,bus=rp",
                          "-device", "usb-hub,id=h,bus=x.0,port=1",
                          "-device", "usb-kbd,bus=x.0,port=1.1", "-device", "usb-mouse,id=m1,bus=x.0,port=1.2",
                          "-display", "none", "-serial", "file:" + log, "-qmp", "unix:%s,server,nowait" % sock,
                          "-kernel", os.path.join(arch, "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def uart(): return open(log, errors="replace").read() if os.path.exists(log) else ""
    def wait_for(text, secs=6, after=0):
        end = time.time() + secs
        while time.time() < end:
            if text in uart()[after:]: return True
            time.sleep(0.05)
        return False
    if not wait_for("usb ready: 1 kbd, 1 mouse", 40) or not wait_for("net dhcp 10.0.2.15", 40):
        check("the keyboard, mouse and network came up", False, uart()[-600:]); raise SystemExit
    s_ = socket.socket(socket.AF_UNIX); s_.settimeout(30); s_.connect(sock); f = s_.makefile("rw"); f.readline()
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
        return int(parts[1].split()[0]), parts[3]
    def px(img, x, y): i = (y * img[0] + x) * 3; return tuple(img[1][i:i + 3])
    def region(img, x, y, w, h): return [px(img, i, j) for j in range(y, y + h) for i in range(x, x + w)]
    CARD = (CX, CY, CW, 112)
    def same(img, ref):   # the card's box is the same pixels as the reference, bar the pointer arrow's own box
        a, b = region(img, *CARD), region(ref, *CARD)
        bad = [(i % CW + CX, i // CW + CY) for i in range(len(a)) if a[i] != b[i] and not (abs(i % CW + CX - pos[0]) < 14 and 0 <= i // CW + CY - pos[1] < 24)]
        return not bad, bad[:5]
    def row_ink(img, r):   # pixels in the row's text band unlike the row's own background
        bg = px(img, CX + 12, ROW_Y[r] + 3)
        return sum(1 for c in region(img, CX + 22, ROW_Y[r] + 2, 150, 16) if c != bg)
    pos = [W // 2, H // 2]
    def move_to(x, y):
        while (pos[0], pos[1]) != (x, y):
            dx = max(-100, min(100, x - pos[0])); dy = max(-100, min(100, y - pos[1]))
            cmd("input-send-event", events=[{"type": "rel", "data": {"axis": "x", "value": dx}}, {"type": "rel", "data": {"axis": "y", "value": dy}}])
            pos[0] += dx; pos[1] += dy
            if not wait_for("mouse %d,%d\n" % (pos[0], pos[1]), 4): return False
        return True
    def click(x, y):
        move_to(x, y)
        cmd("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": "left"}}])
        cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": "left"}}])
        time.sleep(0.15)
    def keys(*qcodes):
        for k in qcodes: cmd("send-key", keys=[{"type": "qcode", "data": k}]); time.sleep(0.15)
    def chord(*qcodes): cmd("send-key", keys=[{"type": "qcode", "data": k} for k in qcodes]); time.sleep(0.2)
    def step(text, action, secs=6):
        n = len(uart()); action(); return wait_for(text, secs, n)

    # The Console is up at boot on virt; close it so the desktop is bare wallpaper for the pixel comparisons.
    step("console closed", lambda: click(wx + 24, wy + 16))
    move_to(600, 500); time.sleep(0.4)
    base = shot()
    ok = step("treemenu open", lambda: click(*MARK))
    check("a click on the tree opens the menu", ok, uart()[-300:])
    a = shot()
    check("the card is cream, rounded, under the bar", px(a, CX + CW - 6, CY + 60) == CREAM and px(a, CX + CW // 2, CY + 100) == CREAM
          and region(a, *CARD) != region(base, *CARD), (px(a, CX + CW - 6, CY + 60), px(a, CX + CW // 2, CY + 100)))
    check("the card's corners keep the wallpaper (rounded)", px(a, CX, CY) == WALL, px(a, CX, CY))
    check("all four rows hold text", all(row_ink(a, r) > 15 for r in range(4)), [row_ink(a, r) for r in range(4)])
    check("row 0 is the terracotta selection, row 1 is not", px(a, CX + 12, ROW_Y[0] + 3) == TERRA and px(a, CX + 12, ROW_Y[1] + 3) == CREAM)
    check("a hairline sits between About and Restart", px(a, CX + 100, CY + 31) != CREAM, px(a, CX + 100, CY + 31))
    ok = step("treemenu: row 1", lambda: keys("down"))
    b = shot()
    check("Down moves the selection to Restart", ok and px(b, CX + 12, ROW_Y[1] + 3) == TERRA and px(b, CX + 12, ROW_Y[0] + 3) == CREAM)
    ok = step("treemenu close", lambda: keys("esc"))
    time.sleep(0.2); c = shot()
    check("Esc closes it and the desktop is back pixel for pixel", ok and same(c, base)[0], same(c, base))
    ok = step("treemenu open", lambda: keys("f10"))
    check("F10 opens it from the keyboard", ok)
    ok = step("treemenu close", lambda: click(400, 300))
    time.sleep(0.2)
    time.sleep(0.2); h = shot()
    check("a click outside dismisses it", ok and same(h, base)[0], (ok, same(h, base), uart()[-200:]))
    ok = step("treemenu open", lambda: chord("ctrl", "alt", "t"))
    check("Ctrl+Option+T opens it from the keyboard", ok)
    ok = step("treemenu close", lambda: click(*MARK))
    check("a click on the mark while it is open closes it", ok)

    # About
    step("treemenu open", lambda: keys("f10"))
    ok = step("treemenu: about", lambda: keys("ret"))
    d = shot()
    ver = open(os.path.join(root, "VERSION")).read().strip()
    check("About opens a card with the version line", ok and region(d, CX + 16, CY + 32, 150, 16) != region(base, CX + 16, CY + 32, 150, 16), ver)
    ok = step("treemenu close", lambda: keys("x"))
    check("any key closes About", ok)

    # Restart by mouse, Shut Down by keys, both stop at the chords' own prompt
    step("treemenu open", lambda: click(*MARK))
    ok = step("Restart? Unsaved", lambda: click(CX + 60, ROW_Y[1] + 10))
    check("clicking Restart asks the chords' question in the Terminal", ok, uart()[-300:])
    ok = step("Power action cancelled.", lambda: keys("esc"))
    check("Esc cancels it", ok)
    step("treemenu open", lambda: keys("f10"))
    ok = step("Shut down? Unsaved", lambda: keys("down", "down", "ret"))
    check("Down, Down, Enter on Shut Down asks the same", ok, uart()[-300:])
    ok = step("Power action cancelled.", lambda: keys("esc"))
    check("Esc cancels that too", ok)
    step("treemenu open", lambda: keys("f10"))
    n = len(uart())
    keys("down", "ret")
    ok = wait_for("Restart? Unsaved", 6, n)
    ok2 = step("power: restart (this build has no power control)", lambda: keys("ret"))
    check("Enter on the prompt reaches power_apply (virt has no watchdog)", ok and ok2, uart()[-300:])

    # Log Out closes every window
    click(*slot_centre(6)); wait_for("terminal open", 4)
    keys("f1"); wait_for("spotlight open", 4); keys("c", "a", "l", "c", "ret"); wait_for("calc open", 4)   # 4.1.2: the Apps tile is the Launchpad, the Calculator comes from Spotlight
    e = shot()
    check("a Terminal and a Calculator are on screen first", px(e, wx + 19, wy + 16) == (0xff, 0x5f, 0x57), px(e, wx + 19, wy + 16))
    step("treemenu open", lambda: keys("f10"))
    n = len(uart())
    keys("up", "ret")
    ok = wait_for("treemenu: logout", 6, n) and wait_for("calc closed", 4, n) and wait_for("terminal closed", 4, n)
    time.sleep(0.4); g = shot()
    check("Log Out closes the Calculator and the Terminal", ok, uart()[n:][-400:])
    check("and leaves bare wallpaper where they were", all(c == WALL for c in region(g, wx + 8, wy + 34, ww - 16, wh - 60)))
    check("with the card gone from the corner", same(g, base)[0], same(g, base))

    # Busy: a question in flight refuses both power rows
    step("terminal open", lambda: keys("f2"))
    for ch in "sam hi": keys("spc" if ch == " " else ch)
    keys("ret"); time.sleep(1.0)
    step("treemenu open", lambda: keys("f10"))
    n = len(uart())
    keys("down", "ret")
    ok = wait_for("Wait for Samantha to finish, then try again.", 6, n)
    check("Restart mid-answer is refused politely", ok and "Restart? Unsaved" not in uart()[n:], uart()[n:][-300:])
    step("treemenu open", lambda: keys("f10"))
    n = len(uart())
    keys("down", "down", "ret")
    ok = wait_for("Wait for Samantha to finish, then try again.", 6, n)
    check("Shut Down mid-answer is refused politely", ok and "Shut down? Unsaved" not in uart()[n:], uart()[n:][-300:])
    n = len(uart()); keys("ret")
    time.sleep(0.5)
    check("and nothing was armed: Enter does not reach power_apply", "power: " not in uart()[n:], uart()[n:][-200:])
    release.set()
    check("the answer still arrives", wait_for("All done.", 20))
    time.sleep(0.5)
    step("treemenu open", lambda: keys("f10"))
    n = len(uart())
    keys("down", "ret")
    check("after the answer Restart asks again", wait_for("Restart? Unsaved", 6, n), uart()[n:][-300:])
    if "KERNEL CRASH" in uart(): check("the kernel did not crash", False, uart()[-300:])
except SystemExit:
    pass
finally:
    release.set()
    if q: q.kill(); q.wait()
    relay.shutdown()
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)

# Part 2: the Pi model, real watchdog
if not fails:
    try:
        build("treerestart-kernel8.img", "treeshutdown-kernel8.img", env={k: v for k, v in os.environ.items() if not k.startswith("CLAUDE_RELAY")})
        for name, action in (("treerestart", "restart"), ("treeshutdown", "shutdown")):
            plog = "%s/%s.log" % (tmp, name)
            p = subprocess.Popen(["qemu-system-aarch64", "-machine", "raspi4b", "-serial", "null", "-display", "none", "-serial", "file:" + plog,
                                  "-kernel", os.path.join(arch, name + "-kernel8.img")], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            try:
                end, out, done = time.monotonic() + 60, "", False
                while time.monotonic() < end:
                    out = open(plog, errors="replace").read() if os.path.exists(plog) else ""
                    if action == "shutdown" and p.poll() is not None: done = p.returncode == 0; break
                    if action == "restart" and out.count("Joshua Tree on ARM64") >= 2: done = p.poll() is None; break
                    if p.poll() is not None: break
                    time.sleep(0.1)
                check("raspi4b %s through the tree menu (click, Esc, F10, rows, Enter, Enter)" % action,
                      done and "treemenu: esc survived" in out and out.count("treemenu open") == 2 and ("treemenu: " + action) in out
                      and ("power: " + action) in out, out[-500:])
            finally:
                if p.poll() is None: p.terminate()
                p.wait(timeout=10); p.stderr.close()
    finally:
        subprocess.run(["make", "-C", arch, "clean"], capture_output=True)

if fails:
    print("FAIL: %d tree menu checks failed" % len(fails)); sys.exit(1)
print("PASS: the tree menu opens on a click or F10, shows four rows, closes on Esc or an outside click, About shows the version, "
      "Restart and Shut Down ask the chords' question and refuse mid-answer, Log Out closes every window, raspi4b restarts and halts")
