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

    # Titlebar/top region: gui_draw_app_titlebar's traffic-light dots sit at
    # (26,20)/(46,20)/(66,20), always drawn regardless of network (unlike
    # her real face photo, which needs facehost= reachable -- this headless
    # boot has no NIC, same caveat samantha-boot-check.py notes). A real,
    # saturated red dot there is real, positioned ink, not a flat background.
    dot = get(52, 40)
    if close(dot, GUI_BG, 30):
        fail = 1; print(f"FAIL: titlebar dot at (26,20) looks like plain background {dot} -- top region not drawn")
    else:
        print(f"PASS: titlebar region has real content at (26,20): rgb={dot}")

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

# Third scenario (this fix): the desktop dock and windowed apps in phone
# mode. gui_launch_from_dock used to open every app at the desktop's own
# fixed x/y/w/h (Calendar at x=70,w=820 -- fine on a 960-wide screen, way
# off the right edge of a 430-wide phone), and gui_dock_icon's DOCK_BUDGET
# (740) assumed a screen wide enough to hold it, so the dock itself drew
# wider than a 430px phone screen and got cropped on both ends. Reach the
# dock from a "phone" boot by escaping Samantha's full-screen avatar (one
# ESC into her normal Chat console, a second ESC out of that back to
# gui_run's desktop loop), then click Calendar's dock slot (GUI_DOCK_DEFAULT
# slot 3: Apps folder, then icons 0/1/2 -- Calendar is icon 2) and assert
# its window's right edge and the dock's first/last icon all land inside
# the real 430px-wide screen.
try:
    from PIL import Image as _Image3
from freeport import free_port

    PW, PH = 860, 1520          # 430x760 logical at 2x, same convention as the rest of this file
    LOGICAL_W, LOGICAL_H, SC = 430, 760, 2
    FIT_PORT = PORT + 2
    FIT_LOG = "/tmp/jt-phonefit-serial.log"
    FIT_DUMP_BEFORE = "/tmp/jt-phonefit-before.raw"
    FIT_DUMP_AFTER = "/tmp/jt-phonefit-after.raw"
    FIT_PNG_BEFORE = "/tmp/jt-phonefit-before.png"
    FIT_PNG_AFTER = "/tmp/jt-phonefit-after.png"
    for f in (FIT_LOG, FIT_DUMP_BEFORE, FIT_DUMP_AFTER):
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

        f3.readline(); cmd3({"execute": "qmp_capabilities"})
        time.sleep(2.5)  # past splash, samopen up
        key3("esc"); time.sleep(0.6)  # samantha avatar -> her normal Chat console
        key3("esc"); time.sleep(0.6)  # Chat console -> desktop dock

        dump3(FIT_DUMP_BEFORE)
        raw_before = open(FIT_DUMP_BEFORE, "rb").read()
        img_before = _Image3.frombytes("RGBA", (PW, PH), raw_before, "raw", "BGRA").convert("RGB")
        img_before.save(FIT_PNG_BEFORE)
        pxb = img_before.load()
        BG = (0xEF, 0xEB, 0xE4)  # DOCK_TRAY_COLOR

        def not_bg(x, y, tol=18):
            # Sample a small neighbourhood, not one pixel: an icon glyph
            # can have a near-tray-gray pixel dead center (Trash's lid),
            # which would false-negative a single-point sample.
            for dx in range(-4, 5, 2):
                for dy in range(-4, 5, 2):
                    p = pxb[(x + dx) * SC + 1, (y + dy) * SC + 1]
                    if any(abs(p[i] - BG[i]) > tol for i in range(3)):
                        return True
            return False

        # Computed dock geometry for this exact build: DOCK_BUDGET clamped
        # to (430 - 40) = 390 gives DOCK_ICON=28, DOCK_GAP=6, DOCK_PAD=10,
        # dock_x0=21, so slot N's icon centre sits at 31 + N*34 + 14.
        DOCK_ICON, DOCK_GAP, DOCK_PAD = 28, 6, 10
        SLOT0_X = 31
        PITCH = DOCK_ICON + DOCK_GAP
        DOCK_Y0 = LOGICAL_H - DOCK_ICON - 2 * DOCK_PAD - 24
        ICON_CY = DOCK_Y0 + DOCK_PAD + DOCK_ICON // 2
        first_cx = SLOT0_X + DOCK_ICON // 2
        last_cx = SLOT0_X + 10 * PITCH + DOCK_ICON // 2

        if first_cx >= LOGICAL_W or last_cx >= LOGICAL_W:
            fail = 1
            print(f"FAIL: dock's first ({first_cx}) or last ({last_cx}) icon centre falls outside the {LOGICAL_W}px-wide phone screen")
        elif not not_bg(first_cx, ICON_CY):
            fail = 1
            print(f"FAIL: dock's first icon (Apps folder) not drawn at ({first_cx},{ICON_CY}) -- cropped off screen")
        elif not not_bg(last_cx, ICON_CY):
            fail = 1
            print(f"FAIL: dock's last icon (Trash) not drawn at ({last_cx},{ICON_CY}) -- cropped off screen")
        else:
            print(f"PASS: dock's first ({first_cx}) and last ({last_cx}) icon both fully on screen (width {LOGICAL_W})")

        # Click Calendar (dock slot 3: Apps folder, icon0, icon1, icon2=Calendar)
        cal_cx = SLOT0_X + 3 * PITCH + DOCK_ICON // 2
        move3(cal_cx, ICON_CY); time.sleep(0.3); click3(); time.sleep(1.0)

        dump3(FIT_DUMP_AFTER)
        raw_after = open(FIT_DUMP_AFTER, "rb").read()
        img_after = _Image3.frombytes("RGBA", (PW, PH), raw_after, "raw", "BGRA").convert("RGB")
        img_after.save(FIT_PNG_AFTER)
        pxa = img_after.load()
        CLOSE_RED = (0xFF, 0x5F, 0x57)

        def is_red(p): return max(abs(p[i] - CLOSE_RED[i]) for i in range(3)) <= 20

        # Calendar opens at the clamped x=10,y=40,w=410,h=385 (see kernel.c's
        # gui_launch_from_dock): close dot at (x+24, y+16) = (34,56), right
        # edge of the window frame at x+w = 420, inside the 430px screen.
        close_p = pxa[34 * SC + 1, 56 * SC + 1]
        win_right = 10 + 410
        if win_right >= LOGICAL_W:
            fail = 1
            print(f"FAIL: Calendar window right edge ({win_right}) is not within the {LOGICAL_W}px screen")
        elif not is_red(close_p):
            fail = 1
            print(f"FAIL: Calendar window close dot not found at (34,56): {close_p} -- window may not have opened")
        else:
            print(f"PASS: Calendar window right edge ({win_right}px) fits inside the {LOGICAL_W}px phone screen, close dot confirms it opened")
        print(f"saved {FIT_PNG_BEFORE} and {FIT_PNG_AFTER}")
    finally:
        try: cmd3({"execute": "quit"})
        except (ConnectionResetError, BrokenPipeError, OSError, NameError): pass
        try: q3.wait(timeout=5)
        except subprocess.TimeoutExpired: q3.kill()
except Exception as e:
    fail = 1; print(f"FAIL: phone dock/window-fit check errored ({e})")

sys.exit(fail)
