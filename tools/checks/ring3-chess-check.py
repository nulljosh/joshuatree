#!/usr/bin/env python3
"""Chess runs as a ring-3 program and plays a legal game: by cursor, by typed moves, against its own opponent.

Boots headless with `open=ches`. The first move is played the way a person with no mouse would: the cursor starts
on e2, space picks the pawn, Up twice and space put it on e4, and the board's pixels change. Then six more moves
are typed (each the first legal move the host build of user/chess_core.h lists after the game so far, so it is legal
whatever the computer chose). After each, the computer must answer ("chess: tree MOVE") and the whole game is
replayed through tools/checks/chess-core-check.py: every move, the computer's included, must be legal. An illegal
typed move ("a2a5") must change nothing. n starts a new game; esc closes with exit 0.

Discriminating: break move generation, the typed-move parser, the cursor or the opponent's reply in user/chess.c
or user/chess_core.h and one of these fails.

Usage: tools/checks/ring3-chess-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, re, socket, subprocess, sys, time
from PIL import Image
from freeport import free_port
from scratch import scratch_dir

TMP = scratch_dir("ring3chess")
LOG = os.path.join(TMP, "serial.log")
DUMP = os.path.join(TMP, "fb.raw")
FB = 0xfd000000; W, H = 1920, 1080
PORT = free_port()
WIN = (100, 150, 1780, 900)
CORE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "chess-core-check.py")

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-append", "open=ches",
                      "-display", "none", "-vga", "std",
                      "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = []
def serial():
    try: return open(LOG, errors="replace").read()
    except OSError: return ""
def wait_serial(needle, secs, after=0):
    for _ in range(int(secs * 10)):
        if needle in serial()[after:]: return True
        time.sleep(0.1)
    return False
def game():   # every move so far, both sides, in order
    return re.findall(r"chess: (?:you|tree) ([a-h][1-8][a-h][1-8][qrbn]?)", serial())
def replay(moves):
    r = subprocess.run([sys.executable, CORE, "replay", *moves], capture_output=True, text=True, timeout=120)
    return r.returncode, r.stdout
try:
    s = None
    for _ in range(50):
        time.sleep(0.2)
        try: s = socket.create_connection(("127.0.0.1", PORT)); break
        except OSError: pass
    if s is None: raise SystemExit("FAIL: QEMU's QMP socket never came up")
    f = s.makefile("rw")
    def cmd(o):
        f.write(json.dumps(o) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    f.readline()
    cmd({"execute": "qmp_capabilities"})
    def key(k):
        r = cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k}]}})
        if "error" in r: raise SystemExit(f"FAIL: QMP rejected send-key {k}: {r['error']}")
        time.sleep(0.15)
    def frame():
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB").crop(WIN)

    if not wait_serial("ring3app: launching CHESS.BIN at ring 3", 40):
        raise SystemExit("FAIL: Chess was never launched as a ring-3 program (open=ches flag or ring3app.c broken)")
    if not wait_serial("chess: new game", 10): fails.append("Chess never set up a game")
    time.sleep(0.5)
    before = frame()
    if os.environ.get("JT_SHOT"): before.save(os.environ["JT_SHOT"])

    # 1. by cursor: it starts on e2
    n = len(serial())
    key("spc"); key("up"); key("up"); key("spc")
    if not wait_serial("chess: you e2e4", 10, n): fails.append("space, Up, Up, space from e2 did not play e2e4")
    if not wait_serial("chess: tree ", 60, n): fails.append("the computer did not answer e2e4")
    time.sleep(0.4)
    if frame().tobytes() == before.tobytes(): fails.append("the board's pixels did not change after a move")

    # 2. an illegal typed move changes nothing
    n = len(serial())
    for k in "a2a5": key(k)
    time.sleep(1)
    if "chess: you" in serial()[n:]: fails.append("the illegal a2a5 was played")

    # 3. six typed moves, each legal by the host rules, each answered
    for turn in range(6):
        code, out = replay(game())
        if code: fails.append("the game so far is not legal: " + out.strip()); break
        if not out.startswith("status 0"): break   # the game ended: nothing more to type
        mv = out.split("\n")[1].split()[1]
        n = len(serial())
        for k in mv: key(k)
        if not wait_serial("chess: you " + mv, 10, n): fails.append(f"typing {mv} did not play it"); break
        if not wait_serial("chess: tree ", 60, n) and "chess: checkmate" not in serial()[n:] and "chess: stalemate" not in serial()[n:]:
            fails.append(f"the computer did not answer {mv}"); break
    code, out = replay(game())
    if code: fails.append("the computer played an illegal move: " + out.strip())
    moves = game()
    if len(moves) < 10: fails.append(f"only {len(moves)} moves were played: {moves}")

    # 4. n starts again, esc closes
    n = len(serial())
    key("n")
    if not wait_serial("chess: new game", 5, n): fails.append("n did not start a new game")
    key("esc")
    if not wait_serial("chess: closed", 10, n): fails.append("esc did not close Chess")
    if "chess: crashing" in serial() or "page fault" in serial()[serial().find("CHESS.BIN"):]:
        fails.append("Chess faulted")
finally:
    q.terminate()
    try: q.wait(5)
    except subprocess.TimeoutExpired: q.kill()

if fails: print("FAIL: " + "; ".join(fails)); print(serial()[-1200:]); sys.exit(1)
print("PASS: Chess ran at ring 3, played e2e4 by cursor and six more typed moves, the computer answered each, the "
      f"whole game ({len(moves)} moves) replays legally through the host rules, a2a5 was refused, n and esc work")
