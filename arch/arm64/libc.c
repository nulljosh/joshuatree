/* The few libc calls the TrueType rasterizer (drivers/ttf.c) and the compiler itself make. Built with -fno-builtin so
   the loops below stay loops instead of being turned back into calls to themselves. */
void *memcpy(void *dst, const void *src, unsigned int n) {
    unsigned char *d = dst; const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}
void *memmove(void *dst, const void *src, unsigned int n) {
    unsigned char *d = dst; const unsigned char *s = src;
    if (d < s) while (n--) *d++ = *s++;
    else { d += n; s += n; while (n--) *--d = *--s; }
    return dst;
}
void *memset(void *dst, int c, unsigned int n) {
    unsigned char *d = dst;
    while (n--) *d++ = (unsigned char)c;
    return dst;
}
int memcmp(const void *a, const void *b, unsigned int n) {
    const unsigned char *x = a, *y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x - *y;
    return 0;
}
unsigned int strlen(const char *s) { unsigned int n = 0; while (s[n]) n++; return n; }
