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


/* Mono face: DejaVu Sans Mono, ASCII 0x20..0x7E only (anything else draws
   '?'), one size, every glyph the same 8 px advance. Lives in its own object
   (user/libjt/mono.c, own atlas) so only apps that call it pay for it; this
   is JT_FACE_MONO's role, kept out of the shared face table on purpose, since
   every ring-3 image has to fit the 128KB window (28KB until 2026-10-01). */
#define JT_MONO_ADV 8
int jt_mono_height(void);
/* Draws s at pen x, line top y; each glyph advances JT_MONO_ADV. Returns the x after the last glyph. */
int jt_mono_draw(struct jt_window_info *w, int x, int y, unsigned rgb, const char *s);

#endif
