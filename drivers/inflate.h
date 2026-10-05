#ifndef INFLATE_H
#define INFLATE_H
/* 2.7.1: the one DEFLATE (RFC 1951) inflate in the kernel, shared by the PNG
   decoder (through its zlib wrapper) and by ring3app_seed(), which unpacks the
   compressed app binaries embedded in kernel.elf. Raw deflate only: no zlib
   header, no checksum. Callers that want a checksum use inflate_adler32. */

#define INFLATE_E_TRUNCATED (-2)  /* ran off the end of the compressed data (same value as PNG_E_TRUNCATED) */
#define INFLATE_E_DATA      (-6)  /* bad stream, or output size differs from dst_len (same value as PNG_E_ZLIB) */

/* Inflates a raw deflate stream into dst. The stream must produce exactly
   dst_len bytes. Returns 0 or a negative INFLATE_E_* code. */
int inflate_raw(const unsigned char *src, unsigned int src_len,
                unsigned char *dst, unsigned int dst_len);

/* Adler-32 (RFC 1950) of n bytes, the checksum the embedded apps carry. */
unsigned int inflate_adler32(const unsigned char *p, unsigned int n);

/* inflate_raw, then require the output's Adler-32 to equal sum. This is what ring3app.c uses for
   the embedded apps, and what tools/checks/user-compress-check.py exercises on the host. */
int inflate_checked(const unsigned char *src, unsigned int src_len,
                    unsigned char *dst, unsigned int dst_len, unsigned int sum);
#endif
