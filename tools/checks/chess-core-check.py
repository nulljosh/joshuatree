#!/usr/bin/env python3
"""Chess's rules and opponent, checked on the host against the real code: user/chess_core.h is compiled with the
host's cc (CH_PERFT on) and

- perft counts every legal move tree from five standard positions (the start, "Kiwipete", and positions 3, 4 and 5
  of the Chess Programming Wiki's perft results), which catches any castling, en passant or promotion mistake;
- Fool's Mate ends in checkmate, a known stalemate is a stalemate and bare kings are a draw;
- the opponent takes a mate in one, takes a free queen, and never returns an illegal move from any of the five.

`replay MOVES...` (used by tools/checks/ring3-chess-check.py) plays coordinate moves from the start, fails on the
first illegal one, and prints the status code and the legal moves at the end.

Usage: tools/checks/chess-core-check.py
"""
import os, shutil, subprocess, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
HARNESS = r'''
#include <stdio.h>
#include <string.h>
#define CH_DATA
#define CH_PERFT
#include "chess_core.h"
static int fen(struct ch_pos *p, const char *s) {
    for (int i = 0; i < 64; i++) p->b[i] = 0;
    int r = 7, f = 0;
    for (; *s && *s != ' '; s++) {
        if (*s == '/') { r--; f = 0; continue; }
        if (*s >= '1' && *s <= '8') { f += *s - '0'; continue; }
        const char *k = strchr("pnbrqk", *s >= 'a' ? *s : *s + 32);
        if (!k) return 0;
        p->b[r * 8 + f++] = (unsigned char)((k - "pnbrqk" + 1) | (*s >= 'a' ? CH_BLACK : 0));
    }
    s++; p->side = *s == 'b'; s += 2;
    p->castle = 0;
    for (; *s && *s != ' '; s++) p->castle |= *s == 'K' ? 1 : *s == 'Q' ? 2 : *s == 'k' ? 4 : *s == 'q' ? 8 : 0;
    s++; p->ep = *s == '-' ? -1 : (signed char)((s[1] - '1') * 8 + (s[0] - 'a'));
    return 1;
}
static unsigned short find(const struct ch_pos *p, const char *mv) {
    unsigned short m[CH_MAXM]; char nm[6];
    int n = ch_legal(p, m);
    for (int i = 0; i < n; i++) { ch_name(m[i], nm); if (!strcmp(nm, mv)) return m[i]; }
    return 0;
}
int main(int argc, char **argv) {
    struct ch_pos p, q;
    if (argc > 1 && !strcmp(argv[1], "replay")) {
        ch_start(&p);
        for (int i = 2; i < argc; i++) {
            unsigned short m = find(&p, argv[i]);
            if (!m) { printf("illegal %d %s\n", i - 1, argv[i]); return 1; }
            ch_make(&p, m, &q); p = q;
        }
        printf("status %d\nmoves", ch_status(&p));
        unsigned short m[CH_MAXM]; char nm[6]; int n = ch_legal(&p, m);
        for (int i = 0; i < n; i++) { ch_name(m[i], nm); printf(" %s", nm); }
        printf("\n");
        return 0;
    }
    if (argc > 3 && !strcmp(argv[1], "perft")) {
        if (!fen(&ch_stack[0], argv[2])) return 2;
        printf("%llu\n", ch_perft(0, atoi(argv[3])));
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "status")) { fen(&p, argv[2]); printf("%d\n", ch_status(&p)); return 0; }
    if (argc > 3 && !strcmp(argv[1], "best")) {
        fen(&p, argv[2]); char nm[6]; unsigned short m = ch_best(&p, atoi(argv[3]));
        if (m && !find(&p, (ch_name(m, nm), nm))) { printf("illegal %s\n", nm); return 1; }
        ch_name(m, nm); printf("%s %u\n", m ? nm : "none", ch_nodes); return 0;
    }
    return 2;
}
'''
TMP = scratch_dir("chess-core")
src = os.path.join(TMP, "harness.c"); exe = os.path.join(TMP, "harness")
open(src, "w").write(HARNESS.replace("#include <string.h>", "#include <string.h>\n#include <stdlib.h>"))
cc = shutil.which("cc") or shutil.which("clang")
if not cc: print("SKIP: no host C compiler"); sys.exit(0)
r = subprocess.run([cc, "-O2", "-Wall", "-Werror", "-I", os.path.join(ROOT, "user"), "-o", exe, src], capture_output=True, text=True)
if r.returncode: print("FAIL: the host build of chess_core.h failed\n" + r.stderr); sys.exit(1)
def run(*a): return subprocess.run([exe, *a], capture_output=True, text=True, timeout=300)

if len(sys.argv) > 1 and sys.argv[1] == "replay":   # for ring3-chess-check.py
    out = run("replay", *sys.argv[2:]); print(out.stdout.strip()); sys.exit(out.returncode)

fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + (f" ({detail})" if detail and not ok else ""))
    if not ok: fails.append(name)

START = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"
POSITIONS = [   # FEN, depth, expected node count (chessprogramming.org/Perft_Results)
    ("start", START, 4, 197281),
    ("Kiwipete", "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 3, 97862),
    ("position 3", "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 4, 43238),
    ("position 4", "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 3, 9467),
    ("position 5", "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 3, 62379),
]
for name, f, d, want in POSITIONS:
    got = run("perft", f, str(d)).stdout.strip()
    check(f"perft {name} depth {d} is {want}", got == str(want), f"got {got}")

out = run("replay", "f2f3", "e7e5", "g2g4", "d8h4").stdout.split("\n")[0]
check("Fool's Mate is checkmate", out == "status 1", out)
check("a known stalemate is a stalemate", run("status", "7k/5Q2/6K1/8/8/8/8/8 b - - 0 1").stdout.strip() == "2")
check("bare kings are a draw", run("status", "8/8/4k3/8/8/4K3/8/8 w - - 0 1").stdout.strip() == "3")
check("the start is still playing", run("status", START).stdout.strip() == "0")
mate = run("best", "6k1/5ppp/8/8/8/8/8/R5K1 w - - 0 1", "3").stdout.split()
check("the opponent mates in one (Ra8#)", mate[:1] == ["a1a8"], " ".join(mate))
take = run("best", "4k3/8/8/3q4/8/8/8/3RK3 w - - 0 1", "3").stdout.split()
check("the opponent takes a free queen (Rxd5)", take[:1] == ["d1d5"], " ".join(take))
for name, f, _, _ in POSITIONS:
    r = run("best", f, "3")
    check(f"the opponent's choice in {name} is legal", r.returncode == 0 and not r.stdout.startswith("none"), r.stdout.strip())

if fails: print("FAIL: " + "; ".join(fails)); sys.exit(1)
print("PASS: chess_core.h counts every move from five standard perft positions exactly, knows mate, stalemate and bare "
      "kings, and its opponent mates in one, takes a free queen and only plays legal moves")
