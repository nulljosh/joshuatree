/* Deterministic fuzz driver for drivers/png.c and drivers/jpeg.c, run by
   tools/checks/decoder-fuzz-check.sh under -fsanitize=address,undefined.
   Feeds each decoder: (a) every valid sample truncated at every length (or
   ~200 evenly spaced lengths for large samples), (b) ~2000 deterministic
   single/multi-byte mutations per sample (fixed PRNG seed, reproducible),
   (c) a handful of handcrafted nasties. A per-input alarm() kills the
   process if any single input hangs, so a stuck case fails loudly instead
   of hanging CI.

   This is a bytes-in, does-it-crash harness: it does not check output
   correctness (tools/checks/png-host-check.sh and jpeg-host-check.sh
   already do that with known-good fixtures). Any return code from
   png_decode/jpeg_decode is fine; a crash, sanitizer report, or hang is
   not. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>

#include "png.h"
#include "jpeg.h"
#include "png_testdata.h"
#include "jpeg_testdata.h"

static sigjmp_buf g_jmp;
static volatile sig_atomic_t g_timed_out = 0;

static void on_alarm(int sig) {
    (void)sig;
    g_timed_out = 1;
    siglongjmp(g_jmp, 1);
}

/* per-input timeout in whole-process wall time via SIGALRM + longjmp; the
   decoders never call anything reentrancy-unsafe past the point we jump
   from (they only touch stack/heap state local to the one call), so a
   longjmp out of the call is safe here even though it is not safe in
   general. */
static int run_png(const unsigned char *data, unsigned len) {
    unsigned char *out; unsigned w, h, ch;
    if (sigsetjmp(g_jmp, 1)) return -1;
    alarm(1);
    int r = png_decode(data, len, &out, &w, &h, &ch);
    alarm(0);
    if (r == 0) free(out);
    return r;
}

static int run_jpeg(const unsigned char *data, unsigned len) {
    unsigned char *out; unsigned w, h, ch;
    if (sigsetjmp(g_jmp, 1)) return -1;
    alarm(1);
    int r = jpeg_decode(data, len, &out, &w, &h, &ch);
    alarm(0);
    if (r == 0) free(out);
    return r;
}

/* xorshift32, fixed seed: deterministic across runs/platforms/CI. */
static uint32_t g_rng = 0xC0FFEE01u;
static uint32_t rnd(void) {
    uint32_t x = g_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return g_rng = x;
}

typedef int (*decode_fn)(const unsigned char *, unsigned);

static long g_cases = 0;

static void save_crash(const char *dir, const char *label, const unsigned char *data, unsigned len) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s.bin", dir, label);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(data, 1, len, f); fclose(f); }
    fprintf(stderr, "CRASH/HANG saved to %s (%u bytes)\n", path, len);
}

/* Runs one input; on hang (alarm fired) reports and continues (the
   sanitizer, if a real memory bug fired, would have already aborted the
   whole process, which is the point: ASan/UBSan crashes are caught by the
   shell wrapper's exit code, this longjmp path only guards against an
   infinite loop). */
static int g_bad = 0;
static const char *g_regress_dir;

static void try_one(decode_fn fn, const unsigned char *data, unsigned len, const char *label) {
    g_cases++;
    int r = fn(data, len);
    if (r == -1 && g_timed_out) {
        g_bad = 1;
        save_crash(g_regress_dir, label, data, len);
        g_timed_out = 0;
    }
}

static void fuzz_sample(decode_fn fn, const unsigned char *orig, unsigned n,
                        const char *name, const char *regress_dir) {
    unsigned char *buf = malloc(n);
    memcpy(buf, orig, n);

    /* (a) truncation at every length (small) or ~200 evenly spaced (large) */
    if (n <= 512) {
        for (unsigned len = 0; len <= n; len++) {
            char label[128];
            snprintf(label, sizeof label, "%s_trunc_%u", name, len);
            g_regress_dir = regress_dir;
            try_one(fn, buf, len, label);
        }
    } else {
        for (int i = 0; i <= 200; i++) {
            unsigned len = (unsigned)((uint64_t)i * n / 200);
            char label[128];
            snprintf(label, sizeof label, "%s_trunc_%u", name, len);
            g_regress_dir = regress_dir;
            try_one(fn, buf, len, label);
        }
    }

    /* (b) ~2000 deterministic mutations: mix of single-byte and multi-byte
       flips at pseudo-random (but fixed-seed) offsets/values. */
    for (int i = 0; i < 2000; i++) {
        unsigned char *m = malloc(n);
        memcpy(m, orig, n);
        unsigned nmut = 1 + (rnd() % 4); /* 1..4 bytes touched */
        for (unsigned j = 0; j < nmut; j++) {
            unsigned off = rnd() % n;
            m[off] ^= (unsigned char)(rnd() & 0xFF);
        }
        char label[128];
        snprintf(label, sizeof label, "%s_mut_%d", name, i);
        g_regress_dir = regress_dir;
        try_one(fn, m, n, label);
        free(m);
    }

    free(buf);
}

/* ---- handcrafted nasties ------------------------------------------------ */

static void put_be32(unsigned char *p, unsigned v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

/* We link drivers/png.c in, which exports png_crc32; reuse it directly. */
extern unsigned png_crc32(const unsigned char *p, unsigned n);

static void fix_chunk_crc(unsigned char *c, unsigned pos) {
    unsigned clen = ((unsigned)c[pos] << 24) | ((unsigned)c[pos+1] << 16) | ((unsigned)c[pos+2] << 8) | c[pos+3];
    put_be32(c + pos + 8 + clen, png_crc32(c + pos + 4, clen + 4));
}

static void png_nasties(const char *regress_dir) {
    /* huge width/height in IHDR (with CRC fixed so it reaches the bounds check) */
    {
        unsigned n = sizeof pngt_rgb_dyn;
        unsigned char *c = malloc(n); memcpy(c, pngt_rgb_dyn, n);
        unsigned pos = 8; /* IHDR chunk starts right after the signature */
        put_be32(c + pos + 8, 0xFFFFFFFFu);      /* width */
        put_be32(c + pos + 8 + 4, 0xFFFFFFFFu);  /* height */
        fix_chunk_crc(c, pos);
        g_regress_dir = regress_dir;
        try_one(run_png, c, n, "png_huge_dims");
        free(c);
    }
    /* zero width/height */
    {
        unsigned n = sizeof pngt_rgb_dyn;
        unsigned char *c = malloc(n); memcpy(c, pngt_rgb_dyn, n);
        unsigned pos = 8;
        put_be32(c + pos + 8, 0);
        put_be32(c + pos + 8 + 4, 0);
        fix_chunk_crc(c, pos);
        g_regress_dir = regress_dir;
        try_one(run_png, c, n, "png_zero_dims");
        free(c);
    }
    /* bad chunk length: IHDR claims a length that runs past EOF */
    {
        unsigned n = sizeof pngt_rgb_dyn;
        unsigned char *c = malloc(n); memcpy(c, pngt_rgb_dyn, n);
        put_be32(c + 8, 0x7FFFFFFFu); /* clen for the first chunk */
        g_regress_dir = regress_dir;
        try_one(run_png, c, n, "png_bad_chunklen");
        free(c);
    }
    /* zlib stored block with bad LEN/NLEN complement */
    {
        unsigned n = sizeof pngt_rgb_dyn;
        unsigned char *c = malloc(n); memcpy(c, pngt_rgb_dyn, n);
        /* first IDAT chunk body starts at pos+8; corrupt the zlib header's
           byte just after the 2-byte CMF/FLG to try to shove the stream
           into a bogus stored block; harmless if this particular fixture
           isn't a stored-block one, still exercises the path with garbage. */
        unsigned pos = 8 + 25; /* IDAT chunk header, right after IHDR */
        unsigned char *body = c + pos + 8;
        body[2] = 0x00; /* BFINAL=0, BTYPE=00 (stored) */
        body[3] = 0xFF; body[4] = 0xFF; /* LEN = 0xFFFF */
        body[5] = 0xFF; body[6] = 0xFF; /* NLEN should be ~LEN, make it wrong */
        fix_chunk_crc(c, pos);
        g_regress_dir = regress_dir;
        try_one(run_png, c, n, "png_bad_stored_len");
        free(c);
    }
}

static void jpeg_nasties(const char *regress_dir) {
    /* huge SOF width/height */
    {
        unsigned n = sizeof jpegt_photo_q90_420;
        unsigned char *c = malloc(n); memcpy(c, jpegt_photo_q90_420, n);
        /* find SOF0 (0xFFC0) marker */
        for (unsigned i = 2; i + 4 < n; i++) {
            if (c[i] == 0xFF && c[i+1] == 0xC0) {
                unsigned char *body = c + i + 4; /* skip marker + 2-byte seglen */
                body[1] = 0xFF; body[2] = 0xFF; /* height */
                body[3] = 0xFF; body[4] = 0xFF; /* width */
                break;
            }
        }
        g_regress_dir = regress_dir;
        try_one(run_jpeg, c, n, "jpeg_huge_dims");
        free(c);
    }
    /* zero dimensions */
    {
        unsigned n = sizeof jpegt_photo_q90_420;
        unsigned char *c = malloc(n); memcpy(c, jpegt_photo_q90_420, n);
        for (unsigned i = 2; i + 4 < n; i++) {
            if (c[i] == 0xFF && c[i+1] == 0xC0) {
                unsigned char *body = c + i + 4;
                body[1] = 0; body[2] = 0;
                body[3] = 0; body[4] = 0;
                break;
            }
        }
        g_regress_dir = regress_dir;
        try_one(run_jpeg, c, n, "jpeg_zero_dims");
        free(c);
    }
    /* DHT with an oversubscribed / bogus BITS table (total > 256-ish nonsense) */
    {
        unsigned n = sizeof jpegt_photo_q90_420;
        unsigned char *c = malloc(n); memcpy(c, jpegt_photo_q90_420, n);
        for (unsigned i = 2; i + 4 < n; i++) {
            if (c[i] == 0xFF && c[i+1] == 0xC4) {
                unsigned char *body = c + i + 4;
                for (int k = 0; k < 16; k++) body[1 + k] = 0xFF; /* absurd per-length counts */
                break;
            }
        }
        g_regress_dir = regress_dir;
        try_one(run_jpeg, c, n, "jpeg_bad_dht");
        free(c);
    }
    /* DQT with an out-of-range table index */
    {
        unsigned n = sizeof jpegt_photo_q90_420;
        unsigned char *c = malloc(n); memcpy(c, jpegt_photo_q90_420, n);
        for (unsigned i = 2; i + 4 < n; i++) {
            if (c[i] == 0xFF && c[i+1] == 0xDB) {
                unsigned char *body = c + i + 4;
                body[0] = (body[0] & 0xF0) | 0x0F; /* table id 15, out of range */
                break;
            }
        }
        g_regress_dir = regress_dir;
        try_one(run_jpeg, c, n, "jpeg_bad_dqt_idx");
        free(c);
    }
}

int main(void) {
    signal(SIGALRM, on_alarm);

    const char *regress_dir = "tools/checks/decoder-fuzz-regress";
    /* directory is created by the shell wrapper before running us */

    fuzz_sample(run_png, pngt_rgb_dyn, sizeof pngt_rgb_dyn, "rgb_dyn", regress_dir);
    fuzz_sample(run_png, pngt_rgba_stored, sizeof pngt_rgba_stored, "rgba_stored", regress_dir);
    fuzz_sample(run_png, pngt_rgb_fixed, sizeof pngt_rgb_fixed, "rgb_fixed", regress_dir);
    fuzz_sample(run_png, pngt_pal_dyn, sizeof pngt_pal_dyn, "pal_dyn", regress_dir);
    png_nasties(regress_dir);

    fuzz_sample(run_jpeg, jpegt_photo_q90_420, sizeof jpegt_photo_q90_420, "photo_q90_420", regress_dir);
    fuzz_sample(run_jpeg, jpegt_photo_q75_444, sizeof jpegt_photo_q75_444, "photo_q75_444", regress_dir);
    fuzz_sample(run_jpeg, jpegt_photo_q50_420, sizeof jpegt_photo_q50_420, "photo_q50_420", regress_dir);
    fuzz_sample(run_jpeg, jpegt_photo_q95_444, sizeof jpegt_photo_q95_444, "photo_q95_444", regress_dir);
    fuzz_sample(run_jpeg, jpegt_gray_q85_420, sizeof jpegt_gray_q85_420, "gray_q85_420", regress_dir);
    jpeg_nasties(regress_dir);

    fprintf(stderr, "decoder-fuzz: %ld cases run\n", g_cases);
    puts(g_bad ? "DECODER FUZZ: FAIL (hang detected, see saved regress inputs)" : "DECODER FUZZ: PASS");
    return g_bad;
}
