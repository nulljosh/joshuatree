#include "mp3.h"
#include "stdlib.h"
#include "string.h"

#define MINIMP3_ONLY_MP3 1
#define MINIMP3_NO_SIMD 1
#define MINIMP3_IMPLEMENTATION

#define malloc(x) malloc(x)
#define free(x) free(x)
#define memcpy(d, s, n) memcpy(d, s, n)
#define memset(d, v, n) memset(d, v, n)

#include "../../third_party/minimp3/minimp3.h"

static uint32_t id3v2_skip_size(const uint8_t *buf, uint32_t len) {
    if (len < 10 || buf[0] != 'I' || buf[1] != 'D' || buf[2] != '3') return 0;
    if ((buf[3] & 0xf0) != 0 || (buf[4] & 0xf0) != 0) return 0;
    uint32_t size = ((uint32_t)buf[6] << 21) | ((uint32_t)buf[7] << 14) | ((uint32_t)buf[8] << 7) | buf[9];
    if (size > len - 10) return 0;
    return size + 10;
}

/* Everything the decoder needs that is bigger than a ring-3 program's 4KB stack lives in one heap block:
   the decoder state, the frame and sample buffers, and a stack of its own for minimp3's large scratch
   (it keeps about 15KB of float tables on the stack). The decode runs on that stack. */
#define MP3_STACK_BYTES (64 * 1024)
struct mp3_work {
    mp3dec_t dec;
    uint8_t frame[2304 + 16];
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    mp3dec_frame_info_t info;
    uint32_t frame_len;
    int scan;                         /* 1: read frame headers only (minimp3 skips decoding when pcm is NULL) */
    int samples;
    int pend_i, pend_n;               /* samples of the last decoded frame not handed out yet */
    uint32_t slot[2];                 /* [0] = argument, [1] = function: the cdecl frame the trampoline calls */
    uint8_t stack[MP3_STACK_BYTES];
};

static uint32_t step_bytes(const struct mp3_work *w) { return (uint32_t)w->info.frame_bytes; }   /* minimp3's frame_bytes already counts any junk before the frame */

static void decode_one(struct mp3_work *w) {
    w->samples = mp3dec_decode_frame(&w->dec, w->frame, (int)w->frame_len, w->scan ? 0 : w->pcm, &w->info);
}

#if defined(__i386__)
static void run_on_stack(struct mp3_work *w) {
    uint32_t *top = (uint32_t *)(w->stack + MP3_STACK_BYTES) - 4;   /* 16 bytes of headroom above the frame */
    top[0] = (uint32_t)w;
    top[1] = (uint32_t)decode_one;
    __asm__ volatile ("movl %%esp, %%ebx\n\t"
                      "movl %0, %%esp\n\t"
                      "call *4(%%esp)\n\t"
                      "movl %%ebx, %%esp"
                      : : "r"(top) : "ebx", "eax", "ecx", "edx", "memory", "cc");
}
#else
static void run_on_stack(struct mp3_work *w) { decode_one(w); }   /* host tests: the big stack is already there */
#endif

/* Copies the frame at pos into the padded buffer and decodes it. Returns samples (0 = none). */
static int decode_at(struct mp3_work *w, const uint8_t *buf, uint32_t len, uint32_t pos) {
    uint32_t remaining = len - pos;
    uint32_t n = remaining < sizeof(w->frame) - 16 ? remaining : sizeof(w->frame) - 16;
    memcpy(w->frame, buf + pos, n);
    memset(w->frame + n, 0, sizeof(w->frame) - n);
    w->frame_len = n;
    run_on_stack(w);
    return w->samples;
}

/* Walks the frame headers from pos: returns the frame's sample count and its byte length, 0 at the end. */
static int scan_frame(struct mp3_work *w, const uint8_t *buf, uint32_t len, uint32_t pos, uint32_t *bytes) {
    if (len - pos < 4) return 0;
    w->scan = 1;
    int samples = decode_at(w, buf, len, pos);
    w->scan = 0;
    if (samples <= 0 || w->info.frame_bytes <= 0 || step_bytes(w) > len - pos) return 0;
    *bytes = step_bytes(w);
    return samples;
}

int mp3_open(struct mp3 *m, const uint8_t *buf, uint32_t len) {
    if (!buf || !len) return MP3_E_EMPTY;
    uint32_t skip = id3v2_skip_size(buf, len);
    if (skip >= len || len - skip < 4) return MP3_E_EMPTY;

    struct mp3_work *w = (struct mp3_work *)malloc(sizeof *w);
    if (!w) return MP3_E_EMPTY;
    mp3dec_init(&w->dec); w->pend_i = w->pend_n = 0;
    int samples = decode_at(w, buf, len, skip);
    if (!samples || w->info.hz < 4000 || w->info.hz > 44100) { free(w); return MP3_E_UNSUP; }

    m->buf = buf; m->len = len;
    m->pos = skip + w->info.frame_offset;
    m->rate = w->info.hz; m->channels = w->info.channels;
    m->decoder = w;
    m->next = 0;
    uint32_t total = 0, p = m->pos, b;                 /* length: add up every frame's samples */
    int fs;
    while ((fs = scan_frame(w, buf, len, p, &b)) > 0) { total += (uint32_t)fs; p += b; }
    m->frames = total;
    mp3dec_init(&w->dec);                              /* the first-frame probe above dirtied nothing, but start clean */
    return total ? MP3_OK : MP3_E_EMPTY;
}

uint32_t mp3_read8(struct mp3 *m, uint8_t *out, uint32_t n) {
    if (!m->decoder || !out || !n) return 0;
    struct mp3_work *w = (struct mp3_work *)m->decoder;
    uint32_t total = 0;
    while (total < n) {
        if (w->pend_i >= w->pend_n) {                      /* frame used up: decode the next one */
            if (m->len - m->pos < 4) break;
            int samples = decode_at(w, m->buf, m->len, m->pos);
            if (samples <= 0) break;
            if (w->info.hz < 4000 || w->info.hz > 44100) break;
            if (w->info.frame_bytes <= 0 || step_bytes(w) > m->len - m->pos) break;
            m->pos += step_bytes(w);
            w->pend_i = 0; w->pend_n = samples;
        }
        int i = w->pend_i++, v;                            /* a frame can straddle two calls: keep its tail */
        if (m->channels == 2) { int l = w->pcm[i * 2], r = w->pcm[i * 2 + 1]; v = ((l + r) / 2 >> 8) + 128; }
        else v = (w->pcm[i] >> 8) + 128;
        out[total++] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
    }
    m->next += total;
    return total;
}

void mp3_rewind(struct mp3 *m) {
    if (!m->decoder) return;
    struct mp3_work *w = (struct mp3_work *)m->decoder;
    mp3dec_init(&w->dec); w->pend_i = w->pend_n = 0;
    m->pos = id3v2_skip_size(m->buf, m->len);
    m->next = 0;
}

/* Decodes the frame at m->pos into pend (and steps m->pos past it). Returns 0 or -1 at the end. */
static int load_frame(struct mp3 *m, struct mp3_work *w) {
    int samples = decode_at(w, m->buf, m->len, m->pos);
    if (samples <= 0 || w->info.frame_bytes <= 0 || step_bytes(w) > m->len - m->pos) return -1;
    m->pos += step_bytes(w);
    w->pend_i = 0; w->pend_n = samples;
    return 0;
}

int mp3_seek(struct mp3 *m, uint32_t at) {
    if (!m->decoder) return MP3_E_EMPTY;
    struct mp3_work *w = (struct mp3_work *)m->decoder;
    if (at >= m->frames) at = m->frames ? m->frames - 1 : 0;
    uint32_t hist[256], nh = 0;                        /* starts of the last 256 frames passed: where priming can begin */
    uint32_t p = id3v2_skip_size(m->buf, m->len), cum = 0, b;
    int fs;
    while ((fs = scan_frame(w, m->buf, m->len, p, &b)) > 0 && cum + (uint32_t)fs <= at) {
        hist[nh++ & 255] = p; cum += (uint32_t)fs; p += b;
    }
    /* A frame leans on up to 511 bytes of main data from the frames before it (the bit reservoir). Headers and
       side info are not main data, so tiny frames need several times that many file bytes: decode from about
       2KB back and throw that audio away; the target frame then decodes properly. */
    mp3dec_init(&w->dec); w->pend_i = w->pend_n = 0;
    uint32_t first = nh > 256 ? nh - 256 : 0, from = p;
    for (uint32_t k = nh; k > first; k--) { uint32_t q = hist[(k - 1) & 255]; if (p - q > 2048) break; from = q; }
    for (uint32_t q = from; q < p; ) {
        decode_at(w, m->buf, m->len, q);               /* output ignored */
        if (w->info.frame_bytes <= 0) break;
        q += step_bytes(w);
    }
    m->pos = p;
    if (load_frame(m, w) != 0) { m->next = m->frames; return MP3_E_EMPTY; }
    w->pend_i = (int)(at - cum < (uint32_t)w->pend_n ? at - cum : (uint32_t)w->pend_n);
    m->next = at;
    return MP3_OK;
}

void mp3_close(struct mp3 *m) { if (m->decoder) { free(m->decoder); m->decoder = 0; } }
