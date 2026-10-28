#ifndef UTF8_H
#define UTF8_H
/* 6.78.1: the Terminal draws one byte a cell as the character of that number, DejaVu Sans Mono's Latin-1, so text on its
   way to the screen is folded from UTF-8: a whole sequence for U+0080 to U+00FF becomes that one byte, anything past
   Latin-1 one '?', and a stray byte '?'. A sequence cut short is dropped, and the byte that cut it is read afresh.
   *st holds the bytes still owed in its top 8 bits and the character so far below them; 0 between characters.
   Gives the byte to keep, or -1 while a sequence is still open. The UART still gets every byte as it came. */
static inline int utf8_fold(unsigned *st, unsigned char b) {
    unsigned need = *st >> 24, cp = *st & 0xFFFFFF;
    if (need && (b & 0xC0) == 0x80) {
        cp = cp << 6 | (b & 0x3F);
        if (--need) { *st = need << 24 | cp; return -1; }
        *st = 0;
        return cp <= 0xFF ? (int)cp : '?';
    }
    *st = 0;
    if (b < 0x80) return b;
    if (b >= 0xC2 && b <= 0xF4) {   /* a lead byte: how many follow */
        need = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : 1;
        *st = need << 24 | (b & (0x3Fu >> need));
        return -1;
    }
    return '?';
}
#endif
