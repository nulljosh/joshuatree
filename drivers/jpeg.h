#ifndef JPEG_H
#define JPEG_H
/* v76: minimal baseline JPEG decoder, "part 1" of the same shape as
   drivers/png.c's PNG bridge. Real gap this closes, logged honestly in
   roadmap.md: every plain-HTTP satellite tile source found (Google
   mt0.google.com/vt/lyrs=s, Bing ecn.t0.tiles.virtualearth.net) serves
   JPEG, not PNG, so the map-wallpaper feature was stuck on OpenTopoMap's
   line-art tiles instead of real satellite photography until this
   existed. Both real tile URLs were fetched and their SOF0 headers
   parsed by hand before committing to this scope: both are baseline
   (marker 0xC0), 8-bit, 3-component YCbCr, 4:2:0 subsampled (Y h=2,v=2;
   Cb/Cr h=1,v=1), Huffman-coded, confirming the scope below actually
   unlocks the real feature rather than guessing at it.

   Deliberately narrow, same discipline as png.h: baseline (SOF0) DCT
   only, 8-bit sample precision, 1 (grayscale) or 3 (YCbCr) components,
   Huffman entropy coding only (not arithmetic), 4:4:4 or 4:2:0 chroma
   subsampling only, a single interleaved scan (one SOS covering every
   component, the universal shape for a baseline JPEG straight off a web
   server). Progressive (SOF2), extended-sequential (SOF1), lossless
   (SOF3), arithmetic coding (SOF9-15), 12-bit precision, CMYK/4-component
   frames, 4:2:2 or other odd subsampling ratios, restart markers (DRI
   present at all) and multi-scan (non-interleaved) baseline all return
   JPEG_E_UNSUPPORTED cleanly rather than misdecoding, the same contract
   png.c holds for interlaced/16-bit/odd-bit-depth PNGs.

   Underneath: real marker parsing (SOI/APPn/DQT/SOF0/DHT/DRI/SOS/EOI),
   canonical Huffman decode of DC/AC coefficients (the JPEG DHT format
   already lists symbols in canonical [length,order] order, so no sort
   step is needed, unlike DEFLATE's dynamic-Huffman code-length runs),
   zigzag-order dequantization, a real separable integer IDCT (8x8,
   scaled-cosine fixed-point table, the same "cite real prior art" spirit
   as this repo's other subsystems: this is the textbook naive separable
   IDCT, not libjpeg's faster AAN/islow algorithms, since this decoder has
   no perf requirement), YCbCr->RGB (ITU-R BT.601 integer coefficients),
   and nearest-neighbor chroma upsampling for subsampled components (not
   libjpeg's "fancy"/triangle-filtered upsampling; visually close but not
   bit-identical, see tools/checks/jpeg-host-check.sh for the documented
   comparison tolerance this causes against PIL's reference decode).

   Output: one kmalloc'd buffer, *w * *h * *channels bytes, tightly
   packed, top-down, R,G,B (channels=3) or grayscale (channels=1) byte
   order, the same convention png_decode uses. Caller kfree()s it.
   Returns 0 on success, a negative JPEG_E_* code on failure with *out
   left 0. */

#define JPEG_E_SIGNATURE   (-1)  /* no FFD8 SOI marker: not a JPEG at all */
#define JPEG_E_TRUNCATED   (-2)  /* ran off the end of the file/stream */
#define JPEG_E_UNSUPPORTED (-3)  /* progressive/arithmetic/12-bit/CMYK/odd subsampling/restart/multi-scan */
#define JPEG_E_NOMEM       (-4)  /* kmalloc failed */
#define JPEG_E_FORMAT      (-5)  /* marker order/length/table inconsistent */
#define JPEG_E_HUFFMAN     (-6)  /* corrupt entropy-coded segment */

int jpeg_decode(const unsigned char *data, unsigned int len,
                unsigned char **out, unsigned int *w, unsigned int *h,
                unsigned int *channels);

#endif
