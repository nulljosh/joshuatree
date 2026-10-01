#ifndef LIBJT_TEXT_H
#define LIBJT_TEXT_H
/* libjt: antialiased text for ring-3 apps. Draws DejaVu Sans glyphs
   (pre-rendered from the kernel's own font data by tools/gen/gen_user_text.c,
   see user/libjt/aafont.h) into the window framebuffer, alpha-blending each
   glyph's coverage over whatever pixel is already there, so text sits on any
   background with smooth edges. Text is bytes, Latin-1: ASCII plus 0xB0, the
   degree sign. DISPLAY has digits, '-', degree and space only. */
#include "jtsys.h"

enum { JT_FACE_BODY = 0, JT_FACE_BOLD = 1, JT_FACE_DISPLAY = 2 };

/* Pixel width of s in the given face. */
int jt_text_width(int face, const char *s);
/* Line box height, and the baseline's distance from the line top. */
int jt_text_height(int face);
int jt_text_ascent(int face);
/* Draws s with its line top at y and pen starting at x, colour 0x00RRGGBB.
   Clipped to the window. Returns the x after the last glyph. */
int jt_text_draw(struct jt_window_info *w, int face, int x, int y, unsigned rgb, const char *s);

#endif
