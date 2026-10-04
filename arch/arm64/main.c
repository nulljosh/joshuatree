/* M0: serial hello. QEMU's virt machine has a PL011 UART at 0x09000000. A Raspberry Pi 4 has one at
   0xFE201000 (make PI=1 sets UART_BASE and PI_BUILD) and needs its two GPIO pins and its baud rate
   set first; QEMU's virt machine comes up ready. */
#ifdef PI_BUILD
#define UART_BASE 0xFE201000UL
#define GPIO_BASE 0xFE200000UL
#define GICD_BASE 0xFF841000UL   /* the Pi 4's GIC-400, the same GICv2 programming model as QEMU's virt */
#define GICC_BASE 0xFF842000UL
#else
#define UART_BASE 0x09000000UL
#define GICD_BASE 0x08000000UL
#define GICC_BASE 0x08010000UL
#endif
#define REG(a) (*(volatile unsigned int *)(a))
#define UART_DR   (UART_BASE + 0x00)
#define UART_FR   (UART_BASE + 0x18)
#define UART_IBRD (UART_BASE + 0x24)
#define UART_FBRD (UART_BASE + 0x28)
#define UART_LCRH (UART_BASE + 0x2C)
#define UART_CR   (UART_BASE + 0x30)
#define UART_IMSC (UART_BASE + 0x38)
#define UART_ICR  (UART_BASE + 0x44)
#define TXFF (1u << 5)
#define BUSY (1u << 3)

extern unsigned int boot_el;

static void uart_init(void) {
#ifdef PI_BUILD
    /* GPIO14 (TX) and GPIO15 (RX) to ALT0 (PL011), pulls off. BCM2711 registers: GPFSEL1 and GPIO_PUP_PDN_CNTRL_REG0. */
    unsigned int f = REG(GPIO_BASE + 0x04);
    f &= ~((7u << 12) | (7u << 15)); f |= (4u << 12) | (4u << 15);
    REG(GPIO_BASE + 0x04) = f;
    REG(GPIO_BASE + 0xE4) &= ~((3u << 28) | (3u << 30));
    while (REG(UART_FR) & BUSY) {}
    REG(UART_CR) = 0;
    REG(UART_ICR) = 0x7FF;
    REG(UART_IMSC) = 0;
    REG(UART_IBRD) = 26;   /* 115200 baud from the firmware's 48 MHz UART clock: 48e6 / (16 * 115200) = 26.04 */
    REG(UART_FBRD) = 3;
    REG(UART_LCRH) = 0x70; /* 8 bits, no parity, FIFOs on */
    REG(UART_CR) = 0x301;  /* UART, TX, RX on */
#endif
}
static void uart_putc(char c) {
    while (REG(UART_FR) & TXFF) {}
    REG(UART_DR) = (unsigned char)c;
}
static void uart_puts(const char *s) { while (*s) uart_putc(*s++); }
static void uart_hex(unsigned long v) {
    uart_puts("0x");
    for (int i = 60; i >= 0; i -= 4) uart_putc("0123456789abcdef"[(v >> i) & 15]);
}
static void uart_dec(unsigned v) {
    char b[12]; int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) uart_putc(b[--n]);
}

/* ---- M1: exceptions, the interrupt controller, the timer ---- */
struct frame { unsigned long x[31]; unsigned long elr, esr; };   /* what vectors.S saved */
extern char vectors[];
static volatile unsigned ticks;
static unsigned long timer_step;
#define GICD(o) REG(GICD_BASE + (o))
#define GICC(o) REG(GICC_BASE + (o))
#define TIMER_INTID 30   /* the EL1 physical timer, a private interrupt on every GIC */

void exc_sync(struct frame *f) {
    unsigned ec = (unsigned)(f->esr >> 26);
    if (ec == 0x15) { uart_puts("M1 svc ok\n"); return; }   /* a deliberate svc #0: elr is already the next instruction */
    uart_puts("sync exception, ESR "); uart_hex(f->esr); uart_puts(" ELR "); uart_hex(f->elr); uart_puts("\n");
    for (;;) __asm__ volatile ("wfe");
}
void exc_irq(struct frame *f) {
    (void)f;
    unsigned iar = GICC(0x0C), id = iar & 0x3FF;
    if (id == TIMER_INTID) {
        __asm__ volatile ("msr cntp_tval_el0, %0" :: "r"(timer_step));   /* next tick */
        ticks++;
        uart_puts("tick "); uart_dec(ticks); uart_puts("\n");
        if (ticks >= 3) __asm__ volatile ("msr cntp_ctl_el0, %0" :: "r"(0UL));   /* enough, stop the timer */
    }
    GICC(0x10) = iar;   /* end of interrupt */
}
void exc_bad(struct frame *f) {
    uart_puts("unexpected exception, ESR "); uart_hex(f->esr); uart_puts(" ELR "); uart_hex(f->elr); uart_puts("\n");
}
/* ---- M1b: the MMU, the caches, a heap ---- */
extern char _heap_start[];
static unsigned long l1[512] __attribute__((aligned(4096)));   /* one table of 1 GiB blocks, enough for a flat map */
static unsigned long heap_next;
#define HEAP_SIZE (16UL << 20)
static void *kmalloc(unsigned long n) {
    unsigned long p = (heap_next + 15) & ~15UL;
    if (p + n > (unsigned long)_heap_start + HEAP_SIZE) return 0;
    heap_next = p + n;
    return (void *)p;
}
/* Identity map the first 4 GiB in four 1 GiB blocks: RAM is normal write-back memory, the peripherals are device
   memory (strongly ordered, no caching, no unaligned access). QEMU's virt keeps its devices in the first GiB and RAM in
   the second; a Pi 4 has RAM from 0 and its peripherals in the last GiB. */
static void mmu_init(void) {
    const unsigned long NORMAL = 0x1UL | (1UL << 2) /* attr 1 */ | (3UL << 8) /* inner shareable */ | (1UL << 10) /* access flag */;
    const unsigned long DEVICE = 0x1UL | (0UL << 2) /* attr 0 */ | (1UL << 10) | (1UL << 53) | (1UL << 54);   /* never executable */
    for (int i = 0; i < 512; i++) l1[i] = 0;
    for (int i = 0; i < 4; i++) {
        unsigned long base = (unsigned long)i << 30;
#ifdef PI_BUILD
        int dev = i == 3;
#else
        int dev = i == 0;
#endif
        l1[i] = base | (dev ? DEVICE : NORMAL);
    }
    unsigned long mair = (0xFFUL << 8) | 0x00UL;                       /* attr 1 normal write-back, attr 0 device-nGnRnE */
    unsigned long tcr = 25UL | (1UL << 8) | (1UL << 10) | (3UL << 12) | (1UL << 23) | (2UL << 32);   /* 39-bit VA, 4 KiB pages, write-back, TTBR1 off, 40-bit PA */
    unsigned long sctlr;
    __asm__ volatile ("msr mair_el1, %0\n msr tcr_el1, %1\n msr ttbr0_el1, %2\n dsb sy\n isb\n tlbi vmalle1\n dsb sy\n isb" :: "r"(mair), "r"(tcr), "r"((unsigned long)l1) : "memory");
    __asm__ volatile ("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1UL << 0) | (1UL << 2) | (1UL << 12);                   /* MMU, data cache, instruction cache */
    __asm__ volatile ("msr sctlr_el1, %0\n isb" :: "r"(sctlr) : "memory");
}
static void m1b_selftest(void) {
    heap_next = (unsigned long)_heap_start;
    mmu_init();
    uart_puts("M1b mmu on\n");
    unsigned long *a = kmalloc(4096), *b = kmalloc(100), *c = kmalloc(8);
    if (!a || !b || !c || (unsigned long)a % 16 || (unsigned long)b % 16 || b == a || c <= b) { uart_puts("M1b heap FAIL\n"); return; }
    for (int i = 0; i < 512; i++) a[i] = 0xA5A5A5A500000000UL | (unsigned long)i;
    for (int i = 0; i < 512; i++) if (a[i] != (0xA5A5A5A500000000UL | (unsigned long)i)) { uart_puts("M1b heap FAIL\n"); return; }
    uart_puts("M1b heap ok\n");
}

/* ---- M1c: a framebuffer. QEMU's virt machine has no display unless one is asked for; ramfb is a plain RAM
   framebuffer the guest configures through fw_cfg (a DMA write of the "etc/ramfb" file). A Pi asks its GPU firmware
   for one through the mailbox. Either way fb_setup hands back plain 32-bit pixels and the drawing is shared. ---- */
#define FB_W 800
#define FB_H 600
static unsigned int *fb;
static unsigned fb_pitch;   /* in pixels */
static int fb_swap;         /* red and blue the other way round in memory */
static void dcache_clean(void *p, unsigned long n) {   /* push lines out to RAM, where a GPU or DMA engine reads */
    for (unsigned long a = (unsigned long)p & ~63UL; a < (unsigned long)p + n; a += 64) __asm__ volatile ("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile ("dsb sy" ::: "memory");
}
#ifndef PI_BUILD
#define FW_CFG 0x09020000UL
static unsigned short fw_sel(unsigned short s) { *(volatile unsigned short *)(FW_CFG + 8) = __builtin_bswap16(s); return s; }
static unsigned char fw_byte(void) { return *(volatile unsigned char *)FW_CFG; }
static unsigned fw_be32(void) { unsigned v = 0; for (int i = 0; i < 4; i++) v = v << 8 | fw_byte(); return v; }
static int fw_find(const char *want) {   /* the file directory: count, then 64-byte entries (size, select, pad, name[56]) */
    fw_sel(0x19);
    unsigned n = fw_be32();
    for (unsigned i = 0; i < n; i++) {
        fw_be32();
        unsigned sel = fw_byte() << 8; sel |= fw_byte(); fw_byte(); fw_byte();
        char name[56]; for (int j = 0; j < 56; j++) name[j] = (char)fw_byte();
        int k = 0; while (want[k] && name[k] == want[k]) k++;
        if (!want[k] && !name[k]) return (int)sel;
    }
    return -1;
}
struct fw_dma { unsigned control, length; unsigned long address; } __attribute__((packed, aligned(16)));
struct ramfb_cfg { unsigned long addr; unsigned fourcc, flags, width, height, stride; } __attribute__((packed));
static int fb_setup(void) {
    int sel = fw_find("etc/ramfb");
    if (sel < 0) { uart_puts("M1c no ramfb\n"); return 0; }
    fb = kmalloc((unsigned long)FB_W * FB_H * 4);
    fb_pitch = FB_W;
    static struct ramfb_cfg cfg __attribute__((aligned(16)));
    static struct fw_dma dma;
    cfg.addr = __builtin_bswap64((unsigned long)fb);
    cfg.fourcc = __builtin_bswap32(0x34325258);   /* 'XR24': 32-bit 0x00RRGGBB */
    cfg.flags = 0;
    cfg.width = __builtin_bswap32(FB_W); cfg.height = __builtin_bswap32(FB_H); cfg.stride = __builtin_bswap32(FB_W * 4);
    dma.control = __builtin_bswap32(((unsigned)sel << 16) | 8 | 16);   /* select, write */
    dma.length = __builtin_bswap32(sizeof cfg);
    dma.address = __builtin_bswap64((unsigned long)&cfg);
    dcache_clean(&cfg, sizeof cfg); dcache_clean(&dma, sizeof dma);   /* the DMA engine reads these straight from RAM */
    *(volatile unsigned long *)(FW_CFG + 16) = __builtin_bswap64((unsigned long)&dma);
    while (__builtin_bswap32(*(volatile unsigned *)&dma.control) & ~1u) {}
    if (__builtin_bswap32(*(volatile unsigned *)&dma.control) & 1) { uart_puts("M1c ramfb DMA error\n"); return 0; }
    return 1;
}
#else
/* The Pi 4's VideoCore mailbox, property channel 8. One message sets the size and depth and allocates the buffer;
   the GPU writes its answers into the same message. It lives in cacheable RAM, so it is cleaned out before the GPU
   reads it and invalidated before we read the answers. */
#define MBOX_BASE 0xFE00B880UL
#define MBOX_READ   REG(MBOX_BASE + 0x00)
#define MBOX_STATUS REG(MBOX_BASE + 0x18)
#define MBOX_WRITE  REG(MBOX_BASE + 0x20)
static volatile unsigned mbox[36] __attribute__((aligned(64)));
static int mbox_call(void) {
    unsigned long a = (unsigned long)mbox;
    dcache_clean((void *)mbox, sizeof mbox);
    while (MBOX_STATUS & 0x80000000u) {}             /* full */
    MBOX_WRITE = (unsigned)(a & ~15UL) | 8;
    for (;;) {
        while (MBOX_STATUS & 0x40000000u) {}         /* empty */
        unsigned r = MBOX_READ;
        if (r == ((unsigned)(a & ~15UL) | 8)) break;
    }
    dcache_clean((void *)mbox, sizeof mbox);         /* civac also invalidates: the next reads come from RAM */
    return mbox[1] == 0x80000000u;
}
static int fb_setup(void) {
    unsigned m[] = { sizeof mbox, 0,
        0x48003, 8, 0, FB_W, FB_H,     /* physical size */
        0x48004, 8, 0, FB_W, FB_H,     /* virtual size */
        0x48005, 4, 0, 32,             /* depth */
        0x48006, 4, 0, 0,              /* pixel order BGR: blue in the low byte, so 0x00RRGGBB as a 32-bit word */
        0x40001, 8, 0, 4096, 0,        /* allocate, 4 KiB aligned: answers address and size */
        0x40008, 4, 0, 0,              /* pitch in bytes */
        0 };
    for (unsigned i = 0; i < sizeof m / 4; i++) mbox[i] = m[i];
    if (!mbox_call() || !mbox[23]) { uart_puts("M1c mailbox framebuffer refused\n"); return 0; }
    fb = (unsigned int *)(unsigned long)(mbox[23] & 0x3FFFFFFF);   /* a VideoCore bus address: drop the alias bits */
    fb_pitch = mbox[28] / 4;
    fb_swap = mbox[19] == 1;   /* the firmware answers the order it really used; follow it if it overrode us */
    return 1;
}
#endif
static void fb_rect(int x, int y, int w, int h, unsigned c) {
    if (fb_swap) c = (c & 0xFF00FF00u) | (c >> 16 & 0xFF) | (c & 0xFF) << 16;
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) fb[j * fb_pitch + i] = c;
}
static void fb_init(void) {
    if (!fb_setup()) return;
    fb_rect(0, 0, FB_W, FB_H, 0x00203040);       /* desktop */
    fb_rect(0, 0, FB_W, 24, 0x00e0e0e0);         /* menu bar */
    fb_rect(150, 100, 500, 350, 0x00ffffff);     /* a window */
    fb_rect(150, 100, 500, 28, 0x00b5502c);      /* its title bar, the house accent */
    fb_rect(300, 540, 200, 44, 0x00505a68);      /* the dock */
    /* ponytail: the framebuffer sits in cacheable RAM, so a real GPU only sees pixels once they are cleaned out.
       One clean after drawing is enough for a still picture; a live desktop wants the buffer mapped write-combining. */
    dcache_clean(fb, (unsigned long)fb_pitch * FB_H * 4);
    int ok = fb[200 * fb_pitch + 200] == 0x00ffffff && fb[10 * fb_pitch + 10] == 0x00e0e0e0;   /* grey and white read the same either way */
    uart_puts(ok ? "M1c fb ok\n" : "M1c fb FAIL\n");
}

static void m1_selftest(void) {
    __asm__ volatile ("msr vbar_el1, %0\n isb" :: "r"(vectors));
    uart_puts("M1 vectors set\n");
    m1b_selftest();   /* with the exception table in place a bad map prints instead of hanging */
    __asm__ volatile ("svc #0");                   /* proves the sync path and the return */
    unsigned long freq; __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
    timer_step = freq / 20;                        /* 50 ms */
    GICD(0x000) = 1;                               /* distributor on */
    GICD(0x100) = 1u << TIMER_INTID;               /* enable the timer interrupt */
    *(volatile unsigned char *)(GICD_BASE + 0x400 + TIMER_INTID) = 0x80;   /* priority: a byte register, a 32-bit write there is an alignment fault */
    GICC(0x004) = 0xFF;                            /* let every priority through */
    GICC(0x000) = 1;                               /* CPU interface on */
    __asm__ volatile ("msr cntp_tval_el0, %0" :: "r"(timer_step));
    __asm__ volatile ("msr cntp_ctl_el0, %0" :: "r"(1UL));
    __asm__ volatile ("msr daifclr, #2");          /* unmask interrupts */
    while (ticks < 3) __asm__ volatile ("wfi");
    uart_puts("M1a ok\n");
    fb_init();
}

/* ---- M2: keyboard and mouse. QEMU's virt machine has 32 virtio-mmio slots from 0x0A000000, 0x200 apart; a
   virtio-keyboard or virtio-tablet shows up in one as device 18 (input). Modern virtio (version 2): one event queue the
   guest fills with 8-byte buffers, the device hands them back with Linux evdev events in them. Both devices speak the
   same events, so one driver serves both. The Pi gets USB through xHCI instead (M4). ---- */
#ifndef PI_BUILD
#define VIRTIO_BASE 0x0A000000UL
#define VQ 16
#define MAX_INPUT 2
struct vq_desc { unsigned long addr; unsigned len; unsigned short flags, next; };
struct vq_avail { unsigned short flags, idx, ring[VQ], used_event; };
struct vq_used { unsigned short flags, idx; struct { unsigned id, len; } ring[VQ]; unsigned short avail_event; };
struct input_event { unsigned short type, code; unsigned value; };
static struct vin {
    unsigned long base;
    struct vq_desc *d; struct vq_avail *a; volatile struct vq_used *u; struct input_event *ev;
    unsigned short seen;
} vin[MAX_INPUT];
static int nvin;
static unsigned mouse_x, mouse_y, mouse_moved;
static int vin_add(unsigned long b) {
    struct vin *v = &vin[nvin];
#define VR(o) REG(b + (o))
    VR(0x70) = 0;                                   /* reset */
    VR(0x70) = 1 | 2;                               /* acknowledge, driver */
    VR(0x24) = 1; VR(0x20) = 1;                     /* features 32..63: VIRTIO_F_VERSION_1 only */
    VR(0x24) = 0; VR(0x20) = 0;
    VR(0x70) = 1 | 2 | 8;                           /* features ok */
    if (!(VR(0x70) & 8)) return 0;
    VR(0x30) = 0;                                   /* queue 0, the event queue */
    if (VR(0x34) < VQ) return 0;
    VR(0x38) = VQ;
    v->d = kmalloc(sizeof *v->d * VQ); v->a = kmalloc(sizeof *v->a); v->u = kmalloc(sizeof *v->u); v->ev = kmalloc(sizeof *v->ev * VQ);
    if (!v->d || !v->a || !v->u || !v->ev) return 0;
    for (int i = 0; i < VQ; i++) {
        v->d[i] = (struct vq_desc){ (unsigned long)&v->ev[i], sizeof *v->ev, 2 /* device writes */, 0 };
        v->a->ring[i] = (unsigned short)i;
    }
    v->a->flags = 0; v->a->idx = VQ; v->u->idx = 0; v->seen = 0;
    VR(0x80) = (unsigned)(unsigned long)v->d;  VR(0x84) = (unsigned)((unsigned long)v->d >> 32);
    VR(0x90) = (unsigned)(unsigned long)v->a;  VR(0x94) = (unsigned)((unsigned long)v->a >> 32);
    VR(0xA0) = (unsigned)(unsigned long)v->u;  VR(0xA4) = (unsigned)((unsigned long)v->u >> 32);
    VR(0x44) = 1;                                   /* queue ready */
    VR(0x70) = 1 | 2 | 8 | 4;                       /* driver ok */
    __asm__ volatile ("dsb sy" ::: "memory");
    VR(0x50) = 0;                                   /* buffers are there */
#undef VR
    v->base = b;
    nvin++;
    return 1;
}
static int input_init(void) {
    for (int i = 0; i < 32 && nvin < MAX_INPUT; i++) {
        unsigned long b = VIRTIO_BASE + 0x200UL * i;
        if (REG(b) == 0x74726976 && REG(b + 4) == 2 && REG(b + 8) == 18) vin_add(b);   /* "virt", version 2, input */
    }
    return nvin;
}
static void input_event(struct input_event e) {
    if (e.type == 1) { uart_puts("key "); uart_dec(e.code); uart_puts(e.value ? " down\n" : " up\n"); }   /* EV_KEY: keys and buttons */
    else if (e.type == 3) {                                                                                /* EV_ABS: the tablet, 0..32767 */
        if (e.code == 0) mouse_x = e.value * FB_W / 32768; else if (e.code == 1) mouse_y = e.value * FB_H / 32768;
        mouse_moved = 1;
    } else if (e.type == 0 && mouse_moved) {                                                               /* EV_SYN: one report done */
        uart_puts("mouse "); uart_dec(mouse_x); uart_putc(','); uart_dec(mouse_y); uart_putc('\n');
        mouse_moved = 0;
    }
}
/* ponytail: polled, not interrupt driven. The GIC is already up (M1a), so wiring SPI 16+slot is the upgrade once the
   desktop has an event loop to deliver input to. */
static void input_poll(void) {
    for (int n = 0; n < nvin; n++) {
        struct vin *v = &vin[n];
        while (v->seen != v->u->idx) {
            __asm__ volatile ("dsb sy" ::: "memory");
            unsigned id = v->u->ring[v->seen % VQ].id;
            input_event(v->ev[id]);
            v->a->ring[v->a->idx % VQ] = (unsigned short)id;   /* hand the buffer back */
            __asm__ volatile ("dsb sy" ::: "memory");
            v->a->idx++;
            v->seen++;
            REG(v->base + 0x50) = 0;
        }
    }
}
#else
static int input_init(void) { return 0; }
static void input_poll(void) {}
#endif

void main(void) {
    unsigned long el;
    uart_init();
    __asm__ volatile ("mrs %0, CurrentEL" : "=r"(el));
    uart_puts("Joshua Tree on ARM64\n");
    uart_puts("booted at EL"); uart_putc((char)('0' + boot_el)); uart_puts("\n");
    uart_puts(((el >> 2) & 3) == 1 ? "EL1\n" : "not EL1\n");
    uart_puts("M0 ok\n");
    m1_selftest();
    int inputs = input_init();
    if (inputs) {
        uart_puts("M2 input ready, devices "); uart_dec((unsigned)inputs); uart_putc('\n');
        for (;;) input_poll();
    }
    for (;;) __asm__ volatile ("wfe");
}
