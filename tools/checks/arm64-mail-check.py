#!/usr/bin/env python3
"""ARM64 Mail: Spotlight opens user/mailpi.c at EL0 in its own window; Down moves in the list, Enter fills the reading pane.

Headless QEMU virt with ramfb and a virtio keyboard, the spotlight check's setup. No card, so Mail shows its built-in
sample messages and says so. It checks, from the UART and from QMP screendumps of the window's content:
  - F1, "mail", Enter starts the app: "M3c window mail WxH at X,Y", "mail: n=6", "mail: samples=1";
  - first frame: the mailboxes (accent glyphs), five samples in the Inbox list, one in Sent, the first row
    selected, and a reading pane with only its faded hint line (no dark ink, no message);
  - Down: "mail: sel=1", the list changes (the selection fill moves) and the reading pane does not;
  - Enter: "mail: read=1", the reading pane fills with a subject, sender and wrapped body (dark ink > 400 pixels), the
    unread dot of the opened row is gone, and the list pane's text is otherwise unchanged;
  - Right: "mail: box=1" (Unread), the list is redrawn and the pane cleared; Esc: the app closes ("mail: closed").
No KERNEL CRASH, no EL0 fault. Skips (exit 0) when clang's aarch64 target, ld.lld or qemu-system-aarch64 is missing.
Set MAIL_SHOT=path.ppm to keep the screendump with the message open.
Usage: tools/checks/arm64-mail-check.py   (from the repo root)
"""
import json, os, re, shutil, socket, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
if subprocess.run(["make", "-s", "-C", arch, "kernel8.elf"], capture_output=True).returncode:
    print("FAIL: arch/arm64 `make kernel8.elf` does not build"); sys.exit(1)
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok or not detail else " -- " + str(detail)[-400:]))
    if not ok: fails.append(name)

SIDE_W, LIST_W = 112, 196
tmp = scratch_dir("arm64-mail")
log, sock = tmp + "/uart", tmp + "/qmp"
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-nic", "none",
                      "-global", "virtio-mmio.force-legacy=false", "-device", "ramfb", "-device", "virtio-keyboard-device",
                      "-device", "virtio-tablet-device", "-display", "none", "-serial", "file:" + log,
                      "-qmp", "unix:%s,server,nowait" % sock, "-kernel", os.path.join(arch, "kernel8.elf")],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def uart(): return open(log, errors="replace").read() if os.path.exists(log) else ""
def wait_for(text, secs=8.0, after=0):
    end = time.time() + secs
    while time.time() < end:
        if text in uart()[after:]: return True
        time.sleep(0.05)
    return False
f = None
try:
    for _ in range(300):
        if "M2 input ready" in uart(): break
        if q.poll() is not None: print("FAIL: QEMU exited"); sys.exit(1)
        time.sleep(0.1)
    else: print("FAIL: the desktop did not boot"); sys.exit(1)
    s = socket.socket(socket.AF_UNIX); s.settimeout(20); s.connect(sock); f = s.makefile("rw"); f.readline()
    def cmd(name, args=None):
        f.write(json.dumps({"execute": name, **({"arguments": args} if args else {})}) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    def key(name): cmd("send-key", {"keys": [{"type": "qcode", "data": name}], "hold-time": 50}); time.sleep(0.25)
    def shot():
        path = os.path.join(tmp, "s%d.ppm" % time.monotonic_ns())
        cmd("screendump", {"filename": path})
        for _ in range(60):
            if os.path.exists(path) and os.path.getsize(path) > 15: break
            time.sleep(0.05)
        time.sleep(0.1)
        head, wh, _, pix = open(path, "rb").read().split(b"\n", 3)
        w, h = map(int, wh.split()); return path, w, h, pix
    cmd("qmp_capabilities")
    key("f1")
    for c in "mail": key(c)
    key("ret")
    up = wait_for("mail: ring-3 window") and wait_for("M3c window mail ") and wait_for("mail: samples=1\n")
    out = uart()
    m = re.search(r"M3c window mail (\d+)x(\d+) at (\d+),(\d+)\n", out)
    check("Spotlight opens Mail: the app starts at EL0 in its own window, with the six sample messages", up and m and "mail: n=6\n" in out, out[-500:])
    if not m: raise SystemExit(1)
    cw, ch, cx, cy = map(int, m.groups())
    time.sleep(0.6)
    def crop(img, x0, y0, w, h):
        _, W, _, pix = img
        return [pix[((cy + y0 + j) * W + cx + x0) * 3:((cy + y0 + j) * W + cx + x0 + w) * 3] for j in range(h)]
    def count(rows, pred):
        return sum(1 for r in rows for i in range(0, len(r), 3) if pred(r[i], r[i + 1], r[i + 2]))
    dark = lambda r, g, b: max(r, g, b) < 0x70
    accent = lambda r, g, b: abs(r - 0xB5) < 12 and abs(g - 0x50) < 12 and abs(b - 0x2C) < 12
    RX = SIDE_W + LIST_W + 2
    a = shot()
    side, lst, pane = (0, 0, SIDE_W, 90), (SIDE_W + 1, 0, LIST_W, ch), (RX, 0, cw - RX, ch)
    check("first frame: accent glyphs in the mailboxes, accent unread dots, the reading pane has only its hint",
          count(crop(a, *side), accent) > 30 and count(crop(a, *lst), accent) > 40 and count(crop(a, *pane), dark) == 0,
          (count(crop(a, *side), accent), count(crop(a, *lst), accent), count(crop(a, *pane), dark)))
    key("down")
    ok_sel = wait_for("mail: sel=1\n")
    time.sleep(0.4)
    b = shot()
    check("Down moves the selection (mail: sel=1) and the list is redrawn", ok_sel and crop(a, *lst) != crop(b, *lst))
    check("Down alone leaves the reading pane as it was", crop(a, *pane) == crop(b, *pane))
    key("ret")
    ok_read = wait_for("mail: read=1\n")
    time.sleep(0.5)
    c = shot()
    pa, pc = count(crop(b, *pane), dark), count(crop(c, *pane), dark)
    check("Enter opens the message (mail: read=1) and the reading pane fills with its subject, sender and body", ok_read and pc > 400 and pc > pa + 300, (pa, pc))
    check("the opened row's unread dot is gone", count(crop(c, *lst), accent) < count(crop(b, *lst), accent), (count(crop(b, *lst), accent), count(crop(c, *lst), accent)))
    if os.environ.get("MAIL_SHOT"): shutil.copy(c[0], os.environ["MAIL_SHOT"])
    key("right")
    ok_box = wait_for("mail: box=1\n")
    time.sleep(0.4)
    d = shot()
    check("Right changes to Unread (mail: box=1): the list is redrawn and the reading pane is cleared", ok_box and crop(d, *lst) != crop(c, *lst) and count(crop(d, *pane), dark) == 0)
    key("esc")
    check("Esc closes Mail", wait_for("mail: closed\n") and wait_for("M3c app mail exit 0"), uart()[-300:])
    out = uart()
    for bad in ("KERNEL CRASH", "M3 EL0 fault", "M3c app mail faulted"):
        check("never printed %r" % bad, bad not in out.partition("M3c app mail starts")[2] if bad == "M3 EL0 fault" else bad not in out, out[-300:])
finally:
    if f: f.close()
    if q.poll() is None: q.terminate()
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill(); q.wait()
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)

if fails:
    print("FAIL: %d of the Mail checks failed" % len(fails)); sys.exit(1)
print("PASS: Spotlight opens Mail at EL0, Down moves in the list, Enter fills the reading pane, Right changes mailbox, Esc closes")
