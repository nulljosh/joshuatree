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
static void console_putc(char c);   /* the same text, on the screen once there is one */
static void uart_putc(char c) {
    while (REG(UART_FR) & TXFF) {}
    REG(UART_DR) = (unsigned char)c;
    console_putc(c);
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
/* M4: the same printing for the drivers in their own files (pci.c, xhci.c) */
void kputs(const char *s) { uart_puts(s); }
void kdec(unsigned v) { uart_dec(v); }
void kx(unsigned v) {   /* short hex, no 0x and no leading zeros: status lines stay under 60 columns for the screen */
    int i = 28; while (i > 0 && !(v >> i)) i -= 4;
    for (; i >= 0; i -= 4) uart_putc("0123456789abcdef"[(v >> i) & 15]);
}

/* ---- M1: exceptions, the interrupt controller, the timer ---- */
struct frame { unsigned long x[31]; unsigned long elr, esr; };   /* what vectors.S saved */
extern char vectors[];
static volatile unsigned ticks;
static unsigned long timer_step;
#define GICD(o) REG(GICD_BASE + (o))
#define GICC(o) REG(GICC_BASE + (o))
#define TIMER_INTID 30   /* the EL1 physical timer, a private interrupt on every GIC */

volatile int kprobe_armed, kprobe_faulted;   /* M4: a driver probing for hardware that may not be there (pci.c) */
void exc_sync(struct frame *f) {
    unsigned ec = (unsigned)(f->esr >> 26);
    if (ec == 0x15) { uart_puts("M1 svc ok\n"); return; }   /* a deliberate svc #0: elr is already the next instruction */
    if (ec == 0x25 && kprobe_armed) { kprobe_faulted = 1; kprobe_armed = 0; f->elr += 4; return; }   /* skip the access */
    uart_puts("sync exception, ESR "); uart_hex(f->esr); uart_puts(" ELR "); uart_hex(f->elr); uart_puts("\n");
    for (;;) __asm__ volatile ("wfe");
}
void exc_irq(struct frame *f) {   /* the timer stops after three ticks, so a core asleep in wfi can only be woken by a device */
    (void)f;
    unsigned iar = GICC(0x0C), id = iar & 0x3FF;
#ifndef PI_BUILD
    if (id >= 48 && id < 80) {   /* QEMU virt wires virtio-mmio slot n to INTID 48 + n */
        unsigned long b = 0x0A000000UL + 0x200UL * (id - 48);
        REG(b + 0x64) = REG(b + 0x60);   /* acknowledge what the device raised, or a level interrupt never drops */
    }
#endif
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
static unsigned long l2[512] __attribute__((aligned(4096)));   /* M3a: the RAM GiB split into 2 MiB blocks... */
static unsigned long l3[512] __attribute__((aligned(4096)));   /* ...and one of those into 4 KiB pages, for the EL0 arena */
#ifdef PI_BUILD
#define RAM_GIB 0
#else
#define RAM_GIB 1
#endif
#define USER_BASE (((unsigned long)RAM_GIB << 30) + (64UL << 20))   /* the arena: one 2 MiB block, clear of the kernel image and the heap */
#define UP_CODE  0x0000UL   /* EL0 code, read-only and executable */
#define UP_STACK 0x2000UL   /* EL0 stack, one page; sp starts at its top */
#define UP_KERN  0x4000UL   /* a page in the same arena that only EL1 may touch */
static unsigned long heap_next;
#define HEAP_SIZE (16UL << 20)
void *kmalloc(unsigned int n) {   /* the name and shape drivers/ttf.c expects */
    unsigned long p = (heap_next + 15) & ~15UL;
    if (p + n > (unsigned long)_heap_start + HEAP_SIZE) return 0;
    heap_next = p + n;
    return (void *)p;
}
/* Identity map the first 4 GiB in four 1 GiB blocks: RAM is normal write-back memory, the peripherals are device
   memory (strongly ordered, no caching, no unaligned access). QEMU's virt keeps its devices in the first GiB and RAM in
   the second; a Pi 4 has RAM from 0 and its peripherals in the last GiB. */
static void mmu_init(void) {
    const unsigned long NORMAL = 0x1UL | (1UL << 2) /* attr 1 */ | (3UL << 8) /* inner shareable */ | (1UL << 10) /* access flag */ | (1UL << 54) /* EL0 may not execute it: only the user code page can */;
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
    /* M4: the GiB holding PCIe, as device memory. QEMU virt's ECAM config space sits at 0x40_1000_0000; a Pi 4's
       PCIe outbound window (the VL805 USB controller's registers) at 0x6_0000_0000. */
#ifdef PI_BUILD
    l1[0x600000000UL >> 30] = 0x600000000UL | DEVICE;
#else
    l1[0x4000000000UL >> 30] = 0x4000000000UL | DEVICE;
#endif
    /* M3a: the RAM GiB becomes a table of 2 MiB blocks (all kernel-only, as before), and the arena block a table of 4 KiB pages */
    for (int i = 0; i < 512; i++) l2[i] = (((unsigned long)RAM_GIB << 30) + ((unsigned long)i << 21)) | NORMAL;
    for (int i = 0; i < 512; i++) l3[i] = 0;
    l2[(USER_BASE - ((unsigned long)RAM_GIB << 30)) >> 21] = (unsigned long)l3 | 3;
    l1[RAM_GIB] = (unsigned long)l2 | 3;
    unsigned long mair = (0xFFUL << 8) | 0x00UL;                       /* attr 1 normal write-back, attr 0 device-nGnRnE */
    unsigned long tcr = 25UL | (1UL << 8) | (1UL << 10) | (3UL << 12) | (1UL << 23) | (2UL << 32);   /* 39-bit VA, 4 KiB pages, write-back, TTBR1 off, 40-bit PA */
    unsigned long sctlr;
    __asm__ volatile ("msr mair_el1, %0\n msr tcr_el1, %1\n msr ttbr0_el1, %2\n dsb sy\n isb\n tlbi vmalle1\n dsb sy\n isb" :: "r"(mair), "r"(tcr), "r"((unsigned long)l1) : "memory");
    __asm__ volatile ("mrs %0, sctlr_el1" : "=r"(sctlr));
    sctlr |= (1UL << 0) | (1UL << 2) | (1UL << 12);                   /* MMU, data cache, instruction cache */
    __asm__ volatile ("msr sctlr_el1, %0\n isb" :: "r"(sctlr) : "memory");
}
void kfree(void *p) { (void)p; }   /* a bump heap never frees; text.c rolls the heap back instead (heap_mark, heap_release) */
unsigned long heap_mark(void) { return heap_next; }
void heap_release(unsigned long m) { heap_next = m; }
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

/* ---- M3a: the first EL0 program, a syscall layer, and a page EL0 may not touch ----
   The arena is four mapped 4 KiB pages inside the identity map: the program's code (EL0 read and execute, nobody writes),
   its stack (EL0 read and write, never executable) and a kernel-only page (AP=00: EL1 only) holding a secret. The program
   is user.S, copied in. Syscalls use svc #0 with the number in x8 (Linux's: write 64, exit 93), arguments in x0 to x2 and
   the result in x0. A write only reads user memory that really is the program's own; anything else gets -EFAULT. A fault
   in EL0 kills the program and the kernel carries on. */
extern char _start[];
extern char user_start[], user_end[], user_entry_a[], user_entry_b[], user_entry_c[];
extern unsigned long enter_user(unsigned long entry, unsigned long sp, unsigned long arg, unsigned long arg2);
extern void leave_user(unsigned long status) __attribute__((noreturn));
#define SYS_WRITE 64
#define SYS_EXIT 93
static unsigned long user_status;
static int user_in_range(unsigned long a, unsigned long n) {   /* wholly inside the code page or the stack page */
    if (n > 256) return 0;
    unsigned long c = USER_BASE + UP_CODE, st = USER_BASE + UP_STACK;
    return (a >= c && a <= c + 4096 - n) || (a >= st && a <= st + 4096 - n);   /* written so a + n can never wrap */
}
void exc_el0_sync(struct frame *f) {
    unsigned ec = (unsigned)(f->esr >> 26);
    if (ec == 0x15) {   /* svc from EL0 */
        unsigned long n = f->x[8];
        if (n == SYS_WRITE) {
            const char *p = (const char *)f->x[1]; unsigned long len = f->x[2];
            if (f->x[0] != 1 || !user_in_range((unsigned long)p, len)) { f->x[0] = (unsigned long)-14; return; }   /* -EFAULT */
            for (unsigned long i = 0; i < len; i++) uart_putc(p[i]);
            f->x[0] = len;
        } else if (n == SYS_EXIT) {
            uart_puts("M3 EL0 exit "); uart_dec((unsigned)f->x[0]); uart_putc('\n');
            leave_user(f->x[0]);
        } else f->x[0] = (unsigned long)-38;   /* -ENOSYS */
        return;
    }
    unsigned long far; __asm__ volatile ("mrs %0, far_el1" : "=r"(far));
    if (ec == 0x24) {
        unsigned dfsc = (unsigned)(f->esr & 0x3f);
        uart_puts("M3 EL0 fault: data abort, ");
        uart_puts((dfsc & 0x3c) == 0x0c ? "permission fault level " : (dfsc & 0x3c) == 0x04 ? "translation fault level " : "fault code ");
        uart_dec((dfsc & 0x3c) == 0x0c || (dfsc & 0x3c) == 0x04 ? (dfsc & 3) : dfsc);
        uart_puts(", address "); uart_hex(far); uart_putc('\n');
    } else if (ec == 0x20) {
        unsigned ifsc = (unsigned)(f->esr & 0x3f);
        uart_puts("M3 EL0 fault: instruction abort, ");
        uart_puts((ifsc & 0x3c) == 0x0c ? "permission fault level " : (ifsc & 0x3c) == 0x04 ? "translation fault level " : "fault code ");
        uart_dec((ifsc & 0x3c) == 0x0c || (ifsc & 0x3c) == 0x04 ? (ifsc & 3) : ifsc);
        uart_puts(", address "); uart_hex(f->elr); uart_putc('\n');
    } else { uart_puts("M3 EL0 fault: exception class "); uart_hex(ec); uart_puts(", address "); uart_hex(far); uart_putc('\n'); }
    leave_user((unsigned long)-1);
}
static void user_init(void) {
    const unsigned long PAGE = 0x3UL | (1UL << 2) | (3UL << 8) | (1UL << 10);   /* valid page, normal memory, inner shareable, access flag */
    const unsigned long AP_EL0_RW = 1UL << 6, AP_RO = 3UL << 6, PXN = 1UL << 53, UXN = 1UL << 54;
    l3[UP_CODE >> 12] = (USER_BASE + UP_CODE) | PAGE | AP_EL0_RW | PXN | UXN;   /* writable for now, so the kernel can copy the program in */
    l3[UP_STACK >> 12] = (USER_BASE + UP_STACK) | PAGE | AP_EL0_RW | PXN | UXN;
    l3[UP_KERN >> 12] = (USER_BASE + UP_KERN) | PAGE | PXN | UXN;               /* AP=00: EL1 only */
    __asm__ volatile ("dsb ishst\n tlbi vmalle1\n dsb ish\n isb" ::: "memory");
    char *code = (char *)(USER_BASE + UP_CODE);
    for (unsigned long i = 0; i < (unsigned long)(user_end - user_start); i++) code[i] = user_start[i];
    const char *secret = "KERNEL-ONLY-SECRET";
    for (int i = 0; secret[i]; i++) ((char *)(USER_BASE + UP_KERN))[i] = secret[i];
    for (unsigned long a = (unsigned long)code & ~63UL; a < (unsigned long)code + (unsigned long)(user_end - user_start); a += 64)
        __asm__ volatile ("dc cvau, %0" :: "r"(a) : "memory");
    __asm__ volatile ("dsb ish\n ic iallu\n dsb ish\n isb" ::: "memory");
    l3[UP_CODE >> 12] = (USER_BASE + UP_CODE) | PAGE | AP_RO | PXN;             /* now read-only for everyone, executable by EL0 only */
    __asm__ volatile ("dsb ishst\n tlbi vmalle1\n dsb ish\n isb" ::: "memory");
}
static void user_demo(void) {
    user_init();
    unsigned long code = USER_BASE + UP_CODE, sp = USER_BASE + UP_STACK + 4096, kpage = USER_BASE + UP_KERN;
    uart_puts("M3 EL0 task starts\n");
    enter_user(code + (unsigned long)(user_entry_a - user_start), sp, kpage, 0);
    uart_puts("M3 kernel survived the fault\n");
    uart_puts("M3 EL0 jump task starts\n");   /* branch to kernel text: EL0 may not execute it */
    enter_user(code + (unsigned long)(user_entry_c - user_start), sp, kpage, (unsigned long)_start);
    uart_puts("M3 kernel survived the jump\n");
    uart_puts("M3 EL0 second task starts\n");
    user_status = enter_user(code + (unsigned long)(user_entry_b - user_start), sp, kpage, 0);
    if (user_status == 7) uart_puts("M3 userland ok\n"); else uart_puts("M3 userland FAIL\n");
}

/* ---- M1c: a framebuffer. QEMU's virt machine has no display unless one is asked for; ramfb is a plain RAM
   framebuffer the guest configures through fw_cfg (a DMA write of the "etc/ramfb" file). A Pi asks its GPU firmware
   for one through the mailbox, at the monitor's own size when the firmware knows it. Either way fb_setup hands back
   plain 32-bit pixels and the drawing is shared. Everything is laid out against an 800x600 design and scaled by
   height/600, so 800x600 (QEMU) draws exactly what it always did. ---- */
static unsigned fb_w = 800, fb_h = 600;   /* the screen we really got */
static unsigned disp_w, disp_h;           /* what the firmware said the monitor is, 0x0 when nobody asked */
static unsigned int *fb;
static unsigned fb_pitch;   /* in pixels */
static int fb_swap;         /* red and blue the other way round in memory */
static int sc(int v) { return v * (int)fb_h / 600; }   /* a length on the 800x600 design, at this screen's size */
static void dcache_clean(void *p, unsigned long n) {   /* push lines out to RAM, where a GPU or DMA engine reads */
    for (unsigned long a = (unsigned long)p & ~63UL; a < (unsigned long)p + n; a += 64) __asm__ volatile ("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile ("dsb sy" ::: "memory");
}
/* The framebuffer is ordinary cached RAM, and the GPU reads RAM, not the cache. Any pixel drawn and not cleaned out
   stays invisible, so every drawing step that can happen after boot ends with this over the rectangle it touched. */
static void fb_flush(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)fb_w) w = (int)fb_w - x;
    if (y + h > (int)fb_h) h = (int)fb_h - y;
    if (w <= 0 || h <= 0) return;
    for (int j = y; j < y + h; j++) {
        unsigned long a = (unsigned long)&fb[(unsigned)j * fb_pitch + (unsigned)x], e = a + (unsigned long)w * 4;
        for (a &= ~63UL; a < e; a += 64) __asm__ volatile ("dc civac, %0" :: "r"(a) : "memory");
    }
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
    fb = kmalloc(fb_w * fb_h * 4);
    fb_pitch = fb_w;
    static struct ramfb_cfg cfg __attribute__((aligned(16)));
    static struct fw_dma dma;
    cfg.addr = __builtin_bswap64((unsigned long)fb);
    cfg.fourcc = __builtin_bswap32(0x34325258);   /* 'XR24': 32-bit 0x00RRGGBB */
    cfg.flags = 0;
    cfg.width = __builtin_bswap32(fb_w); cfg.height = __builtin_bswap32(fb_h); cfg.stride = __builtin_bswap32(fb_w * 4);
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
    /* every wait is bounded: a wrong guess about the real GPU should print an error, not hang a first boot in silence */
    for (unsigned n = 0; MBOX_STATUS & 0x80000000u; n++) if (n > 5000000) return 0;   /* full */
    MBOX_WRITE = (unsigned)(a & ~15UL) | 8;
    for (unsigned n = 0;; n++) {
        for (unsigned m = 0; MBOX_STATUS & 0x40000000u; m++) if (m > 5000000) return 0;   /* empty */
        unsigned r = MBOX_READ;
        if (r == ((unsigned)(a & ~15UL) | 8)) break;
        if (n > 1000) return 0;
    }
    dcache_clean((void *)mbox, sizeof mbox);         /* civac also invalidates: the next reads come from RAM */
    return mbox[1] == 0x80000000u;
}
/* First ask how big the monitor is, so the picture fills it at its own pixels instead of an 800x600 box the firmware
   blows up. Past 2560 wide (a 4K panel) take exactly half each way: a 2x scale stays crisp and the buffer stays small.
   Anything odd, or no answer, keeps 800x600. */
static void fb_pick_size(void) {
    unsigned q[] = { 8 * 4, 0, 0x40003, 8, 0, 0, 0, 0 };   /* get physical (display) width and height */
    for (unsigned i = 0; i < sizeof q / 4; i++) mbox[i] = q[i];
    if (!mbox_call()) return;
    disp_w = mbox[5]; disp_h = mbox[6];
    if (disp_w > 8192 || disp_h > 8192) return;
    unsigned w = disp_w, h = disp_h;
    if (w > 2560) { w /= 2; h /= 2; }
    if (w >= 800 && h >= 600 && (w > 800 || h > 600)) { fb_w = w; fb_h = h; }
}
static int fb_setup(void) {
    fb_pick_size();
    unsigned m[] = { sizeof mbox, 0,
        0x48003, 8, 0, fb_w, fb_h,     /* physical size */
        0x48004, 8, 0, fb_w, fb_h,     /* virtual size */
        0x48005, 4, 0, 32,             /* depth */
        0x48006, 4, 0, 0,              /* pixel order BGR: blue in the low byte, so 0x00RRGGBB as a 32-bit word */
        0x40001, 8, 0, 4096, 0,        /* allocate, 4 KiB aligned: answers address and size */
        0x40008, 4, 0, 0,              /* pitch in bytes */
        0 };
    for (unsigned i = 0; i < sizeof m / 4; i++) mbox[i] = m[i];
    if ((!mbox_call() || !mbox[23]) && (fb_w != 800 || fb_h != 600)) {   /* the big size was refused: 800x600 worked before */
        fb_w = 800; fb_h = 600;
        m[5] = m[10] = 800; m[6] = m[11] = 600;
        for (unsigned i = 0; i < sizeof m / 4; i++) mbox[i] = m[i];
        mbox_call();
    }
    if (mbox[1] != 0x80000000u || !mbox[23]) { uart_puts("M1c mailbox framebuffer refused\n"); return 0; }
    fb = (unsigned int *)(unsigned long)(mbox[23] & 0x3FFFFFFF);   /* a VideoCore bus address: drop the alias bits */
    fb_pitch = mbox[28] / 4;
    fb_swap = mbox[19] == 1;   /* the firmware answers the order it really used; follow it if it overrode us */
    if (mbox[5] >= 800 && mbox[6] >= 600 && mbox[5] <= fb_pitch) { fb_w = mbox[5]; fb_h = mbox[6]; }   /* the size it really gave */
    return 1;
}
/* M4: tell the firmware the VL805 USB controller is out of PCIe reset, so it loads the VL805's firmware.
   Tag 0x00030058, the device as bus << 20 | slot << 15 | function << 12 (Linux reset-raspberrypi.c, Circle). */
int mbox_notify_xhci_reset(unsigned dev_addr) {
    unsigned m[] = { 7 * 4, 0, 0x30058, 4, 0, dev_addr, 0 };
    for (unsigned i = 0; i < sizeof m / 4; i++) mbox[i] = m[i];
    return mbox_call();
}
#endif
static void fb_rect(int x, int y, int w, int h, unsigned c) {
    if (fb_swap) c = (c & 0xFF00FF00u) | (c >> 16 & 0xFF) | (c & 0xFF) << 16;
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) fb[j * fb_pitch + i] = c;
}
/* ---- The boot log on the screen. Everything the kernel prints over the UART is also kept in a small buffer and drawn
   into the window, so a first boot with the monitor plugged in shows what happened even if the serial cable is wrong.
   Lines printed before the screen exists are replayed once it does. When the window fills, it is wiped and the newest
   half page is redrawn at the top, so the last lines printed are always the ones on screen. ---- */
#include "../../drivers/vgafont.h"
#define LOG_MAX 4096
#define CON_FG 0x00202020
#define CON_BG 0x00ffffff
int text_init(void);   /* arch/arm64/text.c: the DejaVu faces through drivers/ttf.c */
int text_draw(int which, const char *s, int x, int baseline, int px10, unsigned fg, unsigned *fb, unsigned pitch, int w, int h);
int text_width(int which, const char *s, int px10);
int text_selftest(int px10, int *w, int *h, int *adv);
static int text_ok;     /* smooth text is up */
static unsigned fb_color(unsigned c) { return fb_swap ? (c & 0xFF00FF00u) | (c >> 16 & 0xFF) | (c & 0xFF) << 16 : c; }
static char con_log[LOG_MAX];
static unsigned con_len, con_col, con_row;
static int con_live;    /* the framebuffer is up: draw as we go */
static int con_x, con_y, con_cw, con_ch, con_base, con_cols, con_rows, con_px10;
static int con_vga;     /* 0: DejaVu Sans Mono; n: the 8x16 VGA font drawn n times its size */
static int mono_w, mono_h, mono_adv, mono_ok;   /* the 'M' self-test, kept for the diagnostic line */
static void con_glyph(unsigned col, unsigned row, char c) {
    int x = con_x + (int)col * con_cw, y = con_y + (int)row * con_ch;
    if (!con_vga) {
        char b[2] = { c, 0 };
        text_draw(0, b, x, y + con_base, con_px10, fb_color(CON_FG), fb, fb_pitch, (int)fb_w, (int)fb_h);
    } else if (c >= VGAFONT_FIRST && c <= VGAFONT_LAST) {   /* integer only: no rasterizer, no floating point */
        const unsigned char *g = vgafont_glyphs + (c - VGAFONT_FIRST) * 16;
        unsigned px = fb_color(CON_FG);
        for (int gy = 0; gy < 16; gy++)
            for (int gx = 0; gx < 8; gx++)
                if (g[gy] & (0x80 >> gx))
                    for (int a = 0; a < con_vga; a++)
                        for (int b = 0; b < con_vga; b++) fb[(unsigned)(y + gy * con_vga + a) * fb_pitch + (unsigned)(x + gx * con_vga + b)] = px;
    }
    fb_flush(x - 4, y, con_cw + 8, con_ch);   /* a few pixels spare: a smooth glyph can lean past its cell */
}
static void con_wipe(void) {
    fb_rect(con_x, con_y, con_cols * con_cw, con_rows * con_ch, CON_BG);
    fb_flush(con_x - 4, con_y, con_cols * con_cw + 8, con_rows * con_ch);
    con_col = con_row = 0;
}
static int con_replaying;
static void con_draw(unsigned i);
static void con_scroll(unsigned end) {   /* the window is full: wipe it and redraw the newest half page from the log */
    con_wipe();
    if (con_replaying) return;
    unsigned start = end; int nl = 0;
    while (start > 0) { if (con_log[start - 1] == '\n' && ++nl > con_rows / 2) break; start--; }
    con_replaying = 1;
    for (unsigned k = start; k < end; k++) con_draw(k);
    con_replaying = 0;
}
static void con_draw(unsigned i) {   /* draws con_log[i]; a newline only moves the cursor, the next character scrolls */
    char c = con_log[i];
    if (c == '\r') return;
    if (c == '\n') { con_col = 0; con_row++; return; }
    if (con_col >= (unsigned)con_cols) { con_col = 0; con_row++; }
    if (con_row >= (unsigned)con_rows) {
        con_scroll(i);
        if (con_col >= (unsigned)con_cols) { con_col = 0; con_row++; }
        if (con_row >= (unsigned)con_rows) con_wipe();
    }
    con_glyph(con_col++, con_row, c);
}
static void console_putc(char c) {
    if (con_len == LOG_MAX) {   /* full: forget the older half */
        for (unsigned i = 0; i < LOG_MAX / 2; i++) con_log[i] = con_log[i + LOG_MAX / 2];
        con_len = LOG_MAX / 2;
    }
    con_log[con_len++] = c;
    if (con_live) con_draw(con_len - 1);
}
/* The console's type. DejaVu Sans Mono at the screen's scale, unless a quick test of the rasterizer says no: then the
   8x16 VGA font at a whole-number scale (2x on a 1080p screen), which needs nothing but integer stores. */
static void con_layout(int win_x, int win_y, int win_w, int win_h) {
    con_x = win_x + sc(8); con_y = win_y + sc(32);
    con_px10 = sc(133);
    mono_ok = text_ok && text_selftest(con_px10, &mono_w, &mono_h, &mono_adv);
    if (mono_ok) { con_vga = 0; con_cw = mono_adv; con_ch = sc(16); con_base = sc(12); }
    else {
        con_vga = ((int)fb_h * 10 / 600 + 5) / 10;
        if (con_vga < 1) con_vga = 1;
        con_cw = 8 * con_vga; con_ch = 16 * con_vga; con_base = 0;
    }
    con_cols = (win_w - sc(12)) / con_cw;
    con_rows = (win_h - sc(46)) / con_ch;
}
static void con_start(void) {   /* the screen is ready: replay what was printed before it */
    con_live = 1;
    con_col = con_row = 0;
    for (unsigned i = 0; i < con_len; i++) con_draw(i);
}

static void fb_init(void) {
    if (!fb_setup()) return;
    int W = (int)fb_w, H = (int)fb_h;
    int win_w = sc(500), win_h = sc(350), win_x = (W - win_w) / 2, win_y = sc(100);
    int dock_w = sc(200), dock_h = sc(44), dock_x = (W - dock_w) / 2, dock_y = H - sc(60);
    fb_rect(0, 0, W, H, 0x00203040);                      /* desktop */
    fb_rect(0, 0, W, sc(24), 0x00e0e0e0);                 /* menu bar */
    fb_rect(win_x, win_y, win_w, win_h, CON_BG);          /* a window */
    fb_rect(win_x, win_y, win_w, sc(28), 0x00b5502c);     /* its title bar, the house accent */
    fb_rect(dock_x, dock_y, dock_w, dock_h, 0x00505a68);  /* the dock */
    text_ok = text_init();
    if (text_ok) {
        text_draw(1, "Joshua Tree", sc(8), sc(17), sc(150), fb_color(0x00202020), fb, fb_pitch, W, H);              /* menu bar */
        text_draw(1, "Console", win_x + sc(10), win_y + sc(20), sc(150), fb_color(0x00ffffff), fb, fb_pitch, W, H);  /* title bar */
        text_draw(2, "ARM64", W - sc(80), sc(17), sc(130), fb_color(0x00505a68), fb, fb_pitch, W, H);
        /* Steve Jobs died on 5 October 2011. Fifteen years on, one quiet line above the dock. */
        const char *thanks = "Steve Jobs, 1955 to 2011. Thank you.";
        text_draw(2, thanks, (W - text_width(2, thanks, sc(110))) / 2, dock_y - sc(12), sc(110), fb_color(0x00a8b4c4), fb, fb_pitch, W, H);
    } else uart_puts("M1d text FAIL\n");
    con_layout(win_x, win_y, win_w, win_h);
    con_start();
    dcache_clean(fb, (unsigned long)fb_pitch * fb_h * 4);   /* the whole still picture out to RAM; con_glyph cleans as it goes from here */
    int ok = fb[(unsigned)(win_y + win_h - 4) * fb_pitch + fb_w / 2] == 0x00ffffff && fb[(unsigned)sc(10) * fb_pitch + fb_w / 2] == 0x00e0e0e0;   /* blank spots, clear of any text; grey and white read the same either way */
    uart_puts(ok ? "M1c fb ok\n" : "M1c fb FAIL\n");
}
/* One line that turns a photo of the screen into a measurement: where the firmware really put the kernel, the monitor
   size it reported, the buffer we got and its pitch in bytes, and the console font's self-test ('M': width x height,
   advance, verdict). For example "@0x80000 fb 1920x1080>1920x1080 p7680 M13x18 a14 ok". */
static void uart_hexs(unsigned long v) {   /* hex without the leading zeros */
    int i = 60; while (i > 0 && !((v >> i) & 15)) i -= 4;
    uart_puts("0x"); for (; i >= 0; i -= 4) uart_putc("0123456789abcdef"[(v >> i) & 15]);
}
static void fb_diag(void) {
    unsigned long at; __asm__ volatile ("adrp %0, _start\n add %0, %0, :lo12:_start" : "=r"(at));
    uart_puts("@"); uart_hexs(at);
    uart_puts(" fb "); uart_dec(disp_w); uart_putc('x'); uart_dec(disp_h);   /* monitor > buffer: 53 characters at most, */
    uart_putc('>'); uart_dec(fb_w); uart_putc('x'); uart_dec(fb_h);          /* so it fits one console row in either font */
    uart_puts(" p"); uart_dec(fb_pitch * 4);
    uart_puts(" M"); uart_dec((unsigned)mono_w); uart_putc('x'); uart_dec((unsigned)mono_h);
    uart_puts(" a"); uart_dec((unsigned)mono_adv);
    uart_puts(mono_ok ? " ok\n" : " FAIL\n");
}

static void m1_selftest(void) {
    __asm__ volatile ("msr vbar_el1, %0\n isb" :: "r"(vectors));
    uart_puts("M1 vectors set\n");
    m1b_selftest();   /* with the exception table in place a bad map prints instead of hanging */
    __asm__ volatile ("svc #0");                   /* proves the sync path and the return */
    unsigned long freq; __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
    if (!freq) freq = 54000000;                    /* a Pi 4 whose firmware left CNTFRQ unset runs its timer at 54 MHz */
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

/* ---- Input, from any driver: Linux evdev events (virtio input speaks them natively; the USB HID driver in xhci.c
   translates its reports into them). The pointer starts mid-screen; a tablet sets it, a mouse moves it. ---- */
struct input_event { unsigned short type, code; unsigned value; };
static unsigned mouse_x = 400, mouse_y = 300, mouse_moved;
static void input_event(struct input_event e) {
    if (e.type == 1) { uart_puts("key "); uart_dec(e.code); uart_puts(e.value ? " down\n" : " up\n"); }   /* EV_KEY: keys and buttons */
    else if (e.type == 3) {                                                                                /* EV_ABS: the tablet, 0..32767 */
        if (e.code == 0) mouse_x = e.value * fb_w / 32768; else if (e.code == 1) mouse_y = e.value * fb_h / 32768;
        mouse_moved = 1;
    } else if (e.type == 2) {                                                                              /* EV_REL: a mouse, clamped to the screen */
        int v = (int)e.value;
        if (e.code == 0) { int x = (int)mouse_x + v; mouse_x = x < 0 ? 0 : x >= (int)fb_w ? (int)fb_w - 1 : (unsigned)x; }
        else if (e.code == 1) { int y = (int)mouse_y + v; mouse_y = y < 0 ? 0 : y >= (int)fb_h ? (int)fb_h - 1 : (unsigned)y; }
        mouse_moved = 1;
    } else if (e.type == 0 && mouse_moved) {                                                               /* EV_SYN: one report done */
        uart_puts("mouse "); uart_dec(mouse_x); uart_putc(','); uart_dec(mouse_y); uart_putc('\n');
        mouse_moved = 0;
    }
}
void kinput(unsigned type, unsigned code, int value) { input_event((struct input_event){ (unsigned short)type, (unsigned short)code, (unsigned)value }); }
int usb_init(void);    /* xhci.c */
void usb_poll(void);

/* ---- M2: devices on QEMU's virt machine. 32 virtio-mmio slots from 0x0A000000, 0x200 apart, each says which device
   sits there: 1 is a network card, 18 is input (keyboard or tablet). Modern virtio (version 2) only: QEMU needs
   -global virtio-mmio.force-legacy=false. Every device talks through queues of buffers the guest lends it. The Pi gets
   USB through xHCI and Ethernet through the Genet MAC instead (M4). ---- */
#ifndef PI_BUILD
#define VIRTIO_BASE 0x0A000000UL
#define VQ 16
struct vq_desc { unsigned long addr; unsigned len; unsigned short flags, next; };
struct vq_avail { unsigned short flags, idx, ring[VQ], used_event; };
struct vq_used { unsigned short flags, idx; struct { unsigned id, len; } ring[VQ]; unsigned short avail_event; };
struct vq { unsigned long base; unsigned q; struct vq_desc *d; struct vq_avail *a; volatile struct vq_used *u; unsigned short seen; };
#define VR(b, o) REG((b) + (o))
static unsigned long vio_find(unsigned id, unsigned long after) {   /* the next slot past `after` holding device `id` */
    for (unsigned long b = after ? after + 0x200 : VIRTIO_BASE; b < VIRTIO_BASE + 32 * 0x200; b += 0x200)
        if (REG(b) == 0x74726976 && REG(b + 4) == 2 && REG(b + 8) == id) return b;   /* "virt", version 2 */
    return 0;
}
static int vio_start(unsigned long b, unsigned features) {   /* reset, then agree on VIRTIO_F_VERSION_1 plus `features` */
    VR(b, 0x70) = 0;
    VR(b, 0x70) = 1 | 2;                            /* acknowledge, driver */
    VR(b, 0x24) = 1; VR(b, 0x20) = 1;               /* features 32..63: VERSION_1 */
    VR(b, 0x24) = 0; VR(b, 0x20) = features & VR(b, 0x10);
    VR(b, 0x70) = 1 | 2 | 8;                        /* features ok */
    return (VR(b, 0x70) & 8) != 0;
}
static int vq_setup(struct vq *v, unsigned long b, unsigned q) {
    VR(b, 0x30) = q;
    if (VR(b, 0x34) < VQ) return 0;
    VR(b, 0x38) = VQ;
    v->base = b; v->q = q; v->seen = 0;
    v->d = kmalloc(sizeof *v->d * VQ); v->a = kmalloc(sizeof *v->a); v->u = kmalloc(sizeof *v->u);
    if (!v->d || !v->a || !v->u) return 0;
    v->a->flags = 0; v->a->idx = 0; v->u->idx = 0;
    VR(b, 0x80) = (unsigned)(unsigned long)v->d;  VR(b, 0x84) = (unsigned)((unsigned long)v->d >> 32);
    VR(b, 0x90) = (unsigned)(unsigned long)v->a;  VR(b, 0x94) = (unsigned)((unsigned long)v->a >> 32);
    VR(b, 0xA0) = (unsigned)(unsigned long)v->u;  VR(b, 0xA4) = (unsigned)((unsigned long)v->u >> 32);
    VR(b, 0x44) = 1;                                /* queue ready */
    return 1;
}
static void vq_give(struct vq *v, unsigned i, void *buf, unsigned len, int device_writes) {   /* lend descriptor i */
    v->d[i] = (struct vq_desc){ (unsigned long)buf, len, (unsigned short)(device_writes ? 2 : 0), 0 };
    v->a->ring[v->a->idx % VQ] = (unsigned short)i;
    __asm__ volatile ("dsb sy" ::: "memory");
    v->a->idx++;
}
static int vq_take(struct vq *v, unsigned *len) {   /* the next descriptor the device handed back, or -1 */
    if (v->seen == v->u->idx) return -1;
    __asm__ volatile ("dsb sy" ::: "memory");
    unsigned id = v->u->ring[v->seen % VQ].id;
    if (len) *len = v->u->ring[v->seen % VQ].len;
    v->seen++;
    return (int)id;
}
static void vq_kick(struct vq *v) { __asm__ volatile ("dsb sy" ::: "memory"); VR(v->base, 0x50) = v->q; }

/* Keyboard and mouse: one event queue each, lent 8-byte buffers the device fills with Linux evdev events. Both devices
   speak the same events, so one driver serves both. */
#define MAX_INPUT 2
static void gic_enable(unsigned id) {   /* a shared peripheral: on, priority, sent to core 0 */
    *(volatile unsigned char *)(GICD_BASE + 0x400 + id) = 0x80;
    *(volatile unsigned char *)(GICD_BASE + 0x800 + id) = 1;
    GICD(0x100 + 4 * (id / 32)) = 1u << (id % 32);
}
static struct vq vin[MAX_INPUT];
static struct input_event *vin_ev[MAX_INPUT];
static int nvin;
static int input_init(void) {
    for (unsigned long b = vio_find(18, 0); b && nvin < MAX_INPUT; b = vio_find(18, b)) {
        struct vq *v = &vin[nvin];
        struct input_event *ev = kmalloc(sizeof *ev * VQ);
        if (!ev || !vio_start(b, 0) || !vq_setup(v, b, 0)) continue;
        for (unsigned i = 0; i < VQ; i++) vq_give(v, i, &ev[i], sizeof *ev, 1);
        VR(b, 0x70) = 1 | 2 | 8 | 4;                /* driver ok */
        vq_kick(v);
        gic_enable(48 + (unsigned)((b - VIRTIO_BASE) / 0x200));   /* interrupt when an event lands */
        vin_ev[nvin++] = ev;
    }
    return nvin;
}
/* The interrupt only wakes the core and acknowledges the device; the events are read here, in the main loop. */
static void input_poll(void) {
    for (int n = 0; n < nvin; n++) {
        int id;
        while ((id = vq_take(&vin[n], 0)) >= 0) {
            input_event(vin_ev[n][id]);
            vq_give(&vin[n], (unsigned)id, &vin_ev[n][id], sizeof vin_ev[n][id], 1);   /* hand the buffer back */
            vq_kick(&vin[n]);
        }
    }
}

/* Network: queue 0 receives, queue 1 sends. Every frame carries a 12-byte virtio-net header in front, all zero here
   (no checksum offload, no segmentation). The first proof is ARP: ask QEMU's user-mode router (10.0.2.2) for its
   hardware address and print the answer. */
#define NET_BUF 1536
static struct vq net_rx, net_tx;
static unsigned char *net_rxbuf, *net_txbuf, net_mac[6];
static const unsigned char my_ip[4] = { 10, 0, 2, 15 }, gw_ip[4] = { 10, 0, 2, 2 };
static void uart_mac(const unsigned char *m) {
    for (int i = 0; i < 6; i++) { if (i) uart_putc(':'); uart_putc("0123456789abcdef"[m[i] >> 4]); uart_putc("0123456789abcdef"[m[i] & 15]); }
}
static int net_init(void) {
    unsigned long b = vio_find(1, 0);
    if (!b || !vio_start(b, 1u << 5 /* VIRTIO_NET_F_MAC */)) return 0;
    if (!vq_setup(&net_rx, b, 0) || !vq_setup(&net_tx, b, 1)) return 0;
    net_rxbuf = kmalloc(NET_BUF * VQ); net_txbuf = kmalloc(NET_BUF);
    if (!net_rxbuf || !net_txbuf) return 0;
    for (int i = 0; i < 6; i++) net_mac[i] = *(volatile unsigned char *)(b + 0x100 + i);   /* config space: mac first */
    for (unsigned i = 0; i < VQ; i++) vq_give(&net_rx, i, net_rxbuf + i * NET_BUF, NET_BUF, 1);
    VR(b, 0x70) = 1 | 2 | 8 | 4;                    /* driver ok */
    vq_kick(&net_rx);
    uart_puts("M2 net mac "); uart_mac(net_mac); uart_putc('\n');
    return 1;
}
static void net_arp_probe(void) {
    unsigned char *f = net_txbuf;
    for (int i = 0; i < 12; i++) f[i] = 0;          /* virtio-net header */
    unsigned char *e = f + 12;
    for (int i = 0; i < 6; i++) { e[i] = 0xFF; e[6 + i] = net_mac[i]; }
    e[12] = 0x08; e[13] = 0x06;                     /* ARP */
    const unsigned char hdr[8] = { 0, 1, 8, 0, 6, 4, 0, 1 };   /* Ethernet, IPv4, 6, 4, request */
    for (int i = 0; i < 8; i++) e[14 + i] = hdr[i];
    for (int i = 0; i < 6; i++) { e[22 + i] = net_mac[i]; e[32 + i] = 0; }
    for (int i = 0; i < 4; i++) { e[28 + i] = my_ip[i]; e[38 + i] = gw_ip[i]; }
    vq_give(&net_tx, 0, f, 12 + 42, 0);
    vq_kick(&net_tx);
    unsigned long freq, t0, t; __asm__ volatile ("mrs %0, cntfrq_el0\n mrs %1, cntpct_el0" : "=r"(freq), "=r"(t0));
    do {
        unsigned len; int id;
        while ((id = vq_take(&net_rx, &len)) >= 0) {
            unsigned char *r = net_rxbuf + (unsigned)id * NET_BUF + 12;
            int arp_reply = len >= 12 + 42 && r[12] == 0x08 && r[13] == 0x06 && r[20] == 0 && r[21] == 2;
            int from_gw = r[28] == gw_ip[0] && r[29] == gw_ip[1] && r[30] == gw_ip[2] && r[31] == gw_ip[3];
            vq_give(&net_rx, (unsigned)id, net_rxbuf + (unsigned)id * NET_BUF, NET_BUF, 1);
            vq_kick(&net_rx);
            if (arp_reply && from_gw) { uart_puts("M2 net gateway 10.0.2.2 is "); uart_mac(r + 22); uart_putc('\n'); return; }
        }
        __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(t));
    } while (t - t0 < freq * 2);
    uart_puts("M2 net no ARP reply\n");
}

/* Disk: one request queue. A request is three descriptors chained: a 16-byte header (type, sector), the data buffer,
   and one status byte the device writes back. Polled, one request at a time. */
struct blk_hdr { unsigned type, reserved; unsigned long sector; };
static struct vq blk_q;
static int blk_ok;
static int blk_init(void) {
    unsigned long b = vio_find(2, 0);
    if (!b || !vio_start(b, 0) || !vq_setup(&blk_q, b, 0)) return 0;
    VR(b, 0x70) = 1 | 2 | 8 | 4;                    /* driver ok */
    unsigned long sectors = (unsigned long)VR(b, 0x100) | (unsigned long)VR(b, 0x104) << 32;   /* config space: capacity */
    uart_puts("M2 blk sectors "); uart_dec((unsigned)sectors); uart_putc('\n');
    blk_ok = 1;
    return 1;
}
static int blk_read(unsigned long sector, void *buf) {   /* one 512-byte sector, 1 on success */
    static struct blk_hdr hdr __attribute__((aligned(16)));
    static volatile unsigned char status __attribute__((aligned(16)));
    hdr.type = 0; hdr.reserved = 0; hdr.sector = sector;   /* 0 = read */
    status = 0xFF;
    dcache_clean(&hdr, sizeof hdr); dcache_clean(buf, 512); dcache_clean((void *)&status, 1);   /* QEMU is coherent; a real DMA engine is not */
    blk_q.d[0] = (struct vq_desc){ (unsigned long)&hdr, sizeof hdr, 1 /* next */, 1 };
    blk_q.d[1] = (struct vq_desc){ (unsigned long)buf, 512, 1 | 2 /* next, device writes */, 2 };
    blk_q.d[2] = (struct vq_desc){ (unsigned long)&status, 1, 2, 0 };
    blk_q.a->ring[blk_q.a->idx % VQ] = 0;
    __asm__ volatile ("dsb sy" ::: "memory");
    blk_q.a->idx++;
    vq_kick(&blk_q);
    unsigned long freq, t0, t; __asm__ volatile ("mrs %0, cntfrq_el0\n mrs %1, cntpct_el0" : "=r"(freq), "=r"(t0));
    while (vq_take(&blk_q, 0) < 0) {
        __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(t));
        if (t - t0 > freq * 2) return 0;
    }
    dcache_clean(buf, 512); dcache_clean((void *)&status, 1);   /* drop our stale copies so the reads below see what the device wrote */
    return status == 0;
}
static void blk_probe(void) {
    static unsigned char sec[512] __attribute__((aligned(64)));
    if (!blk_read(0, sec)) { uart_puts("M2 blk read FAIL\n"); return; }
    sec[511] = 0;   /* the test disk's first sector is text, ends in a newline */
    uart_puts("M2 blk sector0 "); for (int i = 0; i < 32 && sec[i] >= 32 && sec[i] < 127; i++) uart_putc((char)sec[i]); uart_putc('\n');
}
#else
static int input_init(void) { return 0; }
static void input_poll(void) {}
static int net_init(void) { return 0; }
static void net_arp_probe(void) {}
static int blk_init(void) { return 0; }
static void blk_probe(void) {}
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
    user_demo();
    if (net_init()) net_arp_probe();
    if (blk_init()) blk_probe();
    fb_diag();   /* last, so it is the newest line on the screen */
    int inputs = input_init();
    if (inputs) { uart_puts("M2 input ready, devices "); uart_dec((unsigned)inputs); uart_putc('\n'); }
    if (usb_init()) {
        /* USB is polled, so nothing interrupts on its own: the virtual timer (INTID 27) wakes wfi every 2 ms. IRQs stay
           masked around wfi (a pending one still wakes it) and the timer is stopped before they are let through again,
           so its handler never runs; the short unmask is for the virtio devices, whose handler acknowledges them. */
        *(volatile unsigned char *)(GICD_BASE + 0x400 + 27) = 0x80;
        GICD(0x100) = 1u << 27;
        unsigned long step = timer_step / 25;   /* timer_step is 50 ms */
#ifdef PI_BUILD
        (void)step;
        for (;;) { usb_poll(); input_poll(); }   /* nothing on the Pi sleeps: no wake source to trust yet, so spin and poll */
#else
        for (;;) {
            __asm__ volatile ("msr daifset, #2\n msr cntv_tval_el0, %0\n msr cntv_ctl_el0, %1\n isb\n wfi\n"
                              " msr cntv_ctl_el0, xzr\n isb\n msr daifclr, #2\n isb" :: "r"(step), "r"(1UL) : "memory");
            usb_poll(); input_poll();
        }
#endif
    }
    if (inputs) for (;;) { __asm__ volatile ("wfi"); input_poll(); }   /* asleep until a device interrupts */
    for (;;) __asm__ volatile ("wfe");
}
