/* Native host harness for drivers/png.c, run by tools/png-host-check.sh.
   Same three embedded PNGs and the same pixel-for-pixel comparison against
   wallpaper_rgb that the in-kernel `pngtest` does, plus fault-injection
   cases that prove the CRC-32, Adler-32 and DEFLATE paths actually reject
   corruption instead of returning garbage. Fast iteration for decoder
   work; the kernel-side test remains the one that ships. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "png.h"
#include "wallpaper.h"
#include "png_testdata.h"

static unsigned fnv(const unsigned char *p, unsigned n) {
    unsigned h = 0x811c9dc5;
    for (unsigned i = 0; i < n; i++) { h ^= p[i]; h *= 0x01000193; }
    return h;
}

static int check(const char *name, const unsigned char *png, unsigned len,
                 unsigned x0, unsigned y0, unsigned ew, unsigned eh, unsigned ech, unsigned efnv) {
    unsigned char *out; unsigned w, h, ch;
    int r = png_decode(png, len, &out, &w, &h, &ch);
    if (r) { printf("%s: decode err %d FAIL\n", name, r); return 1; }
    unsigned mism = 0;
    for (unsigned y = 0; y < h; y++)
        for (unsigned x = 0; x < w; x++) {
            const unsigned char *e = &wallpaper_rgb[((y0 + y) * WALLPAPER_W + x0 + x) * 3];
            const unsigned char *d = out + (y * w + x) * ch;
            if (e[0] != d[0] || e[1] != d[1] || e[2] != d[2]) mism++;
            if (ch == 4 && d[3] != ((x * 7 + y * 3) & 0xff)) mism++;
        }
    unsigned f = fnv(out, w * h * ch);
    int ok = (w == ew && h == eh && ch == ech && mism == 0 && f == efnv);
    printf("%s: %ux%u ch=%u mismatches=%u fnv=%08x want=%08x %s\n", name, w, h, ch, mism, f, efnv, ok ? "ok" : "FAIL");
    free(out);
    return !ok;
}

static void put_be32(unsigned char *p, unsigned v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

/* Corrupt one byte inside the first IDAT body, then recompute that chunk's
   CRC so the corruption has to be caught by zlib/Adler-32, not the CRC. */
static int corrupt_past_crc(void) {
    unsigned n = sizeof pngt_rgb_dyn;
    unsigned char *c = malloc(n); memcpy(c, pngt_rgb_dyn, n);
    unsigned pos = 8 + 25; /* after IHDR */
    unsigned clen = (c[pos] << 24) | (c[pos+1] << 16) | (c[pos+2] << 8) | c[pos+3];
    c[pos + 8 + clen / 2] ^= 0x55;
    put_be32(c + pos + 8 + clen, png_crc32(c + pos + 4, clen + 4));
    unsigned char *o; unsigned w, h, ch;
    int r = png_decode(c, n, &o, &w, &h, &ch);
    free(c); if (!r) free(o);
    printf("corrupt IDAT byte, CRC repaired: r=%d %s\n", r, (r == PNG_E_ZLIB || r == PNG_E_TRUNCATED) ? "ok (zlib/adler caught it)" : "FAIL");
    return !(r == PNG_E_ZLIB || r == PNG_E_TRUNCATED);
}

int main(void) {
    int bad = 0;
    bad |= check("rgb_dyn", pngt_rgb_dyn, sizeof pngt_rgb_dyn, PNGT_RGB_DYN_X0, PNGT_RGB_DYN_Y0, PNGT_RGB_DYN_W, PNGT_RGB_DYN_H, 3, PNGT_RGB_DYN_FNV);
    bad |= check("rgba_stored", pngt_rgba_stored, sizeof pngt_rgba_stored, PNGT_RGBA_STORED_X0, PNGT_RGBA_STORED_Y0, PNGT_RGBA_STORED_W, PNGT_RGBA_STORED_H, 4, PNGT_RGBA_STORED_FNV);
    bad |= check("rgb_fixed", pngt_rgb_fixed, sizeof pngt_rgb_fixed, PNGT_RGB_FIXED_X0, PNGT_RGB_FIXED_Y0, PNGT_RGB_FIXED_W, PNGT_RGB_FIXED_H, 3, PNGT_RGB_FIXED_FNV);

    unsigned n = sizeof pngt_rgb_dyn;
    unsigned char *c = malloc(n); memcpy(c, pngt_rgb_dyn, n); c[5000] ^= 0x55;
    unsigned char *o; unsigned w, h, ch;
    int r = png_decode(c, n, &o, &w, &h, &ch);
    printf("corrupt IDAT byte: r=%d %s\n", r, r == PNG_E_CRC ? "ok (crc caught it)" : "FAIL");
    bad |= (r != PNG_E_CRC);
    free(c);

    bad |= corrupt_past_crc();

    r = png_decode(pngt_rgb_dyn, 4000, &o, &w, &h, &ch);
    printf("truncated file: r=%d %s\n", r, r == PNG_E_TRUNCATED ? "ok" : "FAIL");
    bad |= (r != PNG_E_TRUNCATED);

    puts(bad ? "PNG HOST CHECK: FAIL" : "PNG HOST CHECK: PASS");
    return bad;
}
