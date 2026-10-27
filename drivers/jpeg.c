/* Minimal baseline JPEG decoder: see jpeg.h for the exact scope. Marker
   walk, canonical Huffman decode of DC/AC coefficients, zigzag dequant,
   a real separable fixed-point IDCT, YCbCr->RGB, nearest-neighbor chroma
   upsampling. No recursion, no dynamic tables beyond one kmalloc per
   component plane plus the final RGB/gray output buffer. */
#include "jpeg.h"
#ifdef JPEG_USER
/* Built into user/libjt (user/libjt/jpeg.c includes this file): the same decoder
   in ring 3, no libc, planes from libjt's malloc arena. */
#include "../user/libjt/string.h"
#include "../user/libjt/stdlib.h"
#define JPEG_STATIC __attribute__((section(".data"))) /* flat ring-3 images have no .bss */
#define kmalloc malloc
#define kfree free
#else
#include "kheap.h"
#include "libc.h"
#define JPEG_STATIC
#endif

typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef int i32;

/* ---- zigzag order (spec Annex A) --------------------------------------- */
static const u8 zigzag[64] = {
    0,  1,  8, 16,  9,  2,  3, 10,
   17, 24, 32, 25, 18, 11,  4,  5,
   12, 19, 26, 33, 40, 48, 41, 34,
   27, 20, 13,  6,  7, 14, 21, 28,
   35, 42, 49, 56, 57, 50, 43, 36,
   29, 22, 15, 23, 30, 37, 44, 51,
   58, 59, 52, 45, 38, 31, 39, 46,
   53, 60, 61, 54, 47, 55, 62, 63
};

/* ---- fixed-point separable IDCT cosine table --------------------------
   T[x][u] = round(4096 * 0.5 * C(u) * cos((2x+1)*u*pi/16)), C(0)=1/sqrt2
   else 1. Computed once, offline, in Python (documented in jpeg.h's
   comment header); baked in the same way png.c bakes its DEFLATE length/
   distance tables, no floating point at runtime (this kernel builds with
   -mno-sse -mno-mmx and no FPU init, so runtime float math is off the
   table entirely, not just an efficiency choice). */
#define IDCT_FIX 12
static const i32 idct_tab[8][8] = {
    { 1448,  2009,  1892,  1703,  1448,  1138,   784,   400 },
    { 1448,  1703,   784,  -400, -1448, -2009, -1892, -1138 },
    { 1448,  1138,  -784, -2009, -1448,   400,  1892,  1703 },
    { 1448,   400, -1892, -1138,  1448,  1703,  -784, -2009 },
    { 1448,  -400, -1892,  1138,  1448, -1703,  -784,  2009 },
    { 1448, -1138,  -784,  2009, -1448,  -400,  1892, -1703 },
    { 1448, -1703,   784,   400, -1448,  2009, -1892,  1138 },
    { 1448, -2009,  1892, -1703,  1448, -1138,   784,  -400 },
};

/* Defensive clamp on dequantized coefficients before the IDCT: real DCT
   coefficients of 8-bit level-shifted samples are bounded to roughly
   this range by construction; clamping keeps every downstream int32
   multiply/accumulate provably inside range even against a corrupt or
   adversarial quant table, the same spirit as png.c's overflow guards
   on stride/raw_len. */
#define COEF_CLAMP 2048

static i32 clampi(i32 v, i32 lo, i32 hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* block: 64 dequantized coefficients in natural (row-major) order.
   out: 64 spatial-domain bytes (already level-shifted +128, clamped). */
static void idct8x8(const i32 *block, u8 *out) {
    i32 tmp[64]; /* tmp[x*8+v] = pass-1 result: IDCT along the row-frequency (u) axis */
    for (int v = 0; v < 8; v++) {
        for (int x = 0; x < 8; x++) {
            i32 sum = 0;
            for (int u = 0; u < 8; u++) {
                i32 c = clampi(block[u * 8 + v], -COEF_CLAMP, COEF_CLAMP - 1);
                sum += c * idct_tab[x][u];
            }
            tmp[x * 8 + v] = sum >> IDCT_FIX;
        }
    }
    for (int x = 0; x < 8; x++) {
        for (int y = 0; y < 8; y++) {
            i32 sum = 0;
            for (int v = 0; v < 8; v++) sum += tmp[x * 8 + v] * idct_tab[y][v];
            i32 px = (sum >> IDCT_FIX) + 128;
            out[x * 8 + y] = (u8)clampi(px, 0, 255);
        }
    }
}

/* ---- bitstream reader (MSB-first, byte-stuffed) ------------------------ */
struct jbits {
    const u8 *src;
    u32 len;
    u32 pos;
    u32 bitbuf;
    u32 bitcnt;
    int overrun;   /* ran off the end */
    int marker;    /* hit a real marker (0xFF followed by a non-stuff, non-RST byte) mid-stream */
};

/* Feeds one byte at a time, destuffing 0xFF 0x00 -> 0xFF. Any other byte
   following 0xFF (a real marker) stops the feed: restart markers are out
   of scope (DRI is rejected at parse time, so none should legitimately
   appear), and EOI/next-segment markers are the normal end of a scan. */
static int jbits_fill(struct jbits *b) {
    if (b->pos >= b->len) { b->overrun = 1; return -1; }
    u8 c = b->src[b->pos];
    if (c == 0xFF) {
        if (b->pos + 1 >= b->len) { b->overrun = 1; return -1; }
        u8 n = b->src[b->pos + 1];
        if (n == 0x00) { b->pos += 2; return 0xFF; }
        b->marker = 1;
        return -1;
    }
    b->pos++;
    return c;
}

static u32 jgetbit(struct jbits *b) {
    if (b->bitcnt == 0) {
        int c = jbits_fill(b);
        if (c < 0) { b->overrun = 1; return 0; }
        b->bitbuf = (u32)c;
        b->bitcnt = 8;
    }
    b->bitcnt--;
    return (b->bitbuf >> b->bitcnt) & 1;
}

static u32 jgetbits(struct jbits *b, u32 n) {
    u32 v = 0;
    for (u32 i = 0; i < n; i++) v = (v << 1) | jgetbit(b);
    return v;
}

/* Sign-extend a JPEG "receive"d magnitude per the category, table F.12:
   values 0..2^(s-1)-1 are actually negative, offset by -(2^s - 1). */
static i32 jextend(u32 v, u32 s) {
    if (s == 0) return 0;
    if (v < (1u << (s - 1))) return (i32)v - (i32)((1u << s) - 1);
    return (i32)v;
}

/* ---- canonical Huffman table (JPEG DHT is already sorted by
   [length,order], so no build/sort step is needed: count[len] straight
   from the 16 BITS bytes, symbol[] straight from HUFFVAL). ------------- */
#define JMAXBITS 16
struct jhuff {
    u16 count[JMAXBITS + 1];
    u8 symbol[256];
    int present;
};

static int jhuff_decode(struct jbits *b, const struct jhuff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= JMAXBITS; len++) {
        code = (code << 1) | (int)jgetbit(b);
        if (b->overrun) return -1;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first = (first << 1);
    }
    return -1;
}

/* ---- component / frame state -------------------------------------------- */
struct jcomp {
    int id;
    int h, v;      /* sampling factors */
    int tq;        /* quant table id */
    int td, ta;    /* huffman table ids, set at SOS */
    int dc_pred;
    u8 *plane;     /* mcus_x*h*8 by mcus_y*v*8, one byte per sample */
    u32 plane_w, plane_h;
};

static u32 be16(const u8 *p) { return ((u32)p[0] << 8) | p[1]; }

/* ---- progressive (SOF2), joshuatree 6.46.0 ------------------------------
   A progressive file sends each block's coefficients over several scans:
   DC first then AC bands, each band coarse first (Al) and then refined a
   bit at a time (Ah). Every block's 64 coefficients are kept, in zigzag
   order, in one i16 array per component (bw x bh blocks, the same padded
   grid the baseline planes use); after EOI the MCU loop below takes them
   from there instead of from a Huffman scan, and the IDCT and colour
   conversion are shared. Spec Annex G.1.2; the AC refinement follows the
   well known stb_image shape. Restart intervals stay out of scope. */
typedef short i16;
static int prog_block(struct jbits *b, i16 *blk, int ss, int se, int ah, int al, const struct jhuff *dc, const struct jhuff *ac, int *pred, u32 *eobrun) {
    if (ss == 0) {   /* DC */
        if (ah == 0) {
            int t = jhuff_decode(b, dc);
            if (t < 0 || t > 11) return -1;
            i32 diff = t ? jextend(jgetbits(b, (u32)t), (u32)t) : 0;
            *pred += diff;
            blk[0] = (i16)(*pred * (1 << al));
        } else if (jgetbit(b)) blk[0] = (i16)(blk[0] | (1 << al));
        return b->overrun ? -1 : 0;
    }
    if (ah == 0) {   /* AC, first pass of the band */
        if (*eobrun) { (*eobrun)--; return 0; }
        for (int k = ss; k <= se; k++) {
            int rs = jhuff_decode(b, ac);
            if (rs < 0) return -1;
            int r = rs >> 4, sz = rs & 15;
            if (sz == 0) {
                if (r < 15) { *eobrun = (1u << r) - 1; if (r) *eobrun += jgetbits(b, (u32)r); break; }
                k += 15; continue;   /* ZRL: sixteen zeros */
            }
            k += r;
            if (k > 63) return -1;
            blk[k] = (i16)(jextend(jgetbits(b, (u32)sz), (u32)sz) * (1 << al));
        }
        return b->overrun ? -1 : 0;
    }
    i16 bit = (i16)(1 << al);   /* AC refinement: one more bit for the nonzero ones, and new ones of +-bit */
    int k = ss;
    if (*eobrun) {
        (*eobrun)--;
        for (; k <= se; k++) if (blk[k] && jgetbit(b) && !(blk[k] & bit)) blk[k] = (i16)(blk[k] > 0 ? blk[k] + bit : blk[k] - bit);
        return b->overrun ? -1 : 0;
    }
    do {
        int rs = jhuff_decode(b, ac);
        if (rs < 0) return -1;
        int r = rs >> 4, sz = rs & 15; i16 val = 0;
        if (sz == 0) {
            if (r < 15) { *eobrun = (1u << r) - 1; if (r) *eobrun += jgetbits(b, (u32)r); r = 64; }
        } else {
            if (sz != 1) return -1;
            val = jgetbit(b) ? bit : (i16)-bit;
        }
        while (k <= se) {
            i16 *p = &blk[k++];
            if (*p) { if (jgetbit(b) && !(*p & bit)) *p = (i16)(*p > 0 ? *p + bit : *p - bit); }
            else { if (r == 0) { *p = val; break; } r--; }
        }
    } while (k <= se && !b->overrun);
    return b->overrun ? -1 : 0;
}

/* Average one accumulated destination row (dw sums of R,G,B,count) into RGB565 (or, when jpeg_out32 is set, packed 0RGB
   in a u32 array) and clear it. */
static JPEG_STATIC int jpeg_out32 = 0;
static void jpeg_flush_row(void *out, u32 *acc, u32 dw, u32 dy) {
    for (u32 x = 0; x < dw; x++) {
        u32 *a = acc + x * 4, n = a[3] ? a[3] : 1;
        u32 r = a[0] / n, g = a[1] / n, b = a[2] / n;
        if (jpeg_out32) ((u32 *)out)[dy * dw + x] = (r << 16) | (g << 8) | b;
        else ((u16 *)out)[dy * dw + x] = (u16)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        a[0] = a[1] = a[2] = a[3] = 0;
    }
}

/* One decode, two outputs. dst == 0: the full RGB/gray buffer (jpeg_decode). dst != 0 (3-channel
   only): a box-filtered dw x dh RGB565 frame decoded one MCU row at a time, so only one MCU row of
   planes is ever held (jpeg_decode_scaled, what ring 3 uses; a 320x320 frame cannot be held whole
   inside a ring-3 image). */
static int jpeg_run(const u8 *data, u32 len, u8 **out, u32 *w, u32 *h, u32 *channels, void *dst, u32 dw, u32 dh) {
    *out = 0;
    if (len < 4) return JPEG_E_TRUNCATED;
    if (data[0] != 0xFF || data[1] != 0xD8) return JPEG_E_SIGNATURE;

    static JPEG_STATIC u16 qtab[4][64] = {{0}};
    int have_q[4] = { 0, 0, 0, 0 };
    static JPEG_STATIC struct jhuff dc_huff[4] = {{0}}, ac_huff[4] = {{0}};
    for (int i = 0; i < 4; i++) { dc_huff[i].present = 0; ac_huff[i].present = 0; }

    int have_sof = 0, have_dri = 0, prog = 0;
    i16 *cf[3] = { 0, 0, 0 }; u32 cbw[3] = { 0, 0, 0 };   /* progressive: each component's coefficients, and its blocks across */
    #define PFAIL(e) do { for (int q_ = 0; q_ < 3; q_++) if (cf[q_]) kfree(cf[q_]); return (e); } while (0)
    u32 width = 0, height = 0;
    int nf = 0;
    struct jcomp comp[3];
    for (int i = 0; i < 3; i++) comp[i].plane = 0;

    u32 pos = 2;
    u32 sos_header_end = 0;
    int have_sos = 0;
    int scan_ns = 0;

    /* Pass 1: walk markers up to and including the SOS header (the scan's
       own component list), collecting DQT/DHT/SOF along the way. The
       entropy-coded data that follows SOS is handled separately below,
       since it isn't itself a sequence of length-prefixed segments. */
    while (pos + 2 <= len) {
        if (data[pos] != 0xFF) PFAIL(JPEG_E_FORMAT);
        u32 mpos = pos + 1;
        while (mpos < len && data[mpos] == 0xFF) mpos++; /* fill bytes */
        if (mpos >= len) PFAIL(JPEG_E_TRUNCATED);
        u8 marker = data[mpos];
        pos = mpos + 1;
        if (marker == 0xD9) { if (prog && have_sos) break; PFAIL(JPEG_E_FORMAT); } /* EOI: the end of a progressive file, too soon for a baseline one */
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue; /* TEM/RST: no payload */
        if (pos + 2 > len) PFAIL(JPEG_E_TRUNCATED);
        u32 seglen = be16(data + pos);
        if (seglen < 2 || pos + seglen > len) PFAIL(JPEG_E_TRUNCATED);
        const u8 *body = data + pos + 2;
        u32 blen = seglen - 2;

        if (marker == 0xDB) { /* DQT: one or more tables */
            u32 p = 0;
            while (p < blen) {
                if (p + 1 > blen) PFAIL(JPEG_E_FORMAT);
                u32 pq = body[p] >> 4, tqid = body[p] & 0xF;
                p++;
                if (tqid > 3) PFAIL(JPEG_E_FORMAT);
                if (pq != 0) PFAIL(JPEG_E_UNSUPPORTED); /* 16-bit quant values: 12-bit-precision territory */
                if (p + 64 > blen) PFAIL(JPEG_E_TRUNCATED);
                for (int i = 0; i < 64; i++) qtab[tqid][i] = body[p + i];
                p += 64;
                have_q[tqid] = 1;
            }
        } else if (marker == 0xC4) { /* DHT: one or more tables */
            u32 p = 0;
            while (p < blen) {
                if (p + 17 > blen) PFAIL(JPEG_E_TRUNCATED);
                u32 tc = body[p] >> 4, thid = body[p] & 0xF;
                p++;
                if (tc > 1 || thid > 3) PFAIL(JPEG_E_FORMAT);
                struct jhuff *ht = tc ? &ac_huff[thid] : &dc_huff[thid];
                u32 total = 0;
                for (int i = 1; i <= 16; i++) { ht->count[i] = body[p + i - 1]; total += ht->count[i]; }
                p += 16;
                if (total > 256 || p + total > blen) PFAIL(JPEG_E_TRUNCATED);
                for (u32 i = 0; i < total; i++) ht->symbol[i] = body[p + i];
                p += total;
                ht->present = 1;
            }
        } else if (marker == 0xC0 || marker == 0xC2) { /* SOF0: baseline DCT; SOF2: progressive (6.46.0) */
            prog = marker == 0xC2;
            if (have_sof) PFAIL(JPEG_E_FORMAT);
            if (blen < 6) PFAIL(JPEG_E_TRUNCATED);
            u32 precision = body[0];
            height = be16(body + 1);
            width = be16(body + 3);
            nf = body[5];
            if (precision != 8) PFAIL(JPEG_E_UNSUPPORTED);
            if (nf != 1 && nf != 3) PFAIL(JPEG_E_UNSUPPORTED); /* grayscale or YCbCr only, no CMYK */
            if (width == 0 || height == 0 || width > 16384 || height > 16384) PFAIL(JPEG_E_FORMAT);
            if (blen < 6u + 3u * (u32)nf) PFAIL(JPEG_E_TRUNCATED);
            for (int i = 0; i < nf; i++) {
                const u8 *c = body + 6 + i * 3;
                comp[i].id = c[0];
                comp[i].h = c[1] >> 4;
                comp[i].v = c[1] & 0xF;
                comp[i].tq = c[2];
                comp[i].dc_pred = 0;
                if (comp[i].h == 0 || comp[i].h > 2 || comp[i].v == 0 || comp[i].v > 2) PFAIL(JPEG_E_UNSUPPORTED);
                if (comp[i].tq > 3) PFAIL(JPEG_E_FORMAT);
            }
            if (nf == 1) {
                if (comp[0].h != 1 || comp[0].v != 1) PFAIL(JPEG_E_UNSUPPORTED);
            } else {
                /* Only 4:4:4 (all 1,1) or 4:2:0 (Y 2,2; Cb/Cr 1,1) in scope;
                   4:2:2 and anything else, cleanly rejected. */
                int y44 = (comp[0].h == 1 && comp[0].v == 1 && comp[1].h == 1 && comp[1].v == 1 && comp[2].h == 1 && comp[2].v == 1);
                int y420 = (comp[0].h == 2 && comp[0].v == 2 && comp[1].h == 1 && comp[1].v == 1 && comp[2].h == 1 && comp[2].v == 1);
                if (!y44 && !y420) PFAIL(JPEG_E_UNSUPPORTED);
            }
            have_sof = 1;
        } else if (marker == 0xC1 || marker == 0xC3 ||
                   (marker >= 0xC5 && marker <= 0xC7) || (marker >= 0xC9 && marker <= 0xCF)) {
            /* extended-sequential/progressive/lossless/arithmetic: all out of scope */
            PFAIL(JPEG_E_UNSUPPORTED);
        } else if (marker == 0xDD) { /* DRI: restart intervals out of scope, reject rather than mishandle */
            have_dri = 1;
        } else if (marker == 0xDA && prog) { /* a progressive scan: decode it now, then walk on to the next marker */
            if (!have_sof) PFAIL(JPEG_E_FORMAT);
            if (blen < 1) PFAIL(JPEG_E_TRUNCATED);
            int ns = body[0], sc[3];
            if (ns < 1 || ns > nf || blen < 1u + 2u * (u32)ns + 3u) PFAIL(JPEG_E_FORMAT);
            for (int i = 0; i < ns; i++) {
                int found = -1;
                for (int c = 0; c < nf; c++) if ((u32)comp[c].id == body[1 + i * 2]) found = c;
                if (found < 0) PFAIL(JPEG_E_FORMAT);
                comp[found].td = body[2 + i * 2] >> 4; comp[found].ta = body[2 + i * 2] & 0xF;
                if (comp[found].td > 3 || comp[found].ta > 3) PFAIL(JPEG_E_FORMAT);
                sc[i] = found;
            }
            u32 tl = 1 + 2 * (u32)ns;
            int ss = body[tl], se = body[tl + 1], ah = body[tl + 2] >> 4, al = body[tl + 2] & 0xF;
            if (ss > se || se > 63 || (ss == 0 && se != 0) || (ss > 0 && ns != 1) || al > 13 || ah > 13) PFAIL(JPEG_E_FORMAT);
            if (have_dri) PFAIL(JPEG_E_UNSUPPORTED);
            int hm = 1, vm = 1;
            for (int i = 0; i < nf; i++) { if (comp[i].h > hm) hm = comp[i].h; if (comp[i].v > vm) vm = comp[i].v; }
            u32 mx_n = (width + 8 * hm - 1) / (8 * hm), my_n = (height + 8 * vm - 1) / (8 * vm);
            if (!cf[0]) {
                for (int i = 0; i < nf; i++) {
                    u32 bw = mx_n * comp[i].h, bh = my_n * comp[i].v, nb = bw * bh;
                    if (bw == 0 || nb / bw != bh || nb > (64u << 20) / 128) PFAIL(JPEG_E_FORMAT);   /* 64 i16 a block, 64 MiB at most */
                    cf[i] = kmalloc(nb * 128u);
                    if (!cf[i]) PFAIL(JPEG_E_NOMEM);
                    memset(cf[i], 0, nb * 128u);
                    cbw[i] = bw;
                }
            }
            for (int i = 0; i < ns; i++) {
                if (ss == 0 && ah == 0 && !dc_huff[comp[sc[i]].td].present) PFAIL(JPEG_E_FORMAT);
                if (ss > 0 && !ac_huff[comp[sc[i]].ta].present) PFAIL(JPEG_E_FORMAT);
                comp[sc[i]].dc_pred = 0;
            }
            struct jbits pb = { data, len, pos + seglen, 0, 0, 0, 0 };
            u32 eobrun = 0; int err = 0;
            if (ns > 1) {   /* interleaved (DC only): MCU by MCU, as baseline */
                for (u32 my = 0; my < my_n && !err; my++) for (u32 mx = 0; mx < mx_n && !err; mx++)
                    for (int i = 0; i < ns && !err; i++) {
                        struct jcomp *cc = &comp[sc[i]];
                        for (int by = 0; by < cc->v && !err; by++) for (int bx = 0; bx < cc->h && !err; bx++)
                            err = prog_block(&pb, cf[sc[i]] + ((my * (u32)cc->v + (u32)by) * cbw[sc[i]] + mx * (u32)cc->h + (u32)bx) * 64,
                                             ss, se, ah, al, &dc_huff[cc->td], &ac_huff[cc->ta], &cc->dc_pred, &eobrun);
                    }
            } else {   /* one component: its own blocks, only those the image covers */
                struct jcomp *cc = &comp[sc[0]];
                u32 cw = (width * (u32)cc->h + (u32)hm - 1) / (u32)hm, ch = (height * (u32)cc->v + (u32)vm - 1) / (u32)vm;
                u32 nbx = (cw + 7) / 8, nby = (ch + 7) / 8;
                for (u32 by = 0; by < nby && !err; by++) for (u32 bx = 0; bx < nbx && !err; bx++)
                    err = prog_block(&pb, cf[sc[0]] + (by * cbw[sc[0]] + bx) * 64, ss, se, ah, al, &dc_huff[cc->td], &ac_huff[cc->ta], &cc->dc_pred, &eobrun);
            }
            if (err) PFAIL(JPEG_E_HUFFMAN);
            have_sos = 1; scan_ns = nf;
            pos = pb.pos;   /* the next marker, where the scan's bytes stopped */
            continue;
        } else if (marker == 0xDA) { /* SOS */
            if (!have_sof) PFAIL(JPEG_E_FORMAT);
            if (blen < 1) PFAIL(JPEG_E_TRUNCATED);
            scan_ns = body[0];
            if (scan_ns != nf) PFAIL(JPEG_E_UNSUPPORTED); /* non-interleaved / multi-scan baseline: out of scope */
            if (blen < 1u + 2u * (u32)scan_ns + 3u) PFAIL(JPEG_E_TRUNCATED);
            for (int i = 0; i < scan_ns; i++) {
                u32 cid = body[1 + i * 2];
                u32 tdta = body[2 + i * 2];
                int found = -1;
                for (int c = 0; c < nf; c++) if ((u32)comp[c].id == cid) found = c;
                if (found < 0) PFAIL(JPEG_E_FORMAT);
                comp[found].td = tdta >> 4;
                comp[found].ta = tdta & 0xF;
                if (comp[found].td > 3 || comp[found].ta > 3) PFAIL(JPEG_E_FORMAT);
            }
            u32 tail = 1 + 2 * scan_ns;
            u32 ss = body[tail], se = body[tail + 1], ahal = body[tail + 2];
            if (ss != 0 || se != 63 || ahal != 0) PFAIL(JPEG_E_UNSUPPORTED); /* non-baseline scan parameters */
            sos_header_end = pos + seglen;
            have_sos = 1;
            break;
        }
        pos += seglen;
    }
    if (!have_sos) PFAIL(JPEG_E_TRUNCATED);
    if (have_dri) PFAIL(JPEG_E_UNSUPPORTED);
    for (int i = 0; i < nf; i++) if (!have_q[comp[i].tq]) PFAIL(JPEG_E_FORMAT);
    for (int i = 0; i < nf && !prog; i++) {
        if (!dc_huff[comp[i].td].present || !ac_huff[comp[i].ta].present) return JPEG_E_FORMAT;
    }

    int hmax = 1, vmax = 1;
    for (int i = 0; i < nf; i++) { if (comp[i].h > hmax) hmax = comp[i].h; if (comp[i].v > vmax) vmax = comp[i].v; }
    u32 mcus_x = (width + 8 * hmax - 1) / (8 * hmax);
    u32 mcus_y = (height + 8 * vmax - 1) / (8 * vmax);
    /* overflow guard, same discipline as png.c's stride/raw_len check */
    if (mcus_x != 0 && (mcus_x * mcus_y) / mcus_x != mcus_y) PFAIL(JPEG_E_FORMAT);
    if (mcus_x > 0x10000 || mcus_y > 0x10000) PFAIL(JPEG_E_FORMAT);

    for (int i = 0; i < nf; i++) {
        comp[i].plane_w = mcus_x * comp[i].h * 8;
        comp[i].plane_h = (dst ? 1 : mcus_y) * comp[i].v * 8;
        u32 psize = comp[i].plane_w * comp[i].plane_h;
        if (comp[i].plane_w == 0 || psize / comp[i].plane_w != comp[i].plane_h) {
            for (int j = 0; j < i; j++) kfree(comp[j].plane);
            PFAIL(JPEG_E_FORMAT);
        }
        comp[i].plane = kmalloc(psize);
        if (!comp[i].plane) {
            for (int j = 0; j < i; j++) kfree(comp[j].plane);
            PFAIL(JPEG_E_NOMEM);
        }
    }

    u32 *acc = 0, acc_dy = 0;
    if (dst) {
        acc = kmalloc(dw * 16u);
        if (!acc) { for (int i = 0; i < nf; i++) kfree(comp[i].plane); PFAIL(JPEG_E_NOMEM); }
        memset(acc, 0, dw * 16u);
    }
    struct jbits bits = { data, len, sos_header_end, 0, 0, 0, 0 };
    i32 coef[64];
    int decode_err = 0;

    for (u32 my = 0; my < mcus_y && !decode_err; my++) {
        for (u32 mx = 0; mx < mcus_x && !decode_err; mx++) {
            for (int ci = 0; ci < nf && !decode_err; ci++) {
                struct jcomp *cc = &comp[ci];
                for (int by = 0; by < cc->v && !decode_err; by++) {
                    for (int bx = 0; bx < cc->h && !decode_err; bx++) {
                        for (int i = 0; i < 64; i++) coef[i] = 0;
                        if (prog) {   /* 6.46.0: the stored coefficients, dequantized into natural order */
                            const i16 *src = cf[ci] + ((my * (u32)cc->v + (u32)by) * cbw[ci] + mx * (u32)cc->h + (u32)bx) * 64;
                            for (int k = 0; k < 64; k++) coef[zigzag[k]] = (i32)src[k] * (i32)qtab[cc->tq][k];
                            goto idct;
                        }

                        int cat = jhuff_decode(&bits, &dc_huff[cc->td]);
                        if (cat < 0 || cat > 11 || bits.overrun) { decode_err = 1; break; }
                        u32 diffbits = cat ? jgetbits(&bits, (u32)cat) : 0;
                        if (bits.overrun) { decode_err = 1; break; }
                        i32 diff = jextend(diffbits, (u32)cat);
                        cc->dc_pred += diff;
                        coef[0] = cc->dc_pred * (i32)qtab[cc->tq][0];

                        int k = 1;
                        while (k < 64) {
                            int rs = jhuff_decode(&bits, &ac_huff[cc->ta]);
                            if (rs < 0 || bits.overrun) { decode_err = 1; break; }
                            int r = rs >> 4, s = rs & 0xF;
                            if (s == 0) {
                                if (r == 15) { k += 16; continue; } /* ZRL */
                                break; /* EOB */
                            }
                            k += r;
                            if (k >= 64) { decode_err = 1; break; }
                            u32 vbits = jgetbits(&bits, (u32)s);
                            if (bits.overrun) { decode_err = 1; break; }
                            i32 v = jextend(vbits, (u32)s);
                            coef[zigzag[k]] = v * (i32)qtab[cc->tq][k];
                            k++;
                        }
                        if (decode_err) break;

                    idct:;
                        u8 block[64];
                        idct8x8(coef, block);
                        u32 ox = (mx * cc->h + bx) * 8, oy = ((dst ? 0 : my) * cc->v + by) * 8;
                        for (int py = 0; py < 8; py++)
                            memcpy(cc->plane + (oy + (u32)py) * cc->plane_w + ox, block + py * 8, 8);
                    }
                }
            }
        }
        if (dst && !decode_err) {
            if (nf != 3) { for (int i = 0; i < nf; i++) kfree(comp[i].plane); kfree(acc); PFAIL(JPEG_E_UNSUPPORTED); }
            int sx_cb = hmax / comp[1].h, sy_cb = vmax / comp[1].v;
            int sx_cr = hmax / comp[2].h, sy_cr = vmax / comp[2].v;
            for (u32 ry = 0; ry < (u32)vmax * 8; ry++) {
                u32 y = my * (u32)vmax * 8 + ry;
                if (y >= height) break;
                const u8 *yrow = comp[0].plane + ry * comp[0].plane_w;
                const u8 *cbrow = comp[1].plane + (ry / (u32)sy_cb) * comp[1].plane_w;
                const u8 *crrow = comp[2].plane + (ry / (u32)sy_cr) * comp[2].plane_w;
                u32 dy = y * dh / height;
                if (dy != acc_dy) { jpeg_flush_row(dst, acc, dw, acc_dy); acc_dy = dy; }
                for (u32 x = 0; x < width; x++) {
                    i32 Y = yrow[x], Cb = cbrow[x / (u32)sx_cb] - 128, Cr = crrow[x / (u32)sx_cr] - 128;
                    u32 *a = acc + (x * dw / width) * 4;
                    a[0] += (u32)clampi(Y + ((91881 * Cr) >> 16), 0, 255);
                    a[1] += (u32)clampi(Y + ((-22554 * Cb - 46802 * Cr) >> 16), 0, 255);
                    a[2] += (u32)clampi(Y + ((116130 * Cb) >> 16), 0, 255);
                    a[3]++;
                }
            }
        }
    }
    if (decode_err || bits.overrun) {
        for (int i = 0; i < nf; i++) kfree(comp[i].plane);
        if (acc) kfree(acc);
        PFAIL(JPEG_E_HUFFMAN);
    }
    if (dst) {
        jpeg_flush_row(dst, acc, dw, acc_dy);
        for (int i = 0; i < nf; i++) kfree(comp[i].plane);
        kfree(acc);
        for (int q = 0; q < 3; q++) if (cf[q]) kfree(cf[q]);
        *w = width; *h = height;
        return 0;
    }

    u32 outch = (nf == 1) ? 1 : 3;
    u8 *px = kmalloc((u32)width * height * outch);
    if (!px) {
        for (int i = 0; i < nf; i++) kfree(comp[i].plane);
        PFAIL(JPEG_E_NOMEM);
    }
    for (int q = 0; q < 3; q++) if (cf[q]) { kfree(cf[q]); cf[q] = 0; }

    if (nf == 1) {
        for (u32 y = 0; y < height; y++)
            memcpy(px + y * width, comp[0].plane + y * comp[0].plane_w, width);
    } else {
        int sx_cb = hmax / comp[1].h, sy_cb = vmax / comp[1].v;
        int sx_cr = hmax / comp[2].h, sy_cr = vmax / comp[2].v;
        for (u32 y = 0; y < height; y++) {
            const u8 *yrow = comp[0].plane + y * comp[0].plane_w;
            const u8 *cbrow = comp[1].plane + (y / (u32)sy_cb) * comp[1].plane_w;
            const u8 *crrow = comp[2].plane + (y / (u32)sy_cr) * comp[2].plane_w;
            u8 *orow = px + y * width * 3;
            for (u32 x = 0; x < width; x++) {
                i32 Y = yrow[x];
                i32 Cb = cbrow[x / (u32)sx_cb] - 128;
                i32 Cr = crrow[x / (u32)sx_cr] - 128;
                i32 r = Y + ((91881 * Cr) >> 16);
                i32 g = Y + ((-22554 * Cb - 46802 * Cr) >> 16);
                i32 b = Y + ((116130 * Cb) >> 16);
                orow[x * 3 + 0] = (u8)clampi(r, 0, 255);
                orow[x * 3 + 1] = (u8)clampi(g, 0, 255);
                orow[x * 3 + 2] = (u8)clampi(b, 0, 255);
            }
        }
    }

    for (int i = 0; i < nf; i++) kfree(comp[i].plane);
    *out = px; *w = width; *h = height; *channels = outch;
    return 0;
}

int jpeg_decode(const unsigned char *data, unsigned int len, unsigned char **out, unsigned int *w, unsigned int *h, unsigned int *channels) {
    return jpeg_run(data, len, out, w, h, channels, 0, 0, 0);
}

int jpeg_decode_scaled(const unsigned char *data, unsigned int len, unsigned short *dst, unsigned int dw, unsigned int dh,
                       unsigned int *w, unsigned int *h) {
    unsigned char *none = 0; unsigned int ch = 0;
    if (!dst || dw == 0 || dh == 0 || dw > 320) return JPEG_E_FORMAT;
    jpeg_out32 = 0;
    return jpeg_run(data, len, &none, w, h, &ch, dst, dw, dh);
}

int jpeg_decode_scaled32(const unsigned char *data, unsigned int len, unsigned int *dst, unsigned int dw, unsigned int dh,
                         unsigned int *w, unsigned int *h) {
    unsigned char *none = 0; unsigned int ch = 0;
    if (!dst || dw == 0 || dh == 0 || dw > 1024) return JPEG_E_FORMAT;
    jpeg_out32 = 1;
    int r = jpeg_run(data, len, &none, w, h, &ch, dst, dw, dh);
    jpeg_out32 = 0;
    return r;
}
