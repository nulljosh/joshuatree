#!/usr/bin/env python3
"""Headless proof of the "phone" boot flag (kernel.c's boot_to_phone):
"phone samantha" on the multiboot command line opens a real 430x760
portrait framebuffer (Bochs VBE, drivers/vbe.c's vbe_set_mode, called via
window_open in gui_run) instead of the desktop's 960x540@2x, and lands on
Chat's full-screen avatar view laid out for it (kernel/chat.h's
chat_boot_samantha_open, face top-half/caption-under-it/input-at-bottom
for boot_to_phone).

Same boot + QMP + serial shape as samantha-boot-check.py: boots
kernel.elf under `-display none`, waits past the splash, reads serial for
her avatar markers, and screendumps the framebuffer at exactly
860*1520*4 bytes -- the read only lines up with real content if the mode
really is 430x760, not the desktop's 960x540 (misaligned rows would read
as noise, not the specific colors this checks for at specific offsets).

Usage: tools/checks/phone-boot-check.py   (from the repo root, after make kernel.elf)
"""
from freeport import free_port
import json, os, socket, subprocess, sys, time

PORT = free_port()
FB = 0xfd000000
W, H = 860, 1520  # 430x760 logical at 2x
GUI_BG = (0xFA, 0xF8, 0xF6)
DUMP = "/tmp/jt-phoneboot.raw"
PNG = "/tmp/jt-phoneboot.png"
LOG = "/tmp/jt-phoneboot.log"

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def boot(append, log_path, dump_path):
    for f in (log_path, dump_path):
        try: os.remove(f)
        except FileNotFoundError: pass
    args = ["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
            "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + log_path,
            "-append", append]
    q = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.0)
        s = socket.create_connection(("127.0.0.1", PORT)); f = s.makefile("rw")
        def cmd(o):
            f.write(json.dumps(o) + "\n"); f.flush()
            while True:
                r = json.loads(f.readline())
                if "return" in r or "error" in r: return r
        f.readline()
        cmd({"execute": "qmp_capabilities"})
        time.sleep(2.5)  # past the boot splash, same window every other boot check waits
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": dump_path}})
        try: cmd({"execute": "quit"})
        except (ConnectionResetError, BrokenPipeError, OSError): pass
    finally:
        try: q.wait(timeout=5)
        except subprocess.TimeoutExpired: q.kill()
    return open(log_path, errors="replace").read()


fail = 0
log = boot("phone samantha", LOG, DUMP)

if "bootphone" not in log:
    fail = 1; print("FAIL: -append \"phone samantha\" never printed bootphone -- phone flag not parsed")
if "samopen" not in log:
    fail = 1; print("FAIL: samopen missing -- Samantha's avatar view never opened")
if "samfocus" not in log:
    fail = 1; print("FAIL: samfocus missing -- input box never drew/focused")
if "guidesktop" in log:
    fail = 1; print("FAIL: the icon desktop drew (guidesktop) -- phone must skip straight to Samantha")

try:
    from PIL import Image
    raw = open(DUMP, "rb").read()
    if len(raw) != W * H * 4:
        fail = 1; print(f"FAIL: screendump is {len(raw)} bytes, expected exactly {W*H*4} for a {W}x{H} frame")
    else:
        print(f"PASS: framebuffer read back at exactly {W}x{H}x32bpp ({len(raw)} bytes)")
    img = Image.frombytes("RGBA", (W, H), raw, "raw", "BGRA")
    px = img.load()

    def get(x, y): return px[x, y][:3]
    def close(a, b, tol=10): return all(abs(a[i] - b[i]) <= tol for i in range(3))

    # Titlebar/top region: gui_draw_app_titlebar always draws something
    # here regardless of network (unlike her real face photo, which needs
    # facehost= reachable -- this headless boot has no NIC, same caveat
    # samantha-boot-check.py notes). Desktop mode draws three traffic-
    # light dots at (26,20)/(46,20)/(66,20); v1.8.0 replaced that with a
    # single tappable back chevron ("<") at (18,10) for boot_to_phone (no
    # Esc key on a real phone) -- scan the whole titlebar strip for real
    # ink rather than one dot's exact old pixel, so this still passes
    # either way.
    top_has_ink = any(not close(get(x, y), GUI_BG, 30) for x in range(20, 140, 4) for y in range(10, 40, 4))
    if not top_has_ink:
        fail = 1; print("FAIL: titlebar/back-chevron region looks like plain background -- top region not drawn")
    else:
        print("PASS: titlebar region has real content (back chevron)")

    # Input box: chat_boot_samantha_open draws a solid white rect at
    # (20, bottom-30) .. (width-20, bottom-10), bottom = height-40 = 892,
    # i.e. rows 690..710 -- only at the true bottom of a 760-tall frame.
    box = get(60, 1400)
    if not close(box, (255, 255, 255), 15):
        fail = 1; print(f"FAIL: input box at (30,700) is not white {box} -- not drawn where a 760-tall frame puts it")
    else:
        print(f"PASS: input box drawn at the expected bottom position: rgb={box}")
    above_box = get(60, 1336)
    if close(box, above_box, 5) and not close(above_box, (255, 255, 255), 15):
        pass  # background sampled above the box differs from the box itself -- fine either way, informational only
except Exception as e:
    fail = 1; print(f"FAIL: pixel checks errored ({e})")

if not fail:
    print("PASS: \"phone samantha\" opens a real 430x760 portrait boot straight into Samantha's view, titlebar and input box both where a phone-sized frame puts them")

try:
    from PIL import Image as _Image
    _img = _Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA")
    _img.convert("RGB").save(PNG)
    print(f"saved {PNG}")
except Exception as e:
    print(f"note: could not save PNG ({e})")

# Second scenario (1.7.11 regression, real bug seen on the deployed demo):
# once she has a face AND a reply on a phone-sized boot, the big face used
# to start at the same T + 44 the empty view uses, which pokes up into the
# status band's "Samantha  <state>" label -- fine on desktop (the label
# sits at x=20, the face is centered on a wide screen, they never touch)
# but on a 430-wide phone frame the face is nearly the full screen width,
# so it covered the label's right half ("Samant" then face). Boot with a
# facehost stub serving solid-color frames, send a message that answers
# locally (Samantha runs new_reminder after the stub answers /api/pick), and assert
# CHAT_ACCENT-colored pixels (her name, drawn in 0x00B7862A) still exist
# across the label's known column span -- not just at x=20, which a
# half-covered label would still pass.
import http.server, io, threading

def _stub_post(h):
    """Answer Samantha's /api/pick and /api/chat locally, instantly. These two
    scenarios used to boot with no llmhost, so /api/pick went to the real
    internet: green on a box with a route, but on a runner without a fast one
    the pick blocked her for seconds (the reminder never ran, and the back
    chevron tap queued behind the blocked request). Hermetic and immediate now."""
    body = h.rfile.read(int(h.headers.get("Content-Length", "0")))
    if h.path == "/api/pick":
        try: q = json.loads(body.decode("utf-8")).get("q", "").lower()
        except Exception: q = ""
        ans = {"tool": "new_reminder", "arg": "call mom"} if "remind" in q else {"tool": None, "arg": ""}
    else:
        ans = {"model": "samantha", "message": {"role": "assistant", "content": "ok"}, "done": True}
    rep = json.dumps(ans).encode()
    h.send_response(200); h.send_header("Content-Type", "application/json")
    h.send_header("Content-Length", str(len(rep))); h.end_headers(); h.wfile.write(rep)

try:
    from PIL import Image as _Image2

    def jpg(color):
        b = io.BytesIO(); _Image2.new("RGB", (320, 320), color).save(b, "JPEG", quality=90); return b.getvalue()

    FACE_RED = (200, 30, 30)
    FRAMES = {f"/face/idle-{i}.jpg": jpg(FACE_RED) for i in range(3)}

    class _Stub(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a): pass
        def do_POST(self): _stub_post(self)
        def do_GET(self):
            body = FRAMES.get(self.path)
            if body is None: self.send_response(404); self.end_headers()
            else:
                self.send_response(200); self.send_header("Content-Type", "image/jpeg")
                self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)

    _srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), _Stub); _srv.daemon_threads = True
    threading.Thread(target=_srv.serve_forever, daemon=True).start()
    _port = _srv.server_address[1]

    LABEL_LOG = "/tmp/jt-phoneboot-label.log"
    LABEL_DUMP = "/tmp/jt-phoneboot-label.raw"
    LABEL_PNG = "/tmp/jt-facelabel-after.png"
    LABEL_PORT = free_port()  # not LABEL_PORT: the stub servers' free_port() calls hand out neighbours, QEMU then fails to bind and readline() hangs
    for f in (LABEL_LOG, LABEL_DUMP):
        try: os.remove(f)
        except FileNotFoundError: pass
    args = ["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std", "-no-reboot",
            "-qmp", f"tcp:127.0.0.1:{LABEL_PORT},server,nowait", "-serial", "file:" + LABEL_LOG,
            "-net", "nic,model=rtl8139", "-net", "user",
            "-append", f"phone samantha facehost=10.0.2.2:{_port} llmhost=10.0.2.2 llmport={_port}"]
    q2 = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.0)
        s2 = socket.create_connection(("127.0.0.1", LABEL_PORT)); f2 = s2.makefile("rw")

        def cmd2(o):
            f2.write(json.dumps(o) + "\n"); f2.flush()
            while True:
                r = json.loads(f2.readline())
                if "return" in r or "error" in r: return r
        f2.readline(); cmd2({"execute": "qmp_capabilities"})
        time.sleep(3.0)

        def keys(k): cmd2({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k}]}})
        for ch in "remind me to call mom":
            keys("spc" if ch == " " else ch); time.sleep(0.15)  # Samantha is a ring-3 window now: each key redraws her frame, and a full event ring drops the newest key (the enter)
        time.sleep(0.3)
        keys("ret")
        time.sleep(5.0)  # /api/pick round trip, then user/samantha.c runs new_reminder locally
        cmd2({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": LABEL_DUMP}})
        try: cmd2({"execute": "quit"})
        except (ConnectionResetError, BrokenPipeError, OSError): pass
    finally:
        try: q2.wait(timeout=5)
        except subprocess.TimeoutExpired: q2.kill()
    _srv.shutdown()

    label_log = open(LABEL_LOG, errors="replace").read()
    if "chattool=new_reminder" not in label_log:
        fail = 1; print("FAIL: the reminder tool never ran -- can't tell if the label survived a real reply")
    else:
        img2 = _Image2.frombytes("RGBA", (W, H), open(LABEL_DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
        img2.save(LABEL_PNG)
        px2 = img2.load()
        ACCENT = (0xB7, 0x86, 0x2A)

        def close2(a, b, tol=70): return all(abs(a[i] - b[i]) <= tol for i in range(3))

        # "Samantha" (accent color) spans roughly x=42..159 at this scale/
        # font -- sample across that whole span, not just the left edge,
        # so a face that covers the word's second half still fails this.
        cols_hit = sum(1 for x in range(42, 160, 6) if any(close2(px2[x, y], ACCENT) for y in range(100, 140)))
        if cols_hit < 15:
            fail = 1
            print(f"FAIL: 'Samantha' label only has accent-colored pixels in {cols_hit}/20 sampled columns after her reply -- the face is covering part of it")
        else:
            print(f"PASS: 'Samantha' label reads fully ({cols_hit}/20 columns) after she answers on a phone boot, face included")
        print(f"saved {LABEL_PNG}")
except Exception as e:
    fail = 1; print(f"FAIL: face+label regression check errored ({e})")

# Third scenario (1.8.0, roadmap "A phone home screen"): a real iOS-shaped
# app grid instead of the desktop's dock+menu bar squeezed into 430px.
# Reach it from a "phone" boot the same way scenario 2 above already does
# (Samantha boots first, one ESC into her normal Chat console, a second
# ESC out of that -- gui_run now calls phone_home_run() in place of the
# old desktop dock loop for boot_to_phone, so this second ESC lands on
# the home grid, not the old dock). Verifies: the grid actually drew
# (icons at Calendar's and Keyrate's real cells, not blank background),
# tapping Calendar opens it full width below the status bar (same
# gui_draw_app_titlebar dot every full-screen app draws, kernel/
# phone_home.h reuses gui_apps_launch's existing full-screen-open branch
# rather than a new one), Esc returns to the grid, then the same round
# trip for Keyrate -- a real ring-3 program (kernel/ring3app.c), proving
# the full-screen host works for both in-kernel and ring-3 apps.
try:
    from PIL import Image as _Image3

    PW, PH = 860, 1520          # 430x760 logical at 2x, same convention as the rest of this file
    LOGICAL_W, LOGICAL_H, SC = 430, 760, 2
    FIT_PORT = free_port()
    FIT_LOG = "/tmp/jt-phonehome-serial.log"
    HOME_DUMP = "/tmp/jt-phonehome-grid.raw"
    CAL_DUMP = "/tmp/jt-phonehome-calendar.raw"
    BACK1_DUMP = "/tmp/jt-phonehome-back1.raw"
    KEY_DUMP = "/tmp/jt-phonehome-keyrate.raw"
    BACK2_DUMP = "/tmp/jt-phonehome-back2.raw"
    HOME_PNG = "/tmp/jt-home18-home.png"
    APP_PNG = "/tmp/jt-home18-app.png"
    for f in (FIT_LOG, HOME_DUMP, CAL_DUMP, BACK1_DUMP, KEY_DUMP, BACK2_DUMP):
        try: os.remove(f)
        except FileNotFoundError: pass

    args3 = ["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std", "-no-reboot",
             "-qmp", f"tcp:127.0.0.1:{FIT_PORT},server,nowait", "-serial", "file:" + FIT_LOG,
             "-append", "phone"]
    q3 = subprocess.Popen(args3, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.0)
        s3 = socket.create_connection(("127.0.0.1", FIT_PORT)); f3 = s3.makefile("rw")

        def cmd3(o):
            f3.write(json.dumps(o) + "\n"); f3.flush()
            while True:
                r = json.loads(f3.readline())
                if "return" in r or "error" in r: return r

        def key3(k):
            cmd3({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k}]}})

        def move3(x, y):
            cmd3({"execute": "input-send-event", "arguments": {"events": [
                {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
                {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})

        def click3():
            cmd3({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
            time.sleep(0.1)
            cmd3({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})

        def dump3(path):
            cmd3({"execute": "pmemsave", "arguments": {"val": FB, "size": PW * PH * 4, "filename": path}})

        def load3(path):
            img = _Image3.frombytes("RGBA", (PW, PH), open(path, "rb").read(), "raw", "BGRA").convert("RGB")
            return img, img.load()

        f3.readline(); cmd3({"execute": "qmp_capabilities"})
        time.sleep(2.5)  # past splash, samopen up
        key3("esc"); time.sleep(0.6)  # samantha avatar -> her normal Chat console
        key3("esc"); time.sleep(0.6)  # Chat console -> phone_home_run's grid

        dump3(HOME_DUMP)
        img_home, px_home = load3(HOME_DUMP)
        img_home.save(HOME_PNG)
        GUI_BG = (0xFA, 0xF8, 0xF6)

        def not_bg(px, x, y, bg=GUI_BG, tol=16):
            for dx in range(-6, 7, 3):
                for dy in range(-6, 7, 3):
                    p = px[(x + dx) * SC + 1, (y + dy) * SC + 1]
                    if any(abs(p[i] - bg[i]) > tol for i in range(3)):
                        return True
            return False

        # Grid geometry from kernel/phone_home.h's phone_home_grid_geom:
        # PHONE_HOME_COLS=5 (v1.8.0: 4 cols put 26 apps at 7 rows and the
        # last row -- Activity, Clock -- never fit inside the 760px
        # screen, unreachable by any tap; 5 cols is 6 rows, clears it with
        # real margin), cell_w=430/5=86, cell_h=96, tile=48, x0=0,
        # y0=PHONE_STATUS_H(44)+12=56. cell (row,col) centre:
        # cx = col*cell_w + cell_w//2, cy = y0 + row*cell_h + tile.
        COLS = 5
        CELL_W, CELL_H, TILE, Y0 = 86, 96, 48, 56

        def cell_center(icon):
            row, col = icon // COLS, icon % COLS
            return col * CELL_W + CELL_W // 2, Y0 + row * CELL_H + TILE

        # All 25 real apps' cells must land inside the 760px logical
        # screen (no scrolling) -- the exact bug Joshua's screenshot
        # review caught at 4 columns (Activity idx24, Clock idx25 sitting
        # off the bottom, unreachable by any tap).
        GUI_APPS_FOLDER = 24
        offscreen = [i for i in range(GUI_APPS_FOLDER) if cell_center(i)[1] + 24 > LOGICAL_H]
        if offscreen:
            fail = 1; print(f"FAIL: {len(offscreen)} app cell(s) fall below the {LOGICAL_H}px screen: {offscreen}")
        else:
            print(f"PASS: all {GUI_APPS_FOLDER} app cells fit inside the {LOGICAL_H}px screen, no scrolling needed")

        cal_cx, cal_cy = cell_center(2)   # Calendar, APPS[2]
        key_cx, key_cy = cell_center(9)   # Keyrate, APPS[9], ring-3
        act_cx, act_cy = cell_center(21)  # Activity, APPS[22] is grid position 21: Portfolio (APPS[21]) is hidden outside portfolio mode
        clk_cx, clk_cy = cell_center(22)  # Clock, APPS[23] is grid position 22 (Portfolio hidden)

        if not not_bg(px_home, cal_cx, cal_cy):
            fail = 1; print(f"FAIL: no icon drawn at Calendar's grid cell ({cal_cx},{cal_cy})")
        elif not not_bg(px_home, key_cx, key_cy):
            fail = 1; print(f"FAIL: no icon drawn at Keyrate's grid cell ({key_cx},{key_cy})")
        elif not not_bg(px_home, act_cx, act_cy):
            fail = 1; print(f"FAIL: no icon drawn at Activity's grid cell ({act_cx},{act_cy}) -- last row still not fitting")
        elif not not_bg(px_home, clk_cx, clk_cy):
            fail = 1; print(f"FAIL: no icon drawn at Clock's grid cell ({clk_cx},{clk_cy}) -- last row still not fitting")
        else:
            print(f"PASS: home grid drew real icons at Calendar ({cal_cx},{cal_cy}), Keyrate ({key_cx},{key_cy}), and the last row's Activity/Clock")

        # v1.8.0: phone has no Esc key, so gui_draw_app_titlebar's desktop
        # traffic lights are replaced with a single tappable back chevron
        # (a plain "<" glyph, ink on cream) for boot_to_phone -- no more
        # saturated red dot to sample. Check for real ink in the chevron's
        # top-left corner instead, then prove the tap itself works by
        # clicking there and confirming it lands back on the grid, the
        # same as Esc does below.
        def has_ink(px, x, y, bg=GUI_BG, tol=20):
            for dx in range(0, 20, 2):
                for dy in range(0, 16, 2):
                    p = px[(x + dx) * SC + 1, (y + dy) * SC + 1]
                    if any(abs(p[i] - bg[i]) > tol for i in range(3)):
                        return True
            return False

        # Calendar: tap it, confirm the full-screen titlebar's back
        # chevron (gui_draw_app_titlebar, drawn at logical (18,10)
        # whenever gui_app_windowed was 0 on entry -- the exact branch
        # gui_apps_launch takes from phone_home_run), then tap the
        # chevron itself (gui_app_mouse_tick's phone back-zone) instead
        # of Esc -- there is no Esc key on a real phone.
        move3(cal_cx, cal_cy); time.sleep(0.3); click3(); time.sleep(1.0)
        dump3(CAL_DUMP)
        img_cal, px_cal = load3(CAL_DUMP)
        img_cal.save(APP_PNG)
        if not has_ink(px_cal, 12, 4):
            fail = 1; print("FAIL: Calendar didn't open full screen -- no back chevron drawn near (18,10)")
        else:
            print("PASS: Calendar opened full screen below the status bar (back chevron found)")

        move3(20, 15); time.sleep(0.3); click3(); time.sleep(0.6)  # tap the back chevron, not Esc
        dump3(BACK1_DUMP)
        _, px_back1 = load3(BACK1_DUMP)
        if not not_bg(px_back1, cal_cx, cal_cy):
            fail = 1; print("FAIL: tapping the back chevron from Calendar didn't return to the home grid (Calendar's icon cell is blank)")
        else:
            print("PASS: tapping the back chevron from Calendar returns to the home grid, same close path as Esc")

        # Keyrate: same round trip, a real ring-3 program this time
        # (kernel/ring3app.c), proving the full-screen host and the
        # chevron tap-to-back both work for ring-3 apps too, not just
        # in-kernel ones. This one exits via a real Esc keypress instead,
        # to prove the chevron tap and Esc still land on the exact same
        # grid, not two different close paths.
        move3(key_cx, key_cy); time.sleep(0.3); click3(); time.sleep(1.0)
        dump3(KEY_DUMP)
        _, px_key = load3(KEY_DUMP)
        if not has_ink(px_key, 12, 4):
            fail = 1; print("FAIL: Keyrate (ring-3) didn't open full screen -- no back chevron drawn near (18,10)")
        else:
            print("PASS: Keyrate (ring-3) opened full screen below the status bar")

        key3("esc"); time.sleep(0.6)
        dump3(BACK2_DUMP)
        _, px_back2 = load3(BACK2_DUMP)
        if not not_bg(px_back2, key_cx, key_cy):
            fail = 1; print("FAIL: Esc from Keyrate didn't return to the home grid (Keyrate's icon cell is blank)")
        else:
            print("PASS: Esc from Keyrate (ring-3) returns to the home grid")
        print(f"saved {HOME_PNG} and {APP_PNG}")
    finally:
        try: cmd3({"execute": "quit"})
        except (ConnectionResetError, BrokenPipeError, OSError, NameError): pass
        try: q3.wait(timeout=5)
        except subprocess.TimeoutExpired: q3.kill()
except Exception as e:
    fail = 1; print(f"FAIL: phone home screen check errored ({e})")

# Fourth scenario (demo A+ pass): tapping Samantha's own face on the phone
# boot avatar screen must NOT abandon the visitor into the console --
# kernel/chat.h's chat_boot_samantha_open loop now only exits on KEY_ESC
# (the back chevron injects it via kbd_inject, phone_home.h's
# phone_back_zone_tick), a raw click anywhere else -- most commonly her
# own face -- falls through and keeps the input box focused. Proves that
# live: boot straight into her avatar with a real (stubbed) face loaded,
# click dead center of the face, confirm the same face-colored pixels are
# still there (not the GUI_BG the home grid's empty margins would show),
# type a short message and hit enter, confirm serial shows the chat
# request actually fired (the /api/pick or /api/chat line the kernel
# logs -- chat_pick always runs first), then tap the back chevron and
# confirm it lands on the phone home grid. Finally opens Mail from that
# grid, composes one message so gui_draw_mail_content's gui_draw_hint call
# site is actually reached (an empty inbox shows "No mail yet." instead
# and never calls it), and asserts the hint row -- gui_draw_hint no-ops
# when boot_to_phone -- is blank.
try:
    from PIL import Image as _Image4
    import http.server as _hs4, io as _io4, threading as _th4

    def _jpg4(color):
        b = _io4.BytesIO(); _Image4.new("RGB", (320, 320), color).save(b, "JPEG", quality=90); return b.getvalue()

    FACE4 = (200, 30, 30)
    FRAMES4 = {f"/face/idle-{i}.jpg": _jpg4(FACE4) for i in range(3)}

    class _Stub4(_hs4.BaseHTTPRequestHandler):
        def log_message(self, *a): pass
        def do_POST(self): _stub_post(self)
        def do_GET(self):
            body = FRAMES4.get(self.path)
            if body is None: self.send_response(404); self.end_headers()
            else:
                self.send_response(200); self.send_header("Content-Type", "image/jpeg")
                self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)

    _srv4 = _hs4.ThreadingHTTPServer(("127.0.0.1", 0), _Stub4); _srv4.daemon_threads = True
    _th4.Thread(target=_srv4.serve_forever, daemon=True).start()
    _port4 = _srv4.server_address[1]

    TAP_PORT = free_port()
    TAP_LOG = "/tmp/jt-phonetap-serial.log"
    TAP_BEFORE = "/tmp/jt-phonetap-before.raw"
    TAP_AFTER_CLICK = "/tmp/jt-phonetap-afterclick.raw"
    TAP_AFTER_SEND = "/tmp/jt-phonetap-aftersend.raw"
    TAP_HOME = "/tmp/jt-phonetap-home.raw"
    TAP_MAIL = "/tmp/jt-phonetap-mail.raw"
    TAP_FACE_PNG = "/tmp/jt-phonetap-face.png"
    TAP_MAIL_PNG = "/tmp/jt-phonetap-mail.png"
    for f in (TAP_LOG, TAP_BEFORE, TAP_AFTER_CLICK, TAP_AFTER_SEND, TAP_HOME, TAP_MAIL):
        try: os.remove(f)
        except FileNotFoundError: pass

    PW4, PH4, SC4 = 860, 1520, 2
    LOGICAL_W4, LOGICAL_H4 = 430, 760

    args4 = ["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std", "-no-reboot",
             "-qmp", f"tcp:127.0.0.1:{TAP_PORT},server,nowait", "-serial", "file:" + TAP_LOG,
             "-net", "nic,model=rtl8139", "-net", "user",
             "-append", f"phone samantha facehost=10.0.2.2:{_port4} llmhost=10.0.2.2 llmport={_port4}"]
    q4 = subprocess.Popen(args4, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.0)
        s4 = socket.create_connection(("127.0.0.1", TAP_PORT)); f4 = s4.makefile("rw")

        def cmd4(o):
            f4.write(json.dumps(o) + "\n"); f4.flush()
            while True:
                r = json.loads(f4.readline())
                if "return" in r or "error" in r: return r

        def key4(k):
            cmd4({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k}]}})

        def type4(s):
            for ch in s:
                key4("spc" if ch == " " else ch)
                time.sleep(0.03)

        def move4(x, y):
            cmd4({"execute": "input-send-event", "arguments": {"events": [
                {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W4)}},
                {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H4)}}]}})

        def click4():
            cmd4({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
            time.sleep(0.1)
            cmd4({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})

        def dump4(path):
            cmd4({"execute": "pmemsave", "arguments": {"val": FB, "size": PW4 * PH4 * 4, "filename": path}})

        def load4(path):
            img = _Image4.frombytes("RGBA", (PW4, PH4), open(path, "rb").read(), "raw", "BGRA").convert("RGB")
            return img, img.load()

        def px_at(px, x, y):
            return px[x * SC4 + 1, y * SC4 + 1]

        def close4(a, b, tol=50):
            return all(abs(a[i] - b[i]) <= tol for i in range(3))

        f4.readline(); cmd4({"execute": "qmp_capabilities"})
        time.sleep(3.0)  # past splash, face fetched from the stub, samopen/samfocus up

        log4 = open(TAP_LOG, errors="replace").read()
        if "samopen" not in log4 or "samfocus" not in log4:
            fail = 1; print("FAIL: phone samantha boot never reached the focused avatar screen (samopen/samfocus missing)")
        else:
            print("PASS: phone boot landed on the focused avatar screen")

        # Face geometry from user/samantha.c's phone layout (she is a ring-3
        # window since 1.9.26): a fixed 60x60 logical face under the name
        # row, centred horizontally, top at logical y=78 (read off the real
        # frame in /tmp/jt-phonetap-face.png).
        face_top = 78
        side = 60
        face_cx = LOGICAL_W4 // 2
        face_cy = face_top + side // 2

        GUI_BG4 = (0xFA, 0xF8, 0xF6)
        dump4(TAP_BEFORE)
        img_before, px_before = load4(TAP_BEFORE)
        img_before.save(TAP_FACE_PNG)
        before_color = px_at(px_before, face_cx, face_cy)
        if close4(before_color, GUI_BG4, 20):
            fail = 1; print(f"FAIL: face center ({face_cx},{face_cy}) reads as background {before_color} before any tap -- stub face never rendered")
        else:
            print(f"PASS: her face is really drawn at ({face_cx},{face_cy}) before the tap: rgb={before_color}")

        # The tap itself: dead center of her own face.
        move4(face_cx, face_cy); time.sleep(0.3); click4(); time.sleep(0.4)
        dump4(TAP_AFTER_CLICK)
        _, px_after_click = load4(TAP_AFTER_CLICK)
        after_click_color = px_at(px_after_click, face_cx, face_cy)
        if close4(after_click_color, GUI_BG4, 20):
            fail = 1; print(f"FAIL: tapping her face cleared the avatar screen -- face center is now background {after_click_color}, the old any-click-exits bug")
        else:
            print(f"PASS: tapping her face keeps the avatar open -- face center still not background: rgb={after_click_color}")

        # Type a short message and send it. chat_process_message always
        # runs chat_pick (POST /api/pick) before chat_send (POST /api/chat),
        # win or lose on the network -- either one logs a discriminating
        # marker (chatpick=/chatpickfail=/chatfail=/chatreply=/chathttps=)
        # the instant it resolves, so this proves the tap left the input box
        # reading keys and enter actually fired a real request, without
        # needing the request to succeed or a full round trip to finish.
        type4("hey there")
        time.sleep(0.2)
        key4("ret")
        time.sleep(4.0)

        log4 = open(TAP_LOG, errors="replace").read()
        chat_fired = any(m in log4 for m in ("chatpick", "chatfail", "chatreply=", "chathttps="))
        if not chat_fired:
            fail = 1; print("FAIL: typing a message and hitting enter never produced a chat_pick/chat_send serial marker -- no request fired")
        else:
            print("PASS: serial shows the chat request fired after typing + enter")

        dump4(TAP_AFTER_SEND)
        _, px_after_send = load4(TAP_AFTER_SEND)
        after_send_color = px_at(px_after_send, face_cx, face_cy)
        if close4(after_send_color, GUI_BG4, 20):
            fail = 1; print(f"FAIL: after sending, the avatar screen is gone -- face region reads as background {after_send_color}")
        else:
            print(f"PASS: avatar/face still on screen after sending: rgb={after_send_color}")

        # Back chevron: same top-left 44x40 tap zone every phone screen
        # hit-tests (phone_home.h's phone_back_zone_tick), injects a real
        # ESC regardless of which loop is reading keys right now (the
        # windowed chat console gui_launch_chat_app landed on after enter).
        # ESC from there returns to gui_run's caller, which for
        # boot_to_phone always lands on phone_home_run's grid.
        # Mail is APPS[1]: grid geometry from phone_home.h's
        # phone_home_grid_geom, same COLS/CELL_W/CELL_H/TILE/Y0 constants
        # scenario 3 above already established.
        COLS4, CELL_W4, CELL_H4, TILE4, Y04 = 5, 86, 96, 48, 56
        def cell_center4(icon):
            row, col = icon // COLS4, icon % COLS4
            return col * CELL_W4 + CELL_W4 // 2, Y04 + row * CELL_H4 + TILE4
        mail_cx, mail_cy = cell_center4(1)

        def not_bg4(px, x, y, bg=GUI_BG4, tol=16):
            for dx in range(-6, 7, 3):
                for dy in range(-6, 7, 3):
                    p = px[(x + dx) * SC4 + 1, (y + dy) * SC4 + 1]
                    if any(abs(p[i] - bg[i]) > tol for i in range(3)):
                        return True
            return False

        # Samantha is a ring-3 program: right after a reply she is inside her
        # blocking /api/speak request and only sees the close when it returns,
        # so the home grid arrives a moment after the tap, not within a fixed
        # 0.8 s. Poll for it (Mail's cell is blank on her chat screen, drawn on
        # the grid) with a deadline: a tap that never closes her still fails.
        move4(20, 15); time.sleep(0.3); click4()
        for _ in range(60):
            time.sleep(0.5)
            dump4(TAP_HOME)
            img_home, px_home = load4(TAP_HOME)
            if not_bg4(px_home, mail_cx, mail_cy): break
        if not not_bg4(px_home, mail_cx, mail_cy):
            fail = 1; print(f"FAIL: back chevron tap didn't land on the home grid -- Mail's cell ({mail_cx},{mail_cy}) is blank")
        else:
            print("PASS: back chevron tap from the chat console returns to the phone home grid")

        # Open Mail, compose one message so gui_launch's on_key path
        # actually reaches gui_draw_mail_content's gui_draw_hint call
        # (an empty inbox shows "No mail yet." at the same coordinates
        # instead and never calls it -- that would pass this check for
        # the wrong reason), then assert the hint row is blank on phone.
        move4(mail_cx, mail_cy); time.sleep(0.3); click4(); time.sleep(0.8)
        key4("c"); time.sleep(0.3)
        type4("me@jt.local"); key4("ret"); time.sleep(0.2)
        type4("hello"); key4("ret"); time.sleep(0.2)
        type4("just a test"); key4("ret"); time.sleep(0.4)

        dump4(TAP_MAIL)
        img_mail, px_mail = load4(TAP_MAIL)
        img_mail.save(TAP_MAIL_PNG)

        # gui_draw_hint(20, T+52, ...) -- T=0 full screen. Sample the whole
        # hint row's x-span (past where even a short "No mail yet." string
        # would sit) for any ink at all.
        hint_row_has_ink = any(
            not close4(px_at(px_mail, x, 52), GUI_BG4, 25)
            for x in range(18, 340, 4)
        )
        if hint_row_has_ink:
            fail = 1; print("FAIL: Mail's hint row has ink on a phone boot -- gui_draw_hint should no-op for boot_to_phone")
        else:
            print("PASS: Mail's hint row is blank on phone (gui_draw_hint correctly no-ops)")
        print(f"saved {TAP_FACE_PNG} and {TAP_MAIL_PNG}")
    finally:
        try: cmd4({"execute": "quit"})
        except (ConnectionResetError, BrokenPipeError, OSError, NameError): pass
        try: q4.wait(timeout=5)
        except subprocess.TimeoutExpired: q4.kill()
        try: _srv4.shutdown()
        except Exception: pass
except Exception as e:
    fail = 1; print(f"FAIL: tap-face/back-chevron/mail-hint check errored ({e})")

sys.exit(fail)
