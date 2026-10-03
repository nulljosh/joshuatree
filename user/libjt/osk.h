#ifndef LIBJT_OSK_H
#define LIBJT_OSK_H
/* libjt: the phone on-screen keyboard for ring-3 apps. Five rows of 40 px
   pinned to the bottom of the window: 1234567890, qwertyuiop, asdfghjkl
   (half a cell in), zxcvbnm and backspace (one cell in), then space 60%,
   enter 20%, done 20%. A cell is the window width over ten. A tap arrives
   as a click, so the program hands the click to jt_osk_hit and treats a
   non-zero answer as that key. Done answers Esc. Only draw it in phone
   mode: the launcher passes "phone" as argv[1] when the kernel booted
   with boot_to_phone. */
#include "../jtsys.h"

#define JT_OSK_ROW_H 40
#define JT_OSK_ROWS 5

/* Pixel height the keyboard takes at the window bottom. */
int jt_osk_height(void);
/* Draws the keyboard over the bottom jt_osk_height() rows of w. */
void jt_osk_draw(struct jt_window_info *w);
/* Key for a click at x,y: an ASCII byte, 8 backspace, JT_KEY_ENTER, JT_KEY_ESC
   (done). 0 when the click is not on a key. */
int jt_osk_hit(struct jt_window_info *w, int x, int y);

#endif
