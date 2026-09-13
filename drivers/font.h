#ifndef FONT_H
#define FONT_H
/* Not hand-authored glyph data (the exact "garbled but technically renders"
   trap this item was deferred over): dumps the real 8x16 font QEMU's VGA
   BIOS already loaded into hardware plane 2 for text mode, a standard,
   well-documented technique, not a guess. Must run while still in plain
   VGA text mode, before any Bochs VBE mode switch (window_open), the
   planar font memory isn't guaranteed reachable the same way afterward. */
void font_init(void);

/* Draws one 8x16 glyph at pixel (x,y) into the currently open window.c
   surface. bg -1 means transparent (skip background pixels, draw only the
   glyph's foreground bits). */
void font_draw_char(unsigned char c, int x, int y, unsigned int fg, int bg);
void font_draw_string(const char *s, int x, int y, unsigned int fg, int bg);
#endif
