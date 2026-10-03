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

int mp3_open(struct mp3 *m, const uint8_t *buf, uint32_t len) {
    if (!buf || !len) return MP3_E_EMPTY;

    uint32_t skip = id3v2_skip_size(buf, len);
    if (skip >= len) return MP3_E_EMPTY;

    const uint8_t *mp3_start = buf + skip;
    uint32_t mp3_len = len - skip;
    if (mp3_len < 4) return MP3_E_EMPTY;

    mp3dec_t *dec = (mp3dec_t *)malloc(sizeof(mp3dec_t));
    if (!dec) return MP3_E_EMPTY;

    mp3dec_init(dec);

    uint8_t padded_buf[2304 + 16];
    uint32_t to_copy = mp3_len < sizeof(padded_buf) - 16 ? mp3_len : sizeof(padded_buf) - 16;
    memcpy(padded_buf, mp3_start, to_copy);
    memset(padded_buf + to_copy, 0, sizeof(padded_buf) - to_copy);

    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    mp3dec_frame_info_t info;
    int samples = mp3dec_decode_frame(dec, padded_buf, to_copy, pcm, &info);

    if (!samples || info.hz < 4000 || info.hz > 44100) {
        free(dec);
        return MP3_E_UNSUP;
    }

    m->buf = buf;
    m->len = len;
    m->pos = skip + info.frame_offset;
    m->rate = info.hz;
    m->channels = info.channels;
    m->decoder = (void *)dec;
    return MP3_OK;
}

uint32_t mp3_read8(struct mp3 *m, uint8_t *out, uint32_t n) {
    if (!m->decoder || !out || !n) return 0;

    mp3dec_t *dec = (mp3dec_t *)m->decoder;
    uint32_t total = 0;

    while (total < n && m->pos < m->len) {
        uint32_t remaining = m->len - m->pos;
        if (remaining < 4) break;

        uint8_t frame_buf[2304 + 16];
        uint32_t to_copy = remaining < sizeof(frame_buf) ? remaining : sizeof(frame_buf) - 16;
        memcpy(frame_buf, m->buf + m->pos, to_copy);
        memset(frame_buf + to_copy, 0, sizeof(frame_buf) - to_copy);

        int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(dec, frame_buf, to_copy, pcm, &info);

        if (samples <= 0) break;
        if (info.hz < 4000 || info.hz > 44100) break;
        if (info.frame_bytes > remaining) break;

        m->pos += info.frame_bytes;

        for (int i = 0; i < samples && total < n; i++) {
            int v;
            if (m->channels == 2) {
                int l = pcm[i * 2], r = pcm[i * 2 + 1];
                v = ((l + r) / 2 >> 8) + 128;
            } else {
                v = (pcm[i] >> 8) + 128;
            }
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            out[total++] = (uint8_t)v;
        }
    }

    return total;
}

void mp3_rewind(struct mp3 *m) {
    if (!m->decoder) return;
    mp3dec_t *dec = (mp3dec_t *)m->decoder;
    mp3dec_init(dec);
    uint32_t skip = id3v2_skip_size(m->buf, m->len);
    m->pos = skip;
}
