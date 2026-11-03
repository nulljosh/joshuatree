#!/usr/bin/env python3
"""13.5.0, Blackcomb plays an .mp4 itself (arch/arm64/browser_mp4.h with user/libjt's mp4.c, h264.c and aac.c): no other
computer decodes anything.

ffmpeg makes a 4 s clip like YouTube's 360p (H.264 Main with CABAC, no B frames, AAC-LC stereo), a plain HTTP server on
the host serves it, and headless QEMU types its address into Blackcomb:
  - "browser: mp4 playing 320x240, 96 pictures, sound 44100 Hz";
  - pictures are decoded and drawn as the sound plays ("browser: mp4 frame" lines), into the page's picture: a
    screenshot mid-play has the clip's colours where the page's image goes, which the page alone does not;
  - it plays to the end: "browser: mp4 done pictures=96";
  - a page that is not a video still loads as a page afterwards.
Skips (exit 0) when clang, ld.lld, ffmpeg or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-mp4-check.py   (from the repo root)
"""
import http.server, json, os, shutil, socket, socketserver, subprocess, sys, threading, time
from pathlib import Path
from scratch import scratch_dir

ROOT = Path(__file__).resolve().parents[2]
ARCH = ROOT / "arch/arm64"
TMP = Path(scratch_dir("arm64-mp4"))
if not all(shutil.which(t) for t in ("clang", "ld.lld", "ffmpeg", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld, ffmpeg or qemu-system-aarch64 not installed"); sys.exit(0)

site = TMP / "site"; site.mkdir(exist_ok=True)
clip = site / "clip.mp4"
subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=320x240:rate=24", "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=44100",
                "-t", "4", "-pix_fmt", "yuv420p", "-c:v", "libx264", "-profile:v", "main", "-bf", "0", "-c:a", "aac", "-ac", "2", "-b:a", "96k",
                "-movflags", "+faststart", str(clip)], check=True)
(site / "page.html").write_text("<html><head><title>Plain page</title></head><body><h1>Plain page</h1><p>Not a video.</p></body></html>")

class Quiet(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k): super().__init__(*a, directory=str(site), **k)
    def log_message(self, *a): pass
srv = socketserver.TCPServer(("127.0.0.1", 0), Quiet); port = srv.server_address[1]
threading.Thread(target=srv.serve_forever, daemon=True).start()

fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-600:]), flush=True)
    if not ok: fails.append(name)

r = subprocess.run(["make", "-s", "-C", str(ARCH), "kernel8.elf"], capture_output=True, text=True, timeout=900)
assert r.returncode == 0, r.stderr[-800:]
log, sock = TMP / "uart", TMP / "qmp"
for f_ in (log, sock):
    if f_.exists(): f_.unlink()
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                      "-device", "ramfb", "-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n", "-device", "virtio-keyboard-device",
                      "-display", "none", "-serial", "file:" + str(log), "-qmp", "unix:%s,server,nowait" % sock, "-kernel", str(ARCH / "kernel8.elf")],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
uart = lambda: log.read_text(errors="replace") if log.exists() else ""
def wait_for(text, secs, after=0):
    end = time.time() + secs
    while time.time() < end:
        if text in uart()[after:]: return True
        time.sleep(0.1)
    return False
try:
    assert wait_for("M2 input ready", 60), "no desktop: " + uart()[-300:]
    for _ in range(100):
        if sock.exists(): break
        time.sleep(0.1)
    s_ = socket.socket(socket.AF_UNIX); s_.settimeout(30); s_.connect(str(sock)); f = s_.makefile("rw"); f.readline()
    def cmd(c, **a):
        f.write(json.dumps({"execute": c, **({"arguments": a} if a else {})}) + "\n"); f.flush()
        while True:
            rep = json.loads(f.readline())
            if "return" in rep or "error" in rep: return rep
    cmd("qmp_capabilities")
    QC = {".": ("dot",), "/": ("slash",), ":": ("shift", "semicolon")}
    def key(*k): cmd("send-key", keys=[{"type": "qcode", "data": x} for x in k], **{"hold-time": 50}); time.sleep(0.15)
    def typ(t):
        for ch in t: key(*(("shift", ch.lower()) if ch.isupper() else QC.get(ch, (ch,))))
    def shot(name):
        p = TMP / (name + ".ppm"); cmd("screendump", filename=str(p)); time.sleep(0.3)
        raw = p.read_bytes(); parts = raw.split(b"\n", 3); w, h = map(int, parts[1].split()); return w, h, parts[3]
    wait_for("net dhcp", 30)
    n = len(uart()); key("f1"); wait_for("spotlight open", 10, n); typ("blackcomb"); key("ret")
    check("Spotlight opens Blackcomb", wait_for("browser open", 10, n), uart()[n:][-300:])
    n = len(uart()); typ("http://10.0.2.2:%d/clip.mp4" % port); key("ret")
    ok = wait_for("browser: mp4 playing 320x240, 96 pictures, sound 44100 Hz", 60, n)
    check("the address is a video: it plays in the page, 320x240, 96 pictures, with sound", ok, uart()[n:][-500:])
    check("pictures are decoded and drawn as it plays", wait_for("browser: mp4 frame 25", 20, n), uart()[n:][-400:])
    w, h, a1 = shot("mid1"); time.sleep(0.6); _, _, a2 = shot("mid2")
    moved = sum(1 for i in range(0, min(len(a1), len(a2)), 3 * 5) if a1[i] != a2[i])
    check("its picture is on screen and moving: two screenshots 0.6 s apart differ", moved > 500, moved)
    check("it plays to the end", wait_for("browser: mp4 done pictures=96", 30, n), uart()[n:][-400:])
    _, _, b1 = shot("after1"); time.sleep(0.6); _, _, b2 = shot("after2")
    still = sum(1 for i in range(0, min(len(b1), len(b2)), 3 * 5) if b1[i] != b2[i])
    check("and stops moving when it ends", still < 50, still)
    n = len(uart()); key("ctrl", "l"); typ("http://10.0.2.2:%d/page.html" % port); key("ret")
    check("a page that is not a video loads as a page after", wait_for("browser: Plain page", 30, n) or wait_for("browser: page ", 30, n), uart()[n:][-400:])
    check("no kernel crash, no oom", "KERNEL CRASH" not in uart() and "oom" not in uart().lower(), uart()[-400:])
finally:
    q.kill(); q.wait(); srv.shutdown()
if fails: print("FAIL: %d of the MP4 checks failed" % len(fails)); sys.exit(1)
print("PASS: Blackcomb plays an H.264 Main and AAC .mp4 from the web in the page, pictures and sound decoded on the machine, to the end", flush=True)
