/* AVI reader for the Movie app: motion-JPEG video plus optional PCM audio, parsed straight out of a
   file held in memory. Hands back the chunks in file order; it never reads outside the buffer. */
#ifndef JT_AVI_H
#define JT_AVI_H
#include <stdint.h>

#define AVI_OK        0
#define AVI_E_FORMAT -1  /* not RIFF/AVI, or the headers run past the file */
#define AVI_E_UNSUP  -2  /* video is not MJPG, audio is not 8/16-bit PCM, or the size is silly */
#define AVI_E_EMPTY  -3  /* no video stream or no movi list */

#define AVI_VIDEO 1
#define AVI_AUDIO 2

struct avi {
    const uint8_t *buf;
    uint32_t len, movi, movi_end, pos;
    uint32_t width, height, us_per_frame;
    uint32_t arate;                 /* 0 when the clip has no sound */
    uint16_t achannels, abits;
    uint8_t vstream, astream;
};
struct avi_chunk { int kind; const uint8_t *data; uint32_t len; };

int avi_open(struct avi *a, const uint8_t *buf, uint32_t len);
void avi_rewind(struct avi *a);
/* Next video or audio chunk: 1 got one, 0 end of file (a cut-off last chunk counts as the end). */
int avi_next(struct avi *a, struct avi_chunk *c);
/* Audio chunk bytes to 8-bit unsigned mono, the format SYS_AUDIO takes. Returns samples written. */
uint32_t avi_audio8(const struct avi *a, const uint8_t *in, uint32_t inlen, uint8_t *out, uint32_t outmax);
#endif
