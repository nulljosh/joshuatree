#!/usr/bin/env python3
"""ARM64 system clipboard and the Cmd keys (headless QEMU virt, own private scratch).

The Cmd key is the keyboard's Super/Windows key (QMP qcode meta_l). One boot, every key through the virtio keyboard:
  1. Spotlight opens Notes, `n` starts a note, "pasteok" is typed, Cmd+A selects it (Notes says edsel=0,7), Cmd+C copies
     it (the kernel says CLIPCOPY:7: user/notes.c reached the one kernel buffer through JT_SYS_CLIPBOARD, 399).
     Cmd+N then starts a second note (a second `notes: new=`), the first saved on the way.
  2. Spotlight opens the Terminal; Cmd+V puts the text on the input line (CLIPPASTE:7) and Enter runs it, which the
     shell can only answer with "zsh: command not found: pasteok", so the text itself crossed from Notes to the Terminal.
  3. Cmd+X on a typed line cuts it (CLIPCOPY:3) and Cmd+V puts it back (CLIPPASTE:3): the Terminal's own line works.
  4. Cmd+W closes the Terminal ("terminal closed") and, with Notes in front again, closes Notes the way its red dot does.
Usage: tools/checks/arm64-clipboard-check.py   (from the repo root)
"""
import json, os, shutil, socket, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
from sdcard import build_card

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
TMP = scratch_dir("clipboard")
subprocess.run(["make", "-s", "-C", os.path.join(root, "arch/arm64"), "kernel8.elf"], check=True, capture_output=True)
log, sock, card = os.path.join(TMP, "uart"), os.path.join(TMP, "qmp"), os.path.join(TMP, "card.img")
build_card(card, False)   # Notes keeps its notes on the card: without one, `n` finds no folder and no note opens
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-nic", "none",
    "-global", "virtio-mmio.force-legacy=false", "-device", "ramfb", "-device", "virtio-keyboard-device",
    "-device", "virtio-tablet-device", "-drive", "if=none,id=card,file=%s,format=raw" % card,
    "-device", "virtio-blk-device,drive=card", "-display", "none", "-serial", "file:" + log,
    "-qmp", "unix:" + sock + ",server,nowait", "-kernel", os.path.join(root, "arch/arm64/kernel8.elf")],
    stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
s = f = None
fails = []


def uart(): return open(log, errors="replace").read() if os.path.exists(log) else ""


def command(name, args=None):
    f.write(json.dumps({"execute": name, **({"arguments": args} if args else {})}) + "\n"); f.flush()
    while True:
        reply = json.loads(f.readline())
        assert "error" not in reply, reply
        if "return" in reply: return reply["return"]


def key(*names):
    command("send-key", {"keys": [{"type": "qcode", "data": n} for n in names], "hold-time": 60}); time.sleep(.25)


def wait_for(text, secs=20, after=0):
    end = time.time() + secs
    while time.time() < end:
        if text in uart()[after:]: return True
        time.sleep(.1)
    return False


def check(name, ok):
    print(("  ok: " if ok else "  FAIL: ") + name)
    if not ok: fails.append(name)


def spot(word):
    key("f1")
    for c in word: key(c)
    key("ret")


try:
    for _ in range(300):
        if "M2 input ready" in uart(): break
        assert q.poll() is None, "QEMU exited"
        time.sleep(.1)
    else: raise AssertionError("desktop did not boot")
    s = socket.socket(socket.AF_UNIX); s.settimeout(10); s.connect(sock); f = s.makefile("rw"); f.readline()
    command("qmp_capabilities")
    start = len(uart())
    spot("notes")
    check("Notes opened", wait_for("M3c window notes first frame"))
    time.sleep(1)
    key("n")
    for c in "pasteok": key(c)
    key("meta_l", "a")
    check("Cmd+A selects the text in Notes", wait_for("edsel=0,7", 5))
    key("meta_l", "c")
    check("Cmd+C reaches the kernel clipboard (CLIPCOPY:7)", wait_for("CLIPCOPY:7", 5))
    news = uart().count("notes: new=")
    key("meta_l", "n")
    end = time.time() + 5
    while time.time() < end and uart().count("notes: new=") <= news: time.sleep(.1)
    check("Cmd+N in the editor starts a second note", uart().count("notes: new=") > news)
    mark = len(uart())
    spot("terminal")
    check("Terminal opened", wait_for("terminal open", 10, mark) or wait_for("focus terminal", 2, mark))
    mark = len(uart())
    key("meta_l", "v")
    check("Cmd+V in the Terminal pastes 7 bytes (CLIPPASTE:7)", wait_for("CLIPPASTE:7", 5, mark))
    key("ret")
    check("the pasted text ran as the line: zsh: command not found: pasteok", wait_for("zsh: command not found: pasteok", 10, mark))
    for c in "abc": key(c)
    key("meta_l", "x")
    check("Cmd+X cuts the Terminal's line (CLIPCOPY:3)", wait_for("CLIPCOPY:3", 5, mark))
    key("meta_l", "v")
    check("Cmd+V puts it back (CLIPPASTE:3)", wait_for("CLIPPASTE:3", 5, mark))
    mark = len(uart())
    key("meta_l", "w")
    check("Cmd+W closes the Terminal", wait_for("terminal closed", 5, mark))
    mark = len(uart())
    spot("notes")
    wait_for("focus app", 5, mark)
    mark = len(uart())
    key("meta_l", "w")
    check("Cmd+W closes Notes like its red dot", wait_for("M3c app notes closed by its red dot", 10, mark))
    check("no EL0 fault and no kernel crash since Notes opened", not any(w in uart()[start:] for w in ("KERNEL CRASH", "EL0 fault")))
finally:
    if f: f.close()
    if s: s.close()
    if q.poll() is None: q.terminate()
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill(); q.wait()
    q.stderr.close()
if fails:
    print("FAIL: " + "; ".join(fails)); print(uart()[-6000:]); sys.exit(1)
print("PASS: Cmd+A, C, X, V work in Notes and the Terminal through one kernel clipboard, and Cmd+W closes the front window")
