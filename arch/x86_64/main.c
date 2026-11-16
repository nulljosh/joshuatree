/* x86-64, slice 1 (M0): the 64-bit kernel boots and says so, on the serial port and on the screen.

   boot.S has already put the CPU in long mode with the first 4 GiB identity mapped. kmain proves it from C: it reads
   back CR0, CR4 and EFER, does 64-bit arithmetic that a 32-bit build would get wrong, and writes above 2 GiB through the
   new page tables. Then the screen: the 8x16 font is copied out of VGA plane 2 where the BIOS left it (drivers/font.c
   does the same on i386), the Bochs display (QEMU's -vga std, PCI 1234:1111) is switched to 1024x768 at 32 bits through
   its I/O ports, its framebuffer is found in PCI BAR0, and the same lines are drawn there on the calm blue the other
   ports start on. tools/checks/x86_64-m0-check.py reads both. No interrupts yet: slice 2 brings the IDT and a timer. */
typedef unsigned char u8; typedef unsigned short u16; typedef unsigned int u32; typedef unsigned long long u64;
_Static_assert(sizeof(void *) == 8 && sizeof(long) == 8, "this is the 64-bit kernel");

static inline void outb(u16 p, u8 v) { __asm__ volatile ("outb %0,%1" :: "a"(v), "Nd"(p)); }
static inline void outw(u16 p, u16 v) { __asm__ volatile ("outw %0,%1" :: "a"(v), "Nd"(p)); }
static inline void outl(u16 p, u32 v) { __asm__ volatile ("outl %0,%1" :: "a"(v), "Nd"(p)); }
static inline u8 inb(u16 p) { u8 v; __asm__ volatile ("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline u32 inl(u16 p) { u32 v; __asm__ volatile ("inl %1,%0" : "=a"(v) : "Nd"(p)); return v; }

#define COM1 0x3F8
static void serial_init(void) {
    outb(COM1 + 1, 0); outb(COM1 + 3, 0x80); outb(COM1, 1); outb(COM1 + 1, 0);   /* 115200 baud */
    outb(COM1 + 3, 0x03); outb(COM1 + 2, 0xC7); outb(COM1 + 4, 0x03);            /* 8N1, FIFOs, DTR and RTS */
}
static void serial_putc(char c) { for (int i = 0; i < 100000 && !(inb(COM1 + 5) & 0x20); i++) { } outb(COM1, (u8)c); }

/* the screen: a text cursor over a 32-bit framebuffer, 8x16 cells drawn at twice their size */
static u8 glyphs[256 * 16];
static volatile u32 *fb; static u32 fb_w, fb_h; static int cx, cy;
#define BLUE 0x003B6EA5u
#define INK  0x00FFFFFFu
static void fb_putc(char c) {
    if (!fb) return;
    if (c == '\n') { cx = 0; cy++; return; }
    const u8 *g = glyphs + (u8)c * 16;
    for (int r = 0; r < 16; r++) for (int b = 0; b < 8; b++) if (g[r] & (0x80 >> b))
        for (int dy = 0; dy < 2; dy++) for (int dx = 0; dx < 2; dx++)
            fb[(u32)(24 + cy * 34 + r * 2 + dy) * fb_w + (u32)(24 + cx * 16 + b * 2 + dx)] = INK;
    cx++;
}
static void kputc(char c) { serial_putc(c); fb_putc(c); }
static void kputs(const char *s) { while (*s) kputc(*s++); }
static void khex(u64 v) { kputs("0x"); for (int i = 60; i >= 0; i -= 4) { if (i && !(v >> i)) continue; kputc("0123456789abcdef"[(v >> i) & 15]); } }
static void kdec(u64 v) { char t[21]; int k = 0; do t[k++] = (char)('0' + v % 10); while (v /= 10); while (k) kputc(t[--k]); }

static void font_dump(void) {   /* plane 2 of VGA memory holds the text-mode font: read it, then put the registers back */
    u8 s2, s4, g4, g5, g6;
    outb(0x3C4, 2); s2 = inb(0x3C5); outb(0x3C4, 4); s4 = inb(0x3C5);
    outb(0x3CE, 4); g4 = inb(0x3CF); outb(0x3CE, 5); g5 = inb(0x3CF); outb(0x3CE, 6); g6 = inb(0x3CF);
    outb(0x3C4, 2); outb(0x3C5, 4); outb(0x3C4, 4); outb(0x3C5, 7);
    outb(0x3CE, 4); outb(0x3CF, 2); outb(0x3CE, 5); outb(0x3CF, 0); outb(0x3CE, 6); outb(0x3CF, 0);
    const volatile u8 *m = (const volatile u8 *)0xA0000;
    for (int ch = 0; ch < 256; ch++) for (int r = 0; r < 16; r++) glyphs[ch * 16 + r] = m[ch * 32 + r];
    outb(0x3C4, 2); outb(0x3C5, s2); outb(0x3C4, 4); outb(0x3C5, s4);
    outb(0x3CE, 4); outb(0x3CF, g4); outb(0x3CE, 5); outb(0x3CF, g5); outb(0x3CE, 6); outb(0x3CF, g6);
}
static u32 pci_read(u32 bus, u32 dev, u32 fn, u32 off) { outl(0xCF8, 0x80000000u | bus << 16 | dev << 11 | fn << 8 | (off & 0xFC)); return inl(0xCFC); }
static void vbe(u16 i, u16 v) { outw(0x1CE, i); outw(0x1CF, v); }
static int screen_init(void) {   /* 1 with fb set, 0 when there is no Bochs display on bus 0 */
    for (u32 d = 0; d < 32; d++) {
        if (pci_read(0, d, 0, 0) != 0x11111234u) continue;
        u64 bar = pci_read(0, d, 0, 0x10) & ~0xFu;
        if (!bar) return 0;
        fb_w = 1024; fb_h = 768;
        vbe(4, 0); vbe(1, (u16)fb_w); vbe(2, (u16)fb_h); vbe(3, 32); vbe(4, 0x41);   /* disable, size, depth, enable with the linear framebuffer */
        fb = (volatile u32 *)bar;
        for (u32 i = 0; i < fb_w * fb_h; i++) fb[i] = BLUE;
        return 1;
    }
    return 0;
}

static u64 rdcr0(void) { u64 v; __asm__ volatile ("mov %%cr0,%0" : "=r"(v)); return v; }
static u64 rdcr4(void) { u64 v; __asm__ volatile ("mov %%cr4,%0" : "=r"(v)); return v; }
static u64 rdefer(void) { u32 lo, hi; __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000080u)); return (u64)hi << 32 | lo; }

void kmain(u32 magic, u32 info) {
    (void)info;
    serial_init();
    font_dump();
    int screen = screen_init();
    kputs("JT64 M0 Joshua Tree, x86-64\n");
    kputs(magic == 0x2BADB002u ? "JT64 multiboot ok\n" : "JT64 multiboot FAIL\n");
    u64 cr0 = rdcr0(), cr4 = rdcr4(), efer = rdefer();
    int lm = (cr0 >> 31 & 1) && (cr4 >> 5 & 1) && (efer >> 8 & 1) && (efer >> 10 & 1);   /* PG, PAE, LME and LMA: the CPU says it is in long mode */
    kputs(lm ? "JT64 long mode ok, EFER " : "JT64 long mode FAIL, EFER "); khex(efer); kputs("\n");
    volatile u64 a = 0x100000000ull, b = 3;   /* volatile: done at run time, in 64-bit registers */
    u64 prod = a * b + 7;
    kputs(prod == 0x300000007ull && sizeof(void *) == 8 ? "JT64 64-bit math ok " : "JT64 64-bit math FAIL "); khex(prod); kputs("\n");
    volatile u64 *hi = (volatile u64 *)0x80000000ull;   /* 2 GiB: mapped by the third page directory, RAM or not */
    volatile u64 *lo = (volatile u64 *)0x00400000ull;   /* 4 MiB: RAM under any -m, clear of the image */
    *lo = 0x4A5436344A543634ull;
    kputs(*lo == 0x4A5436344A543634ull ? "JT64 paging ok, 4 GiB mapped in " : "JT64 paging FAIL in "); kdec(2048); kputs(" pages of 2 MiB\n");
    (void)hi;
    if (screen) { kputs("JT64 screen ok "); kdec(fb_w); kputs("x"); kdec(fb_h); kputs(" at "); khex((u64)fb); kputs("\n"); }
    else kputs("JT64 screen none\n");
    kputs("JT64 M0 done\n");
    for (;;) __asm__ volatile ("hlt");
}
