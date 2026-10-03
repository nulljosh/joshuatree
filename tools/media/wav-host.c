/* Host test for user/libjt/wav.c: golden values, bad headers, then mutation fuzz. Built with ASan+UBSan. */
#include "../../user/libjt/wav.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
static uint8_t *slurp(const char *dir, const char *n, uint32_t *len) {
    char p[512]; snprintf(p, sizeof p, "%s/%s", dir, n);
    FILE *f = fopen(p, "rb"); if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END); *len = ftell(f); rewind(f);
    uint8_t *b = malloc(*len ? *len : 1); fread(b, 1, *len, f); fclose(f); return b;
}
static uint8_t *exact(const uint8_t *src, uint32_t n) { uint8_t *c = malloc(n ? n : 1); memcpy(c, src, n); return c; } /* heap copy so ASan sees any overrun */
int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : "."; uint32_t n; struct wav w; uint8_t out[8192];
    uint8_t *m8 = slurp(dir, "m8.wav", &n);
    CHECK(wav_open(&w, m8, n) == WAV_OK); CHECK(w.frames == 800 && w.rate == 8000 && w.channels == 1);
    CHECK(wav_read8(&w, 0, out, 800) == 800); CHECK(memcmp(out, w.pcm, 800) == 0);   /* 8-bit mono is passthrough */
    CHECK(wav_read8(&w, 790, out, 100) == 10); CHECK(wav_read8(&w, 800, out, 10) == 0); CHECK(wav_read8(&w, 0xFFFFFFFFu, out, 10) == 0);
    uint8_t *m16 = slurp(dir, "m16.wav", &n);
    CHECK(wav_open(&w, m16, n) == WAV_OK && w.frames == 2205);
    wav_read8(&w, 0, out, 2205); CHECK(out[0] == 128);                              /* sin(0) = silence = 128 */
    int lo = 255, hi = 0; for (int i = 0; i < 2205; i++) { if (out[i] < lo) lo = out[i]; if (out[i] > hi) hi = out[i]; }
    CHECK(hi > 180 && hi < 200 && lo < 76 && lo > 56);                              /* 0.5 amplitude, not clipped, not flat */
    uint8_t *s16 = slurp(dir, "s16.wav", &n);
    CHECK(wav_open(&w, s16, n) == WAV_OK && w.frames == 4410 && w.channels == 2);
    wav_read8(&w, 0, out, 4410); hi = 0; for (int i = 0; i < 4410; i++) if (out[i] > hi) hi = out[i];
    CHECK(hi >= 172 && hi <= 180);                                                    /* (0.5+0.25)/2 = 0.375 -> ~176 max */
    uint8_t *s8 = slurp(dir, "s8.wav", &n); CHECK(wav_open(&w, s8, n) == WAV_OK && w.rate == 11025);
    wav_read8(&w, 0, out, 1102);
    for (int i = 0; i < 1102; i++) CHECK(out[i] == (w.pcm[i * 2] + w.pcm[i * 2 + 1]) / 2);   /* stereo 8-bit is the plain average */
    /* bad inputs */
    uint32_t sn = 0; uint8_t *src = slurp(dir, "m8.wav", &sn);
    for (uint32_t cut = 0; cut < 45; cut++) { uint8_t *t = exact(src, cut); CHECK(wav_open(&w, t, cut) != WAV_OK); free(t); }   /* truncated before any data */
    uint8_t *t = exact(src, sn - 100); CHECK(wav_open(&w, t, sn - 100) == WAV_OK && w.frames == 700); free(t);                    /* short data clamps */
    memcpy(out, src, 44); out[22] = 6; { uint8_t *c = exact(out, 44); CHECK(wav_open(&w, c, 44) != WAV_OK); free(c); }            /* 6 channels */
    { uint8_t *c = exact(src, sn); c[40] = c[41] = c[42] = c[43] = 0xFF; CHECK(wav_open(&w, c, sn) == WAV_OK && w.frames == 800); free(c); } /* huge data size */
    { uint8_t *c = exact(src, sn); c[16] = c[17] = c[18] = c[19] = 0xFF; CHECK(wav_open(&w, c, sn) != WAV_OK); free(c); }           /* huge fmt size */
    { uint8_t *c = exact(src, sn); c[24] = c[25] = c[26] = c[27] = 0; CHECK(wav_open(&w, c, sn) != WAV_OK); free(c); }             /* rate 0 */
    /* fuzz: flip bytes, every outcome must be clean and every read in bounds (ASan is the judge) */
    srand(1); uint8_t *files[] = { m8, m16, s8, s16 };
    for (int it = 0; it < 20000; it++) {
        uint8_t *f = files[it & 3]; uint32_t fl = (it & 3) == 0 ? 844 : 0;
        if (!fl) { fl = (it & 3) == 1 ? 44 + 4410 : (it & 3) == 2 ? 44 + 2204 : 44 + 17640; }
        uint8_t *c = exact(f, fl); int flips = 1 + rand() % 4;
        while (flips--) c[rand() % (fl < 64 ? fl : 64)] = rand();       /* header-heavy mutation */
        uint32_t cl = rand() % 3 == 0 ? rand() % fl : fl;
        uint8_t *e = exact(c, cl); free(c);
        if (wav_open(&w, e, cl) == WAV_OK) { wav_read8(&w, rand() % (w.frames + 5), out, 4096); wav_read8(&w, 0, out, sizeof out); }
        free(e);
    }
    printf(fails ? "wav-host: %d FAILED\n" : "wav-host: ok\n", fails);
    return fails != 0;
}
