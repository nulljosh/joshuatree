#!/usr/bin/env python3
"""ARM64 games: Spotlight opens Mines, Toroid, Hamurabi and Keyrate (the unchanged user/*.c at EL0), one real move each
changes the pixels, and each closes (headless QEMU, own private scratch). Set ARM_GAMES_SHOTS=dir to keep a screenshot of each."""
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import time
from PIL import Image
from scratch import scratch_dir

ROOT = Path(__file__).resolve().parents[2]
TMP = Path(scratch_dir("arm-games"))
KEEP = os.environ.get("ARM_GAMES_SHOTS")
WIN = (216, 70, 792, 468)   # the app window on the 800x600 ramfb desktop
subprocess.run(["make", "-s", "-C", str(ROOT / "arch/arm64"), "kernel8.elf"], check=True)
log = TMP / "uart"; sock = TMP / "qmp"
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256",
    "-nic", "none", "-global", "virtio-mmio.force-legacy=false", "-device", "ramfb",
    "-device", "virtio-keyboard-device", "-device", "virtio-tablet-device", "-display", "none",
    "-serial", "file:" + str(log), "-qmp", "unix:" + str(sock) + ",server,nowait",
    "-kernel", str(ROOT / "arch/arm64/kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
s = None; f = None
def uart(): return log.read_text(errors="replace") if log.exists() else ""
def wait(text, secs=10, after=0):
    for _ in range(int(secs * 10)):
        if text in uart()[after:]: return
        assert q.poll() is None, "QEMU exited"
        time.sleep(.1)
    raise AssertionError("timed out waiting for %r\n%s" % (text, uart()[-800:]))
try:
    wait("M2 input ready", 30)
    s = socket.socket(socket.AF_UNIX); s.settimeout(10); s.connect(str(sock)); f = s.makefile("rw"); f.readline()
    def command(name, args=None):
        f.write(json.dumps({"execute": name, **({"arguments": args} if args else {})}) + "\n"); f.flush()
        while True:
            reply = json.loads(f.readline())
            assert "error" not in reply, reply
            if "return" in reply: return reply["return"]
    def key(name, hold=50):
        command("send-key", {"keys": [{"type": "qcode", "data": name}], "hold-time": hold}); time.sleep(hold / 1000 + .15)
    def shot(name):
        path = TMP / (name + ".ppm"); command("screendump", {"filename": str(path)})
        return Image.open(path).convert("RGB")
    def keep(name, image):
        if KEEP:
            Path(KEEP).mkdir(parents=True, exist_ok=True); image.save(Path(KEEP) / (name + ".png"))
    def win(image): return image.crop(WIN).tobytes()
    def click(x, y):
        command("input-send-event", {"events": [{"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / 800)}},
                                                {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / 600)}}]})
        time.sleep(.2)
        for down in (True, False):
            command("input-send-event", {"events": [{"type": "btn", "data": {"button": "left", "down": down}}]}); time.sleep(.1)
        time.sleep(.3)
    def open_app(name, app):
        key("f1")
        for k in name: key(k)
        n = len(uart()); key("ret")
        wait("M3c app %s starts" % app, after=n); wait("M3c window %s first frame" % app, after=n); time.sleep(.5)
    def close(app):
        for _ in range(6):   # Hamurabi takes an Esc per screen on its way back to the title
            n = len(uart()); key("esc"); time.sleep(.3)
            if "M3c app %s exit 0" % app in uart()[n:]: return
        raise AssertionError(app + " did not close on Esc\n" + uart()[-600:])
    command("qmp_capabilities")

    # Spotlight finds all four by name (Hamurabi's game shows as Hamurapi, so both spellings find it)
    for typed, row in (("mine", "Mines"), ("toro", "Toroid"), ("hamurabi", "Hamurapi"), ("hamurapi", "Hamurapi"), ("keyr", "Keyrate")):
        n = len(uart()); key("f1")
        for k in typed: key(k)
        assert "spotlight: %s 1\n" % typed in uart()[n:], "Spotlight does not find " + typed + "\n" + uart()[-400:]
        key("esc")

    base = len(uart())   # the boot self-tests fault on purpose; only what the games do counts
    open_app("mines", "mines")                      # 9x9, fixed first deal
    a = shot("mines-a"); key("ret")                 # the cursor's cell opens (and the empty ones around it)
    wait("M3c event key", 5); time.sleep(.4); b = shot("mines-b")
    assert win(a) != win(b), "Mines: Enter revealed nothing"
    click(414, 362); c = shot("mines-c")            # a mouse click on a hidden cell near the bottom
    assert win(b) != win(c), "Mines: a click revealed nothing"
    keep("mines", c); close("mines")

    open_app("toroid", "toroid")                    # Game of Life on a torus
    key("spc"); time.sleep(.4); a = shot("toroid-a")
    key("s"); time.sleep(.4); b = shot("toroid-b")  # paused: s steps one generation
    assert win(a) != win(b), "Toroid: s did not step a generation"
    keep("toroid", b); close("toroid")

    open_app("hamurabi", "hamurabi")                # the title, then Classic 1968 by keyboard, then one year
    key("right"); key("right"); key("ret"); wait("phase orders year 1", 8)
    time.sleep(.5); a = shot("hamurabi-a"); n = len(uart())
    key("ret"); wait("report year 1", 8, n); time.sleep(.8); b = shot("hamurabi-b")
    assert win(a) != win(b), "Hamurabi: the year did not change the screen"
    keep("hamurabi", b); close("hamurabi")

    open_app("keyrate", "keyrate")                  # a typing test: type to begin
    a = shot("keyrate-a")
    for k in "thequickbrown": key(k, 30)
    time.sleep(.4); b = shot("keyrate-b")
    assert win(a) != win(b), "Keyrate: typing changed nothing"
    keep("keyrate", b); close("keyrate")
    assert "EL0 fault" not in uart()[base:], "an app faulted\n" + uart()[-600:]
    print("PASS: Spotlight opens Mines, Toroid, Hamurabi and Keyrate at EL0; a reveal, a generation, a year and typing each change the pixels and each closes on Esc", flush=True)
finally:
    if f: f.close()
    if s: s.close()
    if q.poll() is None: q.terminate()
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill(); q.wait()
    q.stderr.close()
