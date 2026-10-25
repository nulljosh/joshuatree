#!/usr/bin/env python3
"""Solitaire (Klondike, draw one) runs as a ring-3 program and plays by the rules.

Boots headless with `open=soli`. user/solitaire.c deals its first game from a
fixed xorshift seed, so this check deals the same cards with the same
generator and keeps its own copy of the game. It plays a greedy game by
keyboard (a sends cards up, then a column move that turns a card over, then
the waste onto a column, else draw) and every "solitaire: move" line the app
logs must be the move this model made, in order. Cards are counted after
every move: "solitaire: bad state" fails it. Esc must close it with exit 0.

Discriminating: change the seed, the shuffle, the colour rule, the rank rule
or which run a column move takes in user/solitaire.c and the moves differ.

Usage: tools/checks/ring3-solitaire-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image
from freeport import free_port
from scratch import scratch_dir

TMP = scratch_dir("ring3solitaire")
LOG = os.path.join(TMP, "serial.log")
DUMP = os.path.join(TMP, "fb.raw")
FB = 0xfd000000; W, H = 1920, 1080
PORT = free_port()
SEED = 0x2545F491
UP = 0x80
TABLE = (0xE9, 0xE3, 0xD9)
STEPS = 160

def rank(c): return (c & 0x7F) % 13 + 1
def suit(c): return (c & 0x7F) // 13
def red(c): return suit(c) in (1, 2)
def name(c): return "A23456789TJQK"[rank(c) - 1] + "SHDC"[suit(c)]

class Game:
    """The same rules as user/solitaire.c, pile for pile."""
    def __init__(self):
        s = SEED; m = 0xFFFFFFFF
        def rnd():
            nonlocal s
            s ^= (s << 13) & m; s ^= s >> 17; s ^= (s << 5) & m
            return s
        deck = list(range(52))
        for i in range(51, 0, -1):
            j = rnd() % (i + 1); deck[i], deck[j] = deck[j], deck[i]
        self.stock, self.waste = [], []
        self.found = [[], [], [], []]
        self.tab = [[] for _ in range(7)]
        k = 0
        for col in range(7):
            for row in range(col + 1):
                self.tab[col].append(deck[k] | (UP if row == col else 0)); k += 1
        self.stock = deck[k:]
        self.moves = []
    def tops(self): return "solitaire: tops " + " ".join(name(t[-1]) for t in self.tab)
    def draw(self):
        if self.stock: self.waste.append(self.stock.pop() | UP)
        elif self.waste:
            while self.waste: self.stock.append(self.waste.pop() & 0x7F)
    def fits_found(self, c): return bool(c & UP) and rank(c) == len(self.found[suit(c)]) + 1
    def fits_col(self, c, col):
        t = self.tab[col]
        if not t: return rank(c) == 13
        return bool(t[-1] & UP) and red(t[-1]) != red(c) and rank(t[-1]) == rank(c) + 1
    def src_pile(self, src): return self.waste if src == "w" else self.tab[int(src) - 1]
    def run_start(self, src, dst):
        """The index the app would move from src onto column dst, or None."""
        p = self.src_pile(src)
        if not p or src == str(dst + 1): return None
        if src == "w": return len(p) - 1 if self.fits_col(p[-1], dst) else None
        i = 0
        while i < len(p) and not (p[i] & UP): i += 1
        while i < len(p) and not self.fits_col(p[i], dst): i += 1
        return i if i < len(p) else None
    def move(self, src, dst):   # dst "f" or a column 0..6
        p = self.src_pile(src)
        if dst == "f":
            if not p or not self.fits_found(p[-1]): return False
            c = p.pop(); self.found[suit(c)].append(c); d = "f"
        else:
            i = self.run_start(src, dst)
            if i is None: return False
            c = p[i]; self.tab[dst] += p[i:]; del p[i:]; d = str(dst + 1)
        if src != "w" and p and not (p[-1] & UP): p[-1] |= UP
        self.moves.append(f"solitaire: move {src} {d} {name(c)}")
        return True
    def auto(self):
        again = True
        while again:
            again = self.move("w", "f")
            for col in range(7):
                if self.move(str(col + 1), "f"): again = True
    def reveal_move(self):
        """A column move that turns a face-down card over, as (src, dst)."""
        for s in range(7):
            p = self.tab[s]
            first = next((i for i, c in enumerate(p) if c & UP), None)
            if first is None or first == 0: continue
            for d in range(7):
                if d != s and self.run_start(str(s + 1), d) == first: return str(s + 1), d
        return None
    def won(self): return sum(len(f) for f in self.found) == 52

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-append", "open=soli",
                      "-display", "none", "-vga", "std",
                      "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = []
def serial():
    try: return open(LOG, errors="replace").read()
    except OSError: return ""
def wait_serial(needle, secs):
    for _ in range(int(secs * 10)):
        if needle in serial(): return True
        time.sleep(0.1)
    return False
def app_moves(): return ["solitaire: move " + l.split("solitaire: move ", 1)[1].strip() for l in serial().splitlines() if "solitaire: move " in l]
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
        time.sleep(0.06)
    def frame():
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")

    # 1. the program is up, has its window, and dealt the same cards as the model
    g = Game()
    if not wait_serial("ring3app: launching SOLITAIRE.BIN at ring 3", 40):
        fails.append("Solitaire was never launched as a ring-3 program (open=soli flag or ring3app.c broken)")
    if not wait_serial("solitaire: ring-3 window ", 10):
        fails.append("the program did not report its window through write()")
    if not wait_serial(g.tops(), 5):
        fails.append(f"the deal differs: expected '{g.tops()}'")
    time.sleep(0.5)
    img = frame()
    if os.environ.get("JT_SHOT"): img.save(os.environ["JT_SHOT"])
    table = sum(1 for x in range(0, W, 8) for y in range(0, H, 8) if img.getpixel((x, y)) == TABLE)
    if table < 2000:
        fails.append(f"the window does not show the card table ({table} table samples)")

    # 2. play the same greedy game on both sides
    for step in range(STEPS):
        if g.won(): break
        before = len(g.moves)
        g.auto(); key("a")
        if len(g.moves) > before: continue
        rm = g.reveal_move()
        if rm:
            g.move(*rm); key(rm[0]); key(str(rm[1] + 1)); continue
        wm = next((d for d in range(7) if g.run_start("w", d) is not None), None)
        if wm is not None:
            g.move("w", wm); key("w"); key(str(wm + 1)); continue
        g.draw(); key("d")
    for _ in range(50):
        if len(app_moves()) >= len(g.moves): break
        time.sleep(0.1)
    got = app_moves()
    print(f"model made {len(g.moves)} moves, the app logged {len(got)}, {sum(len(x) for x in g.found)} cards up")
    if len(g.moves) < 5:
        fails.append("the scripted game made fewer than 5 moves, so it proves little: pick another seed")
    if got != g.moves:
        diff = next((i for i in range(min(len(got), len(g.moves))) if got[i] != g.moves[i]), min(len(got), len(g.moves)))
        fails.append(f"move {diff} differs: app {got[diff:diff + 2]} vs model {g.moves[diff:diff + 2]}")
    if "solitaire: bad state" in serial():
        fails.append("the app lost or doubled a card")
    if g.won() and not wait_serial("solitaire: won", 3):
        fails.append("the model won but the app never said so")

    # 3. Esc closes it cleanly
    keys_before = serial().count("SOLITAIRE.BIN exited 0")
    key("esc")
    if not wait_serial("solitaire: closed", 5):
        fails.append("Esc did not reach the program (no closed line)")
    for _ in range(50):
        if serial().count("SOLITAIRE.BIN exited 0") > keys_before: break
        time.sleep(0.1)
    if serial().count("SOLITAIRE.BIN exited 0") <= keys_before:
        fails.append("Solitaire did not exit 0 on Esc")
    if "exception: ring-0" in serial() or "panic in" in serial() or "ring3app: BUG" in serial():
        fails.append("the kernel faulted or ring3app logged a BUG line")
finally:
    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError, ValueError, NameError): pass
    q.terminate()
    try: q.wait(5)
    except subprocess.TimeoutExpired: q.kill()

if fails:
    print("FAIL:")
    for x in fails: print("  - " + x)
    print("--- serial tail ---")
    print(serial()[-1500:])
    sys.exit(1)
print("PASS: Solitaire ran at ring 3, dealt the expected cards, every move matched the rules model, and it closed on Esc")
