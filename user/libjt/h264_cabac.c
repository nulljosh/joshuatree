/* CABAC (ITU-T H.264 clause 9.3) for h264.c, included into it: the arithmetic decoder, its contexts, and the macroblock
   layer's syntax elements as CABAC codes them. Frames only, 4:2:0, no 8x8 transform. */

static const short cab_mn[460][4][2] = {
#include "h264_cabac.inc"
};
static const u8 lps_t[64][4] = {   /* Table 9-44 */
    {128,176,208,240},{128,167,197,227},{128,158,187,216},{123,150,178,205},{116,142,169,195},{111,135,160,185},{105,128,152,175},{100,122,144,166},
    {95,116,137,158},{90,110,130,150},{85,104,123,142},{81,99,117,135},{77,94,111,128},{73,89,105,122},{69,85,100,116},{66,80,95,110},
    {62,76,90,104},{59,72,86,99},{56,69,81,94},{53,65,77,89},{51,62,73,85},{48,59,69,80},{46,56,66,76},{43,53,63,72},
    {41,50,59,69},{39,48,56,65},{37,45,54,62},{35,43,51,59},{33,41,48,56},{32,39,46,53},{30,37,43,50},{29,35,41,48},
    {27,33,39,45},{26,31,37,43},{24,30,35,41},{23,28,33,39},{22,27,32,37},{21,26,30,35},{20,24,29,33},{19,23,27,31},
    {18,22,26,30},{17,21,25,28},{16,20,23,27},{15,19,22,25},{14,18,21,24},{14,17,20,23},{13,16,19,22},{12,15,18,21},
    {12,14,17,20},{11,14,16,19},{11,13,15,18},{10,12,15,17},{10,12,14,16},{9,11,13,15},{9,11,12,14},{8,10,12,14},
    {8,9,11,13},{7,9,11,12},{7,9,10,12},{7,8,10,11},{6,8,9,11},{6,7,9,10},{6,7,8,9},{2,2,2,2},
};
static const u8 trans_lps[64] = {0,0,1,2,2,4,4,5,6,7,8,9,9,11,11,12,13,13,15,15,16,16,18,18,19,19,21,21,22,22,23,24,
    24,25,26,26,27,27,28,29,29,30,30,30,31,32,32,33,33,33,34,34,35,35,35,36,36,36,37,37,37,38,38,63};

static void cab_ctx_init(struct h264 *d, int sqp) {   /* 9.3.1.1 */
    int col = d->stype == 2 ? 0 : 1 + d->cabac_idc;
    for (int c = 0; c < 460; c++) {
        int pre = clip3(1, 126, ((cab_mn[c][col][0] * clip3(0, 51, sqp)) >> 4) + cab_mn[c][col][1]);
        d->cst[c] = (u8)(pre <= 63 ? (63 - pre) << 1 : (pre - 64) << 1 | 1);
    }
}
static void cab_start(struct h264 *d, struct bs *s) { d->cs = s; d->crng = 510; d->coff = rb(s, 9); }
static int cdec(struct h264 *d, int ctx) {   /* 9.3.3.2.1 */
    u8 *p = &d->cst[ctx]; int st = *p >> 1, mps = *p & 1, bin;
    u32 lps = lps_t[st][(d->crng >> 6) & 3];
    d->crng -= lps;
    if (d->coff >= d->crng) { bin = !mps; d->coff -= d->crng; d->crng = lps; if (!st) mps ^= 1; st = trans_lps[st]; }
    else { bin = mps; if (st < 62) st++; }
    *p = (u8)(st << 1 | mps);
    while (d->crng < 256) { d->crng <<= 1; d->coff = d->coff << 1 | u1(d->cs); }
    return bin;
}
static int cbyp(struct h264 *d) { d->coff = d->coff << 1 | u1(d->cs); if (d->coff >= d->crng) { d->coff -= d->crng; return 1; } return 0; }
static int cterm(struct h264 *d) {
    d->crng -= 2;
    if (d->coff >= d->crng) return 1;
    while (d->crng < 256) { d->crng <<= 1; d->coff = d->coff << 1 | u1(d->cs); }
    return 0;
}

/* neighbouring macroblocks A (left) and B (above), or 0 */
static struct mb *nbA(struct h264 *d, int cur) { int mx = cur % d->mbw, my = cur / d->mbw; return mb_ok(d, mx - 1, my, cur) ? &d->mbs[cur - 1] : 0; }
static struct mb *nbB(struct h264 *d, int cur) { int mx = cur % d->mbw, my = cur / d->mbw; return mb_ok(d, mx, my - 1, cur) ? &d->mbs[cur - d->mbw] : 0; }

static int c_mbtype_i(struct h264 *d, int base, int cur, int in_p) {   /* Table 9-36: 0 I_NxN, 1..24 I_16x16, 25 I_PCM */
    if (!in_p) {
        struct mb *a = nbA(d, cur), *b = nbB(d, cur);
        if (!cdec(d, base + (a && a->type != 1) + (b && b->type != 1))) return 0;
    } else if (!cdec(d, base)) return 0;
    if (cterm(d)) return 25;
    int l = cdec(d, base + (in_p ? 1 : 3)), cb = cdec(d, base + (in_p ? 2 : 4)), c = 0;
    if (cb) c = cdec(d, base + (in_p ? 2 : 5)) ? 2 : 1;
    int p1 = cdec(d, base + (in_p ? 3 : 6)), p0 = cdec(d, base + (in_p ? 3 : 7));
    return 1 + (p1 << 1 | p0) + 4 * c + 12 * l;
}
static int c_ref(struct h264 *d, int cur, int x4, int y4) {   /* 9.3.3.1.1.6 */
    int inc = 0, r; struct mb *m;
    if ((m = blk(d, cur, x4 - 1, y4, &r)) && !m->skip && m->type == 3 && m->ref[r] > 0) inc += 1;
    if ((m = blk(d, cur, x4, y4 - 1, &r)) && !m->skip && m->type == 3 && m->ref[r] > 0) inc += 2;
    int v = 0;
    if (!cdec(d, 54 + inc)) return 0;
    v = 1;
    while (cdec(d, v == 1 ? 58 : 59)) if (++v > 31) break;
    return v;
}
static int c_mvd(struct h264 *d, int cur, int x4, int y4, int comp) {   /* 9.3.3.1.1.7, UEG3 */
    int sum = 0, r; struct mb *m;
    if ((m = blk(d, cur, x4 - 1, y4, &r)) && m->type == 3) sum += m->mvd[r][comp];
    if ((m = blk(d, cur, x4, y4 - 1, &r)) && m->type == 3) sum += m->mvd[r][comp];
    int base = comp ? 47 : 40, v = 0;
    if (!cdec(d, base + (sum < 3 ? 0 : sum > 32 ? 2 : 1))) return 0;
    v = 1;
    while (v < 9 && cdec(d, base + (v == 1 ? 3 : v == 2 ? 4 : v == 3 ? 5 : 6))) v++;
    if (v >= 9) { int k = 3; while (cbyp(d)) { v += 1 << k; if (++k > 24) break; } while (k--) v += cbyp(d) << k; }
    return cbyp(d) ? -v : v;
}
static void set_mvd(struct mb *m, int x4, int y4, int w4, int h4, int dx, int dy) {
    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    for (int y = y4; y < y4 + h4; y++) for (int x = x4; x < x4 + w4; x++) { m->mvd[y * 4 + x][0] = (u8)(ax > 127 ? 127 : ax); m->mvd[y * 4 + x][1] = (u8)(ay > 127 ? 127 : ay); }
}

/* coded_block_flag's context increment (9.3.3.1.1.9): cat 0 luma DC, 1/2 luma 4x4 at (x4, y4), 3 chroma DC, 4 chroma AC */
static int cbf_term(struct h264 *d, int cur, struct mb *n, int intra, int avail_blk, int flag) {
    (void)d; (void)cur;
    if (!n) return intra;
    if (n->type == 4) return 1;
    if (!avail_blk) return 0;
    return flag;
}
static int cbf_inc(struct h264 *d, int cur, int cat, int x4, int y4, int c, int intra) {
    struct mb *cm = &d->mbs[cur];
    int t[2];
    for (int k = 0; k < 2; k++) {
        int dx = k ? 0 : -1, dy = k ? -1 : 0;
        if (cat == 0 || cat == 3) {
            struct mb *n = k ? nbB(d, cur) : nbA(d, cur);
            int avail = n && !n->skip && (cat == 0 ? n->type == 2 : (n->cbp >> 4) != 0);
            t[k] = cbf_term(d, cur, n, intra, avail, n ? (n->dcf >> (cat == 0 ? 0 : 1 + c)) & 1 : 0);
        } else if (cat == 4) {
            int bx = x4 + dx, by = y4 + dy;   /* chroma 4x4 coordinates, 0..1 */
            struct mb *n = bx >= 0 && by >= 0 ? cm : bx < 0 ? nbA(d, cur) : nbB(d, cur);
            int r = (by & 1) * 2 + (bx & 1);
            int avail = n && !n->skip && (n->cbp >> 4) == 2;
            t[k] = cbf_term(d, cur, n, intra, avail, n ? n->nnzc[c][r] != 0 : 0);
        } else {
            int r; struct mb *n = blk(d, cur, x4 + dx, y4 + dy, &r);
            int avail = n && !n->skip && ((n->cbp >> (((r >> 2) / 2) * 2 + (r & 3) / 2)) & 1);
            t[k] = cbf_term(d, cur, n, intra, avail, n ? n->nnz[r] != 0 : 0);
        }
    }
    return t[0] + 2 * t[1];
}
/* residual_block_cabac (7.3.5.3.3): coefficients in scan order, the number of non-zero ones back */
static int cres(struct h264 *d, int cat, int max, int inc, int *out) {
    static const u8 cbf_off[5] = {0, 4, 8, 12, 16}, sig_off[5] = {0, 15, 29, 44, 47}, abs_off[5] = {0, 10, 20, 30, 39};
    for (int i = 0; i < max; i++) out[i] = 0;
    if (!cdec(d, 85 + cbf_off[cat] + inc)) return 0;
    u8 sig[16] = {0}; int last = -1;
    for (int i = 0; i < max - 1; i++) {
        int ci = cat == 3 ? (i < 2 ? i : 2) : i;
        if (cdec(d, 105 + sig_off[cat] + ci)) { sig[i] = 1; if (cdec(d, 166 + sig_off[cat] + ci)) { last = i; break; } }
    }
    if (last < 0) { last = max - 1; sig[last] = 1; }
    int eq1 = 0, gt1 = 0, n = 0;
    for (int i = last; i >= 0; i--) {
        if (!sig[i]) continue;
        int v = 0;
        if (cdec(d, 227 + abs_off[cat] + (gt1 ? 0 : (1 + eq1 < 4 ? 1 + eq1 : 4)))) {
            int ck = 227 + abs_off[cat] + 5 + (gt1 < 4 - (cat == 3) ? gt1 : 4 - (cat == 3));
            v = 1;
            while (v < 14 && cdec(d, ck)) v++;
            if (v == 14) { int k = 0; while (cbyp(d)) { v += 1 << k; if (++k > 24) break; } while (k--) v += cbyp(d) << k; }
        }
        int lv = v + 1;
        if (lv == 1) eq1++; else gt1++;
        out[i] = cbyp(d) ? -lv : lv;
        n++;
    }
    return n;
}

/* the 8x8 block's coefficients (ctxBlockCat 5, frame): no coded_block_flag in 4:2:0, Table 9-43's context map */
static const u8 sig8[63] = {0,1,2,3,4,5,5,4,4,3,3,4,4,4,5,5,4,4,4,4,3,3,6,7,7,7,8,9,10,9,8,7,7,6,11,12,13,11,6,7,8,9,14,10,9,8,6,11,
    12,13,11,6,9,14,10,9,11,12,13,11,14,10,12};
static const u8 last8[63] = {0,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,2,3,3,3,3,3,3,3,3,4,4,4,4,4,4,4,4,5,5,5,5,6,6,6,6,7,7,7,7,8,8,8};
static int cres8(struct h264 *d, int *out) {
    for (int i = 0; i < 64; i++) out[i] = 0;
    u8 sig[64] = {0}; int last = -1;
    for (int i = 0; i < 63; i++)
        if (cdec(d, 402 + sig8[i])) { sig[i] = 1; if (cdec(d, 417 + last8[i])) { last = i; break; } }
    if (last < 0) { last = 63; sig[63] = 1; }
    int eq1 = 0, gt1 = 0, n = 0;
    for (int i = last; i >= 0; i--) {
        if (!sig[i]) continue;
        int v = 0;
        if (cdec(d, 426 + (gt1 ? 0 : (1 + eq1 < 4 ? 1 + eq1 : 4)))) {
            int ck = 426 + 5 + (gt1 < 4 ? gt1 : 4);
            v = 1;
            while (v < 14 && cdec(d, ck)) v++;
            if (v == 14) { int k = 0; while (cbyp(d)) { v += 1 << k; if (++k > 24) break; } while (k--) v += cbyp(d) << k; }
        }
        int lv = v + 1;
        if (lv == 1) eq1++; else gt1++;
        out[i] = cbyp(d) ? -lv : lv;
        n++;
    }
    return n;
}
static int c_t8(struct h264 *d, int cur) {   /* transform_size_8x8_flag, ctx 399 + neighbours that used it */
    struct mb *A = nbA(d, cur), *B = nbB(d, cur);
    return cdec(d, 399 + (A && A->t8) + (B && B->t8));
}

static int c_intra_modes(struct h264 *d, int cur, i8 *modes) {   /* the same prediction as intra_modes, CABAC bins */
    int mx = cur % d->mbw, my = cur / d->mbw;
    for (int b = 0; b < 16; b++) {
        int x4 = blk_x[b], y4 = blk_y[b], prev = cdec(d, 68), rem = 0;
        if (!prev) { rem = cdec(d, 69); rem |= cdec(d, 69) << 1; rem |= cdec(d, 69) << 2; }
        int avA = x4 ? 1 : mb_ok(d, mx - 1, my, cur), avB = y4 ? 1 : mb_ok(d, mx, my - 1, cur), pm;
        struct mb *A = x4 ? &d->mbs[cur] : avA ? &d->mbs[cur - 1] : 0, *Bm = y4 ? &d->mbs[cur] : avB ? &d->mbs[cur - d->mbw] : 0;
        if (!avA || !avB || (d->p->cip && ((A && A->type == 3) || (Bm && Bm->type == 3)))) pm = 2;
        else {
            int ma = A->type == 1 ? (A == &d->mbs[cur] ? modes[y4 * 4 + x4 - 1] : A->i4[y4 * 4 + 3]) : 2;
            int mb_ = Bm->type == 1 ? (Bm == &d->mbs[cur] ? modes[(y4 - 1) * 4 + x4] : Bm->i4[12 + x4]) : 2;
            pm = ma < mb_ ? ma : mb_;
        }
        modes[y4 * 4 + x4] = (i8)(prev ? pm : rem < pm ? rem : rem + 1);
    }
    return 0;
}

static int c_cbp(struct h264 *d, int cur) {   /* 9.3.3.1.1.4 */
    struct mb *A = nbA(d, cur), *B = nbB(d, cur);
    int cbp = 0;
    for (int b8 = 0; b8 < 4; b8++) {
        int ta, tb;
        if (b8 & 1) ta = !((cbp >> (b8 - 1)) & 1);
        else ta = !A || A->type == 4 ? 0 : !A->skip && ((A->cbp >> (b8 + 1)) & 1) ? 0 : 1;
        if (b8 >= 2) tb = !((cbp >> (b8 - 2)) & 1);
        else tb = !B || B->type == 4 ? 0 : !B->skip && ((B->cbp >> (b8 + 2)) & 1) ? 0 : 1;
        cbp |= cdec(d, 73 + ta + 2 * tb) << b8;
    }
    int a0 = A && (A->type == 4 || (!A->skip && (A->cbp >> 4))), b0 = B && (B->type == 4 || (!B->skip && (B->cbp >> 4)));
    if (cdec(d, 77 + a0 + 2 * b0)) {
        int a1 = A && (A->type == 4 || (!A->skip && (A->cbp >> 4) == 2)), b1 = B && (B->type == 4 || (!B->skip && (B->cbp >> 4) == 2));
        cbp |= (cdec(d, 81 + a1 + 2 * b1) ? 2 : 1) << 4;
    }
    return cbp;
}

static int macroblock_cabac(struct h264 *d, int cur) {
    struct mb *m = &d->mbs[cur];
    mb_begin(d, cur);
    static struct mbd x; mbd_clear(&x);
    int t, intra;
    if (d->stype == 2) { t = c_mbtype_i(d, 3, cur, 0); intra = 1; }
    else if (cdec(d, 14)) { t = c_mbtype_i(d, 17, cur, 1); intra = 1; }
    else if (!cdec(d, 15)) { t = cdec(d, 16) ? 3 : 0; intra = 0; }
    else { t = cdec(d, 17) ? 1 : 2; intra = 0; }
    if (intra && t == 25) { mb_pcm(d, d->cs, cur); cab_start(d, d->cs); d->last_dqp = 0; return 0; }
    x.intra = intra; x.i16 = intra && t >= 1;
    if (intra) {
        m->type = x.i16 ? 2 : 1;
        for (int i = 0; i < 16; i++) m->ref[i] = -1;
        if (!x.i16 && d->p->t8mode) m->t8 = (u8)c_t8(d, cur);
        if (!x.i16 && m->t8) for (int b8 = 0; b8 < 4; b8++) {
            int prev = cdec(d, 68), rem = 0;
            if (!prev) { rem = cdec(d, 69); rem |= cdec(d, 69) << 1; rem |= cdec(d, 69) << 2; }
            int pm = pred_mode8(d, cur, b8, x.modes);
            set_mode8(x.modes, b8, prev ? pm : rem < pm ? rem : rem + 1);
        }
        else if (!x.i16) c_intra_modes(d, cur, x.modes);
        if (!x.i16) for (int i = 0; i < 16; i++) m->i4[i] = x.modes[i];
        if (x.i16) x.i16mode = (t - 1) % 4;
        struct mb *A = nbA(d, cur), *B = nbB(d, cur);
        int inc = (A && (A->type == 1 || A->type == 2) && A->cmode) + (B && (B->type == 1 || B->type == 2) && B->cmode);
        x.cmode = 0;
        if (cdec(d, 64 + inc)) { x.cmode = 1; if (cdec(d, 67)) { x.cmode = 2; if (cdec(d, 67)) x.cmode = 3; } }
        m->cmode = (u8)x.cmode;
    } else {
        m->type = 3; x.no_sub8 = 1;
        int nr = d->nref;
        if (t < 3) {
            int np = t == 0 ? 1 : 2, refs[2] = {0, 0};
            for (int i = 0; i < np; i++) {
                int x4 = t == 2 ? i * 2 : 0, y4 = t == 1 ? i * 2 : 0, w4 = t == 2 ? 2 : 4, h4 = t == 1 ? 2 : 4;
                refs[i] = nr > 1 ? c_ref(d, cur, x4, y4) : 0;
                if (refs[i] >= nr) return fail(d, "bad ref");
                for (int y = y4; y < y4 + h4; y++) for (int xx = x4; xx < x4 + w4; xx++) m->ref[y * 4 + xx] = (i8)refs[i];
            }
            for (int i = 0; i < np; i++) {
                int x4 = t == 2 ? i * 2 : 0, y4 = t == 1 ? i * 2 : 0, w4 = t == 2 ? 2 : 4, h4 = t == 1 ? 2 : 4;
                short p[2]; mvpred(d, cur, x4, y4, w4, refs[i], t, i, p);
                int dx = c_mvd(d, cur, x4, y4, 0), dy = c_mvd(d, cur, x4, y4, 1);
                short mv[2] = {(short)(p[0] + dx), (short)(p[1] + dy)};
                set_part(m, x4, y4, w4, h4, refs[i], d->list[refs[i]], mv);
                set_mvd(m, x4, y4, w4, h4, dx, dy);
            }
        } else {
            int sub[4], refs[4];
            for (int i = 0; i < 4; i++) { if (cdec(d, 21)) sub[i] = 0; else if (!cdec(d, 22)) sub[i] = 1; else sub[i] = cdec(d, 23) ? 2 : 3; }
            x.no_sub8 = !sub[0] && !sub[1] && !sub[2] && !sub[3];
            for (int i = 0; i < 4; i++) {
                int bx = (i & 1) * 2, by = (i >> 1) * 2;
                refs[i] = nr > 1 ? c_ref(d, cur, bx, by) : 0;
                if (refs[i] >= nr) return fail(d, "bad ref");
                for (int y = by; y < by + 2; y++) for (int xx = bx; xx < bx + 2; xx++) m->ref[y * 4 + xx] = (i8)refs[i];
            }
            for (int i = 0; i < 4; i++) {
                int bx = (i & 1) * 2, by = (i >> 1) * 2, ns = sub[i] == 0 ? 1 : sub[i] == 3 ? 4 : 2;
                for (int j = 0; j < ns; j++) {
                    int x4 = bx + (sub[i] == 2 || sub[i] == 3 ? (j & 1) : 0), y4 = by + (sub[i] == 1 ? j : sub[i] == 3 ? j >> 1 : 0);
                    int w4 = sub[i] == 0 || sub[i] == 1 ? 2 : 1, h4 = sub[i] == 0 || sub[i] == 2 ? 2 : 1;
                    short p[2]; mvpred(d, cur, x4, y4, w4, refs[i], 0, 0, p);
                    int dx = c_mvd(d, cur, x4, y4, 0), dy = c_mvd(d, cur, x4, y4, 1);
                    short mv[2] = {(short)(p[0] + dx), (short)(p[1] + dy)};
                    set_part(m, x4, y4, w4, h4, refs[i], d->list[refs[i]], mv);
                    set_mvd(m, x4, y4, w4, h4, dx, dy);
                }
            }
        }
    }
    if (!x.i16) x.cbp = c_cbp(d, cur);
    else x.cbp = ((t - 1) / 4 % 3) << 4 | (t >= 13 ? 15 : 0);
    m->cbp = (u8)x.cbp;
    if (!intra && (x.cbp & 15) && d->p->t8mode && x.no_sub8) m->t8 = (u8)c_t8(d, cur);
    int dqp = 0;
    if (x.cbp || x.i16) {   /* mb_qp_delta: unary of the mapped value (9.3.2.7) */
        int k = 0;
        if (cdec(d, 60 + (d->last_dqp != 0))) { k = 1; while (cdec(d, k == 1 ? 62 : 63)) if (++k > 104) break; }
        dqp = k & 1 ? (k + 1) / 2 : -(k / 2);
        d->qp = (d->qp + dqp + 52) % 52;
    }
    d->last_dqp = dqp;
    m->qp = (u8)d->qp; m->qpc = (u8)qpc_of(d, d->qp); m->qpc2 = (u8)qpc2_of(d, d->qp);
    int sc[16];
    if (x.i16) {
        int n = cres(d, 0, 16, cbf_inc(d, cur, 0, 0, 0, 0, 1), sc);
        if (n) m->dcf |= 1;
        for (int i = 0; i < 16; i++) x.dc[zz[i]] = sc[i];
    }
    if (m->t8) for (int b8 = 0; b8 < 4; b8++) {   /* 8x8 blocks: coef holds 4 blocks of 64, raster */
        if (!((x.cbp & 15) >> b8 & 1)) continue;
        int sc8[64], n = cres8(d, sc8);
        for (int i = 0; i < 64; i++) x.coef[b8 * 64 + zz8[i]] = sc8[i];
        for (int k = 0; k < 4; k++) { int bb = b8 * 4 + k; m->nnz[blk_y[bb] * 4 + blk_x[bb]] = (u8)n; }
    }
    else for (int b = 0; b < 16; b++) {
        int x4 = blk_x[b], y4 = blk_y[b], r = y4 * 4 + x4, *o = x.coef + r * 16;
        if (!((x.cbp & 15) >> (b >> 2) & 1)) continue;
        int n = cres(d, x.i16 ? 1 : 2, x.i16 ? 15 : 16, cbf_inc(d, cur, x.i16 ? 1 : 2, x4, y4, 0, intra), sc);
        m->nnz[r] = (u8)n;
        if (x.i16) for (int i = 0; i < 15; i++) o[zz[i + 1]] = sc[i];
        else for (int i = 0; i < 16; i++) o[zz[i]] = sc[i];
    }
    if (x.cbp >> 4) for (int c = 0; c < 2; c++) { if (cres(d, 3, 4, cbf_inc(d, cur, 3, 0, 0, c, intra), x.dcc[c])) m->dcf |= (u8)(2 << c); }
    if ((x.cbp >> 4) == 2) for (int c = 0; c < 2; c++) for (int b = 0; b < 4; b++) {
        int n = cres(d, 4, 15, cbf_inc(d, cur, 4, b & 1, b >> 1, c, intra), sc);
        m->nnzc[c][b] = (u8)n;
        for (int i = 0; i < 15; i++) x.ac[c][b][zz[i + 1]] = sc[i];
    }
    mb_recon(d, cur, &x);
    return 0;
}
