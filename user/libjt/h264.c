/* H.264 Constrained Baseline decoder: see h264.h. Clause numbers are ITU-T H.264's. */
#include "h264.h"

typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32; typedef signed char i8;

#define MAXREF 16
#define NBUF (MAXREF + 1)

struct sps { int ok, id, log2_max_fn, poc_type, log2_max_poc, dpo_zero, num_ref, mbw, mbh, crop[4]; };
struct pps { int ok, id, sps, nref_def, qp, cqp_off, deblock_ctl, cip, bfpo, cabac, wp; };
struct frame { u8 *y, *u, *v; int fn, ref, lt, ltidx, picnum; };   /* ref: 0 unused, 1 short, 2 long */
struct mb {
    u8 type, qp, qpc, skip;    /* type: 0 not decoded, 1 I4x4, 2 I16x16, 3 inter, 4 PCM */
    u8 cbp, cmode, dcf;        /* CABAC's neighbour context: coded_block_pattern, intra chroma mode, DC coded flags (luma, Cb, Cr) */
    u8 mvd[16][2];             /* |mvd| per 4x4, capped */
    u16 slice;
    u8 nnz[16], nnzc[2][4];    /* total coefficients per 4x4 block, raster order */
    i8 i4[16];                 /* intra 4x4 modes, raster */
    i8 ref[16];                /* reference index per 4x4, -1 intra */
    short mv[16][2];
    i8 refbuf[16];             /* which frame buffer that index named, for deblocking */
    i8 dbidc, dba, dbb;        /* the slice's deblocking control */
};
struct h264 {
    void *(*alloc)(unsigned long);
    struct sps sps[32]; struct pps pps[256];
    const struct sps *s; const struct pps *p;
    int mbw, mbh, w, h, cw, ch;
    struct frame buf[NBUF]; int nbuf;
    struct mb *mbs;
    u8 *rbsp; unsigned rbsp_max;
    int cur, decoded, slice_num, maxfn, prev_fn;
    /* the slice */
    int stype, nal_ref, idr, fn, qp, nref, dbidc, dba, dbb, mmco_adaptive, mmco5, lt_ref, cabac_idc, last_dqp;
    u32 crng, coff; struct bs *cs; u8 cst[460];   /* CABAC's engine and contexts */
    int list[MAXREF + 1];   /* buffer per reference index, one spare for 8.2.4.3.1 */
    int wp_on, lwd, cwd, lw[MAXREF][2], cwt[MAXREF][2][2];   /* explicit weighted prediction (7.3.3.2): weight, offset per reference */
    int mmco[64][3], nmmco;
    const char *err;
};

const char *h264_error(const struct h264 *d) { return d->err ? d->err : "ok"; }
static int fail(struct h264 *d, const char *why) { d->err = why; return -1; }

/* ---- bits (7.2) ---- */
struct bs { const u8 *b; u32 bytes, pos, end; };   /* end: the position of the rbsp stop bit */
static u32 peek(const struct bs *s, int n) {   /* n <= 25 */
    u32 i = s->pos >> 3, w = 0;
    for (int k = 0; k < 4; k++) w = w << 8 | (i + k < s->bytes ? s->b[i + k] : 0);
    return (w << (s->pos & 7)) >> (32 - n);
}
static u32 rb(struct bs *s, int n) { u32 v = 0; if (n > 16) { v = peek(s, 16) << (n - 16); s->pos += 16; n -= 16; } v |= peek(s, n); s->pos += n; return v; }
static u32 u1(struct bs *s) { return rb(s, 1); }
static u32 ue(struct bs *s) { int z = 0; while (!u1(s) && z < 32) z++; return z ? (1u << z) - 1 + rb(s, z) : 0; }
static int se(struct bs *s) { u32 k = ue(s); return k & 1 ? (int)((k + 1) >> 1) : -(int)(k >> 1); }
static int more_data(const struct bs *s) { return s->pos < s->end; }

/* ---- CAVLC tables (9.2), index total_coeff * 4 + trailing_ones ---- */
static const u8 ct_len[3][68] = {
    {1,0,0,0, 6,2,0,0, 8,6,3,0, 9,8,7,5, 10,9,8,6, 11,10,9,7, 13,11,10,8, 13,13,11,9, 13,13,13,10, 14,14,13,11, 14,14,14,13, 15,15,14,14, 15,15,15,14, 16,15,15,15, 16,16,16,15, 16,16,16,16, 16,16,16,16},
    {2,0,0,0, 6,2,0,0, 6,5,3,0, 7,6,6,4, 8,6,6,4, 8,7,7,5, 9,8,8,6, 11,9,9,6, 11,11,11,7, 12,11,11,9, 12,12,12,11, 12,12,12,11, 13,13,13,12, 13,13,13,13, 13,14,13,13, 14,14,14,13, 14,14,14,14},
    {4,0,0,0, 6,4,0,0, 6,5,4,0, 6,5,5,4, 7,5,5,4, 7,5,5,4, 7,6,6,4, 7,6,6,4, 8,7,7,5, 8,8,7,6, 9,8,8,7, 9,9,8,8, 9,9,9,8, 10,9,9,9, 10,10,10,10, 10,10,10,10, 10,10,10,10},
};
static const u8 ct_bits[3][68] = {
    {1,0,0,0, 5,1,0,0, 7,4,1,0, 7,6,5,3, 7,6,5,3, 7,6,5,4, 15,6,5,4, 11,14,5,4, 8,10,13,4, 15,14,9,4, 11,10,13,12, 15,14,9,12, 11,10,13,8, 15,1,9,12, 11,14,13,8, 7,10,9,12, 4,6,5,8},
    {3,0,0,0, 11,2,0,0, 7,7,3,0, 7,10,9,5, 7,6,5,4, 4,6,5,6, 7,6,5,8, 15,6,5,4, 11,14,13,4, 15,10,9,4, 11,14,13,12, 8,10,9,8, 15,14,13,12, 11,10,9,12, 7,11,6,8, 9,8,10,1, 7,6,5,4},
    {15,0,0,0, 15,14,0,0, 11,15,13,0, 8,12,14,12, 15,10,11,11, 11,8,9,10, 9,14,13,9, 8,10,9,8, 15,14,13,13, 11,14,10,12, 15,10,13,12, 11,14,9,12, 8,10,13,8, 13,7,9,12, 9,12,11,10, 5,8,7,6, 1,4,3,2},
};
static const u8 cdc_len[20] = {2,0,0,0, 6,1,0,0, 6,6,3,0, 6,7,7,6, 6,8,8,7};
static const u8 cdc_bits[20] = {1,0,0,0, 7,1,0,0, 4,6,1,0, 3,3,2,5, 2,3,2,0};
static const u8 tz_len[15][16] = {
    {1,3,3,4,4,5,5,6,6,7,7,8,8,9,9,9}, {3,3,3,3,3,4,4,4,4,5,5,6,6,6,6}, {4,3,3,3,4,4,3,3,4,5,5,6,5,6}, {5,3,4,4,3,3,3,4,3,4,5,5,5},
    {4,4,4,3,3,3,3,3,4,5,4,5}, {6,5,3,3,3,3,3,3,4,3,6}, {6,5,3,3,3,2,3,4,3,6}, {6,4,5,3,2,2,3,3,6}, {6,6,4,2,2,3,2,5},
    {5,5,3,2,2,2,4}, {4,4,3,3,1,3}, {4,4,2,1,3}, {3,3,1,2}, {2,2,1}, {1,1},
};
static const u8 tz_bits[15][16] = {
    {1,3,2,3,2,3,2,3,2,3,2,3,2,3,2,1}, {7,6,5,4,3,5,4,3,2,3,2,3,2,1,0}, {5,7,6,5,4,3,4,3,2,3,2,1,1,0}, {3,7,5,4,6,5,4,3,3,2,2,1,0},
    {5,4,3,7,6,5,4,3,2,1,1,0}, {1,1,7,6,5,4,3,2,1,1,0}, {1,1,5,4,3,3,2,1,1,0}, {1,1,1,3,3,2,2,1,0}, {1,0,1,3,2,1,1,1},
    {1,0,1,3,2,1,1}, {0,1,1,2,1,3}, {0,1,1,1,1}, {0,1,1,1}, {0,1,1}, {0,1},
};
static const u8 ctz_len[3][4] = {{1,2,3,3}, {1,2,2}, {1,1}};
static const u8 ctz_bits[3][4] = {{1,1,1,0}, {1,1,0}, {1,0}};
static const u8 rb_len[7][16] = {{1,1}, {1,2,2}, {2,2,2,2}, {2,2,2,3,3}, {2,2,3,3,3,3}, {2,3,3,3,3,3,3}, {3,3,3,3,3,3,3,4,5,6,7,8,9,10,11}};
static const u8 rb_bits[7][16] = {{1,0}, {1,1,0}, {3,2,1,0}, {3,2,1,1,0}, {3,2,3,2,1,0}, {3,0,1,3,2,5,4}, {7,6,5,4,3,2,1,1,1,1,1,1,1,1,1}};

/* The entry whose code matches the next bits: its index, or -1. ponytail: a scan per symbol, a lookup table if it shows in a profile. */
static int vlc(struct bs *s, const u8 *len, const u8 *bits, int n) {
    for (int l = 1; l <= 16; l++) {
        u32 c = peek(s, l);
        for (int i = 0; i < n; i++) if (len[i] == l && bits[i] == c) { s->pos += l; return i; }
    }
    return -1;
}

/* residual_block_cavlc (7.3.5.3.2): coefficients in scan order into out[0..max-1], the count of non-zero ones back, or -1 */
static int cavlc(struct bs *s, int nc, int max, int *out) {
    int tok, total, t1;
    for (int i = 0; i < max; i++) out[i] = 0;
    if (nc == -1) tok = vlc(s, cdc_len, cdc_bits, 20);
    else if (nc >= 8) { u32 v = rb(s, 6); tok = v == 3 ? 0 : (int)(((v >> 2) + 1) * 4 + (v & 3)); if (v != 3 && (v & 3) > (v >> 2) + 1) return -1; }
    else tok = vlc(s, ct_len[nc < 2 ? 0 : nc < 4 ? 1 : 2], ct_bits[nc < 2 ? 0 : nc < 4 ? 1 : 2], 68);
    if (tok < 0) return -1;
    total = tok >> 2; t1 = tok & 3;
    if (!total) return 0;
    if (total > max) return -1;
    int level[16], run[16], suf = total > 10 && t1 < 3;
    for (int i = 0; i < total; i++) {
        if (i < t1) { level[i] = u1(s) ? -1 : 1; continue; }
        int prefix = 0;
        while (!u1(s)) if (++prefix > 25) return -1;
        int ssize = prefix == 14 && !suf ? 4 : prefix >= 15 ? prefix - 3 : suf;
        int code = ((prefix < 15 ? prefix : 15) << suf) + (ssize ? (int)rb(s, ssize) : 0);
        if (prefix >= 15 && !suf) code += 15;
        if (prefix >= 16) code += (1 << (prefix - 3)) - 4096;
        if (i == t1 && t1 < 3) code += 2;
        level[i] = code & 1 ? (-code - 1) >> 1 : (code + 2) >> 1;
        if (!suf) suf = 1;
        int a = level[i] < 0 ? -level[i] : level[i];
        if (a > (3 << (suf - 1)) && suf < 6) suf++;
    }
    int zeros = 0;
    if (total < max) {
        zeros = max == 4 ? vlc(s, ctz_len[total - 1], ctz_bits[total - 1], 4 - total + 1) : vlc(s, tz_len[total - 1], tz_bits[total - 1], 16 - total + 1);
        if (zeros < 0) return -1;
    }
    for (int i = 0; i < total - 1; i++) {
        if (zeros > 0) {
            int t = zeros > 7 ? 6 : zeros - 1;
            run[i] = vlc(s, rb_len[t], rb_bits[t], t == 6 ? 15 : t + 2);
            if (run[i] < 0 || run[i] > zeros) return -1;
            zeros -= run[i];
        } else run[i] = 0;
    }
    run[total - 1] = zeros;
    int k = -1;
    for (int i = total - 1; i >= 0; i--) { k += run[i] + 1; if (k >= max) return -1; out[k] = level[i]; }
    return total;
}

/* ---- tables for reconstruction ---- */
static const u8 zz[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};   /* scan position -> raster */
static const u8 blk_x[16] = {0,1,0,1,2,3,2,3,0,1,0,1,2,3,2,3}, blk_y[16] = {0,0,1,1,0,0,1,1,2,2,3,3,2,2,3,3};
static const u8 norm[6][3] = {{10,16,13}, {11,18,14}, {13,20,16}, {14,23,18}, {16,25,20}, {18,29,23}};
static const u8 qpc_tab[52] = {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,
    29,30,31,32,32,33,34,34,35,35,36,36,37,37,37,38,38,38,39,39,39,39};
static const u8 cbp_intra[48] = {47,31,15,0,23,27,29,30,7,11,13,14,39,43,45,46,16,3,5,10,12,19,21,26,28,35,37,42,44,1,2,4,8,17,18,20,24,6,9,22,25,32,33,34,36,40,38,41};
static const u8 cbp_inter[48] = {0,16,1,2,4,8,32,3,5,10,12,15,47,7,11,13,14,6,9,31,35,37,42,44,33,34,36,40,39,43,45,46,17,18,20,24,19,21,26,28,23,27,29,30,22,25,38,41};
static int cls(int r) { int i = r >> 2, j = r & 3; return !(i & 1) && !(j & 1) ? 0 : (i & 1) && (j & 1) ? 1 : 2; }
static int clip3(int lo, int hi, int v) { return v < lo ? lo : v > hi ? hi : v; }
static u8 clip1(int v) { return v < 0 ? 0 : v > 255 ? 255 : (u8)v; }

static void idct_add(u8 *dst, int stride, int *d) {   /* 8.5.12.2: d raster, dequantised */
    int t[16];
    for (int i = 0; i < 4; i++) {
        int *r = d + 4 * i, e0 = r[0] + r[2], e1 = r[0] - r[2], e2 = (r[1] >> 1) - r[3], e3 = r[1] + (r[3] >> 1);
        t[4 * i] = e0 + e3; t[4 * i + 1] = e1 + e2; t[4 * i + 2] = e1 - e2; t[4 * i + 3] = e0 - e3;
    }
    for (int j = 0; j < 4; j++) {
        int e0 = t[j] + t[8 + j], e1 = t[j] - t[8 + j], e2 = (t[4 + j] >> 1) - t[12 + j], e3 = t[4 + j] + (t[12 + j] >> 1);
        int f[4] = {e0 + e3, e1 + e2, e1 - e2, e0 - e3};
        for (int i = 0; i < 4; i++) dst[i * stride + j] = clip1(dst[i * stride + j] + ((f[i] + 32) >> 6));
    }
}

/* ---- neighbours ---- */
static int mb_ok(const struct h264 *d, int mx, int my, int cur) {   /* decoded already, in the same slice */
    if (mx < 0 || my < 0 || mx >= d->mbw || my >= d->mbh) return 0;
    const struct mb *m = &d->mbs[my * d->mbw + mx], *c = &d->mbs[cur];
    return m->type && m->slice == c->slice && my * d->mbw + mx < cur;
}
/* The 4x4 block at (x4, y4) relative to MB cur, which may be outside it: its MB, or 0 when not available */
static struct mb *blk(struct h264 *d, int cur, int x4, int y4, int *r) {
    int mx = cur % d->mbw, my = cur / d->mbw;
    int ox = x4 < 0 ? -1 : x4 > 3 ? 1 : 0, oy = y4 < 0 ? -1 : 0;
    if (!ox && !oy) { *r = y4 * 4 + x4; return &d->mbs[cur]; }
    if (!mb_ok(d, mx + ox, my + oy, cur)) return 0;
    *r = (y4 & 3) * 4 + (x4 & 3);
    return &d->mbs[(my + oy) * d->mbw + mx + ox];
}
static int dec_order(int x4, int y4) { for (int i = 0; i < 16; i++) if (blk_x[i] == x4 && blk_y[i] == y4) return i; return 16; }
static int nc_luma(struct h264 *d, int cur, int x4, int y4) {
    int r, a = 0, b = 0, na = 0, nb = 0; struct mb *m;
    if ((m = blk(d, cur, x4 - 1, y4, &r))) { a = 1; na = m->nnz[r]; }
    if ((m = blk(d, cur, x4, y4 - 1, &r))) { b = 1; nb = m->nnz[r]; }
    return a && b ? (na + nb + 1) >> 1 : a ? na : b ? nb : 0;
}
static int nc_chroma(struct h264 *d, int cur, int c, int x2, int y2) {
    int mx = cur % d->mbw, my = cur / d->mbw, a = 0, b = 0, na = 0, nb = 0;
    if (x2) { a = 1; na = d->mbs[cur].nnzc[c][y2 * 2]; } else if (mb_ok(d, mx - 1, my, cur)) { a = 1; na = d->mbs[cur - 1].nnzc[c][y2 * 2 + 1]; }
    if (y2) { b = 1; nb = d->mbs[cur].nnzc[c][x2]; } else if (mb_ok(d, mx, my - 1, cur)) { b = 1; nb = d->mbs[cur - d->mbw].nnzc[c][2 + x2]; }
    return a && b ? (na + nb + 1) >> 1 : a ? na : b ? nb : 0;
}
/* intra neighbours: an inter neighbour does not count under constrained_intra_pred */
static int intra_ok(struct h264 *d, int cur, int mx, int my) {
    if (!mb_ok(d, mx, my, cur)) return 0;
    return !(d->p->cip && d->mbs[my * d->mbw + mx].type == 3);
}

/* ---- intra prediction (8.3) ---- */
static void pred4(struct h264 *d, int cur, u8 *dst, int st, int x4, int y4, int mode) {
    int mx = cur % d->mbw, my = cur / d->mbw;
    int L = x4 ? 1 : intra_ok(d, cur, mx - 1, my), T = y4 ? 1 : intra_ok(d, cur, mx, my - 1);
    int TL = x4 && y4 ? 1 : x4 ? T : y4 ? L : intra_ok(d, cur, mx - 1, my - 1);
    int TR;
    if (y4 == 0) TR = x4 < 3 ? T : intra_ok(d, cur, mx + 1, my - 1);
    else TR = x4 < 3 && dec_order(x4 + 1, y4 - 1) < dec_order(x4, y4);
    int t[8], l[4], tl = TL ? dst[-st - 1] : 0;
    for (int i = 0; i < 4; i++) { t[i] = T ? dst[-st + i] : 0; l[i] = L ? dst[i * st - 1] : 0; }
    for (int i = 4; i < 8; i++) t[i] = TR ? dst[-st + i] : t[3];
    int p[16];
    switch (mode) {
    case 0: for (int i = 0; i < 16; i++) p[i] = t[i & 3]; break;
    case 1: for (int i = 0; i < 16; i++) p[i] = l[i >> 2]; break;
    case 2: { int s = 0, v;
        if (T && L) { for (int i = 0; i < 4; i++) s += t[i] + l[i]; v = (s + 4) >> 3; }
        else if (L) { for (int i = 0; i < 4; i++) s += l[i]; v = (s + 2) >> 2; }
        else if (T) { for (int i = 0; i < 4; i++) s += t[i]; v = (s + 2) >> 2; }
        else v = 128;
        for (int i = 0; i < 16; i++) p[i] = v; } break;
    case 3:
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++)
            p[y * 4 + x] = x == 3 && y == 3 ? (t[6] + 3 * t[7] + 2) >> 2 : (t[x + y] + 2 * t[x + y + 1] + t[x + y + 2] + 2) >> 2;
        break;
    default: {
        for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
            int v, zv = 2 * x - y, zh = 2 * y - x;
            if (mode == 4) {
                if (x > y) v = ((x - y - 2 < 0 ? tl : t[x - y - 2]) + 2 * t[x - y - 1] + t[x - y] + 2) >> 2;
                else if (x < y) v = ((y - x - 2 < 0 ? tl : l[y - x - 2]) + 2 * l[y - x - 1] + l[y - x] + 2) >> 2;
                else v = (t[0] + 2 * tl + l[0] + 2) >> 2;
            } else if (mode == 5) {
                if (zv >= 0 && !(zv & 1)) v = ((x - (y >> 1) - 1 < 0 ? tl : t[x - (y >> 1) - 1]) + t[x - (y >> 1)] + 1) >> 1;
                else if (zv >= 0) v = ((x - (y >> 1) - 2 < 0 ? tl : t[x - (y >> 1) - 2]) + 2 * (x - (y >> 1) - 1 < 0 ? tl : t[x - (y >> 1) - 1]) + t[x - (y >> 1)] + 2) >> 2;
                else if (zv == -1) v = (l[0] + 2 * tl + t[0] + 2) >> 2;
                else v = (l[y - 1] + 2 * (y - 2 < 0 ? tl : l[y - 2]) + (y - 3 < 0 ? tl : l[y - 3]) + 2) >> 2;
            } else if (mode == 6) {
                if (zh >= 0 && !(zh & 1)) v = ((y - (x >> 1) - 1 < 0 ? tl : l[y - (x >> 1) - 1]) + l[y - (x >> 1)] + 1) >> 1;
                else if (zh >= 0) v = ((y - (x >> 1) - 2 < 0 ? tl : l[y - (x >> 1) - 2]) + 2 * (y - (x >> 1) - 1 < 0 ? tl : l[y - (x >> 1) - 1]) + l[y - (x >> 1)] + 2) >> 2;
                else if (zh == -1) v = (l[0] + 2 * tl + t[0] + 2) >> 2;
                else v = (t[x - 1] + 2 * (x - 2 < 0 ? tl : t[x - 2]) + (x - 3 < 0 ? tl : t[x - 3]) + 2) >> 2;
            } else if (mode == 7) {
                v = !(y & 1) ? (t[x + (y >> 1)] + t[x + (y >> 1) + 1] + 1) >> 1 : (t[x + (y >> 1)] + 2 * t[x + (y >> 1) + 1] + t[x + (y >> 1) + 2] + 2) >> 2;
            } else {   /* 8: horizontal up */
                int z = x + 2 * y;
                if (z < 5 && !(z & 1)) v = (l[y + (x >> 1)] + l[y + (x >> 1) + 1] + 1) >> 1;
                else if (z < 5) v = (l[y + (x >> 1)] + 2 * l[y + (x >> 1) + 1] + l[y + (x >> 1) + 2 > 3 ? 3 : y + (x >> 1) + 2] + 2) >> 2;
                else if (z == 5) v = (l[2] + 3 * l[3] + 2) >> 2;
                else v = l[3];
            }
            p[y * 4 + x] = v;
        }
    }
    }
    for (int i = 0; i < 16; i++) dst[(i >> 2) * st + (i & 3)] = (u8)p[i];
}

static void pred_block(struct h264 *d, int cur, u8 *dst, int st, int n, int mode, int chroma) {   /* 16x16 luma or 8x8 chroma */
    int mx = cur % d->mbw, my = cur / d->mbw, L = intra_ok(d, cur, mx - 1, my), T = intra_ok(d, cur, mx, my - 1), TL = intra_ok(d, cur, mx - 1, my - 1);
    int vmode = chroma ? 2 : 0, hmode = chroma ? 1 : 1, dcmode = chroma ? 0 : 2;
    if (mode == vmode) { for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) dst[y * st + x] = dst[-st + x]; return; }
    if (mode == hmode) { for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) dst[y * st + x] = dst[y * st - 1]; return; }
    if (mode == dcmode && !chroma) {
        int s = 0, v;
        if (T && L) { for (int i = 0; i < 16; i++) s += dst[-st + i] + dst[i * st - 1]; v = (s + 16) >> 5; }
        else if (L) { for (int i = 0; i < 16; i++) s += dst[i * st - 1]; v = (s + 8) >> 4; }
        else if (T) { for (int i = 0; i < 16; i++) s += dst[-st + i]; v = (s + 8) >> 4; }
        else v = 128;
        for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) dst[y * st + x] = (u8)v;
        return;
    }
    if (mode == dcmode) {   /* chroma DC, each 4x4 on its own (8.3.4.1-3) */
        for (int by = 0; by < 2; by++) for (int bx = 0; bx < 2; bx++) {
            int st_ = 0, sl = 0, v;
            for (int i = 0; i < 4; i++) { if (T) st_ += dst[-st + bx * 4 + i]; if (L) sl += dst[(by * 4 + i) * st - 1]; }
            if (bx == by) v = T && L ? (st_ + sl + 4) >> 3 : L ? (sl + 2) >> 2 : T ? (st_ + 2) >> 2 : 128;
            else if (bx) v = T ? (st_ + 2) >> 2 : L ? (sl + 2) >> 2 : 128;
            else v = L ? (sl + 2) >> 2 : T ? (st_ + 2) >> 2 : 128;
            for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) dst[(by * 4 + y) * st + bx * 4 + x] = (u8)v;
        }
        return;
    }
    /* plane */
    (void)TL;
    int half = n / 2, H = 0, V = 0;
    for (int i = 0; i < half; i++) {
        H += (i + 1) * (dst[-st + half + i] - dst[-st + half - 2 - i]);
        V += (i + 1) * (dst[(half + i) * st - 1] - dst[(half - 2 - i) * st - 1]);
    }
    int a = 16 * (dst[(n - 1) * st - 1] + dst[-st + n - 1]);
    int b = chroma ? (34 * H + 32) >> 6 : (5 * H + 32) >> 6, c = chroma ? (34 * V + 32) >> 6 : (5 * V + 32) >> 6;
    for (int y = 0; y < n; y++) for (int x = 0; x < n; x++) dst[y * st + x] = clip1((a + b * (x - half + 1) + c * (y - half + 1) + 16) >> 5);
}

/* ---- inter prediction (8.4) ---- */
static int pix(const u8 *p, int st, int w, int h, int x, int y) { x = x < 0 ? 0 : x >= w ? w - 1 : x; y = y < 0 ? 0 : y >= h ? h - 1 : y; return p[y * st + x]; }
static int tap6(int a, int b, int c, int e, int f, int g) { return a - 5 * b + 20 * c + 20 * e - 5 * f + g; }
static int hraw(const u8 *p, int st, int w, int h, int x, int y) { return tap6(pix(p,st,w,h,x-2,y), pix(p,st,w,h,x-1,y), pix(p,st,w,h,x,y), pix(p,st,w,h,x+1,y), pix(p,st,w,h,x+2,y), pix(p,st,w,h,x+3,y)); }
static int vraw(const u8 *p, int st, int w, int h, int x, int y) { return tap6(pix(p,st,w,h,x,y-2), pix(p,st,w,h,x,y-1), pix(p,st,w,h,x,y), pix(p,st,w,h,x,y+1), pix(p,st,w,h,x,y+2), pix(p,st,w,h,x,y+3)); }
static int luma_q(const u8 *p, int st, int w, int h, int x, int y, int fx, int fy) {   /* 8.4.2.2.1 */
    #define G(dx, dy) pix(p, st, w, h, x + (dx), y + (dy))
    #define B(dy) clip1((hraw(p, st, w, h, x, y + (dy)) + 16) >> 5)
    #define Hh(dx) clip1((vraw(p, st, w, h, x + (dx), y) + 16) >> 5)
    #define J() clip1((tap6(hraw(p,st,w,h,x,y-2), hraw(p,st,w,h,x,y-1), hraw(p,st,w,h,x,y), hraw(p,st,w,h,x,y+1), hraw(p,st,w,h,x,y+2), hraw(p,st,w,h,x,y+3)) + 512) >> 10)
    switch (fy * 4 + fx) {
    case 0: return G(0, 0);
    case 1: return (G(0, 0) + B(0) + 1) >> 1;
    case 2: return B(0);
    case 3: return (G(1, 0) + B(0) + 1) >> 1;
    case 4: return (G(0, 0) + Hh(0) + 1) >> 1;
    case 5: return (B(0) + Hh(0) + 1) >> 1;
    case 6: return (B(0) + J() + 1) >> 1;
    case 7: return (B(0) + Hh(1) + 1) >> 1;
    case 8: return Hh(0);
    case 9: return (Hh(0) + J() + 1) >> 1;
    case 10: return J();
    case 11: return (J() + Hh(1) + 1) >> 1;
    case 12: return (G(0, 1) + Hh(0) + 1) >> 1;
    case 13: return (Hh(0) + B(1) + 1) >> 1;
    case 14: return (J() + B(1) + 1) >> 1;
    default: return (Hh(1) + B(1) + 1) >> 1;
    }
    #undef G
    #undef B
    #undef Hh
    #undef J
}
static int wgt(int v, int w, int o, int logwd) { return clip1(logwd >= 1 ? ((v * w + (1 << (logwd - 1))) >> logwd) + o : v * w + o); }   /* 8.4.2.3.2 */
static void mc(struct h264 *d, int cur, int bx, int by, int bw, int bh, int refbuf, const short *mv, int ref) {   /* bx, by, bw, bh in luma samples within the MB */
    const struct frame *r = &d->buf[refbuf]; struct frame *f = &d->buf[d->cur];
    int wp = d->wp_on && ref >= 0 && ref < MAXREF;
    int mx = cur % d->mbw * 16 + bx, my = cur / d->mbw * 16 + by;
    for (int y = 0; y < bh; y++) for (int x = 0; x < bw; x++)
        { int v = luma_q(r->y, d->w, d->w, d->h, mx + x + (mv[0] >> 2), my + y + (mv[1] >> 2), mv[0] & 3, mv[1] & 3);
          f->y[(my + y) * d->w + mx + x] = (u8)(wp ? wgt(v, d->lw[ref][0], d->lw[ref][1], d->lwd) : v); }
    int cx = mx / 2, cy = my / 2, fx = mv[0] & 7, fy = mv[1] & 7;
    for (int c = 0; c < 2; c++) {
        const u8 *rp = c ? r->v : r->u; u8 *dp = c ? f->v : f->u;
        for (int y = 0; y < bh / 2; y++) for (int x = 0; x < bw / 2; x++) {
            int xi = cx + x + (mv[0] >> 3), yi = cy + y + (mv[1] >> 3);
            int A = pix(rp, d->cw, d->cw, d->ch, xi, yi), Bv = pix(rp, d->cw, d->cw, d->ch, xi + 1, yi);
            int C = pix(rp, d->cw, d->cw, d->ch, xi, yi + 1), D = pix(rp, d->cw, d->cw, d->ch, xi + 1, yi + 1);
            int v = ((8 - fx) * (8 - fy) * A + fx * (8 - fy) * Bv + (8 - fx) * fy * C + fx * fy * D + 32) >> 6;
            dp[(cy + y) * d->cw + cx + x] = (u8)(wp ? wgt(v, d->cwt[ref][c][0], d->cwt[ref][c][1], d->cwd) : v);
        }
    }
}
static int med(int a, int b, int c) { int mn = a < b ? a : b, mxv = a > b ? a : b; return c < mn ? mn : c > mxv ? mxv : c; }
/* 8.4.1.3: the predicted mv of a partition at (x4, y4) w4 wide (in 4x4 blocks) with reference ref; shape 1 16x8, 2 8x16 */
static void mvpred(struct h264 *d, int cur, int x4, int y4, int w4, int ref, int shape, int part, short *out) {
    int r; struct mb *m;
    int ra = -1, rbv = -1, rc = -1, av = 0, bv = 0, cv = 0; short a[2] = {0, 0}, b[2] = {0, 0}, c[2] = {0, 0};
    if ((m = blk(d, cur, x4 - 1, y4, &r))) { av = 1; ra = m->ref[r]; if (ra >= 0) { a[0] = m->mv[r][0]; a[1] = m->mv[r][1]; } }
    if ((m = blk(d, cur, x4, y4 - 1, &r))) { bv = 1; rbv = m->ref[r]; if (rbv >= 0) { b[0] = m->mv[r][0]; b[1] = m->mv[r][1]; } }
    int cx = x4 + w4, cy = y4 - 1;   /* C, above right; D, above left, when C is not there or not decoded yet */
    int cok = cy < 0 ? 1 : cx <= 3 && dec_order(cx, cy) < dec_order(x4, y4);
    if (cok && (m = blk(d, cur, cx, cy, &r))) { cv = 1; rc = m->ref[r]; if (rc >= 0) { c[0] = m->mv[r][0]; c[1] = m->mv[r][1]; } }
    else if ((m = blk(d, cur, x4 - 1, y4 - 1, &r))) { cv = 1; rc = m->ref[r]; if (rc >= 0) { c[0] = m->mv[r][0]; c[1] = m->mv[r][1]; } }
    if (!bv && !cv && av) { rbv = rc = ra; b[0] = c[0] = a[0]; b[1] = c[1] = a[1]; }
    if (shape == 1 && part == 0 && rbv == ref) { out[0] = b[0]; out[1] = b[1]; return; }
    if (shape == 1 && part == 1 && ra == ref) { out[0] = a[0]; out[1] = a[1]; return; }
    if (shape == 2 && part == 0 && ra == ref) { out[0] = a[0]; out[1] = a[1]; return; }
    if (shape == 2 && part == 1 && rc == ref) { out[0] = c[0]; out[1] = c[1]; return; }
    int n = (ra == ref) + (rbv == ref) + (rc == ref);
    if (n == 1) { const short *s = ra == ref ? a : rbv == ref ? b : c; out[0] = s[0]; out[1] = s[1]; return; }
    out[0] = (short)med(a[0], b[0], c[0]); out[1] = (short)med(a[1], b[1], c[1]);
}
static void set_part(struct mb *m, int x4, int y4, int w4, int h4, int ref, int refbuf, const short *mv) {
    for (int y = y4; y < y4 + h4; y++) for (int x = x4; x < x4 + w4; x++) { m->ref[y * 4 + x] = (i8)ref; m->refbuf[y * 4 + x] = (i8)refbuf; m->mv[y * 4 + x][0] = mv[0]; m->mv[y * 4 + x][1] = mv[1]; }
}

/* ---- residual and reconstruction ---- */
static int dq(int c, int qp, int r) { return c * norm[qp % 6][cls(r)] * (1 << (qp / 6)); }

static int luma_residual(struct h264 *d, struct bs *s, int cur, int cbp, int i16, int *coef /* 16x16 raster per block */, int *dc) {
    struct mb *m = &d->mbs[cur];
    int sc[16];
    if (i16) {
        int n = cavlc(s, nc_luma(d, cur, 0, 0), 16, sc); if (n < 0) return -1;
        for (int i = 0; i < 16; i++) dc[zz[i]] = sc[i];
    }
    for (int b = 0; b < 16; b++) {
        int x4 = blk_x[b], y4 = blk_y[b], r = y4 * 4 + x4, *o = coef + r * 16;
        for (int i = 0; i < 16; i++) o[i] = 0;
        if (!(cbp & (1 << (b >> 2)))) { m->nnz[r] = 0; continue; }
        int nc = nc_luma(d, cur, x4, y4);
        int n = i16 ? cavlc(s, nc, 15, sc) : cavlc(s, nc, 16, sc);
        if (n < 0) return -1;
        m->nnz[r] = (u8)n;
        if (i16) for (int i = 0; i < 15; i++) o[zz[i + 1]] = sc[i];
        else for (int i = 0; i < 16; i++) o[zz[i]] = sc[i];
    }
    return 0;
}
static void luma_recon(struct h264 *d, int cur, int qp, int i16, int *coef, int *dc, int i4modes, const i8 *modes) {
    u8 *base = d->buf[d->cur].y + cur / d->mbw * 16 * d->w + cur % d->mbw * 16;
    if (i16) {   /* 8.5.10: the DC Hadamard */
        int t[16], f[16];
        for (int i = 0; i < 4; i++) { int *c = dc + 4 * i; t[4*i] = c[0]+c[1]+c[2]+c[3]; t[4*i+1] = c[0]+c[1]-c[2]-c[3]; t[4*i+2] = c[0]-c[1]-c[2]+c[3]; t[4*i+3] = c[0]-c[1]+c[2]-c[3]; }
        for (int j = 0; j < 4; j++) { f[j] = t[j]+t[4+j]+t[8+j]+t[12+j]; f[4+j] = t[j]+t[4+j]-t[8+j]-t[12+j]; f[8+j] = t[j]-t[4+j]-t[8+j]+t[12+j]; f[12+j] = t[j]-t[4+j]+t[8+j]-t[12+j]; }
        int ls = 16 * norm[qp % 6][0];
        for (int i = 0; i < 16; i++) {
            int v = qp >= 36 ? f[i] * ls * (1 << (qp / 6 - 6)) : (f[i] * ls + (1 << (5 - qp / 6))) >> (6 - qp / 6);
            coef[i * 16] = v;   /* block (raster i)'s DC, already scaled */
        }
    }
    for (int b = 0; b < 16; b++) {
        int x4 = blk_x[b], y4 = blk_y[b], r = y4 * 4 + x4, *o = coef + r * 16, dd[16];
        u8 *dst = base + y4 * 4 * d->w + x4 * 4;
        if (i4modes) pred4(d, cur, dst, d->w, x4, y4, modes[r]);
        for (int i = 0; i < 16; i++) dd[i] = i16 && !i ? o[0] : dq(o[i], qp, i);
        int any = 0; for (int i = 0; i < 16; i++) any |= dd[i];
        if (any) idct_add(dst, d->w, dd);
    }
}
struct mbd { int intra, i16, i16mode, cmode, cbp; i8 modes[16]; int coef[256], dc[16], dcc[2][4], ac[2][4][16]; };
static int chroma_cavlc(struct h264 *d, struct bs *s, int cur, struct mbd *x) {
    struct mb *m = &d->mbs[cur]; int sc[16], cbpc = x->cbp >> 4;
    if (cbpc) for (int c = 0; c < 2; c++) { if (cavlc(s, -1, 4, x->dcc[c]) < 0) return -1; }
    if (cbpc == 2) for (int c = 0; c < 2; c++) for (int b = 0; b < 4; b++) {
        int n = cavlc(s, nc_chroma(d, cur, c, b & 1, b >> 1), 15, sc); if (n < 0) return -1;
        m->nnzc[c][b] = (u8)n;
        for (int i = 0; i < 15; i++) x->ac[c][b][zz[i + 1]] = sc[i];
    }
    return 0;
}
static void chroma_recon(struct h264 *d, int cur, struct mbd *x, int qpc) {
    int mx = cur % d->mbw * 8, my = cur / d->mbw * 8;
    for (int c = 0; c < 2; c++) {
        int *k = x->dcc[c], f[4] = {k[0] + k[1] + k[2] + k[3], k[0] - k[1] + k[2] - k[3], k[0] + k[1] - k[2] - k[3], k[0] - k[1] - k[2] + k[3]};
        u8 *p = (c ? d->buf[d->cur].v : d->buf[d->cur].u) + my * d->cw + mx;
        for (int b = 0; b < 4; b++) {
            int dd[16];
            dd[0] = (f[b] * 16 * norm[qpc % 6][0] * (1 << (qpc / 6))) >> 5;
            for (int i = 1; i < 16; i++) dd[i] = dq(x->ac[c][b][i], qpc, i);
            int any = 0; for (int i = 0; i < 16; i++) any |= dd[i];
            if (any) idct_add(p + (b >> 1) * 4 * d->cw + (b & 1) * 4, d->cw, dd);
        }
    }
}

/* ---- one macroblock (7.3.5) ---- */
static int intra_modes(struct h264 *d, struct bs *s, int cur, i8 *modes) {
    int mx = cur % d->mbw, my = cur / d->mbw;
    for (int b = 0; b < 16; b++) {
        int x4 = blk_x[b], y4 = blk_y[b], prev = u1(s), rem = prev ? 0 : (int)rb(s, 3);
        int ma, mb_, avA = x4 ? 1 : mb_ok(d, mx - 1, my, cur), avB = y4 ? 1 : mb_ok(d, mx, my - 1, cur);
        int pm;
        struct mb *A = x4 ? &d->mbs[cur] : avA ? &d->mbs[cur - 1] : 0, *Bm = y4 ? &d->mbs[cur] : avB ? &d->mbs[cur - d->mbw] : 0;
        if (!avA || !avB || (d->p->cip && ((A && A->type == 3) || (Bm && Bm->type == 3)))) pm = 2;
        else {
            ma = A->type == 1 ? (A == &d->mbs[cur] ? modes[y4 * 4 + x4 - 1] : A->i4[y4 * 4 + 3]) : 2;
            mb_ = Bm->type == 1 ? (Bm == &d->mbs[cur] ? modes[(y4 - 1) * 4 + x4] : Bm->i4[12 + x4]) : 2;
            pm = ma < mb_ ? ma : mb_;
        }
        modes[y4 * 4 + x4] = (i8)(prev ? pm : rem < pm ? rem : rem + 1);
    }
    return 0;
}

static void skip_mv(struct h264 *d, int cur, short *mv) {   /* 8.4.1.1 */
    int mx = cur % d->mbw, my = cur / d->mbw, r;
    if (!mb_ok(d, mx - 1, my, cur) || !mb_ok(d, mx, my - 1, cur)) { mv[0] = mv[1] = 0; return; }
    struct mb *a = blk(d, cur, -1, 0, &r);
    if (a->ref[r] == 0 && !a->mv[r][0] && !a->mv[r][1]) { mv[0] = mv[1] = 0; return; }
    struct mb *b = blk(d, cur, 0, -1, &r);
    if (b->ref[r] == 0 && !b->mv[r][0] && !b->mv[r][1]) { mv[0] = mv[1] = 0; return; }
    mvpred(d, cur, 0, 0, 4, 0, 0, 0, mv);
}

static void mb_begin(struct h264 *d, int cur) {
    struct mb *m = &d->mbs[cur];
    for (int i = 0; i < 16; i++) { m->ref[i] = -2; m->nnz[i] = 0; m->i4[i] = 2; m->mv[i][0] = m->mv[i][1] = 0; m->refbuf[i] = -1; m->mvd[i][0] = m->mvd[i][1] = 0; }
    for (int c = 0; c < 2; c++) for (int i = 0; i < 4; i++) m->nnzc[c][i] = 0;
    m->slice = (u16)d->slice_num; m->dbidc = (i8)d->dbidc; m->dba = (i8)d->dba; m->dbb = (i8)d->dbb;
    m->skip = 0; m->cbp = 0; m->cmode = 0; m->dcf = 0;
}
static int qpc_of(const struct h264 *d, int qp) { int q = qp + d->p->cqp_off; return qpc_tab[q < 0 ? 0 : q > 51 ? 51 : q]; }
static void mb_skip(struct h264 *d, int cur) {
    struct mb *m = &d->mbs[cur]; short mv[2];
    mb_begin(d, cur);
    m->type = 3; m->skip = 1; skip_mv(d, cur, mv);
    set_part(m, 0, 0, 4, 4, 0, d->list[0], mv);
    m->qp = (u8)d->qp; m->qpc = (u8)qpc_of(d, d->qp);
    mc(d, cur, 0, 0, 16, 16, d->list[0], mv, 0);
}
static void mb_pcm(struct h264 *d, struct bs *s, int cur) {
    struct mb *m = &d->mbs[cur];
    s->pos = (s->pos + 7) & ~7u;
    u8 *y = d->buf[d->cur].y + cur / d->mbw * 16 * d->w + cur % d->mbw * 16;
    for (int i = 0; i < 256; i++) y[(i >> 4) * d->w + (i & 15)] = (u8)rb(s, 8);
    for (int c = 0; c < 2; c++) { u8 *p = (c ? d->buf[d->cur].v : d->buf[d->cur].u) + cur / d->mbw * 8 * d->cw + cur % d->mbw * 8; for (int i = 0; i < 64; i++) p[(i >> 3) * d->cw + (i & 7)] = (u8)rb(s, 8); }
    m->type = 4; m->qp = 0; m->qpc = (u8)qpc_of(d, 0); m->cbp = 0x2f; m->dcf = 7;
    for (int i = 0; i < 16; i++) { m->nnz[i] = 16; m->ref[i] = -1; }
    for (int c = 0; c < 2; c++) for (int i = 0; i < 4; i++) m->nnzc[c][i] = 16;
}
static void mb_recon(struct h264 *d, int cur, struct mbd *x) {
    struct mb *m = &d->mbs[cur];
    u8 *base = d->buf[d->cur].y + cur / d->mbw * 16 * d->w + cur % d->mbw * 16;
    if (x->i16) pred_block(d, cur, base, d->w, 16, x->i16mode, 0);
    if (!x->intra) for (int r = 0; r < 16; r++) mc(d, cur, (r & 3) * 4, (r >> 2) * 4, 4, 4, m->refbuf[r], m->mv[r], m->ref[r]);
    else {
        int mx = cur % d->mbw * 8, my = cur / d->mbw * 8;
        pred_block(d, cur, d->buf[d->cur].u + my * d->cw + mx, d->cw, 8, x->cmode, 1);
        pred_block(d, cur, d->buf[d->cur].v + my * d->cw + mx, d->cw, 8, x->cmode, 1);
    }
    luma_recon(d, cur, m->qp, x->i16, x->coef, x->dc, x->intra && !x->i16, x->modes);
    chroma_recon(d, cur, x, m->qpc);
}
static void mbd_clear(struct mbd *x) { unsigned char *p = (unsigned char *)x; for (unsigned i = 0; i < sizeof *x; i++) p[i] = 0; }

static int macroblock(struct h264 *d, struct bs *s, int cur, int skip) {
    if (skip) { mb_skip(d, cur); return 0; }
    struct mb *m = &d->mbs[cur];
    mb_begin(d, cur);
    static struct mbd x; mbd_clear(&x);
    int t = (int)ue(s), intra;
    if (d->stype == 0) { if (t >= 5) t -= 5, intra = 1; else intra = 0; } else intra = 1;
    if (intra && t > 25) return fail(d, "bad mb_type");
    if (intra && t == 25) { mb_pcm(d, s, cur); return 0; }
    x.intra = intra; x.i16 = intra && t >= 1;
    if (intra) {
        m->type = x.i16 ? 2 : 1;
        for (int i = 0; i < 16; i++) m->ref[i] = -1;
        if (!x.i16) { intra_modes(d, s, cur, x.modes); for (int i = 0; i < 16; i++) m->i4[i] = x.modes[i]; }
        else x.i16mode = (t - 1) % 4;
        x.cmode = (int)ue(s); if (x.cmode > 3) return fail(d, "bad chroma mode");
    } else {
        m->type = 3;
        int nparts = t == 0 ? 1 : t < 3 ? 2 : 4, refs[4] = {0, 0, 0, 0};
        int nr = d->nref;
        if (t < 3) {
            for (int i = 0; i < nparts; i++) refs[i] = nr > 1 ? (nr == 2 ? !u1(s) : (int)ue(s)) : 0;
            for (int i = 0; i < nparts; i++) {
                if (refs[i] >= nr) return fail(d, "bad ref");
                int x4 = t == 2 ? i * 2 : 0, y4 = t == 1 ? i * 2 : 0, w4 = t == 2 ? 2 : 4, h4 = t == 1 ? 2 : 4;
                short p[2]; mvpred(d, cur, x4, y4, w4, refs[i], t, i, p);
                short mv[2] = {(short)(p[0] + se(s)), 0}; mv[1] = (short)(p[1] + se(s));
                set_part(m, x4, y4, w4, h4, refs[i], d->list[refs[i]], mv);
            }
        } else {
            int sub[4];
            for (int i = 0; i < 4; i++) { sub[i] = (int)ue(s); if (sub[i] > 3) return fail(d, "bad sub_mb_type"); }
            for (int i = 0; i < 4; i++) refs[i] = t == 4 || nr <= 1 ? 0 : nr == 2 ? !u1(s) : (int)ue(s);
            for (int i = 0; i < 4; i++) {
                if (refs[i] >= nr) return fail(d, "bad ref");
                int bx = (i & 1) * 2, by = (i >> 1) * 2, ns = sub[i] == 0 ? 1 : sub[i] == 3 ? 4 : 2;
                for (int j = 0; j < ns; j++) {
                    int x4 = bx + (sub[i] == 2 || sub[i] == 3 ? (j & 1) : 0), y4 = by + (sub[i] == 1 ? j : sub[i] == 3 ? j >> 1 : 0);
                    int w4 = sub[i] == 0 || sub[i] == 1 ? 2 : 1, h4 = sub[i] == 0 || sub[i] == 2 ? 2 : 1;
                    short p[2]; mvpred(d, cur, x4, y4, w4, refs[i], 0, 0, p);
                    short mv[2] = {(short)(p[0] + se(s)), 0}; mv[1] = (short)(p[1] + se(s));
                    set_part(m, x4, y4, w4, h4, refs[i], d->list[refs[i]], mv);
                }
            }
        }
    }
    if (!x.i16) { u32 k = ue(s); if (k > 47) return fail(d, "bad cbp"); x.cbp = intra ? cbp_intra[k] : cbp_inter[k]; }
    else x.cbp = ((t - 1) / 4 % 3) << 4 | (t >= 13 ? 15 : 0);
    m->cbp = (u8)x.cbp;
    if (x.cbp || x.i16) d->qp = (d->qp + se(s) + 52) % 52;
    m->qp = (u8)d->qp; m->qpc = (u8)qpc_of(d, d->qp);
    if (luma_residual(d, s, cur, x.cbp & 15, x.i16, x.coef, x.dc) < 0) return fail(d, "bad luma residual");
    if (chroma_cavlc(d, s, cur, &x) < 0) return fail(d, "bad chroma residual");
    mb_recon(d, cur, &x);
    return 0;
}

#include "h264_cabac.c"

/* ---- parameter sets (7.3.2) ---- */
static int parse_sps(struct h264 *d, struct bs *s) {
    int prof = (int)rb(s, 8); rb(s, 16);
    u32 id = ue(s); if (id > 31) return fail(d, "bad sps id");
    struct sps *p = &d->sps[id];
    if (prof == 100 || prof == 110 || prof == 122 || prof == 244 || prof == 44 || prof == 83 || prof == 86 || prof == 118 || prof == 128) return fail(d, "High profile is not supported yet");
    p->id = (int)id;
    p->log2_max_fn = (int)ue(s) + 4;
    p->poc_type = (int)ue(s);
    if (p->poc_type == 0) p->log2_max_poc = (int)ue(s) + 4;
    else if (p->poc_type == 1) { p->dpo_zero = (int)u1(s); se(s); se(s); u32 n = ue(s); for (u32 i = 0; i < n && i < 256; i++) se(s); }
    p->num_ref = (int)ue(s); if (p->num_ref > MAXREF) return fail(d, "too many reference frames");
    u1(s);
    p->mbw = (int)ue(s) + 1; p->mbh = (int)ue(s) + 1;
    if (!u1(s)) return fail(d, "interlaced streams are not supported");
    u1(s);
    p->crop[0] = p->crop[1] = p->crop[2] = p->crop[3] = 0;
    if (u1(s)) for (int i = 0; i < 4; i++) p->crop[i] = (int)ue(s) * 2;
    if (p->mbw > 128 || p->mbh > 72) return fail(d, "picture too big");
    p->ok = 1;
    return 0;
}
static int parse_pps(struct h264 *d, struct bs *s) {
    u32 id = ue(s), sid = ue(s); if (id > 255 || sid > 31) return fail(d, "bad pps id");
    struct pps *p = &d->pps[id];
    p->id = (int)id; p->sps = (int)sid;
    p->cabac = (int)u1(s);
    p->bfpo = (int)u1(s);
    if (ue(s)) return fail(d, "slice groups are not supported");
    p->nref_def = (int)ue(s) + 1; ue(s);
    p->wp = (int)u1(s); rb(s, 2);   /* weighted_bipred_idc: B slices are refused anyway */
    p->qp = 26 + se(s); se(s);
    p->cqp_off = se(s);
    p->deblock_ctl = (int)u1(s); p->cip = (int)u1(s);
    if (u1(s)) return fail(d, "redundant pictures are not supported");
    p->ok = 1;
    return 0;
}

static int setup(struct h264 *d, const struct sps *sp) {
    if (d->mbs && d->mbw == sp->mbw && d->mbh == sp->mbh && d->nbuf >= sp->num_ref + 1) return 0;
    if (d->mbs) return fail(d, "the picture size changed mid-stream");
    d->mbw = sp->mbw; d->mbh = sp->mbh; d->w = d->mbw * 16; d->h = d->mbh * 16; d->cw = d->w / 2; d->ch = d->h / 2;
    d->nbuf = sp->num_ref + 1 > 2 ? sp->num_ref + 1 : 2;
    unsigned long fs = (unsigned long)d->w * d->h * 3 / 2;
    for (int i = 0; i < d->nbuf; i++) {
        u8 *p = d->alloc(fs); if (!p) return fail(d, "out of memory");
        d->buf[i].y = p; d->buf[i].u = p + d->w * d->h; d->buf[i].v = d->buf[i].u + d->cw * d->ch; d->buf[i].ref = 0;
    }
    d->mbs = d->alloc(sizeof(struct mb) * (unsigned long)d->mbw * d->mbh);
    return d->mbs ? 0 : fail(d, "out of memory");
}

/* ---- reference lists and marking (8.2.4, 8.2.5) ---- */
static int picnum(const struct h264 *d, const struct frame *f) { return f->fn > d->fn ? f->fn - d->maxfn : f->fn; }
static void build_list(struct h264 *d) {
    int n = 0, idx[NBUF];
    for (int i = 0; i < d->nbuf; i++) if (i != d->cur && d->buf[i].ref == 1) idx[n++] = i;
    for (int i = 0; i < n; i++) for (int j = i + 1; j < n; j++) if (picnum(d, &d->buf[idx[j]]) > picnum(d, &d->buf[idx[i]])) { int t = idx[i]; idx[i] = idx[j]; idx[j] = t; }
    int nl = 0, lidx[NBUF];
    for (int i = 0; i < d->nbuf; i++) if (i != d->cur && d->buf[i].ref == 2) lidx[nl++] = i;
    for (int i = 0; i < nl; i++) for (int j = i + 1; j < nl; j++) if (d->buf[lidx[j]].ltidx < d->buf[lidx[i]].ltidx) { int t = lidx[i]; lidx[i] = lidx[j]; lidx[j] = t; }
    int k = 0;
    for (int i = 0; i < n && k < MAXREF; i++) d->list[k++] = idx[i];
    for (int i = 0; i < nl && k < MAXREF; i++) d->list[k++] = lidx[i];
    for (; k <= MAXREF; k++) d->list[k] = k ? d->list[0] : d->cur;
}
static int modify_list(struct h264 *d, struct bs *s) {
    if (!u1(s)) return 0;
    int pred = d->fn, i = 0;
    for (;;) {
        u32 op = ue(s);
        if (op == 3) break;
        if (op > 3 || i >= d->nref) return fail(d, "bad list modification");
        int target = -1;
        if (op < 2) {
            int diff = (int)ue(s) + 1;
            pred = op == 0 ? pred - diff : pred + diff;
            if (pred < 0) pred += d->maxfn; if (pred >= d->maxfn) pred -= d->maxfn;
            int pn = pred > d->fn ? pred - d->maxfn : pred;
            for (int b = 0; b < d->nbuf; b++) if (b != d->cur && d->buf[b].ref == 1 && picnum(d, &d->buf[b]) == pn) target = b;
        } else {
            int lt = (int)ue(s);
            for (int b = 0; b < d->nbuf; b++) if (b != d->cur && d->buf[b].ref == 2 && d->buf[b].ltidx == lt) target = b;
        }
        if (target < 0) return fail(d, "list modification names a missing picture");
        for (int k = d->nref; k > i; k--) d->list[k] = d->list[k - 1];   /* 8.2.4.3.1 */
        d->list[i++] = target;
        int w = i;
        for (int k = i; k <= d->nref; k++) if (d->list[k] != target) d->list[w++] = d->list[k];
    }
    return 0;
}
static void mark(struct h264 *d) {
    struct frame *c = &d->buf[d->cur];
    if (!d->nal_ref) { c->ref = 0; return; }
    if (d->idr) {
        for (int i = 0; i < d->nbuf; i++) if (i != d->cur) d->buf[i].ref = 0;
        c->ref = d->lt_ref ? 2 : 1; c->ltidx = 0;
        return;
    }
    if (d->mmco_adaptive) {
        for (int k = 0; k < d->nmmco; k++) {
            int op = d->mmco[k][0], a = d->mmco[k][1], b = d->mmco[k][2];
            if (op == 1 || op == 3) {
                int pn = d->fn - (a + 1);
                for (int i = 0; i < d->nbuf; i++) if (i != d->cur && d->buf[i].ref == 1 && picnum(d, &d->buf[i]) == pn) { if (op == 1) d->buf[i].ref = 0; else { d->buf[i].ref = 2; d->buf[i].ltidx = b; } }
            } else if (op == 2) { for (int i = 0; i < d->nbuf; i++) if (d->buf[i].ref == 2 && d->buf[i].ltidx == a) d->buf[i].ref = 0; }
            else if (op == 4) { for (int i = 0; i < d->nbuf; i++) if (d->buf[i].ref == 2 && d->buf[i].ltidx > a - 1) d->buf[i].ref = 0; }
            else if (op == 5) { for (int i = 0; i < d->nbuf; i++) if (i != d->cur) d->buf[i].ref = 0; }
            else if (op == 6) { c->ref = 2; c->ltidx = a; return; }
        }
    } else {
        int n = 0, oldest = -1;
        for (int i = 0; i < d->nbuf; i++) if (i != d->cur && d->buf[i].ref) { n++; if (d->buf[i].ref == 1 && (oldest < 0 || picnum(d, &d->buf[i]) < picnum(d, &d->buf[oldest]))) oldest = i; }
        int max = d->s->num_ref > 0 ? d->s->num_ref : 1;
        if (n >= max && oldest >= 0) d->buf[oldest].ref = 0;
    }
    c->ref = 1;
}

/* ---- slices (7.3.3) ---- */
static int slice(struct h264 *d, struct bs *s, int nal_type, int nal_ref) {
    u32 first = ue(s), st = ue(s) % 5;
    if (st != 0 && st != 2) return fail(d, "only I and P slices are supported");
    u32 pid = ue(s); if (pid > 255 || !d->pps[pid].ok || !d->sps[d->pps[pid].sps].ok) return fail(d, "slice before its parameter sets");
    d->p = &d->pps[pid]; d->s = &d->sps[d->p->sps];
    if (setup(d, d->s) < 0) return -1;
    d->maxfn = 1 << d->s->log2_max_fn;
    int fn = (int)rb(s, d->s->log2_max_fn);
    if (!d->decoded) {   /* the first slice of a picture: a free buffer */
        int f = -1;
        for (int i = 0; i < d->nbuf; i++) if (!d->buf[i].ref) { f = i; break; }
        if (f < 0) { int o = -1; for (int i = 0; i < d->nbuf; i++) if (d->buf[i].ref == 1 && (o < 0 || picnum(d, &d->buf[i]) < picnum(d, &d->buf[o]))) o = i; f = o < 0 ? 0 : o; d->buf[f].ref = 0; }
        d->cur = f;
        for (int i = 0; i < d->mbw * d->mbh; i++) d->mbs[i].type = 0;
        d->slice_num = 0;
        d->buf[d->cur].fn = fn;
    } else d->slice_num++;
    d->stype = (int)st; d->nal_ref = nal_ref; d->idr = nal_type == 5; d->fn = fn;
    if (d->idr) ue(s);
    if (d->s->poc_type == 0) { rb(s, d->s->log2_max_poc); if (d->p->bfpo) se(s); }
    if (d->s->poc_type == 1 && !d->s->dpo_zero) { se(s); if (d->p->bfpo) se(s); }
    d->nref = d->p->nref_def;
    if (st == 0) { if (u1(s)) d->nref = (int)ue(s) + 1; if (d->nref > MAXREF) return fail(d, "too many references"); }
    d->wp_on = 0;
    if (st == 0) { build_list(d); if (modify_list(d, s) < 0) return -1; for (int i = 0; i < d->nref; i++) if (d->list[i] == d->cur || !d->buf[d->list[i]].ref) return fail(d, "a reference picture is missing"); }
    if (st == 0 && d->p->wp) {   /* pred_weight_table (7.3.3.2) */
        d->wp_on = 1; d->lwd = (int)ue(s); d->cwd = (int)ue(s);
        if (d->lwd > 7 || d->cwd > 7) return fail(d, "bad weight denominator");
        for (int i = 0; i < d->nref; i++) {
            d->lw[i][0] = 1 << d->lwd; d->lw[i][1] = 0;
            if (u1(s)) { d->lw[i][0] = se(s); d->lw[i][1] = se(s); }
            for (int c = 0; c < 2; c++) { d->cwt[i][c][0] = 1 << d->cwd; d->cwt[i][c][1] = 0; }
            if (u1(s)) for (int c = 0; c < 2; c++) { d->cwt[i][c][0] = se(s); d->cwt[i][c][1] = se(s); }
        }
    }
    d->mmco_adaptive = 0; d->nmmco = 0; d->lt_ref = 0;
    if (nal_ref) {
        if (d->idr) { u1(s); d->lt_ref = (int)u1(s); }
        else if ((d->mmco_adaptive = (int)u1(s))) {
            for (;;) {
                u32 op = ue(s); if (!op) break;
                if (op > 6 || d->nmmco >= 64) return fail(d, "bad mmco");
                int a = 0, b = 0;
                if (op == 1 || op == 3) a = (int)ue(s);
                if (op == 2) a = (int)ue(s);
                if (op == 3 || op == 6) b = (int)ue(s);
                if (op == 6) a = b;
                if (op == 4) a = (int)ue(s);
                d->mmco[d->nmmco][0] = (int)op; d->mmco[d->nmmco][1] = a; d->mmco[d->nmmco][2] = b; d->nmmco++;
            }
        }
    }
    d->cabac_idc = 0;
    if (d->p->cabac && st != 2) { d->cabac_idc = (int)ue(s); if (d->cabac_idc > 2) return fail(d, "bad cabac_init_idc"); }
    d->qp = d->p->qp + se(s);
    if (d->qp < 0 || d->qp > 51) return fail(d, "bad qp");
    d->dbidc = 0; d->dba = d->dbb = 0;
    if (d->p->deblock_ctl) { d->dbidc = (int)ue(s); if (d->dbidc != 1) { d->dba = se(s) * 2; d->dbb = se(s) * 2; } }
    if (first >= (u32)(d->mbw * d->mbh)) return fail(d, "bad first_mb");
    int cur = (int)first;
    if (d->p->cabac) {   /* 7.3.4 with CABAC: align, start the engine, a skip flag and an end flag per macroblock */
        while (s->pos & 7) u1(s);
        cab_ctx_init(d, d->qp); cab_start(d, s); d->last_dqp = 0;
        for (;;) {
            int skip = 0;
            if (st == 0) {
                d->mbs[cur].slice = (u16)d->slice_num;   /* nbA/nbB compare slices */
                struct mb *A = nbA(d, cur), *B = nbB(d, cur);
                skip = cdec(d, 11 + (A && !A->skip) + (B && !B->skip));
            }
            if (skip) { mb_skip(d, cur); d->last_dqp = 0; }
            else if (macroblock_cabac(d, cur) < 0) return -1;
            cur++;
            if (cterm(d) || cur >= d->mbw * d->mbh) break;
        }
    } else for (;;) {
        if (st == 0) {
            u32 run = ue(s);
            while (run-- && cur < d->mbw * d->mbh) { if (macroblock(d, s, cur, 1) < 0) return -1; cur++; }
            if (cur >= d->mbw * d->mbh || !more_data(s)) break;
        }
        if (macroblock(d, s, cur, 0) < 0) return -1;
        cur++;
        if (!more_data(s) || cur >= d->mbw * d->mbh) break;
    }
    d->decoded = 1;
    return 0;
}

/* ---- deblocking (8.7) ---- */
static const u8 alpha_t[52] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,4,4,5,6,7,8,9,10,12,13,15,17,20,22,25,28,32,36,40,45,50,56,63,71,80,90,101,113,127,144,162,182,203,226,255,255};
static const u8 beta_t[52] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,2,2,2,3,3,3,3,4,4,4,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13,14,14,15,15,16,16,17,17,18,18};
static const u8 tc0_t[52][3] = {{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0},
    {0,0,1},{0,0,1},{0,0,1},{0,0,1},{0,1,1},{0,1,1},{1,1,1},{1,1,1},{1,1,1},{1,1,1},{1,1,2},{1,1,2},{1,1,2},{1,1,2},{1,2,3},{1,2,3},{2,2,3},{2,2,4},{2,3,4},{2,3,4},
    {3,3,5},{3,4,6},{3,4,6},{4,5,7},{4,5,8},{4,6,9},{5,7,10},{6,8,11},{6,8,13},{7,10,14},{8,11,16},{9,12,18},{10,13,20},{11,15,23},{13,17,25}};
static int iabs(int v) { return v < 0 ? -v : v; }
/* one line of samples across an edge: p[-k*step], q[k*step] around pos */
static void filt(u8 *q0p, int step, int bs, int qpav, int fa, int fb, int chroma) {
    int ia = clip3(0, 51, qpav + fa), ib = clip3(0, 51, qpav + fb), al = alpha_t[ia], be = beta_t[ib];
    int p0 = q0p[-step], p1 = q0p[-2 * step], q0 = q0p[0], q1 = q0p[step];
    if (!(iabs(p0 - q0) < al && iabs(p1 - p0) < be && iabs(q1 - q0) < be)) return;
    int p2 = chroma ? 0 : q0p[-3 * step], q2 = chroma ? 0 : q0p[2 * step];
    if (bs < 4) {
        int tc0 = tc0_t[ia][bs - 1], ap = iabs(p2 - p0), aq = iabs(q2 - q0);
        int tc = chroma ? tc0 + 1 : tc0 + (ap < be) + (aq < be);
        int dl = clip3(-tc, tc, ((q0 - p0) * 4 + (p1 - q1) + 4) >> 3);
        q0p[-step] = clip1(p0 + dl); q0p[0] = clip1(q0 - dl);
        if (!chroma) {
            if (ap < be) q0p[-2 * step] = (u8)(p1 + clip3(-tc0, tc0, (p2 + ((p0 + q0 + 1) >> 1) - (p1 << 1)) >> 1));
            if (aq < be) q0p[step] = (u8)(q1 + clip3(-tc0, tc0, (q2 + ((p0 + q0 + 1) >> 1) - (q1 << 1)) >> 1));
        }
        return;
    }
    if (chroma) { q0p[-step] = (u8)((2 * p1 + p0 + q1 + 2) >> 2); q0p[0] = (u8)((2 * q1 + q0 + p1 + 2) >> 2); return; }
    int p3 = q0p[-4 * step], q3 = q0p[3 * step], ap = iabs(p2 - p0), aq = iabs(q2 - q0), small = iabs(p0 - q0) < ((al >> 2) + 2);
    if (ap < be && small) { q0p[-step] = (u8)((p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3); q0p[-2 * step] = (u8)((p2 + p1 + p0 + q0 + 2) >> 2); q0p[-3 * step] = (u8)((2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3); }
    else q0p[-step] = (u8)((2 * p1 + p0 + q1 + 2) >> 2);
    if (aq < be && small) { q0p[0] = (u8)((p1 + 2 * p0 + 2 * q0 + 2 * q1 + q2 + 4) >> 3); q0p[step] = (u8)((p0 + q0 + q1 + q2 + 2) >> 2); q0p[2 * step] = (u8)((2 * q3 + 3 * q2 + q1 + q0 + p0 + 4) >> 3); }
    else q0p[0] = (u8)((2 * q1 + q0 + p1 + 2) >> 2);
}
static int bstr(const struct h264 *d, const struct mb *p, int rp, const struct mb *q, int rq, int mbedge) {
    int pi = p->type != 3, qi = q->type != 3;
    if (pi || qi) return mbedge ? 4 : 3;
    if (p->nnz[rp] || q->nnz[rq]) return 2;
    if (p->refbuf[rp] != q->refbuf[rq]) return 1;
    (void)d;
    return iabs(p->mv[rp][0] - q->mv[rq][0]) >= 4 || iabs(p->mv[rp][1] - q->mv[rq][1]) >= 4;
}
static void deblock(struct h264 *d) {
    struct frame *f = &d->buf[d->cur];
    for (int cur = 0; cur < d->mbw * d->mbh; cur++) {
        struct mb *m = &d->mbs[cur];
        if (m->dbidc == 1) continue;
        int mx = cur % d->mbw, my = cur / d->mbw;
        for (int dir = 0; dir < 2; dir++) {   /* 0: vertical edges, 1: horizontal */
            for (int e = 0; e < 4; e++) {
                struct mb *n = m;
                if (!e) {
                    int nx = mx - !dir, ny = my - dir;
                    if (nx < 0 || ny < 0) continue;
                    n = &d->mbs[ny * d->mbw + nx];
                    if (m->dbidc == 2 && n->slice != m->slice) continue;
                }
                int bs[4];
                for (int k = 0; k < 4; k++) {
                    int rq = dir ? e * 4 + k : k * 4 + e, rp = dir ? (e ? (e - 1) * 4 + k : 12 + k) : (e ? k * 4 + e - 1 : k * 4 + 3);
                    bs[k] = bstr(d, n, rp, m, rq, !e);
                }
                int qpav = (n->qp + m->qp + 1) >> 1;
                for (int i = 0; i < 16; i++) {
                    if (!bs[i >> 2]) continue;
                    u8 *q = dir ? f->y + (my * 16 + e * 4) * d->w + mx * 16 + i : f->y + (my * 16 + i) * d->w + mx * 16 + e * 4;
                    filt(q, dir ? d->w : 1, bs[i >> 2], qpav, m->dba, m->dbb, 0);
                }
                if (e & 1) continue;   /* chroma edges at luma 0 and 8 */
                int cq = (n->qpc + m->qpc + 1) >> 1;
                for (int c = 0; c < 2; c++) {
                    u8 *pl = c ? f->v : f->u;
                    for (int i = 0; i < 8; i++) {
                        int b = bs[i >> 1];
                        if (!b) continue;
                        u8 *q = dir ? pl + (my * 8 + e * 2) * d->cw + mx * 8 + i : pl + (my * 8 + i) * d->cw + mx * 8 + e * 2;
                        filt(q, dir ? d->cw : 1, b, cq, m->dba, m->dbb, 1);
                    }
                }
            }
        }
    }
}

/* ---- entry points ---- */
struct h264 *h264_new(void *(*alloc)(unsigned long)) {
    struct h264 *d = alloc(sizeof *d);
    if (!d) return 0;
    for (unsigned long i = 0; i < sizeof *d; i++) ((u8 *)d)[i] = 0;
    d->alloc = alloc;
    return d;
}

int h264_nal(struct h264 *d, const u8 *nal, unsigned len) {
    if (len < 2) return 0;
    int type = nal[0] & 31, ref = (nal[0] >> 5) & 3;
    if (type != 1 && type != 5 && type != 7 && type != 8) return 0;
    if (len > d->rbsp_max) {
        unsigned n = len < (64u << 10) ? 64u << 10 : len * 2;
        if (!(d->rbsp = d->alloc(n))) return fail(d, "out of memory");
        d->rbsp_max = n;
    }
    unsigned n = 0, zeros = 0;   /* 7.4.1: drop emulation prevention bytes */
    for (unsigned i = 1; i < len; i++) {
        if (zeros >= 2 && nal[i] == 3) { zeros = 0; continue; }
        zeros = nal[i] ? 0 : zeros + 1;
        d->rbsp[n++] = nal[i];
    }
    struct bs s = {d->rbsp, n, 0, 0};
    unsigned last = n;
    while (last && !d->rbsp[last - 1]) last--;
    if (!last) return fail(d, "empty NAL unit");
    u8 b = d->rbsp[last - 1]; int bit = 0; while (!(b & (1 << bit))) bit++;
    s.end = (last - 1) * 8 + (7 - bit);
    d->err = 0;
    if (type == 7) return parse_sps(d, &s);
    if (type == 8) return parse_pps(d, &s);
    return slice(d, &s, type, ref);
}

int h264_end(struct h264 *d, struct h264_pic *p) {
    if (!d->decoded) return 0;
    deblock(d);
    mark(d);
    d->decoded = 0;
    const struct frame *f = &d->buf[d->cur];
    p->y = f->y; p->u = f->u; p->v = f->v; p->stride = (unsigned)d->w; p->cstride = (unsigned)d->cw;
    p->x = (unsigned)d->s->crop[0]; p->y0 = (unsigned)d->s->crop[2];
    p->w = (unsigned)(d->w - d->s->crop[0] - d->s->crop[1]); p->h = (unsigned)(d->h - d->s->crop[2] - d->s->crop[3]);
    return 1;
}
