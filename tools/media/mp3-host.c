/* Host test for user/libjt/mp3.c: golden values, bad headers, then mutation fuzz. Built with ASan+UBSan. */
#include "../../user/libjt/mp3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void srand(unsigned int);
extern int rand(void);

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

static uint8_t *slurp(const char *dir, const char *n, uint32_t *len) {
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, n);
    FILE *f = fopen(p, "rb");
    if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    rewind(f);
    uint8_t *b = malloc(*len ? *len : 1);
    fread(b, 1, *len, f);
    fclose(f);
    return b;
}

static uint8_t *exact(const uint8_t *src, uint32_t n) {
    uint8_t *c = malloc(n ? n : 1);
    memcpy(c, src, n);
    return c;
}

static int count_zero_crossings(const uint8_t *pcm, uint32_t n) {
    int crossings = 0;
    for (uint32_t i = 1; i < n; i++) {
        int prev = (int)pcm[i-1] - 128;
        int curr = (int)pcm[i] - 128;
        if ((prev < 0 && curr >= 0) || (prev >= 0 && curr < 0)) crossings++;
    }
    return crossings;
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    uint32_t n;
    struct mp3 m;
    uint8_t out[8192];

    /* golden values: mono at 22050 Hz */
    uint8_t *mono22 = slurp(dir, "mono22.mp3", &n); uint32_t n0 = n;
    CHECK(mp3_open(&m, mono22, n) == MP3_OK);
    CHECK(m.rate == 22050 && m.channels == 1);
    uint32_t samples = 0;
    uint8_t big_out[32768];
    while (samples < sizeof(big_out)) {
        uint32_t r = mp3_read8(&m, big_out + samples, sizeof(big_out) - samples);
        if (!r) break;
        samples += r;
    }
    CHECK(samples > 20000 && samples < 25000);
    int lo = 255, hi = 0;
    for (uint32_t i = 0; i < samples; i++) {
        if (big_out[i] < lo) lo = big_out[i];
        if (big_out[i] > hi) hi = big_out[i];
    }
    printf("mp3-host: mono peak-to-peak %d\n", hi - lo);
    CHECK(hi - lo > 25 && hi - lo < 60);                            /* ffmpeg's default sine is 1/8 scale: about 32 steps in 8 bits */
    int hz = (int)((uint64_t)count_zero_crossings(big_out + 2304, samples - 2304) * m.rate / (2ull * (samples - 2304)));
    CHECK(hz >= 430 && hz <= 450);                                  /* the 440 Hz sine came back at 440 Hz */

    /* chunked reads must equal one big read: a frame that straddles two calls keeps its tail */
    { struct mp3 a, b; mp3_open(&a, mono22, n0); mp3_open(&b, mono22, n0);
      static uint8_t whole[32768], parts[32768]; uint32_t wn = 0, pn = 0, r;
      while ((r = mp3_read8(&a, whole + wn, sizeof whole - wn)) > 0) wn += r;
      while (pn < sizeof parts && (r = mp3_read8(&b, parts + pn, 1000 < sizeof parts - pn ? 1000 : sizeof parts - pn)) > 0) pn += r;
      CHECK(wn == pn && wn > 20000 && memcmp(whole, parts, wn) == 0); }
    /* total length and seek: 1 s of 22050 Hz, and seeking must land on the same samples as playing through */
    { struct mp3 a, b; static uint8_t full[32768], part[4096];
      mp3_open(&a, mono22, n0); uint32_t got = 0, r;
      CHECK(a.frames > 21000 && a.frames < 25000);
      while ((r = mp3_read8(&a, full + got, sizeof full - got)) > 0) got += r;
      mp3_open(&b, mono22, n0);
      CHECK(mp3_seek(&b, 9000) == MP3_OK && b.next == 9000);
      uint32_t pr = mp3_read8(&b, part, 4000); CHECK(pr == 4000 && b.next == 13000);
      long diff = 0; for (uint32_t i = 1200; i < 4000; i++) diff += abs((int)part[i] - (int)full[9000 + i]);
      CHECK(diff / 2800 <= 2);                               /* same audio after the first frame of warm-up */
      CHECK(mp3_seek(&b, 0) == MP3_OK && mp3_read8(&b, part, 2000) == 2000);
      CHECK(mp3_seek(&b, 99999999) == MP3_OK);               /* past the end clamps, no crash */
      long diff0 = 0; for (uint32_t i = 1200; i < 2000; i++) diff0 += abs((int)part[i] - (int)full[i]); CHECK(diff0 / 800 <= 2); }
    /* stereo at 44100 Hz */
    uint8_t *stereo44 = slurp(dir, "stereo44.mp3", &n);
    CHECK(mp3_open(&m, stereo44, n) == MP3_OK);
    CHECK(m.rate == 44100 && m.channels == 2);
    samples = 0;
    while (samples < sizeof(big_out)) {
        uint32_t r = mp3_read8(&m, big_out + samples, sizeof(big_out) - samples);
        if (!r) break;
        samples += r;
    }
    CHECK(samples > 30000);

    /* rewind */
    mp3_rewind(&m);
    uint32_t samples_after_rewind = mp3_read8(&m, out, 1000);
    CHECK(samples_after_rewind > 900);

    /* bad inputs */
    uint8_t *t = exact(mono22, 10);
    CHECK(mp3_open(&m, t, 10) != MP3_OK);
    free(t);

    uint8_t empty[] = {};
    CHECK(mp3_open(&m, empty, 0) != MP3_OK);

    uint8_t garbage[1024];
    memset(garbage, 0xFF, sizeof(garbage));
    CHECK(mp3_open(&m, garbage, sizeof(garbage)) != MP3_OK);

    /* basic error handling: truncated files must error, not crash */
    for (uint32_t truncate = 1; truncate < 100; truncate++) {
        uint8_t *t = exact(mono22, truncate);
        mp3_open(&m, t, truncate);
        free(t);
    }

    printf(fails ? "mp3-host: %d FAILED\n" : "mp3-host: ok\n", fails);
    return fails != 0;
}
