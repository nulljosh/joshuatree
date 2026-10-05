/* Minimal PNG decoder: see png.h for the exact scope. Three layers, top
   down: chunk walk (signature, IHDR, IDAT concatenation, IEND, CRC per
   chunk), zlib wrapper (RFC 1950) around the shared DEFLATE inflate
   in drivers/inflate.c (RFC 1951, all three block kinds),
   scanline unfiltering (RFC 2083 6.6, all five filter types). No
   recursion, no tables larger than a few hundred bytes on the stack, one
   kmalloc for the inflated scanlines plus one for the pixel output. */
#include "png.h"
#include "inflate.h"
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

static u32 adler32(const u8 *p, u32 n) { return inflate_adler32(p, n); }

int png_zlib_inflate(const u8 *src, u32 src_len, u8 *dst, u32 dst_len) {
    if (src_len < 6) return PNG_E_TRUNCATED;
    /* RFC 1950 header: CM=8 (deflate), FCHECK makes CMF*256+FLG a multiple of 31,
       FDICT must be clear (PNG forbids preset dictionaries) */
    u32 cmf = src[0], flg = src[1];
    if ((cmf & 0x0F) != 8 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20)) return PNG_E_ZLIB;

    /* the stream runs from byte 2 to the last 4 bytes, which are the Adler-32 */
    int r = inflate_raw(src + 2, src_len - 6, dst, dst_len);
    if (r) return r;

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
    /* v75: color type 3 (indexed). The PLTE chunk's RGB triples are
       looked up per sample after unfiltering; bpp for the filters is 1. */
    int paletted = 0;
    const u8 *plte = 0; u32 plte_n = 0;

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
            if (depth != 8 || (ctype != 2 && ctype != 6 && ctype != 3) || ilace != 0) return PNG_E_UNSUPPORTED;
            ch = ctype == 6 ? 4 : 3;
            paletted = (ctype == 3);
            have_ihdr = 1;
        } else if (!memcmp(tag, "PLTE", 4)) {
            /* RFC 2083 4.1.2: 1..256 entries, length a multiple of 3, must
               precede the first IDAT. Only meaningful for ctype 3 here
               (a PLTE on an RGB image is a suggested-quantization hint
               and is ignored). */
            if (!have_ihdr || plte || idat_total) return PNG_E_FORMAT;
            if (clen == 0 || clen > 768 || clen % 3 != 0) return PNG_E_FORMAT;
            plte = body; plte_n = clen / 3;
        } else if (!memcmp(tag, "IDAT", 4)) {
            if (!have_ihdr) return PNG_E_FORMAT;
            if (paletted && !plte) return PNG_E_FORMAT; /* indexed with no palette to index */
            idat_total += clen;
        } else if (!memcmp(tag, "IEND", 4)) {
            have_iend = 1;
        }
        /* any other chunk (tEXt, pHYs, gAMA, ...): CRC checked above, content ignored */
        pos += 12 + clen;
    }
    if (!have_ihdr || !have_iend || idat_total == 0) return PNG_E_FORMAT;

    u32 bpp = paletted ? 1 : ch;    /* bytes per pixel as the FILTERS see them */
    u32 stride = width * bpp;        /* one scanline of encoded samples */
    u32 raw_len = height * (stride + 1);
    /* overflow guard: 16384*16384*4 fits u32 only barely, keep it honest */
    if (stride / bpp != width || raw_len / height != stride + 1) return PNG_E_FORMAT;

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
    r = unfilter(raw, px, stride, height, bpp);
    kfree(raw);
    if (r) { kfree(px); return r; }

    if (paletted) {
        /* Expand indices to RGB through PLTE. An index past the palette's
           real length is a format error per the spec, not silently
           clamped: a map tile with a 20-entry palette and a stray 200
           would otherwise paint whatever bytes follow the chunk. */
        u32 n = width * height;
        u8 *rgb = kmalloc(n * 3);
        if (!rgb) { kfree(px); return PNG_E_NOMEM; }
        for (u32 i = 0; i < n; i++) {
            u32 idx = px[i];
            if (idx >= plte_n) { kfree(px); kfree(rgb); return PNG_E_FORMAT; }
            rgb[i * 3] = plte[idx * 3]; rgb[i * 3 + 1] = plte[idx * 3 + 1]; rgb[i * 3 + 2] = plte[idx * 3 + 2];
        }
        kfree(px);
        px = rgb;
    }

    *out = px; *w = width; *h = height; *channels = ch;
    return 0;
}
