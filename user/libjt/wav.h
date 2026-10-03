/* WAV reader for the Music app: parses a RIFF/WAVE file in memory and hands back 8-bit unsigned
   mono PCM, the only format SYS_AUDIO takes. Never reads outside the buffer it is given. */
#ifndef JT_WAV_H
#define JT_WAV_H
#include <stdint.h>

#define WAV_OK        0
#define WAV_E_FORMAT -1  /* not RIFF/WAVE, or a chunk runs past the file */
#define WAV_E_UNSUP  -2  /* not 8/16-bit PCM, 1-2 channels, 4000..44100 Hz */
#define WAV_E_EMPTY  -3  /* no fmt or no data */

struct wav {
    const uint8_t *pcm;   /* first sample byte inside the caller's buffer */
    uint32_t frames;      /* one frame = one sample per channel */
    uint32_t rate;
    uint16_t channels, bits;
};

int wav_open(struct wav *w, const uint8_t *buf, uint32_t len);
/* Writes up to n mono 8-bit unsigned samples starting at frame `from`; returns how many. */
uint32_t wav_read8(const struct wav *w, uint32_t from, uint8_t *out, uint32_t n);
#endif
