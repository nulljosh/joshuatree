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
static int con_quiet;   /* the rest of this line goes to the UART only: key echoes stay off the screen while the Terminal has the keys */
static int con_line_start = 1;   /* the next character begins a line */
static int con_to_term;          /* a Terminal command is running: what it prints goes to the Terminal, not the Console */
void term_output(int on) { con_to_term = on; }   /* ask.c, around one command */
static void uart_putc(char c) {
    while (REG(UART_FR) & TXFF) {}
    REG(UART_DR) = (unsigned char)c;
    if (!con_quiet) console_putc(c);
    if (c == '\n') con_quiet = 0;
    con_line_start = c == '\n';
}
/* The boot narrative is long, and on the screen it pushed the lines that matter out of the window. These line starts go to
   the UART only (the serial log is unchanged): the self-test chatter, the USB enumeration walk and the Wi-Fi steps that went
   fine. Every FAIL, every odd status and the summary lines still reach the screen. */
static int con_noise(const char *s) {
#ifdef CON_VERBOSE
    (void)s; return 0;   /* the scroll check builds with every line on the screen so there is a long log to scroll */
#else
    static const char *const skip[] = { "tick", "M0 ", "M1 ", "M1a ", "M1b ", "M1c fb ok", "M1d dock ", "M3 ", "EL0", "EL1", "booted at ",
        "usb ", "wifi power", "wifi sdio", "wifi f1", "wifi alp", "wifi chip", "wifi cores", "wifi arm", "wifi fw ", "wifi ht ",
        "wifi bus", "wifi radio up", "wifi ver", "wifi mac", "wifi found", "wifi scan", "wifi joining", "wifi handshake", "wifi assoc",
        "Wi-Fi: found", "Wi-Fi: looked", "Wi-Fi: this Pi", "Wi-Fi: chip", "Wi-Fi: connecting", "Wi-Fi: the router", "Wi-Fi: we answered", "Wi-Fi: handshake",
        "dhcp: lease", "net dhcp", "@", "M1d calendar", "Wi-Fi: connected", "Internet: online" };
    for (unsigned i = 0; i < sizeof skip / sizeof skip[0]; i++) {
        const char *a = s, *b = skip[i];
        while (*b && *a == *b) { a++; b++; }
        if (!*b) return 1;
    }
    return 0;
#endif
}
static void uart_puts(const char *s) {
    if (con_line_start && con_noise(s)) con_quiet = 1;
    while (*s) uart_putc(*s++);
}
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
static int term_front(void);   /* the Terminal window is open: it has the keyboard */
void kputs(const char *s) {
    if (term_front() && s[0] == 'u' && s[1] == 's' && s[2] == 'b' && s[3] == ' ' && s[4] == 'k' && s[5] == 'e' && s[6] == 'y' && s[7] == ' ')
        con_quiet = 1;   /* xhci.c's "usb key 0x0d j" echo: the letter is on the Terminal's ask> row, the line stays on the UART */
    uart_puts(s);
}
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

static void crash(const struct frame *f);   /* the crash screen, further down: it needs the framebuffer code */
#ifdef FP_TEST
void fp_clobber(void);   /* fptest.c: stands in for a handler that uses floating point */
unsigned long fp_spin(volatile unsigned *t, unsigned want);
#endif
volatile int kprobe_armed, kprobe_faulted;   /* M4: a driver probing for hardware that may not be there (pci.c) */
void exc_sync(struct frame *f) {
    unsigned ec = (unsigned)(f->esr >> 26);
    if (ec == 0x15) { uart_puts("M1 svc ok\n"); return; }   /* a deliberate svc #0: elr is already the next instruction */
    if (ec == 0x25 && kprobe_armed) { kprobe_faulted = 1; kprobe_armed = 0; f->elr += 4; return; }   /* skip the access */
    crash(f);
}
void exc_irq(struct frame *f) {   /* the timer stops after three ticks, so a core asleep in wfi can only be woken by a device */
    (void)f;
#ifdef FP_TEST
    fp_clobber();
#endif
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
    crash(f);   /* an exception on a vector nothing handles: it never returns */
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
/* The arena: one 2 MiB block, clear of the kernel image and the heap. 64 MiB into RAM unless the heap reaches that far
   (an image with a language model baked in, llm_model.S), then the first 2 MiB boundary after the heap. */
#define ARENA_LOW (((unsigned long)RAM_GIB << 30) + (64UL << 20))
#define HEAP_END ((unsigned long)_heap_start + HEAP_SIZE)
#define USER_BASE (HEAP_END <= ARENA_LOW ? ARENA_LOW : (HEAP_END + (2UL << 20) - 1) & ~((2UL << 20) - 1))
#define UP_CODE  0x0000UL   /* EL0 code, read-only and executable */
#define UP_STACK 0x2000UL   /* EL0 stack, one page; sp starts at its top */
#define UP_KERN  0x4000UL   /* a page in the same arena that only EL1 may touch */
static unsigned long heap_next;
#ifndef HEAP_SIZE   /* the oomtest builds (arch/arm64/Makefile) shrink it so an allocation fails on purpose */
#define HEAP_SIZE (16UL << 20)
#endif
static unsigned heap_oom;   /* allocations refused so far: fb_init tells an out-of-memory font failure from a bad font */
void *kmalloc(unsigned int n) {   /* the name and shape drivers/ttf.c expects; 0 when the heap is full, and every caller checks */
    unsigned long p = (heap_next + 15) & ~15UL;
    if (p + n > (unsigned long)_heap_start + HEAP_SIZE) { heap_oom++; return 0; }
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
            for (unsigned long i = 0; i < len; i++) { if (con_line_start) con_quiet = 1; uart_putc(p[i]); }   /* the EL0 self-test talks to the UART only */
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
static int sg(int v) { return v * (int)fb_h / 540; }   /* a length on the real desktop's 960x540 grid (docs/DESIGN.md), at this screen's size */
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
    if (!fb) { uart_puts("oom fb\n"); return 0; }   /* no screen, but the kernel carries on over the UART */
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
/* The Pi 4's green activity light is GPIO 42 on the chip itself (bcm2711-rpi-4-b.dts: led-act, gpio 42, active high),
   not on the firmware expander the Pi 3 uses, so it is driven straight through the GPIO registers: function select 4
   (pins 40 to 49, three bits each, pin 42 is bits 6 to 8) set to output, then GPSET1 or GPCLR1 bit 10 (pin 42 minus 32). */
int led_set(unsigned state) {
    unsigned f = REG(GPIO_BASE + 0x10);
    REG(GPIO_BASE + 0x10) = (f & ~(7u << 6)) | (1u << 6);
    REG(GPIO_BASE + (state ? 0x20 : 0x2C)) = 1u << 10;
    return 1;
}
static void led_wait(void) {   /* 300 ms on the generic counter: `ticks` stops at 3, so waiting on it never ends */
    unsigned long f, c, end;
    __asm__ volatile ("mrs %0, cntfrq_el0\n mrs %1, cntpct_el0" : "=r"(f), "=r"(c));
    end = c + (f ? f : 54000000) / 100 * 30;
    do __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(c)); while (c < end);
}
void led_blink(unsigned times) {   /* Samantha's [[led blink]] (ask.c). Never at boot: the boot call left the USB keyboard dead */
    for (unsigned i = 0; i < times; i++) { led_set(1); led_wait(); led_set(0); led_wait(); }
}
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
   half page is redrawn at the top, so the last lines printed are always the ones on screen. A USB or virtio keyboard
   scrolls it: Page Up and Page Down by half a page, Home to the first line, End to the newest. The title bar says which
   lines are shown, and a pinned row under the text keeps the latest wifi and usb status in view wherever you are. ---- */
#include "../../drivers/vgafont.h"
#define LOG_MAX 16384
#define CON_FG 0x00202020
#define CON_BG 0x00ffffff
int wall_paint(unsigned *fb, unsigned pitch, int w, int h, int swap);   /* arch/arm64/wall.c: the Satellite photo scaled onto the screen */
#define MENUBAR_H 26             /* docs/DESIGN.md: GUI_MENUBAR_H, on the 960x540 grid */
#define MENUBAR_RULE 0x00BDB8B0  /* docs/DESIGN.md: MENUBAR_RULE */
int text_init(void);   /* arch/arm64/text.c: the DejaVu faces through drivers/ttf.c */
int text_draw(int which, const char *s, int x, int baseline, int px10, unsigned fg, unsigned *fb, unsigned pitch, int w, int h);
int text_width(int which, const char *s, int px10);
int text_selftest(int px10, int *w, int *h, int *adv);
static int text_ok;     /* smooth text is up */
static unsigned fb_color(unsigned c) { return fb_swap ? (c & 0xFF00FF00u) | (c >> 16 & 0xFF) | (c & 0xFF) << 16 : c; }
/* Two windows share this renderer, each with its own text: the Console (the boot log, logs only, no input) and the
   Terminal (the ask> command line, docs/TERMINAL.md). cp is the one in front; the other only keeps its text. */
struct pane {
    char log[LOG_MAX];
    unsigned len, col, row;
    unsigned anchor;          /* offset in the log of the line at the top of the window while it follows the newest output */
    int scrolled;             /* the keyboard moved the view back: new output is only logged until End */
    unsigned vtop, vlast;     /* while scrolled: the first line shown (0 based) and the last (1 based) */
};
static struct pane con_p, term_p, *cp = &con_p;
static int con_wx, con_wy, con_ww;
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
    cp->col = cp->row = 0;
}
static int con_replaying;
static void con_draw(unsigned i);
static void con_scroll(unsigned end) {   /* the window is full: wipe it and redraw the newest half page from the log */
    con_wipe();
    if (con_replaying) return;
    unsigned start = end; int nl = 0;
    while (start > 0) { if (cp->log[start - 1] == '\n' && ++nl > con_rows / 2) break; start--; }
    cp->anchor = start;
    con_replaying = 1;
    for (unsigned k = start; k < end; k++) con_draw(k);
    con_replaying = 0;
}
static void con_draw(unsigned i) {   /* draws cp->log[i]; a newline only moves the cursor, the next character scrolls */
    char c = cp->log[i];
    if (c == '\r') return;
    if (c == '\n') { cp->col = 0; cp->row++; return; }
    if (cp->col >= (unsigned)con_cols) { cp->col = 0; cp->row++; }
    if (cp->row >= (unsigned)con_rows) {
        con_scroll(i);
        if (cp->col >= (unsigned)con_cols) { cp->col = 0; cp->row++; }
        if (cp->row >= (unsigned)con_rows) con_wipe();
    }
    con_glyph(cp->col++, cp->row, c);
}
static unsigned con_nlines(void) {   /* lines in the log, a last line still being written counts */
    unsigned n = 0;
    for (unsigned i = 0; i < cp->len; i++) if (cp->log[i] == '\n') n++;
    return n + (cp->len && cp->log[cp->len - 1] != '\n');
}
static unsigned con_line_off(unsigned line) {   /* where line n starts in the log */
    unsigned i = 0;
    while (line && i < cp->len) { if (cp->log[i++] == '\n') line--; }
    return i;
}
static unsigned con_off_line(unsigned off) { unsigned n = 0; for (unsigned i = 0; i < off && i < cp->len; i++) if (cp->log[i] == '\n') n++; return n; }
#define CON_TITLE_BG  0x00F5F0EB   /* the window frame's cream title band (gui_draw_window_frame) */
#define CON_HINT_INK  0x0075726E   /* secondary grey ink on it */
static void con_text(const char *s, int right, int y) {   /* a short grey string, right edge at x = right, vertically in the title band */
    int px = sc(110);
    if (text_ok) { text_draw(1, s, right - text_width(1, s, px), y + sc(20), px, fb_color(CON_HINT_INK), fb, fb_pitch, (int)fb_w, (int)fb_h); return; }
    int n = 0; while (s[n]) n++;
    int x = right - n * 8 * con_vga; y += (sc(28) - 16 * con_vga) / 2;
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (c < VGAFONT_FIRST || c > VGAFONT_LAST) continue;
        const unsigned char *g = vgafont_glyphs + (c - VGAFONT_FIRST) * 16;
        for (int gy = 0; gy < 16; gy++) for (int gx = 0; gx < 8; gx++) if (g[gy] & (0x80 >> gx))
            for (int a = 0; a < con_vga; a++) for (int b = 0; b < con_vga; b++)
                fb[(unsigned)(y + gy * con_vga + a) * fb_pitch + (unsigned)(x + (i * 8 + gx) * con_vga + b)] = fb_color(CON_HINT_INK);
    }
}
static void con_hint(void) {   /* the title bar's "lines 12-27 of 61": a photo says which part of the log it shows */
    unsigned total = con_nlines(), first = cp->scrolled ? cp->vtop : con_off_line(cp->anchor), last = cp->scrolled ? cp->vlast : total;
    char b[40]; int n = 0;
    for (const char *t = "lines "; *t; ) b[n++] = *t++;
    unsigned v[3] = { first + 1, last, total };
    for (int k = 0; k < 3; k++) {
        char d[12]; int m = 0; unsigned x = v[k];
        do { d[m++] = (char)('0' + x % 10); x /= 10; } while (x);
        while (m) b[n++] = d[--m];
        if (k == 0) b[n++] = '-'; else if (k == 1) { b[n++] = ' '; b[n++] = 'o'; b[n++] = 'f'; b[n++] = ' '; }
    }
    b[n] = 0;
    /* right of the centred title and left of the rounded corner, above the hairline: only the band's flat cream */
    int x0 = con_wx + con_ww / 2 + sc(40), y0 = con_wy + sc(3), w = con_ww / 2 - sc(60), h = sc(24);
    fb_rect(x0, y0, w, h, CON_TITLE_BG);
    con_text(b, x0 + w - sc(4), con_wy);
    fb_flush(x0, y0, w, h);
}
static int con_is_status(const char *l, unsigned n) {   /* wifi and usb lines that report a failure; no news is good news */
    int bad = 0; for (unsigned i = 0; i + 4 <= n; i++) if (l[i] == 'F' && l[i + 1] == 'A' && l[i + 2] == 'I' && l[i + 3] == 'L') bad = 1;
    if (!bad) return 0;
    if (n >= 4 && l[0] == 'w' && l[1] == 'i' && l[2] == 'f' && l[3] == 'i') return 1;
    if (n >= 4 && l[0] == 'u' && l[1] == 's' && l[2] == 'b' && l[3] == ' ') return !(n >= 8 && l[4] == 'k' && l[5] == 'e' && l[6] == 'y' && l[7] == ' ');
    return 0;
}
static void con_summary(void) {   /* the row under the text: the newest wifi and usb line, cut to fit, whatever is scrolled into view */
    if (cp != &con_p) return;   /* the Console's own row: the Terminal has its prompt there */
    unsigned wo = 0, wn = 0, uo = 0, un = 0;
    for (unsigned i = 0; i < con_p.len; ) {
        unsigned e = i; while (e < con_p.len && con_p.log[e] != '\n') e++;
        if (con_is_status(con_p.log + i, e - i)) { if (con_p.log[i] == 'w') { wo = i; wn = e - i; } else { uo = i; un = e - i; } }
        i = e + 1;
    }
    unsigned cols = (unsigned)con_cols > 90 ? 90 : (unsigned)con_cols, half = cols > 4 ? (cols - 3) / 2 : 1;
    char b[96]; unsigned n = 0;
    for (int part = 0; part < 2; part++) {
        const char *l = part ? con_p.log + uo : con_p.log + wo, *none = part ? "usb -" : "wifi -"; unsigned ln = part ? un : wn;
        unsigned start = n;
        if (!ln) { l = none; (void)l; ln = 0; }
        for (unsigned i = 0; i < ln && n - start < half; i++) b[n++] = (l[i] >= 32 && l[i] < 127) ? l[i] : '?';
        if (!part) { while (n - start < half) b[n++] = ' '; b[n++] = '|'; b[n++] = ' '; }
    }
    if (!wn && !un) n = 0;   /* nothing broke: an empty row */
    int y = con_y + con_rows * con_ch;
    fb_rect(con_x, y, con_cols * con_cw, con_ch, CON_BG);
    fb_flush(con_x - 4, y, con_cols * con_cw + 8, con_ch);
    for (unsigned i = 0; i < n; i++) if (b[i] != ' ') con_glyph(i, (unsigned)con_rows, b[i]);
}
static int con_render(unsigned top) {   /* draws the window from line `top` down; 1 if the end of the log fits */
    unsigned nl = con_nlines(), line = top, last = top;
    con_wipe();
    for (unsigned k = con_line_off(top); k < cp->len; k++) {
        char c = cp->log[k];
        if (c == '\r') continue;
        if (c == '\n') {
            cp->col = 0; cp->row++; line++;
            if (cp->row >= (unsigned)con_rows) { cp->vlast = last + 1; return 0; }
            last = line; continue;
        }
        if (cp->col >= (unsigned)con_cols) { cp->col = 0; cp->row++; }
        if (cp->row >= (unsigned)con_rows) { cp->vlast = last + 1; return 0; }
        con_glyph(cp->col++, cp->row, c);
        last = line;
    }
    cp->vlast = last < nl ? last + 1 : nl;
    return 1;
}
static void con_follow(void) {   /* End: back to the newest, the same picture the streaming draw would have made */
    if (!cp->scrolled) return;
    cp->scrolled = 0;
    con_wipe();
    for (unsigned k = cp->anchor; k < cp->len; k++) con_draw(k);
}
static int con_key(unsigned code, unsigned value) {   /* Page Up 104, Page Down 109, Home 102, End 107: 1 if the console took the key */
    if (code != 104 && code != 109 && code != 102 && code != 107) return 0;
    if (!con_live || !value) return 1;
    unsigned half = (unsigned)con_rows / 2 ? (unsigned)con_rows / 2 : 1, nl = con_nlines();
    if (code == 104 || code == 102) {
        unsigned cur = cp->scrolled ? cp->vtop : con_off_line(cp->anchor);
        cp->vtop = code == 102 ? 0 : cur > half ? cur - half : 0;
        cp->scrolled = 1;
        con_render(cp->vtop);
    } else if (code == 109 && cp->scrolled) {
        cp->vtop += half; if (cp->vtop >= nl) cp->vtop = nl ? nl - 1 : 0;
        if (con_render(cp->vtop)) con_follow(); /* the end of the log is in view: follow again */
    } else if (code == 107) con_follow();
    con_hint();
    return 1;
}
static int cur_hold(void);           /* slice 4, below: take the pointer's arrow off the Console while it draws */
static void cur_release(int held);   /* ...and put it back */
static void pane_putc(struct pane *p, char c) {
    if (p->len == LOG_MAX) {   /* full: forget the older half, up to a line end */
        unsigned drop = LOG_MAX / 2, gone = 0;
        while (drop < p->len && p->log[drop - 1] != '\n') drop++;
        for (unsigned i = 0; i < drop; i++) if (p->log[i] == '\n') gone++;
        for (unsigned i = drop; i < p->len; i++) p->log[i - drop] = p->log[i];
        p->len -= drop;
        p->anchor = p->anchor > drop ? p->anchor - drop : 0;
        p->vtop = p->vtop > gone ? p->vtop - gone : 0; p->vlast = p->vlast > gone ? p->vlast - gone : 0;
    }
    p->log[p->len++] = c;
    if (!con_live || p != cp) return;   /* a window that is closed or behind only keeps its text */
    int held = cur_hold();
    unsigned was = cp->anchor;
    if (!cp->scrolled) con_draw(cp->len - 1);
    if (c == '\n' || cp->anchor != was) {
        unsigned e = cp->len - 1, s = e; while (s > 0 && cp->log[s - 1] != '\n') s--;
        if (c == '\n' && con_is_status(cp->log + s, e - s)) con_summary();
        con_hint();
    }
    cur_release(held);
}
static void console_putc(char c) { pane_putc(con_to_term ? &term_p : &con_p, c); }
/* The console's type. DejaVu Sans Mono at the screen's scale, unless a quick test of the rasterizer says no: then the
   8x16 VGA font at a whole-number scale (2x on a 1080p screen), which needs nothing but integer stores. */
static void con_layout(int win_x, int win_y, int win_w, int win_h) {
    con_wx = win_x; con_wy = win_y; con_ww = win_w;
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
    con_rows = (win_h - sc(46)) / con_ch - 1;   /* then one row: the Console's wifi and usb status, or the Terminal's ask> prompt */
}
/* The Terminal's bottom row: "ask> " and the line being typed (ask.c), its tail when it is longer than the row, then a cursor. */
void con_prompt(const char *s, unsigned n) {
    if (!con_live || cp != &term_p) return;
    int held = cur_hold();
    unsigned row = (unsigned)con_rows, cols = (unsigned)con_cols, room = cols > 7 ? cols - 6 : 1;
    int y = con_y + (int)row * con_ch;
    fb_rect(con_x, y, con_cols * con_cw, con_ch, CON_BG);
    fb_flush(con_x - 4, y, con_cols * con_cw + 8, con_ch);
    const char *p = "ask> ";
    unsigned col = 0;
    for (; *p; p++) con_glyph(col++, row, *p);
    if (n > room) { s += n - room; n = room; }
    for (unsigned i = 0; i < n; i++, col++) if (s[i] != ' ') con_glyph(col, row, s[i]);
    con_glyph(col, row, '_');
    cur_release(held);
}
int con_columns(void) { return con_live ? con_cols : 0; }
static void con_start(void) {   /* the screen is ready: replay what was printed before it */
    con_live = 1;
    cp->col = cp->row = 0;
    for (unsigned i = 0; i < cp->len; i++) con_draw(i);
    con_summary(); con_hint();
}

/* ---- The crash screen. An unexpected exception at EL1 (a kernel bug; the deliberate EL0 faults are answered in
   exc_el0_sync) ends here: the report goes out on the UART first, then onto the screen as a red panel over whatever was
   drawn, then the core sleeps in a wfe loop for good. Nothing here uses floating point or the console (which may be
   the thing that faulted): the UART is written raw and the panel is the 8x16 VGA font, integer stores only, cleaned
   out of the data cache so a real GPU shows it. A fault inside this code halts quietly rather than looping. ---- */
#define CRASH_BG 0x00900000
#define CRASH_ROWS 12
#define CRASH_COLS 100
static char crash_txt[CRASH_ROWS][CRASH_COLS];
static unsigned crash_row, crash_col;
static void crash_c(char c) { if (crash_row < CRASH_ROWS && crash_col < CRASH_COLS - 1) crash_txt[crash_row][crash_col++] = c; }
static void crash_s(const char *s) { while (*s) crash_c(*s++); }
static void crash_x(unsigned long v) { crash_s("0x"); for (int i = 60; i >= 0; i -= 4) crash_c("0123456789abcdef"[(v >> i) & 15]); }
static void crash_nl(void) { if (crash_row < CRASH_ROWS) crash_txt[crash_row][crash_col] = 0; crash_row++; crash_col = 0; }
static const char *crash_class(unsigned ec) {
    switch (ec) {
    case 0x00: return "unknown reason";   case 0x01: return "wfi or wfe trapped";   case 0x07: return "floating point access trapped";
    case 0x0e: return "illegal execution state";   case 0x15: return "svc";   case 0x20: case 0x21: return "instruction abort";
    case 0x22: return "pc alignment fault";   case 0x24: case 0x25: return "data abort";   case 0x26: return "sp alignment fault";
    case 0x2c: return "floating point exception";   case 0x2f: return "serror";   case 0x3c: return "brk";
    default: return "exception";
    }
}
static void crash(const struct frame *f) {
    static int crashing;
    __asm__ volatile ("msr daifset, #3" ::: "memory");   /* no interrupt may run while we report */
    if (crashing++) for (;;) __asm__ volatile ("wfe");   /* a second fault, in here: stay quiet and halted */
    con_live = 0;   /* console_putc only keeps the log from now on: the screen is ours */
    unsigned long far; __asm__ volatile ("mrs %0, far_el1" : "=r"(far));
    unsigned ec = (unsigned)(f->esr >> 26), fsc = (unsigned)(f->esr & 0x3f);
    crash_s("KERNEL CRASH: "); crash_s(crash_class(ec));
    if (ec == 0x20 || ec == 0x21 || ec == 0x24 || ec == 0x25) {
        crash_s((fsc & 0x3c) == 0x0c ? ", permission fault level " : (fsc & 0x3c) == 0x04 ? ", translation fault level " : (fsc & 0x3c) == 0x08 ? ", access flag fault level " : ", fault code ");
        crash_c((char)('0' + ((fsc & 0x3c) == 0x0c || (fsc & 0x3c) == 0x04 || (fsc & 0x3c) == 0x08 ? (fsc & 3) : fsc % 10)));
    }
    crash_nl();
    crash_s("ESR "); crash_x(f->esr); crash_s(" EC "); crash_x(ec); crash_nl();
    crash_s("FAR "); crash_x(far); crash_nl();
    crash_s("ELR "); crash_x(f->elr); crash_nl();
    crash_s("last console lines:"); crash_nl();
    unsigned end = con_p.len;
    if (end && con_p.log[end - 1] == '\n') end--;
    unsigned start = end; int nl = 0;
    while (start > 0) { if (con_p.log[start - 1] == '\n' && ++nl >= 5) break; start--; }
    for (unsigned i = start; i < end; i++) { if (con_p.log[i] == '\n') crash_nl(); else if (con_p.log[i] != '\r') crash_c(con_p.log[i]); }
    crash_nl();
    crash_s("halted: this core sleeps in a wfe loop"); crash_nl();
    for (unsigned r = 0; r < CRASH_ROWS && r < crash_row; r++) {   /* the UART first: it works whatever state the screen is in */
        for (const char *p = crash_txt[r]; *p; p++) { while (REG(UART_FR) & TXFF) {} REG(UART_DR) = (unsigned char)*p; }
        while (REG(UART_FR) & TXFF) {}
        REG(UART_DR) = '\n';
    }
    if (fb) {
        int s = ((int)fb_h * 10 / 600 + 5) / 10; if (s < 1) s = 1;
        int px = 16 * s, py = 32 * s, pw = (int)fb_w - 32 * s, ph = 16 * s * (CRASH_ROWS + 2);
        fb_rect(px, py, pw, ph, CRASH_BG);
        unsigned white = fb_color(0x00ffffff);
        int cols = (pw - 16 * s) / (8 * s);
        for (unsigned r = 0; r < CRASH_ROWS && r < crash_row; r++)
            for (int c = 0; c < cols && crash_txt[r][c]; c++) {
                char ch = crash_txt[r][c];
                if (ch < VGAFONT_FIRST || ch > VGAFONT_LAST) continue;
                const unsigned char *g = vgafont_glyphs + (ch - VGAFONT_FIRST) * 16;
                int gx0 = px + 8 * s + c * 8 * s, gy0 = py + 8 * s + (int)r * 16 * s;
                for (int gy = 0; gy < 16; gy++) for (int gx = 0; gx < 8; gx++) if (g[gy] & (0x80 >> gx))
                    for (int a = 0; a < s; a++) for (int b = 0; b < s; b++) fb[(unsigned)(gy0 + gy * s + a) * fb_pitch + (unsigned)(gx0 + gx * s + b)] = white;
            }
        fb_flush(px, py, pw, ph);   /* out of the data cache to RAM: the GPU reads RAM */
    }
    for (;;) __asm__ volatile ("wfe");
}

/* Desktop slice 2: the dock is the i386 desktop's own painters (kernel/gui_paint.c) and layout (kernel/dock_geom.c),
   linked into this build. They draw through five window.h calls, answered here over the framebuffer: a 1080p or
   taller screen is the i386 desktop's 960x540 grid at scale 2, anything smaller is scale 1. */
#include "../../drivers/window.h"
#include "../../drivers/png.h"
#include "../../kernel/dock_geom.h"
#include "../../kernel/gui_paint.h"
#include "../../kernel/icon_art.h"
#include "../../kernel/boot_mark.h"
int dock_scale_pct = 7;   /* the i386 default (Settings can change it there; nothing does here yet) */
unsigned int window_scale(void) { return fb_h >= 1080 ? 2 : 1; }
unsigned int window_width(void) { return fb_w / window_scale(); }
unsigned int window_height(void) { return fb_h / window_scale(); }
void window_pixel_phys(int px, int py, unsigned int color) {
    if (px >= 0 && py >= 0 && px < (int)fb_w && py < (int)fb_h) fb[(unsigned)py * fb_pitch + (unsigned)px] = fb_color(color);
}
unsigned int window_get_pixel_phys(int px, int py) {
    return px >= 0 && py >= 0 && px < (int)fb_w && py < (int)fb_h ? fb_color(fb[(unsigned)py * fb_pitch + (unsigned)px]) : 0;
}
void window_fill_rect_phys(int px, int py, int w, int h, unsigned int color) {
    for (int y = py; y < py + h; y++) for (int x = px; x < px + w; x++) window_pixel_phys(x, y, color);
}
/* Slice 3: the logical calls the circle, capsule and window frame painters make. There is never an offscreen target
   here, and a logical pixel is a window_scale() square, as in drivers/window.c. */
int window_has_target(void) { return 0; }
void window_pixel(int x, int y, unsigned int color) { int s = (int)window_scale(); window_fill_rect_phys(x * s, y * s, s, s, color); }
void window_rect(int x, int y, int w, int h, unsigned int color) { int s = (int)window_scale(); window_fill_rect_phys(x * s, y * s, w * s, h * s, color); }
/* The painters' text: DejaVu Sans at 12 on the logical grid, the i386 UI size (a 24 pixel face in a 32 pixel line at
   scale 2), with the baseline 12.5 below the line box's top. Nothing draws before text_init has loaded the faces. */
void gui_text(const char *s, int x, int y, unsigned int fg) {
    int k = (int)window_scale();
    if (text_ok) text_draw(2, s, x * k, y * k + 25 * k / 2, 120 * k, fb_color(fg), fb, fb_pitch, (int)fb_w, (int)fb_h);
}
int gui_text_width(const char *s) { int k = (int)window_scale(); return text_ok ? text_width(2, s, 120 * k) / k : 0; }
/* The icon text (the Calendar face's month and day): bold sans, face 0..3 a 16, 20, 24 or 28 physical pixel face times
   mul, as i386's wx_text, with the line box's top at logical ly; DejaVu's ascent puts the baseline 93% of a face down. */
void gui_icon_text(const char *s, int lx, int ly, int face, int mul, unsigned int fg) {
    int k = (int)window_scale(), px = (16 + 4 * face) * mul;
    if (text_ok) text_draw(1, s, lx * k, ly * k + px * 93 / 100, px * 10, fb_color(fg), fb, fb_pitch, (int)fb_w, (int)fb_h);
}
int gui_icon_text_w(const char *s, int face, int mul) { int k = (int)window_scale(); return text_ok ? (text_width(1, s, (16 + 4 * face) * mul * 10) + k - 1) / k : 0; }
/* The wallpaper is already on the screen, so reading the framebuffer is reading the wallpaper, as long as the dock is
   painted before anything else covers its band. */
unsigned int gui_wallpaper_sample(int px, int py, int sway) { (void)sway; return window_get_pixel_phys(px, py); }
/* 2.25: the brand mark in the menu bar's corner, as on i386 (gui_draw_mark_sized): the small copy of the scribbled
   tree from kernel/boot_mark.h, box-filtered to T physical pixels and blended in ink over what is there. */
static void menu_mark_paint(int cx, int cy, int T, unsigned ink) {
    int ox = cx - T / 2, oy = cy - T / 2;
    for (int row = 0; row < T; row++) for (int col = 0; col < T; col++) {
        int c0 = col * MENU_MARK_W / T, c1 = (col + 1) * MENU_MARK_W / T, r0 = row * MENU_MARK_H / T, r1 = (row + 1) * MENU_MARK_H / T;
        if (c1 == c0) c1 = c0 + 1;
        if (r1 == r0) r1 = r0 + 1;
        int sum = 0, n = (c1 - c0) * (r1 - r0);
        for (int y = r0; y < r1; y++) for (int x = c0; x < c1; x++) sum += menu_mark_cov[y * MENU_MARK_W + x];
        unsigned a = (unsigned)(sum / n);
        if (!a) continue;
        unsigned d = window_get_pixel_phys(ox + col, oy + row), out = 0;
        for (int sh = 0; sh <= 16; sh += 8) out |= ((((ink >> sh) & 0xFF) * a + ((d >> sh) & 0xFF) * (255 - a)) / 255) << sh;
        window_pixel_phys(ox + col, oy + row, out);
    }
}
static void dock_paint(void) {
    static const int order[GUI_ICON_COUNT] = GUI_DOCK_DEFAULT_ORDER;
    gui_draw_dock_tray();
    int size = DOCK_ICON, pw = size * (int)window_scale(), y0 = gui_dock_y0(), drawn = 0;
    for (int slot = 0; slot < GUI_ICON_COUNT; slot++) {
        int icon = order[slot], cx = gui_slot_x(slot) + size / 2, cy_bottom = y0 + DOCK_PAD + size;
        gui_draw_icon_shadow(cx, cy_bottom, size);
        unsigned long mark = heap_mark();   /* the bump heap never frees: the decode and the tile are rolled back */
        unsigned int *tile = kmalloc((unsigned)(pw * pw) * 4);
        unsigned char *art = 0;
        unsigned aw = 0, ah = 0, ach = 0;
        if (tile && png_decode(ICON_ART[icon], ICON_ART_LEN[icon], &art, &aw, &ah, &ach) == 0 && art && aw == ICON_ART_SIZE && ah == ICON_ART_SIZE && ach == 4) {
            gui_icon_art_scale(art, tile, pw, DOCK_TRAY_COLOR);
            gui_blit_tile(tile, cx - size / 2, cy_bottom - size, size, DOCK_TRAY_COLOR);
            /* Calendar's art is a blank page; i386 writes the date on it (gui_calendar_face). The Pi has no battery
               clock and no time source yet, so it gets the face's "date unknown" dashes, never a made-up date. */
            if (icon == GUI_CALENDAR) { gui_calendar_face(cx, cy_bottom, size, 0, 0); uart_puts("M1d calendar face, date unknown\n"); }
            drawn++;
        }
        heap_release(mark);
    }
    uart_puts("M1d dock "); uart_dec((unsigned)drawn); uart_puts(" icons\n");
}
/* Slice 3: the hover label, the i386 one (gui_draw_dock_label). The first hover keeps a copy of the dock's band, so
   moving to another slot or to none puts the plain band back before the next label; nothing is allocated until then.
   The pointer drives it (slice 4, below); the dockhover test build calls it once at boot. Names follow APPS[] in
   kernel/kernel.c for GUI_DOCK_DEFAULT_ORDER (arm64-m1c-check.py compares them). */
static const char *const dock_names[GUI_ICON_COUNT] = {"Apps", "Burrow", "Mail", "Calendar", "Notes", "Reminders", "Terminal", "Samantha", "Weather", "Stocks", "Trash"};
static unsigned *dock_band;
static int cur_on;           /* slice 4: the pointer's arrow is on the screen */
static void cur_hide(void);
static void cur_show(void);
void dock_hover(int slot) {
    int s = (int)window_scale(), top = gui_dock_band_top() * s, rows = (int)fb_h - top;
    int arrow = cur_on;
    cur_hide();   /* the band copy must never hold the arrow, or every later restore paints a ghost of it */
    if (!dock_band) {
        unsigned long mark = heap_mark();
        dock_band = kmalloc((unsigned)rows * fb_pitch * 4);
        if (!dock_band) { heap_release(mark); uart_puts("oom dock hover\n"); return; }
        for (unsigned i = 0; i < (unsigned)rows * fb_pitch; i++) dock_band[i] = fb[(unsigned)top * fb_pitch + i];
    } else for (unsigned i = 0; i < (unsigned)rows * fb_pitch; i++) fb[(unsigned)top * fb_pitch + i] = dock_band[i];
    if (slot >= 0 && slot < GUI_ICON_COUNT) {
        gui_draw_dock_label(gui_slot_x(slot) + DOCK_ICON / 2, gui_dock_y0(), dock_names[slot]);
        con_quiet = 1;   /* the UART only: a pointer sweeping the dock would fill the Console */
        uart_puts("M1d hover "); uart_dec((unsigned)slot); uart_putc(' '); uart_puts(dock_names[slot]); uart_putc('\n');
    }
    fb_flush(0, top, (int)fb_w, rows);
    if (arrow) cur_show();
}

/* ---- Slice 4: the pointer. The i386 arrow (gui_paint.c's software cursor: a save of the pixels it covers, then the
   antialiased arrow blended on top) over the framebuffer. Anything else that draws where the arrow is takes it off
   first and puts it back after (cur_hold, cur_release), so the save never goes stale and nothing smears. Every change
   is cleaned out of the data cache, as the GPU reads RAM. The arrow shows from the first pointer event on, so a desktop
   nobody has touched looks exactly as before. mouse_x and mouse_y are physical pixels; the painters and the dock's hit
   test work on the logical grid, so both are divided by window_scale(). ---- */
static unsigned mouse_x = 400, mouse_y = 300, mouse_moved;   /* fb_init moves it to the middle of the real screen */
static int cur_wanted;                   /* a pointer has moved: show the arrow */
static int win_lx, win_ly, win_lw, win_lh;   /* the Console's frame on the logical grid */
static unsigned *con_under;              /* the wallpaper under that frame, saved before it was drawn: closing puts it back */
static int hover_slot = -1;
static void cur_flush(int lx, int ly) { int s = (int)window_scale(); fb_flush(lx * s, ly * s, CURSOR_W * s, CURSOR_H * s); }
static void cur_hide(void) {
    if (!cur_on) return;
    int x = cursor_saved_x, y = cursor_saved_y;
    gui_cursor_restore();
    cur_flush(x, y);
    cur_on = 0;
}
static void cur_show(void) {
    if (cur_on || !cur_wanted || !fb) return;
    int s = (int)window_scale(), x = (int)mouse_x / s, y = (int)mouse_y / s;
    gui_cursor_save(x, y);
    gui_draw_cursor(x, y);
    cur_flush(x, y);
    cur_on = 1;
}
static int cur_hold(void) {   /* the Console is about to draw: take the arrow off if it is over the Console's frame */
    if (!cur_on) return 0;
    int ax = cursor_saved_x, ay = cursor_saved_y;
    if (ax >= win_lx + win_lw || ax + CURSOR_W <= win_lx || ay >= win_ly + win_lh || ay + CURSOR_H <= win_ly) return 0;
    cur_hide();
    return 1;
}
static void cur_release(int held) { if (held) cur_show(); }
static void console_frame(void) {   /* the i386 window frame, and a white well for the text */
    int s = (int)window_scale();
    gui_draw_window_frame(win_lx, win_ly, win_lw, win_lh, cp == &term_p ? "Terminal" : "Console");
    fb_rect((win_lx + 8) * s, (win_ly + 30) * s, (win_lw - 16) * s, (win_lh - 38) * s, CON_BG);
}
static int term_front(void) { return cp == &term_p && (con_live || !fb); }   /* with no screen at all, the UART is the Terminal */
/* The red close button puts the wallpaper back where the window was. The log keeps every line (and the UART still
   prints them); a click on any dock tile opens it again with the newest lines, so the one debug view on a Pi can
   never be lost for good. The Terminal (its tile, or F1) takes the same rectangle: one of the two is in front. */
static void console_close(void) {
    if (!con_live || !con_under) return;
    int s = (int)window_scale(), x = win_lx * s, y = win_ly * s, w = win_lw * s, h = win_lh * s;
    cur_hide();
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) fb[(unsigned)(y + j) * fb_pitch + (unsigned)(x + i)] = con_under[j * w + i];
    fb_flush(x, y, w, h);
    con_live = 0;
    uart_puts(cp == &term_p ? "terminal closed\n" : "console closed\n");
    cur_show();
}
void ask_redraw(void);   /* ask.c: the line being typed, back on the ask> row */
static void pane_open(struct pane *p) {   /* the Console or the Terminal in front, its newest lines in view */
    if (con_live && cp == p) return;
    if (!fb) { cp = p; uart_puts(p == &term_p ? "terminal open\n" : "console open\n"); return; }   /* no screen: the keys still follow */
    int s = (int)window_scale();
    cur_hide();
    cp = p;
    console_frame();
    con_live = 1; cp->scrolled = 0;
    unsigned nl = con_nlines(), keep = con_rows > 1 ? (unsigned)con_rows - 1 : 1;
    cp->anchor = con_line_off(nl > keep ? nl - keep : 0);
    con_wipe();
    for (unsigned k = cp->anchor; k < cp->len; k++) con_draw(k);
    con_summary(); con_hint(); ask_redraw();
    fb_flush(win_lx * s, win_ly * s, win_lw * s, win_lh * s);
    uart_puts(p == &term_p ? "terminal open\n" : "console open\n");
    cur_show();
}
static void console_open(void) { pane_open(&con_p); }
/* The Calculator (calc.c holds the sums and the keypad; this draws it). One window at a time: it takes the Console's
   place, so the Console's saved wallpaper is what closing it puts back, and the Console then comes back. The Apps
   tile or F2 opens it; a click on a key or typing works it; Esc or the red dot closes it. */
int calc_keys(int *cols);
const char *calc_key(int i);
int calc_press(const char *k);
int calc_type(int ch);
const char *calc_input(void);
const char *calc_output(void);
int calc_flags(void);   /* bit 0 scientific, bit 1 degrees, bit 2 memory held */
static int calc_live;
#define CALC_TOP 36      /* the display's top, under the title band */
#define CALC_DISP 46     /* the display's height */
static void calc_cell(int i, int *x, int *y, int *w, int *h) {   /* key i's box on the logical grid */
    int cols, n = calc_keys(&cols), rows = (n + cols - 1) / cols, gap = 4;
    int ax = win_lx + 12, ay = win_ly + CALC_TOP + CALC_DISP + 8, aw = win_lw - 24, ah = win_ly + win_lh - 10 - ay;
    *w = (aw - gap * (cols - 1)) / cols; *h = (ah - gap * (rows - 1)) / rows;
    *x = ax + (i % cols) * (*w + gap); *y = ay + (i / cols) * (*h + gap);
}
static void calc_paint(void) {
    int s = (int)window_scale(), x0 = win_lx + 12, w0 = win_lw - 24, y0 = win_ly + CALC_TOP, f = calc_flags();
    cur_hide();
    gui_draw_window_frame(win_lx, win_ly, win_lw, win_lh, "Calculator");
    window_rect(x0, y0, w0, CALC_DISP, 0x00DDD8CE);
    window_rect(x0 + 1, y0 + 1, w0 - 2, CALC_DISP - 2, 0x00FFFFFF);
    const char *in = calc_input(), *out = calc_output();
    while (*in && gui_text_width(in) > w0 - 70) in++;   /* keep the end of a long sum in view */
    gui_text(*in ? in : "Type or click a sum", x0 + 6, y0 + 3, *in ? 0x001C1C1E : 0x0075726E);
    gui_text(f & 1 ? (f & 2 ? "DEG" : "RAD") : "", x0 + w0 - 34, y0 + 3, 0x0075726E);
    if (f & 4) gui_text("M", x0 + w0 - 50, y0 + 3, 0x00b5502c);
    if (*out) gui_icon_text(out, x0 + w0 - 8 - gui_icon_text_w(out, 1, s), y0 + 20, 1, s, *out == 'E' ? 0x00b5502c : 0x001C1C1E);
    int cols, n = calc_keys(&cols);
    for (int i = 0; i < n; i++) {
        const char *k = calc_key(i);
        int x, y, w, h, eq = k[0] == '=', op = !k[1] && (k[0] == '+' || k[0] == '-' || k[0] == '*' || k[0] == '/');
        calc_cell(i, &x, &y, &w, &h);
        window_rect(x, y, w, h, eq ? 0x00b5502c : 0x00DDD8CE);
        if (!eq) window_rect(x + 1, y + 1, w - 2, h - 2, op ? 0x00EFEBE4 : 0x00FFFFFF);
        gui_text(k, x + (w - gui_text_width(k)) / 2, y + (h - 16) / 2, eq ? 0x00FFFFFF : 0x001C1C1E);
    }
    fb_flush(win_lx * s, win_ly * s, win_lw * s, win_lh * s);
    cur_show();
}
static void calc_open(void) {
    if (calc_live || !con_under) return;   /* no saved wallpaper (a full heap): nothing to close it back to */
    console_close();
    calc_live = 1;
    calc_paint();
    uart_puts("calc open\n");
}
static void calc_close(void) {
    if (!calc_live) return;
    calc_live = 0;
    con_live = 1;   /* console_close puts the Console's saved wallpaper back over the same rectangle */
    console_close();
    pane_open(cp);   /* whichever of the Console and the Terminal was in front before */
}
static void calc_did(int ran) {   /* after a key: redraw, and log a finished sum on the UART (what arm64-calc-check.py reads) */
    calc_paint();
    if (ran) { con_quiet = 1; uart_puts("calc: "); uart_puts(calc_input()); uart_puts(" = "); uart_puts(calc_output()); uart_putc('\n'); }
}
static void pointer_moved(void) {   /* one report done: the arrow to its new place, the dock's label to the slot under it */
    if (!fb) return;
    int s = (int)window_scale(), slot = gui_dock_hit_test((int)mouse_x / s, (int)mouse_y / s);
    cur_wanted = 1;
    cur_hide();
    if (slot != hover_slot) { hover_slot = slot; dock_hover(slot); }
    cur_show();
}
static void pointer_click(void) {   /* the left button went down */
    if (!fb) return;
    int s = (int)window_scale(), lx = (int)mouse_x / s, ly = (int)mouse_y / s, slot = gui_dock_hit_test(lx, ly);
    int dx = lx - (win_lx + 24), dy = ly - (win_ly + 16);   /* the close button: gui_draw_window_frame's red dot, radius 7 */
    if (slot == 0) calc_open();   /* Apps: the Calculator, the one app on ARM so far */
    else if (calc_live && slot < 0) {
        if (dx * dx + dy * dy <= 8 * 8) { calc_close(); return; }
        int cols, n = calc_keys(&cols);
        for (int i = 0; i < n; i++) {
            int x, y, w, h; calc_cell(i, &x, &y, &w, &h);
            if (lx >= x && lx < x + w && ly >= y && ly < y + h) { calc_did(calc_press(calc_key(i))); return; }
        }
    } else if (slot == 6) {   /* dock_names[6], Terminal: the command line */
        calc_close();
        pane_open(&term_p);
    } else if (slot >= 0) {
        calc_close();
        console_open();
        uart_puts("dock "); uart_puts(dock_names[slot]); uart_puts(": not on ARM yet\n");
    } else if (con_live && dx * dx + dy * dy <= 8 * 8) console_close();
}

unsigned long net_clock_utc(void);   /* ip.c: UTC seconds once the network has told us, 0 before */
static unsigned *mb_save; static int mb_h, mb_x0;
static int mb_wifi = -1;   /* the Wi-Fi state last shown (0 off, 1 working, 2 connected), so the clock can redraw alone each minute */
static int clock_minute = -1;
/* Pacific time (Vancouver) from UTC: 8 hours back, 7 in daylight time, which runs from the second Sunday of March to the
   first Sunday of November (the change itself is at 2 am local, 10:00 and 09:00 UTC). */
static unsigned long civil_days(long y, unsigned m, unsigned d) {
    y -= m <= 2; long era = (y >= 0 ? y : y - 399) / 400; unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (unsigned long)(era * 146097 + (long)doe - 719468);
}
static void civil_from_days(unsigned long z, long *y, unsigned *m, unsigned *d) {
    z += 719468; unsigned long era = z / 146097; unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; long yy = (long)yoe + (long)era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1; *m = mp < 10 ? mp + 3 : mp - 9; *y = yy + (*m <= 2);
}
static unsigned long nth_sunday_utc(long y, unsigned mon, unsigned nth, unsigned hour) {   /* the nth Sunday of a month, at hour UTC */
    unsigned long d1 = civil_days(y, mon, 1); unsigned dow = (unsigned)((d1 + 4) % 7);   /* 1970-01-01 was a Thursday; 0 = Sunday */
    unsigned first = dow == 0 ? 1 : 1 + (7 - dow);
    return (civil_days(y, mon, first + (nth - 1) * 7)) * 86400UL + hour * 3600UL;
}
static void clock_local(unsigned long utc, unsigned *hh, unsigned *mm, unsigned *mon, unsigned *day) {
    long y; unsigned m, d; civil_from_days(utc / 86400, &y, &m, &d);
    int dst = utc >= nth_sunday_utc(y, 3, 2, 10) && utc < nth_sunday_utc(y, 11, 1, 9);
    unsigned long loc = utc - (dst ? 7 : 8) * 3600UL;
    civil_from_days(loc / 86400, &y, mon, day);
    *hh = (unsigned)(loc % 86400) / 3600; *mm = (unsigned)(loc % 3600) / 60;
}
int wifi_signal_level(void);   /* wifi.c: 1 to 3 from how loud our network was in the scan */
/* The right end of the menu bar: the clock in 12-hour form, and left of it a three-bar signal icon, no name. Connected
   bars show in the same ink as the clock, as many as the signal earns; anything else (starting, joining, no network) is three
   quiet grey bars. Each call puts the saved bare bar back first, so old pixels never show through. */
void menubar_wifi(int state) {
    if (!fb || !text_ok) return;
    int W = (int)fb_w, H = (int)fb_h, mb = sg(MENUBAR_H);
    if (mb_save) for (int y = 0; y < mb_h; y++) for (int x = mb_x0; x < W; x++) fb[(unsigned)y * fb_pitch + (unsigned)x] = mb_save[y * (W - mb_x0) + (x - mb_x0)];
    mb_wifi = state;
    char clkbuf[12] = "--:--"; const char *clk = clkbuf;   /* the clock slot: dashes until the network has told us the time */
    unsigned long u = net_clock_utc();
    if (u) {
        unsigned h, m, mo, dd; clock_local(u, &h, &m, &mo, &dd); unsigned h12 = h % 12 ? h % 12 : 12; unsigned n = 0;
        if (h12 >= 10) clkbuf[n++] = '1';
        clkbuf[n++] = (char)('0' + h12 % 10); clkbuf[n++] = ':'; clkbuf[n++] = (char)('0' + m / 10); clkbuf[n++] = (char)('0' + m % 10);
        clkbuf[n++] = ' '; clkbuf[n++] = h >= 12 ? 'P' : 'A'; clkbuf[n++] = 'M'; clkbuf[n] = 0;
        clock_minute = (int)(h * 60 + m);
    }
    int cx = W - sg(16) - text_width(2, clk, sg(120));
    text_draw(2, clk, cx, (mb + sg(8)) / 2, sg(120), fb_color(0x001C1C1E), fb, fb_pitch, W, H);
    int R = sg(11), th = sg(3) / 2, dr = sg(3) / 2, ox = cx - sg(16) - R, oy = mb / 2 + sg(5);   /* the classic fan: a dot, then three arcs above it */
    int level = state == 2 ? wifi_signal_level() : 0;
    for (int y = -R; y <= dr; y++) for (int x = -R; x <= R; x++) {
        int d2 = x * x + y * y, ring = 0;
        if (d2 <= dr * dr) ring = 1;   /* the dot */
        else if (y < 0 && x <= -y && -x <= -y) for (int i = 1; i <= 3; i++) { int ro = sg(3 * i + 2), ri = ro - th; if (d2 <= ro * ro && d2 >= ri * ri) ring = i + 1; }   /* 45 degrees either side of straight up */
        if (ring) fb_rect(ox + x, oy + y, 1, 1, ring - 1 <= level && (ring > 1 || level) ? 0x001C1C1E : 0x00B8B4AC);
    }
    fb_flush(mb_x0, 0, W - mb_x0, mb);
}
void menubar_tick(void) {   /* the poll loop calls this; it redraws only when the minute has changed */
    unsigned long u = net_clock_utc();
    if (!u || mb_wifi < 0) return;
    unsigned h, m, mo, dd; clock_local(u, &h, &m, &mo, &dd);
    if ((int)(h * 60 + m) != clock_minute) menubar_wifi(mb_wifi);
}
static void fb_init(void) {
    if (!fb_setup()) return;
    int W = (int)fb_w, H = (int)fb_h;
    mouse_x = fb_w / 2; mouse_y = fb_h / 2;               /* the pointer starts in the middle of the screen */
    int win_w = sc(500), win_h = sc(350), win_x = (W - win_w) / 2, win_y = sc(100);
    int mb = sg(MENUBAR_H);                               /* the menu bar's height on this screen: 26 on the 960x540 grid */
    {   /* the boot screen while the photo decodes (the slow part): the tree on off-white and a thin bar a third full,
           one of three steps done (screen up; the photo and the desktop follow). The wallpaper paints straight over it. */
        fb_rect(0, 0, W, H, 0x00faf8f4);
        menu_mark_paint(W / 2, H / 2 - sg(30), sg(110), 0x001C1C1E);
        int bw = sg(140), bh = sg(4) > 2 ? sg(4) : 2, bx = (W - bw) / 2, by = H / 2 + sg(50);
        fb_rect(bx, by, bw, bh, 0x00DDD8CE);
        fb_rect(bx, by, bw / 3, bh, 0x001C1C1E);
        fb_flush(0, 0, W, H);
    }
    int wall_ok = wall_paint(fb, fb_pitch, W, H, fb_swap);
    if (!wall_ok) fb_rect(0, 0, W, H, 0x00203040);   /* the Satellite photo; flat only if it will not decode */
    for (int y = 0; y < mb - 1; y++) for (int x = 0; x < W; x++) {   /* menu bar: half wallpaper, half white, per pixel */
        unsigned c = fb[(unsigned)y * fb_pitch + (unsigned)x];
        fb[(unsigned)y * fb_pitch + (unsigned)x] = ((c >> 1) & 0x007F7F7Fu) + 0x00808080u;   /* each colour lane: half itself plus half of 255 */
    }
    fb_rect(0, mb - 1, W, 1, MENUBAR_RULE);               /* closed by a one pixel rule */
    mb_h = mb - 1; mb_x0 = W / 2;                          /* keep the bare right half of the bar, for menubar_status() */
    mb_save = kmalloc((unsigned)(mb_h * (W - mb_x0)) * 4);
    if (mb_save) for (int y = 0; y < mb_h; y++) for (int x = mb_x0; x < W; x++) mb_save[y * (W - mb_x0) + (x - mb_x0)] = fb[(unsigned)y * fb_pitch + (unsigned)x];
    text_ok = text_init();                                /* before the dock (the Calendar face) and the window (its title) */
    dock_paint();                                         /* the i386 dock, while only the wallpaper is under it */
    int band_y = gui_dock_band_top() * (int)window_scale();   /* the top of the dock's band, room for a hover label */
    /* Slice 3: the Console wears the i386 window frame (gui_paint.c): rounded cream body on the wallpaper, traffic
       lights, centred name, a hairline under the title band. Its content well is white for the log. */
    int s = (int)window_scale(), lx = win_x / s, ly = win_y / s, lw = win_w / s, lh = win_h / s;
    win_lx = lx; win_ly = ly; win_lw = lw; win_lh = lh;
    con_under = kmalloc((unsigned)(lw * s * lh * s) * 4);   /* slice 4: what the close button puts back */
    if (con_under) { for (int j = 0; j < lh * s; j++) for (int i = 0; i < lw * s; i++) con_under[j * lw * s + i] = fb[(unsigned)(ly * s + j) * fb_pitch + (unsigned)(lx * s + i)]; }
    else uart_puts("oom console close\n");                  /* the Console then just has no working close button */
    console_frame();
    if (text_ok) {
        menu_mark_paint(sg(24), mb / 2, sg(20), 0x001C1C1E);   /* the mark in the corner, the i386 menu bar's */
        text_draw(1, "Joshua Tree", sg(40), (mb + sg(8)) / 2, sg(130), fb_color(0x001C1C1E), fb, fb_pitch, W, H);   /* menu bar title: bold sans, 13 on the grid, ink */
        menubar_wifi(1);   /* replaces the ARM64 badge: wifi.c moves it along as the chip comes up */
        /* Steve Jobs died on 5 October 2011. Fifteen years on, one quiet line above the dock, ending on the title
           of the Steve Jobs Archive's book of his own words, which Joshua was reading that week. */
        const char *thanks = "Steve Jobs, 1955 to 2011. Make something wonderful.";
        int ar = sc(5), gapx = sc(6), tw = text_width(2, thanks, sc(110)), tx = (W - tw + 2 * ar + gapx) / 2, ty = band_y - sc(4), sh = sc(1) > 1 ? sc(1) : 1;
        {   /* a small apple to the left of the line: a round fruit with a dip on top and a leaf, no bite, so it is a fruit and not a logo */
            int acx = tx - gapx - ar, acy = ty - sc(4);
            for (int pass = 0; pass < 2; pass++) for (int y = -2 * ar; y <= ar; y++) for (int x = -ar - 1; x <= ar + 1; x++) {
                int body = x * x + y * y <= ar * ar && !(x * x + (y + ar) * (y + ar) <= (ar / 3) * (ar / 3));
                int lx2 = x - ar / 3, ly2 = y + ar + ar / 2, leaf = 4 * lx2 * lx2 + 9 * ly2 * ly2 - 4 * lx2 * ly2 <= ar * ar && y < -ar + 1 && x > 0;
                if (body || leaf) fb_rect(acx + x + (pass ? 0 : sh), acy + y + (pass ? 0 : sh), 1, 1, pass ? 0x00f0f4f8 : 0x00101010);
            }
        }
        text_draw(2, thanks, tx + sh, ty + sh, sc(110), fb_color(0x00101010), fb, fb_pitch, W, H);   /* a dark shadow so it reads on the busy photo */
        text_draw(2, thanks, tx, ty, sc(110), fb_color(0x00f0f4f8), fb, fb_pitch, W, H);
    } else uart_puts(heap_oom ? "oom text\n" : "M1d text FAIL\n");
    con_layout(win_x, win_y, win_w, win_h);
    {   /* keep the log inside the frame's white well: under the title band's hairline, clear of the cream margins */
        int wx = (lx + 8) * s, wy = (ly + 30) * s + 2 * s, wr = (lx + lw - 8) * s, wb = (ly + lh - 8) * s;
        if (con_x < wx) con_x = wx;
        if (con_y < wy) con_y = wy;
        if (con_x + con_cols * con_cw > wr) con_cols = (wr - con_x) / con_cw;
        if (con_y + (con_rows + 1) * con_ch > wb) con_rows = (wb - con_y) / con_ch - 1;
    }
    con_start();
#ifdef DOCK_HOVER_TEST   /* the dockhover test build: the label over one slot, then (arm64-m1c-check.py) its pixels */
    dock_hover(DOCK_HOVER_TEST);
#endif
#ifdef CURSOR_TEST   /* the cursortest build: the pointer over dock slot 3 at boot, the 1080p (scale 2) arrow for arm64-mouse-check.py */
    mouse_x = (unsigned)((gui_slot_x(3) + DOCK_ICON / 2) * s); mouse_y = (unsigned)((gui_dock_y0() + DOCK_PAD + DOCK_ICON / 2) * s);
    pointer_moved();
#endif
    dcache_clean(fb, (unsigned long)fb_pitch * fb_h * 4);   /* the whole still picture out to RAM; con_glyph cleans as it goes from here */
    /* Blank spots, clear of any text: the bottom of the window's white well, the menu bar's rule, and the wallpaper (not
       the old flat fill, and not one colour everywhere). Grey and white read the same either byte order; the rule is
       checked in fb_color order. */
    int ok = fb[(unsigned)((ly + lh - 8) * s - 2) * fb_pitch + fb_w / 2] == 0x00ffffff && fb[(unsigned)(mb - 1) * fb_pitch + fb_w / 2] == fb_color(MENUBAR_RULE);
    unsigned w0 = fb[(unsigned)sc(300) * fb_pitch + 50], w1 = fb[(unsigned)(H - sc(8)) * fb_pitch + fb_w - 50], w2 = fb[(unsigned)(mb + sc(40)) * fb_pitch + fb_w - 50];
    ok = ok && (!wall_ok || (w0 != 0x00203040 && (w0 != w1 || w1 != w2)));   /* a full heap leaves the flat fill, which is the fallback working */
    uart_puts(ok ? "M1c fb ok\n" : "M1c fb FAIL\n");
#ifdef CALC_TEST   /* the calctest build: the Calculator open at boot, four sums typed into it (arm64-calc-check.py) */
    calc_open();
    static const char *const sums[] = {"sin(30deg)", "5!", "2^10", "1/0"};
    for (unsigned i = 0; i < 4; i++) { calc_press("C"); for (const char *p = sums[i]; *p; p++) calc_type(*p); calc_did(calc_type('\n')); }
    calc_press("Sci"); calc_press("M+"); calc_did(0);   /* end on the scientific pad, memory held, for the screendump */
#endif
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
#ifdef FP_TEST   /* fptest.c: hold known FP registers across the timer interrupts, whose handler wipes them */
    unsigned long bad = fp_spin(&ticks, 3);
    uart_puts(bad ? "FPTEST FAIL, registers changed: " : "FPTEST ok\n");
    if (bad) { uart_hex(bad); uart_putc('\n'); }
#else
    while (ticks < 3) __asm__ volatile ("wfi");
#endif
    uart_puts("M1a ok\n");
    fb_init();
#ifdef CRASH_TEST   /* arm64-crash-check.py: an unexpected EL1 fault right after the screen is up */
    uart_puts("CRASHTEST: storing to unmapped memory\n");
    *(volatile unsigned long *)0x300000000UL = 1;
#endif
}

/* ---- Input, from any driver: Linux evdev events (virtio input speaks them natively; the USB HID driver in xhci.c
   translates its reports into them). The pointer starts mid-screen; a tablet sets it, a mouse moves it. ---- */
struct input_event { unsigned short type, code; unsigned value; };
int ask_key(unsigned code, unsigned value);   /* ask.c: the ask> line editor */
int ask_char(unsigned code);                  /* ask.c: a key code as the character it types, Shift included */
void ask_poll(void);
static void input_event(struct input_event e) {
    if (e.type == 1 && e.code >= 272 && e.code <= 274) {                                                   /* BTN_LEFT, RIGHT, MIDDLE */
        con_quiet = 1;   /* the UART only */
        uart_puts("key "); uart_dec(e.code); uart_puts(e.value ? " down\n" : " up\n");
        if (e.code == 272 && e.value) pointer_click();
        return;
    }
    if (e.type == 1 && e.value && (e.code == 60 || (calc_live && e.code != 42 && e.code != 54))) {        /* F2, or a key for the open Calculator (Shift still goes to ask.c) */
        con_quiet = 1;
        uart_puts("key "); uart_dec(e.code); uart_puts(" down\n");
        if (e.code == 60) { if (calc_live) calc_close(); else calc_open(); return; }
        if (e.code == 1) { calc_close(); return; }                                                          /* Esc */
        int ch = e.code == 15 ? '\t' : e.code == 14 ? '\b' : e.code == 28 || e.code == 96 ? '\n' : ask_char(e.code);
        if (ch) calc_did(calc_type(ch));
        return;
    }
    if (e.type == 1 && calc_live && e.code != 42 && e.code != 54) return;                                  /* its key ups */
    if (e.type == 1 && e.code == 59) {                                                                     /* F1: Terminal and Console swap */
        if (e.value) { if (term_front()) console_open(); else pane_open(&term_p); }   /* the open Calculator keeps F1 (above) */
        return;
    }
    if (e.type == 1) {                                                                                     /* EV_KEY: keys */
        int held = cur_hold(), term = term_front();
        if (!con_key(e.code, e.value)) {   /* the scroll keys are not logged: that would add lines to the picture they move */
            /* Only the Terminal takes typing; Shift always reaches ask.c, as the Calculator reads its state there */
            if (((term || e.code == 42 || e.code == 54) && ask_key(e.code, e.value)) || term) con_quiet = 1;   /* typed keys show on the ask> row; the echo stays on the UART */
            uart_puts("key "); uart_dec(e.code); uart_puts(e.value ? " down\n" : " up\n");
        }
        cur_release(held);
    }
    else if (e.type == 3) {                                                                                /* EV_ABS: the tablet, 0..32767 */
        if (e.code == 0) mouse_x = e.value * fb_w / 32768; else if (e.code == 1) mouse_y = e.value * fb_h / 32768;
        mouse_moved = 1;
    } else if (e.type == 2) {                                                                              /* EV_REL: a mouse, clamped to the screen */
        int v = (int)e.value;
        if (e.code == 0) { int x = (int)mouse_x + v; mouse_x = x < 0 ? 0 : x >= (int)fb_w ? (int)fb_w - 1 : (unsigned)x; }
        else if (e.code == 1) { int y = (int)mouse_y + v; mouse_y = y < 0 ? 0 : y >= (int)fb_h ? (int)fb_h - 1 : (unsigned)y; }
        mouse_moved = 1;
    } else if (e.type == 0 && mouse_moved) {                                                               /* EV_SYN: one report done */
        con_quiet = 1;   /* the UART only: every report would scroll the Console */
        uart_puts("mouse "); uart_dec(mouse_x); uart_putc(','); uart_dec(mouse_y); uart_putc('\n');
        mouse_moved = 0;
        pointer_moved();
    }
}
void kinput(unsigned type, unsigned code, int value) { input_event((struct input_event){ (unsigned short)type, (unsigned short)code, (unsigned)value }); }
int usb_init(void);    /* xhci.c */
int wifi_init(void);
#ifdef PI_BUILD
#define WIFI_ON_PI 1
#else
#define WIFI_ON_PI 0
#endif   /* wifi.c: M4 Wi-Fi stage 1, polled, every wait bounded */
void usb_poll(void);
/* The boot demo: the Pi has no mouse yet, so one lap of the dock labels plays by itself, Apps to Trash, a beat each,
   and then stops. Joshua's request for the first video of it booting (2026-10-07). */
#ifdef PI_BUILD
static void demo_tick(void) {
    static unsigned next; static int slot = -1;
    if (slot > GUI_ICON_COUNT) return;
    unsigned long f, c;
    __asm__ volatile ("mrs %0, cntfrq_el0\n mrs %1, cntpct_el0" : "=r"(f), "=r"(c));
    unsigned t = (unsigned)(c / ((f ? f : 54000000) / 100));   /* 100 a second, like ip.c's ticks() */
    if (!next) next = t + 300;   /* three seconds after the loop starts: the console has settled */
    if (t < next) return;
    next = t + 80;
    if (slot < 0) slot = 0;
    hover_slot = slot < GUI_ICON_COUNT ? slot : -1;
    dock_hover(hover_slot);
    slot++;
}
#endif

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
    if (!v->d || !v->a || !v->u) { uart_puts("oom vq\n"); return 0; }
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
        if (!ev) { uart_puts("oom input\n"); continue; }
        if (!vio_start(b, 0) || !vq_setup(v, b, 0)) continue;
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
static int net_up;
static const unsigned char my_ip[4] = { 10, 0, 2, 15 }, gw_ip[4] = { 10, 0, 2, 2 };
static void uart_mac(const unsigned char *m) {
    for (int i = 0; i < 6; i++) { if (i) uart_putc(':'); uart_putc("0123456789abcdef"[m[i] >> 4]); uart_putc("0123456789abcdef"[m[i] & 15]); }
}
static int net_init(void) {
    unsigned long b = vio_find(1, 0);
    if (!b || !vio_start(b, 1u << 5 /* VIRTIO_NET_F_MAC */)) return 0;
    if (!vq_setup(&net_rx, b, 0) || !vq_setup(&net_tx, b, 1)) return 0;
    net_rxbuf = kmalloc(NET_BUF * VQ); net_txbuf = kmalloc(NET_BUF);
    if (!net_rxbuf || !net_txbuf) { uart_puts("oom net\n"); return 0; }
    for (int i = 0; i < 6; i++) net_mac[i] = *(volatile unsigned char *)(b + 0x100 + i);   /* config space: mac first */
    for (unsigned i = 0; i < VQ; i++) vq_give(&net_rx, i, net_rxbuf + i * NET_BUF, NET_BUF, 1);
    VR(b, 0x70) = 1 | 2 | 8 | 4;                    /* driver ok */
    vq_kick(&net_rx);
    uart_puts("M2 net mac "); uart_mac(net_mac); uart_putc('\n');
    net_up = 1;
    return 1;
}
/* The card under the shared IP stack (drivers/nic.h; drivers/net.c sits on top). One send buffer, so a send waits
   until the device has handed back every frame it was lent, the ARP probe's included, before reusing it. */
int nic_init(void) { return net_up; }
void nic_mac(unsigned char mac[6]) { for (int i = 0; i < 6; i++) mac[i] = net_mac[i]; }
int nic_send(const void *frame, unsigned int len) {
    if (!net_up || len > NET_BUF - 12) return 0;
    for (int i = 0; i < 12; i++) net_txbuf[i] = 0;
    for (unsigned i = 0; i < len; i++) net_txbuf[12 + i] = ((const unsigned char *)frame)[i];
    vq_give(&net_tx, 0, net_txbuf, 12 + len, 0);
    vq_kick(&net_tx);
    unsigned long freq, t0, t; __asm__ volatile ("mrs %0, cntfrq_el0\n mrs %1, cntpct_el0" : "=r"(freq), "=r"(t0));
    while (net_tx.u->idx != net_tx.a->idx) {
        __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(t));
        if (t - t0 > freq) return 0;
    }
    net_tx.seen = net_tx.u->idx;
    return 1;
}
unsigned int nic_recv(void *buf, unsigned int max) {
    unsigned len; int id;
    if (!net_up || (id = vq_take(&net_rx, &len)) < 0) return 0;
    unsigned char *r = net_rxbuf + (unsigned)id * NET_BUF;
    if (len > NET_BUF) len = NET_BUF;
    len = len > 12 ? len - 12 : 0;
    if (len > max) len = max;
    for (unsigned i = 0; i < len; i++) ((unsigned char *)buf)[i] = r[12 + i];
    vq_give(&net_rx, (unsigned)id, r, NET_BUF, 1);
    vq_kick(&net_rx);
    return len;
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
/* The Pi's network card is its Wi-Fi chip (wifi.c): up once the handshake has installed the keys. */
int wifi_nic_up(void); void wifi_nic_mac(unsigned char mac[6]); int wifi_nic_send(const void *f, unsigned len); unsigned wifi_nic_recv(void *b, unsigned max);
int nic_init(void) { return wifi_nic_up(); }
void nic_mac(unsigned char mac[6]) { wifi_nic_mac(mac); }
int nic_send(const void *frame, unsigned int len) { return wifi_nic_send(frame, len); }
unsigned int nic_recv(void *buf, unsigned int max) { return wifi_nic_recv(buf, max); }
static int blk_init(void) { return 0; }
static void blk_probe(void) {}
#endif

void net_stack_demo(void);
void net_clock_sync(void);
void tls_demo(void);
void main(void) {
    unsigned long el;
    uart_init();
    __asm__ volatile ("mrs %0, CurrentEL" : "=r"(el));
    uart_puts("Joshua Tree on ARM64\n");
    { const char *sp = "ABCDEFGHIJKLMNOPQRSTUVWXYZ\nabcdefghijklmnopqrstuvwxyz\n0123456789 .,;:!?&@#$%\n"; while (*sp) console_putc(*sp++); }   /* the type specimen: screen only, at the top of the log */
    uart_puts("booted at EL"); uart_putc((char)('0' + boot_el)); uart_puts("\n");
    uart_puts(((el >> 2) & 3) == 1 ? "EL1\n" : "not EL1\n");
    uart_puts("M0 ok\n");
    m1_selftest();
    user_demo();
    if (net_init()) net_arp_probe();
    if (blk_init()) blk_probe();
    net_stack_demo();   /* ip.c: DHCP and an HTTP POST through the shared stack, silent with no card */
    tls_demo();         /* ip.c: the HTTPS proof, only in a TLSPORT= build */
    fb_diag();   /* last, so it is the newest line on the screen */
    int inputs = input_init();
    if (inputs) { uart_puts("M2 input ready, devices "); uart_dec((unsigned)inputs); uart_putc('\n'); }
    int usb_ok = usb_init();   /* USB first: the keyboard is the way in, and Wi-Fi bring-up is a blocking stretch of seconds on the real Pi */
    if (!wifi_init()) menubar_wifi(0);
#ifdef PI_BUILD
    if (wifi_nic_up()) { net_stack_demo(); net_clock_sync(); menubar_wifi(2); }   /* the address from the router, then the time */
#endif   /* prints `wifi ...` lines; on QEMU it ends at `wifi no host` and the desktop carries on */
    if (usb_ok) {
        /* USB is polled, so nothing interrupts on its own: the virtual timer (INTID 27) wakes wfi every 2 ms. IRQs stay
           masked around wfi (a pending one still wakes it) and the timer is stopped before they are let through again,
           so its handler never runs; the short unmask is for the virtio devices, whose handler acknowledges them. */
        *(volatile unsigned char *)(GICD_BASE + 0x400 + 27) = 0x80;
        GICD(0x100) = 1u << 27;
        unsigned long step = timer_step / 25;   /* timer_step is 50 ms */
#ifdef PI_BUILD
        (void)step;
        for (;;) { usb_poll(); input_poll(); ask_poll(); demo_tick(); menubar_tick(); }   /* nothing on the Pi sleeps: no wake source to trust yet, so spin and poll */
#else
        for (;;) {
            __asm__ volatile ("msr daifset, #2\n msr cntv_tval_el0, %0\n msr cntv_ctl_el0, %1\n isb\n wfi\n"
                              " msr cntv_ctl_el0, xzr\n isb\n msr daifclr, #2\n isb" :: "r"(step), "r"(1UL) : "memory");
            usb_poll(); input_poll(); ask_poll();
        }
#endif
    }
    if (inputs) for (;;) { __asm__ volatile ("wfi"); input_poll(); ask_poll(); }   /* asleep until a device interrupts */
    for (;;) __asm__ volatile ("wfe");
}
