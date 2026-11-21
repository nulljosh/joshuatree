#!/usr/bin/env python3
"""Film the Pi's big features for the landing: one headless boot of the ARM kernel per clip (QEMU virt, a USB keyboard and
mouse behind a hub, the same machine arm64-spaces-check.py drives), a short scripted use, recorded with QMP screendump
at about 10 fps into landing/clips/feature-<name>.mp4, muted, 800x600, with its first frame as feature-<name>.jpg.

Clips: spaces (Ctrl+Left/Right between desktops), mission (Ctrl+Up), spotlight (Cmd+Space, type, Enter), launchpad
(the dock's first tile).

Features that need a fake relay, radio or signed release are filmed through their own check instead (tools/film/), then cut:

Usage: python3 tools/feature-clips.py [name ...]   (from anywhere; default: all)
       python3 tools/feature-clips.py --cut FILMED.mp4 START END NAME [SPEED]   (seconds; SPEED 2 plays a slow wait twice as fast)
"""
import json, os, shutil, socket, subprocess, sys, threading, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "checks"))
from scratch import scratch_dir
from sdcard import build_card

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
ARCH = os.path.join(ROOT, "arch", "arm64")
W, H = 800, 600
ICON = max(16, min(H * 7 // 100, (min(740, W - 40) - 2 * 10 - 11 * 6) // 12))
DOCK_X0, DOCK_Y0 = (W - (12 * ICON + 11 * 6 + 2 * 10)) // 2, H - ICON - 2 * 10 - 24
def slot(i): return DOCK_X0 + 10 + i * (ICON + 6) + ICON // 2, DOCK_Y0 + 10 + ICON // 2
PARK = (W - 40, 60)
REMIND, NOTES = 5, 4   # dock slots, as arm64-spaces-check.py has them


def film(name):
    tmp = scratch_dir("feature-clips-" + name)
    log, sock = tmp + "/uart", tmp + "/qmp"
    card = tmp + "/card.img"; build_card(card, False)   # the checks' clean FAT32 card: Reminders and Notes save to it, no "No card" line
    kern = tmp + "/kernel8.elf"; shutil.copy(os.path.join(ARCH, "kernel8.elf"), kern)   # its own copy: a parallel build can't swap it mid-boot
    q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-nic", "none", "-device", "ramfb",
                          "-device", "pcie-root-port,id=rp,chassis=1", "-device", "qemu-xhci,id=x,bus=rp", "-device", "usb-hub,id=h,bus=x.0,port=1",
                          "-global", "virtio-mmio.force-legacy=false", "-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "virtio-blk-device,drive=card",
                          "-device", "usb-kbd,bus=x.0,port=1.1", "-device", "usb-mouse,id=m1,bus=x.0,port=1.2",
                          "-display", "none", "-serial", "file:" + log, "-qmp", "unix:%s,server,nowait" % sock,
                          "-kernel", kern], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def uart(): return open(log, errors="replace").read() if os.path.exists(log) else ""
    def wait_for(text, tries=80, after=0):
        for _ in range(tries):
            if text in uart()[after:]: return True
            time.sleep(0.1)
        return False
    try:
        if not wait_for("usb ready: 1 kbd, 1 mouse", 300): sys.exit(f"FAIL {name}: no USB keyboard and mouse")
        s = socket.socket(socket.AF_UNIX); s.connect(sock); f = s.makefile("rw"); f.readline(); lock = threading.Lock()
        def cmd(c, **a):
            with lock:
                f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
                while True:
                    r = json.loads(f.readline())
                    if "return" in r or "error" in r: return r
        cmd("qmp_capabilities"); time.sleep(0.5)
        pos = [W // 2, H // 2]
        def move_to(x, y):
            while (pos[0], pos[1]) != (x, y):
                dx = max(-100, min(100, x - pos[0])); dy = max(-100, min(100, y - pos[1]))
                cmd("input-send-event", events=[{"type": "rel", "data": {"axis": "x", "value": dx}}, {"type": "rel", "data": {"axis": "y", "value": dy}}])
                pos[0] += dx; pos[1] += dy
                if not wait_for(f"mouse {pos[0]},{pos[1]}\n", 40): sys.exit(f"FAIL {name}: the pointer did not reach {x},{y}")
        def click():
            for down in (True, False): cmd("input-send-event", events=[{"type": "btn", "data": {"down": down, "button": "left"}}])
        def keys(*k, wait=0.6): cmd("send-key", keys=[{"type": "qcode", "data": x} for x in k]); time.sleep(wait)
        def type_text(t):
            for ch in t: keys({" ": "spc"}.get(ch, ch), wait=0.12)
        def open_app(i, needle):
            n = len(uart()); move_to(*slot(i)); time.sleep(0.3); click()
            if not wait_for(needle, 100, n): sys.exit(f"FAIL {name}: no '{needle}'")
            move_to(*PARK); time.sleep(0.8)

        n = len(uart()); move_to((W - 500) // 2 + 24, 100 + 16); time.sleep(0.2); click()   # the boot Console's red dot
        if not wait_for("console closed", 40, n): sys.exit(f"FAIL {name}: the boot Console did not close")
        move_to(*PARK); time.sleep(0.5)
        if name in ("spaces", "mission"): open_app(REMIND, "M3c window reminders first frame")   # not Calendar (virt has no clock the Pi reads: 1970), not Terminal (the Console follows every desktop)
        if name == "mission": open_app(NOTES, "M3c window notes first frame")

        # record from here: a thread pulls screendumps and pipes them to ffmpeg on the wall clock
        out = os.path.join(ROOT, "landing", "clips", f"feature-{name}.mp4"); os.makedirs(os.path.dirname(out), exist_ok=True)
        enc = subprocess.Popen(["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{W}x{H}",
                                "-use_wallclock_as_timestamps", "1", "-i", "-", "-vf", "fps=15", "-c:v", "libx264", "-preset", "slow",
                                "-crf", "28", "-pix_fmt", "yuv420p", "-movflags", "+faststart", "-an", out], stdin=subprocess.PIPE)
        stop = threading.Event()
        def grab():
            k = 0
            while not stop.is_set():
                path = f"{tmp}/f{k % 4}.ppm"; k += 1
                cmd("screendump", filename=path)
                data = open(path, "rb").read().split(b"\n", 3)[3]
                if len(data) == W * H * 3: enc.stdin.write(data)
        t = threading.Thread(target=grab); t.start(); time.sleep(1.0)

        if name == "spaces":
            keys("ctrl", "right", wait=1.2); open_app(NOTES, "M3c window notes first frame"); time.sleep(0.6)
            keys("ctrl", "left", wait=1.4); keys("ctrl", "right", wait=1.4); keys("ctrl", "left", wait=1.2)
        elif name == "mission":
            keys("ctrl", "up", wait=2.0); keys("esc", wait=1.0); keys("ctrl", "up", wait=2.0); keys("esc", wait=0.8)
        elif name == "spotlight":
            keys("meta_l", "spc", wait=0.8); type_text("term"); time.sleep(0.8); keys("ret", wait=1.5)
            type_text("cores"); keys("ret", wait=1.5); type_text("mem"); keys("ret", wait=1.5)
        elif name == "launchpad":
            move_to(*slot(0)); time.sleep(0.3); click(); move_to(*PARK); time.sleep(1.5)
            for k in ("right", "right", "down", "right"): keys(k, wait=0.5)
            time.sleep(0.8); keys("esc", wait=1.0)
        time.sleep(1.0)
        stop.set(); t.join(); enc.stdin.close()
        if enc.wait() != 0: sys.exit(f"FAIL {name}: ffmpeg could not write the clip")
        if "KERNEL CRASH" in uart(): sys.exit(f"FAIL {name}: the kernel crashed while filming")
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", out, "-frames:v", "1", "-q:v", "4", out[:-4] + ".jpg"], check=True)
        print(f"filmed {os.path.relpath(out, ROOT)} ({os.path.getsize(out)} bytes)")
    finally:
        q.terminate()
        try: q.wait(5)
        except subprocess.TimeoutExpired: q.kill()


def cut(src, start, end, name, speed="1"):
    """Trim a boot filmed by tools/film/qemu-system-aarch64 into landing/clips/feature-<name>.mp4 and its poster."""
    out = os.path.join(ROOT, "landing", "clips", f"feature-{name}.mp4")
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-ss", start, "-to", end, "-i", src, "-vf", "setpts=PTS/%s" % speed, "-c:v", "libx264", "-preset", "slow", "-crf", "28",
                    "-pix_fmt", "yuv420p", "-movflags", "+faststart", "-an", out], check=True)
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", out, "-frames:v", "1", "-q:v", "4", out[:-4] + ".jpg"], check=True)
    print(f"cut {os.path.relpath(out, ROOT)} ({os.path.getsize(out)} bytes)")


if __name__ == "__main__":
    if sys.argv[1:2] == ["--cut"]: cut(*sys.argv[2:7]); sys.exit(0)
    if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "ffmpeg")): sys.exit("needs clang, ld.lld, qemu-system-aarch64, ffmpeg")
    subprocess.run(["make", "-s", "-C", ARCH, "kernel8.elf"], check=True)
    for n in sys.argv[1:] or ["spaces", "mission", "spotlight", "launchpad"]: film(n)
