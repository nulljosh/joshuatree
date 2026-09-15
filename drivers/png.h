#ifndef PNG_H
#define PNG_H
/* v74 (0.66.0): minimal PNG decoder, the "prerequisite bridge, part 1" the
   satellite-wallpaper goal was blocked on. Deliberately narrow: 8-bit
   truecolor (RGB, color type 2) and truecolor+alpha (RGBA, color type 6),
   non-interlaced, one IHDR + any number of IDATs + IEND. Everything a
   real map/satellite tile server or a PIL/libpng default RGB export
   produces; nothing else (palette, grayscale, 16-bit, Adam7) is
   attempted, those return PNG_E_UNSUPPORTED cleanly rather than
   misdecoding.

   Underneath is a real zlib/DEFLATE inflater (stored, fixed-Huffman and
   dynamic-Huffman blocks, RFC 1951) plus the five PNG scanline filters
   (None/Sub/Up/Average/Paeth, RFC 2083 6.6). Both chunk CRC-32s and the
   zlib Adler-32 trailer are checked, so a corrupted byte anywhere in the
   compressed data fails loudly instead of quietly producing garbage
   pixels; `pngtest` proves that path too.

   Output: a kmalloc'd buffer of *w * *h * *channels bytes, tightly packed
   rows, top-down, R,G,B[,A] byte order, exactly the layout wallpaper_rgb
   already uses. Caller kfree()s it. Returns 0 on success, a negative
   PNG_E_* code on failure with *out left 0. */

#define PNG_E_SIGNATURE   (-1)   /* not a PNG at all */
#define PNG_E_TRUNCATED   (-2)   /* ran off the end of the file/stream */
#define PNG_E_CRC         (-3)   /* chunk CRC-32 mismatch */
#define PNG_E_UNSUPPORTED (-4)   /* bit depth / color type / interlace outside scope */
#define PNG_E_NOMEM       (-5)   /* kmalloc failed */
#define PNG_E_ZLIB        (-6)   /* zlib header / DEFLATE stream / Adler-32 error */
#define PNG_E_FILTER      (-7)   /* scanline filter byte out of range */
#define PNG_E_FORMAT      (-8)   /* chunk order / size / dimensions inconsistent */

int png_decode(const unsigned char *data, unsigned int len,
               unsigned char **out, unsigned int *w, unsigned int *h,
               unsigned int *channels);

/* Exposed for tests and for any future caller with a raw zlib stream
   (not PNG-wrapped). Inflates a zlib stream into dst (exactly dst_len
   bytes expected). Returns 0 or PNG_E_ZLIB / PNG_E_TRUNCATED. */
int png_zlib_inflate(const unsigned char *src, unsigned int src_len,
                     unsigned char *dst, unsigned int dst_len);

/* CRC-32 (IEEE, the PNG one), exposed so a future PNG *writer* or any
   other checksum consumer shares the one table. */
unsigned int png_crc32(const unsigned char *p, unsigned int n);
#endif
