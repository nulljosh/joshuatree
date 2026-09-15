/* Minimal PNG decoder: see png.h for the exact scope. Three layers, top
   down: chunk walk (signature, IHDR, IDAT concatenation, IEND, CRC per
   chunk), zlib/DEFLATE inflate (RFC 1950/1951, all three block kinds),
   scanline unfiltering (RFC 2083 6.6, all five filter types). No
   recursion, no tables larger than a few hundred bytes on the stack, one
   kmalloc for the inflated scanlines plus one for the pixel output. */
#include "png.h"
#include "kheap.h"
#include "libc.h"

typedef unsigned int u32;
typedef unsigned char u8;

/* ---- CRC-32 (PNG chunks) --------------------------------------------- */
static u32 crc_table[256];
static int crc_ready = 0;

static void crc_init(void) {
    for (u32 n = 0; n < 256; n++) {
        u32 c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_table[n] = c;
    }
    crc_ready = 1;
}

u32 png_crc32(const u8 *p, u32 n) {
    if (!crc_ready) crc_init();
    u32 c = 0xFFFFFFFFu;
    for (u32 i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ---- DEFLATE ---------------------------------------------------------- */
struct bits {
    const u8 *src;
    u32 len;
    u32 pos;      /* next byte */
    u32 bitbuf;
    u32 bitcnt;
    int overrun;  /* set once a read went past the end */
};

static u32 getbit(struct bits *b) {
    if (b->bitcnt == 0) {
        if (b->pos >= b->len) { b->overrun = 1; return 0; }
        b->bitbuf = b->src[b->pos++];
        b->bitcnt = 8;
    }
    u32 v = b->bitbuf & 1;
    b->bitbuf >>= 1;
    b->bitcnt--;
    return v;
}

/* n <= 16, LSB-first as DEFLATE packs everything except Huffman codes */
static u32 getbits(struct bits *b, u32 n) {
    u32 v = 0;
    for (u32 i = 0; i < n; i++) v |= getbit(b) << i;
    return v;
}

/* Canonical Huffman table in the zlib "puff" form: count[len] = number of
   codes of that length, symbol[] = symbols sorted by (length, value).
   Decoding walks lengths 1..15 accumulating the code MSB-first, which is
   how DEFLATE stores Huffman codes (the one thing not LSB-first). */
#define MAXBITS 15
struct huff {
    unsigned short count[MAXBITS + 1];
    unsigned short symbol[288];
};

/* Returns 0 on success, -1 if the lengths describe an over-subscribed
   (invalid) code. Incomplete codes are allowed (a single-code distance
   tree is legal per RFC 1951 3.2.7). */
static int huff_build(struct huff *h, const unsigned short *lengths, u32 n) {
    unsigned short offs[MAXBITS + 1];
    for (u32 i = 0; i <= MAXBITS; i++) h->count[i] = 0;
    for (u32 i = 0; i < n; i++) h->count[lengths[i]]++;
    if (h->count[0] == n) return 0; /* no codes at all: fine, never used */
    int left = 1;
    for (u32 len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return -1;
    }
    offs[1] = 0;
    for (u32 len = 1; len < MAXBITS; len++) offs[len + 1] = offs[len] + h->count[len];
    for (u32 i = 0; i < n; i++)
        if (lengths[i]) h->symbol[offs[lengths[i]]++] = (unsigned short)i;
    return 0;
}

static int huff_decode(struct bits *b, const struct huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        code |= (int)getbit(b);
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1; /* ran out of codes: corrupt stream */
}

static const unsigned short len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const unsigned short len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const unsigned short dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const unsigned short dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

struct out {
    u8 *dst;
    u32 len;
    u32 pos;
};

/* Decode one Huffman-coded block's symbols until end-of-block (256). */
static int inflate_codes(struct bits *b, struct out *o,
                         const struct huff *lencode, const struct huff *distcode) {
    for (;;) {
        int sym = huff_decode(b, lencode);
        if (sym < 0 || b->overrun) return PNG_E_ZLIB;
        if (sym < 256) {
            if (o->pos >= o->len) return PNG_E_ZLIB; /* more output than the caller sized for */
            o->dst[o->pos++] = (u8)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return PNG_E_ZLIB;
            u32 len = len_base[sym] + getbits(b, len_extra[sym]);
            int dsym = huff_decode(b, distcode);
            if (dsym < 0 || dsym >= 30) return PNG_E_ZLIB;
            u32 dist = dist_base[dsym] + getbits(b, dist_extra[dsym]);
            if (b->overrun) return PNG_E_TRUNCATED;
            if (dist > o->pos) return PNG_E_ZLIB;          /* back-reference before start */
            if (o->pos + len > o->len) return PNG_E_ZLIB;   /* output overflow */
            /* byte-by-byte on purpose: dist < len overlaps (run-length) is legal */
            for (u32 i = 0; i < len; i++, o->pos++) o->dst[o->pos] = o->dst[o->pos - dist];
        }
    }
}

static int inflate_stored(struct bits *b, struct out *o) {
    b->bitbuf = 0; b->bitcnt = 0; /* discard to byte boundary */
    if (b->pos + 4 > b->len) return PNG_E_TRUNCATED;
    u32 len = b->src[b->pos] | ((u32)b->src[b->pos + 1] << 8);
    u32 nlen = b->src[b->pos + 2] | ((u32)b->src[b->pos + 3] << 8);
    b->pos += 4;
    if ((len ^ 0xFFFFu) != nlen) return PNG_E_ZLIB;
    if (b->pos + len > b->len) return PNG_E_TRUNCATED;
    if (o->pos + len > o->len) return PNG_E_ZLIB;
    memcpy(o->dst + o->pos, b->src + b->pos, len);
    o->pos += len;
    b->pos += len;
    return 0;
}

static int inflate_fixed(struct bits *b, struct out *o) {
    static struct huff lencode, distcode;
    static int built = 0;
    if (!built) {
        unsigned short lengths[288];
        u32 i = 0;
        for (; i < 144; i++) lengths[i] = 8;
        for (; i < 256; i++) lengths[i] = 9;
        for (; i < 280; i++) lengths[i] = 7;
        for (; i < 288; i++) lengths[i] = 8;
        huff_build(&lencode, lengths, 288);
        for (i = 0; i < 30; i++) lengths[i] = 5;
        huff_build(&distcode, lengths, 30);
        built = 1;
    }
    return inflate_codes(b, o, &lencode, &distcode);
}

static int inflate_dynamic(struct bits *b, struct out *o) {
    static const unsigned short order[19] =
        { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    unsigned short lengths[286 + 30];
    struct huff lencode, distcode;

    u32 nlen = getbits(b, 5) + 257;
    u32 ndist = getbits(b, 5) + 1;
    u32 ncode = getbits(b, 4) + 4;
    if (nlen > 286 || ndist > 30) return PNG_E_ZLIB;

    u32 i;
    for (i = 0; i < ncode; i++) lengths[order[i]] = (unsigned short)getbits(b, 3);
    for (; i < 19; i++) lengths[order[i]] = 0;
    if (huff_build(&lencode, lengths, 19) != 0) return PNG_E_ZLIB;

    /* code lengths for the literal/length + distance alphabets, run-length
       coded with symbols 16 (repeat previous), 17 (zeros x3-10), 18 (zeros x11-138) */
    i = 0;
    while (i < nlen + ndist) {
        int sym = huff_decode(b, &lencode);
        if (sym < 0 || b->overrun) return PNG_E_ZLIB;
        if (sym < 16) {
            lengths[i++] = (unsigned short)sym;
        } else {
            u32 rep, val = 0;
            if (sym == 16) {
                if (i == 0) return PNG_E_ZLIB;
                val = lengths[i - 1];
                rep = 3 + getbits(b, 2);
            } else if (sym == 17) {
                rep = 3 + getbits(b, 3);
            } else {
                rep = 11 + getbits(b, 7);
            }
            if (i + rep > nlen + ndist) return PNG_E_ZLIB;
            while (rep--) lengths[i++] = (unsigned short)val;
        }
    }
    if (lengths[256] == 0) return PNG_E_ZLIB; /* no end-of-block code */
    if (huff_build(&lencode, lengths, nlen) != 0) return PNG_E_ZLIB;
    if (huff_build(&distcode, lengths + nlen, ndist) != 0) return PNG_E_ZLIB;
    return inflate_codes(b, o, &lencode, &distcode);
}

static u32 adler32(const u8 *p, u32 n) {
    u32 a = 1, b = 0;
    for (u32 i = 0; i < n; i++) {
        a += p[i]; if (a >= 65521u) a -= 65521u;
        b += a;    b %= 65521u;
    }
    return (b << 16) | a;
}

int png_zlib_inflate(const u8 *src, u32 src_len, u8 *dst, u32 dst_len) {
    if (src_len < 6) return PNG_E_TRUNCATED;
    /* RFC 1950 header: CM=8 (deflate), FCHECK makes CMF*256+FLG a multiple of 31,
       FDICT must be clear (PNG forbids preset dictionaries) */
    u32 cmf = src[0], flg = src[1];
    if ((cmf & 0x0F) != 8 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20)) return PNG_E_ZLIB;

    struct bits b = { src, src_len - 4, 2, 0, 0, 0 }; /* last 4 bytes are the Adler-32 */
    struct out o = { dst, dst_len, 0 };
    u32 last;
    do {
        last = getbit(&b);
        u32 type = getbits(&b, 2);
        int r;
        if (type == 0)      r = inflate_stored(&b, &o);
        else if (type == 1) r = inflate_fixed(&b, &o);
        else if (type == 2) r = inflate_dynamic(&b, &o);
        else                return PNG_E_ZLIB;
        if (r) return r;
        if (b.overrun) return PNG_E_TRUNCATED;
    } while (!last);
    if (o.pos != dst_len) return PNG_E_ZLIB; /* short stream: fewer scanline bytes than IHDR promised */

    const u8 *t = src + src_len - 4;
    u32 want = ((u32)t[0] << 24) | ((u32)t[1] << 16) | ((u32)t[2] << 8) | t[3];
    if (adler32(dst, dst_len) != want) return PNG_E_ZLIB;
    return 0;
}

/* ---- scanline filters --------------------------------------------------- */
static u8 paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return (u8)a;
    if (pb <= pc) return (u8)b;
    return (u8)c;
}

/* In-place: raw holds h rows of (1 + stride) bytes (filter byte first);
   out receives h rows of stride bytes. prev is the already-reconstructed
   previous output row (0 for the first row, where Up/Paeth's "b" and "c"
   are defined as zero). */
static int unfilter(const u8 *raw, u8 *out, u32 w_bytes, u32 h, u32 bpp) {
    const u8 *prev = 0;
    for (u32 y = 0; y < h; y++) {
        const u8 *in = raw + y * (w_bytes + 1);
        u8 ftype = in[0];
        u8 *cur = out + y * w_bytes;
        in++;
        switch (ftype) {
        case 0:
            memcpy(cur, in, w_bytes);
            break;
        case 1:
            for (u32 i = 0; i < w_bytes; i++)
                cur[i] = (u8)(in[i] + (i >= bpp ? cur[i - bpp] : 0));
            break;
        case 2:
            for (u32 i = 0; i < w_bytes; i++)
                cur[i] = (u8)(in[i] + (prev ? prev[i] : 0));
            break;
        case 3:
            for (u32 i = 0; i < w_bytes; i++) {
                u32 a = i >= bpp ? cur[i - bpp] : 0;
                u32 b = prev ? prev[i] : 0;
                cur[i] = (u8)(in[i] + ((a + b) >> 1));
            }
            break;
        case 4:
            for (u32 i = 0; i < w_bytes; i++) {
                int a = i >= bpp ? cur[i - bpp] : 0;
                int b = prev ? prev[i] : 0;
                int c = (prev && i >= bpp) ? prev[i - bpp] : 0;
                cur[i] = (u8)(in[i] + paeth(a, b, c));
            }
            break;
        default:
            return PNG_E_FILTER;
        }
        prev = cur;
    }
    return 0;
}

/* ---- chunk walk ---------------------------------------------------------- */
static u32 be32(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

int png_decode(const u8 *data, u32 len, u8 **out, u32 *w, u32 *h, u32 *channels) {
    static const u8 sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    *out = 0;
    if (len < 8 + 25) return PNG_E_TRUNCATED;
    if (memcmp(data, sig, 8) != 0) return PNG_E_SIGNATURE;

    u32 pos = 8;
    u32 width = 0, height = 0, ch = 0;
    int have_ihdr = 0, have_iend = 0;
    u32 idat_total = 0;

    /* Pass 1: validate every chunk's length + CRC, read IHDR, sum IDAT sizes. */
    while (pos + 12 <= len && !have_iend) {
        u32 clen = be32(data + pos);
        const u8 *tag = data + pos + 4;
        if (clen > len || pos + 12 + clen > len) return PNG_E_TRUNCATED;
        if (png_crc32(tag, clen + 4) != be32(data + pos + 8 + clen)) return PNG_E_CRC;
        const u8 *body = data + pos + 8;
        if (!memcmp(tag, "IHDR", 4)) {
            if (have_ihdr || clen != 13 || pos != 8) return PNG_E_FORMAT;
            width = be32(body); height = be32(body + 4);
            u32 depth = body[8], ctype = body[9], comp = body[10], filt = body[11], ilace = body[12];
            if (width == 0 || height == 0 || width > 16384 || height > 16384) return PNG_E_FORMAT;
            if (comp != 0 || filt != 0) return PNG_E_FORMAT;
            if (depth != 8 || (ctype != 2 && ctype != 6) || ilace != 0) return PNG_E_UNSUPPORTED;
            ch = ctype == 6 ? 4 : 3;
            have_ihdr = 1;
        } else if (!memcmp(tag, "IDAT", 4)) {
            if (!have_ihdr) return PNG_E_FORMAT;
            idat_total += clen;
        } else if (!memcmp(tag, "IEND", 4)) {
            have_iend = 1;
        }
        /* any other chunk (tEXt, pHYs, gAMA, ...): CRC checked above, content ignored */
        pos += 12 + clen;
    }
    if (!have_ihdr || !have_iend || idat_total == 0) return PNG_E_FORMAT;

    u32 stride = width * ch;
    u32 raw_len = height * (stride + 1);
    /* overflow guard: 16384*16384*4 fits u32 only barely, keep it honest */
    if (stride / ch != width || raw_len / height != stride + 1) return PNG_E_FORMAT;

    /* Pass 2: concatenate IDAT bodies. PNG splits one zlib stream across
       IDAT chunks at arbitrary byte boundaries, so they must be joined
       before inflating, not inflated one at a time. */
    u8 *zbuf = kmalloc(idat_total);
    if (!zbuf) return PNG_E_NOMEM;
    u32 zpos = 0;
    pos = 8;
    for (;;) {
        u32 clen = be32(data + pos);
        const u8 *tag = data + pos + 4;
        if (!memcmp(tag, "IDAT", 4)) { memcpy(zbuf + zpos, data + pos + 8, clen); zpos += clen; }
        if (!memcmp(tag, "IEND", 4)) break;
        pos += 12 + clen;
    }

    u8 *raw = kmalloc(raw_len);
    if (!raw) { kfree(zbuf); return PNG_E_NOMEM; }
    int r = png_zlib_inflate(zbuf, zpos, raw, raw_len);
    kfree(zbuf);
    if (r) { kfree(raw); return r; }

    u8 *px = kmalloc(height * stride);
    if (!px) { kfree(raw); return PNG_E_NOMEM; }
    r = unfilter(raw, px, stride, height, ch);
    kfree(raw);
    if (r) { kfree(px); return r; }

    *out = px; *w = width; *h = height; *channels = ch;
    return 0;
}
