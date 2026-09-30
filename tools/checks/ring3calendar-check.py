#!/usr/bin/env python3
"""Calendar runs as a real ring-3 process, the sixteenth app out of the kernel
(roadmap 2.0, 1.9.11).

Boots headless with `open=cale`, which launches Calendar from the dock path
the moment the desktop is up, and with the clock pinned to 2026-08-15 (a
Saturday). Calendar is user/calendar.c, a flat binary loaded off the VFS by
exec_user and run at CPL 3 through the table-driven launcher
(kernel/ring3app.c, RING3_APPS). Today comes to it through SYS_TIME, so the
epoch-to-date and Zeller weekday math now run in ring 3, and EVENTS.TXT goes
through the ordinary file calls: no new syscall. The check then:

  1. asserts, off the serial log, that the program opened a window of the
     dock viewport's size (804x345), computed today as 2026-08-15 with
     weekday 6 (Saturday) from SYS_TIME, found no file and loaded 0 events;
  2. reads the framebuffer: the month grid's weekday rule spans the grid,
     today's accent disc sits in the third row's Saturday cell, and that
     cell also carries the selection tint;
  3. presses Enter, types "dentist" through the real editor (the
     "calendarprompt" marker must grow once per keystroke), presses Enter
     and waits for "calendar: saved 1" and "calendar: event 2026-08-15":
     the save hit EVENTS.TXT, and the event dot is drawn under today;
  4. switches to Week view (key 2) and finds the event's accent bar in
     Saturday's column, then back to Month (key 3);
  5. closes on Esc and asserts a clean exit 0 and a window released;
  6. reopens Calendar through the Apps folder (digit 3, grid index 2):
     "calendar: loaded 1" proves a fresh process read the event back off
     the file; Esc closes it and the folder;
  7. opens Samantha, asks "what is on my calendar today" and, with a fake
     /api/pick server on the host naming calendar_today (the same fixture
     tools/checks/chattools-check.py uses), waits for
     "chattool=calendar_today:dentist": the kernel tool read the file the
     ring-3 program wrote;
  8. asserts the desktop is back (dock drawn, Mail opens from the dock).

Every wait has a deadline. Discriminating: on a kernel without the port
(1.9.9) there is no `open=cale` flag and no CALENDAR.BIN, so step 1 fails
at once; with the save skipped in set_event, step 6's "loaded 1" and step
7's "calendar_today:dentist" both fail.

Usage: tools/checks/ring3calendar-check.py   (from the repo root, after make kernel.elf)
"""
import http.server, json, os, socket, subprocess, sys, threading, time
from PIL import Image
from freeport import free_port

LOG = "/tmp/jt-ring3calendar-serial.log"
DUMP = "/tmp/jt-ring3calendar.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = free_port()
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
CLOSE_X, CLOSE_Y = 94, 56
CLOSE_RED = (0xFF, 0x5F, 0x57)
VIEW_X, VIEW_Y = 78, 72   # gui_launch_from_dock: viewport at (x+8, y+32) for x=70, y=40
PARK = (480, 200)
T = -32                   # user/calendar.c draws with calendar.h's dock-window offsets
DOCK_CHAT = 7
ACCENT, SEL, RULE = (0xA0, 0x55, 0x3F), (0xED, 0xE6, 0xDC), (0xDD, 0xD9, 0xD3)
WHITE = (0xFF, 0xFF, 0xFF)
# August 2026 in an 804x345 viewport: 72px cells, grid x0 = 150, rows from
# T+150 with 36px cells (six rows). The 1st is a Saturday (column 6), so
# the 15th is row 2, column 6: centre (618, T+150+2*36+18).
CELL_W, X0, Y0, CELL_H = 72, 150, T + 150, 36
TODAY_CX, TODAY_CY = X0 + 6 * CELL_W + CELL_W // 2, Y0 + 2 * CELL_H + CELL_H // 2

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass


class Pick(http.server.BaseHTTPRequestHandler):
    """The picker Samantha asks which local tool fits a message. Names
    calendar_today for anything mentioning the calendar, null otherwise."""
    def log_message(self, *a): pass
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0")); body = self.rfile.read(n)
        if self.path == "/api/pick":
            try: q = json.loads(body.decode("utf-8")).get("q", "")
            except Exception: q = ""
            rep = json.dumps({"tool": "calendar_today", "arg": ""} if "calendar" in q.lower() else {"tool": None, "arg": ""}).encode()
        else:
            rep = json.dumps({"model": "samantha", "message": {"role": "assistant", "content": "ok"}, "done": True}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(rep)))
        self.end_headers()
        self.wfile.write(rep)


srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Pick)
srv.daemon_threads = True
threading.Thread(target=srv.serve_forever, daemon=True).start()
llm_port = srv.server_address[1]

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf",
                      "-append", f"open=cale llmhost=10.0.2.2 llmport={llm_port}",
                      "-display", "none", "-vga", "std",
                      "-rtc", "base=2026-08-15T12:00:00",
                      "-net", "nic,model=rtl8139", "-net", "user",
                      "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = []
def serial():
    try: return open(LOG, errors="replace").read()
    except OSError: return ""
def wait_serial(needle, secs, count=1):
    for _ in range(int(secs * 10)):
        if serial().count(needle) >= count: return True
        time.sleep(0.1)
    return False
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
    def keys(*qcodes):
        r = cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k} for k in qcodes]}})
        if "error" in r: raise SystemExit(f"FAIL: QMP rejected send-key {qcodes}: {r['error']}")
        time.sleep(0.35)  # faster than this drops scancodes on a loaded host
    def typed(word):
        for ch in word: keys({" ": "spc", ".": "dot"}.get(ch, ch))
    def move(x, y):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
    def click():
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
        time.sleep(0.1)
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
    def frame():
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
    def pixel(x, y, img=None):
        return (img or frame()).getpixel((x * SCALE + 1, y * SCALE + 1))
    def vpx(x, y, img=None): return pixel(VIEW_X + x, VIEW_Y + y, img)
    def near(p, c, tol=12): return max(abs(p[i] - c[i]) for i in range(3)) <= tol
    def count_color(img, x0, y0, x1, y1, c, tol=12):
        return sum(1 for y in range(y0, y1) for x in range(x0, x1) if near(vpx(x, y, img), c, tol))

    # 1. the program is up, got today from SYS_TIME and found no file
    if not wait_serial("autoopen=calendar", 40):
        fails.append("the open=cale boot flag was not recognised (ring3app.c)")
    if not wait_serial("ring3app: launching CALENDAR.BIN at ring 3", 40):
        fails.append("Calendar was never launched as a ring-3 program (open=cale flag or ring3app.c broken)")
    if not wait_serial("syscall: window opened for ring-3 task", 10):
        fails.append("SYS_WINDOW_OPEN never succeeded from ring 3")
    if not wait_serial("calendar: ring-3 window 804x345", 10):
        fails.append("the program did not report the app viewport's size (expected 804x345) through write()")
    if not wait_serial("calendar: today 2026-08-15 dow 6", 10):
        fails.append('expected "calendar: today 2026-08-15 dow 6" from SYS_TIME with the RTC pinned to that Saturday: '
                     + repr([l for l in serial().splitlines() if l.startswith("calendar: today")]))
    if not wait_serial("calendar: loaded 0", 10):
        fails.append('expected "calendar: loaded 0" on a disk with no EVENTS.TXT')
    time.sleep(0.6)

    # 2. the month grid is on screen
    img = frame()
    rule = count_color(img, 0, T + 142, 804, T + 143, RULE, 4)
    print(f"month grid: weekday rule {rule} px wide (want the 504px grid)")
    if rule < 300: fails.append(f"the month grid's weekday rule is missing or short ({rule} px)")
    disc = vpx(TODAY_CX, TODAY_CY, img)
    tint = vpx(X0 + 6 * CELL_W + 4, Y0 + 2 * CELL_H + 4, img)
    print(f"today's cell: disc centre {disc}, selection tint at the corner {tint}")
    if not near(disc, ACCENT): fails.append(f"today's accent disc is not in the 15th's cell (got {disc})")
    if not near(tint, SEL): fails.append(f"today's cell is not the selected day (corner {tint}, want {SEL})")

    # 3. add an event through the real editor
    keys("ret")
    if not wait_serial("calendarprompt\n", 5):
        fails.append("pressing Enter did not open the event editor (no calendarprompt marker)")
    p0 = serial().count("calendarprompt\n")
    typed("dentist")
    if not wait_serial("calendarprompt\n", 5, p0 + 7):
        fails.append("typing seven letters did not redraw the editor once per keystroke")
    keys("ret")
    if not wait_serial("calendar: saved 1", 5):
        fails.append('the save did not log "calendar: saved 1" (EVENTS.TXT not written)')
    if not wait_serial("calendar: event 2026-08-15", 5):
        fails.append('the save did not log "calendar: event 2026-08-15"')
    time.sleep(0.5)
    dot = vpx(TODAY_CX, TODAY_CY + 14)
    print(f"event dot under today: {dot} (white on the accent disc)")
    if not near(dot, WHITE): fails.append(f"no event dot drawn under today after the save (got {dot})")

    # 4. week view shows the event's bar in Saturday's column; back to month
    keys("2")
    if not wait_serial("calendar: view 1", 5):
        fails.append('key 2 did not switch to Week view ("calendar: view 1")')
    time.sleep(0.5)
    col_w = (804 - 40) // 7; wx0 = (804 - col_w * 7) // 2
    img = frame()
    bar = count_color(img, wx0 + 6 * col_w + 6, T + 190, wx0 + 6 * col_w + 9, T + 208, ACCENT)
    print(f"week view: event bar accent px in Saturday's column {bar}")
    if bar < 30: fails.append(f"week view did not draw the saved event's bar in the selected column ({bar} px)")
    keys("3")
    if not wait_serial("calendar: view 2", 5):
        fails.append('key 3 did not switch back to Month view ("calendar: view 2")')

    # 5. Esc closes it cleanly
    exits = serial().count("CALENDAR.BIN exited 0")
    keys("esc")
    if not wait_serial("calendar: closed", 5):
        fails.append("Esc did not reach the program (no closed line)")
    if not wait_serial("CALENDAR.BIN exited 0", 5, exits + 1):
        fails.append("Calendar did not exit 0 on Esc")
    if not wait_serial("syscall: window released, task gone", 5):
        fails.append("Calendar did not release its window on Esc")
    move(*PARK); time.sleep(0.5)
    for _ in range(100):
        if not near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED): break
        time.sleep(0.1)
    if near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED):
        fails.append("an app window is still open after Esc")

    # 6. reopen through the Apps folder: a fresh process must read EVENTS.TXT back
    launches = serial().count("ring3app: launching CALENDAR.BIN")
    n = serial().count("appsfullrepaint")
    move(SLOT0_X + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.3); click()
    if not wait_serial("appsfullrepaint", 10, n + 1):
        fails.append("the Apps folder did not open from the dock")
    else:
        time.sleep(0.6)
        keys("3")  # grid index 2 is Calendar: digits 1-9 launch the first nine tiles
        if not wait_serial("ring3app: launching CALENDAR.BIN", 10, launches + 1):
            fails.append("Calendar did not launch from the Apps folder grid (digit 3)")
        if not wait_serial("calendar: loaded 1", 10):
            fails.append('the reopened app did not report "calendar: loaded 1": the event did not persist in EVENTS.TXT')
        time.sleep(0.5)
        n = serial().count("appsfullrepaint")
        keys("esc")
        if not wait_serial("appsfullrepaint", 10, n + 1):
            fails.append("the Apps folder did not come back after Calendar closed")
        time.sleep(0.5)
        keys("esc"); time.sleep(0.8)  # close the folder

    # 7. Samantha's calendar_today tool reads the same file
    move(SLOT0_X + DOCK_CHAT * PITCH + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.3); click()
    opened = False
    for _ in range(100):
        time.sleep(0.1)
        if near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED): opened = True; break
    if not opened:
        fails.append("Samantha did not open from the dock")
    else:
        time.sleep(0.5)
        keys("n"); time.sleep(0.4)
        typed("what is on my calendar today")
        keys("ret")
        if not wait_serial("chattool=calendar_today:dentist", 30):
            fails.append("Samantha's calendar_today tool did not see the event the ring-3 program saved: "
                         + repr([l for l in serial().splitlines() if l.startswith("chattool=") or l.startswith("chatpick")]))
        time.sleep(0.5)
        move(CLOSE_X, CLOSE_Y); time.sleep(0.3); click(); time.sleep(0.8)

    # 8. the desktop must answer
    if "exception: ring-0" in serial() or "panic in" in serial() or "ring3app: BUG" in serial():
        fails.append("the kernel faulted or ring3app logged a BUG line")
    move(*PARK); time.sleep(0.5)
    dock = pixel(480, 511)
    if dock != (0xEF, 0xEB, 0xE4):
        fails.append(f"desktop dock not on screen after the close (got {dock})")
    move(SLOT0_X + 2 * PITCH + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.3); click()
    opened = False
    for _ in range(40):
        time.sleep(0.1)
        if near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED): opened = True; break
    print(f"Mail opens from the dock after the close: {'yes' if opened else 'NO'}")
    if not opened: fails.append("Mail did not open from a dock click after Calendar closed: desktop not responsive")
    keys("esc"); time.sleep(0.5)
finally:
    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError, ValueError, NameError): pass
    q.terminate()
    try: q.wait(5)
    except subprocess.TimeoutExpired: q.kill()
    srv.shutdown()

if fails:
    print("FAIL:")
    for x in fails: print("  - " + x)
    print("--- serial tail ---")
    print(serial()[-1500:])
    sys.exit(1)
print("PASS: Calendar ran at ring 3 with its own window, got today from SYS_TIME, drew the month grid, saved an event through the real editor, kept EVENTS.TXT across a fresh run, fed Samantha's calendar_today, closed on Esc, and the desktop stayed alive")
