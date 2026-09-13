/* Dumps the real IBM CP437 8x16 font out of VGA hardware instead of hand-
   authoring glyph bitmaps blind, exactly the trap this item was deferred
   over. The font QEMU's VGA BIOS loads for text mode lives in plane 2 of
   video memory; reconfiguring the Sequencer/Graphics Controller to read
   that plane through the legacy 0xA0000 window and copying it out is a
   standard, well-documented technique (OSDev wiki's "VGA Fonts" article),
   not a guess. Each glyph's hardware slot is 32 bytes even though an 8x16
   font only uses the first 16, the BIOS reserves room for taller fonts. */
#include "font.h"
#include "window.h"

typedef unsigned char  u8;
typedef unsigned short u16;

static inline u8   inb(u16 p)        { u8 v; __asm__ volatile ("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline void outb(u16 p, u8 v) { __asm__ volatile ("outb %0,%1" :: "a"(v), "Nd"(p)); }

#define SEQ_INDEX 0x3C4
#define SEQ_DATA  0x3C5
#define GC_INDEX  0x3CE
#define GC_DATA   0x3CF

static u8 glyphs[256 * 16];

void font_init(void) {
    outb(SEQ_INDEX, 0x02); u8 seq2 = inb(SEQ_DATA);
    outb(SEQ_INDEX, 0x04); u8 seq4 = inb(SEQ_DATA);
    outb(GC_INDEX,  0x04); u8 gc4  = inb(GC_DATA);
    outb(GC_INDEX,  0x05); u8 gc5  = inb(GC_DATA);
    outb(GC_INDEX,  0x06); u8 gc6  = inb(GC_DATA);

    outb(SEQ_INDEX, 0x02); outb(SEQ_DATA, 0x04); /* map mask: plane 2 only */
    outb(SEQ_INDEX, 0x04); outb(SEQ_DATA, 0x07); /* sequential addressing, extended memory */
    outb(GC_INDEX,  0x04); outb(GC_DATA, 0x02);  /* read map select: plane 2 */
    outb(GC_INDEX,  0x05); outb(GC_DATA, 0x00);  /* read mode 0, no odd/even */
    outb(GC_INDEX,  0x06); outb(GC_DATA, 0x00);  /* map memory at 0xA0000, 128K window */

    const volatile u8 *vga_mem = (const volatile u8 *)0xA0000;
    for (int ch = 0; ch < 256; ch++)
        for (int row = 0; row < 16; row++)
            glyphs[ch * 16 + row] = vga_mem[ch * 32 + row]; /* 32-byte stride per glyph slot */

    outb(SEQ_INDEX, 0x02); outb(SEQ_DATA, seq2);
    outb(SEQ_INDEX, 0x04); outb(SEQ_DATA, seq4);
    outb(GC_INDEX,  0x04); outb(GC_DATA, gc4);
    outb(GC_INDEX,  0x05); outb(GC_DATA, gc5);
    outb(GC_INDEX,  0x06); outb(GC_DATA, gc6);
}

void font_draw_char(unsigned char c, int x, int y, unsigned int fg, int bg) {
    const u8 *glyph = glyphs + (unsigned int)c * 16;
    for (int row = 0; row < 16; row++) {
        u8 bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            int on = (bits >> (7 - col)) & 1;
            if (on) window_pixel(x + col, y + row, fg);
            else if (bg >= 0) window_pixel(x + col, y + row, (unsigned int)bg);
        }
    }
}

void font_draw_string(const char *s, int x, int y, unsigned int fg, int bg) {
    int cx = x;
    for (; *s; s++) {
        if (*s == '\n') { y += 16; cx = x; continue; }
        font_draw_char((unsigned char)*s, cx, y, fg, bg);
        cx += 8;
    }
}
