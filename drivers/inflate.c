/* DEFLATE (RFC 1951) inflate, shared. Moved out of drivers/png.c in 2.7.1
   so the PNG decoder and the ring-3 app unpacker (kernel/ring3app.c) run the
   one inflate instead of two. All three block kinds: stored, fixed Huffman,
   dynamic Huffman. No recursion, small stack tables, no allocation. */
#include "inflate.h"
#include "libc.h"

typedef unsigned int u32;
typedef unsigned char u8;

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
        if (sym < 0 || b->overrun) return INFLATE_E_DATA;
        if (sym < 256) {
            if (o->pos >= o->len) return INFLATE_E_DATA; /* more output than the caller sized for */
            o->dst[o->pos++] = (u8)sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return INFLATE_E_DATA;
            u32 len = len_base[sym] + getbits(b, len_extra[sym]);
            int dsym = huff_decode(b, distcode);
            if (dsym < 0 || dsym >= 30) return INFLATE_E_DATA;
            u32 dist = dist_base[dsym] + getbits(b, dist_extra[dsym]);
            if (b->overrun) return INFLATE_E_TRUNCATED;
            if (dist > o->pos) return INFLATE_E_DATA;          /* back-reference before start */
            if (o->pos + len > o->len) return INFLATE_E_DATA;   /* output overflow */
            /* byte-by-byte on purpose: dist < len overlaps (run-length) is legal */
            for (u32 i = 0; i < len; i++, o->pos++) o->dst[o->pos] = o->dst[o->pos - dist];
        }
    }
}

static int inflate_stored(struct bits *b, struct out *o) {
    b->bitbuf = 0; b->bitcnt = 0; /* discard to byte boundary */
    if (b->pos + 4 > b->len) return INFLATE_E_TRUNCATED;
    u32 len = b->src[b->pos] | ((u32)b->src[b->pos + 1] << 8);
    u32 nlen = b->src[b->pos + 2] | ((u32)b->src[b->pos + 3] << 8);
    b->pos += 4;
    if ((len ^ 0xFFFFu) != nlen) return INFLATE_E_DATA;
    if (b->pos + len > b->len) return INFLATE_E_TRUNCATED;
    if (o->pos + len > o->len) return INFLATE_E_DATA;
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
    if (nlen > 286 || ndist > 30) return INFLATE_E_DATA;

    u32 i;
    for (i = 0; i < ncode; i++) lengths[order[i]] = (unsigned short)getbits(b, 3);
    for (; i < 19; i++) lengths[order[i]] = 0;
    if (huff_build(&lencode, lengths, 19) != 0) return INFLATE_E_DATA;

    /* code lengths for the literal/length + distance alphabets, run-length
       coded with symbols 16 (repeat previous), 17 (zeros x3-10), 18 (zeros x11-138) */
    i = 0;
    while (i < nlen + ndist) {
        int sym = huff_decode(b, &lencode);
        if (sym < 0 || b->overrun) return INFLATE_E_DATA;
        if (sym < 16) {
            lengths[i++] = (unsigned short)sym;
        } else {
            u32 rep, val = 0;
            if (sym == 16) {
                if (i == 0) return INFLATE_E_DATA;
                val = lengths[i - 1];
                rep = 3 + getbits(b, 2);
            } else if (sym == 17) {
                rep = 3 + getbits(b, 3);
            } else {
                rep = 11 + getbits(b, 7);
            }
            if (i + rep > nlen + ndist) return INFLATE_E_DATA;
            while (rep--) lengths[i++] = (unsigned short)val;
        }
    }
    if (lengths[256] == 0) return INFLATE_E_DATA; /* no end-of-block code */
    if (huff_build(&lencode, lengths, nlen) != 0) return INFLATE_E_DATA;
    if (huff_build(&distcode, lengths + nlen, ndist) != 0) return INFLATE_E_DATA;
    return inflate_codes(b, o, &lencode, &distcode);
}

u32 inflate_adler32(const u8 *p, u32 n) {
    u32 a = 1, b = 0;
    for (u32 i = 0; i < n; i++) {
        a += p[i]; if (a >= 65521u) a -= 65521u;
        b += a;    b %= 65521u;
    }
    return (b << 16) | a;
}

int inflate_raw(const u8 *src, u32 src_len, u8 *dst, u32 dst_len) {
    struct bits b = { src, src_len, 0, 0, 0, 0 };
    struct out o = { dst, dst_len, 0 };
    u32 last;
    do {
        last = getbit(&b);
        u32 type = getbits(&b, 2);
        int r;
        if (type == 0)      r = inflate_stored(&b, &o);
        else if (type == 1) r = inflate_fixed(&b, &o);
        else if (type == 2) r = inflate_dynamic(&b, &o);
        else                return INFLATE_E_DATA;
        if (r) return r;
        if (b.overrun) return INFLATE_E_TRUNCATED;
    } while (!last);
    if (o.pos != dst_len) return INFLATE_E_DATA; /* short stream: fewer bytes than the caller promised */
    return 0;
}

int inflate_checked(const u8 *src, u32 src_len, u8 *dst, u32 dst_len, u32 sum) {
    int r = inflate_raw(src, src_len, dst, dst_len);
    if (r) return r;
    if (inflate_adler32(dst, dst_len) != sum) return INFLATE_E_DATA;
    return 0;
}
