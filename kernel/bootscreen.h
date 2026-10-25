#ifndef BOOTSCREEN_H
#define BOOTSCREEN_H
/* kernel/bootscreen.c: the brand mark at any size, the boot splash and About. */
void gui_draw_mark_sized(int cx, int cy, int size, unsigned int ink);   /* bold copy at 48 px and under (menu bar, About) */
void gui_draw_boot_screen(void);   /* the splash: the tree draws on, then a bar if the hold passes a second */
void gui_launch_about(void);       /* the About card; returns when it closes */
#endif
