#!/usr/bin/env python3
"""YouTube in Blackcomb (6.9.0 and 6.10.0): a video's address shows a page about it, and its Play link plays it in Movies.

Host: tools/claude-relay/yt.py with --fixture (a saved `yt-dlp -j` reply, so no network and no yt-dlp) answers POST
/api/yt/page with the token and a video id; no token is 401, a bad id is 400, and the page has the title, the channel,
the length and the description, with a Play link.
QEMU virt, the kernel built with the service at 10.0.2.2 (the relay's port, plus two for this service) and the token:
Spotlight opens Blackcomb, `youtu.be/jNQXAC9IVRw` is typed, and the UART says `browser: youtube dQw4w9WgXcQ`, the
video's title, and a page of lines. 6.10.0: Tab picks the page's Play link and Enter follows it: the clip (--fixture-avi,
avigen.py's 40 frame CLIP.AVI) is saved as YT.AVI on the check card, PLAYNOW.TXT names it, and Movies opens and plays all
40 frames on its own. The token never appears on the UART.

Discriminating: drop the yt_id branch in browser.c's fetch and the page is youtu.be's own (a redirect, then script).

Usage: tools/checks/arm64-youtube-check.py   (from the repo root)
"""
import json, os, shutil, socket, subprocess, sys, tempfile, time, urllib.request, urllib.error
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch", "arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: no aarch64 toolchain"); sys.exit(0)
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + (f" ({str(detail)[-400:]})" if detail and not ok else ""))
    if not ok: fails.append(name)

TOKEN = "youtube-check-token-0123456789ab"
tmp = scratch_dir("arm64-youtube")
token_file, fixture = os.path.join(tmp, "token"), os.path.join(tmp, "video.json")
open(token_file, "w").write(TOKEN)
json.dump({"title": "Me at the zoo", "channel": "jawed", "duration": 19, "view_count": 380000000,
           "description": "The first video on YouTube.\nElephants have really long trunks."}, open(fixture, "w"))
subprocess.run([sys.executable, os.path.join(root, "tools/media/avigen.py"), os.path.join(tmp, "fx"), "--movie"], check=True, capture_output=True)
clip_avi = os.path.join(tmp, "fx", "CLIP.AVI")
from sdcard import PART, build_card
card = os.path.join(tmp, "card.img"); build_card(card, False)
subprocess.run(["mmd", "-i", f"{card}@@{PART * 512}", "::JT"], capture_output=True, env=dict(os.environ, MTOOLS_SKIP_CHECK="1"))
def free_port():
    while True:
        s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close()
        t = socket.socket()
        try: t.bind(("127.0.0.1", p + 2)); t.close(); return p
        except OSError: t.close()
port = free_port()
yt = subprocess.Popen([sys.executable, os.path.join(root, "tools/claude-relay/yt.py"), "--port", str(port + 2), "--token-file", token_file,
                       "--fixture", fixture, "--fixture-avi", clip_avi], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
q = None
try:
    check("yt.py starts", "listening" in yt.stdout.readline())
    def post(body, auth=True):
        req = urllib.request.Request("http://127.0.0.1:%d/api/yt/page" % (port + 2), data=body.encode(), method="POST")
        if auth: req.add_header("Authorization", "Bearer " + TOKEN)
        try:
            with urllib.request.urlopen(req, timeout=10) as r: return r.status, r.read().decode()
        except urllib.error.HTTPError as e: return e.code, e.read().decode()
    check("no token is refused (401)", post("jNQXAC9IVRw", auth=False)[0] == 401)
    check("a body that is not a video id is refused (400)", post("../../etc/pw")[0] == 400)
    st, page = post("jNQXAC9IVRw")
    check("the page has the title, the channel, the length and a Play link", st == 200 and "<title>Me at the zoo</title>" in page
          and "jawed - 0:19 - 380,000,000 views" in page and "Play in Movies" in page and "<p>Elephants have really long trunks.</p>" in page, page[:400])

    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)   # claude_cfg.h is made once per build: start clean so it takes the token
    env = dict({k: v for k, v in os.environ.items() if not k.startswith("CLAUDE_RELAY")}, CLAUDE_RELAY_HOST="10.0.2.2",
               CLAUDE_RELAY_PORT=str(port), CLAUDE_RELAY_TOKEN_FILE=token_file)
    r = subprocess.run(["make", "-C", arch, "kernel8.elf"], env=env, capture_output=True, text=True, timeout=600)
    if r.returncode: check("arch/arm64 builds with the service's address", False, r.stderr[-800:]); raise SystemExit
    log, sock = os.path.join(tmp, "uart"), os.path.join(tmp, "qmp")
    q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                          "-device", "ramfb", "-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n", "-device", "virtio-keyboard-device",
                          "-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "virtio-blk-device,drive=card",
                          "-display", "none", "-serial", "file:" + log, "-qmp", "unix:%s,server,nowait" % sock,
                          "-kernel", os.path.join(arch, "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def uart(): return open(log, errors="replace").read() if os.path.exists(log) else ""
    def wait_for(text, secs, after=0):
        end = time.time() + secs
        while time.time() < end:
            if text in uart()[after:]: return True
            time.sleep(0.1)
        return False
    if not wait_for("M2 input ready", 60): check("the desktop came up", False, uart()[-300:]); raise SystemExit
    for _ in range(100):
        if os.path.exists(sock): break
        time.sleep(0.1)
    s_ = socket.socket(socket.AF_UNIX); s_.settimeout(30); s_.connect(sock); f = s_.makefile("rw"); f.readline()
    def cmd(c, **a):
        f.write(json.dumps({"execute": c, **({"arguments": a} if a else {})}) + "\n"); f.flush()
        while True:
            rep = json.loads(f.readline())
            if "return" in rep or "error" in rep: return rep
    cmd("qmp_capabilities")
    QCODE = {".": ("dot",), "/": ("slash",)}
    def key(*k): cmd("send-key", keys=[{"type": "qcode", "data": x} for x in k], **{"hold-time": 50}); time.sleep(0.15)
    def type_text(t):
        for ch in t: key(*(("shift", ch.lower()) if ch.isupper() else QCODE.get(ch, (ch,))))
    check("DHCP leased an address", wait_for("net dhcp", 30), uart()[-300:])
    n = len(uart()); key("f1"); wait_for("spotlight open", 10, n); type_text("blackcomb"); key("ret")
    check("Spotlight opens Blackcomb", wait_for("browser open", 10, n), uart()[n:][-300:])
    n = len(uart()); type_text("youtu.be/jNQXAC9IVRw"); key("ret")
    check("the address is a video: it goes to the video service", wait_for("browser: youtube jNQXAC9IVRw", 20, n), uart()[n:][-400:])
    check("and the page about it shows, titled", wait_for("browser: Me at the zoo", 40, n) and wait_for("browser: page ", 10, n), uart()[n:][-500:])
    n = len(uart()); key("tab"); wait_for("browser: selected link 1", 10, n); key("ret")
    check("Play saves the clip as YT.AVI", wait_for("browser: saved YT.AVI, %d bytes" % os.path.getsize(clip_avi), 60, n), uart()[n:][-400:])
    check("and Movies opens and plays it, all 40 frames, on its own", wait_for("movie=play YT.AVI frames=40", 30, n) and wait_for("movie=done frames=40", 30, n), uart()[n:][-500:])
    check("the token never appears on the UART", TOKEN not in uart())
except SystemExit:
    pass
finally:
    if q:
        q.terminate()
        try: q.wait(5)
        except subprocess.TimeoutExpired: q.kill()
    yt.terminate()
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)   # no token left in a build
if fails: print("FAIL: " + "; ".join(fails)); sys.exit(1)
print("PASS: a YouTube address in Blackcomb shows a page about the video from the Mac's video service, its Play link saves the clip and Movies plays it, the token stays off the UART, and the service refuses a missing token and a bad id")
