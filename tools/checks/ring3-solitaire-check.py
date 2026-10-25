#!/usr/bin/env python3
"""Solitaire (Klondike, draw one) runs as a ring-3 program and the fixed first deal can be won.

Boots headless with `open=soli`. user/solitaire.c deals its first game from a
fixed xorshift seed. This check deals the same cards, searches for a win with
a depth-first solver over the app's own rules (only the moves the app allows:
waste or column up to the foundations, waste or a column's run onto a column,
draw and turn over), then plays that win by keyboard. Every "solitaire: move"
line the app logs must be the move the model made, in order, the app must say
"solitaire: won", and cards are counted after every move ("solitaire: bad
state" fails it). Esc must close it with exit 0.

Discriminating: change the seed, the shuffle, the colour or rank rule, which
run a column move takes, or the win test in user/solitaire.c and the moves
differ or the win never comes.

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

# The search: states are (stock, waste, foundation heights, columns) as tuples, moves in the app's own terms.
def start_state(g): return (tuple(g.stock), (), (0, 0, 0, 0), tuple(tuple(t) for t in g.tab))
def fits_col_t(c,t):
    if not t: return rank(c)==13
    return bool(t[-1]&UP) and red(t[-1])!=red(c) and rank(t[-1])==rank(c)+1
def moves(st):
    stock,waste,found,tab=st
    out=[]
    def flip(t):
        t=list(t)
        if t and not (t[-1]&UP): t[-1]|=UP
        return tuple(t)
    # to foundation
    if waste:
        c=waste[-1]
        if rank(c)==found[suit(c)]+1:
            f=list(found); f[suit(c)]+=1
            out.append((('w','f'),(stock,waste[:-1],tuple(f),tab),0))
    for i,t in enumerate(tab):
        if t and (t[-1]&UP) and rank(t[-1])==found[suit(t[-1])]+1:
            f=list(found); f[suit(t[-1])]+=1; nt=list(tab); nt[i]=flip(t[:-1])
            out.append(((str(i+1),'f'),(stock,waste,tuple(f),tuple(nt)),0))
    # tab to tab (app semantics: first face-up index that fits)
    for s,t in enumerate(tab):
        first=next((i for i,c in enumerate(t) if c&UP),None)
        if first is None: continue
        for d in range(7):
            if d==s: continue
            i=first
            while i<len(t) and not fits_col_t(t[i],tab[d]): i+=1
            if i>=len(t): continue
            if i==0 and not tab[d] : continue  # king from bare column to empty: pointless
            nt=list(tab); nt[d]=tab[d]+t[i:]; nt[s]=flip(t[:i])
            pri=1 if (i>0 and not (t[i-1]&UP)) else 3
            out.append(((str(s+1),str(d)),(stock,waste,found,tuple(nt)),pri))
    if waste:
        c=waste[-1]
        for d in range(7):
            if fits_col_t(c,tab[d]):
                nt=list(tab); nt[d]=tab[d]+(c,)
                out.append((('w',str(d)),(stock,waste[:-1],found,tuple(nt)),2))
    if stock: out.append((('d',),(stock[:-1],waste+(stock[-1]|UP,),found,tab),4))
    elif waste: out.append((('d',),(tuple(c&0x7F for c in reversed(waste)),(),found,tab),5))
    out.sort(key=lambda x:x[2])
    return out
def solve(st, limit=400000):
    seen=set(); path=[]; n=[0]
    sys.setrecursionlimit(10000)
    def key(s): return s
    def dfs(s):
        if sum(s[2])==52: return True
        n[0]+=1
        if n[0]>limit: return False
        if s in seen: return False
        seen.add(s)
        for mv,ns,_ in moves(s):
            path.append(mv)
            if dfs(ns): return True
            path.pop()
        return False
    ok=dfs(st)
    return ok, path, n[0]

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

    # 2. find a win for this deal and play it on both sides
    ok, plan, tried = solve(start_state(g))
    print(f"solver: {'a win in ' + str(len(plan)) + ' moves' if ok else 'no win found'} after {tried} states")
    if not ok:
        fails.append("the solver found no win for the first deal: pick a winnable seed")
    for mv in plan if ok else []:
        if mv == ("d",):
            g.draw(); key("d"); continue
        src, dst = mv
        g.move(src, "f" if dst == "f" else int(dst))
        key(src); key("f" if dst == "f" else str(int(dst) + 1))
    for _ in range(80):
        if len(app_moves()) >= len(g.moves): break
        time.sleep(0.1)
    got = app_moves()
    print(f"model made {len(g.moves)} moves, the app logged {len(got)}, {sum(len(x) for x in g.found)} cards up")
    if got != g.moves:
        diff = next((i for i in range(min(len(got), len(g.moves))) if got[i] != g.moves[i]), min(len(got), len(g.moves)))
        fails.append(f"move {diff} differs: app {got[diff:diff + 2]} vs model {g.moves[diff:diff + 2]}")
    if "solitaire: bad state" in serial():
        fails.append("the app lost or doubled a card")
    if ok and not g.won():
        fails.append("the model did not end won after the plan: the solver and the model disagree")
    if ok and not wait_serial("solitaire: won", 5):
        fails.append("the plan won the game but the app never said so")

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
print("PASS: Solitaire ran at ring 3, dealt the expected cards, a full win was played move for move against the rules model, and it closed on Esc")
