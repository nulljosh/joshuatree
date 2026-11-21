#!/usr/bin/env python3
"""Recapture every app tile in landing/shots/ the same way, so they match:
same boot, same dock launch, same window crop, same output size.

Each tile is gui_launch_from_dock's window rect (x=70,y=40,w=820,h=385 in
the 960x540 logical canvas, 1640x770 physical at 2x), scaled by exactly
0.6 to 984x462. Same ratio in and out, so nothing is stretched. The old
tiles were saved at 960x553, which squashed every one of them, and two
(Weather, Stocks) were never cropped at all. The Samantha tile is her full-screen view, not
a window, and is cropped lower (y=90): since 2.12.1 her mouth sits lower in the picture, and
y=40 cut through her lower lip. y=90 keeps both lips and her chin and stops above the input panel.

Headless only: QEMU -display none, QMP input, pmemsave of the framebuffer.

Each retake also records a hash of the sources that draw the tile in landing/shots/sources.json
(tools/landing_shots_manifest.py). tools/checks/landing-shots-fresh-check.py fails when an app
changed and its tile was not retaken. `--record` writes the hashes without retaking (use it
only when a source change cannot alter the picture, such as a comment).

Fleet apps (bookrank, tonchi, curbfind, epiphany) boot with no network, so they show their
offline samples and never change with the live data.

--clips also films each tile (13.19.3): the same crop, recorded from the window opening through a short
scripted use (PLAY below), to landing/clips/<tile>.mp4 at 15 fps, muted. The landing's wall plays them
with the still as the poster, so the stills stay for the README, reduced motion and before a clip loads.

Usage: python3 tools/landing-shots.py [--record] [--clips] [name ...]   (from anywhere; default: all)
"""
import json, os, socket, subprocess, sys, threading, time
from PIL import Image

REPO = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
os.chdir(REPO)
LOG = "/tmp/jt-landingshots-serial.log"; DUMP = "/tmp/jt-landingshots.raw"
FB = 0xfd000000; W, H = 1920, 1080; PORT = 4458
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
WIN = (70, 40, 820, 385)            # logical x, y, w, h
WIN_OF = {}   # per-tile crop overrides (none now)
OUT = (984, 462)                    # 820x385 * 0.6, same ratio
CLIPS = "--clips" in sys.argv
# What each clip does once its window is up: (keys, seconds after each). Apps with none just open and sit.
PLAY = {"paddle": [("spc", 0.5)] + [("up", 0.35)] * 6 + [("down", 0.35)] * 6,
        "bricks": [("spc", 0.6)] + [("left", 0.3)] * 5 + [("right", 0.3)] * 8,
        "snake": [("right", 0.9), ("up", 0.2), ("left", 0.9), ("down", 0.2)] * 3 + [("right", 0.1)],   # ends heading across the open board   # a small loop: it moves 12 cells a second
        "calendar": [("right", 0.8)] * 4 + [("left", 0.8)] * 2,
        "depths": [("up", 0.5)] * 4 + [("left", 0.6), ("up", 0.5), ("up", 0.5), ("spc", 0.8)],
        "chess": [("spc", 0.3), ("up", 0.3), ("up", 0.3), ("spc", 3.0)],
        "solitaire": [("d", 0.8)] * 4,
        "bookrank": [("down", 0.7)] * 4, "curbfind": [("down", 0.7)] * 4, "epiphany": [("down", 0.7)] * 3,
        "stocks": [("down", 0.8)] * 3, "tonchi": [("1", 1.5), ("2", 1.5)], "hamurabi": [("ret", 1.2)] * 2,
        "files": [("1", 1.0), ("2", 1.0)]}
DESKTOP_PX, DESKTOP_RGB = (480, 511), (0xEF, 0xEB, 0xE4)

# kernel.c GUI_DOCK_DEFAULT: Apps,Files,Mail,Calendar,Notes,Reminders,Terminal,Chat,Weather,Stocks,Trash
SLOT0_X, PITCH, ICON, DOCK_Y = 226, 43, 37, 487
SLOT = {"files": 1, "calendar": 3, "notes": 4, "terminal": 6, "weather": 8, "stocks": 9}
# Fleet apps live in the Apps folder, not the dock. They boot with `open=<app>` (the flag the ring3 checks
# use) and no NIC at all, so each one draws its built-in offline samples and no tile depends on the network.
FLEET = {"bookrank": ("bookr", "bookrank: ring-3 window", "bookrank: samples"),
         "tonchi": ("tonchi", "tonchi: ring-3 window", "tonchi: samples"),
         "curbfind": ("curb", "curbfind: ring-3 window", "curbfind: samples"),
         "epiphany": ("epip", "epiphany: ring-3 window", "epiphany: fetch"),   # (open= flag, window opened, offline fallback chosen)
         "hamurabi": ("hamu", "hamurabi: ring-3 window", "hamurabi: phase title"),   # Hamurapi: its title scene, drawn from first frame, no network
         "windgate": ("windgate", "windgate: ring-3 window", "windgate: clock ready"),
         "panes": ("pane", "panes: ring-3 window", "panes: tabs a=1 b=1"),
         "solitaire": ("soli", "solitaire: ring-3 window", "solitaire: tops"),   # the games (4.23.0): each draws its first frame with no network
         "paddle": ("padd", "paddle: ring-3 window", "paddle: ring-3 window"),
         "bricks": ("bric", "bricks: ring-3 window", "bricks: ring-3 window"),
         "depths": ("dept", "depths: ring-3 window", "depths: ready"),
         "snake": ("snak", "snake: ring-3 window", "snake: first apple"),
         "chess": ("ches", "chess: ring-3 window", "chess: new game")}   # Windgate: offline by design; Space starts it and the shot lands mid in-breath
FILE = {"bookrank": "app-bookrank", "tonchi": "app-tonchi", "curbfind": "app-curbfind", "epiphany": "app-epiphany",
        "hamurabi": "app-hamurapi", "windgate": "app-windgate", "panes": "app-panes", "notes": "app-notes", "calendar": "app-calendar", "weather": "app-weather",
        "files": "app-files", "terminal": "app-terminal", "stocks": "app-stocks", "desktop-apps": "desktop-apps",
        "solitaire": "app-solitaire", "paddle": "app-paddle", "bricks": "app-bricks", "depths": "app-depths", "snake": "app-snake", "chess": "app-chess"}


def shoot(name):
    for f in (LOG, DUMP):
        try: os.remove(f)
        except FileNotFoundError: pass
    fleet = name in FLEET
    net = [] if fleet else ["-net", "nic,model=rtl8139", "-net", "user"]
    boot = ["-append", "open=" + FLEET[name][0]] if fleet else []
    q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                          "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG] + net + boot + [
                          "-rtc", "base=2026-10-03T12:00:00,clock=vm"],   # pinned clock: the Calendar tile is the same on every run
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        s = None
        for _ in range(100):
            time.sleep(0.2)
            try: s = socket.create_connection(("127.0.0.1", PORT)); break
            except OSError: pass
        if s is None: sys.exit(f"FAIL {name}: QMP never came up")
        f = s.makefile("rw"); lock = threading.RLock()   # the clip recorder grabs frames from its own thread
        def cmd(o):
          with lock:
            f.write(json.dumps(o) + "\n"); f.flush()
            while True:
                r = json.loads(f.readline())
                if "return" in r or "error" in r: return r
        f.readline(); cmd({"execute": "qmp_capabilities"})

        def move(x, y):
            cmd({"execute": "input-send-event", "arguments": {"events": [
                {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
                {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
        def click(x, y):
            move(x, y); time.sleep(0.3)
            for down in (True, False):
                cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": down, "button": "left"}}]}})
                time.sleep(0.1)
        def key(k):
            cmd({"execute": "human-monitor-command", "arguments": {"command-line": f"sendkey {k} 30"}})
            time.sleep(0.12)
        def type_text(t):
            for ch in t:
                key("shift-" + ch.lower() if ch.isupper() else {" ": "spc", ".": "dot", ",": "comma"}.get(ch, ch))
        def frame():
          with lock:
            cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
            return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
        def serial():
            try: return open(LOG, errors="replace").read()
            except FileNotFoundError: return ""

        rec = None
        def start_clip():
            nonlocal rec
            if not CLIPS or name == "desktop-apps": return
            os.makedirs("landing/clips", exist_ok=True)
            move(LOGICAL_W - 2, 2); time.sleep(0.3)   # the pointer would sit in the film
            enc = subprocess.Popen(["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24",
                                    "-s", f"{OUT[0]}x{OUT[1]}", "-use_wallclock_as_timestamps", "1", "-i", "-",
                                    "-vf", "fps=15", "-c:v", "libx264", "-preset", "slow", "-crf", "30", "-pix_fmt", "yuv420p",
                                    "-movflags", "+faststart", "-an", f"landing/clips/{FILE[name]}.mp4"], stdin=subprocess.PIPE)
            stop = threading.Event(); x, y, w, h = WIN_OF.get(name, WIN)
            def grab():
                while not stop.is_set():
                    enc.stdin.write(frame().crop((x * SCALE, y * SCALE, (x + w) * SCALE, (y + h) * SCALE)).resize(OUT, Image.BILINEAR).tobytes())
            t = threading.Thread(target=grab); t.start(); rec = (enc, stop, t)
        def stop_clip():
            enc, stop, t = rec; stop.set(); t.join(); enc.stdin.close()
            if enc.wait() != 0: sys.exit(f"FAIL {name}: ffmpeg could not write the clip")
            print(f"filmed landing/clips/{FILE[name]}.mp4 ({os.path.getsize(f'landing/clips/{FILE[name]}.mp4')} bytes)")

        for _ in range(160):
            if frame().getpixel((DESKTOP_PX[0] * SCALE + 1, DESKTOP_PX[1] * SCALE + 1)) == DESKTOP_RGB: break
            time.sleep(0.25)
        else:
            sys.exit(f"FAIL {name}: desktop never appeared")
        time.sleep(4.0)

        if fleet:
            for needle in FLEET[name][1:]:     # window first, then the offline fallback: a frame before it is mid-draw
                for _ in range(120):
                    if needle in serial(): break
                    time.sleep(0.25)
                else:
                    sys.exit(f"FAIL {name}: no '{needle}' on serial")
            time.sleep(2.0)                    # let the frame after the fallback land
            start_clip()
            if name == "panes":
                type_text("ls"); key("ret"); time.sleep(0.6)
                key("ctrl-t"); time.sleep(0.6)
                type_text("sleep 2 build done"); key("ret"); time.sleep(0.6)
                key("ctrl-1")                  # the second tab keeps running; its dot shows once the timer ends
                for _ in range(120):
                    if "panes: job done" in serial(): break
                    time.sleep(0.25)
                time.sleep(1.0)
            if name == "windgate":
                key("spc"); time.sleep(2.0)    # start the breath: the tile shows the circle growing
        elif name == "desktop-apps":           # the hero (README and landing): the desktop at work, two apps open
            for app in ("terminal", "weather"):   # two windows: the most i386 keeps open at once
                click(SLOT0_X + SLOT[app] * PITCH + ICON // 2, DOCK_Y); time.sleep(2.5)
                move(700, 250); time.sleep(0.5)   # off the dock before the next window: no hover label under it
                if app == "terminal":
                    for line in ("uptime", "mem", "ls"): type_text(line); key("ret"); time.sleep(0.6)
            for _ in range(8):
                if "wxstate=ok" in serial(): break
                time.sleep(4); key("r")
        else:
            start_clip()                       # from before the click, so the clip shows the window opening
            click(SLOT0_X + SLOT[name] * PITCH + ICON // 2, DOCK_Y)
            if CLIPS: move(LOGICAL_W - 2, 2)   # the pointer would sit in the film
            time.sleep(1.5)

        if name == "notes":
            key("n")                          # folder view -> a fresh note, before typing
            for _ in range(40):
                if "notes: edit=" in serial(): break
                time.sleep(0.25)
            else:
                sys.exit("FAIL notes: new note never opened")
            time.sleep(1.0)
            for line in ("Groceries for Sunday", "Oat milk, eggs, coffee beans, lemons", "Call the garage about the door"):
                type_text(line); key("ret")
        elif name == "files":
            key("2")                           # Icons view
        elif name == "terminal":
            for line in ("uptime", "mem", "ls"): type_text(line); key("ret"); time.sleep(0.6)
        elif name == "weather":
            # Real fetch through QEMU's NAT; the free forecast API sometimes 503s, so retry until serial says ok.
            for _ in range(8):
                if "wxstate=ok" in serial(): break
                time.sleep(4); key("r")
            else:
                sys.exit("FAIL weather: never got a live reading (wxstate=ok)")
        elif name == "stocks":
            time.sleep(4)
        if name == "desktop-apps": move(40, 470)   # on the wallpaper, left of the dock and below the windows   # the whole screen is the shot: the pointer sits on the wallpaper, whole
        else: move(LOGICAL_W - 2, 2)           # park the pointer outside the window crop
        time.sleep(1.0)
        if rec:
            for k, wait in PLAY.get(name, []): key(k); time.sleep(wait)
            time.sleep(1.5)
            stop_clip()

        if name == "desktop-apps":            # the whole screen, half size: menu bar, wallpaper, dock
            img = frame().resize((W // 2, H // 2), Image.LANCZOS)
        else:
            x, y, w, h = WIN_OF.get(name, WIN)
            img = frame().crop((x * SCALE, y * SCALE, (x + w) * SCALE, (y + h) * SCALE)).resize(OUT, Image.LANCZOS)
        out = f"landing/shots/{FILE[name]}.webp"
        for qual in (85, 80, 70, 60):
            img.save(out, "WEBP", quality=qual, method=6)
            if os.path.getsize(out) < 120 * 1024: break
        sys.path.insert(0, os.path.join(REPO, "tools")); import landing_shots_manifest as M
        M.record(FILE[name])
        print(f"saved {out} {img.size[0]}x{img.size[1]} ({os.path.getsize(out)} bytes, q={qual})")
    finally:
        q.terminate()
        try: q.wait(timeout=5)
        except Exception: q.kill()


if __name__ == "__main__":
    if "--record" in sys.argv:
        sys.path.insert(0, os.path.join(REPO, "tools")); import landing_shots_manifest as M
        for t in list(M.PROGRAM) + list(M.KERNEL): M.record(t)
        sys.exit(0)
    subprocess.run(["make", "-s", "kernel.elf"], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for n in ([a for a in sys.argv[1:] if not a.startswith("--")] or ["desktop-apps"] + list(SLOT) + list(FLEET)):
        shoot(n)
