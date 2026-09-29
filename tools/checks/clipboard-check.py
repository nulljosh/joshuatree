#!/usr/bin/env python3
"""Headless proof of the system-wide clipboard (roadmap.md: "Clipboard
copy/paste"). Same QMP absolute-pointer + qcode-keyboard shape
app-interact-check.py already uses for real interaction QA.

No selection model exists yet (docs/roadmap.md's own "Text selection and
undo in editors" is still an open item), so the honest, useful contract
this proves is: Ctrl+C copies the current line (Notes) or the current
input line (Terminal); Ctrl+X cuts it; Ctrl+V pastes at the cursor, in
every text field that already accepts typed input, respecting that
field's own max length -- never overflowing it.

Three real scenarios, each verified by grepping a discriminating serial
marker ("CLIPCOPY:<len>:<hash>" / "CLIPPASTE:<len>:<hash>" / "CLIPTRUNC", never the text) that
kernel/kernel.c's clipboard_set()/clip_serial_dump() emit at the exact
moment the clipboard is set or a paste lands -- proof the text actually
arrived, not just that a key was sent:

  1. Type a line in Notes, Ctrl+C, Ctrl+V pastes it again right after
     itself on the same line -- proves copy-then-paste round-trips real
     text through the one global buffer.
  2. Type a line in Notes, Ctrl+X (cut), open Terminal, Ctrl+V into the
     terminal input line, Enter runs it as a real shell command (echo) --
     proves the SAME buffer crosses from one app to a completely
     different one.
  3. Type a string longer than Terminal's TERM_COLS-1 input limit into
     Notes, Ctrl+C, switch to Terminal, Ctrl+V -- proves the paste
     truncates cleanly at the field's own limit (CLIPTRUNC marker) and
     inserts exactly TERM_COLS-1 bytes, never overflowing input[].

Usage: tools/checks/clipboard-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time, tempfile

REPO = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
os.chdir(REPO)
ARTIFACTS = tempfile.mkdtemp(prefix="jt-clipboard-")
LOG = os.path.join(ARTIFACTS, "serial.log")
DUMP = os.path.join(ARTIFACTS, "framebuffer.raw")
FB = 0xfd000000; W, H = 1920, 1080
SOCKET = os.path.join(ARTIFACTS, "qmp.sock")
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
NOTES_SLOT, TERMINAL_SLOT = 4, 6
CLOSE_X, CLOSE_Y = 94, 56
CLOSE_RED = (0xFF, 0x5F, 0x57)
PARK = (480, 200)

for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

# "cliptrace" makes the kernel add the content hash to its CLIPCOPY/CLIPPASTE
# lines (a normal boot logs the length only, see clip_trace in kernel.c).
q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                      "-append", "cliptrace",
                      "-qmp", f"unix:{SOCKET},server,nowait", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

def m(t):  # matches clip_serial_dump: "<len>:<fnv1a32 hex>", never the text
    h = 2166136261
    for c in t.encode(): h = ((h ^ c) * 16777619) & 0xFFFFFFFF
    return f"{len(t)}:{h:08x}"

fails = []
try:
    s = None
    for _ in range(50):
        time.sleep(0.2)
        candidate = socket.socket(socket.AF_UNIX)
        try:
            candidate.connect(SOCKET)
            candidate.settimeout(10)
            s = candidate
            break
        except OSError:
            candidate.close()
    if s is None: raise SystemExit("FAIL: QEMU's QMP socket never came up")
    f = s.makefile("rw")
    def cmd(o):
        f.write(json.dumps(o) + "\n"); f.flush()
        while True:
            line = f.readline()
            if not line:
                if o['execute'] == 'quit': return {}
                raise ConnectionError('QEMU disconnected before replying')
            r = json.loads(line)
            if "error" in r: raise RuntimeError(r["error"])
            if "return" in r: return r
    f.readline()
    cmd({"execute": "qmp_capabilities"})

    def wait_desktop_up(deadline=20.0, quiet_for=0.35):
        """drivers/window.c's window_present() writes a "present\\r\\n"
        line to serial every frame it actually flips to the screen, capped
        at 256 lines for the whole boot (window.c's own present_logged
        guard). The boot splash plus the first several idle-desktop frames
        burn through that cap in about two seconds of real time, after
        which the count is pinned for good -- a one-shot, discriminating
        signal that boot has gotten well past the splash and into the idle
        desktop, not a guess at how long that takes. Poll the serial log
        until the count stops growing for `quiet_for` seconds instead of
        blindly sleeping past the splash on a slow host. `deadline` is a
        generous floor so a loaded CI runner that's genuinely still
        booting doesn't get cut short; it only makes a real hang take
        longer to fail, never a false pass."""
        start = time.time(); last_count = -1; last_change = start
        while time.time() - start < deadline:
            try:
                count = open(LOG, "rb").read().count(b"present\r\n")
            except FileNotFoundError:
                count = 0
            now = time.time()
            if count != last_count:
                last_count = count; last_change = now
            elif count > 0 and now - last_change >= quiet_for:
                return
            time.sleep(0.05)
    wait_desktop_up()

    def move(x, y):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
    def click():
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
        time.sleep(0.1)
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
    def pixel(x, y):
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        from PIL import Image
        img = Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
        return img.getpixel((x * SCALE + 1, y * SCALE + 1))
    def is_red(p): return max(abs(p[i] - CLOSE_RED[i]) for i in range(3)) <= 12
    def window_open(): return is_red(pixel(CLOSE_X, CLOSE_Y))
    centre = lambda slot: SLOT0_X + slot * PITCH + DOCK_ICON // 2

    QCODE = {" ": "spc", ".": "dot", "-": "minus", "/": "slash", "\n": "ret", "\b": "backspace"}
    def key(c):
        codes = QCODE.get(c, 'shift-' + c.lower() if c.isupper() else c).split('-')
        cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": code} for code in codes], "hold-time": 30}})
        time.sleep(0.08)
    def keys(*qcodes):
        cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k} for k in qcodes], "hold-time": 30}})
        time.sleep(0.15)
    def type_str(s):
        for c in s: key(c)
    def ctrl(letter):
        keys("ctrl", letter); time.sleep(0.15)

    def open_slot(slot):
        move(centre(slot), ICON_ROW_Y); time.sleep(0.3)
        click(); time.sleep(1.2)
    def close_via_x():
        move(CLOSE_X, CLOSE_Y); time.sleep(0.3)
        click(); time.sleep(0.8)
        move(*PARK); time.sleep(0.3)

    def serial_text():
        with open(LOG, "rb") as fh:
            return fh.read().decode("latin1")
    def wait_marker(marker, timeout=30.0):
        """Poll the serial log until the exact marker this step expects
        has landed (or give up after `timeout` s). Notes processes queued
        keystrokes one redraw at a time (kernel/editor.h's main loop pops
        exactly one scancode per iteration before redrawing), so by the
        time Ctrl+C/X/V is sent the editor can still be a second behind
        the typing on a laptop and several seconds behind on a shared,
        loaded CI runner; a fixed sleep, or waiting for "any new serial
        output" (window_present logs a line per frame), both read the log
        before the marker exists.
        The scancode ring (irq.c's kbd_buf) preserves strict FIFO order
        and every keystroke this check sends (including Ctrl+C/X/V) goes
        through the same QMP connection one at a time, so the marker is
        never in doubt, only *when* it lands: on GitHub's shared runners a
        real run (CI job 107600495751, 2026-09-24) sent scenario 3's
        116-character line, then Ctrl+C, and the exact CLIPCOPY marker for
        the full line still hadn't appeared after the old flat 15s here --
        not a dropped or reordered keystroke, just a loaded host taking
        longer to drain 116 queued scancodes (each with its own line
        redraw) than a quiet one. 15s was tuned against short (~15-19
        char) lines in scenarios 1-2 and never re-checked against
        scenario 3's much longer one. Doubled as a general floor, and
        scenario 3 below passes an explicit larger timeout on top of
        that scaled to its own line length -- never a shorter one, so
        this can only get slower to fail, not more likely to pass on a
        real bug (a wrong hash, or a marker that plain never gets
        written, still exhausts the deadline and fails exactly as
        before)."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            log = serial_text()
            if marker in log: return log
            time.sleep(0.05)
        return serial_text()

    move(*PARK); time.sleep(0.5)

    # ---- 1: Notes, type a line, Ctrl+C, Ctrl+V pastes it again ----
    open_slot(NOTES_SLOT)
    if not window_open(): fails.append("Notes: dock click did not open a window")
    else:
        type_str("clip-roundtrip")
        ctrl("c")
        log = wait_marker("CLIPCOPY:" + m("clip-roundtrip"))
        ctrl("v")
        log = wait_marker("CLIPPASTE:" + m("clip-roundtrip"))
        if "CLIPCOPY:" + m("clip-roundtrip") not in log:
            fails.append("Notes Ctrl+C: CLIPCOPY marker with the typed line not found in serial log")
        elif "CLIPPASTE:" + m("clip-roundtrip") not in log:
            fails.append("Notes Ctrl+V: CLIPPASTE marker with the copied text not found in serial log")
        else:
            print("Notes    : typed a line, Ctrl+C copied it, Ctrl+V pasted it back (serial-verified)")
        close_via_x()

    # ---- 2: Notes, type + Ctrl+X (cut), Terminal Ctrl+V + Enter runs it ----
    open_slot(NOTES_SLOT)
    if not window_open(): fails.append("Notes: dock click did not open a window (scenario 2)")
    else:
        # Notes keeps its buffer in RAM across a close/reopen within the same
        # boot (editor.h's own documented contract), so scenario 1's text is
        # still there; Enter starts a fresh line so Ctrl+X below cuts only
        # the line this scenario actually types.
        keys("ret")
        type_str("echo cross-app-clip")
        ctrl("x")
        log = wait_marker("CLIPCOPY:" + m("echo cross-app-clip"))
        if "CLIPCOPY:" + m("echo cross-app-clip") not in log:
            fails.append("Notes Ctrl+X: CLIPCOPY marker with the cut line not found in serial log")
        close_via_x()

    open_slot(TERMINAL_SLOT)
    if not window_open(): fails.append("Terminal: dock click did not open a window")
    else:
        ctrl("v")
        log = wait_marker("CLIPPASTE:" + m("echo cross-app-clip"))
        if "CLIPPASTE:" + m("echo cross-app-clip") not in log:
            fails.append("Terminal Ctrl+V: CLIPPASTE marker with the cut Notes line not found in serial log")
        else:
            print("Terminal : cut a line in Notes, pasted it into Terminal's input line (serial-verified)")
        # Runs it as a real command too -- Terminal's output isn't
        # serial-logged, but a hung/garbled paste would leave the prompt
        # broken; Enter here proves the pasted bytes are real, typeable
        # input the shell accepts, not just bytes sitting in input[].
        keys("ret"); time.sleep(0.4)
        close_via_x()

    # ---- 3: paste bigger than Terminal's input limit truncates cleanly ----
    TERM_COLS = 96  # kernel/kernel.c's own #define; input[] holds TERM_COLS-1 chars plus the trailing nul
    long_text = "x" * (TERM_COLS + 20)
    # Scaled, not just bumped: scenario 1-2's lines are ~15-19 chars and the
    # 30s default (see wait_marker's own comment) already covers those with
    # real room to spare. This line is 116 chars, each its own editor.h
    # keystroke + full-line redraw before Ctrl+C is even in the ring buffer
    # -- proportionally more real guest-side work to drain on a loaded
    # runner, so it gets proportionally more patience: 0.3s/char on top of
    # the 30s floor is 15s of extra slack here, comfortably above the ~9s
    # this check's own per-key typing delay already spends sending it.
    long_timeout = 30.0 + len(long_text) * 0.3
    open_slot(NOTES_SLOT)
    if not window_open(): fails.append("Notes: dock click did not open a window (scenario 3)")
    else:
        keys("ret")
        type_str(long_text)
        ctrl("c")
        log = wait_marker("CLIPCOPY:" + m(long_text), timeout=long_timeout)
        if ("CLIPCOPY:" + m(long_text)) not in log:
            fails.append("Notes Ctrl+C: CLIPCOPY marker with the long line not found in serial log")
        close_via_x()

    open_slot(TERMINAL_SLOT)
    if not window_open(): fails.append("Terminal: dock click did not open a window (scenario 3)")
    else:
        ctrl("v")
        expect_paste = "CLIPPASTE:" + m("x" * (TERM_COLS - 1))
        log = wait_marker(expect_paste, timeout=long_timeout)
        time.sleep(0.3); log = serial_text()  # let CLIPTRUNC, written right after, land too
        if expect_paste not in log:
            fails.append(f"Terminal Ctrl+V: expected a clean {TERM_COLS - 1}-byte truncated paste, marker not found")
        elif "CLIPPASTE:" + m("x" * TERM_COLS) in log:
            fails.append("Terminal Ctrl+V: pasted text was NOT truncated at TERM_COLS-1 -- overflow risk")
        elif "CLIPTRUNC" not in log.split(expect_paste, 1)[1][:40]:
            fails.append("Terminal Ctrl+V: truncation marker (CLIPTRUNC) missing right after the paste")
        else:
            print(f"Terminal : pasted a {len(long_text)}-byte clipboard, truncated cleanly to {TERM_COLS - 1} bytes (serial-verified)")
        close_via_x()

    cmd({"execute": "quit"})
except Exception as e:
    fails.append(f"exception: {e}")
finally:
    try: q.wait(timeout=5)
    except Exception: q.kill()

if fails:
    print("FAIL:")
    for msg in fails: print("  - " + msg)
    print(f"artifacts: {ARTIFACTS}")
    sys.exit(1)
print("PASS: clipboard copy/cut/paste round-trips real text within Notes, across Notes -> Terminal, and truncates a too-long paste cleanly at Terminal's own input limit")
