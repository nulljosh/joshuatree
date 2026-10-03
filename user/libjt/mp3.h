/* MP3 reader for the Music app: parses MP3 frames in memory and hands back 8-bit unsigned
   mono PCM, the only format SYS_AUDIO takes. Never reads outside the buffer it is given. */
#ifndef JT_MP3_H
#define JT_MP3_H
#include <stdint.h>

#define MP3_OK        0
#define MP3_E_FORMAT -1  /* not a valid MP3, or corrupted frames */
#define MP3_E_UNSUP  -2  /* rate not 4000..44100 Hz */
#define MP3_E_EMPTY  -3  /* no frames found */

struct mp3 {
    const uint8_t *buf;
    uint32_t len;
    uint32_t pos;         /* current position in buffer */
    uint32_t rate;
    uint16_t channels;
    uint32_t frames;      /* total mono samples at rate, found by a header-only scan at open */
    uint32_t next;        /* the sample index mp3_read8 will return next */
    void *decoder;        /* opaque minimp3 decoder state */
};

int mp3_open(struct mp3 *m, const uint8_t *buf, uint32_t len);
/* Decodes forward, downmixes to 8-bit unsigned mono; returns samples written, 0 at end. */
uint32_t mp3_read8(struct mp3 *m, uint8_t *out, uint32_t n);
void mp3_rewind(struct mp3 *m);
/* Jumps so the next mp3_read8 returns sample `at` (clamped to the end). Returns 0 or a negative MP3_E_*. */
int mp3_seek(struct mp3 *m, uint32_t at);
/* Frees the decoder block. Safe on a struct that never opened. */
void mp3_close(struct mp3 *m);
#endif
