/* Host test for user/libjt/avi.c: chunk order and counts, every frame decodes with the real JPEG
   decoder, bad headers, truncation, then mutation fuzz under ASan+UBSan. */
#include "../../user/libjt/avi.h"
#include "../../drivers/jpeg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static uint8_t *slurp(const char *dir, const char *n, uint32_t *len) {
    char p[512]; snprintf(p, sizeof p, "%s/%s", dir, n);
    FILE *f = fopen(p, "rb"); if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END); *len = ftell(f); rewind(f);
    uint8_t *b = malloc(*len); fread(b, 1, *len, f); fclose(f); return b;
}
static uint8_t *exact(const uint8_t *s, uint32_t n) { uint8_t *c = malloc(n ? n : 1); memcpy(c, s, n); return c; }
static int decode_ok(const uint8_t *d, uint32_t n, uint32_t w, uint32_t h) {
    unsigned char *px = 0; unsigned int dw, dh, ch;
    int r = jpeg_decode(d, n, &px, &dw, &dh, &ch);
    int ok = r == 0 && dw == w && dh == h; free(px); return ok;
}
static void walk(const char *dir, const char *name, uint32_t w, uint32_t h, int nv, int audio, uint32_t arate, uint32_t abytes) {
    uint32_t n; uint8_t *b = slurp(dir, name, &n); struct avi a; struct avi_chunk c;
    CHECK(avi_open(&a, b, n) == AVI_OK);
    CHECK(a.width == w && a.height == h && a.us_per_frame == 100000 && a.arate == (audio ? arate : 0));
    int v = 0; uint32_t bytes = 0, samples = 0; uint8_t out[16384]; int video_first_of_pair = 1;
    while (avi_next(&a, &c) == 1) {
        if (c.kind == AVI_VIDEO) { CHECK(decode_ok(c.data, c.len, w, h)); v++; video_first_of_pair = 1; }
        else { bytes += c.len; samples += avi_audio8(&a, c.data, c.len, out, sizeof out); CHECK(video_first_of_pair); }
    }
    CHECK(v == nv); CHECK(bytes == abytes); CHECK(audio ? samples > 0 : samples == 0);
    avi_rewind(&a); CHECK(avi_next(&a, &c) == 1 && c.kind == AVI_VIDEO);          /* rewind restarts */
    free(b);
}
int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    walk(dir, "av8.avi", 64, 48, 10, 1, 11025, 10 * 1102);
    walk(dir, "av16s.avi", 48, 32, 6, 1, 22050, 6 * 2205 * 4);   /* rec groups, stereo 16-bit */
    walk(dir, "silent.avi", 32, 32, 4, 0, 0, 0);
    uint32_t n; uint8_t *src = slurp(dir, "av8.avi", &n); struct avi a; struct avi_chunk c;
    /* 8-bit mono audio passes through, 16-bit stereo averages */
    CHECK(avi_open(&a, src, n) == AVI_OK); int got = 0; uint8_t o[2048];
    while (avi_next(&a, &c) == 1 && !got) if (c.kind == AVI_AUDIO) { CHECK(avi_audio8(&a, c.data, c.len, o, sizeof o) == c.len && memcmp(o, c.data, c.len) == 0); got = 1; }
    CHECK(got);
    { struct avi st = { .achannels = 2, .abits = 8 }; const uint8_t in[4] = { 10, 30, 200, 100 }; uint8_t ou[2];
      CHECK(avi_audio8(&st, in, 4, ou, 2) == 2 && ou[0] == 20 && ou[1] == 150); }          /* 8-bit stereo is the plain average */
    /* bad input: every cut before the movi list ends the headers must fail cleanly, later cuts must still open */
    for (uint32_t cut = 0; cut < 400 && cut < n; cut++) { uint8_t *t = exact(src, cut); (void)avi_open(&a, t, cut); free(t); }
    { uint8_t *t = exact(src, n / 2); CHECK(avi_open(&a, t, n / 2) == AVI_OK); int k = 0; while (avi_next(&a, &c) == 1) { k++; if (c.kind == AVI_VIDEO) CHECK(decode_ok(c.data, c.len, a.width, a.height)); }   /* a cut-off frame must not come back */ CHECK(k > 0 && k < 20); free(t); }   /* half a file plays what is there */
    { avi_rewind(&a); CHECK(avi_open(&a, src, n) == AVI_OK && avi_next(&a, &c) == 1);
      uint32_t cut = (uint32_t)(c.data - src) + c.len / 2; uint8_t *t = exact(src, cut);
      CHECK(avi_open(&a, t, cut) == AVI_OK && avi_next(&a, &c) == 0); free(t); }              /* cut inside the first frame: nothing comes back */
    { uint8_t *t = exact(src, 200); CHECK(avi_open(&a, t, 200) != AVI_OK); free(t); }
    { uint8_t *t = exact(src, 11); CHECK(avi_open(&a, t, 11) == AVI_E_FORMAT); free(t); }
    uint8_t *m = exact(src, n); memcpy(m, "RIFX", 4); CHECK(avi_open(&a, m, n) == AVI_E_FORMAT); free(m);
    m = exact(src, n); { uint8_t *p = memmem(m, n, "MJPG", 4); CHECK(p); if (p) memcpy(p, "H264", 4); } CHECK(avi_open(&a, m, n) == AVI_E_UNSUP); free(m);
    /* fuzz */
    srand(7); uint32_t sn; uint8_t *sil = slurp(dir, "silent.avi", &sn); uint8_t *files[2] = { src, sil }; uint32_t fl[2] = { n, sn };
    for (int it = 0; it < 6000; it++) {
        int f = it & 1; uint8_t *c2 = exact(files[f], fl[f]); int flips = 1 + rand() % 6;
        while (flips--) c2[rand() % (rand() % 2 ? fl[f] : 600)] = rand();
        uint32_t cl = rand() % 4 == 0 ? rand() % fl[f] : fl[f]; uint8_t *e = exact(c2, cl); free(c2);
        if (avi_open(&a, e, cl) == AVI_OK) { int guard = 0; while (avi_next(&a, &c) == 1 && guard++ < 100000) if (c.kind == AVI_AUDIO) avi_audio8(&a, c.data, c.len, o, sizeof o); }
        free(e);
    }
    printf(fails ? "avi-host: %d FAILED\n" : "avi-host: ok\n", fails);
    return fails != 0;
}
