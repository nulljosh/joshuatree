/* chess_core.h: the rules and the opponent for user/chess.c, with no system calls, so
 * tools/checks/chess-perft-check.py compiles the same code on the host and counts moves (perft).
 *
 * The board is 64 squares, a1 = 0 to h8 = 63 (rank * 8 + file). A piece is its type (P N B R Q K = 1 to 6) plus
 * CH_BLACK for Black. A move is a 16-bit number: from, to and a promotion type. The search keeps its positions and
 * move lists in per-ply arrays, not on the stack: an i386 ring-3 program has one 4 KB stack page.
 * The include site defines CH_DATA (JT_DATA in the app, empty on the host) for the writable arrays.
 */
#ifndef CHESS_CORE_H
#define CHESS_CORE_H

#define CH_P 1
#define CH_N 2
#define CH_B 3
#define CH_R 4
#define CH_Q 5
#define CH_K 6
#define CH_BLACK 8
#define CH_TYPE(p) ((p) & 7)
#define CH_SIDE(p) ((p) >> 3)            /* 0 White, 1 Black (only for a non-empty square) */
#define CH_MOVE(f, t, pr) ((unsigned short)((f) | ((t) << 6) | ((pr) << 12)))
#define CH_FROM(m) ((m) & 63)
#define CH_TO(m) (((m) >> 6) & 63)
#define CH_PROMO(m) ((m) >> 12)
#define CH_MAXM 256
#define CH_MAXPLY 16
#define CH_MATE 30000

struct ch_pos {
    unsigned char b[64];
    unsigned char side;                  /* to move: 0 White, 1 Black */
    unsigned char castle;                /* 1 White O-O, 2 White O-O-O, 4 Black O-O, 8 Black O-O-O */
    signed char ep;                      /* the square a pawn just skipped, or -1 */
};

static struct ch_pos ch_stack[CH_MAXPLY + 1] CH_DATA;        /* the search's positions, one per ply */
static unsigned short ch_moves[CH_MAXPLY + 1][CH_MAXM] CH_DATA;
static unsigned ch_nodes CH_DATA;

static const signed char ch_kn[8][2] = {{1, 2}, {2, 1}, {2, -1}, {1, -2}, {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2}};
static const signed char ch_dirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 1}, {-1, -1}};   /* rook first, then bishop */

static void ch_start(struct ch_pos *p) {
    static const unsigned char back[8] = {CH_R, CH_N, CH_B, CH_Q, CH_K, CH_B, CH_N, CH_R};
    for (int i = 0; i < 64; i++) p->b[i] = 0;
    for (int f = 0; f < 8; f++) {
        p->b[f] = back[f]; p->b[8 + f] = CH_P;
        p->b[48 + f] = CH_P | CH_BLACK; p->b[56 + f] = back[f] | CH_BLACK;
    }
    p->side = 0; p->castle = 15; p->ep = -1;
}

static int ch_on(int r, int f) { return r >= 0 && r < 8 && f >= 0 && f < 8; }

/* Is square sq attacked by side `by`? */
static int ch_attacked(const struct ch_pos *p, int sq, int by) {
    int r = sq >> 3, f = sq & 7, col = by ? CH_BLACK : 0;
    int pr = by ? r + 1 : r - 1;          /* a pawn of `by` attacking sq stands one rank behind it */
    if (pr >= 0 && pr < 8) {
        if (f > 0 && p->b[pr * 8 + f - 1] == (CH_P | col)) return 1;
        if (f < 7 && p->b[pr * 8 + f + 1] == (CH_P | col)) return 1;
    }
    for (int i = 0; i < 8; i++) {
        int rr = r + ch_kn[i][0], ff = f + ch_kn[i][1];
        if (ch_on(rr, ff) && p->b[rr * 8 + ff] == (CH_N | col)) return 1;
        rr = r + ch_dirs[i][0]; ff = f + ch_dirs[i][1];
        if (ch_on(rr, ff) && p->b[rr * 8 + ff] == (CH_K | col)) return 1;
    }
    for (int d = 0; d < 8; d++) {
        int rr = r, ff = f;
        for (;;) {
            rr += ch_dirs[d][0]; ff += ch_dirs[d][1];
            if (!ch_on(rr, ff)) break;
            int q = p->b[rr * 8 + ff];
            if (!q) continue;
            if (CH_SIDE(q) == by && (CH_TYPE(q) == CH_Q || CH_TYPE(q) == (d < 4 ? CH_R : CH_B))) return 1;
            break;
        }
    }
    return 0;
}

static int ch_king(const struct ch_pos *p, int side) {
    for (int i = 0; i < 64; i++) if (p->b[i] == (CH_K | (side ? CH_BLACK : 0))) return i;
    return -1;
}
static int ch_in_check(const struct ch_pos *p) { int k = ch_king(p, p->side); return k >= 0 && ch_attacked(p, k, !p->side); }

static int ch_add(unsigned short *m, int n, int f, int t, int promo_rank) {
    if (promo_rank) { for (int pr = CH_Q; pr >= CH_N; pr--) m[n++] = CH_MOVE(f, t, pr); }
    else m[n++] = CH_MOVE(f, t, 0);
    return n;
}

/* Every pseudo-legal move (the king may be left in check; ch_make reports that). */
static int ch_gen(const struct ch_pos *p, unsigned short *m) {
    int n = 0, s = p->side, col = s ? CH_BLACK : 0;
    for (int sq = 0; sq < 64; sq++) {
        int pc = p->b[sq];
        if (!pc || CH_SIDE(pc) != s) continue;
        int r = sq >> 3, f = sq & 7, t = CH_TYPE(pc);
        if (t == CH_P) {
            int dr = s ? -1 : 1, last = s ? 0 : 7, home = s ? 6 : 1, rr = r + dr;
            if (rr < 0 || rr > 7) continue;
            if (!p->b[rr * 8 + f]) {
                n = ch_add(m, n, sq, rr * 8 + f, rr == last);
                if (r == home && !p->b[(rr + dr) * 8 + f]) m[n++] = CH_MOVE(sq, (rr + dr) * 8 + f, 0);
            }
            for (int df = -1; df <= 1; df += 2) {
                int ff = f + df;
                if (ff < 0 || ff > 7) continue;
                int to = rr * 8 + ff, q = p->b[to];
                if ((q && CH_SIDE(q) != s) || to == p->ep) n = ch_add(m, n, sq, to, rr == last);
            }
        } else if (t == CH_N || t == CH_K) {
            for (int i = 0; i < 8; i++) {
                int rr = r + (t == CH_N ? ch_kn[i][0] : ch_dirs[i][0]), ff = f + (t == CH_N ? ch_kn[i][1] : ch_dirs[i][1]);
                if (!ch_on(rr, ff)) continue;
                int q = p->b[rr * 8 + ff];
                if (!q || CH_SIDE(q) != s) m[n++] = CH_MOVE(sq, rr * 8 + ff, 0);
            }
            if (t == CH_K && sq == (s ? 60 : 4) && !ch_attacked(p, sq, !s)) {
                int base = s ? 56 : 0, ks = s ? 4 : 1, qs = s ? 8 : 2;
                if ((p->castle & ks) && !p->b[base + 5] && !p->b[base + 6] && p->b[base + 7] == (CH_R | col)
                    && !ch_attacked(p, base + 5, !s) && !ch_attacked(p, base + 6, !s)) m[n++] = CH_MOVE(sq, base + 6, 0);
                if ((p->castle & qs) && !p->b[base + 3] && !p->b[base + 2] && !p->b[base + 1] && p->b[base] == (CH_R | col)
                    && !ch_attacked(p, base + 3, !s) && !ch_attacked(p, base + 2, !s)) m[n++] = CH_MOVE(sq, base + 2, 0);
            }
        } else {
            int d0 = t == CH_B ? 4 : 0, d1 = t == CH_R ? 4 : 8;
            for (int d = d0; d < d1; d++) {
                int rr = r, ff = f;
                for (;;) {
                    rr += ch_dirs[d][0]; ff += ch_dirs[d][1];
                    if (!ch_on(rr, ff)) break;
                    int q = p->b[rr * 8 + ff];
                    if (q && CH_SIDE(q) == s) break;
                    m[n++] = CH_MOVE(sq, rr * 8 + ff, 0);
                    if (q) break;
                }
            }
        }
        if (n > CH_MAXM - 16) break;   /* never in a real game; keeps a malformed position inside the array */
    }
    return n;
}

/* Play m from p into q. Returns 0 when it leaves the mover's own king in check (q is then garbage). */
static int ch_make(const struct ch_pos *p, unsigned short m, struct ch_pos *q) {
    *q = *p;
    int f = CH_FROM(m), t = CH_TO(m), pc = q->b[f], s = p->side;
    q->b[t] = pc; q->b[f] = 0;
    if (CH_TYPE(pc) == CH_P) {
        if (t == p->ep) q->b[t + (s ? 8 : -8)] = 0;                     /* en passant takes the pawn beside */
        if (CH_PROMO(m)) q->b[t] = (unsigned char)(CH_PROMO(m) | (s ? CH_BLACK : 0));
    }
    if (CH_TYPE(pc) == CH_K && (t - f == 2 || f - t == 2)) {              /* castling: the rook jumps over */
        int base = s ? 56 : 0;
        if (t > f) { q->b[base + 5] = q->b[base + 7]; q->b[base + 7] = 0; }
        else { q->b[base + 3] = q->b[base]; q->b[base] = 0; }
    }
    q->ep = (CH_TYPE(pc) == CH_P && (t - f == 16 || f - t == 16)) ? (signed char)((f + t) / 2) : -1;
    if (f == 4 || t == 4) q->castle &= ~3;
    if (f == 60 || t == 60) q->castle &= ~12;
    if (f == 0 || t == 0) q->castle &= ~2;
    if (f == 7 || t == 7) q->castle &= ~1;
    if (f == 56 || t == 56) q->castle &= ~8;
    if (f == 63 || t == 63) q->castle &= ~4;
    q->side = (unsigned char)!s;
    int k = ch_king(q, s);
    return k >= 0 && !ch_attacked(q, k, !s);
}

/* The legal moves into m; returns how many. */
static int ch_legal(const struct ch_pos *p, unsigned short *m) {
    int n = ch_gen(p, m), k = 0;
    struct ch_pos q;
    for (int i = 0; i < n; i++) if (ch_make(p, m[i], &q)) m[k++] = m[i];
    return k;
}

#ifdef CH_PERFT   /* the host check only */
static unsigned long long ch_perft(int ply, int depth) {
    if (!depth) return 1;
    struct ch_pos *p = &ch_stack[ply];
    unsigned short *m = ch_moves[ply];
    int n = ch_gen(p, m);
    unsigned long long c = 0;
    for (int i = 0; i < n; i++) if (ch_make(p, m[i], &ch_stack[ply + 1])) c += ch_perft(ply + 1, depth - 1);
    return c;
}
#endif

/* The evaluation, for the side to move: material plus a small pull to the centre and, for pawns, forward. */
static const short ch_val[7] = {0, 100, 320, 330, 500, 900, 0};
static int ch_eval(const struct ch_pos *p) {
    int score = 0;
    for (int sq = 0; sq < 64; sq++) {
        int pc = p->b[sq];
        if (!pc) continue;
        int t = CH_TYPE(pc), r = sq >> 3, f = sq & 7, v = ch_val[t];
        int cr = r < 4 ? r : 7 - r, cf = f < 4 ? f : 7 - f;   /* 0 on the edge, 3 in the centre */
        if (t == CH_N || t == CH_B) v += 5 * (cr + cf);
        else if (t == CH_Q) v += 2 * (cr + cf);
        else if (t == CH_P) v += 6 * (CH_SIDE(pc) ? 6 - r : r - 1) + (cf >= 2 && cr >= 2 ? 10 : 0);
        score += CH_SIDE(pc) == p->side ? v : -v;
    }
    return score;
}

static int ch_capture_value(const struct ch_pos *p, unsigned short m) {
    int t = p->b[CH_TO(m)];
    return t ? 10 * ch_val[CH_TYPE(t)] - ch_val[CH_TYPE(p->b[CH_FROM(m)])] : (CH_PROMO(m) ? 800 : 0);
}
/* Best first: captures by most valuable victim, then the rest in generation order (stable, so play repeats exactly). */
static void ch_order(const struct ch_pos *p, unsigned short *m, int n) {
    for (int i = 1; i < n; i++) {
        unsigned short x = m[i]; int v = ch_capture_value(p, x), j = i;
        while (j > 0 && ch_capture_value(p, m[j - 1]) < v) { m[j] = m[j - 1]; j--; }
        m[j] = x;
    }
}

static int ch_quiesce(int ply, int alpha, int beta) {
    struct ch_pos *p = &ch_stack[ply];
    ch_nodes++;
    int stand = ch_eval(p);
    if (stand >= beta) return stand;
    if (stand > alpha) alpha = stand;
    if (ply >= CH_MAXPLY - 1) return alpha;
    unsigned short *m = ch_moves[ply];
    int n = ch_gen(p, m), k = 0;
    for (int i = 0; i < n; i++) if (p->b[CH_TO(m[i])] || CH_PROMO(m[i])) m[k++] = m[i];
    ch_order(p, m, k);
    for (int i = 0; i < k; i++) {
        if (!ch_make(p, m[i], &ch_stack[ply + 1])) continue;
        int v = -ch_quiesce(ply + 1, -beta, -alpha);
        if (v >= beta) return v;
        if (v > alpha) alpha = v;
    }
    return alpha;
}

static int ch_search(int ply, int depth, int alpha, int beta) {
    if (depth <= 0) return ch_quiesce(ply, alpha, beta);
    struct ch_pos *p = &ch_stack[ply];
    ch_nodes++;
    unsigned short *m = ch_moves[ply];
    int n = ch_gen(p, m), any = 0;
    ch_order(p, m, n);
    for (int i = 0; i < n; i++) {
        if (!ch_make(p, m[i], &ch_stack[ply + 1])) continue;
        any = 1;
        int v = -ch_search(ply + 1, depth - 1, -beta, -alpha);
        if (v >= beta) return v;
        if (v > alpha) alpha = v;
    }
    if (!any) return ch_in_check(p) ? -CH_MATE + ply : 0;   /* mate, sooner is worse; stalemate is a draw */
    return alpha;
}

/* The opponent's choice in p at `depth` plies (plus captures to quiet), or 0 when it has no legal move. */
static unsigned short ch_best(const struct ch_pos *p, int depth) {
    ch_stack[0] = *p; ch_nodes = 0;
    unsigned short *m = ch_moves[0];
    int n = ch_gen(&ch_stack[0], m), best = -CH_MATE - 1;
    unsigned short pick = 0;
    ch_order(&ch_stack[0], m, n);
    for (int i = 0; i < n; i++) {
        unsigned short mv = m[i];
        if (!ch_make(&ch_stack[0], mv, &ch_stack[1])) continue;
        int v = -ch_search(1, depth - 1, -CH_MATE - 1, -best);
        if (v > best) { best = v; pick = mv; }
    }
    return pick;
}

/* "e2e4", or "e7e8q" with a promotion, into out (6 bytes). */
static void ch_name(unsigned short m, char *out) {
    static const char pr[7] = {0, 0, 'n', 'b', 'r', 'q', 0};
    out[0] = (char)('a' + (CH_FROM(m) & 7)); out[1] = (char)('1' + (CH_FROM(m) >> 3));
    out[2] = (char)('a' + (CH_TO(m) & 7)); out[3] = (char)('1' + (CH_TO(m) >> 3));
    out[4] = CH_PROMO(m) ? pr[CH_PROMO(m)] : 0; out[5] = 0;
}

/* 0 playing on, 1 the side to move is mated, 2 stalemate, 3 bare kings. */
static int ch_status(const struct ch_pos *p) {
    unsigned short *m = ch_moves[CH_MAXPLY];   /* the search never reaches this row */
    int others = 0;
    for (int i = 0; i < 64; i++) if (p->b[i] && CH_TYPE(p->b[i]) != CH_K) others = 1;
    if (!others) return 3;
    if (ch_legal(p, m)) return 0;
    return ch_in_check(p) ? 1 : 2;
}
#endif
