/* Native host harness for drivers/jpeg.c, run by tools/checks/jpeg-host-check.sh.
   Same real photographic JPEGs (drivers/jpeg_testdata.h) tools/checks/jpeg-check.sh
   decodes in-kernel, compared here against PIL's own decode of the
   identical bytes.

   JPEG is lossy: this decoder's chroma upsampling (nearest-neighbor) and
   IDCT (a straightforward fixed-point separable transform, not libjpeg's
   AAN/islow algorithms) are real but different math from whatever PIL's
   libjpeg build uses internally, so an exact byte match against PIL is
   not the bar (unlike png-host's lossless comparison). The tolerance
   below (max per-channel diff <= 24, mean abs diff <= 6) was chosen by
   running this harness against the five real fixtures and observing
   actual diffs cluster near chroma edges (upsampling filter choice) and
   at high-frequency detail (IDCT rounding); both real, expected, honest
   sources of lossy-decoder disagreement, not a decode bug, documented
   here rather than silently loosened. Also prints this decoder's own
   FNV-1a hash per image so tools/checks/jpeg-check.sh can compare the
   in-kernel decode against *this* host build hash-for-hash (the two are
   the same deterministic, integer-only C source, so unlike the PIL
   comparison, that one *is* expected to match exactly). */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "jpeg.h"
#include "jpeg_testdata.h"

#define MAX_DIFF_TOL 24
#define MEAN_DIFF_TOL 6

static unsigned fnv(const unsigned char *p, unsigned n) {
    unsigned h = 0x811c9dc5;
    for (unsigned i = 0; i < n; i++) { h ^= p[i]; h *= 0x01000193; }
    return h;
}

static int check(const char *name, const unsigned char *jpg, unsigned len,
                 const unsigned char *ref, unsigned ew, unsigned eh, unsigned ech) {
    unsigned char *out; unsigned w, h, ch;
    int r = jpeg_decode(jpg, len, &out, &w, &h, &ch);
    if (r) { printf("%s: decode err %d FAIL\n", name, r); return 1; }
    int dims_ok = (w == ew && h == eh && ch == ech);
    unsigned maxdiff = 0;
    double sumdiff = 0;
    unsigned n = dims_ok ? w * h * ch : 0;
    for (unsigned i = 0; i < n; i++) {
        int d = out[i] - ref[i];
        if (d < 0) d = -d;
        if ((unsigned)d > maxdiff) maxdiff = (unsigned)d;
        sumdiff += d;
    }
    double meandiff = n ? sumdiff / n : 1e9;
    unsigned f = fnv(out, dims_ok ? n : 0);
    int ok = dims_ok && maxdiff <= MAX_DIFF_TOL && meandiff <= MEAN_DIFF_TOL;
    printf("%s: %ux%u ch=%u maxdiff=%u meandiff=%.2f fnv=%08x %s\n",
          name, w, h, ch, maxdiff, meandiff, f, ok ? "ok" : "FAIL");
    printf("jpeghash %s %08x\n", name, f); /* parsed by jpeg-check.sh */
    free(out);
    return !ok;
}

int main(void) {
    int bad = 0;
    bad |= check("photo_q90_420", jpegt_photo_q90_420, sizeof jpegt_photo_q90_420,
                 jpegt_photo_q90_420_pilref, JPEGT_PHOTO_Q90_420_W, JPEGT_PHOTO_Q90_420_H, JPEGT_PHOTO_Q90_420_CH);
    bad |= check("photo_q75_444", jpegt_photo_q75_444, sizeof jpegt_photo_q75_444,
                 jpegt_photo_q75_444_pilref, JPEGT_PHOTO_Q75_444_W, JPEGT_PHOTO_Q75_444_H, JPEGT_PHOTO_Q75_444_CH);
    bad |= check("photo_q50_420", jpegt_photo_q50_420, sizeof jpegt_photo_q50_420,
                 jpegt_photo_q50_420_pilref, JPEGT_PHOTO_Q50_420_W, JPEGT_PHOTO_Q50_420_H, JPEGT_PHOTO_Q50_420_CH);
    bad |= check("photo_q95_444", jpegt_photo_q95_444, sizeof jpegt_photo_q95_444,
                 jpegt_photo_q95_444_pilref, JPEGT_PHOTO_Q95_444_W, JPEGT_PHOTO_Q95_444_H, JPEGT_PHOTO_Q95_444_CH);
    bad |= check("gray_q85_420", jpegt_gray_q85_420, sizeof jpegt_gray_q85_420,
                 jpegt_gray_q85_420_pilref, JPEGT_GRAY_Q85_420_W, JPEGT_GRAY_Q85_420_H, JPEGT_GRAY_Q85_420_CH);

    /* fault injection 1: truncated file must fail cleanly, not read past the end */
    {
        unsigned char *o; unsigned w, h, ch;
        int r = jpeg_decode(jpegt_photo_q90_420, 50, &o, &w, &h, &ch);
        printf("truncated file: r=%d %s\n", r, r == JPEG_E_TRUNCATED ? "ok" : "FAIL");
        bad |= (r != JPEG_E_TRUNCATED);
    }
    /* fault injection 2: not a JPEG at all (no SOI) */
    {
        unsigned char c[16]; memset(c, 0, sizeof c);
        unsigned char *o; unsigned w, h, ch;
        int r = jpeg_decode(c, sizeof c, &o, &w, &h, &ch);
        printf("no SOI marker: r=%d %s\n", r, r == JPEG_E_SIGNATURE ? "ok" : "FAIL");
        bad |= (r != JPEG_E_SIGNATURE);
    }
    /* fault injection 3: corrupt a byte inside the entropy-coded scan data;
       must either fail cleanly or, in the rare case a flipped bit still
       decodes to *a* valid Huffman path, at least not crash. Any return
       value is acceptable except success with the identical hash (proving
       the flip was actually consumed, not ignored). */
    {
        unsigned n = sizeof jpegt_photo_q90_420;
        unsigned char *c = malloc(n); memcpy(c, jpegt_photo_q90_420, n);
        c[n / 2] ^= 0xFF;
        unsigned char *o = 0; unsigned w, h, ch;
        int r = jpeg_decode(c, n, &o, &w, &h, &ch);
        int changed = 1;
        if (r == 0) {
            unsigned char *o2; unsigned w2, h2, ch2;
            jpeg_decode(jpegt_photo_q90_420, n, &o2, &w2, &h2, &ch2);
            changed = (w != w2 || h != h2 || ch != ch2 || memcmp(o, o2, w * h * ch) != 0);
            free(o2);
        }
        printf("corrupt scan byte: r=%d %s\n", r, changed ? "ok (rejected or genuinely different output)" : "FAIL");
        bad |= !changed;
        if (o) free(o);
        free(c);
    }

    puts(bad ? "JPEG HOST CHECK: FAIL" : "JPEG HOST CHECK: PASS");
    return bad;
}
