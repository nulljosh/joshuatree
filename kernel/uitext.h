#ifndef UITEXT_H
#define UITEXT_H
/* kernel/uitext.c: the i386 antialiased UI text, split out of kernel.c. */
int gui_aa_advance(unsigned char c);
void gui_aa_char(unsigned char c, int px, int py, unsigned int fg, int bg, int cell);
void gui_aa_char_mono(unsigned char c, int px, int py, unsigned int fg, int bg, int cell);   /* font_set_aa_mono: the terminal grid */
char *wx_put_int(char *o, int v);
void uitext_cmdline(const char *cl);   /* 4.27.0: fontbreak=NAME, the preload check's failure path */
int uitext_fonts_preload(void);        /* 4.27.0: every face parsed and tried once at boot; one serial line */
#endif
