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
from freeport import free_port
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
from freeport import free_port
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
# locally (chat_run_tool's new_reminder, no llmhost needed), and assert
# CHAT_ACCENT-colored pixels (her name, drawn in 0x00B7862A) still exist
# across the label's known column span -- not just at x=20, which a
# half-covered label would still pass.
import http.server, io, threading

try:
    from PIL import Image as _Image2
from freeport import free_port

    def jpg(color):
        b = io.BytesIO(); _Image2.new("RGB", (320, 320), color).save(b, "JPEG", quality=90); return b.getvalue()

    FACE_RED = (200, 30, 30)
    FRAMES = {f"/face/idle-{i}.jpg": jpg(FACE_RED) for i in range(3)}

    class _Stub(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a): pass
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
    for f in (LABEL_LOG, LABEL_DUMP):
        try: os.remove(f)
        except FileNotFoundError: pass
    args = ["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std", "-no-reboot",
            "-qmp", f"tcp:127.0.0.1:{PORT + 1},server,nowait", "-serial", "file:" + LABEL_LOG,
            "-net", "nic,model=rtl8139", "-net", "user",
            "-append", f"phone samantha facehost=10.0.2.2:{_port}"]
    q2 = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.0)
        s2 = socket.create_connection(("127.0.0.1", PORT + 1)); f2 = s2.makefile("rw")

        def cmd2(o):
            f2.write(json.dumps(o) + "\n"); f2.flush()
            while True:
                r = json.loads(f2.readline())
                if "return" in r or "error" in r: return r
        f2.readline(); cmd2({"execute": "qmp_capabilities"})
        time.sleep(3.0)

        def keys(k): cmd2({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k}]}})
        for ch in "remind me to call mom":
            keys("spc" if ch == " " else ch); time.sleep(0.03)
        time.sleep(0.3)
        keys("ret")
        time.sleep(2.0)
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

        def close2(a, b, tol=40): return all(abs(a[i] - b[i]) <= tol for i in range(3))

        # "Samantha" (accent color) spans roughly x=42..159 at this scale/
        # font -- sample across that whole span, not just the left edge,
        # so a face that covers the word's second half still fails this.
        cols_hit = sum(1 for x in range(42, 160, 6) if any(close2(px2[x, y], ACCENT) for y in range(100, 130)))
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
from freeport import free_port

    PW, PH = 860, 1520          # 430x760 logical at 2x, same convention as the rest of this file
    LOGICAL_W, LOGICAL_H, SC = 430, 760, 2
    FIT_PORT = PORT + 2
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

        # All 26 real apps' cells must land inside the 760px logical
        # screen (no scrolling) -- the exact bug Joshua's screenshot
        # review caught at 4 columns (Activity idx24, Clock idx25 sitting
        # off the bottom, unreachable by any tap).
        GUI_APPS_FOLDER = 26
        offscreen = [i for i in range(GUI_APPS_FOLDER) if cell_center(i)[1] + 24 > LOGICAL_H]
        if offscreen:
            fail = 1; print(f"FAIL: {len(offscreen)} app cell(s) fall below the {LOGICAL_H}px screen: {offscreen}")
        else:
            print(f"PASS: all {GUI_APPS_FOLDER} app cells fit inside the {LOGICAL_H}px screen, no scrolling needed")

        cal_cx, cal_cy = cell_center(2)   # Calendar, APPS[2]
        key_cx, key_cy = cell_center(9)   # Keyrate, APPS[9], ring-3
        act_cx, act_cy = cell_center(24)  # Activity, APPS[24] -- the row the 4-col grid used to drop
        clk_cx, clk_cy = cell_center(25)  # Clock, APPS[25]

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


sys.exit(fail)
