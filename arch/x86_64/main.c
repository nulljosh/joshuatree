/* x86-64, slices 1 and 2: the 64-bit kernel boots and says so, then takes interrupts.

   Slice 1 (M0): boot.S has already put the CPU in long mode with the first 4 GiB identity mapped. kmain proves it from C:
   it reads back CR0, CR4 and EFER, does 64-bit arithmetic that a 32-bit build would get wrong, and writes above 2 GiB
   through the new page tables. Then the screen: the 8x16 font is copied out of VGA plane 2 where the BIOS left it, the
   Bochs display (QEMU's -vga std, PCI 1234:1111) is switched to 1024x768 at 32 bits, and the M0 lines are drawn there.

   Slice 2: a 64-bit IDT with 256 gates. Vectors 0 to 31 are the CPU exceptions: each prints its vector, error code and
   RIP on COM1 and halts. The legacy 8259 PIC moves to vectors 32 to 47, and only IRQ0 (the PIT, set to 100 Hz) and IRQ1
   (the PS/2 keyboard) are unmasked. IRQ1 prints each scancode read from port 0x60. The IRQ lines go to COM1 only, so the
   M0 screen stays as slice 1 drew it. tools/checks/x86_64-m0-check.py reads the M0 lines,
   tools/checks/x86_64-irq-check.py reads the rest.

   Slice 3: the multiboot memory map gives the usable RAM (printed in MB). A bitmap of 4 KiB frames covers the RAM above
   the image, and a small kmalloc and kfree sit on a 4 MiB arena of those frames; a self-test proves them. The PS/2 mouse
   on IRQ12 (through the 8042's aux port) moves a white square cursor on the framebuffer, clamped to the screen, and
   prints its position on COM1 at most every 10 ticks. tools/checks/x86_64-mem-check.py reads these lines. */
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

/* serial only: the interrupt lines, printed from the handlers too */
static void ser_puts(const char *s) { while (*s) serial_putc(*s++); }
static void ser_hex(u64 v, int digits) { ser_puts("0x"); for (int i = (digits - 1) * 4; i >= 0; i -= 4) serial_putc("0123456789abcdef"[(v >> i) & 15]); }
static void ser_dec(u64 v) { char t[21]; int k = 0; do t[k++] = (char)('0' + v % 10); while (v /= 10); while (k) serial_putc(t[--k]); }

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

/* ---- slice 3: memory ---- */

/* The multiboot info struct (flags at 0, mmap_length at 44, mmap_addr at 48). A map entry is a u32 size that does not count
   itself, a u64 base, a u64 length and a u32 type, where 1 is usable RAM. The bytes are read one at a time because the map
   need not be aligned. */
extern char _bss_end[];   /* end of the image (linker.ld): code, data, page tables, stack and the bitmap below it */
static u32 rd32(u64 a) { u32 v = 0; for (int i = 3; i >= 0; i--) v = v << 8 | *(const u8 *)(a + i); return v; }
static u64 rd64(u64 a) { return (u64)rd32(a + 4) << 32 | rd32(a); }

#define FRAME 4096ull
#define FRAMES (1u << 20)                 /* 4 GiB of frames: the most the identity map reaches */
static u8 frame_free[FRAMES / 8];         /* one bit per frame, 1 = free; zero at boot, so every frame starts in use */
static u64 frame_base;                    /* physical address of frame 0: the first 4 KiB above the image */
static u32 frame_count;
static u64 usable_bytes;

static void frame_mark(u32 i, int free) {
    if (free) frame_free[i >> 3] |= (u8)(1u << (i & 7));
    else      frame_free[i >> 3] &= (u8)~(1u << (i & 7));
}
static int frame_is_free(u32 i) { return frame_free[i >> 3] >> (i & 7) & 1; }

static void mem_init(u32 info) {
    if (!(rd32(info) & (1u << 6))) return;   /* flags bit 6: the memory map is there */
    frame_base = ((u64)_bss_end + FRAME - 1) & ~(FRAME - 1);
    frame_count = (u32)(((1ull << 32) - frame_base) / FRAME);
    u64 p = rd32(info + 48), end = p + rd32(info + 44);
    for (u64 e = p; e < end; ) {
        u32 size = rd32(e);
        u64 base = rd64(e + 4), span = rd64(e + 12);
        if (rd32(e + 20) == 1) {
            usable_bytes += span;
            u64 lo = base < frame_base ? frame_base : base;
            u64 hi = base + span > (1ull << 32) ? (1ull << 32) : base + span;
            lo = (lo + FRAME - 1) & ~(FRAME - 1);
            hi &= ~(FRAME - 1);
            for (u64 a = lo; a < hi; a += FRAME) frame_mark((u32)((a - frame_base) / FRAME), 1);
        }
        e += size + 4;
    }
}

/* the first run of n free frames, marked in use; the physical address, or 0 when there is no such run */
static u64 frame_alloc(u32 n) {
    for (u32 i = 0; i + n <= frame_count; i++) {
        u32 k = 0;
        while (k < n && frame_is_free(i + k)) k++;
        if (k == n) { for (u32 j = 0; j < n; j++) frame_mark(i + j, 0); return frame_base + (u64)i * FRAME; }
        i += k;   /* frame i + k is in use, so the next try starts after it */
    }
    return 0;
}
static void frame_release(u64 addr) { frame_mark((u32)((addr - frame_base) / FRAME), 1); }

/* kmalloc and kfree: bumped out of a 4 MiB arena of frames. A freed block goes on a list and is reused for the first request
   that fits. No splitting or merging, so it stays small. Each block has a 16-byte header that holds its size. */
#define HEAP_FRAMES 1024
typedef struct blk { u64 size; struct blk *next; } blk;
static u8 *heap_next, *heap_end;
static blk *heap_free;

static int heap_init(void) {
    u64 a = frame_alloc(HEAP_FRAMES);
    if (!a) return 0;
    heap_next = (u8 *)a;
    heap_end = heap_next + HEAP_FRAMES * FRAME;
    return 1;
}
void *kmalloc(u64 n) {
    n = (n + 15) & ~15ull;
    if (!n) n = 16;
    for (blk **pp = &heap_free; *pp; pp = &(*pp)->next)
        if ((*pp)->size >= n) { blk *b = *pp; *pp = b->next; return b + 1; }
    if ((u64)(heap_end - heap_next) < sizeof(blk) + n) return 0;
    blk *b = (blk *)heap_next;
    heap_next += sizeof(blk) + n;
    b->size = n;
    return b + 1;
}
void kfree(void *p) {
    if (!p) return;
    blk *b = (blk *)p - 1;
    b->next = heap_free;
    heap_free = b;
}

/* the self-test: one frame comes back from the bitmap; a block is written, freed, and the same block comes back */
static int mem_selftest(void) {
    u64 f = frame_alloc(1);
    if (!f) return 0;
    int ok = f >= frame_base && !(f & (FRAME - 1));
    frame_release(f);
    ok &= frame_alloc(1) == f;
    frame_release(f);
    volatile u8 *a = kmalloc(100), *b, *c;
    if (!a) return 0;
    for (int i = 0; i < 100; i++) a[i] = (u8)(i * 3 + 1);
    for (int i = 0; i < 100; i++) ok &= a[i] == (u8)(i * 3 + 1);
    kfree((void *)a);
    b = kmalloc(100);
    if (!b) return 0;
    ok &= b == a;
    for (int i = 0; i < 100; i++) b[i] = 0xA5;
    for (int i = 0; i < 100; i++) ok &= b[i] == 0xA5;
    c = kmalloc(5000);
    if (!c) return 0;
    c[4999] = 0x7E;
    ok &= c[4999] == 0x7E;
    kfree((void *)b); kfree((void *)c);
    return ok;
}

/* ---- slice 2: interrupts ---- */

/* The stubs. Exceptions: the CPU pushes an error code for 8, 10 to 14, 17, 21, 29 and 30; the stub pushes a 0 for the
   others, then the vector, so exc_common always finds vector, error and RIP at the same offsets. IRQ stubs save the
   caller-saved registers around the C dispatcher and return with iretq. */
__asm__(
".pushsection .text\n"
".macro EXC_NOERR n\n"
"exc_stub_\\n:\n"
"    pushq $0\n"
"    pushq $\\n\n"
"    jmp exc_common\n"
".pushsection .rodata\n"
"    .quad exc_stub_\\n\n"
".popsection\n"
".endm\n"
".macro EXC_ERR n\n"
"exc_stub_\\n:\n"
"    pushq $\\n\n"
"    jmp exc_common\n"
".pushsection .rodata\n"
"    .quad exc_stub_\\n\n"
".popsection\n"
".endm\n"
".macro IRQ_STUB n\n"
"irq_stub_\\n:\n"
"    pushq %rdi\n"
"    movl $(32 + \\n), %edi\n"
"    jmp irq_common\n"
".pushsection .rodata\n"
"    .quad irq_stub_\\n\n"
".popsection\n"
".endm\n"
".pushsection .rodata\n"
".globl exc_table\n"
"exc_table:\n"
".popsection\n"
"EXC_NOERR 0\nEXC_NOERR 1\nEXC_NOERR 2\nEXC_NOERR 3\nEXC_NOERR 4\nEXC_NOERR 5\nEXC_NOERR 6\nEXC_NOERR 7\n"
"EXC_ERR 8\nEXC_NOERR 9\nEXC_ERR 10\nEXC_ERR 11\nEXC_ERR 12\nEXC_ERR 13\nEXC_ERR 14\nEXC_NOERR 15\n"
"EXC_NOERR 16\nEXC_ERR 17\nEXC_NOERR 18\nEXC_NOERR 19\nEXC_NOERR 20\nEXC_ERR 21\nEXC_NOERR 22\nEXC_NOERR 23\n"
"EXC_NOERR 24\nEXC_NOERR 25\nEXC_NOERR 26\nEXC_NOERR 27\nEXC_NOERR 28\nEXC_ERR 29\nEXC_ERR 30\nEXC_NOERR 31\n"
".pushsection .rodata\n"
".globl irq_table\n"
"irq_table:\n"
".popsection\n"
"IRQ_STUB 0\nIRQ_STUB 1\nIRQ_STUB 2\nIRQ_STUB 3\nIRQ_STUB 4\nIRQ_STUB 5\nIRQ_STUB 6\nIRQ_STUB 7\n"
"IRQ_STUB 8\nIRQ_STUB 9\nIRQ_STUB 10\nIRQ_STUB 11\nIRQ_STUB 12\nIRQ_STUB 13\nIRQ_STUB 14\nIRQ_STUB 15\n"
"exc_common:\n"
"    movq 0(%rsp), %rdi            /* vector */\n"
"    movq 8(%rsp), %rsi            /* error code, or 0 */\n"
"    movq 16(%rsp), %rdx           /* RIP the CPU saved */\n"
"    andq $-16, %rsp\n"
"    call exc_halt\n"
"1:  hlt\n"
"    jmp 1b\n"
"irq_common:\n"
"    pushq %rax\n    pushq %rcx\n    pushq %rdx\n    pushq %rsi\n"
"    pushq %r8\n    pushq %r9\n    pushq %r10\n    pushq %r11\n"
"    pushq %rbp\n"
"    movq %rsp, %rbp\n"
"    andq $-16, %rsp\n"
"    call irq_dispatch             /* vector in edi */\n"
"    movq %rbp, %rsp\n"
"    popq %rbp\n"
"    popq %r11\n    popq %r10\n    popq %r9\n    popq %r8\n"
"    popq %rsi\n    popq %rdx\n    popq %rcx\n    popq %rax\n"
"    popq %rdi\n"
"    iretq\n"
".popsection\n");

typedef struct { u16 off_lo, sel; u8 ist, type; u16 off_mid; u32 off_hi, zero; } gate;   /* a 16-byte 64-bit gate */
typedef struct __attribute__((packed)) { u16 limit; u64 base; } desc_ptr;
static gate idt[256];
extern const u64 exc_table[32], irq_table[16];
static volatile u64 ticks;

static void idt_set(u32 v, u64 handler) {
    idt[v].off_lo = (u16)handler; idt[v].sel = 0x08; idt[v].ist = 0; idt[v].type = 0x8E;   /* present, ring 0, interrupt gate */
    idt[v].off_mid = (u16)(handler >> 16); idt[v].off_hi = (u32)(handler >> 32); idt[v].zero = 0;
}

/* called from exc_common: say which exception, where, and stop */
void exc_halt(u64 vec, u64 err, u64 rip) {
    __asm__ volatile ("cli");
    ser_puts("JT64 exception "); ser_hex(vec, 2); ser_puts(" at RIP "); ser_hex(rip, 16);
    ser_puts(", error "); ser_hex(err, 8); ser_puts(", halted\n");
    for (;;) __asm__ volatile ("hlt");
}

static u8 pic_isr(void) { outb(0x20, 0x0B); return inb(0x20); }   /* OCW3: read the in-service register */
static u8 pic_isr_slave(void) { outb(0xA0, 0x0B); return inb(0xA0); }

static void mouse_tick(void);   /* the mouse section, below */
static void mouse_byte(u8 b);

/* called from irq_common with the vector in edi */
void irq_dispatch(u32 vec) {
    if (vec == 39 && !(pic_isr() & 0x80)) return;   /* a spurious IRQ7 gets no EOI */
    if (vec == 47 && !(pic_isr_slave() & 0x80)) { outb(0x20, 0x20); return; }   /* spurious IRQ15: the master only */
    if (vec == 32) { ticks++; mouse_tick(); }
    else if (vec == 33) { u8 sc = inb(0x60); ser_puts("JT64 key "); ser_hex(sc, 2); ser_puts("\n"); }
    else if (vec == 44) mouse_byte(inb(0x60));
    if (vec >= 40) outb(0xA0, 0x20);   /* IRQ8 to 15 also need an end of interrupt on the slave */
    outb(0x20, 0x20);   /* end of interrupt to the master PIC */
}

static void io_wait(void) { outb(0x80, 0); }
static void pic_init(void) {
    outb(0x20, 0x11); io_wait(); outb(0xA0, 0x11); io_wait();   /* ICW1: start the init sequence, ICW4 follows */
    outb(0x21, 0x20); io_wait(); outb(0xA1, 0x28); io_wait();   /* ICW2: IRQ0 to 7 at vector 32, IRQ8 to 15 at 40 */
    outb(0x21, 0x04); io_wait(); outb(0xA1, 0x02); io_wait();   /* ICW3: the slave hangs off IRQ2 */
    outb(0x21, 0x01); io_wait(); outb(0xA1, 0x01); io_wait();   /* ICW4: 8086 mode */
    outb(0x21, 0xF8); outb(0xA1, 0xFF);                         /* masks: IRQ0, IRQ1 and the cascade IRQ2 open; the slave's IRQ12 opens later */
}
static void pit_100hz(void) {
    outb(0x43, 0x36);                                  /* channel 0, low byte then high byte, square wave */
    outb(0x40, 11931 & 0xFF); outb(0x40, 11931 >> 8);  /* 1193182 Hz / 11931, about 100.01 Hz */
}

/* ---- slice 3: the PS/2 mouse on IRQ12 ---- */

#define CURSOR 8
static int mouse_x, mouse_y, mouse_dirty, mouse_n;
static u64 mouse_last;            /* tick of the last "JT64 mouse" line; starts far back so the first move prints at once */
static u8 mouse_pkt[3];
static u32 under[CURSOR * CURSOR];   /* the pixels the cursor covers, put back when it moves */

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int i8042_wait_in(void) { for (int i = 0; i < 100000; i++) if (!(inb(0x64) & 0x02)) return 1; return 0; }
static int i8042_wait_out(void) { for (int i = 0; i < 100000; i++) if (inb(0x64) & 0x01) return 1; return 0; }

/* the next byte from the mouse port (0xFA is the ACK); keyboard bytes that arrive first are dropped (status bit 5 marks mouse data) */
static int aux_ack(void) {
    for (int i = 0; i < 100000; i++) {
        u8 st = inb(0x64);
        if ((st & 0x21) == 0x21) return inb(0x60);
        if (st & 0x01) inb(0x60);
    }
    return -1;
}
static int aux_send(u8 b) {   /* 0xD4 routes the next byte to the mouse */
    if (!i8042_wait_in()) return -1;
    outb(0x64, 0xD4);
    if (!i8042_wait_in()) return -1;
    outb(0x60, b);
    return aux_ack();
}
static int mouse_init(void) {
    if (!i8042_wait_in()) return 0;
    outb(0x64, 0xA8);                       /* enable the aux port */
    if (!i8042_wait_in()) return 0;
    outb(0x64, 0x20);                       /* read the controller config byte */
    if (!i8042_wait_out()) return 0;
    u8 cfg = inb(0x60) | 0x02;              /* IRQ12 on */
    cfg &= (u8)~0x20;                       /* mouse clock on */
    if (!i8042_wait_in()) return 0;
    outb(0x64, 0x60);                       /* write the config byte back */
    if (!i8042_wait_in()) return 0;
    outb(0x60, cfg);
    if (aux_send(0xF6) != 0xFA) return 0;   /* set defaults */
    if (aux_send(0xF4) != 0xFA) return 0;   /* enable streaming */
    return 1;
}

/* the cursor: draw saves the pixels under the square and paints it; not draw puts the saved pixels back */
static void cursor_put(int x, int y, int draw) {
    for (int r = 0; r < CURSOR; r++) for (int c = 0; c < CURSOR; c++) {
        int px = x + c, py = y + r;
        if (px < 0 || py < 0 || px >= (int)fb_w || py >= (int)fb_h) continue;
        volatile u32 *p = &fb[(u32)py * fb_w + (u32)px];
        u32 *u = &under[r * CURSOR + c];
        if (draw) { *u = *p; *p = INK; } else *p = *u;
    }
}

/* a packet is three bytes; the first has bit 3 set, so a byte without it at the start means we are out of step */
static void mouse_byte(u8 b) {
    if (mouse_n == 0 && !(b & 0x08)) return;
    mouse_pkt[mouse_n++] = b;
    if (mouse_n < 3) return;
    mouse_n = 0;
    int dx = mouse_pkt[1] - ((mouse_pkt[0] & 0x10) ? 256 : 0);   /* bits 4 and 5: the sign of x and y */
    int dy = mouse_pkt[2] - ((mouse_pkt[0] & 0x20) ? 256 : 0);
    int nx = clampi(mouse_x + dx, 0, (int)fb_w - 1);
    int ny = clampi(mouse_y - dy, 0, (int)fb_h - 1);   /* the packet's y grows upward, the screen's downward */
    if (nx == mouse_x && ny == mouse_y) return;
    cursor_put(mouse_x, mouse_y, 0);
    mouse_x = nx; mouse_y = ny;
    cursor_put(mouse_x, mouse_y, 1);
    mouse_dirty = 1;
}

/* from the timer: one line at most every 10 ticks, and only when the pointer moved */
static void mouse_tick(void) {
    if (!mouse_dirty || ticks - mouse_last < 10) return;
    mouse_dirty = 0;
    mouse_last = ticks;
    ser_puts("JT64 mouse "); ser_dec((u64)mouse_x); ser_puts(" "); ser_dec((u64)mouse_y); ser_puts("\n");
}

/* with interrupts still off: turn the mouse on, put the cursor in the middle, then open IRQ12 on the slave */
static void mouse_start(int screen) {
    int ok = screen && mouse_init();
    pic_init();   /* the handshake's ACK bytes latched IRQ1 and IRQ12 while interrupts were off; ICW1 clears the latches */
    if (!ok) { ser_puts("JT64 mouse none\n"); return; }
    mouse_x = (int)(fb_w / 2); mouse_y = (int)(fb_h / 2);
    mouse_last = (u64)-10;
    cursor_put(mouse_x, mouse_y, 1);
    outb(0xA1, inb(0xA1) & ~0x10);   /* unmask IRQ12 (the cascade IRQ2 is open from pic_init) */
    ser_puts("JT64 mouse ready\n");
}
static void idt_load(void) {
    for (u32 v = 0; v < 32; v++) idt_set(v, exc_table[v]);
    for (u32 v = 0; v < 16; v++) idt_set(32 + v, irq_table[v]);
    desc_ptr idtr = { (u16)(sizeof(idt) - 1), (u64)idt };
    __asm__ volatile ("lidt %0" :: "m"(idtr));
}

void kmain(u32 magic, u32 info) {
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

    mem_init(info);
    ser_puts("JT64 memory "); ser_dec(usable_bytes >> 20); ser_puts(" MB\n");
    ser_puts(heap_init() && mem_selftest() ? "JT64 heap ok\n" : "JT64 heap FAIL\n");

    idt_load();
    ser_puts("JT64 idt ok\n");
    pic_init();
    pit_100hz();
    mouse_start(screen);
    __asm__ volatile ("sti");
    while (ticks < 20) __asm__ volatile ("hlt");
    u64 t = ticks;
    ser_puts("JT64 timer 100 Hz, "); ser_dec(t); ser_puts(" ticks\n");
    for (;;) __asm__ volatile ("hlt");
}
