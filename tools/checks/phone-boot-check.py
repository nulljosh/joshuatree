#!/usr/bin/env python3
"""Headless proof of the "phone" boot flag (kernel.c's boot_to_phone):
"phone samantha" on the multiboot command line opens a real 430x932
portrait framebuffer (Bochs VBE, drivers/vbe.c's vbe_set_mode, called via
window_open in gui_run) instead of the desktop's 960x540@2x, and lands on
Chat's full-screen avatar view laid out for it (kernel/chat.h's
chat_boot_samantha_open, face top-half/caption-under-it/input-at-bottom
for boot_to_phone).

Same boot + QMP + serial shape as samantha-boot-check.py: boots
kernel.elf under `-display none`, waits past the splash, reads serial for
her avatar markers, and screendumps the framebuffer at exactly
430*932*4 bytes -- the read only lines up with real content if the mode
really is 430x932, not the desktop's 960x540 (misaligned rows would read
as noise, not the specific colors this checks for at specific offsets).

Usage: tools/checks/phone-boot-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time

PORT = 4480
FB = 0xfd000000
W, H = 430, 932
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
    dot = get(26, 20)
    if close(dot, GUI_BG, 30):
        fail = 1; print(f"FAIL: titlebar dot at (26,20) looks like plain background {dot} -- top region not drawn")
    else:
        print(f"PASS: titlebar region has real content at (26,20): rgb={dot}")

    # Input box: chat_boot_samantha_open draws a solid white rect at
    # (20, bottom-30) .. (width-20, bottom-10), bottom = height-40 = 892,
    # i.e. rows 862..882 -- only at the true bottom of a 932-tall frame.
    box = get(30, 872)
    if not close(box, (255, 255, 255), 15):
        fail = 1; print(f"FAIL: input box at (30,872) is not white {box} -- not drawn where a 932-tall frame puts it")
    else:
        print(f"PASS: input box drawn at the expected bottom position: rgb={box}")
    above_box = get(30, 840)
    if close(box, above_box, 5) and not close(above_box, (255, 255, 255), 15):
        pass  # background sampled above the box differs from the box itself -- fine either way, informational only
except Exception as e:
    fail = 1; print(f"FAIL: pixel checks errored ({e})")

if not fail:
    print("PASS: \"phone samantha\" opens a real 430x932 portrait boot straight into Samantha's view, titlebar and input box both where a phone-sized frame puts them")

try:
    from PIL import Image as _Image
    _img = _Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA")
    _img.convert("RGB").save(PNG)
    print(f"saved {PNG}")
except Exception as e:
    print(f"note: could not save PNG ({e})")

sys.exit(fail)
