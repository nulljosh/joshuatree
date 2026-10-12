#!/usr/bin/env python3
"""ARM64 volume (arch/arm64/audio.c master volume, `vol` and volume_key in cmd.c, ui_key in main.c, hid_media in xhci.c).

Headless QEMU virt, keys through QMP send-key like arm64-keys-check.py. virt has no PWM, so the proof is the ring: every
`audio:` line carries the sum of |sample| over what was pushed, after the volume scaled it.
  1. On the bare desktop F12 raises the volume from the default 70 to 80, F11 lowers it: each prints `notice: Volume N` (the
     menu bar's own notice line) and plays the tick (`audio: tone 1000 Hz 40 ms`).
  2. In the Terminal, `beep 440 100` at `vol 100`, `vol 50` and `vol 0`: the ring sums equal a tone computed here in Python,
     `vol 50` is half of `vol 100` within 1 percent, `vol 0` is a silent ring (abs 0).
  3. `vol`, `vol up`, `vol 150` (refused, level kept). F12 with the Terminal in front, with Spotlight open (it stays open),
     and at 100 (stays 100, never past it); F11 at 0 stays 0. The mute key toggles 0 and the last level.
  4. The Mac media keys: a made-up consumer report for volume down (0xEA), up (0xE9) and mute (0xE2) through hid_media in
     the keydbg build steps the level the same way.
Skips (exit 0) when the tools are missing. The jack itself is NOT proved here (the real board).
"""
import json, os, re, shutil, socket, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch", "arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
if subprocess.run(["make", "-C", arch, "kernel8.elf", "keydbgtest"], capture_output=True, text=True, timeout=900).returncode:
    print("FAIL: make -C arch/arm64 kernel8.elf keydbgtest failed"); sys.exit(1)
fails = []


def step(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok or not detail else " -- " + detail))
    if not ok: fails.append(name)


def tone_abs(freq, ms, vol, rate=44100, amp=6000):
    """Sum of |sample| over both channels of the beep tone at a volume, computed independently of audio.c."""
    step_, phase, total = (freq << 32) // rate, 0, 0
    for _ in range(rate * ms // 1000):
        a = (abs((phase >> 18) - 8192) - 4096) * amp
        s = a // 4096 if a >= 0 else -((-a) // 4096)
        total += 2 * (abs(s) * vol // 100)
        phase = (phase + step_) & 0xFFFFFFFF
    return total


def boot(elf, body, keyboard=True):
    tmp = scratch_dir("volume")
    log, sock = os.path.join(tmp, "uart"), os.path.join(tmp, "qmp")
    q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-nic", "none",
                          "-global", "virtio-mmio.force-legacy=false", "-device", "ramfb", "-device", "virtio-keyboard-device",
                          "-display", "none", "-serial", "file:" + log, "-qmp", "unix:%s,server,nowait" % sock,
                          "-kernel", os.path.join(arch, elf)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def uart(): return open(log, errors="replace").read() if os.path.exists(log) else ""

    def wait_for(text, tries=100, start=0):
        for _ in range(tries):
            if text in uart()[start:]: return True
            time.sleep(0.1)
        return False
    try:
        if not wait_for("M1c fb ok", 600): raise SystemExit("FAIL: the desktop did not come up: %r" % uart()[-600:])
        time.sleep(1.0)
        s_ = socket.socket(socket.AF_UNIX); s_.settimeout(30); s_.connect(sock); f = s_.makefile("rw"); f.readline()

        def cmd(c, **a):
            f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
            while True:
                r = json.loads(f.readline())
                if "return" in r or "error" in r: return r

        cmd("qmp_capabilities")

        def key(*qcodes): cmd("send-key", keys=[{"type": "qcode", "data": k} for k in qcodes]); time.sleep(0.15)

        def after(action, text, name, tries=60):   # do something, then wait for text in what the UART printed since
            n = len(uart()); action(); ok = wait_for(text, tries, n)
            step(name, ok, uart()[n:][-300:]); return uart()[n:] if ok else ""

        def typed(line, text, name, tries=60):
            codes = {" ": "spc"}
            return after(lambda: key(*[codes.get(c, c) for c in line], "ret"), text, name, tries)
        body(uart, wait_for, key, after, typed)
    finally:
        q.kill(); q.wait()


def main_body(uart, wait_for, key, after, typed):
    out = after(lambda: key("f12"), "notice: Volume 80", "F12 on the bare desktop: 70 to 80, one notice line")
    step("the key plays the tick through beep", "audio: tone 1000 Hz 40 ms" in out, out[-300:])
    after(lambda: key("f11"), "notice: Volume 70", "F11 steps back down to 70")
    after(lambda: key("f11"), "notice: Volume 60", "F11 again: 60")
    after(lambda: key("f2"), "terminal open", "F2 opens the Terminal")
    typed("vol", "volume 60", "`vol` prints the level the keys left")
    typed("vol 100", "volume 100", "`vol 100` sets full")
    sums = {}
    for v in (100, 50, 0):
        typed("vol %d" % v, "volume %d" % v, "`vol %d`" % v)
        out = typed("beep 440 100", "audio: 4410 frames", "`beep 440 100` at volume %d fills the ring" % v)
        m = re.search(r"ring sum 0x[0-9a-f]{8}, abs (\d+), volume (\d+)", out)
        sums[v] = int(m.group(1)) if m else -1
        step("the ring says volume %d and its |sample| sum equals the Python tone (%s)" % (v, tone_abs(440, 100, v)),
             bool(m) and int(m.group(2)) == v and sums[v] == tone_abs(440, 100, v), out[-300:])
    step("vol 50 halves the ring sum of vol 100 within 1 percent (%d against %d)" % (sums[50], sums[100]),
         sums[100] > 0 and abs(2 * sums[50] - sums[100]) * 100 <= sums[100], str(sums))
    step("vol 0 is a silent ring (abs 0)", sums[0] == 0, str(sums))
    typed("vol up", "volume 10", "`vol up` from 0 steps by 10")
    typed("vol 150", "vol: use vol", "`vol 150` is refused")
    typed("vol", "volume 10", "and the level is still 10")
    after(lambda: key("f12"), "notice: Volume 20", "F12 with the Terminal in front still reaches volume")
    after(lambda: key("f1"), "spotlight open", "F1 opens Spotlight")
    n = len(uart())
    after(lambda: key("f12"), "notice: Volume 30", "F12 with Spotlight open steps volume")
    step("and Spotlight stays open", "spotlight close" not in uart()[n:])
    after(lambda: key("esc"), "spotlight close", "Esc closes Spotlight")
    typed("vol 100", "volume 100", "`vol 100`")
    after(lambda: key("f12"), "notice: Volume 100", "F12 at 100 stays 100")
    after(lambda: key("audiomute"), "notice: Volume 0", "the mute key mutes")
    after(lambda: key("audiomute"), "notice: Volume 100", "and again returns to the last level")
    typed("vol 0", "volume 0", "`vol 0`")
    after(lambda: key("f11"), "notice: Volume 0", "F11 at 0 stays 0")
    after(lambda: key("volumeup"), "notice: Volume 10", "the Volume Up key (evdev 115) steps up")
    after(lambda: key("volumedown"), "notice: Volume 0", "the Volume Down key (evdev 114) steps down")


def media_body(uart, wait_for, key, after, typed):
    wait_for("keytest: done", 100); out = uart()

    def between(a, b): i = out.find(a); j = out.find(b, i); return out[i:j] if i >= 0 and j >= 0 else ""
    step("a consumer report 0xEA (Mac volume down) steps 70 to 60", "notice: Volume 60" in between("keytest: media vol down", "keytest: media vol up"), out[-300:])
    step("0xE9 (volume up) steps back to 70", "notice: Volume 70" in between("keytest: media vol up", "keytest: media mute"))
    step("0xE2 (mute) goes to 0", "notice: Volume 0" in between("keytest: media mute", "keytest: media unmute"))
    step("0xE2 again returns to 70", "notice: Volume 70" in between("keytest: media unmute", "keytest: done"))


boot("kernel8.elf", main_body)
boot("keydbg-kernel8.elf", media_body)

for m in fails: print("FAIL: " + m)
if fails: sys.exit(1)
print("PASS: volume 0 to 100 scales the ring (vol 50 is half of vol 100, vol 0 silent), F11/F12 and the media keys step it before any pane with a menu bar notice and a tick, and it never passes 100 or goes below 0 (the real jack is unverified)")
