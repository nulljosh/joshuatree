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

PORT = 4480
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

sys.exit(fail)
