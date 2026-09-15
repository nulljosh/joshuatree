/* Minimal baseline JPEG decoder: see jpeg.h for the exact scope. Marker
   walk, canonical Huffman decode of DC/AC coefficients, zigzag dequant,
   a real separable fixed-point IDCT, YCbCr->RGB, nearest-neighbor chroma
   upsampling. No recursion, no dynamic tables beyond one kmalloc per
   component plane plus the final RGB/gray output buffer. */
#include "jpeg.h"
#include "kheap.h"
#include "libc.h"

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

int jpeg_decode(const u8 *data, u32 len, u8 **out, u32 *w, u32 *h, u32 *channels) {
    *out = 0;
    if (len < 4) return JPEG_E_TRUNCATED;
    if (data[0] != 0xFF || data[1] != 0xD8) return JPEG_E_SIGNATURE;

    static u16 qtab[4][64];
    int have_q[4] = { 0, 0, 0, 0 };
    static struct jhuff dc_huff[4], ac_huff[4];
    for (int i = 0; i < 4; i++) { dc_huff[i].present = 0; ac_huff[i].present = 0; }

    int have_sof = 0, have_dri = 0;
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
        if (data[pos] != 0xFF) return JPEG_E_FORMAT;
        u32 mpos = pos + 1;
        while (mpos < len && data[mpos] == 0xFF) mpos++; /* fill bytes */
        if (mpos >= len) return JPEG_E_TRUNCATED;
        u8 marker = data[mpos];
        pos = mpos + 1;
        if (marker == 0xD9) return JPEG_E_FORMAT; /* EOI before SOS */
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue; /* TEM/RST: no payload */
        if (pos + 2 > len) return JPEG_E_TRUNCATED;
        u32 seglen = be16(data + pos);
        if (seglen < 2 || pos + seglen > len) return JPEG_E_TRUNCATED;
        const u8 *body = data + pos + 2;
        u32 blen = seglen - 2;

        if (marker == 0xDB) { /* DQT: one or more tables */
            u32 p = 0;
            while (p < blen) {
                if (p + 1 > blen) return JPEG_E_FORMAT;
                u32 pq = body[p] >> 4, tqid = body[p] & 0xF;
                p++;
                if (tqid > 3) return JPEG_E_FORMAT;
                if (pq != 0) return JPEG_E_UNSUPPORTED; /* 16-bit quant values: 12-bit-precision territory */
                if (p + 64 > blen) return JPEG_E_TRUNCATED;
                for (int i = 0; i < 64; i++) qtab[tqid][i] = body[p + i];
                p += 64;
                have_q[tqid] = 1;
            }
        } else if (marker == 0xC4) { /* DHT: one or more tables */
            u32 p = 0;
            while (p < blen) {
                if (p + 17 > blen) return JPEG_E_TRUNCATED;
                u32 tc = body[p] >> 4, thid = body[p] & 0xF;
                p++;
                if (tc > 1 || thid > 3) return JPEG_E_FORMAT;
                struct jhuff *ht = tc ? &ac_huff[thid] : &dc_huff[thid];
                u32 total = 0;
                for (int i = 1; i <= 16; i++) { ht->count[i] = body[p + i - 1]; total += ht->count[i]; }
                p += 16;
                if (total > 256 || p + total > blen) return JPEG_E_TRUNCATED;
                for (u32 i = 0; i < total; i++) ht->symbol[i] = body[p + i];
                p += total;
                ht->present = 1;
            }
        } else if (marker == 0xC0) { /* SOF0: baseline DCT */
            if (have_sof) return JPEG_E_FORMAT;
            if (blen < 6) return JPEG_E_TRUNCATED;
            u32 precision = body[0];
            height = be16(body + 1);
            width = be16(body + 3);
            nf = body[5];
            if (precision != 8) return JPEG_E_UNSUPPORTED;
            if (nf != 1 && nf != 3) return JPEG_E_UNSUPPORTED; /* grayscale or YCbCr only, no CMYK */
            if (width == 0 || height == 0 || width > 16384 || height > 16384) return JPEG_E_FORMAT;
            if (blen < 6u + 3u * (u32)nf) return JPEG_E_TRUNCATED;
            for (int i = 0; i < nf; i++) {
                const u8 *c = body + 6 + i * 3;
                comp[i].id = c[0];
                comp[i].h = c[1] >> 4;
                comp[i].v = c[1] & 0xF;
                comp[i].tq = c[2];
                comp[i].dc_pred = 0;
                if (comp[i].h == 0 || comp[i].h > 2 || comp[i].v == 0 || comp[i].v > 2) return JPEG_E_UNSUPPORTED;
                if (comp[i].tq > 3) return JPEG_E_FORMAT;
            }
            if (nf == 1) {
                if (comp[0].h != 1 || comp[0].v != 1) return JPEG_E_UNSUPPORTED;
            } else {
                /* Only 4:4:4 (all 1,1) or 4:2:0 (Y 2,2; Cb/Cr 1,1) in scope;
                   4:2:2 and anything else, cleanly rejected. */
                int y44 = (comp[0].h == 1 && comp[0].v == 1 && comp[1].h == 1 && comp[1].v == 1 && comp[2].h == 1 && comp[2].v == 1);
                int y420 = (comp[0].h == 2 && comp[0].v == 2 && comp[1].h == 1 && comp[1].v == 1 && comp[2].h == 1 && comp[2].v == 1);
                if (!y44 && !y420) return JPEG_E_UNSUPPORTED;
            }
            have_sof = 1;
        } else if (marker == 0xC1 || marker == 0xC2 || marker == 0xC3 ||
                   (marker >= 0xC5 && marker <= 0xC7) || (marker >= 0xC9 && marker <= 0xCF)) {
            /* extended-sequential/progressive/lossless/arithmetic: all out of scope */
            return JPEG_E_UNSUPPORTED;
        } else if (marker == 0xDD) { /* DRI: restart intervals out of scope, reject rather than mishandle */
            have_dri = 1;
        } else if (marker == 0xDA) { /* SOS */
            if (!have_sof) return JPEG_E_FORMAT;
            if (blen < 1) return JPEG_E_TRUNCATED;
            scan_ns = body[0];
            if (scan_ns != nf) return JPEG_E_UNSUPPORTED; /* non-interleaved / multi-scan baseline: out of scope */
            if (blen < 1u + 2u * (u32)scan_ns + 3u) return JPEG_E_TRUNCATED;
            for (int i = 0; i < scan_ns; i++) {
                u32 cid = body[1 + i * 2];
                u32 tdta = body[2 + i * 2];
                int found = -1;
                for (int c = 0; c < nf; c++) if ((u32)comp[c].id == cid) found = c;
                if (found < 0) return JPEG_E_FORMAT;
                comp[found].td = tdta >> 4;
                comp[found].ta = tdta & 0xF;
                if (comp[found].td > 3 || comp[found].ta > 3) return JPEG_E_FORMAT;
            }
            u32 tail = 1 + 2 * scan_ns;
            u32 ss = body[tail], se = body[tail + 1], ahal = body[tail + 2];
            if (ss != 0 || se != 63 || ahal != 0) return JPEG_E_UNSUPPORTED; /* non-baseline scan parameters */
            sos_header_end = pos + seglen;
            have_sos = 1;
            break;
        }
        pos += seglen;
    }
    if (!have_sos) return JPEG_E_TRUNCATED;
    if (have_dri) return JPEG_E_UNSUPPORTED;
    for (int i = 0; i < nf; i++) if (!have_q[comp[i].tq]) return JPEG_E_FORMAT;
    for (int i = 0; i < nf; i++) {
        if (!dc_huff[comp[i].td].present || !ac_huff[comp[i].ta].present) return JPEG_E_FORMAT;
    }

    int hmax = 1, vmax = 1;
    for (int i = 0; i < nf; i++) { if (comp[i].h > hmax) hmax = comp[i].h; if (comp[i].v > vmax) vmax = comp[i].v; }
    u32 mcus_x = (width + 8 * hmax - 1) / (8 * hmax);
    u32 mcus_y = (height + 8 * vmax - 1) / (8 * vmax);
    /* overflow guard, same discipline as png.c's stride/raw_len check */
    if (mcus_x != 0 && (mcus_x * mcus_y) / mcus_x != mcus_y) return JPEG_E_FORMAT;
    if (mcus_x > 0x10000 || mcus_y > 0x10000) return JPEG_E_FORMAT;

    for (int i = 0; i < nf; i++) {
        comp[i].plane_w = mcus_x * comp[i].h * 8;
        comp[i].plane_h = mcus_y * comp[i].v * 8;
        u32 psize = comp[i].plane_w * comp[i].plane_h;
        if (comp[i].plane_w == 0 || psize / comp[i].plane_w != comp[i].plane_h) {
            for (int j = 0; j < i; j++) kfree(comp[j].plane);
            return JPEG_E_FORMAT;
        }
        comp[i].plane = kmalloc(psize);
        if (!comp[i].plane) {
            for (int j = 0; j < i; j++) kfree(comp[j].plane);
            return JPEG_E_NOMEM;
        }
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

                        u8 block[64];
                        idct8x8(coef, block);
                        u32 ox = (mx * cc->h + bx) * 8, oy = (my * cc->v + by) * 8;
                        for (int py = 0; py < 8; py++)
                            memcpy(cc->plane + (oy + (u32)py) * cc->plane_w + ox, block + py * 8, 8);
                    }
                }
            }
        }
    }
    if (decode_err || bits.overrun) {
        for (int i = 0; i < nf; i++) kfree(comp[i].plane);
        return JPEG_E_HUFFMAN;
    }

    u32 outch = (nf == 1) ? 1 : 3;
    u8 *px = kmalloc((u32)width * height * outch);
    if (!px) {
        for (int i = 0; i < nf; i++) kfree(comp[i].plane);
        return JPEG_E_NOMEM;
    }

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
