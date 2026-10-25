#ifndef CLOCKICON_H
#define CLOCKICON_H
/* kernel/clockicon.c: the Clock tile as a live analog face. */
void gui_icon_clock(int cx, int cy, int s, unsigned int bg);   /* the plain tile; the face goes on live */
void gui_clock_draw_hands(int cx_center, int cy_bottom, int size);
int gui_clock_tick(int *seen);                                   /* 1 when the minute moved since *seen */
void gui_icon_overlay(int icon, int cx_center, int cy_bottom, int size);   /* the live layer over a cached tile */
#endif
