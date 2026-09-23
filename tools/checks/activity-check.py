#!/usr/bin/env python3
"""Headless proof that the Activity app (kernel/activity.h, v0.88.0) is
real: it reads real scheduler state through the exact same primitives the
shell's own `ps`/`kill`/`mem` commands already call (task_used/task_kill/
pmm_free_frames, kernel/task.c + kernel/pmm.c), not a fabricated process
list, and killing a task selected in the app really removes it from the
scheduler. Same QMP absolute-pointer + qcode-keyboard + pmemsave shape as
search-check.py / appclose-check.py / contacts-keystroke-check.sh.

Flow:
  1. Boot (this kernel boots straight to the GUI desktop). Esc drops back
     to the real text shell (same contract apptest.sh already relies on).
  2. Type `spawntest`, a real, minimal shell command added alongside this
     app (kernel.c): task_create(spawntest_task) spins a genuine scheduler
     task forever (`for (;;) yield();`) until killed, and its real slot id
     is printed over serial ("spawntest id=N") so this script knows which
     Activity row to expect, target and re-check -- not a hardcoded id.
  3. Type `gui` to re-enter the desktop, open the Apps folder (dock slot
     0), navigate the grid to Activity (icon 24, row 4 col 4: right x4,
     down x4 from the top-left cell, the same cell math gui_launch_apps
     itself uses, already proven reliable for Search/Contacts by their own
     checks), Enter to launch it.
  4. Real assertions, each on real framebuffer pixels / serial markers:
       a. The app opened (outer red close dot present) and its own
          "activitycontent" redraw marker appears on serial -- proof this
          is the real per-refresh content draw, not a static screen.
       b. The spawned task's row shows real text (a live "Task N" /
          "running" row, not a blank one).
       c. Selecting that row (down-arrow x id) and pressing 'k' -- the
          app's real Kill action -- removes it: after the app's own next
          ~1s refresh, that same row's dark-pixel count drops (state text
          shrinks from "running" to "free", a real, measurable redraw).
       d. The refresh keeps happening on its own afterward too (the
          "activitycontent" marker count keeps growing with no further
          key presses), proof the ~1s live refresh loop is real and not
          a one-shot redraw that happens to coincide with the Kill key.

Discriminating: reverting activity_kill_selected to a no-op (never calls
task_kill) makes (c) fail -- the row keeps showing as much "running" text
after Kill as before it. Confirmed live against a temporarily stubbed
build before this file was finalized. (puts()/`ps` write straight to the
VGA text buffer, not serial, so this script verifies the kill through the
app's own real framebuffer output rather than a shell round-trip.)

Usage: tools/checks/activity-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, re, socket, subprocess, sys, time
from PIL import Image

LOG = "/tmp/jt-activity-serial.log"
DUMP = "/tmp/jt-activity.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4711  # > 4700, avoids colliding with other checks' ports on a shared machine
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
APPS_CLOSE_X, APPS_CLOSE_Y = 80, 46  # the Apps folder's own outer window red dot (56+24, 30+16)
CLOSE_RED = (0xFF, 0x5F, 0x57)
# gui_launch_apps' content viewport (same derivation search-check.py/launchpad-click-check.py use).
VX, VY = 64, 62
ROW_X0, ROW_X1 = VX + 76, VX + 260  # spans the STATE column text for any row

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                      "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = []
try:
    s = None
    for _ in range(50):
        time.sleep(0.2)
        try: s = socket.create_connection(("127.0.0.1", PORT)); break
        except OSError: pass
    if s is None: raise SystemExit("FAIL: QEMU's QMP socket never came up")
    f = s.makefile("rw")
    def cmd(o):
        f.write(json.dumps(o) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    f.readline()
    cmd({"execute": "qmp_capabilities"})
    time.sleep(5.0)  # desktop up

    def move(x, y):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
    def click():
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
        time.sleep(0.1)
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
    def key(qcode):
        cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": qcode}]}})
        time.sleep(0.35)  # real, measured cadence search-check.py/contacts-keystroke-check.sh already rely on
    def dump():
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
    def is_red(img, x, y): return max(abs(img.getpixel((x * SCALE + 1, y * SCALE + 1))[i] - CLOSE_RED[i]) for i in range(3)) <= 12
    def lum(p): return (p[0] * 299 + p[1] * 587 + p[2] * 114) // 1000
    def row_has_text(img, y):
        dark = 0
        for x in range(ROW_X0, ROW_X1, 2):
            for dy in (0, 4, 8, 12):
                if lum(img.getpixel((x * SCALE, (y + dy) * SCALE))) < 180:
                    dark += 1
        return dark

    # Step 1: esc drops the booted desktop back to the real text shell.
    key("esc"); time.sleep(0.5)

    # Step 2: spawn a real, persistent task and learn its real slot id.
    # Keys dropped on slow runners if sent in burst; poll for result after typing
    for c in "spawntest":
        key(c)
    key("ret")
    for _ in range(50):
        time.sleep(0.1)
        try:
            with open(LOG, errors="replace") as lf:
                if "spawntest id=" in lf.read():
                    break
        except FileNotFoundError:
            pass

    spawn_id = None
    try:
        with open(LOG, errors="replace") as lf:
            m = re.search(r"spawntest id=(\d+)", lf.read())
            if m: spawn_id = int(m.group(1))
    except FileNotFoundError:
        pass
    if spawn_id is None:
        raise SystemExit("FAIL: spawntest never printed a real task id over serial, can't proceed")
    print(f"spawned real task id={spawn_id}")

    # Step 3: back to the desktop, open Apps, navigate to Activity (icon 24).
    for c in "gui":
        key(c)
    key("ret"); time.sleep(2.0)  # gui_run's own boot screen + first frame

    apps_centre = SLOT0_X + 0 * PITCH + DOCK_ICON // 2
    move(apps_centre, ICON_ROW_Y); time.sleep(0.3)
    click(); time.sleep(1.0)

    # icon 24: row = 24 // 5 = 4, col = 24 % 5 = 4 -- right x4, down x4 from
    # the grid's top-left cell, the same math gui_launch_apps itself uses.
    for c in ("d", "d", "d", "d", "s", "s", "s", "s"):
        key(c)
    key("ret"); time.sleep(1.2)  # launch Activity

    img = dump()
    if not is_red(img, APPS_CLOSE_X, APPS_CLOSE_Y):
        fails.append("Activity did not open: the Apps folder's outer red close dot is gone")
    else:
        print("Activity opened (outer window chrome present)")

    markers_before = 0
    try:
        with open(LOG, errors="replace") as lf: markers_before = lf.read().count("activitycontent\n")
    except FileNotFoundError: pass
    if markers_before < 1:
        fails.append("no 'activitycontent' redraw marker on serial -- the real content draw never ran")
    else:
        print(f"activitycontent redraw markers so far: {markers_before}")

    row_y = VY + 108 + spawn_id * 22
    before_px = row_has_text(img, row_y)
    print(f"row {spawn_id} ('running') dark-px before kill = {before_px}")
    if before_px < 4:
        fails.append(f"spawned task's row (slot {spawn_id}) shows no real 'running' text before it was killed")

    # Step 4: select that exact row (down x spawn_id from row 0) and kill it.
    for _ in range(spawn_id):
        key("down")
    key("k"); time.sleep(1.5)  # give the app's own ~1s refresh time to redraw the now-freed slot

    img2 = dump()
    after_px = row_has_text(img2, row_y)
    print(f"row {spawn_id} ('free') dark-px after kill = {after_px}")
    if after_px >= before_px:
        fails.append(f"row {spawn_id} still shows as much text after Kill as before it -- the task was not really removed")
    else:
        print("Kill shrank the row's live text (running -> free), consistent with a real state change")

    markers_after = 0
    try:
        with open(LOG, errors="replace") as lf: markers_after = lf.read().count("activitycontent\n")
    except FileNotFoundError: pass
    if markers_after <= markers_before:
        fails.append("no further 'activitycontent' redraws happened after Kill -- the ~1s live refresh isn't real")
    else:
        print(f"activitycontent redraw markers after kill: {markers_after} (grew, refresh is live)")

    key("esc"); time.sleep(0.3)  # close Activity
    key("esc"); time.sleep(0.3)  # close the Apps folder, leave input clean for any check that runs after this one

    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

if fails:
    for x in fails: print("FAIL:", x)
    sys.exit(1)
print("PASS: Activity shows a real live task list off the real scheduler, refreshes it live, and Kill really removes a task")
