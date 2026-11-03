/* MP4 reader for Blackcomb's video (13.4.0): the moov box's sample tables, so a clip can be fetched a sample at a time
   with HTTP ranges instead of held whole. The first video track (H.264, avcC) and the first audio track (AAC, its
   AudioSpecificConfig) are kept; everything else is skipped. It never reads outside the buffer it is given. */
#ifndef JT_MP4_H
#define JT_MP4_H
#include <stdint.h>

#define MP4_OK        0
#define MP4_E_FORMAT -1   /* no moov in the buffer, or a box runs past it */
#define MP4_E_EMPTY  -2   /* no H.264 video track */

#define MP4_VIDEO 1
#define MP4_AUDIO 2

struct mp4_track {
    int kind;                         /* 0 when absent */
    uint32_t timescale, nsamples;
    uint32_t width, height;           /* video */
    uint32_t rate, channels;          /* audio */
    const uint8_t *cfg; uint32_t cfglen;   /* avcC's payload, or AAC's AudioSpecificConfig */
    const uint8_t *stsz, *stco, *stsc, *stts;   /* the tables' payloads, past version and flags */
    uint32_t stsz_len, stco_len, stsc_len, stts_len;
    int co64;
};
struct mp4 { struct mp4_track video, audio; };

/* buf holds the file from its first byte far enough to include all of moov (a clip made for the web puts it first). */
int mp4_open(struct mp4 *m, const uint8_t *buf, uint32_t len);

/* Samples of one track in order: where each starts in the file, how long it is, and its decode time in the track's
   timescale. */
struct mp4_iter {
    const struct mp4_track *t;
    uint32_t i, chunk, left_in_chunk, stsc_i, stts_i, stts_left, stts_delta;
    uint64_t off, dts;
};
void mp4_iter_init(struct mp4_iter *it, const struct mp4_track *t);
int mp4_iter_next(struct mp4_iter *it, uint64_t *off, uint32_t *size, uint64_t *dts);   /* 1 got one, 0 the end */
#endif
