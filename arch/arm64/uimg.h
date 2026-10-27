/* uimg.h (6.51.0): is this a user image main.c's user_load can take? Since an app can come off the card (JT/APPS/NAME.APP),
   the image is untrusted bytes, so the whole check is here, with no kernel state, where tools/fuzz-host/fuzz_uimg.c can reach
   it. The format is user_c.ld's: the image, then a 16 byte trailer { "JTU1", bytes of code (a page multiple), .rela.dyn start,
   .rela.dyn end }, every relocation R_AARCH64_RELATIVE on an aligned word inside the image, its addend inside the pages
   user_load maps (the image, rounded up, and `zero` more pages). 1 it is, and *code_end, *rela_s, *rela_e are set; 0 it is not. */
#ifndef UIMG_H
#define UIMG_H
#define UIMG_MAGIC 0x3155544a
static inline unsigned uimg_rd32(const char *p) { const unsigned char *u = (const unsigned char *)p; return u[0] | u[1] << 8 | u[2] << 16 | (unsigned)u[3] << 24; }
static inline unsigned long uimg_rd64(const char *p) { return uimg_rd32(p) | (unsigned long)uimg_rd32(p + 4) << 32; }
static inline int uimg_ok(const char *img, unsigned long len, unsigned long max, unsigned long zero,
                          unsigned *code_end, unsigned *rela_s, unsigned *rela_e) {
    if (len < 32 || len > max || (len & 3)) return 0;
    unsigned magic = uimg_rd32(img + len - 16), ce = uimg_rd32(img + len - 12), rs = uimg_rd32(img + len - 8), re = uimg_rd32(img + len - 4);
    if (magic != UIMG_MAGIC || !ce || (ce & 4095) || ce > len - 16 || rs > re || re > len - 16 || (rs & 7) || (re - rs) % 24) return 0;
    unsigned long span = ((len + 4095) / 4096 + zero) * 4096;
    for (unsigned long r = rs; r < re; r += 24) {
        unsigned long off = uimg_rd64(img + r), info = uimg_rd64(img + r + 8), add = uimg_rd64(img + r + 16);
        if (info != 1027 /* R_AARCH64_RELATIVE */ || (off & 7) || off > len - 24 || add > span) return 0;
        if (off + 8 > rs && off < re) return 0;   /* one that rewrites the list: the next entry would point anywhere (found by fuzz_uimg.c) */
    }
    *code_end = ce; *rela_s = rs; *rela_e = re;
    return 1;
}
#endif
