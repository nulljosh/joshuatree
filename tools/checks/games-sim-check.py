#!/usr/bin/env python3
"""Paddle and Bricks can be won, and neither can get stuck: their physics played to the end on the host.

A move-by-move copy of step() from user/paddle.c and user/bricks.c (same integers, same truncating division),
played by a scripted player at key speed (Paddle: 22 px a press every 8 ticks; Bricks: 28 px a press every 6
ticks, aiming the hit toward the lowest standing brick). Two QA findings are fixed in the C and read back from it
here, so taking a fix out of the C makes this fail:

- Paddle's computer reacts only once the ball is in its half. Without that, a player who hits the ball square
  rallied with the computer forever, nobody scoring in 33 minutes of play.
- Bricks never sends the ball straight up. A centre hit set the sideways speed to zero and the ball bounced in one
  cleared column forever.

Passes when a steady Paddle player and an angling one each finish a game to 7 inside ten minutes of ticks, the
angling one wins, and Bricks is cleared before the balls run out.

Usage: tools/checks/games-sim-check.py
"""
import os, sys
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
PADDLE = open(os.path.join(ROOT, "user", "paddle.c")).read()
BRICKS = open(os.path.join(ROOT, "user", "bricks.c")).read()
HALF = "if (vx > 0 && bx / FP > cw() / 2) {" in PADDLE
MINX = "if (vx > -8 && vx < 8) vx = x + BS / 2 < px + PW / 2 ? -8 : 8;" in BRICKS

def tdiv(a, b): q = abs(a) // abs(b); return q if (a >= 0) == (b > 0) else -q
FP = 16
def paddle(cw=804, h=345, ticks=60000, aim=0):
    TOP, PW, PH, BS, PX = 40, 8, 56, 8, 20
    s = 0x6D2B79F5; M = 0xFFFFFFFF
    def rnd():
        nonlocal s; s ^= (s << 13) & M; s ^= s >> 17; s ^= (s << 5) & M; return s
    chh = h - TOP
    st = dict(me=0, cpu=0, ly=TOP + chh // 2 - PH // 2, ry=TOP + chh // 2 - PH // 2)
    def clampy(y): return max(TOP, min(h - PH, y))
    def serve(t):
        st['bx'] = (cw // 2 - BS // 2) * FP; st['by'] = (TOP + chh // 2 - BS // 2) * FP
        st['vx'] = t * 3 * FP; vy = (rnd() % 5) * 8 - 16; st['vy'] = vy or 8; st['wait'] = 70
    serve(1); rallies = 0
    for t in range(ticks):
        # the player: Up/Down are 22px a press; assume a key every 8 ticks toward the ball's centre
        if t % 8 == 0:
            want = st['by'] // FP + BS // 2 - PH // 2 + aim
            if st['ly'] < want - 11: st['ly'] = clampy(st['ly'] + 22)
            elif st['ly'] > want + 11: st['ly'] = clampy(st['ly'] - 22)
        target = st['by'] // FP + BS // 2 - PH // 2
        if st['vx'] > 0 and (not HALF or st['bx'] // FP > cw // 2):
            if st['ry'] < target - 2: st['ry'] += 2
            elif st['ry'] > target + 2: st['ry'] -= 2
        st['ry'] = clampy(st['ry'])
        if st['wait']: st['wait'] -= 1; continue
        st['bx'] += st['vx']; st['by'] += st['vy']
        top, bot = TOP * FP, (h - BS) * FP
        if st['by'] < top: st['by'] = top; st['vy'] = -st['vy']
        if st['by'] > bot: st['by'] = bot; st['vy'] = -st['vy']
        x, y = tdiv(st['bx'], FP), tdiv(st['by'], FP); ly, ry = st['ly'], st['ry']
        if st['vx'] < 0 and x <= PX + PW and x + BS >= PX and y + BS >= ly and y <= ly + PH:
            st['bx'] = (PX + PW) * FP; st['vx'] = -st['vx'] + 2; st['vy'] = (y + BS // 2 - (ly + PH // 2)) * 2
            st['vx'] = min(st['vx'], 7 * FP); rallies += 1
        rx = cw - PX - PW
        if st['vx'] > 0 and x + BS >= rx and x <= rx + PW and y + BS >= ry and y <= ry + PH:
            st['bx'] = (rx - BS) * FP; st['vx'] = -st['vx'] - 2; st['vy'] = (y + BS // 2 - (ry + PH // 2)) * 2
            st['vx'] = max(st['vx'], -7 * FP)
        if x + BS < 0: st['cpu'] += 1; serve(1)
        elif x > cw: st['me'] += 1; serve(-1)
        if st['me'] >= 7 or st['cpu'] >= 7: return st['me'], st['cpu'], t, rallies
    return st['me'], st['cpu'], ticks, rallies

def bricks(cw=804, h=345, ticks=60000):
    TOP, PW, PH, BS, ROWS, COLS, BH, BGAP = 40, 72, 8, 8, 6, 12, 14, 4
    BTOP = TOP + 30; py = h - 28; bw = tdiv(cw - 32 - (COLS - 1) * BGAP, COLS)
    alive = [[1] * COLS for _ in range(ROWS)]; left = ROWS * COLS; lives = 3
    def bxof(c): return 16 + c * (bw + BGAP)
    def byof(r): return BTOP + r * (BH + BGAP)
    px = cw // 2 - PW // 2
    def serve():
        return (px + PW // 2 - BS // 2) * FP, (py - BS - 2) * FP, 24, -3 * FP
    bx, by, vx, vy = serve()
    for t in range(ticks):
        # the player: Left/Right are 28px a press, a press every 6 ticks; it aims the hit to send the ball toward
        # the lowest brick still standing (a hit right of centre sends it right)
        if t % 6 == 0:
            tgt = next(((bxof(c) + bw // 2) for r in range(ROWS - 1, -1, -1) for c in range(COLS) if alive[r][c]), cw // 2)
            ballx = bx // FP
            off = max(-28, min(28, (tgt - ballx) // 6))      # where on the paddle to take it
            want = ballx + BS // 2 - off - PW // 2
            if px < want - 14: px = min(cw - PW, px + 28)
            elif px > want + 14: px = max(0, px - 28)
        bx += vx; by += vy
        hi = (cw - BS) * FP
        if bx < 0: bx = 0; vx = -vx
        if bx > hi: bx = hi; vx = -vx
        if by < TOP * FP: by = TOP * FP; vy = -vy
        x, y = tdiv(bx, FP), tdiv(by, FP)
        if vy > 0 and y + BS >= py and y <= py + PH and x + BS >= px and x <= px + PW:
            by = (py - BS) * FP; vy = -vy; vx = (x + BS // 2 - (px + PW // 2)) * 2
            if vy > -3 * FP: vy = -3 * FP
            if MINX and -8 < vx < 8: vx = -8 if x + BS // 2 < px + PW // 2 else 8
        x, y = tdiv(bx, FP), tdiv(by, FP)
        hit = False
        for r in range(ROWS):
            for c in range(COLS):
                if not alive[r][c] or hit: continue
                X, Y = bxof(c), byof(r)
                if x + BS <= X or x >= X + bw or y + BS <= Y or y >= Y + BH: continue
                alive[r][c] = 0; left -= 1; hit = True
                ox = x + BS - X if vx > 0 else X + bw - x; oy = y + BS - Y if vy > 0 else Y + BH - y
                if ox < oy: vx = -vx
                else: vy = -vy
        if not left: return 'cleared', 3 - lives, t
        if y > h:
            lives -= 1
            if lives <= 0: return 'out', left, t
            bx, by, vx, vy = serve()
    return 'stuck', left, ticks


fails = []
steady, angled, (bres, bleft, bt) = paddle(), paddle(aim=18), bricks()
print(f"paddle, square hits: you {steady[0]}, computer {steady[1]} after {steady[2]} ticks ({steady[3]} returns)")
print(f"paddle, angled hits: you {angled[0]}, computer {angled[1]} after {angled[2]} ticks")
print(f"bricks: {bres} ({bleft} {'balls lost' if bres == 'cleared' else 'bricks left'}) after {bt} ticks")
if max(steady[:2]) < 7: fails.append("a game of Paddle with square hits never finished: the computer and the player rally forever")
if angled[0] < 7: fails.append("a player who angles shots did not beat the computer to 7")
if bres != "cleared": fails.append(f"Bricks was not cleared ({bres}): the ball got stuck or the balls ran out")
if fails:
    print("FAIL:"); [print("  - " + f) for f in fails]; sys.exit(1)
print("PASS: Paddle finishes and can be won, and Bricks can be cleared, with the physics played to the end on the host")
