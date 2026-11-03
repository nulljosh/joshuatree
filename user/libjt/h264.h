/* H.264 decoder for Blackcomb's video (13.4.0): Constrained Baseline, the profile web video falls back to and YouTube's
   360p format uses. CAVLC, I and P slices, 4:2:0, 8 bits, frames only. No B slices, CABAC, interlace, FMO or ASO: a stream
   that needs them is refused, not guessed at. Written from ITU-T H.264 (clauses 7, 8 and 9.2); checked frame for frame
   against ffmpeg (tools/checks/h264-check.py). Memory comes from the caller's allocator, once, when the first SPS arrives. */
#ifndef JT_H264_H
#define JT_H264_H

struct h264;
struct h264_pic {
    const unsigned char *y, *u, *v;   /* planes of the cropped picture's full MB area */
    unsigned stride, cstride;         /* bytes per row: luma, chroma */
    unsigned x, y0, w, h;             /* the crop: picture is w x h starting at (x, y0) in luma samples */
};
/* alloc returns zeroed or not, it does not matter; it is called a few times, never freed (a bump heap is fine). */
struct h264 *h264_new(void *(*alloc)(unsigned long n));
/* One NAL unit, no start code and no length prefix. 0 ok, <0 refused (the reason is h264_error()). */
int h264_nal(struct h264 *d, const unsigned char *nal, unsigned len);
/* The access unit is complete (an MP4 sample ends): deblocks and keeps the picture, 1 with *p filled, 0 when nothing
   was decoded. The planes stay valid until the next call to h264_nal. */
int h264_end(struct h264 *d, struct h264_pic *p);
const char *h264_error(const struct h264 *d);
#endif
