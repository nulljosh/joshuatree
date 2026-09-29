#!/usr/bin/env python3
"""Headless proof of the "samantha" boot flag (kernel.c's boot_to_samantha,
kernel/chat.h's chat_boot_samantha_open): with "samantha" on the multiboot
command line, the very first GUI frame after the splash is Chat's
full-screen avatar view, not the icon desktop -- and without the flag,
boot behaves exactly as it always has.

Same boot + QMP + serial shape as bootlogo-check.py/chat-samantha-check.py:
boots kernel.elf under `-display none`, waits past the splash, and reads
serial for the two markers chat_boot_samantha_open prints (samopen once
her big face and caption are drawn, samfocus once the input box is drawn
and reading keys every frame -- i.e. focused) plus kernel.c's guidesktop
marker (the icon desktop's own first line, printed only in the non-flag
path). A screendump is also taken in the flag case as a second, visual
confirmation that something real painted the framebuffer (not just a
serial marker with no matching draw).

Two boots:
  with "-append samantha":    samopen and samfocus must appear, guidesktop
                               must not (the desktop is skipped entirely).
  with no cmdline at all:     guidesktop must appear, samopen/samfocus must
                               not (normal desktop, unchanged).

Usage: tools/checks/samantha-boot-check.py   (from the repo root, after make kernel.elf)
"""
from freeport import free_port
import json, os, socket, subprocess, sys, time

PORT = free_port()
FB = 0xfd000000
W, H = 1920, 1080

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def boot(append, log_path, dump_path=None):
    for f in (log_path, dump_path):
        if f:
            try: os.remove(f)
            except FileNotFoundError: pass
    args = ["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
            "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + log_path]
    if append:
        args += ["-append", append]
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
        time.sleep(2.5)  # past the boot splash, same window bootlogo-check.py waits
        if dump_path:
            cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": dump_path}})
        try: cmd({"execute": "quit"})
        except (ConnectionResetError, BrokenPipeError, OSError): pass
    finally:
        try: q.wait(timeout=5)
        except subprocess.TimeoutExpired: q.kill()
    return open(log_path, errors="replace").read()


fail = 0

# 1. With the flag: her avatar opens first, desktop never draws.
log_on = boot("samantha", "/tmp/jt-samboot-on.log", "/tmp/jt-samboot-on.raw")
if "samopen" not in log_on:
    fail = 1; print("FAIL: -append samantha never printed samopen -- avatar view did not open")
if "samfocus" not in log_on:
    fail = 1; print("FAIL: -append samantha never printed samfocus -- input box never drew/focused")
if "guidesktop" in log_on:
    fail = 1; print("FAIL: -append samantha still reached the icon desktop (guidesktop fired)")
if "samopen" in log_on and "guidesktop" in log_on and log_on.index("guidesktop") < log_on.index("samopen"):
    fail = 1; print("FAIL: desktop marker printed before the avatar view -- wrong order")

# Visual sanity: something other than a flat background actually painted.
try:
    from PIL import Image
    img = Image.frombytes("RGBA", (W, H), open("/tmp/jt-samboot-on.raw", "rb").read(), "raw", "BGRA").convert("L")
    px = img.load()
    non_bg = sum(1 for y in range(0, H, 3) for x in range(0, W, 3) if px[x, y] < 240)
    # Soft signal only: without facehost= on this boot her face frames never
    # fetch (chat_face.h is opt-in by design), so this view is caption text
    # and an input box, not a big photo -- real, but a much smaller ink
    # count than a full face. The serial markers above are the real proof;
    # this just confirms *something* painted, not a flat, empty screen.
    if non_bg < 20:
        fail = 1; print(f"FAIL: samantha boot framebuffer looks empty (non_bg={non_bg})")
    else:
        print(f"samantha boot framebuffer has real content (non_bg samples={non_bg})")
except Exception as e:
    print(f"note: skipped pixel check ({e})")

# 2. Without the flag: normal desktop, her avatar never opens.
log_off = boot(None, "/tmp/jt-samboot-off.log")
if "guidesktop" not in log_off:
    fail = 1; print("FAIL: boot with no cmdline never reached the normal desktop (guidesktop missing)")
if "samopen" in log_off or "samfocus" in log_off:
    fail = 1; print("FAIL: samantha's avatar view opened without the flag")

if not fail:
    print("PASS: \"samantha\" on the boot command line opens Chat's full-screen avatar view, input focused, before the desktop; without it, boot is unchanged")
sys.exit(fail)
