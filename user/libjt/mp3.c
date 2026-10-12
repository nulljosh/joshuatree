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

/* Copies one ID3 text frame's body (encoding byte first) into out as plain ASCII, stopping at its NUL. */
static void id3_text(const uint8_t *d, uint32_t n, char *out, int cap) {
    int o = 0;
    uint32_t i = 1;
    if (n < 1 || cap < 1) { if (cap > 0) out[0] = 0; return; }
    uint8_t enc = d[0];
    if (enc == 1 || enc == 2) {                         /* UTF-16: a BOM says which byte of each pair is the low one */
        int le = 1;
        if (n >= 3 && d[1] == 0xFE && d[2] == 0xFF) { le = 0; i = 3; }
        else if (n >= 3 && d[1] == 0xFF && d[2] == 0xFE) i = 3;
        else if (enc == 2) le = 0;
        for (; i + 1 < n && o < cap - 1; i += 2) {
            uint8_t lo = d[i + (le ? 0 : 1)], hi = d[i + (le ? 1 : 0)];
            if (lo == 0 && hi == 0) break;
            out[o++] = hi || lo < 0x20 || lo >= 0x7F ? '?' : (char)lo;
        }
    } else {
        for (; i < n && o < cap - 1; i++) {
            uint8_t c = d[i];
            if (c == 0) break;
            if (enc == 3 && c >= 0x80 && c < 0xC0) continue;   /* a UTF-8 continuation byte: the '?' already stands for it */
            out[o++] = c < 0x20 || c >= 0x7F ? '?' : (char)c;
        }
    }
    while (o > 0 && out[o - 1] == ' ') o--;
    out[o] = 0;
}

int mp3_id3(const uint8_t *buf, uint32_t len, char *title, int tn, char *artist, int an) {
    if (tn > 0) title[0] = 0;
    if (an > 0) artist[0] = 0;
    if (len < 10 || buf[0] != 'I' || buf[1] != 'D' || buf[2] != '3') return 0;
    uint8_t ver = buf[3];
    if (ver < 2 || ver > 4) return 0;
    uint32_t end = 10 + (((uint32_t)buf[6] << 21) | ((uint32_t)buf[7] << 14) | ((uint32_t)buf[8] << 7) | buf[9]);
    if (end > len) end = len;                           /* a short read of a long tag: use what is here */
    uint32_t p = 10;
    int idl = ver == 2 ? 3 : 4, hdr = ver == 2 ? 6 : 10;
    if (ver >= 3 && (buf[5] & 0x40) && p + 4 <= end) {  /* an extended header comes first: skip it */
        uint32_t x = ((uint32_t)buf[10] << 24) | ((uint32_t)buf[11] << 16) | ((uint32_t)buf[12] << 8) | buf[13];
        if (ver == 4) x = ((uint32_t)buf[10] << 21) | ((uint32_t)buf[11] << 14) | ((uint32_t)buf[12] << 7) | buf[13];
        else x += 4;
        if (x > end - p) return 0;
        p += x;
    }
    while (p + hdr <= end && buf[p] != 0) {
        const uint8_t *f = buf + p;
        uint32_t sz = ver == 2 ? ((uint32_t)f[3] << 16) | ((uint32_t)f[4] << 8) | f[5]
                    : ver == 3 ? ((uint32_t)f[4] << 24) | ((uint32_t)f[5] << 16) | ((uint32_t)f[6] << 8) | f[7]
                               : ((uint32_t)f[4] << 21) | ((uint32_t)f[5] << 14) | ((uint32_t)f[6] << 7) | f[7];
        uint32_t body = p + (uint32_t)hdr;
        uint32_t avail = end - body;
        uint32_t use = sz < avail ? sz : avail;         /* the last frame may be cut off by a short read */
        int is_t = idl == 3 ? !memcmp(f, "TT2", 3) : !memcmp(f, "TIT2", 4);
        int is_a = idl == 3 ? !memcmp(f, "TP1", 3) : !memcmp(f, "TPE1", 4);
        if (is_t && tn > 0) id3_text(buf + body, use, title, tn);
        if (is_a && an > 0) id3_text(buf + body, use, artist, an);
        if (sz > avail) break;
        p = body + sz;
    }
    return tn > 0 && title[0] != 0;
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

/* Decodes the frame at m->pos into pend (and steps m->pos past it). Returns 0 or -1 at the end.
   A frame minimp3 cannot decode (damaged side info, or main data that lived in frames we never had, as at the start
   of a song cut out of a stream) comes back with no samples but a byte count to skip. mp3_open's header scan counted
   that frame, so it plays as the silence its header says it lasts: the clock keeps matching `frames`, the song does
   not stop there for good, and the frame is not decoded a second time after the decoder has already taken its bytes. */
static int load_frame(struct mp3 *m, struct mp3_work *w) {
    int samples = decode_at(w, m->buf, m->len, m->pos);
    if (w->info.frame_bytes <= 0 || step_bytes(w) > m->len - m->pos) return -1;
    if (w->info.hz < 4000 || w->info.hz > 44100) return -1;
    if (samples <= 0) {
        w->scan = 1; samples = decode_at(w, m->buf, m->len, m->pos); w->scan = 0;   /* header only: how long the frame lasts */
        if (samples <= 0 || samples * 2 > MINIMP3_MAX_SAMPLES_PER_FRAME) return -1;
        memset(w->pcm, 0, sizeof w->pcm);
    }
    m->pos += step_bytes(w);
    w->pend_i = 0; w->pend_n = samples;
    return 0;
}

uint32_t mp3_read8(struct mp3 *m, uint8_t *out, uint32_t n) {
    if (!m->decoder || !out || !n) return 0;
    struct mp3_work *w = (struct mp3_work *)m->decoder;
    uint32_t total = 0;
    while (total < n) {
        if (w->pend_i >= w->pend_n) {                      /* frame used up: decode the next one */
            if (m->len - m->pos < 4 || load_frame(m, w) != 0) break;
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
