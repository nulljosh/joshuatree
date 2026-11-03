#!/usr/bin/env python3
"""Spotify in Blackcomb (13.3.1): a spotify: address is one command for the Mac's tools/claude-relay/spotify.py, and the
remote it sends back is a page whose links are more spotify: commands.

QEMU virt, the kernel built with the service at 10.0.2.2 (the relay's port, plus three for this service) and the token;
spotify.py runs with --fixture (a saved /v1/me/player reply, so no network and no account). Spotlight opens Blackcomb,
`spotify:` is typed, and the UART says `browser: spotify now` and the page's title, Spotify. Tab picks the page's first
link (Prev) and Enter follows it: `browser: spotify prev`, a second page. The token never appears on the UART.

Discriminating: drop the spotify: branch in browser.c's fetch and the address is a bare host (https://spotify:), which fails;
drop spotify: from resolve() and the Prev link resolves against nothing and never goes out.

Usage: tools/checks/arm64-spotify-check.py   (from the repo root)
"""
import json, os, shutil, socket, subprocess, sys, time
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

TOKEN = "spotify-check-token-0123456789ab"
tmp = scratch_dir("arm64-spotify")
token_file, fixture = os.path.join(tmp, "token"), os.path.join(tmp, "player.json")
open(token_file, "w").write(TOKEN)
json.dump({"is_playing": True, "device": {"name": "Mac mini"},
           "item": {"name": "With or Without You", "artists": [{"name": "U2"}], "album": {"name": "The Joshua Tree"}}}, open(fixture, "w"))
from sdcard import build_card
card = os.path.join(tmp, "card.img"); build_card(card, False)
def free_port():
    while True:
        s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close()
        t = socket.socket()
        try: t.bind(("127.0.0.1", p + 3)); t.close(); return p
        except OSError: t.close()
port = free_port()
sp = subprocess.Popen([sys.executable, os.path.join(root, "tools/claude-relay/spotify.py"), "--port", str(port + 3), "--token-file", token_file,
                       "--fixture", fixture], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
q = None
try:
    check("spotify.py starts", "listening" in sp.stdout.readline())
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
    def key(*k): cmd("send-key", keys=[{"type": "qcode", "data": x} for x in k], **{"hold-time": 50}); time.sleep(0.15)
    check("DHCP leased an address", wait_for("net dhcp", 30), uart()[-300:])
    n = len(uart()); key("f1"); wait_for("spotlight open", 10, n)
    for ch in "blackcomb": key(ch)
    key("ret")
    check("Spotlight opens Blackcomb", wait_for("browser open", 10, n), uart()[n:][-300:])
    n = len(uart())
    for ch in "spotify": key(ch)
    key("shift", "semicolon"); key("ret")
    check("spotify: goes to the Spotify service as now", wait_for("browser: spotify now", 20, n), uart()[n:][-400:])
    check("and the remote shows as a page", wait_for("browser: Spotify", 30, n) and wait_for("browser: page ", 10, n), uart()[n:][-500:])
    n = len(uart()); key("tab"); wait_for("browser: selected link 1", 10, n); key("ret")
    check("its Prev link is another spotify: command", wait_for("browser: spotify prev", 20, n), uart()[n:][-400:])
    check("which comes back as a page too", wait_for("browser: http 200", 20, n), uart()[n:][-400:])
    check("the token never appears on the UART", TOKEN not in uart())
except SystemExit:
    pass
finally:
    if q:
        q.terminate()
        try: q.wait(5)
        except subprocess.TimeoutExpired: q.kill()
    sp.terminate()
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)   # no token left in a build
if fails: print("FAIL: " + "; ".join(fails)); sys.exit(1)
print("PASS: a spotify: address in Blackcomb shows the Mac's Spotify remote as a page, its links send the next command, and the token stays off the UART")
