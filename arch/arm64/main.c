/* M0: serial hello. QEMU's virt machine has a PL011 UART at 0x09000000. A Raspberry Pi 4 has one at
   0xFE201000 (make PI=1 sets UART_BASE and PI_BUILD) and needs its two GPIO pins and its baud rate
   set first; QEMU's virt machine comes up ready. Since 2.51.0 the Pi image (CONSOLE_MINI_UART, set by the Makefile)
   prints on the mini UART instead, on the same header pins: the PL011 belongs to the Bluetooth radio (bt.c). */
#ifdef PI_BUILD
#define UART_BASE 0xFE201000UL
#define GPIO_BASE 0xFE200000UL
#define AUX_ENABLES 0xFE215004UL   /* the mini UART (UART1): bit 0 of AUX_ENABLES turns it on */
#define MU_IO       0xFE215040UL
#define MU_IER      0xFE215044UL
#define MU_LCR      0xFE21504CUL
#define MU_MCR      0xFE215050UL
#define MU_LSR      0xFE215054UL
#define MU_CNTL     0xFE215060UL
#define MU_BAUD     0xFE215068UL
#define GICD_BASE 0xFF841000UL   /* the Pi 4's GIC-400, the same GICv2 programming model as QEMU's virt */
#define GICC_BASE 0xFF842000UL
#else
#define UART_BASE 0x09000000UL
#define GICD_BASE 0x08000000UL
#define GICC_BASE 0x08010000UL
#endif
#define REG(a) (*(volatile unsigned int *)(a))
#include "bt.h"
#include "apple_mask.h"
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

/* The console only ever sends. Every wait for room is bounded: a UART that never drains is given up on for good, so a
   wedged one can never freeze the screen's console, which is the only channel on a Pi with no serial cable. */
#if defined(PI_BUILD) && defined(CONSOLE_MINI_UART)
#define CON_TX_FULL() (!(REG(MU_LSR) & 0x20))   /* LSR bit 5: the transmit FIFO can take a byte */
#define CON_TX(c) (REG(MU_IO) = (unsigned char)(c))
#else
#define CON_TX_FULL() (REG(UART_FR) & TXFF)
#define CON_TX(c) (REG(UART_DR) = (unsigned char)(c))
#endif
static int con_tx_dead;
static void con_tx(char c) {
    if (con_tx_dead) return;
    for (unsigned n = 0; CON_TX_FULL(); n++) if (n > 2000000) { con_tx_dead = 1; return; }
    CON_TX(c);
}
static void uart_init(void) {
#if defined(PI_BUILD) && defined(CONSOLE_MINI_UART)
    /* With Bluetooth on, the firmware's primary UART is the mini UART, and enable_uart=1 holds the core clock it counts
       from steady (250 MHz on a Pi 4: the Raspberry Pi UART documentation). The firmware has already printed its own
       lines through it at 115200, so its divisor is kept; 270 (250 MHz / (8 * 115200) - 1) only if it left none.
       GPIO14 (TX) and GPIO15 (RX) to ALT5, pulls off. Nothing here touches the PL011 or GPIO 30-33 (bt.c's). */
    REG(AUX_ENABLES) |= 1;
    REG(MU_IER) = 0;
    REG(MU_LCR) = 3;       /* 8 bits */
    REG(MU_MCR) = 0;
    if (!REG(MU_BAUD)) REG(MU_BAUD) = 270;
    unsigned int f = REG(GPIO_BASE + 0x04);
    f &= ~((7u << 12) | (7u << 15)); f |= (2u << 12) | (2u << 15);
    REG(GPIO_BASE + 0x04) = f;
    REG(GPIO_BASE + 0xE4) &= ~((3u << 28) | (3u << 30));
    REG(MU_CNTL) = 3;      /* transmitter and receiver on */
#elif defined(PI_BUILD)
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
#if defined(PI_BUILD) && !defined(QUIET_BOOT)
#define QUIET_BOOT   /* the desktop comes up bare: the boot log is kept but its window is not drawn (a dock click or the Terminal's Esc brings it up) */
#endif
#ifdef QUIET_BOOT
static int splash_on, splash_bad;   /* the boot splash is up; a FAIL or oom line is being printed under it */
static void splash_abort(void);
#endif
static void uart_putc(char c) {
    con_tx(c);
    if (!con_quiet) console_putc(c);
    if (c == '\n') con_quiet = 0;
    con_line_start = c == '\n';
#ifdef QUIET_BOOT
    if (c == '\n' && splash_bad) { splash_bad = 0; if (splash_on) splash_abort(); }   /* a failure ends the splash once its line is whole */
#endif
}
/* The boot narrative is long, and on the screen it pushed the lines that matter out of the window. These line starts go to
   the UART only (the serial log is unchanged): the self-test chatter, the USB enumeration walk and the Wi-Fi steps that went
   fine. Every FAIL, every odd status and the summary lines still reach the screen. */
static int con_noise(const char *s) {
#ifdef CON_VERBOSE
    (void)s; return 0;   /* the scroll check builds with every line on the screen so there is a long log to scroll */
#else
    static const char *const skip[] = { "tick", "M0 ", "M1 ", "M1a ", "M1b ", "M1c fb ok", "splash ", "M1d dock ", "M3 ", "M3b ", "M3c ", "EL0", "EL1", "booted at ",
        "usb ", "wifi power", "wifi sdio", "wifi f1", "wifi alp", "wifi chip", "wifi cores", "wifi arm", "wifi fw ", "wifi ht ",
        "wifi bus", "wifi radio up", "wifi ver", "wifi mac", "wifi found", "wifi scan", "wifi joining", "wifi handshake", "wifi assoc",
        "Wi-Fi: found", "Wi-Fi: looked", "Wi-Fi: this Pi", "Wi-Fi: chip", "Wi-Fi: connecting", "Wi-Fi: the router", "Wi-Fi: we answered", "Wi-Fi: handshake",
        "dhcp: lease", "net dhcp", "@", "Wi-Fi: connected", "Internet: online", "bt: no radio found" };
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
#ifdef QUIET_BOOT
    if (splash_on) {   /* the splash never hides a failure: any FAIL or oom line brings the desktop up */
        if (s[0] == 'o' && s[1] == 'o' && s[2] == 'm' && s[3] == ' ') splash_bad = 1;
        for (const char *p = s; *p; p++) if (p[0] == 'F' && p[1] == 'A' && p[2] == 'I' && p[3] == 'L') splash_bad = 1;
    }
#endif
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
static volatile int wd_live;           /* 4.5: an app is inside resume_user with the watchdog armed */
static void wd_kill(void) __attribute__((noreturn));

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
    if (id == TIMER_INTID && wd_live) {   /* 4.5: the watchdog, not the boot tick: the timer is the app's three-second budget */
        unsigned long spsr; __asm__ volatile ("mrs %0, spsr_el1" : "=r"(spsr));
        __asm__ volatile ("msr cntp_ctl_el0, %0" :: "r"(0UL));
        GICC(0x10) = iar;
        if ((spsr & 0xF) == 0) wd_kill();   /* it was in EL0: never returns, leave_user hands the core back to app_tick */
        return;
    }
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
#define ARENA_BLOCKS 8   /* M3e: the EL0 arena is 16 MiB, eight of those blocks... */
#define APP_SLOTS 4      /* 4.0: apps that can run at once, one 16 MiB arena each, back to back above USER_BASE */
static unsigned long l3[APP_SLOTS * ARENA_BLOCKS * 512] __attribute__((aligned(4096)));   /* ...each one a table of 4 KiB pages, back to back, so l3[page] works across all eight */
#ifdef PI_BUILD
#define RAM_GIB 0
#else
#define RAM_GIB 1
#endif
/* The arena: 16 MiB (M3e; one 2 MiB block before), clear of the kernel image and the heap. 64 MiB into RAM unless the heap
   reaches that far (an image with a language model baked in, llm_model.S), then the first 2 MiB boundary after the heap. The
   first block holds the M3a pages and the app's image, stack and window buffer; the other seven are the app's brk heap. */
#define ARENA_LOW (((unsigned long)RAM_GIB << 30) + (64UL << 20))
#define HEAP_END ((unsigned long)_heap_start + HEAP_SIZE)
/* 4.3.0: the screen's RAM back buffer (fb points at it; fb_flush copies to the real scanout) and a scratch screen of the same
   size where a moved window's damage is put together, FB_RAM_MAX each, in the 2 MiB blocks right after the heap. The arena
   moves up past them when they would reach it, as it does past a big heap. */
#define FB_RAM_MAX (16UL << 20)
#define FB_RAM_BASE ((HEAP_END + (2UL << 20) - 1) & ~((2UL << 20) - 1))
#define KERN_END (FB_RAM_BASE + 2 * FB_RAM_MAX)
#define USER_BASE (KERN_END <= ARENA_LOW ? ARENA_LOW : KERN_END)
#define UP_CODE  0x0000UL   /* EL0 code, read-only and executable */
#define UP_STACK 0x2000UL   /* EL0 stack, one page; sp starts at its top */
#define UP_KERN  0x4000UL   /* a page in the same arena that only EL1 may touch */
#define UP_APP    0x10000UL /* M3c: the app window, where user_load puts a program from the app table: code pages, data pages, zeroed pages */
#define UP_ASTACK 0x80000UL /* ...and the app's stack: 16 pages (M3e: minimp3 keeps about 16 KB of scratch there), nothing mapped below them */
#define APP_STACK_PAGES 16
#define APP_ZERO_PAGES  8   /* zeroed pages after the image (and the rest of its last page): the pool user/calendar.c keeps its events in, and user/notes.c its 28 KB arena (M3d) */
#define UAPP_MAX 0x40000UL  /* the largest image user_load takes, and what user_c.ld asserts */
#define UP_WIN   0xD00000UL /* M3c part 2: the app's window buffer, read-write and never executable. 3.9.0: the last 3 MiB of the arena, so a device-size
                               buffer fits (504x328 at 2x is 2.6 MB); it was a page above the stack, with room for 560x360 four times over */
#define UWIN_MAX (0x1000000UL - UP_WIN)   /* the arena's end, 16 MiB, less where the buffer starts */
#define UP_HEAP  0x200000UL /* M3e: the app's brk heap, from the second block up to where the window buffer starts (11 MiB since 3.9.0), EL0 read-write, never executable */
#define ARENA_SIZE ((unsigned long)ARENA_BLOCKS << 21)
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
static void apps_init(void);
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
    for (int i = 0; i < APP_SLOTS * ARENA_BLOCKS * 512; i++) l3[i] = 0;
    for (int k = 0; k < APP_SLOTS * ARENA_BLOCKS; k++) l2[((USER_BASE - ((unsigned long)RAM_GIB << 30)) >> 21) + k] = (unsigned long)(l3 + k * 512) | 3;
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
    apps_init();
    uart_puts("M1b mmu on\n");
    unsigned long *a = kmalloc(4096), *b = kmalloc(100), *c = kmalloc(8);
    if (!a || !b || !c || (unsigned long)a % 16 || (unsigned long)b % 16 || b == a || c <= b) { uart_puts("M1b heap FAIL\n"); return; }
    for (int i = 0; i < 512; i++) a[i] = 0xA5A5A5A500000000UL | (unsigned long)i;
    for (int i = 0; i < 512; i++) if (a[i] != (0xA5A5A5A500000000UL | (unsigned long)i)) { uart_puts("M1b heap FAIL\n"); return; }
    uart_puts("M1b heap ok\n");
}

/* ---- M3a: the first EL0 program, a syscall layer, and a page EL0 may not touch ----
   The arena is mapped 4 KiB pages inside the identity map. M3a: the test program's code (EL0 read and execute, nobody
   writes), its stack (EL0 read and write, never executable) and a kernel-only page (AP=00: EL1 only) holding a secret. The
   program is user.S, copied in. Syscalls use svc #0 with the number in x8 (Linux's: write 64, exit 93, and since M3b the
   Joshua Tree numbers of user/jtsys.h), arguments in x0 to x2 and the result in x0. A write only reads user memory that
   really is the program's own; anything else gets -EFAULT. A fault in EL0 kills the program and the kernel carries on.
   M3c: the same arena also holds the app window (UP_APP) and an app stack (UP_ASTACK). user_load puts a program from the app
   table there: code pages read-only and executable, then data pages and a zeroed pool read-write and never executable. */
extern char _start[];
extern char user_start[], user_end[], user_entry_a[], user_entry_b[], user_entry_c[], user_entry_d[], user_entry_e[], user_entry_f[];
extern unsigned long enter_user(unsigned long entry, unsigned long sp, unsigned long arg, unsigned long arg2);
extern void leave_user(unsigned long status) __attribute__((noreturn));
extern unsigned long resume_user(const struct frame *f, unsigned long sp, const unsigned long *fp);   /* vectors.S: back into a parked app (its FP registers too), returns like enter_user */
#define SYS_WRITE 64   /* the Linux numbers the user.S self-test uses */
#define SYS_EXIT 93
#define JT_SYS_EXIT 1    /* Joshua Tree's own, the ones user/jtsys.h and docs/ARCHITECTURE.md give every program (M3b) */
#define JT_SYS_READ 3    /* M3d: the file calls, answered by ufile.c */
#define JT_SYS_WRITE 4
#define JT_SYS_TIME 13    /* M3c */
#define JT_SYS_GETPID 20
#define JT_SYS_SCHED_YIELD 158   /* M3c part 2 */
#define JT_SYS_WINDOW_OPEN 384
#define JT_SYS_WINDOW_POLL 385
#define JT_SYS_AUDIO 393         /* M3e: play, status, stop (audio.c holds the queue) */
#define JT_SYS_TASKS 386         /* 3.19.0: uptime in 100 Hz ticks for hamurabi.c (the rest of struct jt_tasks stays 0), no kill */
#define JT_SYS_BRK 397           /* M3e: the heap top */
#define JT_SYS_TEXT2 402         /* 3.9.0: Tree Sans into a device-size window buffer (user/libjt/hires.h) */
#define JT_SYS_CLIPBOARD 399     /* 3.10.0: copy and paste, see clip_put below */
#define JT_CLIP_SET 1
#define JT_CLIP_GET 2
#define JT_CLIP_MAX 4096
struct user_app { const char *name, *start, *end; };   /* userapps.S: one row per program, ended by a zero name */
extern const struct user_app user_apps[];
static unsigned long user_status;
static int user_faulted;    /* the last EL0 program ended on a fault, not an exit */
#ifdef HANG_TEST
static int hang_arm;       /* app_start sets it for slot 0, so the boot self-tests' own loads are left alone */
#endif
static int user_loud;       /* userrun: what the program writes reaches the screen too; the boot self-test keeps it on the UART */
static unsigned user_pid;   /* programs started so far, what JT_SYS_GETPID answers */
static unsigned long app_len, app_code, app_img;   /* the loaded app: bytes of window mapped, how many of those are code, and the image's own length */
static unsigned long cal_local(void);   /* cal_utc shifted to Vancouver wall time, what SYS_TIME returns (below) */
static unsigned long cal_utc(void);   /* the clock the Calendar tile reads, further down: 0 until the network has told us the time */
/* M3c part 2, the scheduler's state: one app at a time, since the arena has one app window. app_cur is the app it holds,
   running or parked; app_frame and app_sp are where it parked (every register, its pc and its stack). Its window buffer is
   app_wbytes at UP_WIN, 0 with no window, and app_q is the ring of events the desktop queued for it. */
#define APP_EVQ 16
struct app {   /* 4.0: one of APP_SLOTS apps, each in its own arena; the old app_* names below are macros onto `ap`, the slot being run or shown */
    const struct user_app *cur;
    struct frame frame;
    unsigned long fp[66];                 /* M3e: its q0-q31, FPCR and FPSR where it parked (vectors.S's save_fp layout) */
    unsigned long sp, last, wbytes;
    int hi;                               /* 3.9.0: the window buffer is device size (scale 2, the app asked for it) */
    unsigned long brk;                    /* M3e: the heap top JT_SYS_BRK set, 0 while it has none */
    unsigned long close_at;               /* the red dot was clicked: past this count, an app that did not close itself is ended */
    int running, parked, kick;            /* inside resume_user now; it yielded rather than ended; run it on the next pass */
    unsigned runs;                        /* times the desktop's loop resumed it, said when it ends */
    struct { unsigned kind; int a, b; } q[APP_EVQ];
    unsigned qh, qt;
    unsigned long len, code, img;         /* the loaded app: bytes of window mapped, how many of those are code, and the image's own length */
    int lx, ly, lw, lh;                   /* the frame on the logical grid, set in fb_init */
    unsigned cw, ch;                      /* the content area, what the frame holds */
    unsigned bw, bh;                      /* the app's buffer, pixel for pixel: the content's size as of its last JT_SYS_WINDOW_OPEN */
    int rs_log;                           /* a resized buffer is waiting for the app's first frame at the new size */
    unsigned presents;                    /* frames it has handed to JT_SYS_WINDOW_POLL, for the windows gate */
    int shown;                            /* the app has presented a frame since its window opened */
    unsigned long base;                   /* its arena: USER_BASE + slot * ARENA_SIZE (apps are position independent, so each loads where its arena is) */
    unsigned l3off, slot;                 /* its first l3 entry; its index in apps */
};
static struct app apps[APP_SLOTS];
static struct app *ap = &apps[0];         /* the app whose state the code below means: the one resume_user is in, else the one in front */
#define app_cur (ap->cur)
#define app_frame (ap->frame)
#define app_fp (ap->fp)
#define app_sp (ap->sp)
#define app_last (ap->last)
#define app_wbytes (ap->wbytes)
#define app_hi (ap->hi)
#define app_brk (ap->brk)
#define app_close_at (ap->close_at)
#define app_running (ap->running)
#define app_parked (ap->parked)
#define app_kick (ap->kick)
#define app_runs (ap->runs)
#define app_q (ap->q)
#define app_qh (ap->qh)
#define app_qt (ap->qt)
#define app_len (ap->len)
#define app_code (ap->code)
#define app_img (ap->img)
#define app_lx (ap->lx)
#define app_ly (ap->ly)
#define app_lw (ap->lw)
#define app_lh (ap->lh)
#define app_cw (ap->cw)
#define app_ch (ap->ch)
#define app_bw (ap->bw)
#define app_bh (ap->bh)
#define app_rs_log (ap->rs_log)
#define app_shown (ap->shown)
#define UB (ap->base)
static unsigned long cnt_now(void) { unsigned long c; __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(c)); return c; }
static unsigned long cnt_hz(void) { unsigned long f; __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(f)); return f ? f : 54000000; }
/* 4.5: the watchdog. An app that runs three seconds at EL0 without a syscall is stopped: the EL1 physical timer is its budget,
   re-armed on every return to EL0 from a syscall (wd_rearm, called from vectors.S) so time spent inside the kernel, a slow SD
   read or an HTTP wait, is never charged to it. IRQs are unmasked at EL0 only for apps (resume_user); the kernel itself still
   runs with them masked. Counts as a fault: app_tick ends the app and the desktop carries on.
   ponytail: one fixed budget, 3 s; per-app budgets if an app ever needs a long pure-compute stretch. */
static void wd_arm(void) { unsigned long t = cnt_hz() * 3; __asm__ volatile ("msr cntp_tval_el0, %0\n msr cntp_ctl_el0, %1\n isb" :: "r"(t), "r"(1UL)); }
static void wd_off(void) { __asm__ volatile ("msr cntp_ctl_el0, %0\n isb" :: "r"(0UL)); }
void wd_rearm(void) { if (wd_live) wd_arm(); }
static void apps_init(void) { for (unsigned i = 0; i < APP_SLOTS; i++) { apps[i].base = USER_BASE + i * ARENA_SIZE; apps[i].l3off = i * ARENA_BLOCKS * 512; apps[i].slot = i; } }
/* The app windows' stacking: app_z lists the slots that have a window, front first. app_top is the front one (-1 with none); outside
   app_tick, `ap` always points at it, so the desktop's clicks, keys and drags land on the window in front. */
static int app_top = -1;
/* 4.4: an app reaches only its own arena. Every level-2 entry of the other slots carries APTable=01 (bit 61): EL0 may not
   touch anything under it, while EL1 (the compositor reading their window buffers, a syscall copying for them) still can.
   Set before each switch into EL0. -1 until the first switch: the boot self-tests run in slot 0 with nothing else loaded.
   ponytail: one tlbi vmalle1 per switch; ASIDs, one per slot, when four busy apps make that show up in a profile. */
static int el0_slot = -1;
static void arena_only(int slot) {
    if (slot == el0_slot) return;
    unsigned long *e = &l2[(USER_BASE - ((unsigned long)RAM_GIB << 30)) >> 21];
    for (int k = 0; k < APP_SLOTS * ARENA_BLOCKS; k++) e[k] = k / ARENA_BLOCKS == slot ? e[k] & ~(1UL << 61) : e[k] | (1UL << 61);
    __asm__ volatile ("dsb ishst\n tlbi vmalle1\n dsb ish\n isb" ::: "memory");
    el0_slot = slot;
}
static unsigned char app_z[APP_SLOTS];
static unsigned app_zn;
static void app_z_drop(unsigned sl) {
    unsigned o = 0;
    for (unsigned i = 0; i < app_zn; i++) if (app_z[i] != sl) app_z[o++] = app_z[i];
    app_zn = o;
}
static void app_z_front(unsigned sl) {
    app_z_drop(sl);
    for (unsigned i = app_zn; i > 0; i--) app_z[i] = app_z[i - 1];
    app_z[0] = (unsigned char)sl; app_zn++;
    app_top = (int)sl; ap = &apps[sl];
}
static int apps_live(void) { for (unsigned i = 0; i < APP_SLOTS; i++) if (apps[i].cur) return 1; return 0; }
static long app_window_open(unsigned long info, unsigned long flags);                    /* the window half, with the other windows further down */
static long app_window_poll(unsigned long ev, unsigned long flags);
static void app_window_close(void);
static long app_text(unsigned long req);   /* 3.9.0: JT_SYS_TEXT2 */
static long app_audio(unsigned long op, unsigned long arg, unsigned long size);   /* M3e: JT_SYS_AUDIO, with the app loader's byte readers */
static int span(unsigned long a, unsigned long n, unsigned long base, unsigned long len) {   /* [a, a+n) inside [base, base+len), written so a + n can never wrap */
    return n <= len && a >= base && a - base <= len - n;
}
long ufile_syscall(unsigned long n, unsigned long a0, unsigned long a1, unsigned long a2);   /* ufile.c, M3d: open, read, write (fd 3 and up), close, lseek, readdir, readfile, mkdir, unlink */
void ufile_release(void);                 /* ...every descriptor the app left open, closed and written back */
extern unsigned ufile_owner;              /* 4.0: whose descriptors the file calls mean: 0 the boot self-test, else app slot + 1 */
void ufile_mkdirs(const char *path);      /* ...folders inside JT/ an app expects */
static unsigned long heap_len(void) { return app_brk ? app_brk - (UB + UP_HEAP) : 0; }   /* M3e: the heap below its top */
int user_in_range(unsigned long a, unsigned long n) {   /* memory the program may have the kernel read: the M3a code and stack pages, and the loaded app's whole window, stack and heap */
    return span(a, n, UB + UP_CODE, 4096) || span(a, n, UB + UP_STACK, 4096)
        || span(a, n, UB + UP_APP, app_len) || span(a, n, UB + UP_ASTACK, app_len ? APP_STACK_PAGES * 4096UL : 0)
        || span(a, n, UB + UP_WIN, app_wbytes) || span(a, n, UB + UP_HEAP, heap_len());
}
int user_writable(unsigned long a, unsigned long n) {   /* memory the kernel may store into for the program: data, zeroed pool, stacks and heap, never code (read-only at EL1 too: a store there would be a kernel fault) */
    return span(a, n, UB + UP_STACK, 4096) || span(a, n, UB + UP_APP + app_code, app_len - app_code)
        || span(a, n, UB + UP_ASTACK, app_len ? APP_STACK_PAGES * 4096UL : 0) || span(a, n, UB + UP_WIN, app_wbytes)
        || span(a, n, UB + UP_HEAP, heap_len());
}
/* M3e: JT_SYS_BRK with i386's semantics (docs/ARCHITECTURE.md, kernel/brk.c): 0 reads the top, a fresh app's is the heap's
   base; any other value sets it, to the byte. Pages between the old and new top (rounded up to whole pages) are mapped EL0
   read-write and never executable, then zeroed, so no earlier app's bytes are ever seen; shrinking unmaps them. A top below
   the base or past the arena's end is -EINVAL and nothing moves. The memory is the arena's own (identity mapped, 14 MiB),
   so there is nothing to run out of below that cap. app_unmap drops the whole heap when the app ends. */
static long app_brk_set(unsigned long want) {
    const unsigned long PAGE = 0x3UL | (1UL << 2) | (3UL << 8) | (1UL << 10), AP_EL0_RW = 1UL << 6, PXN = 1UL << 53, UXN = 1UL << 54;
    unsigned long base = UB + UP_HEAP, top = app_brk ? app_brk : base;
    if (!want) return (long)top;
    if (want < base || want > UB + UP_WIN) return -22;   /* the heap stops where the window buffer starts */
    unsigned long have = (top + 4095) & ~4095UL, need = (want + 4095) & ~4095UL;
    for (unsigned long v = have; v < need; v += 4096) l3[ap->l3off + ((v - UB) >> 12)] = v | PAGE | AP_EL0_RW | PXN | UXN;
    for (unsigned long v = need; v < have; v += 4096) l3[ap->l3off + ((v - UB) >> 12)] = 0;
    __asm__ volatile ("dsb ishst\n tlbi vmalle1\n dsb ish\n isb" ::: "memory");
    for (unsigned long v = have; v < need; v += 8) *(unsigned long *)v = 0;
    app_brk = want;
    return (long)want;
}
/* JT_SYS_SCHED_YIELD from the scheduled app: keep every register and where it stood, with 0 as the call's answer, and go back
   to the desktop's loop. app_tick resumes it on a later pass, at the instruction after its svc. */
static void app_park(const struct frame *f, const unsigned long *fp) {
    for (int i = 0; i < 31; i++) app_frame.x[i] = f->x[i];
    for (int i = 0; i < 66; i++) app_fp[i] = fp[i];   /* M3e: its floating point registers, the block el0_sync_entry saved */
    app_frame.x[0] = 0; app_frame.elr = f->elr;
    __asm__ volatile ("mrs %0, sp_el0" : "=r"(app_sp));
    app_parked = 1;
    leave_user(0);
}
static void wd_kill(void) {   /* 4.5: from exc_irq, the app has had its three seconds at EL0 without a syscall */
    wd_live = 0;
    uart_puts("M4 watchdog: "); uart_puts(app_cur ? app_cur->name : "app"); uart_puts(" ran 3 s without a syscall, stopped\n");
    user_faulted = 1;
    leave_user((unsigned long)-1);
}
static void svc_log(unsigned long n, unsigned long a0, long r) {   /* the calls that are not write and exit, on the UART: how a program's progress shows with no debugger */
    static unsigned long last_n, last_a0; static long last_r = 1;   /* M3e: a call that repeats the last line exactly (Music asks the time on every pass) is not logged again */
    if (n == last_n && a0 == last_a0 && r == last_r) return;
    last_n = n; last_a0 = a0; last_r = r;
    uart_puts("M3c svc "); uart_dec((unsigned)n); uart_puts(" a0="); uart_hex(a0); uart_puts(" -> ");
    if (r < 0) { uart_putc('-'); r = -r; }
    uart_dec((unsigned)r); uart_putc('\n');
}
#ifdef MUSIC_TEST_UTC
void fp_clobber(void);   /* fptest.c, linked into the musictest build only: the kernel using the FPU inside an app's call and while it is parked */
#define APP_FP_CLOBBER() fp_clobber()
#else
#define APP_FP_CLOBBER() ((void)0)
#endif
/* 3.10.0: the system clipboard, one plain-text buffer of 4 KB (JT_CLIP_MAX) for the whole kernel: Notes reaches it through
   JT_SYS_CLIPBOARD (399, the i386 contract in user/jtsys.h), the Terminal's input line and the Browser's address bar call
   clip_put and clip_take directly. A copy over 4 KB is refused (-EINVAL, as on i386); a paste takes what fits the field and
   says so with CLIPTRUNC. The UART lines are the length only, never the text (CLIPCOPY:n, CLIPPASTE:n). */
static char clip_buf[JT_CLIP_MAX];
static unsigned clip_len;
static void clip_log(const char *tag, unsigned n) { con_quiet = 1; uart_puts(tag); uart_dec(n); uart_putc('\n'); }
void clip_put(const char *s, unsigned n) {
    if (n > JT_CLIP_MAX) n = JT_CLIP_MAX;
    for (unsigned i = 0; i < n; i++) clip_buf[i] = s[i];
    clip_len = n; clip_log("CLIPCOPY:", n);
}
unsigned clip_take(char *o, unsigned room) {   /* at most `room` bytes of the clipboard into o; 0 when it is empty */
    unsigned n = clip_len < room ? clip_len : room;
    for (unsigned i = 0; i < n; i++) o[i] = clip_buf[i];
    if (n) { clip_log("CLIPPASTE:", n); if (n < clip_len) { con_quiet = 1; uart_puts("CLIPTRUNC\n"); } }
    return n;
}
static long app_clip(unsigned long op, unsigned long buf, unsigned long len) {
    if (op == JT_CLIP_SET) {
        if (len > JT_CLIP_MAX) return -22;
        if (len && !user_in_range(buf, len)) return -14;
        clip_put((const char *)buf, (unsigned)len);
        return (long)len;
    }
    if (op == JT_CLIP_GET) {
        if (len && !user_writable(buf, len)) return -14;
        return (long)clip_take((char *)buf, (unsigned)len);
    }
    return -22;
}
void exc_el0_sync(struct frame *f, const unsigned long *fp) {   /* fp: the app's FP registers, saved below the frame by el0_sync_entry */
    unsigned ec = (unsigned)(f->esr >> 26);
    APP_FP_CLOBBER();
    if (ec == 0x15) {   /* svc from EL0 */
        unsigned long n = f->x[8], a0 = f->x[0];
        if (n == JT_SYS_WRITE && a0 != 1 && a0 != 2) f->x[0] = (unsigned long)ufile_syscall(n, a0, f->x[1], f->x[2]);   /* M3d: a file from open (fd 0 is -EBADF there) */
        else if (n == SYS_WRITE || n == JT_SYS_WRITE) {
            const char *p = (const char *)f->x[1]; unsigned long len = f->x[2];
            if (a0 != 1 && a0 != 2) { f->x[0] = (unsigned long)-9; return; }   /* -EBADF: the console is fd 1 and fd 2, nothing else yet */
            if (!user_in_range((unsigned long)p, len)) { f->x[0] = (unsigned long)-14; return; }   /* -EFAULT */
            for (unsigned long i = 0; i < len; i++) { if (con_line_start && !user_loud) con_quiet = 1; uart_putc(p[i]); }   /* the boot self-test talks to the UART only */
            f->x[0] = len;
        } else if (n == SYS_EXIT || n == JT_SYS_EXIT) {
            uart_puts("M3 EL0 exit "); uart_dec((unsigned)f->x[0]); uart_putc('\n');
            leave_user(f->x[0]);
        } else if (n == JT_SYS_SCHED_YIELD) {   /* every turn of an app's loop, so never logged */
            if (app_running) app_park(f, fp);
            f->x[0] = 0;                         /* outside the scheduler (the boot self-test) there is nothing to yield to */
        } else if (n == JT_SYS_WINDOW_POLL) f->x[0] = (unsigned long)app_window_poll(a0, f->x[1]);   /* the same */
        else {
            long r;
            if (n == JT_SYS_TIME) {   /* seconds since the epoch, 0 until the clock is set; a non-null pointer is also stored through */
                unsigned long t = cal_local();   /* local, like the Clock: the Calendar app circles the day the Clock names, not the UTC one */
                r = (int)t;
                if (a0) {
                    if (!user_writable(a0, 4)) r = -14;   /* -EFAULT, like kernel/syscall.c's sys_time */
                    else for (int i = 0; i < 4; i++) ((unsigned char *)a0)[i] = (unsigned char)(t >> (8 * i));
                }
            } else if (n == JT_SYS_GETPID) r = user_pid ? user_pid : 1;
            else if (n == JT_SYS_WINDOW_OPEN) r = app_window_open(a0, f->x[1]);
            else if (n == JT_SYS_TEXT2) r = app_text(a0);
            else if (n == JT_SYS_AUDIO) r = app_audio(a0, f->x[1], f->x[2]);   /* M3e */
            else if (n == JT_SYS_BRK) r = app_brk_set(a0);
            else if (n == JT_SYS_CLIPBOARD) r = app_clip(a0, f->x[1], f->x[2]);   /* 3.10.0 */
            else if (n == JT_SYS_TASKS) {
                if (!a0 || !user_writable(a0, 20)) r = -14;
                else { unsigned t = (unsigned)(cnt_now() / (cnt_hz() / 100)); for (int i = 0; i < 20; i++) ((unsigned char *)a0)[i] = i < 4 ? (unsigned char)(t >> (8 * i)) : 0; r = 0; }
            }
            else r = ufile_syscall(n, a0, f->x[1], f->x[2]);   /* M3d: the file calls; -ENOSYS (-38) for every number the ARM kernel does not answer yet, JT_SYS_AUDIO_RECORD (394) among them */
            if (n != JT_SYS_READ && n != JT_SYS_AUDIO && n != JT_SYS_TEXT2) svc_log(n, a0, r);   /* a read is every few hundred bytes of a file, audio a hundred calls a second: the UART would drown */
            f->x[0] = (unsigned long)r;
        }
        return;
    }
    user_faulted = 1;
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
/* Map the M3a arena and load the test program (user.S) into its code page. Done once; the C programs of M3b and M3c go
   through user_load into their own window and leave this page alone, so the test program is always there to probe them. */
static void user_init(const char *img, unsigned long len) {
    const unsigned long PAGE = 0x3UL | (1UL << 2) | (3UL << 8) | (1UL << 10);   /* valid page, normal memory, inner shareable, access flag */
    const unsigned long AP_EL0_RW = 1UL << 6, AP_RO = 3UL << 6, PXN = 1UL << 53, UXN = 1UL << 54;
    l3[UP_CODE >> 12] = (UB + UP_CODE) | PAGE | AP_EL0_RW | PXN | UXN;   /* writable for now, so the kernel can copy the program in */
    l3[UP_STACK >> 12] = (UB + UP_STACK) | PAGE | AP_EL0_RW | PXN | UXN;
    l3[UP_KERN >> 12] = (UB + UP_KERN) | PAGE | PXN | UXN;               /* AP=00: EL1 only */
    __asm__ volatile ("dsb ishst\n tlbi vmalle1\n dsb ish\n isb" ::: "memory");
    char *code = (char *)(UB + UP_CODE);
    for (unsigned long i = 0; i < len; i++) code[i] = img[i];
    const char *secret = "KERNEL-ONLY-SECRET";
    for (int i = 0; secret[i]; i++) ((char *)(UB + UP_KERN))[i] = secret[i];
    for (unsigned long a = (unsigned long)code & ~63UL; a < (unsigned long)code + len; a += 64)
        __asm__ volatile ("dc cvau, %0" :: "r"(a) : "memory");
    __asm__ volatile ("dsb ish\n ic iallu\n dsb ish\n isb" ::: "memory");
    l3[UP_CODE >> 12] = (UB + UP_CODE) | PAGE | AP_RO | PXN;             /* now read-only for everyone, executable by EL0 only */
    __asm__ volatile ("dsb ishst\n tlbi vmalle1\n dsb ish\n isb" ::: "memory");
}

/* ---- M3c: the app loader ----
   The image table (userapps.S) holds each program as user_c.ld wrote it: a flat -pie image linked at 0, ending in a 16 byte
   trailer { "JTU1", bytes of code (a page multiple), .rela.dyn start, .rela.dyn end }. user_load copies one image to the app
   window and applies its R_AARCH64_RELATIVE entries (base + addend) to that copy only, never to the baked-in original, so a
   program can be loaded again. A relocation of any other kind, or one that lands outside the image, refuses the load. The
   code pages (everything before the trailer's code length) end up read-only and executable by EL0; the data pages after them,
   and APP_ZERO_PAGES more that are zeroed (with the rest of the last data page), read-write and never executable. The stack is
   APP_STACK_PAGES fresh zeroed pages; the pages around the window and the stack are unmapped, so running off either faults. */
static unsigned rd32(const char *p) { const unsigned char *u = (const unsigned char *)p; return u[0] | (unsigned)u[1] << 8 | (unsigned)u[2] << 16 | (unsigned)u[3] << 24; }
static unsigned long rd64(const char *p) { return rd32(p) | (unsigned long)rd32(p + 4) << 32; }
/* M3e: JT_SYS_AUDIO, i386's contract (docs/ARCHITECTURE.md): op 1 queues a struct jt_audio_play's PCM (at most 8192 bytes a
   call, END dropped from a call that is cut), op 2 fills a struct jt_audio_status, op 3 stops. Every pointer is checked here
   before audio.c sees a byte. The LP64 jt_audio_play is { pcm 8, len 4, rate 4, flags 4 } padded to 24 bytes. There is
   always a jack (QEMU virt plays into nothing at the same pace), so never -ENODEV. */
#include "audio.h"
static long app_audio(unsigned long op, unsigned long arg, unsigned long size) {
    if (op == 3) { audio_app_stop(); return 0; }
    if (op == 2) {
        unsigned st[7];   /* version, size, playing, queued, space, rate, played */
        if (size < 8) return -22;
        if (size > sizeof st) size = sizeof st;
        if (!user_writable(arg, size)) return -14;
        audio_app_status(st);
        for (unsigned long i = 0; i < size; i++) ((unsigned char *)arg)[i] = ((const unsigned char *)st)[i];
        return (long)size;
    }
    if (op != 1 || size < 24) return -22;
    if (!user_in_range(arg, 20)) return -14;
    unsigned long pcm = rd64((const char *)arg);
    unsigned len = rd32((const char *)arg + 8), rate = rd32((const char *)arg + 12), flags = rd32((const char *)arg + 16);
    if (!len) return -22;
    if (len > 8192) { len = 8192; flags &= ~1u; }
    if (!user_in_range(pcm, len)) return -14;
    return audio_app_queue((const unsigned char *)pcm, len, rate, flags & 1);
}
static const struct user_app *user_find(const char *name, unsigned n) {
    for (const struct user_app *a = user_apps; a->name; a++) {
        unsigned i = 0;
        while (i < n && a->name[i] == name[i]) i++;
        if (i == n && !a->name[n]) return a;
    }
    return 0;
}
static void app_unmap(void) {   /* its pages, its stack, its window buffer and (M3e) its heap */
    app_len = app_code = app_img = app_wbytes = app_brk = 0; app_hi = 0;
    for (unsigned long i = UP_APP >> 12; i < ARENA_BLOCKS * 512; i++) l3[ap->l3off + i] = 0;
    __asm__ volatile ("dsb ishst\n tlbi vmalle1\n dsb ish\n isb" ::: "memory");
}
static unsigned long user_load(const struct user_app *a) {
    const unsigned long PAGE = 0x3UL | (1UL << 2) | (3UL << 8) | (1UL << 10);
    const unsigned long AP_EL0_RW = 1UL << 6, AP_RO = 3UL << 6, PXN = 1UL << 53, UXN = 1UL << 54;
    unsigned long len = (unsigned long)(a->end - a->start);
    unsigned magic = 0, code_end = 0, rela_s = 0, rela_e = 0;
    if (len >= 32 && len <= UAPP_MAX && !(len & 3)) { magic = rd32(a->end - 16); code_end = rd32(a->end - 12); rela_s = rd32(a->end - 8); rela_e = rd32(a->end - 4); }
    if (magic != 0x3155544a || !code_end || (code_end & 4095) || code_end > len - 16 || rela_s > rela_e || rela_e > len - 16 || (rela_s & 7) || (rela_e - rela_s) % 24) {
        uart_puts("M3c load FAIL "); uart_puts(a->name); uart_puts(": not a user image\n"); return 0;
    }
    unsigned long pages = (len + 4095) / 4096 + APP_ZERO_PAGES;
    char *base = (char *)(UB + UP_APP);
    app_unmap();
    for (unsigned long i = 0; i < pages; i++) l3[ap->l3off + (UP_APP >> 12) + i] = (UB + UP_APP + i * 4096) | PAGE | AP_EL0_RW | PXN | UXN;   /* writable while the kernel fills it in */
    for (unsigned long i = 0; i < APP_STACK_PAGES; i++) l3[ap->l3off + (UP_ASTACK >> 12) + i] = (UB + UP_ASTACK + i * 4096) | PAGE | AP_EL0_RW | PXN | UXN;
    __asm__ volatile ("dsb ishst\n tlbi vmalle1\n dsb ish\n isb" ::: "memory");
    for (unsigned long i = 0; i < pages * 512; i++) ((unsigned long *)base)[i] = 0;   /* a stale program's bytes never reach this one */
    for (unsigned long i = 0; i < APP_STACK_PAGES * 512; i++) ((unsigned long *)(UB + UP_ASTACK))[i] = 0;
    for (unsigned long i = 0; i < len; i++) base[i] = a->start[i];
    for (unsigned long r = rela_s; r < rela_e; r += 24) {
        unsigned long off = rd64(base + r), info = rd64(base + r + 8), add = rd64(base + r + 16);
        if (info != 1027 /* R_AARCH64_RELATIVE */ || (off & 7) || off + 8 > len - 16 || add > pages * 4096) {
            uart_puts("M3c load FAIL "); uart_puts(a->name); uart_puts(": bad relocation\n"); app_unmap(); return 0;
        }
        *(unsigned long *)(base + off) = (unsigned long)base + add;
    }
#ifdef HANG_TEST   /* arm64-watchdog-check.py's build: the app in slot 0 spins on its first instruction, `b .`, no syscall ever */
    if (hang_arm) *(unsigned *)base = 0x14000000;
#endif
    for (unsigned long p = (unsigned long)base & ~63UL; p < (unsigned long)base + code_end; p += 64) __asm__ volatile ("dc cvau, %0" :: "r"(p) : "memory");
    __asm__ volatile ("dsb ish\n ic iallu\n dsb ish\n isb" ::: "memory");
    for (unsigned long i = 0; i < code_end / 4096; i++) l3[ap->l3off + (UP_APP >> 12) + i] = (UB + UP_APP + i * 4096) | PAGE | AP_RO | PXN;   /* code: read-only, EL0 may execute */
    __asm__ volatile ("dsb ishst\n tlbi vmalle1\n dsb ish\n isb" ::: "memory");
    app_len = pages * 4096; app_code = code_end; app_img = len;
    uart_puts("M3c load "); uart_puts(a->name); uart_puts(": "); uart_dec((unsigned)len); uart_puts(" bytes, ");
    uart_dec(code_end / 4096); uart_puts(" code pages, "); uart_dec((unsigned)((rela_e - rela_s) / 24)); uart_puts(" relocations, code ");
    uart_hex((unsigned long)base); uart_puts(" data "); uart_hex((unsigned long)base + code_end);
    uart_puts(" bss "); uart_hex((unsigned long)base + ((len + 15) & ~15UL)); uart_putc('\n');
    return (unsigned long)base;
}
#ifndef PI_BUILD
/* Load an app and run it at EL0 until it exits or faults, for the boot self-test (QEMU builds only). 0: it ran (user_status
   is its exit code, user_faulted says it was a fault, not an exit); -1: it did not load. */
static int user_run(const struct user_app *a) {
    unsigned long entry = user_load(a);
    if (!entry) return -1;
    user_pid++; user_faulted = 0; ufile_owner = 0;
    arena_only((int)ap->slot);
    user_status = enter_user(entry, UB + UP_ASTACK + APP_STACK_PAGES * 4096UL, 0, 0);
    ufile_release();   /* M3d: what it left open is closed for it, however it ended */
    return 0;
}
#endif
/* ---- M3c part 2: a cooperative scheduler for one app ----
   user_run above is the boot self-test's way: enter_user, and the kernel waits for the exit. An app the desktop launches (the
   dock, Spotlight or `userrun`) goes through app_start instead and runs from the desktop's own loop. app_tick resumes it at
   the instruction after its last svc; it runs until it yields (JT_SYS_SCHED_YIELD parks it: app_park), exits or faults, and
   the loop carries on with the keys, the mouse and the clock. So an app that loops forever on poll and yield, as every app
   with a window does, leaves the desktop responsive; only an app run this way gets a window. A parked app runs again on the
   next pass that has an event for it, and every 10 ms (the generic counter, never `ticks`) in case it is animating. An app
   that loops without ever making a syscall still holds the core: preempting one needs IRQs unmasked at EL0, not done yet. */
static void app_kill_one(const char *how);
static void app_end(unsigned long status, const char *how) {   /* an exit, a fault or a kill: the window goes, then the pages */
    const struct user_app *a = app_cur;
    app_cur = 0; app_parked = app_kick = 0; app_close_at = 0; user_status = status;
    ufile_owner = ap->slot + 1;
    ufile_release();   /* M3d: its open files are written back and closed, after an exit, a fault or its red dot alike */
    audio_app_stop();  /* M3e: and its sound stops (what is already in the PWM ring plays out) */
    struct app *me = ap;
    app_window_close();   /* leaves `ap` on the window now in front... */
    ap = me;              /* ...so put it back: the pages unmapped and the line printed below are this app's, not the front one's */
    app_unmap();
    uart_puts("M3c app "); uart_puts(a->name);
    if (how) { uart_putc(' '); uart_puts(how); } else { uart_puts(" exit "); uart_dec((unsigned)status); }
    uart_puts(", ran "); uart_dec(app_runs); uart_puts(" times\n");
}
static void app_kill_one(const char *how) { if (app_cur && !app_running) app_end((unsigned long)-1, how); }
static void app_kill(const char *how) {   /* every app ends: a kernel-drawn window is opening where the apps' windows were */
    for (unsigned i = 0; i < APP_SLOTS; i++) { ap = &apps[i]; if (app_cur && !app_running) app_end((unsigned long)-1, how); }
    ap = &apps[app_top >= 0 ? app_top : 0];
}
static void app_tick(void) {   /* every app with something to do gets its turn: it runs until it yields, exits or faults, and a fault ends that app alone */
    for (unsigned i = 0; i < APP_SLOTS; i++) {
        ap = &apps[i];
        if (!app_cur || app_running) continue;
        unsigned long now = cnt_now();
        if (app_close_at && now > app_close_at) { app_kill_one("closed by its red dot"); continue; }   /* it never read the close click */
        if (!app_kick && now - app_last < cnt_hz() / 100) continue;
        app_kick = 0; app_last = now; app_runs++;
        app_parked = 0; user_faulted = 0; app_running = 1; ufile_owner = ap->slot + 1;
        APP_FP_CLOBBER();
        arena_only((int)ap->slot);
        wd_live = 1; wd_arm();
        unsigned long st = resume_user(&app_frame, app_sp, app_fp);
        wd_live = 0; wd_off();
        app_running = 0;
        if (!app_parked) app_end(st, user_faulted ? "stopped by a fault" : 0);
    }
    ap = &apps[app_top >= 0 ? app_top : 0];
}
static int app_start(const struct user_app *a) {   /* 0 started (it runs on the next pass), -2 it would not load, -3 every slot is taken */
    unsigned sl = 0;
    while (sl < APP_SLOTS && apps[sl].cur) sl++;
    if (sl == APP_SLOTS) return -3;
    ap = &apps[sl];
#ifdef HANG_TEST
    hang_arm = sl == 0;
#endif
    unsigned long entry = user_load(a);
#ifdef HANG_TEST
    hang_arm = 0;
#endif
    if (!entry) return -2;
    if (a->name[0] == 'n' && a->name[1] == 'o' && a->name[2] == 't' && a->name[3] == 'e' && a->name[4] == 's' && !a->name[5])
        ufile_mkdirs("NOTES/NOTES");   /* M3d: Notes' default folder in JT/, made before it looks, as i386's notes_check_support does */
    if (a->name[0] == 'm' && a->name[1] == 'u' && a->name[2] == 's' && a->name[3] == 'i' && a->name[4] == 'c' && !a->name[5])
        ufile_mkdirs("MUSIC");         /* M3e: Music's library, JT/MUSIC/, there for songs dropped onto the card from a Mac */
    if (a->name[0] == 'b' && a->name[1] == 'o' && a->name[2] == 'o' && a->name[3] == 'k' && a->name[4] == 's' && !a->name[5])
        ufile_mkdirs("BOOKS");         /* 2.66: Books' library, JT/BOOKS/, the books the card ships with and the ones dropped there */
    for (int i = 0; i < 31; i++) app_frame.x[i] = 0;   /* argc 0, argv 0, like user_run */
    for (int i = 0; i < 66; i++) app_fp[i] = 0;        /* M3e: its FP registers start at 0, FPCR 0 */
    app_frame.elr = entry; app_sp = UB + UP_ASTACK + APP_STACK_PAGES * 4096UL;
    app_cur = a; app_kick = 1; app_qh = app_qt = 0; app_runs = 0; app_close_at = 0; user_pid++;
    uart_puts("M3c app "); uart_puts(a->name); uart_puts(" starts\n");
    return 0;
}
/* cmd.c's `userrun NAME`: start it and give it its first run here, so what it writes then lands in the Terminal. 0 it ended
   (*status, *faulted), 1 it is running (parked), 2 running in its window, -1 no such app, -2 it would not load, -3 busy. */
int userrun(const char *name, unsigned n, unsigned long *status, int *faulted) {
    const struct user_app *a = user_find(name, n);
    if (!a) return -1;
    int r = app_start(a);
    if (r) return r;
    struct app *me = ap;
    user_loud = 1;
    app_tick();
    user_loud = 0;
    if (me->cur == a) return me->wbytes ? 2 : 1;
    *status = user_status; *faulted = user_faulted;
    return 0;
}
#ifndef PI_BUILD
/* Probe the pages of the app that is loaded, from EL0: user.S's entry D gets { bss word, data page, code base, window size }
   in a block at the bottom of its own stack page and checks them (see user.S). It ends on a store to the code page, which must
   fault; the kernel survives and says so. */
static void user_probe(int pass) {
    unsigned long base = UB + UP_APP, *args = (unsigned long *)(UB + UP_STACK);
    args[0] = base + ((app_img + 15) & ~15UL); args[1] = base + app_code; args[2] = base; args[3] = UAPP_MAX;
    uart_puts("M3c probe "); uart_dec((unsigned)pass); uart_puts(" starts\n");
    user_faulted = 0;
    enter_user(UB + UP_CODE + (unsigned long)(user_entry_d - user_start), UB + UP_STACK + 4096, (unsigned long)args, 0);
    uart_puts(user_faulted ? "M3c probe survived the code write\n" : "M3c probe FAIL: the code write did not fault\n");
}
/* Branch to an address inside the loaded app that is not code: user.S's entry C does the `br`, which must be an instruction
   abort (the pages are never executable), and the kernel carries on. */
static void user_exec_probe(const char *what, unsigned long target) {
    uart_puts("M3c exec "); uart_puts(what); uart_putc(' '); uart_hex(target); uart_putc('\n');
    user_faulted = 0;
    enter_user(UB + UP_CODE + (unsigned long)(user_entry_c - user_start), UB + UP_STACK + 4096, UB + UP_KERN, target);
    uart_puts(user_faulted ? "M3c exec refused\n" : "M3c exec FAIL: it ran\n");
}
#endif
static void user_demo(void) {
    /* 4.5: an APP_BOOT build has opened its apps by now (fb_desktop runs first). These tests load programs into slot 0 and its
       low pages, so with apps live they would write over them, or, with `ap` on another slot, into pages that slot never mapped
       (a kernel data abort at the third app's arena on QEMU). They run only on a machine with no app open. */
    if (apps_live()) { uart_puts("M3 self-tests skipped: apps are open\n"); return; }
    ap = &apps[0];
    user_init(user_start, (unsigned long)(user_end - user_start));
    unsigned long code = UB + UP_CODE, sp = UB + UP_STACK + 4096, kpage = UB + UP_KERN;
    uart_puts("M3 EL0 task starts\n");
    enter_user(code + (unsigned long)(user_entry_a - user_start), sp, kpage, 0);
    uart_puts("M3 kernel survived the fault\n");
    uart_puts("M3 EL0 jump task starts\n");   /* branch to kernel text: EL0 may not execute it */
    enter_user(code + (unsigned long)(user_entry_c - user_start), sp, kpage, (unsigned long)_start);
    uart_puts("M3 kernel survived the jump\n");
    uart_puts("M3 EL0 second task starts\n");
    user_status = enter_user(code + (unsigned long)(user_entry_b - user_start), sp, kpage, 0);
    if (user_status == 7) uart_puts("M3 userland ok\n"); else uart_puts("M3 userland FAIL\n");
    {   /* 4.4: slot 1 gets one page EL0 could use, and from slot 0 user.S's entry F loads from it, which must fault */
        const unsigned long PAGE = 0x3UL | (1UL << 2) | (3UL << 8) | (1UL << 10), AP_EL0_RW = 1UL << 6, PXN = 1UL << 53, UXN = 1UL << 54;
        unsigned long pg = apps[1].base + UP_STACK, *e = &l3[apps[1].l3off + (UP_STACK >> 12)];
        *e = pg | PAGE | AP_EL0_RW | PXN | UXN;
        arena_only(0);
        uart_puts("M4 arena probe starts\n");
        user_faulted = 0;
        enter_user(code + (unsigned long)(user_entry_f - user_start), sp, pg, 0);
        uart_puts(user_faulted ? "M4 arena isolation ok\n" : "M4 arena isolation FAIL: slot 1 was readable from slot 0\n");
        *e = 0;
        __asm__ volatile ("dsb ishst\n tlbi vmalle1\n dsb ish\n isb" ::: "memory");
    }
#ifndef PI_BUILD
    /* M3b: user/hello.c itself, built for aarch64 and talking Joshua Tree syscall numbers over svc. This runs before any
       card is mounted, so since M3d its open of HELLO.TXT is answered -ENOENT (-2), which hello.c reports and answers with
       exit 1. The kernel takes that exit as the pass. With a card holding JT/HELLO.TXT, `userrun hello` goes all the way
       to exit 9 (tools/checks/arm64-m3c-check.py). */
    const struct user_app *hello = user_find("hello", 5), *cal = user_find("calendar", 8);
    uart_puts("M3b EL0 C task starts\n");
    if (hello && !user_run(hello) && !user_faulted && user_status == 1) uart_puts("M3b userland C ok\n"); else uart_puts("M3b userland C FAIL\n");
    /* M3d: user.S's entry E abuses the file calls once each: a kernel pointer as a path, a read into the kernel-only page,
       a path with "..", a 4 GB readfile, a readdir whose records run past its page. Each must come back an errno (the
       checks run before any card is touched), then it exits 0 and the kernel carries on. */
    uart_puts("M3d file call abuse starts\n");
    user_faulted = 0;
    user_status = enter_user(code + (unsigned long)(user_entry_e - user_start), sp, kpage, 0);
    uart_puts(!user_faulted && user_status == 0 ? "M3d file call abuse ok\n" : "M3d file call abuse FAIL\n");
    /* M3c: user/calendar.c, about 34 KB with relocated pointer tables, a data page and a zeroed pool. It opens its window
       first; run here, outside the scheduler, there is none to give (-ENODEV, as i386 answers a program run from the text
       shell), so it writes "calendar: no window" to fd 2 and exits 1, which is the pass. Then
       user.S's probe runs against calendar's pages, twice with a reload between: the second time proves the loader zeroes
       the pool again and puts the data back. */
    uart_puts("M3c EL0 calendar starts\n");
    if (cal && !user_run(cal) && !user_faulted && user_status == 1) uart_puts("M3c userland calendar ok\n"); else uart_puts("M3c userland calendar FAIL\n");
    if (cal) {
        user_probe(1);
        if (user_load(cal)) user_probe(2); else uart_puts("M3c probe FAIL: calendar did not load again\n");
        unsigned long base = UB + UP_APP;   /* never executable: a data page, the first whole page of the zeroed pool, the stack */
        user_exec_probe("data", base + app_code);
        user_exec_probe("pool", base + (app_img + 4095) / 4096 * 4096);
        user_exec_probe("stack", UB + UP_ASTACK);
    }
#endif
}

/* ---- M1c: a framebuffer. QEMU's virt machine has no display unless one is asked for; ramfb is a plain RAM
   framebuffer the guest configures through fw_cfg (a DMA write of the "etc/ramfb" file). A Pi asks its GPU firmware
   for one through the mailbox, at the monitor's own size when the firmware knows it. Either way fb_setup hands back
   plain 32-bit pixels and the drawing is shared. Everything is laid out against an 800x600 design and scaled by
   height/600, so 800x600 (QEMU) draws exactly what it always did. ---- */
#ifdef FB_BOOT_W   /* the glitch test build: QEMU's ramfb at 1080p (window_scale 2), the size a Pi shows on a monitor, so the checks see the scale-2 layout with a USB mouse */
static unsigned fb_w = FB_BOOT_W, fb_h = FB_BOOT_H;
#else
static unsigned fb_w = 800, fb_h = 600;   /* the screen we really got */
#endif
static unsigned disp_w, disp_h;           /* what the firmware said the monitor is, 0x0 when nobody asked */
static unsigned int *fb;
static unsigned fb_pitch;   /* in pixels */
static int fb_swap;         /* red and blue the other way round in memory */
static int sc(int v) { return v * (int)fb_h / 600; }   /* a length on the 800x600 design, at this screen's size */
static int sg(int v) { return v * (int)fb_h / 540; }   /* a length on the real desktop's 960x540 grid (docs/ARCHITECTURE.md), at this screen's size */
static void dcache_clean(void *p, unsigned long n) {   /* push lines out to RAM, where a GPU or DMA engine reads */
    for (unsigned long a = (unsigned long)p & ~63UL; a < (unsigned long)p + n; a += 64) __asm__ volatile ("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile ("dsb sy" ::: "memory");
}
/* 4.3.0: a back buffer. fb is a RAM copy of the screen (FB_RAM_BASE, same size and pitch as the scanout) and every painter
   draws there; fb_scan is the real scanout, and fb_flush is the one place that writes it: it copies the rectangle over and
   cleans it out of the data cache (the GPU reads RAM, not the cache). Without the RAM (a screen bigger than FB_RAM_MAX)
   fb_scan stays 0, fb is the scanout and fb_flush only cleans, as before 4.3.0. Between fb_hold and fb_release the flushes
   only add to one dirty rectangle, copied once at the outermost release: a pointer report that moves a window and the arrow
   reaches the screen as one copy, never as the half-drawn steps. fb_mute drops flushes while win_set names the damage. */
static unsigned *fb_scan;
static int fb_defer, fb_mute, fb_tag, fd_x0, fd_y0, fd_x1, fd_y1;   /* fb_tag: the batch moved a window (FB_TRACE prints it) */
static int off_on;   /* off_begin's buffer is the target: a flush waits for off_end, as fb points into that buffer */
static void fb_copy(int x, int y, int w, int h) {   /* clipped already */
    for (int j = y; j < y + h; j++) {
        unsigned o = (unsigned)j * fb_pitch + (unsigned)x;
        if (fb_scan) {
            unsigned long *d = (unsigned long *)(fb_scan + o); const unsigned long *s = (const unsigned long *)(fb + o); int i = 0;
            if (!(o & 1)) for (; i + 2 <= w; i += 2) *d++ = *s++;
            for (; i < w; i++) fb_scan[o + (unsigned)i] = fb[o + (unsigned)i];
        }
        unsigned long a = (unsigned long)((fb_scan ? fb_scan : fb) + o), e = a + (unsigned long)w * 4;
        for (a &= ~63UL; a < e; a += 64) __asm__ volatile ("dc civac, %0" :: "r"(a) : "memory");
    }
    __asm__ volatile ("dsb sy" ::: "memory");
}
static void fb_flush(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)fb_w) w = (int)fb_w - x;
    if (y + h > (int)fb_h) h = (int)fb_h - y;
    if (w <= 0 || h <= 0 || fb_mute || off_on) return;
    if (!fb_defer) { fb_copy(x, y, w, h); return; }
    if (fd_x1 <= fd_x0) { fd_x0 = x; fd_y0 = y; fd_x1 = x + w; fd_y1 = y + h; return; }
    if (x < fd_x0) fd_x0 = x;
    if (y < fd_y0) fd_y0 = y;
    if (x + w > fd_x1) fd_x1 = x + w;
    if (y + h > fd_y1) fd_y1 = y + h;
}
static void fb_hold(void) { fb_defer++; }
static void fb_release(void) {
    if (!fb_defer || --fb_defer) return;
    if (fd_x1 > fd_x0) {
#ifdef FB_TRACE   /* the dirtyrect build (arm64-dirtyrect-check.py): one line per window move, the rectangle copied and how long the copy took */
        unsigned long t = cnt_now();
        fb_copy(fd_x0, fd_y0, fd_x1 - fd_x0, fd_y1 - fd_y0);
        if (fb_tag) {
            con_quiet = 1; uart_puts("fb: flush "); uart_dec((unsigned)fd_x0); uart_putc(','); uart_dec((unsigned)fd_y0); uart_putc(',');
            uart_dec((unsigned)(fd_x1 - fd_x0)); uart_putc(','); uart_dec((unsigned)(fd_y1 - fd_y0));
            uart_puts(" copy "); uart_dec((unsigned)((cnt_now() - t) * 1000000 / cnt_hz())); uart_puts(" us\n");
        }
#else
        fb_copy(fd_x0, fd_y0, fd_x1 - fd_x0, fd_y1 - fd_y0);
#endif
    }
    fd_x0 = fd_x1 = 0; fb_tag = 0;
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
static int screen_blank(unsigned blank) {
    unsigned m[] = { 7 * 4, 0, 0x40002, 4, 0, blank, 0 };
    for (unsigned i = 0; i < sizeof m / 4; i++) mbox[i] = m[i];
    return mbox_call() && mbox[4] == 0x80000004u && (mbox[5] & 1u) == blank;
}
unsigned mbox_clock_rate(unsigned id) {   /* emmc.c: a firmware clock's rate in Hz (tag 0x00030002; EMMC2 is clock 12), 0 with no answer */
    unsigned m[] = { 8 * 4, 0, 0x30002, 8, 0, id, 0, 0 };
    for (unsigned i = 0; i < sizeof m / 4; i++) mbox[i] = m[i];
    return mbox_call() && mbox[5] == id ? mbox[6] : 0;
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
unsigned wall_get(void);   /* wall.c: the wallpaper colour, 0x00RRGGBB (Settings changes it) */
int wall_paint(unsigned *fb, unsigned pitch, int w, int h, int swap);   /* arch/arm64/wall.c: the flat Navy wallpaper */
#define MENUBAR_H 26             /* docs/ARCHITECTURE.md: GUI_MENUBAR_H, on the 960x540 grid */
#define MENUBAR_RULE 0x00BDB8B0  /* docs/ARCHITECTURE.md: MENUBAR_RULE */
int text_init(void);   /* arch/arm64/text.c: the DejaVu faces through drivers/ttf.c */
int text_draw(int which, const char *s, int x, int baseline, int px10, unsigned fg, unsigned *fb, unsigned pitch, int w, int h);
int text_width(int which, const char *s, int px10);
int text_selftest(int px10, int *w, int *h, int *adv);
static int text_ok;     /* smooth text is up */
static unsigned fb_color(unsigned c) { return fb_swap ? (c & 0xFF00FF00u) | (c >> 16 & 0xFF) | (c & 0xFF) << 16 : c; }
/* Two windows share this renderer, each with its own text: the Console (the boot log, logs only, no input) and the
   Terminal (the ask> command line, docs/GUIDE.md). cp is the one in front; the other only keeps its text. */
struct pane {
    char log[LOG_MAX];
    unsigned len, col, row;
    unsigned anchor;          /* offset in the log of the line at the top of the window while it follows the newest output */
    int scrolled;             /* the keyboard moved the view back: new output is only logged until End */
    unsigned vtop, vlast;     /* while scrolled: the first line shown (0 based) and the last (1 based) */
};
static struct pane con_p, terminals[2], *cp = &con_p;
static unsigned term_index;
#define term_p terminals[term_index]
static int con_wx, con_wy, con_ww;
static int con_live;    /* the framebuffer is up: draw as we go */
static int calc_live, calc_front;   /* slice 5: the Calculator is open; calc_front, it is on top and has the keys, so the pane behind it only keeps its text */
static int app_front;               /* M3c part 2: the same for an EL0 app's window (never open with the Calculator) */
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
int ask_tui(void);   /* ask.c: the chat view (`sam` alone) has the Terminal window */
void ask_tui_scroll(unsigned code);
static int con_key(unsigned code, unsigned value) {   /* Page Up 104, Page Down 109, Home 102, End 107: 1 if the console took the key */
    if (code != 104 && code != 109 && code != 102 && code != 107) return 0;
    if (ask_tui()) { if (value) ask_tui_scroll(code); return 1; }   /* the chat view scrolls its own history, not the pane's log */
    if (!con_live || !value || calc_front || app_front) return 1;
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
    if (!con_live || p != cp || calc_front || app_front) return;   /* a window that is closed or behind only keeps its text */
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
static void con_place(int win_x, int win_y, int win_w, int win_h) {   /* where the grid sits and how many columns and rows fit the window, for the font already chosen */
    con_wx = win_x; con_wy = win_y; con_ww = win_w;
    con_x = win_x + sc(8); con_y = win_y + sc(32);
    con_cols = (win_w - sc(12)) / con_cw;
    con_rows = (win_h - sc(46)) / con_ch - 1;   /* then one row: the Console's wifi and usb status, or the Terminal's ask> prompt */
}
static void con_layout(int win_x, int win_y, int win_w, int win_h) {
    con_px10 = sc(133);
    mono_ok = text_ok && text_selftest(con_px10, &mono_w, &mono_h, &mono_adv);
    if (mono_ok) { con_vga = 0; con_cw = mono_adv; con_ch = sc(16); con_base = sc(12); }
    else {
        con_vga = ((int)fb_h * 10 / 600 + 5) / 10;
        if (con_vga < 1) con_vga = 1;
        con_cw = 8 * con_vga; con_ch = 16 * con_vga; con_base = 0;
    }
    con_place(win_x, win_y, win_w, win_h);
}
const char *ask_prompt(void);   /* ask.c: "joshua@tree % " */
/* The Terminal's bottom row: the prompt and the line being typed (ask.c), its tail when it is longer than the row, then a cursor. */
void con_clear(void) {         /* /clear: the Terminal's window empties, its text stays in the scrollback */
    if (!con_live || calc_front || app_front || ask_tui()) return;
    int held = cur_hold();
    con_wipe(); cp->anchor = cp->len; cp->scrolled = 0; con_hint();
    cur_release(held);
}
static void con_input_row(const char *p, const char *s, unsigned n) {   /* the bottom row: a prompt, the line being typed (its tail when long), a cursor */
    if (!con_live || calc_front || app_front || cp != &term_p) return;
    int held = cur_hold();
    unsigned plen = 0; while (p[plen]) plen++;
    unsigned row = (unsigned)con_rows, cols = (unsigned)con_cols, room = cols > plen + 2 ? cols - plen - 1 : 1;
    int y = con_y + (int)row * con_ch;
    fb_rect(con_x, y, con_cols * con_cw, con_ch, CON_BG);
    fb_flush(con_x - 4, y, con_cols * con_cw + 8, con_ch);
    unsigned col = 0;
    for (; *p; p++) con_glyph(col++, row, *p);
    if (n > room) { s += n - room; n = room; }
    for (unsigned i = 0; i < n; i++, col++) if (s[i] != ' ') con_glyph(col, row, s[i]);
    con_glyph(col, row, '_');
    cur_release(held);
}
void con_prompt(const char *s, unsigned n) { con_input_row(ask_prompt(), s, n); }
/* The chat view (ask.c, `sam` alone) takes the Terminal window over. The pane's log is not touched while it is up and is
   drawn again on the way out. Rows are kept in a shadow so a new line repaints only the rows that changed. */
#define TUI_ROWS 96
#define TUI_COLS 128
static char tui_shadow[TUI_ROWS][TUI_COLS];
static int tui_face_rows;   /* the chat view's face takes the top rows (con_tui_begin), 0 when the window is short or the view is closed */
int con_rows_total(void) { return con_live && fb && cp == &term_p && !calc_front && !app_front ? con_rows - tui_face_rows : 0; }   /* text rows above the input row; 0 with no window to draw in */
#include "face_art.h"
void tui_log(const char *s);   /* below: a line to the UART only */
int png_decode(const unsigned char *data, unsigned int len, unsigned char **out, unsigned int *w, unsigned int *h, unsigned int *channels);   /* drivers/png.c */
/* The chat view's face (5.0, "Samantha is the interface"): her portrait above the rows, from face_art.h (two frames of the
   web Samantha's face, mouth closed and mouth open), blended by how loud her voice is at that moment (ask.c voice_level,
   from the clip the sound path is playing), so her mouth opens with what she says. Seven rows when the window has room
   for them, fourteen in a tall one (21 rows or more), none in a short window. "face: on" on the UART when it is drawn; ask.c logs the mouth. */
static unsigned char *face_idle, *face_talk; static int face_tried, face_mouth = -1;
static void face_blit(int mouth) {
    if (!face_tried) {
        face_tried = 1; unsigned w, h, ch;
        if (png_decode(face_idle_png, FACE_IDLE_LEN, &face_idle, &w, &h, &ch) || w != FACE_PX || h != FACE_PX || ch != 4) face_idle = 0;
        if (png_decode(face_talk_png, FACE_TALK_LEN, &face_talk, &w, &h, &ch) || w != FACE_PX || h != FACE_PX || ch != 4) face_talk = 0;
    }
    int D = con_ch * (tui_face_rows - 1), x0 = con_x + (con_cols * con_cw - D) / 2, y0 = con_y + con_ch / 2;
    if (mouth < 0) mouth = 0;
    if (mouth > 100) mouth = 100;
    if (!face_idle) { fb_rect(x0, y0, D, D, CON_BG); fb_flush(x0, y0, D, D); face_mouth = mouth; return; }
    const unsigned char *talk = face_talk ? face_talk : face_idle;
    for (int y = 0; y < D; y++) for (int x = 0; x < D; x++) {   /* 2x2 samples a pixel, the two frames mixed by the mouth */
        unsigned r = 0, g = 0, b = 0;
        for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++) {
            unsigned o = (((y * 2 + j) * FACE_PX / (D * 2)) * FACE_PX + (x * 2 + i) * FACE_PX / (D * 2)) * 4;
            const unsigned char *p = face_idle + o, *q = talk + o;
            r += p[0] * (100 - mouth) + q[0] * mouth; g += p[1] * (100 - mouth) + q[1] * mouth; b += p[2] * (100 - mouth) + q[2] * mouth;
        }
        fb[(unsigned)(y0 + y) * fb_pitch + (unsigned)(x0 + x)] = fb_color(((r / 400) << 16) | ((g / 400) << 8) | (b / 400));
    }
    fb_flush(x0, y0, D, D);
    face_mouth = mouth;
}
void con_tui_mouth(int mouth) {   /* ask.c, while her voice plays: only the face is repainted */
    if (!tui_face_rows || !con_rows_total() || mouth == face_mouth) return;
    int held = cur_hold(); face_blit(mouth); cur_release(held);
}
void con_tui_begin(void) {
    if (!con_rows_total()) return;
    int held = cur_hold();
    tui_face_rows = con_rows < 14 ? 0 : con_rows >= 21 ? 14 : 7;   /* a tall window (the Samantha tile's, 4.20.0) gets a face twice as big */
    con_wipe();
    for (unsigned r = 0; r < TUI_ROWS; r++) tui_shadow[r][0] = 0;
    if (tui_face_rows) { face_mouth = -1; face_blit(0); tui_log(tui_face_rows > 7 ? "face: on, big\n" : "face: on\n"); }   /* the UART only, never the window */
    cur_release(held);
}
void con_tui_rows(const char *const *rows, unsigned n) {
    if (!con_rows_total()) return;
    int held = cur_hold();
    for (unsigned r = 0; r < n && r < TUI_ROWS && r < (unsigned)(con_rows - tui_face_rows); r++) {
        const char *t = rows[r] ? rows[r] : "";
        unsigned k = 0, lim = (unsigned)con_cols < TUI_COLS - 1 ? (unsigned)con_cols : TUI_COLS - 1;
        while (k < lim && t[k] && t[k] == tui_shadow[r][k]) k++;
        if (k == lim || (!t[k] && !tui_shadow[r][k])) continue;   /* the same text */
        int y = con_y + ((int)r + tui_face_rows) * con_ch;
        fb_rect(con_x, y, con_cols * con_cw, con_ch, CON_BG);
        fb_flush(con_x - 4, y, con_cols * con_cw + 8, con_ch);
        unsigned i = 0;
        for (; i < lim && t[i]; i++) { tui_shadow[r][i] = t[i]; if (t[i] != ' ') con_glyph(i, r + (unsigned)tui_face_rows, t[i]); }
        tui_shadow[r][i] = 0;
    }
    cur_release(held);
}
void con_tui_input(const char *p, const char *s, unsigned n) { con_input_row(p, s, n); }
int con_columns(void) { return con_live ? con_cols : 0; }
static void con_start(void) {   /* the screen is ready: replay what was printed before it */
#ifdef QUIET_BOOT
    return;   /* the log stays in the buffer with its window closed; pane_open draws every line when it is asked for */
#endif
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
        for (const char *p = crash_txt[r]; *p; p++) con_tx(*p);
        con_tx('\n');
    }
    if (fb) {
        fb_defer = fb_mute = 0;   /* a fault inside a batch: the crash screen goes out now */
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
/* Offscreen painting. On the Pi the framebuffer is the live scanout: a window repainted in place shows every half-drawn
   state (the cleared well, then the text going back) as flicker. off_begin copies the window's rectangle into a
   window-sized buffer and points fb at it, shifted so the same screen coordinates land inside it (fb_pitch becomes the
   window's width); the painters then draw as usual. off_end points fb back and copies the finished rectangle to the real
   screen row by row, top to bottom, in one pass. The buffer is one bump-heap block (2.3 MB for the 900x630 window at
   1080p), taken once at boot and kept; if the heap cannot give it, off_begin says 0 and the caller paints in place. */
static unsigned *off_buf, *off_real;
static unsigned off_cap, off_real_pitch, off_x, off_y, off_w, off_h;
static int clip_x0, clip_y0, clip_x1 = 1 << 30, clip_y1 = 1 << 30;   /* window_pixel_phys stays inside the buffer while it is the target */
static void off_copy_row(unsigned *d, const unsigned *s, unsigned n) {
    unsigned long *dl = (unsigned long *)d; const unsigned long *sl = (const unsigned long *)s;
    unsigned i = 0;
    if ((((unsigned long)d | (unsigned long)s) & 7) == 0) for (; i + 2 <= n; i += 2) *dl++ = *sl++;
    for (d += i, s += i; i < n; i++) *d++ = *s++;
}
static void off_reserve(int w, int h) { off_buf = kmalloc((unsigned)(w * h) * 4); off_cap = off_buf ? (unsigned)(w * h) : 0; }   /* once at boot, before any heap_mark: a mark's release would otherwise rewind over a block taken inside it */
static int off_begin(int x, int y, int w, int h) {   /* physical pixels */
    if (off_on || !fb) return 0;
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > (int)fb_w || y + h > (int)fb_h) return 0;
    if (!off_buf || (unsigned)(w * h) > off_cap) return 0;
    off_x = (unsigned)x; off_y = (unsigned)y; off_w = (unsigned)w; off_h = (unsigned)h;
    for (unsigned j = 0; j < off_h; j++) off_copy_row(off_buf + j * off_w, fb + (off_y + j) * fb_pitch + off_x, off_w);
    off_real = fb; off_real_pitch = fb_pitch;
    fb = (unsigned *)((unsigned long)off_buf - ((unsigned long)off_y * off_w + off_x) * 4);
    fb_pitch = off_w;
    clip_x0 = x; clip_y0 = y; clip_x1 = x + w; clip_y1 = y + h;
    off_on = 1;
    return 1;
}
static void off_end(void) {
    if (!off_on) return;
    fb = off_real; fb_pitch = off_real_pitch; off_on = 0;
    clip_x0 = clip_y0 = 0; clip_x1 = clip_y1 = 1 << 30;
    for (unsigned j = 0; j < off_h; j++) off_copy_row(fb + (off_y + j) * fb_pitch + off_x, off_buf + j * off_w, off_w);
    fb_flush((int)off_x, (int)off_y, (int)off_w, (int)off_h);
}
static unsigned long off_screen_hash(int x, int y, int w, int h) {   /* FNV-1a over a rectangle of the REAL screen, whichever buffer fb points at (the trace build's proof) */
    unsigned *r = off_on ? off_real : fb; unsigned p = off_on ? off_real_pitch : fb_pitch; unsigned long v = 1469598103934665603UL;
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) v = (v ^ r[(unsigned)j * p + (unsigned)i]) * 1099511628211UL;
    return v;
}
void window_pixel_phys(int px, int py, unsigned int color) {
    if (px >= clip_x0 && py >= clip_y0 && px < clip_x1 && py < clip_y1 && px >= 0 && py >= 0 && px < (int)fb_w && py < (int)fb_h) fb[(unsigned)py * fb_pitch + (unsigned)px] = fb_color(color);
}
unsigned int window_get_pixel_phys(int px, int py) {
    return px >= clip_x0 && py >= clip_y0 && px < clip_x1 && py < clip_y1 && px >= 0 && py >= 0 && px < (int)fb_w && py < (int)fb_h ? fb_color(fb[(unsigned)py * fb_pitch + (unsigned)px]) : 0;
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
/* The icon text (window labels): bold sans, face 0..3 a 16, 20, 24 or 28 physical pixel face times
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
   tree from kernel/boot_mark.h, box-filtered to T physical pixels and blended in ink over what is there, its ground
   (menu_mark_acc, 3.0) in terracotta. Drawn bigger than stored (the boot screen, 220 px at 1080p), it blends the four
   nearest stored pixels instead, so the curves stay smooth rather than stepping in 2 to 3 pixel blocks. */
static unsigned mark_lerp_n(const unsigned char *m, int n, int x0, int y0, int wx, int wy) {
    int x1 = x0 + 1 < n ? x0 + 1 : x0, y1 = y0 + 1 < n ? y0 + 1 : y0;
    return (unsigned)(((m[y0 * n + x0] * (256 - wx) + m[y0 * n + x1] * wx) * (256 - wy)
                     + (m[y1 * n + x0] * (256 - wx) + m[y1 * n + x1] * wx) * wy) >> 16);
}
static unsigned mark_lerp(const unsigned char *m, int x0, int y0, int wx, int wy) { return mark_lerp_n(m, MENU_MARK_W, x0, y0, wx, wy); }
#ifdef QUIET_BOOT
/* The splash's mark: the 160 px stored copy (the menu's 80 px one, stretched 2.75x, was the smudge on the Pi), ink over a
   known flat ground written straight to the screen, so nothing depends on what was there before. */
static void splash_mark(int cx, int cy, int T, unsigned ink, unsigned bg) {
    int ox = cx - T / 2, oy = cy - T / 2, n = BOOT_MARK_W;
    for (int row = 0; row < T; row++) for (int col = 0; col < T; col++) {
        int fx = (2 * col + 1) * n * 128 / T - 128, fy = (2 * row + 1) * n * 128 / T - 128;
        if (fx < 0) fx = 0;
        if (fy < 0) fy = 0;
        unsigned a = mark_lerp_n(boot_mark_cov, n, fx >> 8, fy >> 8, fx & 255, fy & 255);
        if (!a) continue;
        unsigned out = 0;
        for (int sh = 0; sh <= 16; sh += 8) out |= ((((ink >> sh) & 0xFF) * a + ((bg >> sh) & 0xFF) * (255 - a)) / 255) << sh;
        fb_rect(ox + col, oy + row, 1, 1, out);
    }
    fb_flush(ox, oy, T, T);   /* out of the data cache to RAM: the GPU reads RAM, so unflushed rows reached the Pi screen as torn slivers */
}
#endif
static void menu_mark_paint(int cx, int cy, int T, unsigned ink) {
    int ox = cx - T / 2, oy = cy - T / 2;
    for (int row = 0; row < T; row++) for (int col = 0; col < T; col++) {
        unsigned a, g;
        if (T > MENU_MARK_W) {   /* sample point in 1/256 stored pixels, centre to centre */
            int fx = (2 * col + 1) * MENU_MARK_W * 128 / T - 128, fy = (2 * row + 1) * MENU_MARK_H * 128 / T - 128;
            if (fx < 0) fx = 0;
            if (fy < 0) fy = 0;
            a = mark_lerp(menu_mark_cov, fx >> 8, fy >> 8, fx & 255, fy & 255);
            g = mark_lerp(menu_mark_acc, fx >> 8, fy >> 8, fx & 255, fy & 255);
        } else {
            int c0 = col * MENU_MARK_W / T, c1 = (col + 1) * MENU_MARK_W / T, r0 = row * MENU_MARK_H / T, r1 = (row + 1) * MENU_MARK_H / T;
            if (c1 == c0) c1 = c0 + 1;
            if (r1 == r0) r1 = r0 + 1;
            int sum = 0, gsum = 0, n = (c1 - c0) * (r1 - r0);
            for (int y = r0; y < r1; y++) for (int x = c0; x < c1; x++) { sum += menu_mark_cov[y * MENU_MARK_W + x]; gsum += menu_mark_acc[y * MENU_MARK_W + x]; }
            a = (unsigned)(sum / n); g = (unsigned)(gsum / n);
        }
        if (!a) continue;
        unsigned d = window_get_pixel_phys(ox + col, oy + row), out = 0;
        for (int sh = 0; sh <= 16; sh += 8)
            out |= ((((ink >> sh) & 0xFF) * (a - g) + ((MARK_GROUND >> sh) & 0xFF) * g + ((d >> sh) & 0xFF) * (255 - a)) / 255) << sh;
        window_pixel_phys(ox + col, oy + row, out);
    }
}
/* The live Calendar tile shows a month and day once network time is available. */
unsigned long net_clock_utc(void);   /* ip.c: UTC seconds once the network has told us, 0 before */
static unsigned long civil_days(long y, unsigned m, unsigned d);
static void clock_local(unsigned long utc, unsigned *hh, unsigned *mm, unsigned *mon, unsigned *day);
static long clock_year;   /* the local year of the last clock_local call */
#ifdef CAL_TEST_UTC
static unsigned long cal_utc(void) { return CAL_TEST_UTC; }   /* the caltest build: a fixed date for arm64-calicon-check.py */
#elif defined(MUSIC_TEST_UTC)
static unsigned long cal_utc(void) { return MUSIC_TEST_UTC + cnt_now() / cnt_hz(); }   /* the musictest build: a clock that ticks, which paces Music's `music: at` lines (arm64-music-check.py) */
#else
static unsigned long cal_utc(void) { return net_clock_utc(); }
#endif
static long clock_offset(unsigned long utc);   /* seconds to subtract from UTC for Vancouver wall time (the one time-zone path) */
static unsigned long cal_local(void) { unsigned long u = cal_utc(); return u ? u - (unsigned long)clock_offset(u) : 0; }   /* 0 stays 0: no clock yet */
static int cal_shown = -1;   /* the day the tile shows, as a y/m/d number; -1 is the plain art */
static int cal_today(void) {   /* today's y/m/d number, or -1 with no clock */
    unsigned long u = cal_utc(); if (!u) return -1;
    unsigned h, m, mo, dd; clock_local(u, &h, &m, &mo, &dd);
    return (int)(clock_year * 12 + (long)mo - 1) * 32 + (int)dd;
}
static void cal_overlay(unsigned *tile, int pw, int today) {
    static const char *const months[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    unsigned mo = (unsigned)(today / 32 % 12), d = (unsigned)(today % 32);
    char day[3]; int n = 0;
    if (d >= 10) day[n++] = '0' + d / 10;
    day[n++] = '0' + d % 10; day[n] = 0;
    for (int y = pw * 49 / 128; y < pw * 103 / 128; y++)
        for (int x = pw * 24 / 128; x < pw * 104 / 128; x++) tile[y * pw + x] = 0x00F3F3F6;
    int month_size = pw * 16 / 10, day_size = pw * 38 / 10;
    text_draw(1, months[mo], (pw - text_width(1, months[mo], month_size)) / 2,
              pw * 44 / 128, month_size, 0x00FFFFFF, tile, pw, pw, pw);
    text_draw(1, day, (pw - text_width(1, day, day_size)) / 2,
              pw * 96 / 128, day_size, 0x001C1C1E, tile, pw, pw, pw);
}
static int dock_tile(int slot, int shadow) {   /* one slot's art onto the dock; shadow 0 repaints the tile alone, its corners keep the old shadow */
    static const int order[GUI_ICON_COUNT] = GUI_DOCK_DEFAULT_ORDER;
    int size = DOCK_ICON, pw = size * (int)window_scale(), drawn = 0;
    int icon = order[slot], cx = gui_slot_x(slot) + size / 2, cy_bottom = gui_dock_y0() + DOCK_PAD + size;
    if (shadow) gui_draw_icon_shadow(cx, cy_bottom, size);
    unsigned long mark = heap_mark();   /* the bump heap never frees: the decode and the tile are rolled back */
    unsigned int *tile = kmalloc((unsigned)(pw * pw) * 4);
    unsigned char *art = 0;
    unsigned aw = 0, ah = 0, ach = 0;
    if (tile && png_decode(ICON_ART[icon], ICON_ART_LEN[icon], &art, &aw, &ah, &ach) == 0 && art && aw == ICON_ART_SIZE && ah == ICON_ART_SIZE && ach == 4) {
        gui_icon_art_scale(art, tile, pw, DOCK_TRAY_COLOR);
        if (icon == 2) { cal_shown = cal_today(); if (cal_shown >= 0) cal_overlay(tile, pw, cal_shown); }   /* ICON_ART[2], Calendar */
        gui_blit_tile(tile, cx - size / 2, cy_bottom - size, size, DOCK_TRAY_COLOR);
        drawn = 1;
    }
    heap_release(mark);
    return drawn;
}
static void dock_paint(void) {
    gui_draw_dock_tray();
    int drawn = 0;
    for (int slot = 0; slot < GUI_ICON_COUNT; slot++) drawn += dock_tile(slot, 1);
    uart_puts("M1d dock "); uart_dec((unsigned)drawn); uart_puts(" icons\n");
}
/* Slice 3: the hover label, the i386 one (gui_draw_dock_label). The first hover keeps a copy of the dock's band, so
   moving to another slot or to none puts the plain band back before the next label; nothing is allocated until then.
   The pointer drives it (slice 4, below); the dockhover test build calls it once at boot. Names follow APPS[] in
   kernel/kernel.c for GUI_DOCK_DEFAULT_ORDER (arm64-m1c-check.py compares them). */
static const char *const dock_names[GUI_ICON_COUNT] = {"Apps", "Burrow", "Mail", "Calendar", "Notes", "Reminders", "Terminal", "Samantha", "Weather", "Stocks", "Trash"};
static unsigned *dock_band, *dock_mem;   /* dock_mem is the band copy's memory, taken at boot like the Browser's buffer so no mark's release can rewind over it */
static int cur_on;           /* slice 4: the pointer's arrow is on the screen */
static void cur_hide(void);
static void cur_show(void);
void dock_hover(int slot) {
    int s = (int)window_scale(), top = gui_dock_band_top() * s, rows = (int)fb_h - top;
    int arrow = cur_on;
    cur_hide();   /* the band copy must never hold the arrow, or every later restore paints a ghost of it */
    if (!dock_band) {
        unsigned long mark = heap_mark();
        dock_band = dock_mem ? dock_mem : kmalloc((unsigned)rows * fb_pitch * 4);
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
static int hover_slot = -1;
static void cur_flush(int lx, int ly) { int s = (int)window_scale(); fb_flush(lx * s, ly * s, CURSOR_W * s, CURSOR_H * s); }
static void cur_hide(void) {
    if (!cur_on) return;
    int x = cursor_saved_x, y = cursor_saved_y;
    gui_cursor_restore();
    cur_flush(x, y);
    cur_on = 0;
}
static int cur_frozen;   /* 4.3.0: win_set is putting several windows back: the arrow waits for the end, or its saved pixels go stale */
static void cur_show(void) {
    if (cur_on || !cur_wanted || !fb || cur_frozen) return;
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
static void con_fit(void) {   /* a resized Console or Terminal: the grid for the rectangle as it is now, kept inside the frame's white well */
    int s = (int)window_scale();
    con_place(win_lx * s, win_ly * s, win_lw * s, win_lh * s);
    int wx = (win_lx + 8) * s, wy = (win_ly + 30) * s + 2 * s, wr = (win_lx + win_lw - 8) * s, wb = (win_ly + win_lh - 8) * s;
    if (con_x < wx) con_x = wx;
    if (con_y < wy) con_y = wy;
    if (con_x + con_cols * con_cw > wr) con_cols = (wr - con_x) / con_cw;
    if (con_y + (con_rows + 1) * con_ch > wb) con_rows = (wb - con_y) / con_ch - 1;
}
static void console_frame(void) {   /* the i386 window frame, and a white well for the text */
    int s = (int)window_scale();
    gui_draw_window_frame(win_lx, win_ly, win_lw, win_lh, cp == &term_p ? (term_index ? "Terminal 2" : "Terminal 1") : "Console");
    fb_rect((win_lx + 8) * s, (win_ly + 30) * s, (win_lw - 16) * s, (win_lh - 38) * s, CON_BG);
}
static int term_front(void) { return cp == &term_p && !calc_front && !app_front && (con_live || !fb); }   /* with no screen at all, the UART is the Terminal */
/* The red close button puts the wallpaper back where the window was. The log keeps every line (and the UART still
   prints them); a click on any dock tile opens it again with the newest lines, so the one debug view on a Pi can
   never be lost for good. The Terminal (its tile, or F1) takes the same rectangle: one of the two is in front. With
   the Calculator open behind it, the Calculator comes to the front. */
static void calc_raise(void);
static void app_raise(void);
static int calc_lx, calc_ly, calc_lw, calc_lh;   /* its frame on the logical grid, set in fb_init */
static void wall_fill(int x, int y, int w, int h);
static void tribute_paint(void) {   /* the line over the dock, drawn on the flat wallpaper at boot and again after a window leaves near it */
    int W = (int)fb_w, H = (int)fb_h, band_y = gui_dock_band_top() * (int)window_scale();
    /* Steve Jobs died on 5 October 2011. Fifteen years on, one quiet line above the dock, ending on the title
       of the Steve Jobs Archive's book of his own words, which Joshua was reading that week. */
    const char *thanks = "Seduce. Vilify. Ignore.";
    int ar = sc(6), gapx = sc(7), tw = text_width(2, thanks, sc(110)), tx = (W - tw - 7 * ar / 2 - gapx) / 2, ty = band_y - sc(4);
    {   int bx = tx - 4, by = ty - sc(4) + ar - 7 * ar / 2 - 2;   /* the line's whole box back to flat wallpaper first: a repaint over half of it would blend the edges twice */
        wall_fill(bx, by, tx + tw + gapx + 7 * ar / 2 + 4 - bx, band_y + sc(6) - by);   /* down past the descenders: the line's own pixels, nothing of the hover band's label capsule above the tiles */
    }
    text_draw(2, thanks, tx, ty, sc(110), fb_color(0x00f0f4f8), fb, fb_pitch, W, H);
    {   /* a small bitten apple after the line: apple_mask scaled to 3.5 ar square, 4x4 samples a pixel, onto the flat fill */
        int S = 7 * ar / 2, x0 = tx + tw + gapx, y0 = ty - sc(4) + ar - S; unsigned ink = fb_color(0x00f0f4f8);
        for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
            int sum = 0;
            for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) sum += apple_mask[((y * 4 + j) * APPLE_N / (S * 4)) * APPLE_N + (x * 4 + i) * APPLE_N / (S * 4)];
            unsigned al = (unsigned)sum / 16, d = fb[(unsigned)(y0 + y) * fb_pitch + (unsigned)(x0 + x)], out = 0;
            if (!al) continue;
            for (int b = 0; b <= 16; b += 8) out |= ((((ink >> b) & 0xFF) * al + ((d >> b) & 0xFF) * (255 - al)) / 255) << b;
            fb[(unsigned)(y0 + y) * fb_pitch + (unsigned)(x0 + x)] = out;
        }
    }
}
static void wall_fill(int x, int y, int w, int h) {   /* physical px: the flat wallpaper colour (wall.c); there is no picture to save, so a window can leave from anywhere */
    unsigned c = fb_color(wall_get());
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) fb[(unsigned)(y + j) * fb_pitch + (unsigned)(x + i)] = c;
}
static void wall_minus(int ox, int oy, int ow, int oh, int nx, int ny, int nw, int nh) {   /* physical px: wallpaper over the old rectangle except where the new one is */
    for (int j = 0; j < oh; j++) {
        int y = oy + j, inside = y >= ny && y < ny + nh, a = ox, b = ox + ow;
        if (inside) {   /* the row meets the new rectangle: keep its span, fill the left and right remainders */
            int l = nx < b ? (nx > a ? nx : a) : b, r = nx + nw > a ? (nx + nw < b ? nx + nw : b) : a;
            wall_fill(a, y, l - a, 1); wall_fill(r, y, b - r, 1);
        } else wall_fill(a, y, b - a, 1);
    }
}
/* A window leaves by wall_vacate: the wallpaper over its frame AND a ring of VACATE_PAD logical px around it. Content may
   overhang its frame (Brick's playfield is 6 px wider than the frame at 1080p and its hint line runs 5 px below it), and
   the old closes cleared the frame alone, leaving the overhang as a thin white bar and glyph bits (board photo, 3.6.3).
   The ring stops under the menu bar and above the tribute line, where nothing but the wallpaper is ever under a window. */
#define VACATE_PAD 6
static void wall_vacate(int lx, int ly, int lw, int lh, int ring_only) {
    int s = (int)window_scale(), x0 = (lx - VACATE_PAD) * s, y0 = (ly - VACATE_PAD) * s, x1 = (lx + lw + VACATE_PAD) * s, y1 = (ly + lh + VACATE_PAD) * s;
    int top = sg(MENUBAR_H), bottom = (gui_dock_band_top() - 12) * s;
    if (x0 < 0) x0 = 0;
    if (x1 > (int)fb_w) x1 = (int)fb_w;
    if (y0 < top) y0 = top;
    if (y1 > bottom) y1 = bottom > ly * s + lh * s ? bottom : ly * s + lh * s;   /* never less than the frame itself */
    int fx = lx * s, fy = ly * s, fw = lw * s, fh = lh * s;
    if (!ring_only) wall_fill(x0, y0, x1 - x0, y1 - y0);
    else {   /* a window being dragged keeps its frame: only the ring goes */
        if (fy > y0) wall_fill(x0, y0, x1 - x0, fy - y0);
        if (y1 > fy + fh) wall_fill(x0, fy + fh, x1 - x0, y1 - fy - fh);
        if (fx > x0) wall_fill(x0, fy, fx - x0, fh);
        if (x1 > fx + fw) wall_fill(fx + fw, fy, x1 - fx - fw, fh);
    }
    fb_flush(x0, y0, x1 - x0, y1 - y0);
    if (text_ok && y1 > (gui_dock_band_top() - 40) * s) { tribute_paint(); fb_flush(0, (gui_dock_band_top() - 40) * s, (int)fb_w, 40 * s); }   /* the ring may have cut into the line over the dock */
}
static void console_close(void) {
    if (!con_live || !fb) return;
    cur_hide();
    wall_vacate(win_lx, win_ly, win_lw, win_lh, 0);
    con_live = 0;
    uart_puts(cp == &term_p ? "terminal closed\n" : "console closed\n");
    cur_show();
    if (calc_live) { wall_vacate(calc_lx, calc_ly, calc_lw, calc_lh, 0); calc_raise(); }   /* the wallpaper just went back over the part of it the window covered; the window behind is wiped whole and drawn again, so its anti-aliased edge is not blended over its own old one */
    if (app_wbytes) { wall_vacate(app_lx, app_ly, app_lw, app_lh, 0); app_raise(); }   /* the same for an app's window */
}
void ask_redraw(void);   /* ask.c: the line being typed, back on the ask> row */
static void pane_draw(void) {   /* the frame, the well and the newest lines of cp, in the rectangle as it is now; the caller hides the arrow */
    int s = (int)window_scale();
    console_frame();
    con_live = 1; cp->scrolled = 0;
    unsigned nl = con_nlines(), keep = con_rows > 1 ? (unsigned)con_rows - 1 : 1;
    cp->anchor = con_line_off(nl > keep ? nl - keep : 0);
    con_wipe();
    for (unsigned k = cp->anchor; k < cp->len; k++) con_draw(k);
    con_summary(); con_hint(); ask_redraw();
    fb_flush(win_lx * s, win_ly * s, win_lw * s, win_lh * s);
}
static void pane_open(struct pane *p) {   /* the Console or the Terminal in front, its newest lines in view */
    int raise = con_live && cp == p;   /* already open, behind the Calculator: bring it up and give it the keys */
    if (raise && !calc_front && !app_front) return;
    if (!fb) { cp = p; uart_puts(p == &term_p ? "terminal open\n" : "console open\n"); return; }   /* no screen: the keys still follow */
    cur_hide();
    calc_front = app_front = 0;   /* before any drawing: the pane's own draws are off while the Calculator or an app is on top */
    cp = p;
    pane_draw();
    if (raise) { con_quiet = 1; uart_puts(p == &term_p ? "focus terminal\n" : "focus console\n"); }
    else uart_puts(p == &term_p ? "terminal open\n" : "console open\n");
    cur_show();
}
static void console_open(void) { pane_open(&con_p); }
void con_tui_end(void) {   /* back to the shell: the Terminal's own text and prompt */
    if (!con_rows_total()) return;
    tui_face_rows = 0; face_mouth = -1;
    int held = cur_hold(), sc_ = (int)window_scale();
    cp->scrolled = 0;
    unsigned nl = con_nlines(), keep = con_rows > 1 ? (unsigned)con_rows - 1 : 1;
    cp->anchor = con_line_off(nl > keep ? nl - keep : 0);
    con_wipe();
    for (unsigned k = cp->anchor; k < cp->len; k++) con_draw(k);
    con_summary(); con_hint(); ask_redraw();
    fb_flush(win_lx * sc_, win_ly * sc_, win_lw * sc_, win_lh * sc_);
    cur_release(held);
}

/* The Calculator (calc.c holds the sums and the keypad; this draws it). Slice 5: it is a second window, with its own
   rectangle overlapping the right of the Console's, so the Console or the Terminal stays on screen beside it. The one
   in front has the keys, as on the i386 desktop: a click on the window behind brings it up and gives it the keys
   (calc_front says which). The window behind keeps its text but draws nothing until it is in front again. The Apps
   tile opens it; a click on a key or typing works it; Esc or the red dot closes it and puts the wallpaper back. */
int calc_keys(int *cols);
const char *calc_key(int i);
int calc_press(const char *k);
int calc_type(int ch);
const char *calc_input(void);
const char *calc_output(void);
int calc_flags(void);   /* bit 0 scientific, bit 1 degrees, bit 2 memory held */
static int clock_live, brick_live, spot_live, browser_live, settings_live;
#include "accounts.h"
#include "login.h"
#include "sudo.h"
static int login_live;   /* login_ui.h: the login screen covers everything; the bar, the notices and the clock stand back */
static void set_paint(void);
static void settings_open(int sec);
static void settings_close(void);
static int settings_key(unsigned code);
static void settings_click(int lx, int ly);
static int menubar_click(int x, int y);   /* 1 when the click landed on one of the status icons, which opened Settings */
/* The tree menu (the mark and "Joshua Tree" at the left of the menu bar) is a second card on Spotlight's machinery: the same
   spot_live, declared above (so every window, drag, app frame and click that stands back for the bar stands back for it), the same saved
   pixels, the same rows of SPOT_ROW and terracotta selection. spot_tree says which card is up. */
#define TREE_W 220
#define TREE_H 112
static int spot_tree;
static void tree_open(void);
static void tree_key(unsigned code);
static void tree_click(int lx, int ly);
static void tree_hover(int lx, int ly);
static int tree_bar_hit(void);
static void browser_open(void);
static void browser_close(void);
static int browser_key(unsigned code);
static void browser_click(int lx, int ly);
static void browser_wheel(int v);
static int wheel_acc;   /* wheel notches since the last report ended */
static void brick_open(void);
static void brick_close(void);
static int brick_key(unsigned code, unsigned value);
static void brick_mouse(int lx, int ly);
static void brick_tick(void);
void menubar_notify(const char *message);
static void clock_open(void);
static void clock_close(void);
#define CALC_TOP 36      /* the display's top, under the title band */
#define CALC_DISP 46     /* the display's height */
static void calc_cell(int i, int *x, int *y, int *w, int *h) {   /* key i's box on the logical grid */
    int cols, n = calc_keys(&cols), rows = (n + cols - 1) / cols, gap = 4;
    int ax = calc_lx + 12, ay = calc_ly + CALC_TOP + CALC_DISP + 8, aw = calc_lw - 24, ah = calc_ly + calc_lh - 10 - ay;
    *w = (aw - gap * (cols - 1)) / cols; *h = (ah - gap * (rows - 1)) / rows;
    *x = ax + (i % cols) * (*w + gap); *y = ay + (i / cols) * (*h + gap);
}
static void calc_paint(void) {
    int s = (int)window_scale(), x0 = calc_lx + 12, w0 = calc_lw - 24, y0 = calc_ly + CALC_TOP, f = calc_flags();
    cur_hide();
    gui_draw_window_frame(calc_lx, calc_ly, calc_lw, calc_lh, "Calculator");
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
    fb_flush(calc_lx * s, calc_ly * s, calc_lw * s, calc_lh * s);
    cur_show();
}
static void calc_raise(void) {   /* in front of the Console or the Terminal, with the keys */
    if (!calc_live) return;
    int was = calc_front;
    calc_front = 1;
    calc_paint();
    if (!was) { con_quiet = 1; uart_puts("focus calc\n"); }
}
static void calc_open(void) {
    settings_close();
    brick_close();
    clock_close();
    browser_close();
    app_kill("closed for the Calculator");   /* the two never share the screen, so there is no third window to stack */
    if (calc_live) { calc_raise(); return; }
    calc_live = calc_front = 1;
    calc_paint();
    uart_puts("calc open\n");
}
static void calc_close(void) {
    if (!calc_live) return;
    cur_hide();
    wall_vacate(calc_lx, calc_ly, calc_lw, calc_lh, 0);
    calc_live = calc_front = 0;
    uart_puts("calc closed\n");
    if (con_live) { wall_vacate(win_lx, win_ly, win_lw, win_lh, 0); con_live = 0; pane_open(cp); }   /* the wallpaper went over its corner too: draw it whole, in front, on fresh wallpaper */
    cur_show();
}
static void calc_did(int ran) {   /* after a key: redraw, and log a finished sum on the UART (what arm64-calc-check.py reads) */
    calc_paint();
    if (ran) { con_quiet = 1; uart_puts("calc: "); uart_puts(calc_input()); uart_puts(" = "); uart_puts(calc_output()); uart_putc('\n'); }
}
/* ---- M3c part 2: an EL0 app's window. The Calculator's frame and title (the app's name), placed like it over the right of
   the Console, but as tall as fits between the menu bar and the dock: calendar.c's month grid needs about 320 rows. The app
   draws into its own buffer, app_cw by app_ch pixels at UP_WIN in its address space, and JT_SYS_WINDOW_POLL with
   JT_POLL_PRESENT copies it into the window's content (8, 32 inside the frame, as on i386), each pixel s by s. The window in
   front takes the keys and clicks, which the desktop queues as JT_EV_KEY and JT_EV_CLICK (content coordinates); the red dot
   is i386's close, a click above the content that the app answers by exiting, and an app that ignores it is ended a quarter
   second later. Closing puts back the wallpaper saved at boot and redraws the Console or
   the Terminal whole. Opening one closes the Calculator, and opening the Calculator ends the app. ---- */
static void spot_close(void);
static unsigned mix31(unsigned a, unsigned b) {   /* three parts a, one part b, in each colour lane */
    return ((((a & 0xFF00FFu) * 3 + (b & 0xFF00FFu)) >> 2) & 0xFF00FFu) | ((((a & 0x00FF00u) * 3 + (b & 0x00FF00u)) >> 2) & 0x00FF00u);
}
static void app_blit(void) {
    int s = (int)window_scale(), x0 = (app_lx + 8) * s, y0 = (app_ly + 32) * s;
    const unsigned *src = (const unsigned *)(UB + UP_WIN);
    unsigned bw = app_bw, cw_ = app_bw < app_cw ? app_bw : app_cw, ch_ = app_bh < app_ch ? app_bh : app_ch;   /* while a resize is in flight the buffer is the old size: its pixels go where they fit */
    if (app_hi) {   /* 3.9.0: the app drew at device size (hires.h and JT_SYS_TEXT2): one buffer pixel per screen pixel, nothing to smooth */
        unsigned hw = bw * 2;
        for (unsigned y = 0; y < ch_ * 2; y++) {
            const unsigned *r = src + y * hw; unsigned *d = fb + (unsigned)(y0 + (int)y) * fb_pitch + (unsigned)x0;
            for (unsigned x = 0; x < cw_ * 2; x++) d[x] = fb_color(r[x] & 0x00FFFFFFu);
        }
        return;
    }
    if (s == 2) {   /* an app that did not ask: the apps draw on a 1x grid: spread each pixel over its 2x2 by bilinear weights (3/4 itself, 1/4 the near neighbour), so a stem is a soft ramp, not a 2x2 block */
        int cw = (int)cw_, ch = (int)ch_;
        for (int y = 0; y < ch; y++) for (int j = 0; j < 2; j++) {
            const unsigned *r0 = src + (unsigned)y * bw, *r1 = src + (unsigned)(j ? (y + 1 < ch ? y + 1 : y) : (y ? y - 1 : y)) * bw;
            unsigned *d = fb + (unsigned)(y0 + y * 2 + j) * fb_pitch + (unsigned)x0;
            for (int x = 0; x < cw; x++) for (int i = 0; i < 2; i++) {
                int xn = i ? (x + 1 < cw ? x + 1 : x) : (x ? x - 1 : x);
                unsigned a = mix31(r0[x] & 0x00FFFFFFu, r0[xn] & 0x00FFFFFFu), b = mix31(r1[x] & 0x00FFFFFFu, r1[xn] & 0x00FFFFFFu);
                d[x * 2 + i] = fb_color(mix31(a, b));
            }
        }
        return;
    }
    for (unsigned y = 0; y < ch_; y++) for (unsigned x = 0; x < cw_; x++) {
        unsigned c = fb_color(src[y * bw + x] & 0x00FFFFFFu), *d = fb + (unsigned)(y0 + (int)y * s) * fb_pitch + (unsigned)(x0 + (int)x * s);
        for (int j = 0; j < s; j++) for (int i = 0; i < s; i++) d[(unsigned)j * fb_pitch + (unsigned)i] = c;
    }
}
static void app_paint(void) {   /* the frame, then the last frame the app presented */
    char t[16]; unsigned n = 0;
    for (const char *p = app_cur ? app_cur->name : "app"; *p && n < sizeof t - 1; p++, n++) t[n] = n ? *p : (char)(*p & ~32);   /* "calendar" is titled Calendar */
    t[n] = 0;
    if (n == 8 && t[0] == 'H' && t[7] == 'i') t[6] = 'p';   /* hamurabi is shown as Hamurapi (the store name Hamurabi was taken) */
    int s = (int)window_scale();
    fb_hold();   /* the frame, the content and the arrow reach the screen as one copy */
    cur_hide();
    gui_draw_window_frame(app_lx, app_ly, app_lw, app_lh, t);
    if (app_shown) app_blit();
    fb_flush(app_lx * s, app_ly * s, app_lw * s, app_lh * s);
    cur_show();
    fb_release();
}
static void app_repaint_all(void) {   /* every app window, back to front: what a moved, raised or closed window uncovered comes back whole */
    struct app *keep = ap;
    for (int z = (int)app_zn - 1; z >= 0; z--) { ap = &apps[app_z[z]]; app_paint(); }
    ap = keep;
}
static void app_raise(void) {   /* this app's window in front of the Console, the Terminal and the other apps, with the keys */
    if (!app_wbytes) return;
    int was = app_front && app_top == (int)ap->slot;
    app_front = 1;
    app_z_front(ap->slot);
    if (was) app_paint(); else app_repaint_all();
    if (!was) { con_quiet = 1; uart_puts("focus app\n"); }
}
static void app_push(unsigned kind, int a, int b) {   /* one event for the app; a full ring drops the newest, as on i386 */
    if (!app_cur || app_qt - app_qh >= APP_EVQ) return;
    app_q[app_qt % APP_EVQ].kind = kind; app_q[app_qt % APP_EVQ].a = a; app_q[app_qt % APP_EVQ].b = b;
    app_qt++; app_kick = 1;   /* it runs on this pass, not 10 ms from now */
}
static void st32(unsigned long a, unsigned v) { for (int i = 0; i < 4; i++) ((unsigned char *)a)[i] = (unsigned char)(v >> (8 * i)); }   /* any alignment: the pointer is the program's */
/* JT_SYS_WINDOW_OPEN: only for an app the scheduler runs (the boot self-test gets -ENODEV, as i386 answers a program run
   from the text shell). The first call maps the buffer, zeroed, EL0 read-write and never executable, and puts the window up
   in front; any call fills the LP64 struct jt_window_info: width, height, pitch, then the 64-bit pixels pointer at +16. */
static long app_window_open(unsigned long info, unsigned long flags) {
    if (!app_running || !fb || !app_cw) return -19;
    if (!user_writable(info, 24)) return -14;
    if (!app_wbytes || app_cw != app_bw || app_ch != app_bh) {   /* the first open, or the answer to JT_EV_RESIZE: a buffer of the content's size now, zeroed */
        const unsigned long PAGE = 0x3UL | (1UL << 2) | (3UL << 8) | (1UL << 10), AP_EL0_RW = 1UL << 6, PXN = 1UL << 53, UXN = 1UL << 54;
        int hi = ((flags & 1) || (app_wbytes && app_hi)) && window_scale() == 2;   /* hires.h's ask: only at scale 2, where there are device pixels to give */
        unsigned long bytes = ((unsigned long)app_cw * app_ch * (hi ? 16 : 4) + 4095) & ~4095UL;
        if (bytes > UWIN_MAX) return -12;
        for (unsigned long i = 0; i < bytes / 4096; i++) l3[ap->l3off + (UP_WIN >> 12) + i] = (UB + UP_WIN + i * 4096) | PAGE | AP_EL0_RW | PXN | UXN;
        __asm__ volatile ("dsb ishst\n tlbi vmalle1\n dsb ish\n isb" ::: "memory");
        for (unsigned long i = 0; i < bytes / 8; i++) ((unsigned long *)(UB + UP_WIN))[i] = 0;
        int first = !app_wbytes;
        app_wbytes = bytes; app_shown = 0; app_bw = app_cw; app_bh = app_ch; app_hi = hi;
        if (first) { spot_close(); calc_close(); browser_close(); settings_close(); app_front = 1; app_z_front(ap->slot); app_rs_log = 0; }
        else app_rs_log = 1;
        if (app_front && first) app_paint();   /* 4.3.0: a resize's answer keeps the frame win_set drew (old pixels where they fit) until the redrawn frame is presented, so no empty frame flashes */
        con_quiet = !first; uart_puts("M3c window "); uart_puts(app_cur->name); uart_putc(' '); uart_dec(app_cw); uart_putc('x'); uart_dec(app_ch);
        if (first) { uart_puts(" at "); uart_dec((unsigned)app_lx + 8); uart_putc(','); uart_dec((unsigned)app_ly + 32); }
        else uart_puts(" resized");
        uart_putc('\n');
    }
    st32(info, app_bw); st32(info + 4, app_bh); st32(info + 8, app_bw * (app_hi ? 8 : 4));   /* width and height stay logical; the pitch (bytes) says the buffer is twice as wide */
    st32(info + 12, 0); st32(info + 16, (unsigned)(UB + UP_WIN)); st32(info + 20, (unsigned)((UB + UP_WIN) >> 32));
    return 0;
}
/* JT_SYS_TEXT2 (3.9.0): one run of Tree Sans into the device-size window buffer, for an app that opened it hi-res. The
   request is user/jtsys.h's struct jt_text2 (LP64). Each byte is drawn alone at the pen, which then moves by that byte's
   own advance from the app, so the app's 1x layout carries over exactly while the glyphs are rasterised at twice the size
   by the same rasterizer the Console uses (text.c). Faces: 0 body, 1 bold, 2 display (digits), 3 mono; the sizes are the
   ones tools/gen/gen_user_text.c gave the 1x atlas, times the scale, times pct/100. Answers the pen's x, or -EINVAL when
   the window is not hi-res and -EFAULT for a request or string outside the app's memory. */
struct jt_text2 { int x, y; unsigned rgb; int face, pct, len; unsigned long s, adv; };
static long app_text(unsigned long req) {
    if (!app_running || !app_wbytes || !app_hi || !text_ok) return -22;
    if (!user_in_range(req, sizeof(struct jt_text2))) return -14;
    const struct jt_text2 *t = (const struct jt_text2 *)req;
    int x = t->x, y = t->y, face = t->face, pct = t->pct, n = t->len;
    unsigned rgb = t->rgb; unsigned long sp = t->s, adv = t->adv;
    if (face < 0 || face > 3 || pct < 25 || pct > 400 || n < 1 || n > 255) return -22;
    if (!user_in_range(sp, (unsigned long)n) || !user_in_range(adv, (unsigned long)n)) return -14;
    static const unsigned char which[4] = {2, 1, 2, 0};   /* text.c's faces: 0 mono, 1 sans bold, 2 sans */
    static const int px10[4] = {140, 140, 560, 135};
    int k = (int)window_scale(), bw = (int)app_cw * k, bh = (int)app_ch * k, px = px10[face] * k * pct / 100;
    unsigned *buf = (unsigned *)(UB + UP_WIN);
    for (int i = 0; i < n; i++) {
        char one[2] = { ((const char *)sp)[i], 0 };
        text_draw(which[face], one, x, y, px, rgb, buf, (unsigned)bw, bw, bh);
        x += ((const unsigned char *)adv)[i];
    }
    return x;
}
/* JT_SYS_WINDOW_POLL, i386's semantics: with JT_POLL_PRESENT the buffer goes to the screen first (only while the window is
   in front and Spotlight is not over it; a raise draws it later), then one event (the LP64 struct jt_event: kind, a, b)
   and 1, or -EAGAIN when there is none. It never waits: the app yields and asks again. */
static long app_window_poll(unsigned long ev, unsigned long flags) {
    if (!app_running || !app_wbytes) return -9;
    if (!user_writable(ev, 12)) return -14;
    if (flags & ~1UL) return -22;
    if (flags & 1) {
        ap->presents++;
        if (!app_shown) {
            con_quiet = 1; uart_puts("M3c window "); uart_puts(app_cur->name);
            if (app_rs_log) { uart_puts(" redrawn "); uart_dec(app_bw); uart_putc('x'); uart_dec(app_bh); uart_putc('\n'); app_rs_log = 0; }   /* the first frame in the buffer a resize gave it */
            else uart_puts(" first frame\n");
        }
        app_shown = 1;
        if (app_front && app_top == (int)ap->slot && !spot_live) {
            int s = (int)window_scale();
            fb_hold(); cur_hide(); app_blit(); fb_flush((app_lx + 8) * s, (app_ly + 32) * s, (int)app_cw * s, (int)app_ch * s); cur_show(); fb_release();
        }
    }
    if (app_qh == app_qt) return -11;
    unsigned i = app_qh++ % APP_EVQ;
    st32(ev, app_q[i].kind); st32(ev + 4, (unsigned)app_q[i].a); st32(ev + 8, (unsigned)app_q[i].b);
    con_quiet = 1; uart_puts("M3c event "); uart_puts(app_q[i].kind == 1 ? "key " : app_q[i].kind == 4 ? "resize " : "click "); uart_dec((unsigned)app_q[i].a);
    if (app_q[i].kind != 1) { int y = app_q[i].b; uart_putc(','); if (y < 0) { uart_putc('-'); y = -y; } uart_dec((unsigned)y); }   /* a close click is above the content */
    uart_puts(" to "); uart_puts(app_cur->name); uart_putc('\n');
    return 1;
}
static void app_window_close(void) {
    if (!app_wbytes) return;
    cur_hide();
    wall_vacate(app_lx, app_ly, app_lw, app_lh, 0);   /* the frame and a ring round it: anything that overhung the frame goes too */
    app_shown = 0;
    unsigned gone = ap->slot;
    app_z_drop(gone);
    app_top = app_zn ? (int)app_z[0] : -1;
    ap = &apps[app_top >= 0 ? app_top : gone];
    app_front = app_top >= 0;
    if (app_front) app_repaint_all();   /* the other apps' windows the closed one covered or was covered by */
    else if (con_live) { wall_vacate(win_lx, win_ly, win_lw, win_lh, 0); con_live = 0; pane_open(cp); }   /* the wallpaper went over its part of the Console too: draw it whole */
    cur_show();
}
/* A dock tile whose name is an app in the table (Calendar, and any later one): start it, or bring its window up. */
static int app_dock(const char *name) {
    for (const struct user_app *a = user_apps; a->name; a++) {
        unsigned i = 0;
        while (name[i] && (name[i] | 32) == a->name[i]) i++;
        if (name[i] || a->name[i]) continue;
        for (unsigned k = 0; k < APP_SLOTS; k++) if (apps[k].cur == a) { ap = &apps[k]; app_raise(); return 1; }   /* already open: its window comes to the front */
        if (app_start(a) == -3 && app_zn) {   /* every slot is taken: the window furthest back closes for it, files written back */
            ap = &apps[app_z[app_zn - 1]];
            con_quiet = 1; uart_puts("M3c app swap: "); uart_puts(app_cur->name); uart_puts(" closed for "); uart_puts(a->name); uart_putc('\n');
            app_kill_one("closed for another app");
            app_start(a);
        }
        return 1;
    }
    return 0;
}
/* ---- Window drag and resize. Every window the desktop draws (the Console, the Terminal, Clock, Brick and the Browser in one
   rectangle win_lx..win_lh; the Calculator; an app's frame) is one target of the same path: a press picks the front window and
   what the pointer took hold of, pointer_moved turns the pointer into a new rectangle, and win_set applies it. A press on the
   title band (not on a dot) moves the window, the grab offset kept. A press within RS_ZONE = 12 px of the left, right or
   bottom edge (the bottom-right corner is both) below the title band resizes it, the grabbed edge following the pointer and the
   opposite one staying. The size is held between a minimum (win_limits) and the strip between the menu bar and the dock's
   band, the position inside the screen and that strip, so no band is ever under a window. A move of the Console family copies
   its pixels (the contents come along as they are, the uncovered part becomes wallpaper, the Console's text origin shifts);
   anything else, a resize or a move of the Calculator or an app, puts wallpaper where the window was, draws the Console behind
   whole if the old place met it, then draws the window again at the new rectangle, so it lays its content out to the new
   size: the Console and Terminal re-fit their columns and rows, the Browser re-wraps, the Clock and Brick re-centre, the
   Calculator re-spaces its keys, and an app gets JT_EV_RESIZE (the same event i386's compositor sends), answers it with a
   second JT_SYS_WINDOW_OPEN and redraws. The left button going up ends it. The Console is not moved while the Calculator or
   an app is open (it is behind: there is no stacking to repaint). ---- */
#define WT_MAIN 0
#define WT_CALC 1
#define WT_APP  2
#define RS_L 1
#define RS_R 2
#define RS_B 4
#define RS_ZONE 12   /* logical px inside an edge that take hold of it */
#define MIN_WIN_W 320   /* the smallest the Console, Terminal, Browser and Clock shrink to (Brick keeps its first size) */
#define MIN_WIN_H 220
#define MIN_CALC_W 220
#define MIN_CALC_H 260
#define MIN_APP_W 400
#define MIN_APP_H 300
#define GRAB_TITLE (-1)
static int in_box(int x, int y, int bx, int by, int bw, int bh);
static void clock_paint(void);
static void brick_paint(void);
static void browser_paint(int report);
static int br_frame;
static int win_def[3][2];   /* each window's size when the desktop drew it first: the Console family's, the Calculator's, an app's */
static int *win_rc(int t, int i) {   /* window t's x, y, w, h (i = 0..3); the app's are the front app's, so no table can hold their addresses */
    static int *const a[2][4] = { { &win_lx, &win_ly, &win_lw, &win_lh }, { &calc_lx, &calc_ly, &calc_lw, &calc_lh } };
    int *b[4] = { &app_lx, &app_ly, &app_lw, &app_lh };
    return t == WT_APP ? b[i] : a[t][i];
}
static int drag_on, drag_dx, drag_dy;   /* a title-band press is moving the window; where inside it the pointer took hold, logical px */
static int rs_on, rs_dx, rs_dy;         /* an edge press is resizing it: the edges taken (RS_*), and how far the pointer was past the right and bottom edges */
static int drag_t;                      /* which window (WT_*) either is acting on */
static int win_sizing;                  /* win_set is redrawing a window at a new size: the Clock keeps its log line for the minute */
static int win_target(void) {   /* the window that takes a drag: the one in front, -1 for none */
    if (spot_live) return -1;
    if (brick_live || clock_live || browser_live || settings_live) return WT_MAIN;
    if (app_wbytes && app_front) return WT_APP;
    if (calc_live && calc_front) return WT_CALC;
    if (con_live && !calc_live && !app_wbytes) return WT_MAIN;
    return -1;
}
static void win_limits(int t, int *minw, int *minh) {   /* a window opens at its default size and never has to be larger than that to fit its content: the Brick field has a fixed size, the rest shrink to a floor */
    int dw = win_def[t][0], dh = win_def[t][1], fw = t == WT_APP ? MIN_APP_W : t == WT_CALC ? MIN_CALC_W : MIN_WIN_W, fh = t == WT_APP ? MIN_APP_H : t == WT_CALC ? MIN_CALC_H : MIN_WIN_H;
    if (t == WT_MAIN && brick_live) { fw = dw; fh = dh; }
    *minw = fw < dw ? fw : dw; *minh = fh < dh ? fh : dh;
}
static int win_grab(int t, int x, int y) {   /* what a press at (x, y) takes hold of: GRAB_TITLE, a mask of RS_* edges, or 0 for the window's inside */
    int wx = *win_rc(t, 0), wy = *win_rc(t, 1), ww = *win_rc(t, 2), wh = *win_rc(t, 3);
    if (!in_box(x, y, wx, wy, ww, wh)) return 0;
    if (y < wy + 30) {   /* the title band, but not the three dots (radius 7, a pixel spare) */
        for (int d = 0; d < 3; d++) { int dx = x - (wx + 24 + 22 * d), dy = y - (wy + 16); if (dx * dx + dy * dy <= 8 * 8) return 0; }
        return GRAB_TITLE;
    }
    return (x < wx + RS_ZONE ? RS_L : 0) | (x >= wx + ww - RS_ZONE ? RS_R : 0) | (y >= wy + wh - RS_ZONE ? RS_B : 0);
}
static void app_note_resize(void) {   /* JT_EV_RESIZE for the app; one waiting event is updated rather than a second queued, so a long drag asks once for the size it ends on */
    if (!app_cur) return;
    for (unsigned i = app_qh; i != app_qt; i++) if (app_q[i % APP_EVQ].kind == 4) { app_q[i % APP_EVQ].a = (int)app_cw; app_q[i % APP_EVQ].b = (int)app_ch; app_kick = 1; return; }
    app_push(4, (int)app_cw, (int)app_ch);
}
/* 4.3.0: the damage of a moved or resized Calculator or app window, physical px x0..x1 by y0..y1 (the old and new rectangles'
   union), put together from the bottom up: the wallpaper (and the line over the dock when it is that low), the Console if it
   is open behind, then the Calculator or every app window that meets the union, back to front. With the back buffer it is
   drawn on the scratch screen, where each window may be drawn whole (its corners blend over whatever is there, and outside the
   union that is never looked at), and only the union is copied into the back buffer. So nothing outside it changes, a window
   behind comes back where the moved one uncovered it, and no corner is blended over itself twice. */
static int box_meets(int x0, int y0, int x1, int y1, int lx, int ly, int lw, int lh) {
    int s = (int)window_scale();
    return x0 < (lx + lw) * s && lx * s < x1 && y0 < (ly + lh) * s && ly * s < y1;
}
static void desk_compose(int x0, int y0, int x1, int y1) {
    int s = (int)window_scale();
    unsigned *keep = fb;
    if (fb_scan) fb = (unsigned *)(FB_RAM_BASE + FB_RAM_MAX);
    fb_mute++;
    wall_fill(x0, y0, x1 - x0, y1 - y0);
    if (text_ok && y1 > (gui_dock_band_top() - 40) * s) tribute_paint();
    if (con_live && box_meets(x0, y0, x1, y1, win_lx, win_ly, win_lw, win_lh)) { int cf = calc_front, af = app_front; calc_front = app_front = 0; pane_draw(); calc_front = cf; app_front = af; }
    if (calc_live && box_meets(x0, y0, x1, y1, calc_lx, calc_ly, calc_lw, calc_lh)) calc_paint();
    struct app *k = ap;
    for (int z = (int)app_zn - 1; z >= 0; z--) { ap = &apps[app_z[z]]; if (app_wbytes && box_meets(x0, y0, x1, y1, app_lx, app_ly, app_lw, app_lh)) app_paint(); }
    ap = k;
    fb_mute--;
    if (fb != keep) {
        for (int j = y0; j < y1; j++) for (int i = x0; i < x1; i++) keep[(unsigned)j * fb_pitch + (unsigned)i] = fb[(unsigned)j * fb_pitch + (unsigned)i];
        fb = keep;
    }
}
static void win_set(int t, int nx, int ny, int nw, int nh) {   /* the one place a window changes rectangle */
    if (!fb) return;
    int *px_ = win_rc(t, 0), *py_ = win_rc(t, 1), *pw_ = win_rc(t, 2), *ph_ = win_rc(t, 3);
    if (nx == *px_ && ny == *py_ && nw == *pw_ && nh == *ph_) return;
    int s = (int)window_scale(), ox = *px_ * s, oy = *py_ * s, w = *pw_ * s, h = *ph_ * s, px = nx * s, py = ny * s, dx = px - ox, dy = py - oy;
    int resized = nw != *pw_ || nh != *ph_;
    int ux = ox < px ? ox : px, uy = oy < py ? oy : py, ex = ox + w > px + nw * s ? ox + w : px + nw * s, ey = oy + h > py + nh * s ? oy + h : py + nh * s;
    fb_hold(); fb_tag = 1;   /* 4.3.0: everything below reaches the screen as one copy, the old and new rectangles' union */
    cur_hide(); cur_frozen++;
    if (t == WT_MAIN && !resized) {
        wall_vacate(*px_, *py_, *pw_, *ph_, 1);   /* what overhangs the frame stays behind otherwise */
        for (int j = 0; j < h; j++) {   /* a memmove by rows: the far rows first when moving down, the far columns first when moving right */
            int r = dy > 0 ? h - 1 - j : j;
            unsigned *src = fb + (unsigned)(oy + r) * fb_pitch + (unsigned)ox, *dst = fb + (unsigned)(py + r) * fb_pitch + (unsigned)px;
            if (dx > 0) for (int i = w - 1; i >= 0; i--) dst[i] = src[i];
            else for (int i = 0; i < w; i++) dst[i] = src[i];
        }
        wall_minus(ox, oy, w, h, px, py, w, h);
        *px_ = nx; *py_ = ny;
        con_x += dx; con_y += dy; con_wx += dx; con_wy += dy;   /* con_layout's origin and the title bar's "lines N-M" follow; the size, and so the rows and columns, are unchanged */
    } else if (t == WT_MAIN) {
        wall_minus(ox, oy, w, h, px, py, nw * s, nh * s);
        *px_ = nx; *py_ = ny; *pw_ = nw; *ph_ = nh;
        win_sizing = resized;
        if (brick_live) brick_paint();
        else if (clock_live) clock_paint();
        else if (browser_live) { br_frame = 1; browser_paint(0); }
        else if (settings_live) set_paint();
        else { con_fit(); pane_draw(); }
        win_sizing = 0;
    } else {
        *px_ = nx; *py_ = ny; *pw_ = nw; *ph_ = nh;
        if (t == WT_APP) { app_cw = (unsigned)(nw - 16); app_ch = (unsigned)(nh - 40); if (resized) app_note_resize(); }
        win_sizing = resized;
        desk_compose(ux, uy, ex, ey);   /* the union again from the wallpaper up, every window that meets it in stacking order */
        win_sizing = 0;
    }
    fb_flush(ux, uy, ex - ux, ey - uy);
    cur_frozen--;
    fb_release();
}
static void sam_fill(void) {   /* 4.20.0: the Samantha tile opens her chat across the screen, menu bar to dock, so her face is the big one */
    if (!fb || brick_live || clock_live || browser_live || settings_live || calc_front || app_front || cp != &term_p) return;
    int s = (int)window_scale(), top = sg(MENUBAR_H) / s + 8, bottom = gui_dock_band_top() - 22;
    win_set(WT_MAIN, 16, top, (int)fb_w / s - 32, bottom - top);
    uart_puts("dock Samantha: full\n");
}
static void win_move_to(int nx, int ny) {   /* the front window (or the Console family's, when none is up) to this place, held whole between the menu bar and the dock's band */
    int t = drag_on || rs_on ? drag_t : WT_MAIN;
    int s = (int)window_scale(), top = sg(MENUBAR_H) / s + 8, bottom = gui_dock_band_top() - 22, w = *win_rc(t, 2), h = *win_rc(t, 3), right = (int)fb_w / s - w;
    if (nx > right) nx = right;
    if (nx < 0) nx = 0;
    if (ny > bottom - h) ny = bottom - h;
    if (ny < top) ny = top;
    win_set(t, nx, ny, w, h);
}
static void win_resize_to(int lx, int ly) {   /* the pointer is at (lx, ly): the grabbed edges follow it, the others stay */
    int t = drag_t, s = (int)window_scale(), bottom = gui_dock_band_top() - 22, sw = (int)fb_w / s, mnw, mnh;
    int x = *win_rc(t, 0), y = *win_rc(t, 1), w = *win_rc(t, 2), h = *win_rc(t, 3), r = x + w, nx = x, nw = w, nh = h;
    win_limits(t, &mnw, &mnh);
    if (rs_on & RS_R) nw = lx - rs_dx - x;
    if (rs_on & RS_L) { nx = lx - rs_dx; nw = r - nx; }
    if (rs_on & RS_B) nh = ly - rs_dy - y;
    if (rs_on & RS_R) { if (nw > sw - x) nw = sw - x; }
    if (rs_on & RS_L) { if (nw > r) nw = r; }
    if (nw < mnw) nw = mnw;
    if (nh > bottom - y) nh = bottom - y;
    if (nh < mnh) nh = mnh;
    if (rs_on & RS_L) nx = r - nw;
    if (t == WT_APP && (unsigned long)(nw - 16) * (unsigned)(nh - 40) * 4 > UWIN_MAX) return;   /* the app's buffer must fit its mapped window: the size stays where it last fit */
    win_set(t, nx, y, nw, nh);
}
static void drag_end(void) {
    if (!drag_on && !rs_on) return;
    int t = drag_t, was_sizing = rs_on;
    drag_on = rs_on = 0;
    con_quiet = 1; uart_puts("window at "); uart_dec((unsigned)*win_rc(t, 0)); uart_putc(','); uart_dec((unsigned)*win_rc(t, 1)); uart_putc('\n');
    if (was_sizing) {
        con_quiet = 1; uart_puts("window size "); uart_dec((unsigned)*win_rc(t, 2)); uart_putc('x'); uart_dec((unsigned)*win_rc(t, 3)); uart_putc('\n');
        if (t == WT_APP && app_wbytes && (app_cw != app_bw || app_ch != app_bh)) app_note_resize();   /* an event the full ring dropped is asked again */
    }
}
static void pointer_moved(void) {   /* one report done: the arrow to its new place, the dock's label to the slot under it */
    if (!fb) return;
    int s = (int)window_scale(), slot = gui_dock_hit_test((int)mouse_x / s, (int)mouse_y / s);
    fb_hold();   /* 4.3.0: a moved window and the arrow on top of it go to the screen as one copy, at the end */
    if (drag_on || rs_on) {
        if (win_target() != drag_t) drag_on = rs_on = 0;
        else if (drag_on) win_move_to((int)mouse_x / s - drag_dx, (int)mouse_y / s - drag_dy);
        else win_resize_to((int)mouse_x / s, (int)mouse_y / s);
    }
    if (spot_live && spot_tree) tree_hover((int)mouse_x / s, (int)mouse_y / s);
    if (brick_live) brick_mouse((int)mouse_x / s, (int)mouse_y / s);
    cur_wanted = 1;
    cur_hide();
    if (slot != hover_slot) { hover_slot = slot; dock_hover(slot); }
    cur_show();
    fb_release();
}
static int in_box(int x, int y, int bx, int by, int bw, int bh) { return x >= bx && x < bx + bw && y >= by && y < by + bh; }
static int on_close_dot(int x, int y, int wx, int wy) { int dx = x - (wx + 24), dy = y - (wy + 16); return dx * dx + dy * dy <= 8 * 8; }   /* gui_draw_window_frame's red dot, radius 7 */
static void dock_activate(int slot);
void ask_prefill(const char *s);   /* ask.c: type s on the Terminal's empty line */
#define SPOT_MUSIC (GUI_ICON_COUNT + 2)   /* M3e: Music is in Spotlight (and the app table) but not on the dock, a decision */
#define SPOT_BROWSER (GUI_ICON_COUNT + 3) /* the Browser is in Spotlight only too: the dock's 11 slots stay as they are */
#define SPOT_BOOKS (GUI_ICON_COUNT + 4)   /* 2.66: Books, the same: Spotlight only, not on the dock */
#define SPOT_CALC (GUI_ICON_COUNT + 5)    /* 3.7.5: the Calculator, in Spotlight too (its dock tile is Apps); opens the calc window */
#define SPOT_FINDER (GUI_ICON_COUNT + 6)  /* 3.14.0: the Finder, user/finder.c at EL0; the dock has no Finder tile, so Spotlight and Samantha's open reach it */
#define SPOT_SETTINGS (GUI_ICON_COUNT + 7) /* 3.13.0: Settings, in Spotlight and the menu bar's status icons; the dock keeps its 11 tiles */
#define SPOT_GAME0 (GUI_ICON_COUNT + 8)   /* 3.19.0: Mines, Toroid, Hamurabi and Keyrate, the unchanged user/NAME.c at EL0, Spotlight only */
#define SPOT_LAST (GUI_ICON_COUNT + 11)
static const char *const game_names[4] = {"Mines", "Toroid", "Hamurabi", "Keyrate"};
static int lp_mode;                       /* 4.1.2: spot_live holds the Launchpad's card, not Spotlight's bar */
static void lp_open(void);
static void lp_click(int lx, int ly);
static void pointer_click(void) {   /* the left button went down */
    if (!fb) return;
    int s = (int)window_scale(), lx = (int)mouse_x / s, ly = (int)mouse_y / s, slot = gui_dock_hit_test(lx, ly);
    if (spot_live && spot_tree) { tree_click(lx, ly); return; }   /* the tree menu is up: a click is a row, or it dismisses the card */
    if (tree_bar_hit()) { spot_close(); tree_open(); return; }
    if (spot_live && lp_mode) { lp_click(lx, ly); return; }   /* the Launchpad takes the click: a cell opens, anywhere else closes it */
    if (spot_live && slot < 0) return;   /* the bar is over the windows: a raise under it would leave its saved pixels stale */
    if (slot < 0 && menubar_click((int)mouse_x, (int)mouse_y)) return;   /* a status icon in the menu bar: Settings at its section */
    int on_win = in_box(lx, ly, win_lx, win_ly, win_lw, win_lh), on_calc = calc_live && in_box(lx, ly, calc_lx, calc_ly, calc_lw, calc_lh);
    if (slot < 0 && app_front) {   /* a press on an app window that is not the front one raises it first */
        for (unsigned z = 0; z < app_zn; z++) {
            struct app *w = &apps[app_z[z]];
            if (w->wbytes && in_box(lx, ly, w->lx, w->ly, w->lw, w->lh)) { if ((int)w->slot != app_top) { ap = w; app_raise(); return; } break; }
        }
    }
    int on_app = app_wbytes && in_box(lx, ly, app_lx, app_ly, app_lw, app_lh);
    int wt = slot < 0 ? win_target() : -1, grab = wt >= 0 ? win_grab(wt, lx, ly) : 0;   /* the front window's title band or an edge takes hold of it */
    if (grab == GRAB_TITLE) { drag_on = 1; drag_t = wt; drag_dx = lx - *win_rc(wt, 0); drag_dy = ly - *win_rc(wt, 1); return; }
    if (grab) { rs_on = grab; drag_t = wt; rs_dx = lx - (*win_rc(wt, 0) + *win_rc(wt, 2)); rs_dy = ly - (*win_rc(wt, 1) + *win_rc(wt, 3)); if (grab & RS_L) rs_dx = lx - *win_rc(wt, 0); return; }
    if (brick_live && slot < 0) {
        if (spot_live) return;
        if (on_close_dot(lx, ly, win_lx, win_ly)) brick_close();
        else if (on_win && ly >= win_ly + 32) brick_key(57, 1);
    } else if (clock_live && slot < 0) {
        if (on_close_dot(lx, ly, win_lx, win_ly)) clock_close();
    } else if (settings_live && slot < 0) {
        if (on_close_dot(lx, ly, win_lx, win_ly)) settings_close();
        else if (on_win) settings_click(lx, ly);
    } else if (browser_live && slot < 0) {
        if (spot_live) return;
        if (on_close_dot(lx, ly, win_lx, win_ly)) browser_close();
        else if (on_win) browser_click(lx, ly);
    } else if (slot >= 0) dock_activate(slot);
    else if (on_app && app_front) {   /* the front window takes the click: an app's red dot ends it, its content gets the click */
        if (on_close_dot(lx, ly, app_lx, app_ly)) {   /* i386's close: a click above the content (b < 0), which calendar.c answers by exiting */
            app_push(2, lx - (app_lx + 8), ly - (app_ly + 32));
            if (!app_close_at) app_close_at = cnt_now() + cnt_hz() / 4;   /* and an app that ignores it is ended in a quarter second */
        }
        else if (in_box(lx, ly, app_lx + 8, app_ly + 32, (int)app_cw, (int)app_ch)) app_push(2, lx - (app_lx + 8), ly - (app_ly + 32));
    } else if (on_calc && calc_front) {
        if (on_close_dot(lx, ly, calc_lx, calc_ly)) { calc_close(); return; }
        int cols, n = calc_keys(&cols);
        for (int i = 0; i < n; i++) {
            int x, y, w, h; calc_cell(i, &x, &y, &w, &h);
            if (in_box(lx, ly, x, y, w, h)) { calc_did(calc_press(calc_key(i))); return; }
        }
    } else if (on_win && con_live && !calc_front && !app_front) { if (on_close_dot(lx, ly, win_lx, win_ly)) console_close(); }
    else if (on_calc) calc_raise();                    /* ...one behind it comes to the front with the keys */
    else if (on_app) app_raise();
    else if (on_win && con_live) pane_open(cp);
}
static const char *dock_app_name(int slot) {   /* the EL0 app a slot starts, by its user_apps name; 0 for the kernel-drawn ones */
    return slot == SPOT_MUSIC ? "Music" : slot == SPOT_BOOKS ? "Books" : slot == SPOT_FINDER || slot == 1 ? "Finder" : slot >= SPOT_GAME0 ? game_names[slot - SPOT_GAME0] : dock_names[slot];
}
static void dock_activate(int slot) {   /* a dock tile, from a click, a dock key, Spotlight or the Launchpad; slot 0, Apps, is the Launchpad (it was the Calculator until 4.1.2: the first desktop slice had no Launchpad, so the Apps tile opened the one app there was) */
    if (slot == 0) { lp_open(); return; }
    if (slot == SPOT_CALC) { calc_open(); return; }
    if (slot == GUI_ICON_COUNT) { clock_open(); return; }
    if (slot == GUI_ICON_COUNT + 1) { brick_open(); return; }
    if (slot == SPOT_SETTINGS) { settings_open(0); return; }
    if (slot == SPOT_BROWSER) { browser_open(); return; }
    settings_close();
    brick_close();
    clock_close();
    browser_close();
    if (app_dock(dock_app_name(slot))) return;   /* slot 1 is the Burrow tile: the file window, called Finder on the Pi */   /* Calendar, Notes, Music and Books: user/NAME.c at EL0, in its own window */
    if (slot == 7) { ask_prefill("sam "); pane_open(&term_p); sam_fill(); ask_redraw(); con_quiet = 1; uart_puts("dock Samantha: sam ready\n"); return; }   /* 3.9.8: the Samantha tile is the Terminal with `sam ` already typed */
    if (slot == 6) { pane_open(&term_p); return; }   /* dock_names[6], Terminal: the command line, in front of an open Calculator */
    console_open();
    uart_puts("dock "); uart_puts(dock_names[slot]); uart_puts(": not on ARM yet\n");
}

/* ---- Keyboard only: the Pi has no mouse yet, and the Terminal (the agent's prompt) could only be opened by a click.
   F1 or Cmd+Space opens Spotlight, a small bar over the desktop: type to filter the dock's names, Up and Down to
   choose, Enter opens, Esc closes. With no pane holding the keys (the Console takes none), Left and Right move the
   dock's hover label, Enter opens that tile, Esc clears it. F2 or Ctrl+T opens the Terminal at once; Esc with the
   Terminal in front and no question running hands the keys back to the desktop (the Console). ---- */
int ask_pending(void);
int ask_select(unsigned pane);
int ask_char(unsigned code);                  /* ask.c: a key code as the character it types, Shift included */
#define SPOT_W 300
#define SPOT_ROW 20
#define SPOT_MAX (SPOT_LAST + 1)      /* every app fits: an empty query lists them all, no scrolling */
static char spot_text[24]; static unsigned spot_len;
static int spot_sel, spot_hits[SPOT_MAX], spot_n, dock_sel = -1, ctrl_held, gui_held, alt_held;   /* Ctrl, Cmd (the GUI key), Alt or Option: either side */
static unsigned *spot_under;          /* the pixels under the bar's largest size, or under the Launchpad's card, saved on open */
static int spot_lx, spot_ly, spot_lh, spot_bw, spot_bh;   /* the bar on the logical grid; its height is what is drawn now; spot_bw x spot_bh is the box saved under (the bar at its tallest, or the Launchpad card) */
static int spot_match(const char *name) {   /* the typed text, case folded, anywhere in the name */
    if (!spot_len) return 1;
    for (unsigned i = 0; name[i]; i++) {
        unsigned k = 0;
        while (k < spot_len && name[i + k] && (name[i + k] | 32) == (spot_text[k] | 32)) k++;
        if (k == spot_len) return 1;
    }
    return 0;
}
static const char *spot_name(int slot) { return slot == GUI_ICON_COUNT ? "Clock" : slot == GUI_ICON_COUNT + 1 ? "Brick" : slot == SPOT_MUSIC ? "Music" : slot == SPOT_BROWSER ? "Browser" : slot == SPOT_BOOKS ? "Books" : slot == SPOT_CALC ? "Calculator" : slot == SPOT_FINDER ? "Finder" : slot == SPOT_SETTINGS ? "Settings" : slot == SPOT_GAME0 + 2 ? "Hamurapi" : slot >= SPOT_GAME0 ? game_names[slot - SPOT_GAME0] : dock_names[slot]; }
static int spot_hmax(void) { return 34 + SPOT_ROW * SPOT_MAX + 6; }
static void spot_paint(void) {
    int s = (int)window_scale();
    spot_n = 0;
    for (int i = 1; i <= SPOT_LAST && spot_n < SPOT_MAX; i++) if (spot_match(spot_name(i)) || (i >= SPOT_GAME0 && spot_match(game_names[i - SPOT_GAME0]))) spot_hits[spot_n++] = i;
    if (spot_sel >= spot_n) spot_sel = spot_n ? spot_n - 1 : 0;
    cur_hide();
    int hmax = spot_hmax(), w = SPOT_W * s, h = hmax * s, x = spot_lx * s, y = spot_ly * s;
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) fb[(unsigned)(y + j) * fb_pitch + (unsigned)(x + i)] = spot_under[j * w + i];   /* the desktop back, then the bar at its new height */
    spot_lh = 34 + SPOT_ROW * spot_n + (spot_n ? 6 : 0);
    gui_rounded_rect_on_wallpaper(spot_lx, spot_ly, SPOT_W, spot_lh, 0x00faf8f4, 10);
    window_rect(spot_lx + 12, spot_ly + 9, 3, 16, 0x00b5502c);   /* the accent: a caret mark at the left of the typed line */
    gui_text(spot_len ? spot_text : "Open an app", spot_lx + 22, spot_ly + 9, spot_len ? 0x001C1C1E : 0x0075726E);
    if (spot_n) gui_hairline_h(spot_lx + 10, spot_ly + 33, SPOT_W - 20, 0x00DDD8CE);
    for (int r = 0; r < spot_n; r++) {
        int ry = spot_ly + 37 + r * SPOT_ROW, on = r == spot_sel;
        if (on) window_rect(spot_lx + 8, ry, SPOT_W - 16, SPOT_ROW - 2, 0x00b5502c);
        gui_text(spot_name(spot_hits[r]), spot_lx + 22, ry + 2, on ? 0x00FFFFFF : 0x001C1C1E);
    }
    fb_flush(x, y, w, h);
    cur_show();
    con_quiet = 1; uart_puts("spotlight: "); uart_puts(spot_text); uart_putc(' '); uart_dec((unsigned)spot_n); uart_putc('\n');
}
/* 4.1.2: the Launchpad, the Apps tile's card. It reuses Spotlight's whole mechanism: spot_live, the saved pixels (spot_under, sized for
   the bigger of the two), spot_close's restore, and the SPOT_* slot table, so one list (spot_name, slots 1..SPOT_LAST) drives both. */
#define LP_COLS 6
#define LP_ROWS ((SPOT_LAST + LP_COLS - 1) / LP_COLS)
#define LP_CW 86                      /* a cell: the icon over its name */
#define LP_CH 78
#define LP_PAD 22
#define LP_TOP 20
#define LP_ICON 48
#define LP_W (LP_COLS * LP_CW + 2 * LP_PAD)
#define LP_H (LP_ROWS * LP_CH + 2 * LP_TOP)
static int lp_sel;
static int spot_buf(void) {   /* the saved-pixels buffer, taken once at the larger of the two boxes */
    if (!spot_under) {
        int s = (int)window_scale(), a = LP_W * LP_H, b = SPOT_W * spot_hmax();
        spot_under = kmalloc((unsigned)((a > b ? a : b) * s * s) * 4);
    }
    return spot_under != 0;
}
static void spot_open(void) {
    if (spot_live || !fb) return;
    int s = (int)window_scale(), w = SPOT_W * s, h = spot_hmax() * s;
    spot_lx = ((int)fb_w / s - SPOT_W) / 2; spot_ly = sg(MENUBAR_H) / s + 24;
    if (!spot_buf()) { uart_puts("oom spotlight\n"); return; }
    spot_bw = SPOT_W; spot_bh = spot_hmax(); lp_mode = 0;
    cur_hide();
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) spot_under[j * w + i] = fb[(unsigned)(spot_ly * s + j) * fb_pitch + (unsigned)(spot_lx * s + i)];
    spot_live = 1; spot_tree = 0; spot_len = 0; spot_text[0] = 0; spot_sel = 0;
    con_quiet = 1; uart_puts("spotlight open\n");
    spot_paint();
}
static void spot_close(void) {
    if (!spot_live) return;
    int s = (int)window_scale(), w = (spot_tree ? TREE_W : spot_bw) * s, h = (spot_tree ? TREE_H : spot_bh) * s, x = spot_lx * s, y = spot_ly * s;
    cur_hide();
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) fb[(unsigned)(y + j) * fb_pitch + (unsigned)(x + i)] = spot_under[j * w + i];
    fb_flush(x, y, w, h);
    spot_live = 0;
    con_quiet = 1; uart_puts(spot_tree ? "treemenu close\n" : lp_mode ? "launchpad close\n" : "spotlight close\n");
    spot_tree = 0; lp_mode = 0;
    cur_show();
    if (app_front && app_shown) app_paint();   /* frames the app presented under the bar were held back */
}
static int lp_on_arm(int slot) {   /* the slot starts something here: a kernel-drawn window, the Terminal, or an EL0 app in user_apps; else it is greyed */
    if (slot == 6 || slot == 7 || slot == SPOT_CALC || slot == SPOT_SETTINGS || slot == SPOT_BROWSER || slot == GUI_ICON_COUNT || slot == GUI_ICON_COUNT + 1) return 1;
    const char *name = dock_app_name(slot);
    for (const struct user_app *a = user_apps; a->name; a++) {
        unsigned i = 0;
        while (name[i] && (name[i] | 32) == a->name[i]) i++;
        if (!name[i] && !a->name[i]) return 1;
    }
    return 0;
}
static int lp_art(int slot) {   /* the ICON_ART slot of an entry: a dock tile's own art, else the app's APPS[] art (Brick has none of its own: the apps glyph; Settings: Activity's) */
    static const int order[GUI_ICON_COUNT] = GUI_DOCK_DEFAULT_ORDER;
    return slot < GUI_ICON_COUNT ? order[slot] : slot == GUI_ICON_COUNT ? 23 : slot == GUI_ICON_COUNT + 1 ? GUI_APPS_FOLDER : slot == SPOT_MUSIC ? 24 : slot == SPOT_BROWSER ? 19 : slot == SPOT_BOOKS ? 31
         : slot == SPOT_FINDER ? 0 : slot == SPOT_SETTINGS ? 22 : slot == SPOT_GAME0 ? 30 : slot == SPOT_GAME0 + 1 ? 13 : slot == SPOT_GAME0 + 2 ? 26 : slot == SPOT_GAME0 + 3 ? 9 : 17;
}
static void lp_tile(int icon, int x, int y, int size, unsigned under, int grey) {   /* one icon, the dock tile's art on the card (or its selection plate); grey draws it washed out */
    int pw = size * (int)window_scale();
    unsigned long mark = heap_mark();
    unsigned int *tile = kmalloc((unsigned)(pw * pw) * 4);
    unsigned char *art = 0; unsigned aw = 0, ah = 0, ach = 0;
    if (tile && png_decode(ICON_ART[icon], ICON_ART_LEN[icon], &art, &aw, &ah, &ach) == 0 && art && aw == ICON_ART_SIZE && ah == ICON_ART_SIZE && ach == 4) {
        gui_icon_art_scale(art, tile, pw, under);
        if (icon == 2) { int t = cal_today(); if (t >= 0) cal_overlay(tile, pw, t); }
        if (grey) for (int i = 0; i < pw * pw; i++) {
            unsigned c = tile[i]; if (c == under) continue;
            unsigned g = (((c >> 16) & 255) + ((c >> 8) & 255) + (c & 255)) / 3, o = 0;
            for (int sh = 16; sh >= 0; sh -= 8) o |= ((g * 2 + ((under >> sh) & 255) * 3) / 5) << sh;
            tile[i] = o;
        }
        gui_blit_tile(tile, x, y, size, under);
    }
    heap_release(mark);
}
static void lp_paint(void) {
    int s = (int)window_scale(), w = LP_W * s, h = LP_H * s, x = spot_lx * s, y = spot_ly * s;
    cur_hide();
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) fb[(unsigned)(y + j) * fb_pitch + (unsigned)(x + i)] = spot_under[j * w + i];
    gui_rounded_rect_on_wallpaper(spot_lx, spot_ly, LP_W, LP_H, 0x00faf8f4, 12);
    for (int k = 0; k < SPOT_LAST; k++) {
        int slot = k + 1, cx = spot_lx + LP_PAD + (k % LP_COLS) * LP_CW, cy = spot_ly + LP_TOP + (k / LP_COLS) * LP_CH;
        int on = k == lp_sel, grey = !lp_on_arm(slot);
        unsigned under = on ? 0x00b5502c : 0x00faf8f4;
        if (on) window_rect(cx + 3, cy + 1, LP_CW - 6, LP_CH - 2, under);
        lp_tile(lp_art(slot), cx + (LP_CW - LP_ICON) / 2, cy + 6, LP_ICON, under, grey);
        const char *name = spot_name(slot);
        gui_text(name, cx + (LP_CW - gui_text_width(name)) / 2, cy + 6 + LP_ICON + 5, on ? 0x00FFFFFF : grey ? 0x00A8A39C : 0x001C1C1E);
    }
    fb_flush(x, y, w, h);
    cur_show();
    con_quiet = 1; uart_puts("launchpad: "); uart_dec((unsigned)(lp_sel + 1)); uart_putc(' '); uart_puts(spot_name(lp_sel + 1)); uart_putc('\n');
}
static void lp_open(void) {
    if (spot_live || !fb) return;
    int s = (int)window_scale(), w = LP_W * s, h = LP_H * s, top = sg(MENUBAR_H) / s, bot = gui_dock_band_top();
    spot_lx = ((int)fb_w / s - LP_W) / 2; spot_ly = top + (bot - top - LP_H) / 2;
    if (spot_ly < top + 4) spot_ly = top + 4;
    if (!spot_buf()) { uart_puts("oom launchpad\n"); return; }
    cur_hide();
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) spot_under[j * w + i] = fb[(unsigned)(spot_ly * s + j) * fb_pitch + (unsigned)(spot_lx * s + i)];
    spot_bw = LP_W; spot_bh = LP_H; spot_live = 1; lp_mode = 1; lp_sel = 0;
    con_quiet = 1; uart_puts("launchpad open "); uart_dec(SPOT_LAST); uart_putc('\n');
    lp_paint();
}
static void lp_pick(int slot) { spot_close(); dock_activate(slot); }
static void lp_click(int lx, int ly) {
    int cx = lx - spot_lx - LP_PAD, cy = ly - spot_ly - LP_TOP;
    if (lx < spot_lx || ly < spot_ly || lx >= spot_lx + LP_W || ly >= spot_ly + LP_H) { spot_close(); return; }   /* outside the card */
    if (cx < 0 || cy < 0 || cx >= LP_COLS * LP_CW || cy >= LP_ROWS * LP_CH) return;
    int k = cy / LP_CH * LP_COLS + cx / LP_CW;
    if (k < SPOT_LAST) lp_pick(k + 1);
}
static int lp_key(unsigned code) {   /* arrows move the selection, Enter opens it, Esc closes; every other key is the card's too */
    if (code == 1) spot_close();
    else if (code == 28 || code == 96) lp_pick(lp_sel + 1);
    else {
        int k = lp_sel;
        if (code == 105 && k % LP_COLS) k--;
        else if (code == 106 && k % LP_COLS < LP_COLS - 1 && k + 1 < SPOT_LAST) k++;
        else if (code == 103 && k >= LP_COLS) k -= LP_COLS;
        else if (code == 108 && k + LP_COLS < SPOT_LAST) k += LP_COLS;
        if (k != lp_sel) { lp_sel = k; lp_paint(); }
    }
    return 1;
}
static int spot_key(unsigned code) {   /* a key down with the bar open: every key is Spotlight's */
    if (spot_tree) { tree_key(code); return 1; }
    if (lp_mode) return lp_key(code);
    if (code == 1) spot_close();
    else if (code == 28 || code == 96) { int slot = spot_n ? spot_hits[spot_sel] : -1; spot_close(); if (slot >= 0) dock_activate(slot); }
    else if (code == 103) { if (spot_sel > 0) spot_sel--; spot_paint(); }
    else if (code == 108) { if (spot_sel + 1 < spot_n) spot_sel++; spot_paint(); }
    else if (code == 14) { if (spot_len) spot_text[--spot_len] = 0; spot_sel = 0; spot_paint(); }
    else { int ch = ask_char(code); if (ch >= ' ' && ch < 127 && spot_len + 1 < sizeof spot_text) { spot_text[spot_len++] = (char)ch; spot_text[spot_len] = 0; spot_sel = 0; spot_paint(); } }
    return 1;
}
static void dock_select(int slot) { dock_sel = slot; hover_slot = slot; dock_hover(slot); }
int ask_busy(void);   /* ask.c: a `sam` request is queued or waiting on the relay */
#include "power.h"
#ifdef POWER_FILE_TEST   /* the power check's output catcher: what a Terminal command printed, searched for a word */
static char seen[256];
static void grab(const char *t) { unsigned k = 0; while (seen[k]) k++; for (; *t && k + 1 < sizeof seen; t++) seen[k++] = *t; seen[k] = 0; }
static int has(const char *w) { for (unsigned i = 0; seen[i]; i++) { unsigned j = 0; while (w[j] && seen[i + j] == w[j]) j++; if (!w[j]) return 1; } return 0; }
#endif
/* The reboot and shutdown commands are refused only while a `sam` request is live. They must not use ask_pending(): that is
   also true while the typed line that holds this very command runs, so every typed reboot would be refused (found on the Pi). */
static int power_refused(void) {
    if (!ask_busy()) return 0;
    uart_puts("Wait for Samantha to finish, then try again.\n");
    return 1;
}
#ifdef PI_BUILD
void power_run(int off) {   /* the same action the confirmed chord runs */
    if (!power_refused()) power_apply(off);
}
#else
void power_run(int off) { if (!power_refused()) uart_puts(off ? "power: shutdown (this build has no power control)\n" : "power: restart (this build has no power control)\n"); }   /* the virt test build: only the Pi has a watchdog to pull */
#endif
int ask_edit(unsigned code);   /* ask.c: C X V A on the Terminal's input line */
static int front_close(void) {   /* the window in front, topmost first, ended the way its red dot ends it; always 1 (the chord is the desktop's) */
    if (spot_live) spot_close();
    else if (brick_live) brick_close();
    else if (clock_live) clock_close();
    else if (browser_live) browser_close();
    else if (app_front) { app_push(2, 16, -16); if (!app_close_at) app_close_at = cnt_now() + cnt_hz() / 4; }
    else if (calc_front) calc_close();
    else if (con_live) console_close();
    return 1;
}

/* ---- The tree menu: click the mark or "Joshua Tree" at the left of the menu bar (or F10, Ctrl+Option+T) for About, Restart,
   Shut Down and Log Out, in Spotlight's cream card. Restart and Shut Down are the confirmed chords' own power_ask (the
   Terminal, the Wait-for-Samantha refusal, Enter confirms); About shows the version and closes on any key. ---- */
#ifndef JT_VERSION
#define JT_VERSION "dev"
#endif
static int tree_sel, tree_about;
static const char *const tree_rows[4] = { "About Joshua Tree", "Restart", "Shut Down", "Log Out" };
static int tree_ry(int r) { return spot_ly + (r ? 37 + (r - 1) * SPOT_ROW : 6); }
static int tree_bar_hit(void) {   /* the pointer is on the mark or the title, physical px like the bar's own drawing */
    if (!text_ok || (int)mouse_y >= sg(MENUBAR_H) - 1) return 0;
    return (int)mouse_x < sg(40) + text_width(1, "Joshua Tree", sg(130)) + sg(6);
}
static void tree_paint(void) {
    int s = (int)window_scale(), w = TREE_W * s, h = TREE_H * s, x = spot_lx * s, y = spot_ly * s;
    cur_hide();
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) fb[(unsigned)(y + j) * fb_pitch + (unsigned)(x + i)] = spot_under[j * w + i];
    if (tree_about) {
        gui_rounded_rect_on_wallpaper(spot_lx, spot_ly, TREE_W, 82, 0x00faf8f4, 10);
        gui_text("Joshua Tree", spot_lx + 16, spot_ly + 10, 0x001C1C1E);
        gui_text("Version " JT_VERSION, spot_lx + 16, spot_ly + 32, 0x0075726E);
        gui_text("Made by Joshua, from scratch.", spot_lx + 16, spot_ly + 54, 0x001C1C1E);
    } else {
        gui_rounded_rect_on_wallpaper(spot_lx, spot_ly, TREE_W, 109, 0x00faf8f4, 10);
        gui_hairline_h(spot_lx + 10, spot_ly + 31, TREE_W - 20, 0x00DDD8CE);
        for (int r = 0; r < 4; r++) {
            int ry = tree_ry(r), on = r == tree_sel;
            if (on) window_rect(spot_lx + 8, ry, TREE_W - 16, SPOT_ROW - 2, 0x00b5502c);
            gui_text(tree_rows[r], spot_lx + 22, ry + 2, on ? 0x00FFFFFF : 0x001C1C1E);
        }
    }
    fb_flush(x, y, w, h);
    cur_show();
    con_quiet = 1; uart_puts(tree_about ? "treemenu: about\n" : "treemenu: row "); if (!tree_about) { uart_dec((unsigned)tree_sel); uart_putc('\n'); }
}
static void tree_open(void) {
    if (spot_live || !fb) return;
    int s = (int)window_scale(), w = SPOT_W * s, h = spot_hmax() * s;   /* the buffer Spotlight sizes; the tree's card is smaller */
    spot_lx = 8; spot_ly = sg(MENUBAR_H) / s + 2;
    if (!spot_under) spot_under = kmalloc((unsigned)(w * h) * 4);
    if (!spot_under) { uart_puts("oom treemenu\n"); return; }
    w = TREE_W * s; h = TREE_H * s;
    cur_hide();
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) spot_under[j * w + i] = fb[(unsigned)(spot_ly * s + j) * fb_pitch + (unsigned)(spot_lx * s + i)];
    spot_live = spot_tree = 1; tree_sel = 0; tree_about = 0;
    con_quiet = 1; uart_puts("treemenu open\n");
    tree_paint();
}
/* Log Out closes every window; with no accounts it says so on the menu bar, with accounts the login screen (login_ui.h) comes back. */
static void tree_close_all(void) {
    power_pending = 0; if (dock_sel >= 0) dock_select(-1);
    calc_close(); brick_close(); clock_close(); browser_close(); app_kill("logged out"); console_close();
}
void logout_request(void);   /* login_ui.h */
static void tree_logout(void) {
    con_quiet = 1; uart_puts("treemenu: logout\n");
    if (accounts_count()) { logout_request(); return; }   /* 4.9.0: with accounts the main loop closes everything and shows the login screen */
    tree_close_all();
    menubar_notify("Logged out");
}
static void tree_pick(int r) {
    if (r == 0) { tree_about = 1; tree_paint(); return; }
    spot_close();
    con_quiet = 1; uart_puts(r == 1 ? "treemenu: restart\n" : r == 2 ? "treemenu: shutdown\n" : "");
    if (r == 1) power_ask(0); else if (r == 2) power_ask(1); else tree_logout();
}
static void tree_key(unsigned code) {
    if (tree_about || code == 1) { spot_close(); return; }   /* About closes on any key; Esc closes the menu */
    if (code == 103) { tree_sel = (tree_sel + 3) % 4; tree_paint(); }
    else if (code == 108) { tree_sel = (tree_sel + 1) % 4; tree_paint(); }
    else if (code == 28 || code == 96) tree_pick(tree_sel);
}
static void tree_click(int lx, int ly) {
    if (tree_about || !in_box(lx, ly, spot_lx, spot_ly, TREE_W, 109)) { spot_close(); return; }   /* outside the card, or any click on About, dismisses */
    for (int r = 0; r < 4; r++) if (in_box(lx, ly, spot_lx + 8, tree_ry(r), TREE_W - 16, SPOT_ROW)) { tree_sel = r; tree_pick(r); return; }
}
static void tree_hover(int lx, int ly) {
    if (tree_about) return;
    for (int r = 0; r < 4; r++) if (in_box(lx, ly, spot_lx + 8, tree_ry(r), TREE_W - 16, SPOT_ROW) && r != tree_sel) { tree_sel = r; tree_paint(); return; }
}
void tui_log(const char *s) { con_quiet = 1; uart_puts(s); }   /* ask.c: the chat view's "tui| ..." lines, UART only (one line, newline included) */
int volume_key(unsigned code);   /* cmd.c: F11 and F12, Volume Down, Up and Mute */
static int ui_key(unsigned code) {   /* a key down, before any pane sees it: 1 if the desktop took it */
    int chord = ctrl_held || gui_held || alt_held;
    if (power_key(code)) return 1;
    if (volume_key(code)) return 1;   /* before Spotlight and every pane, like the Spotlight shortcut */
    if (gui_held && (code == 17 || code == 16)) return front_close();   /* Cmd+W closes the front window, Cmd+Q quits the front app: every window here is one app's, so both end it */
    if (code == 59 || (gui_held && code == 57)) { if (spot_live) spot_close(); else spot_open(); return 1; }   /* F1 or Cmd+Space toggle, like a Mac; Ctrl/Alt+Space and a bare Space do nothing (Joshua, 2026-10-11, on the board) */
    if (code == 68 || (ctrl_held && alt_held && code == 20)) { int was = spot_live && spot_tree; spot_close(); if (!was) tree_open(); return 1; }   /* F10 or Ctrl+Option+T: the tree menu, for a board with no mouse; again closes it */
    if (code == 60 || (ctrl_held && code == 20)) { spot_close(); settings_close(); brick_close(); clock_close(); browser_close(); pane_open(&term_p); return 1; }   /* F2, Ctrl+T: the Terminal in front, an open Calculator behind it */
    if (spot_live) return spot_key(code);
    if (brick_live) return brick_key(code, 1);
    if (clock_live) { if (code == 1) clock_close(); return 1; }
    if (settings_live) return settings_key(code);
    if (browser_live) return browser_key(code);
    if (calc_front || app_front) return 0;
    if (code == 61 && term_front()) {   /* F3: the other Terminal session; never move a running request. */
        unsigned next = term_index ^ 1;
        if (ask_select(next)) {
            term_index = next;
            pane_open(&term_p);
            uart_puts(next ? "terminal session 2\n" : "terminal session 1\n");
        }
        return 1;
    }
    if (term_front() && (ctrl_held || gui_held) && ask_edit(code)) return 1;   /* Cmd or Ctrl with C X V A on the input line */
    if (term_front()) { if (code == 1 && !ask_pending()) { console_open(); return 1; } return 0; }   /* Esc: the keys back to the desktop */
    if (!fb) return 0;
    if (code == 105 || code == 106) { dock_select(dock_sel < 0 ? (code == 106 ? 0 : GUI_ICON_COUNT - 1) : (dock_sel + (code == 106 ? 1 : GUI_ICON_COUNT - 1)) % GUI_ICON_COUNT); return 1; }
    if ((code == 28 || code == 96) && dock_sel >= 0) { int slot = dock_sel; dock_select(-1); dock_activate(slot); return 1; }
    if (code == 1) { if (dock_sel >= 0) dock_select(-1); return 1; }   /* Esc with nothing open does nothing */
    /* The bare desktop: no pane has the keys (the Console is logs only). A letter or digit opens Spotlight with it
       typed, so a keyboard whose F-keys or Cmd never arrive still gets in. Space, Enter and Tab alone do nothing. */
    if (chord) return 0;
    int ch = ask_char(code);
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) { spot_open(); if (spot_live) spot_key(code); return 1; }
    return 0;
}

/* The dev card's key debug line (KEY_DEBUG, set by JT_KEY_DEBUG=1 builds only): for 3 seconds after a key, the menu bar
   shows the last raw HID report in hex ("m" in front for a media interface) and the decoded key, so a real-board test
   shows exactly what the keyboard sends. The UART gets the same line ("keydbg: ..."). Release builds compile it out. */
#ifdef KEY_DEBUG
#define KD_X 180                      /* on the 960x540 grid: the menu bar's middle, clear of Spotlight and the status half */
#define KD_W 296
static char kd_hex[48], kd_name[32]; static int kd_dirty, kd_shown; static unsigned long kd_until; static unsigned *kd_under;
static unsigned long kd_ms(void) {
    unsigned long f, c; __asm__ volatile ("mrs %0, cntfrq_el0\n mrs %1, cntpct_el0" : "=r"(f), "=r"(c));
    return c / ((f ? f : 54000000) / 1000);
}
void key_raw(const unsigned char *r, unsigned len, unsigned kind) {   /* xhci.c: every keyboard or media report */
    static unsigned prev; unsigned n = 0, set = 0;
    for (unsigned i = 0; i < len; i++) set += r[i] != 0;
    int release = set < prev || !set; prev = set;
    if (release) return;              /* fewer bytes set: a key let go; keep showing the press */
    if (kind == 3) { kd_hex[n++] = 'm'; kd_hex[n++] = ' '; }
    for (unsigned i = 0; i < len && n + 4 < sizeof kd_hex; i++) {
        kd_hex[n++] = "0123456789abcdef"[r[i] >> 4]; kd_hex[n++] = "0123456789abcdef"[r[i] & 15]; kd_hex[n++] = ' ';
    }
    kd_hex[n] = 0; kd_name[0] = 0; kd_dirty = 1;
}
static void kd_cat(const char *t) { unsigned n = 0; while (kd_name[n]) n++; while (*t && n + 1 < sizeof kd_name) kd_name[n++] = *t++; kd_name[n] = 0; }
static void kd_key(unsigned code) {   /* a key down: its name, with the modifiers held */
    static const char *const fk[] = { "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10" };
    kd_name[0] = 0;
    if (ctrl_held && code != 29 && code != 97) kd_cat("Ctrl+");
    if (gui_held && code != 125 && code != 126) kd_cat("Cmd+");
    if (alt_held && code != 56 && code != 100) kd_cat("Alt+");
    int ch = ask_char(code); char one[2] = { (char)ch, 0 };
    kd_cat(code >= 59 && code <= 68 ? fk[code - 59] : code == 87 ? "F11" : code == 88 ? "F12" : code == 57 ? "Space" :
           code == 28 || code == 96 ? "Enter" : code == 1 ? "Esc" : code == 15 ? "Tab" : code == 14 ? "Backspace" :
           code == 29 || code == 97 ? "Ctrl" : code == 125 || code == 126 ? "Cmd" : code == 56 || code == 100 ? "Alt" :
           code == 42 || code == 54 ? "Shift" : code == 58 ? "Caps" : code == 103 ? "Up" : code == 108 ? "Down" : code == 105 ? "Left" :
           code == 106 ? "Right" : ch > ' ' && ch < 127 ? one : "?");
    if (kd_name[0] == '?') { kd_name[0] = 0; kd_cat("code "); char d[4] = { (char)('0' + code / 100 % 10), (char)('0' + code / 10 % 10), (char)('0' + code % 10), 0 }; kd_cat(d); }
    kd_dirty = 1;
}
static void keydbg_tick(void) {   /* the poll loop: draw a new line, or take an old one down after 3 seconds */
    if (!fb) return;
    int x = sg(KD_X), w = sg(KD_W), h = sg(MENUBAR_H) - 1;
    if (kd_shown && !kd_dirty && kd_ms() > kd_until) {
        cur_hide();
        for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) fb[(unsigned)j * fb_pitch + (unsigned)(x + i)] = kd_under[j * w + i];
        fb_flush(x, 0, w, h);
        kd_shown = 0;
    }
    if (!kd_dirty) return;
    kd_dirty = 0;
    if (!kd_under) kd_under = kmalloc((unsigned)(w * h) * 4);
    if (!kd_under) return;
    cur_hide();
    if (!kd_shown) for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) kd_under[j * w + i] = fb[(unsigned)j * fb_pitch + (unsigned)(x + i)];
    fb_rect(x, 0, w, h, 0x00141311);
    char line[96]; unsigned n = 0;
    for (const char *t = "key "; *t; ) line[n++] = *t++;
    for (const char *t = kd_hex; *t && n < 80; ) line[n++] = *t++;
    for (const char *t = kd_name; *t && n < 95; ) line[n++] = *t++;
    line[n] = 0;
    int s = (int)window_scale();
    gui_text(line, x / s + 6, (h / s - 13) / 2, 0x00faf8f4);
    fb_flush(x, 0, w, h);
    con_quiet = 1; uart_puts("keydbg: "); uart_puts(line); uart_putc('\n');
    kd_shown = 1; kd_until = kd_ms() + 3000;
}
#else
void key_raw(const unsigned char *r, unsigned len, unsigned kind) { (void)r; (void)len; (void)kind; }
static void kd_key(unsigned code) { (void)code; }
static void keydbg_tick(void) {}
#endif
#ifdef KEY_SELFTEST
/* keydbg-kernel8.elf only (arm64-keydbg-check.py): made-up HID reports through xhci.c's parser, since QEMU's usb-kbd
   never sends a report ID. Each step prints a marker, then the UART shows what the keys did. */
void hid_kbd(const unsigned char *r, unsigned len, unsigned char *p);
void hid_media(const unsigned char *r, unsigned len);
static void key_selftest(void) {
    static unsigned char last[8];
    static const unsigned char id_ctrl_t[9] = { 2, 0x01, 0, 0x17 }, id_none[9] = { 2 }, id_esc[9] = { 2, 0, 0, 0x29 };   /* ID 2 reads as Shift if not skipped */
    static const unsigned char cmd_space[8] = { 0x08, 0, 0x2C }, ctrl_space[8] = { 0x01, 0, 0x2C }, space[8] = { 0, 0, 0x2C }, none[8] = { 0 }, esc[8] = { 0, 0, 0x29 };
    uart_puts("keytest: id9 ctrl+t\n"); hid_kbd(id_ctrl_t, 9, last); hid_kbd(id_none, 9, last);
    uart_puts("keytest: id9 esc\n"); hid_kbd(id_esc, 9, last); hid_kbd(id_none, 9, last);
    uart_puts("keytest: boot8 space\n"); hid_kbd(space, 8, last); hid_kbd(none, 8, last);                 /* a bare Space and Ctrl+Space must not open Spotlight */
    uart_puts("keytest: boot8 ctrl+space\n"); hid_kbd(ctrl_space, 8, last); hid_kbd(none, 8, last);
    uart_puts("keytest: boot8 cmd+space\n"); hid_kbd(cmd_space, 8, last); hid_kbd(none, 8, last);
    uart_puts("keytest: boot8 esc\n"); hid_kbd(esc, 8, last); hid_kbd(none, 8, last);
    static const unsigned char br_down[3] = { 3, 0x70, 0 }, br_up[3] = { 3, 0x6F, 0 };   /* a Mac-mode F1 and F2 */
    uart_puts("keytest: media f1\n"); hid_media(br_down, 3);
    uart_puts("keytest: media esc\n"); hid_kbd(esc, 8, last); hid_kbd(none, 8, last);
    uart_puts("keytest: media f2\n"); hid_media(br_up, 3);
    uart_puts("keytest: media esc2\n"); hid_kbd(esc, 8, last); hid_kbd(none, 8, last);
    static const unsigned char vol_dn[3] = { 3, 0xEA, 0 }, vol_up[3] = { 3, 0xE9, 0 }, vol_mute[3] = { 3, 0xE2, 0 };
    uart_puts("keytest: media vol down\n"); hid_media(vol_dn, 3);
    uart_puts("keytest: media vol up\n"); hid_media(vol_up, 3);
    uart_puts("keytest: media mute\n"); hid_media(vol_mute, 3);
    uart_puts("keytest: media unmute\n"); hid_media(vol_mute, 3);
    uart_puts("keytest: done\n");
}
#endif

unsigned long net_clock_utc(void);   /* ip.c: UTC seconds once the network has told us, 0 before */
#include "notice.h"
static unsigned notice_now(void) {
    unsigned long f, c;
    __asm__ volatile ("mrs %0, cntfrq_el0\n mrs %1, cntpct_el0" : "=r"(f), "=r"(c));
    return (unsigned)(c / ((f ? f : 54000000) / 100));
}
static struct menu_notice notice;
static unsigned *mb_save; static int mb_h, mb_x0;
static int mb_hit[4][2];   /* the status icons' columns in physical pixels: power, sound, Wi-Fi, clock (menubar_wifi sets them) */
static int mb_wifi = -1;   /* the Wi-Fi state last shown (0 off, 1 working, 2 connected), so the clock can redraw alone each minute */
static int clock_minute = -1;
/* Vancouver stays UTC-7 from March 8, 2026. Earlier dates retain the seasonal rule. */
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
static long clock_offset(unsigned long utc) {   /* Vancouver: 7 h behind UTC in daylight time, 8 h otherwise */
    long y; unsigned m, d; civil_from_days(utc / 86400, &y, &m, &d);
    int dst = utc >= civil_days(2026, 3, 8) * 86400UL + 10 * 3600UL
           || (utc >= nth_sunday_utc(y, 3, 2, 10) && utc < nth_sunday_utc(y, 11, 1, 9));
    return (dst ? 7 : 8) * 3600L;
}
static void clock_local(unsigned long utc, unsigned *hh, unsigned *mm, unsigned *mon, unsigned *day) {
    long y;
    unsigned long loc = utc - (unsigned long)clock_offset(utc);
    civil_from_days(loc / 86400, &y, mon, day);
    clock_year = y;
    *hh = (unsigned)(loc % 86400) / 3600; *mm = (unsigned)(loc % 3600) / 60;
}
#include "clock_ui.h"
#include "brick_ui.h"
#include "browser_ui.h"
#include "glyphs.h"
#include "settings_ui.h"
/* The right end of the menu bar: the clock in 12-hour form, and left of it the Wi-Fi fan, no name. The fan, the speaker and the power
   symbol are all flat ink, the same 0x001C1C1E as the clock, in every state.
   Each arc is anti-aliased (4x4 samples a pixel, blended onto the bar), so it stays crisp at scale 1 and 2. Each call puts the
   saved bare bar back first, so old pixels never show through. */
#define WIFI_INK 0x001C1C1E
static int mb_mic;
void menubar_mic(int on) {   /* ask.c: voice mode started or ended; the bar is redrawn with or without the mark */
    if (mb_mic == (on != 0)) return;
    mb_mic = on != 0;
    uart_puts(mb_mic ? "mic mark on\n" : "mic mark off\n");
    if (!fb || !text_ok) return;
    cur_hide(); menubar_wifi(mb_wifi); cur_show();
}
void menubar_wifi(int state) {
    mb_wifi = state;   /* kept even before the desktop exists (the boot splash), so its first paint shows the right fan */
    if (!fb || !text_ok || login_live) return;   /* the login screen is over the bar: wall_repaint draws it again when it goes */
    int W = (int)fb_w, H = (int)fb_h, mb = sg(MENUBAR_H);
    if (mb_save) for (int y = 0; y < mb_h; y++) for (int x = mb_x0; x < W; x++) fb[(unsigned)y * fb_pitch + (unsigned)x] = mb_save[y * (W - mb_x0) + (x - mb_x0)];
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
    int R = sg(11), th = sg(3) / 2 < 2 ? 2 : sg(3) / 2, dr = sg(3) / 2, ox = cx - sg(16) - R, oy = mb / 2 + sg(5);   /* the classic fan: a dot, then three arcs above it */
    unsigned ink = fb_color(WIFI_INK);   /* the clock's ink in every state, joined or not (3.19.1: no grey icons) */
    for (int y = -R - 1; y <= dr + 1; y++) for (int x = -R - 1; x <= R + 1; x++) {
        int hit = 0;
        for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) {   /* a sample's offset from the dot's centre, in eighths of a pixel */
            int X = 8 * x + 2 * i - 3, Y = 8 * y + 2 * j - 3, d2 = X * X + Y * Y, in = d2 <= 64 * dr * dr;
            if (!in && Y < 0 && X <= -Y && -X <= -Y) for (int k = 1; k <= 3; k++) { int ro = 8 * sg(3 * k + 2) - 4, ri = ro - 8 * th; if (d2 <= ro * ro && d2 >= ri * ri) in = 1; }   /* 45 degrees either side of straight up */
            hit += in;
        }
        if (!hit) continue;
        unsigned *d = fb + (unsigned)(oy + y) * fb_pitch + (unsigned)(ox + x), o = 0;
        for (int b = 0; b <= 16; b += 8) o |= (((ink >> b & 0xFF) * (unsigned)hit + (*d >> b & 0xFF) * (16u - (unsigned)hit)) / 16) << b;
        *d = o;
    }
#ifdef WIFI_ICON_TEST   /* the wifiicon check's builds: where the fan is, for arm64-quietboot-check.py to look at its pixels */
    uart_puts("wifi icon "); uart_dec((unsigned)ox); uart_putc(','); uart_dec((unsigned)oy); uart_putc(','); uart_dec((unsigned)R); uart_putc('\n');
#endif
    /* 3.13.0: two more status icons left of the fan, a speaker (dim and without waves at volume 0) and a power symbol. A click on
       any of them, the fan and the clock too, opens Settings at its section (menubar_click). */
    int isz = sg(16), sx = ox - R - sg(14) - isz, wx = sx - sg(12) - isz, iy = (mb - 1 - isz) / 2, vol = audio_volume();
    glyph_paint(GL_SOUND, vol == 0 ? 0 : vol <= 50 ? 1 : 2, sx, iy, isz, WIFI_INK);   /* ink at every volume: the waves say the level, not a grey */
    glyph_paint(GL_POWER, 0, wx, iy, isz, WIFI_INK);
    if (mb_mic) {   /* voice mode is listening (ask.c): a terracotta microphone left of the power symbol, a capsule on a stem */
        int mx = wx - sg(12) - isz, cw = isz * 2 / 5, ch = isz * 3 / 5, cx0 = mx + (isz - cw) / 2;
        unsigned t = fb_color(0x00B5502C);
        fb_rect(cx0, iy, cw, ch, t);
        fb_rect(mx + isz / 2 - sg(1), iy + ch, sg(2), isz - ch - sg(2), t);
        fb_rect(cx0 - sg(1), iy + isz - sg(2), cw + sg(2), sg(2), t);
    }
    mb_hit[0][0] = wx - sg(5); mb_hit[0][1] = wx + isz + sg(5);         /* power */
    mb_hit[1][0] = sx - sg(5); mb_hit[1][1] = sx + isz + sg(5);         /* sound */
    mb_hit[2][0] = ox - R - sg(5); mb_hit[2][1] = ox + R + sg(5);       /* Wi-Fi */
    mb_hit[3][0] = cx - sg(5); mb_hit[3][1] = W;                        /* the clock */
    {   static int said[4][2];   /* where the icons are, said when they move (the check clicks them; the clock's width changes with the minute) */
        int moved = 0;
        for (int i = 0; i < 4; i++) for (int j = 0; j < 2; j++) if (said[i][j] != mb_hit[i][j]) { said[i][j] = mb_hit[i][j]; moved = 1; }
        if (moved) {
            static const char *const nm[4] = { "power", "sound", "wifi", "clock" };
            con_quiet = 1; uart_puts("menubar icons");
            for (int i = 0; i < 4; i++) { uart_putc(' '); uart_puts(nm[i]); uart_putc(' '); uart_dec((unsigned)mb_hit[i][0]); uart_putc('-'); uart_dec((unsigned)mb_hit[i][1]); }
            uart_putc('\n');
        }
    }
    int nx = mb_x0 + sg(12), nw = wx - sg(16) - nx;
    if (notice.text[0] && nw > 0)
        text_draw(2, notice.text, nx, (mb + sg(8)) / 2, sg(120), fb_color(0x001C1C1E), fb, fb_pitch, nx + nw, H);
    fb_flush(mb_x0, 0, W - mb_x0, mb);
}
static int menubar_click(int x, int y) {
    static const int section[4] = { SET_ABOUT, SET_SOUND, SET_ABOUT, SET_TIME };
    static const char *const name[4] = { "power", "sound", "wifi", "clock" };
    if (!fb || y >= mb_h) return 0;
    for (int i = 0; i < 4; i++) if (mb_hit[i][1] > mb_hit[i][0] && x >= mb_hit[i][0] && x < mb_hit[i][1]) {
        con_quiet = 1; uart_puts("menubar click "); uart_puts(name[i]); uart_putc('\n');
        settings_open(section[i]);
        return 1;
    }
    return 0;
}
void menubar_notify(const char *message) {
    notice_set(&notice, message, notice_now());
    if (login_live) { uart_puts("notice: "); uart_puts(notice.text); uart_putc('\n'); return; }
    cur_hide(); menubar_wifi(mb_wifi); cur_show();
    if (settings_live && !spot_live) set_paint();   /* the volume keys come through here: the Sound page shows the new level */
    uart_puts("notice: "); uart_puts(notice.text); uart_putc('\n');
}
static void cal_tick(void) {   /* the day has changed (or the clock just arrived): the Calendar tile again, the hover band's copy with it */
    if (!fb || login_live || cal_today() == cal_shown) return;
    int s = (int)window_scale(), h = hover_slot, top = gui_dock_band_top() * s, rows = (int)fb_h - top;
    if (dock_band) dock_hover(-1);   /* the plain band back first, so the copy below holds no label */
    cur_hide();
    dock_tile(3, 0);   /* slot 3, Calendar */
    fb_flush(gui_slot_x(3) * s, gui_dock_y0() * s, DOCK_ICON * s + s, (DOCK_PAD + DOCK_ICON) * s + s);
    if (dock_band) for (unsigned i = 0; i < (unsigned)rows * fb_pitch; i++) dock_band[i] = fb[(unsigned)top * fb_pitch + i];
    cur_show();
    if (h >= 0) dock_hover(h);
    uart_puts("calendar tile redrawn\n");
}
void menubar_tick(void) {   /* the poll loop calls this; it redraws only when the minute has changed */
    if (notice_expire(&notice, notice_now())) {
        cur_hide(); menubar_wifi(mb_wifi); cur_show();
        uart_puts("notice cleared\n");
    }
    cal_tick();
    clock_tick();
    if (settings_live && !spot_live && set_sec == SET_TIME && cal_utc() / 60 != (unsigned long)set_minute) set_paint();   /* the Date and Time page follows the minute */
    unsigned long u = net_clock_utc();
    if (!u || mb_wifi < 0) return;
    unsigned h, m, mo, dd; clock_local(u, &h, &m, &mo, &dd);
    if ((int)(h * 60 + m) != clock_minute) menubar_wifi(mb_wifi);
}
/* The Pi's boot splash (QUIET_BOOT): from the first frame a solid light grey screen with the crisp tree mark centred and a thin
   grey line under it, one sixth longer as each stage of boot finishes (fb, sd, wifi, clock, bt, desktop). Nothing else is drawn,
   not the desktop, not the Console, until splash_finish: Wi-Fi joined or given up, the clock synced or given up, Bluetooth
   up or given up. Then fb_desktop paints the desktop once. The UART says `splash step N name` and `boot done`. */
#ifdef QUIET_BOOT
static int splash_pm;   /* the bar's fill, in thousandths of the track */
static void splash_wait_ms(unsigned ms) {
    unsigned long f, c, end; __asm__ volatile ("mrs %0, cntfrq_el0\n mrs %1, cntpct_el0" : "=r"(f), "=r"(c));
    end = c + (f ? f : 54000000) / 1000 * ms;
    do __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(c)); while (c < end);
}
static void splash_draw(int pm) {   /* 3.9.8: the fill is any width, not one of six */
    int W = (int)fb_w, H = (int)fb_h, bw = sg(140), bh = sg(4) > 2 ? sg(4) : 2, bx = (W - bw) / 2, by = H / 2 + sg(62);
    splash_pm = pm;
    fb_rect(bx, by, bw, bh, 0x00C8C8CC);              /* the track, a calm darker grey */
    fb_rect(bx, by, bw * pm / 1000, bh, 0x00636366);  /* the part of boot done, mid grey */
    fb_flush(bx, by, bw, bh);
}
static void splash_ease(int to) {   /* glide to a new fill in small frames, so the bar never jumps a third */
    int from = splash_pm;
    for (int i = 1; i <= 12; i++) {
        splash_draw(from + (to - from) * i / 12);
        uart_puts("splash bar "); uart_dec((unsigned)splash_pm); uart_putc('\n');
        splash_wait_ms(10);
    }
}
static void splash_creep(void) {   /* inside a long wait (the radio loading): move toward the next stage as time passes, never past it */
    static unsigned long t0;
    unsigned long f, c; __asm__ volatile ("mrs %0, cntfrq_el0\n mrs %1, cntpct_el0" : "=r"(f), "=r"(c));
    if (!splash_on) return;
    if (!t0) t0 = c;
    unsigned long ms = (c - t0) / ((f ? f : 54000000) / 1000);
    int pm = 667 + (int)(166 * ms / (ms + 6000));
    if (pm >= splash_pm + 4) splash_draw(pm);
}
static void splash_step(int n, const char *name) {
    if (!splash_on) return;
    splash_ease(1000 * n / 6);
    uart_puts("splash step "); uart_dec((unsigned)n); uart_putc(' '); uart_puts(name); uart_putc('\n');
#ifdef SPLASH_STEP_MS   /* the splash checks: hold each step so a screendump can catch it */
    splash_wait_ms(SPLASH_STEP_MS);
#endif
}
static void splash_begin(void) {
    int W = (int)fb_w, H = (int)fb_h;
    fb_rect(0, 0, W, H, 0x00E5E5E7);
    fb_flush(0, 0, W, H);
    splash_mark(W / 2, H / 2 - sg(30), sg(150), 0x001C1C1E, 0x00E5E5E7);
    splash_on = 1;
    uart_puts("splash up\n");
    splash_step(1, "fb");
}
#define SPLASH(n, name) splash_step(n, name)
#else
#define SPLASH(n, name) ((void)0)
#endif
static int desktop_up;   /* fb_desktop has painted the desktop once: a wallpaper change may repaint it */
static int text_early;         /* the deferred boot started the text engine for the login card */
static int desktop_deferred;   /* 4.10.0, the Pi's quiet boot: an account is on the card, so the login card goes up first and login_done paints the desktop */
static void menubar_base(void) {   /* the bar over bare wallpaper: half wallpaper, half white, per pixel, closed by a one pixel rule */
    int W = (int)fb_w, mb = sg(MENUBAR_H);
    for (int y = 0; y < mb - 1; y++) for (int x = 0; x < W; x++) {
        unsigned c = fb[(unsigned)y * fb_pitch + (unsigned)x];
        fb[(unsigned)y * fb_pitch + (unsigned)x] = ((c >> 1) & 0x007F7F7Fu) + 0x00808080u;   /* each colour lane: half itself plus half of 255 */
    }
    fb_rect(0, mb - 1, W, 1, MENUBAR_RULE);
}
static void menubar_keep(void) {   /* the bare right half of the bar, kept for menubar_wifi's redraws */
    int W = (int)fb_w;
    if (mb_save) for (int y = 0; y < mb_h; y++) for (int x = mb_x0; x < W; x++) mb_save[y * (W - mb_x0) + (x - mb_x0)] = fb[(unsigned)y * fb_pitch + (unsigned)x];
}
static void desktop_chrome(int band_y) {   /* the mark, the title, the status icons and the quiet line above the dock */
    int W = (int)fb_w, H = (int)fb_h, mb = sg(MENUBAR_H);
    if (text_ok) {
        menu_mark_paint(sg(24), mb / 2, sg(20), 0x001C1C1E);   /* the mark in the corner, the i386 menu bar's */
        text_draw(1, "Joshua Tree", sg(40), (mb + sg(8)) / 2, sg(130), fb_color(0x001C1C1E), fb, fb_pitch, W, H);   /* menu bar title: bold sans, 13 on the grid, ink */
        if (current_user[0]) text_draw(1, current_user, sg(40) + text_width(1, "Joshua Tree", sg(130)) + sg(14), (mb + sg(8)) / 2, sg(130), fb_color(0x00B5502C), fb, fb_pitch, W, H);   /* 4.9.0: who is signed in, after the title */
        menubar_wifi(mb_wifi < 0 ? 1 : mb_wifi);   /* replaces the ARM64 badge: wifi.c moves it along as the chip comes up */
        /* Steve Jobs died on 5 October 2011. Fifteen years on, one quiet line above the dock, ending on the title
           of the Steve Jobs Archive's book of his own words, which Joshua was reading that week. */
        const char *thanks = "Seduce. Vilify. Ignore.";
        int ar = sc(6), gapx = sc(7), tw = text_width(2, thanks, sc(110)), tx = (W - tw - 7 * ar / 2 - gapx) / 2, ty = band_y - sc(4);
        text_draw(2, thanks, tx, ty, sc(110), fb_color(0x00f0f4f8), fb, fb_pitch, W, H);
        {   /* a small bitten apple after the line: apple_mask scaled to 3.5 ar square, 4x4 samples a pixel, onto the flat fill */
            int S = 7 * ar / 2, x0 = tx + tw + gapx, y0 = ty - sc(4) + ar - S; unsigned ink = fb_color(0x00f0f4f8);
            for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
                int sum = 0;
                for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) sum += apple_mask[((y * 4 + j) * APPLE_N / (S * 4)) * APPLE_N + (x * 4 + i) * APPLE_N / (S * 4)];
                unsigned al = (unsigned)sum / 16, d = fb[(unsigned)(y0 + y) * fb_pitch + (unsigned)(x0 + x)], out = 0;
                if (!al) continue;
                for (int b = 0; b <= 16; b += 8) out |= ((((ink >> b) & 0xFF) * al + ((d >> b) & 0xFF) * (255 - al)) / 255) << b;
                fb[(unsigned)(y0 + y) * fb_pitch + (unsigned)(x0 + x)] = out;
            }
        }
    } else uart_puts(heap_oom ? "oom text\n" : "M1d text FAIL\n");
}
static void fb_desktop(void) {
    int W = (int)fb_w, H = (int)fb_h;
    dock_pct_boot = dock_scale_pct;
    int win_w = sc(500), win_h = sc(350), win_x = (W - win_w) / 2, win_y = sc(100);
    int mb = sg(MENUBAR_H);                               /* the menu bar's height on this screen: 26 on the 960x540 grid */
#ifndef QUIET_BOOT
    {   /* the boot screen while the photo decodes (the slow part): the tree on off-white and a thin bar a third full,
           one of three steps done (screen up; the photo and the desktop follow). The wallpaper paints straight over it. */
        fb_rect(0, 0, W, H, 0x00faf8f4);
        menu_mark_paint(W / 2, H / 2 - sg(30), sg(110), 0x001C1C1E);
        int bw = sg(140), bh = sg(4) > 2 ? sg(4) : 2, bx = (W - bw) / 2, by = H / 2 + sg(50);
        fb_rect(bx, by, bw, bh, 0x00DDD8CE);
        fb_rect(bx, by, bw / 3, bh, 0x001C1C1E);
        fb_flush(0, 0, W, H);
    }
#endif
    int wall_ok = wall_paint(fb, fb_pitch, W, H, fb_swap);
    if (!wall_ok) fb_rect(0, 0, W, H, 0x00203040);   /* Navy (clrs.cc), see wall.c; this fallback is now unreachable */
    menubar_base();                                       /* the bar, half wallpaper and half white, closed by a one pixel rule */
    mb_h = mb - 1; mb_x0 = W / 2;                          /* keep the bare right half of the bar, for menubar_status() */
    mb_save = kmalloc((unsigned)(mb_h * (W - mb_x0)) * 4);
    menubar_keep();
    if (!text_early) text_ok = text_init();               /* before the window (its title); the deferred boot did it already for the login card */
    text_early = 0;
    dock_paint();                                         /* the i386 dock, while only the wallpaper is under it */
    int band_y = gui_dock_band_top() * (int)window_scale();   /* the top of the dock's band, room for a hover label */
    /* Slice 3: the Console wears the i386 window frame (gui_paint.c): rounded cream body on the wallpaper, traffic
       lights, centred name, a hairline under the title band. Its content well is white for the log. */
    int s = (int)window_scale(), lx = win_x / s, ly = win_y / s, lw = win_w / s, lh = win_h / s;
    win_lx = lx; win_ly = ly; win_lw = lw; win_lh = lh;
    off_reserve(lw * s, lh * s);
#ifdef BR_TRACE
    uart_puts(off_buf ? "browser: offscreen buffer reserved at boot\n" : "browser: offscreen buffer NOT reserved\n");
#endif
    dock_mem = kmalloc((unsigned)(((int)fb_h - gui_dock_band_top() * s) * (int)fb_pitch) * 4);
    /* Slice 5: the Calculator's own frame, over the right of the Console's so both show, clear of the line above the dock */
    calc_lw = 280; calc_lx = lx + lw - 150; if (calc_lx + calc_lw > W / s - 8) calc_lx = W / s - 8 - calc_lw;
    calc_ly = ly + 40; calc_lh = gui_dock_band_top() - 22 - calc_ly; if (calc_lh > 300) calc_lh = 300;
    /* M3c part 2: an EL0 app's window, placed like the Calculator's, then moved up and cut to fit above the dock's band */
    /* Scale 1 keeps 576x400. At scale 2 (1080p) an app is the Console's size plus a little: its 1x buffer is smoothed up to 2x,
       so a window as big as the 576x400 one would be soft and look huge beside the Terminal. */
    app_lw = s >= 2 ? 520 : 576; if (app_lw > W / s - 16) app_lw = W / s - 16;
    app_lx = lx + lw - 150; if (app_lx + app_lw > W / s - 8) app_lx = W / s - 8 - app_lw;
    {   int top = mb / s + 8, bottom = gui_dock_band_top() - 22;
        app_lh = s >= 2 ? 368 : 400; app_ly = ly + 40;
        if (app_ly + app_lh > bottom) app_ly = bottom - app_lh;
        if (app_ly < top) { app_ly = top; app_lh = bottom - top; }
    }
    app_cw = (unsigned)(app_lw - 16); app_ch = (unsigned)(app_lh - 40);
    for (unsigned i = 1; i < APP_SLOTS; i++) {   /* 4.0: the other slots open cascaded down and to the left, a title band apart, so a second window leaves the first's title showing */
        struct app *o = &apps[i];
        o->lw = app_lw; o->lh = app_lh; o->cw = app_cw; o->ch = app_ch;
        o->lx = app_lx - (int)i * 36; if (o->lx < 8) o->lx = 8;
        o->ly = app_ly + (int)i * 28; if (o->ly + o->lh > gui_dock_band_top() - 22) o->ly = gui_dock_band_top() - 22 - o->lh;
    }
    win_def[WT_MAIN][0] = lw; win_def[WT_MAIN][1] = lh; win_def[WT_CALC][0] = calc_lw; win_def[WT_CALC][1] = calc_lh; win_def[WT_APP][0] = app_lw; win_def[WT_APP][1] = app_lh;
#ifndef QUIET_BOOT
    console_frame();
#endif
    desktop_chrome(band_y);
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
    fb_flush(0, 0, W, H);   /* the whole still picture to the screen; con_glyph flushes as it goes from here */
    /* Blank spots, clear of any text: the bottom of the window's white well, the menu bar's rule, and the wallpaper (the
       design system's dark fill at three corners, not the old 0x203040 fallback). Grey and white read the same either
       byte order; the rule and the fill are checked in fb_color order. */
#ifdef QUIET_BOOT
    unsigned well = fb_color(wall_get());   /* no window was drawn: the spot where its well would end is bare wallpaper */
#else
    unsigned well = 0x00ffffff;
#endif
    int ok = fb[(unsigned)((ly + lh - 8) * s - 2) * fb_pitch + fb_w / 2] == well && fb[(unsigned)(mb - 1) * fb_pitch + fb_w / 2] == fb_color(MENUBAR_RULE);
    unsigned w0 = fb[(unsigned)sc(300) * fb_pitch + 50], w1 = fb[(unsigned)(H - sc(8)) * fb_pitch + fb_w - 50], w2 = fb[(unsigned)(mb + sc(40)) * fb_pitch + fb_w - 50];
    ok = ok && wall_ok && w0 == fb_color(wall_get()) && w1 == w0 && w2 == w0;
    uart_puts(ok ? "M1c fb ok\n" : "M1c fb FAIL\n");
    desktop_up = 1;
#ifdef QUIET_BOOT
    uart_puts("console quiet\n");
#endif
#ifdef DRAG_TEST   /* the dragtest build: a drag of 100 right and 60 down at boot, clamped like a real one, for arm64-drag-check.py at 1080p (scale 2) */
    pane_open(&con_p);   /* the Console is not up at boot: bring it up to be dragged */
    drag_on = 1; win_move_to(win_lx + 100, win_ly + 60); drag_end();
#endif
#ifdef WIN_GATE
#define APP_BOOT "calendar,toroid,notes"   /* the windows gate's three apps (Makefile: wingate) */
#endif
#ifdef HANG_TEST
#define APP_BOOT "calendar,notes"   /* 4.5: slot 0 hangs on its first instruction, slot 1 must keep going (Makefile: hang) */
#endif
#ifdef APP_BOOT   /* the apptest build: that app opened at boot, for arm64-appsize-check.py at 1080p (the Pi model has no USB to click with). 4.0: "a,b,c" opens each in turn, for arm64-windows-check.py */
    int boot_multi = 0;
    {   const char *list = APP_BOOT;
        for (const char *c = list; *c; c++) if (*c == ',') boot_multi = 1;
        if (boot_multi) {
            char one[24]; unsigned n = 0;
            for (const char *c = list;; c++) {
                if (*c == ',' || !*c) { one[n] = 0; app_dock(one); n = 0; if (!*c) break; }
                else if (n < sizeof one - 1) one[n++] = *c;
            }
        }
    }
    if (!boot_multi && !app_dock(APP_BOOT)) {   /* 3.9.0: the kernel-drawn windows too (arm64-sharp2x-check.py), then their frame on the UART: WINBOX name x y w h, logical */
        const char *nm = APP_BOOT; int bx, by, bw, bh;
        #define NM_IS(w) ({ const char *a_ = nm, *b_ = w; while (*a_ && *a_ == *b_) { a_++; b_++; } !*a_ && !*b_; })
        if (NM_IS("calculator")) { calc_open(); bx = calc_lx; by = calc_ly; bw = calc_lw; bh = calc_lh; }
        else {
            if (NM_IS("settings")) settings_open(0);
            else if (NM_IS("clock")) clock_open();
            else if (NM_IS("brick")) brick_open();
            else if (NM_IS("browser")) browser_open();
            else if (NM_IS("terminal")) pane_open(&term_p);
            else console_open();
            bx = win_lx; by = win_ly; bw = win_lw; bh = win_lh;
        }
        uart_puts("WINBOX "); uart_puts(nm); uart_putc(' '); uart_dec((unsigned)bx); uart_putc(' '); uart_dec((unsigned)by); uart_putc(' '); uart_dec((unsigned)bw); uart_putc(' '); uart_dec((unsigned)bh); uart_putc('\n');
    }
#endif
#ifdef AUDIO_TEST   /* the audiotest build: `beep` run at boot on virt, which has no PWM (arm64-audio-check.py) */
    void cmd_run(const char *q, unsigned n, void (*out)(const char *));
    cmd_run("beep", 4, 0);
#endif
#ifdef CALC_TEST   /* the calctest build: the Calculator open at boot, four sums typed into it (arm64-calc-check.py) */
    calc_open();
    static const char *const sums[] = {"sin(30deg)", "5!", "2^10", "1/0"};
    for (unsigned i = 0; i < 4; i++) { calc_press("C"); for (const char *p = sums[i]; *p; p++) calc_type(*p); calc_did(calc_type('\n')); }
    calc_press("Sci"); calc_press("M+"); calc_did(0);   /* end on the scientific pad, memory held, for the screendump */
#endif
}
/* A new wallpaper colour (Settings, or a saved choice read once the card is up): the whole desktop again in the same order
   fb_desktop painted it, over the buffers it already holds (the heap never frees, so nothing is allocated here). The wallpaper,
   the bar and the dock go first; the pixels saved under the Calculator's and an app's windows are plain wallpaper, so they are
   refilled with the new colour (a window closed later must not paint the old one back); the hover band is forgotten so its next
   copy is taken from the new dock. Whatever window was open is drawn again on top. */
static void wall_repaint(void) {
    int W = (int)fb_w, H = (int)fb_h, s = (int)window_scale();
    cur_hide();
    wall_paint(fb, fb_pitch, W, H, fb_swap);
    menubar_base();
    menubar_keep();
    dock_band = 0; hover_slot = -1; dock_sel = -1;
    dock_paint();
    desktop_chrome(gui_dock_band_top() * s);
    fb_flush(0, 0, W, H);
    cur_show();
}
void desktop_wall_changed(void) {   /* settings.c: the colour is already set */
    if (!fb || !desktop_up) return;
    wall_repaint();
    if (settings_live) set_paint();
    else if (con_live) { con_live = 0; pane_open(cp); }
}
static void fb_init(void) {
    if (!fb_setup()) return;
    if ((unsigned long)fb_pitch * fb_h * 4 <= FB_RAM_MAX) {   /* 4.3.0: draw into RAM from here on; fb_flush copies to the scanout */
        fb_scan = fb; fb = (unsigned *)FB_RAM_BASE;
        for (unsigned long i = 0; i < (unsigned long)fb_pitch * fb_h; i++) fb[i] = fb_scan[i];
    }
    mouse_x = fb_w / 2; mouse_y = fb_h / 2;               /* the pointer starts in the middle of the screen */
#ifdef QUIET_BOOT
    splash_begin();                                       /* the desktop waits for splash_finish */
#else
    fb_desktop();
#endif
}
#ifdef QUIET_BOOT
static void fb_diag(void);
static void splash_abort(void) {   /* a FAIL or oom line was printed under the splash: show the desktop now, boot carries on behind it */
    splash_on = 0;
    fb_desktop();
}
static void splash_finish(void) {   /* everything boot waits for is done: the one desktop paint (already made if a failure ended the splash) */
    uart_puts("boot done\n");
    if (splash_on) {
        splash_step(6, "desktop");
        splash_on = 0;
        if (accounts_count()) { desktop_deferred = 1; text_ok = text_init(); text_early = 1; }   /* the login card goes over the splash's colour; the desktop is first drawn after the sign-in */
        else fb_desktop();
    }
    if (!desktop_deferred) fb_diag();             /* it measures the console font, which the desktop's layout does */
}
#endif
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

/* ask.c's agent loop: [[open APP]] opens a dock app by name. Calculator, Console, Clock, Brick, Browser, Settings, Weather and Stocks are available; other names return 0. */
int app_open_name(const char *name) {
    if ((name[0] | 32) == 'r' && (name[1] | 32) == 'e' && (name[2] | 32) == 'm' && (name[3] | 32) == 'i' && (name[4] | 32) == 'n' && (name[5] | 32) == 'd' && (name[6] | 32) == 'e' && (name[7] | 32) == 'r' && (name[8] | 32) == 's' && !name[9]) return app_dock("Reminders");   /* 3.17.0: [[open Reminders]] */
    if ((name[0] | 32) == 'm' && (name[1] | 32) == 'a' && (name[2] | 32) == 'i' && (name[3] | 32) == 'l' && !name[4]) { dock_activate(2); return 1; }   /* 3.15.0: the Mail tile */
    static const char *const openable[] = {"calculator", "console", "clock", "brick", "browser", "settings", "weather", "stocks"};
    for (unsigned a = 0; a < 8; a++) {
        const char *w = openable[a]; unsigned i = 0;
        for (; w[i] && name[i] && (name[i] | 32) == w[i]; i++) ;
        if (!w[i] && !name[i]) { if (a == 0) calc_open(); else if (a == 2) clock_open(); else if (a == 3) brick_open(); else if (a == 4) browser_open(); else if (a == 5) settings_open(0); else if (a == 6) dock_activate(8); else if (a == 7) dock_activate(9); return 1; }
    }
    static const char *const finder[] = {"finder", "burrow", "files"};   /* 3.14.0: Samantha's [[open Finder]] starts user/finder.c, as the Burrow tile and Spotlight do */
    for (unsigned a = 0; a < 3; a++) {
        const char *w = finder[a]; unsigned i = 0;
        for (; w[i] && name[i] && (name[i] | 32) == w[i]; i++) ;
        if (!w[i] && !name[i]) { dock_activate(SPOT_FINDER); return 1; }
    }
    for (int s = 1; s <= SPOT_LAST; s++) {   /* 4.17.0: anything Spotlight lists and the Pi runs, by the name it shows (Notes, Music, Calendar, the games), the way Spotlight's Enter opens it */
        const char *w = spot_name(s); unsigned i = 0;
        for (; w[i] && name[i] && (name[i] | 32) == (w[i] | 32); i++) ;
        if (!w[i] && !name[i] && lp_on_arm(s)) { dock_activate(s); return 1; }
    }
    return 0;
}

/* ---- Input, from any driver: Linux evdev events (virtio input speaks them natively; the USB HID driver in xhci.c
   translates its reports into them). The pointer starts mid-screen; a tablet sets it, a mouse moves it. ---- */
#include "virtio.h"   /* struct input_event, and the virtio devices (moved to virtio.c in 4.23.2) */
int ask_key(unsigned code, unsigned value);   /* ask.c: the prompt's line editor */
int ask_secret(void);                         /* ask.c: the line being typed holds a password */
int ask_char(unsigned code);                  /* ask.c: a key code as the character it types, Shift included */
void ask_poll(void);
#ifdef PI_BUILD
#include "screen_idle.h"
#endif
#include "login_ui.h"   /* 4.9.0: the login screen, over everything while JT/USERS.TXT has an account and nobody has signed in */
/* Key repeat. A USB keyboard (SET_IDLE(0)) reports only a change, so a held key sends one press and then nothing.
   The last typed key down (a typed character or Backspace; not a function key, Brick's keys, a modifier or a chord)
   is remembered with the generic counter's time. After KEY_REPEAT_DELAY_MS the main loop feeds it back through
   input_event as value 2, which every handler treats as a press, so it lands wherever the first press went (Terminal,
   Calculator, Spotlight), every KEY_REPEAT_RATE_MS, until its release or any other key or button goes down.
   The counter, never `ticks`, which stops counting. */
#define KEY_REPEAT_DELAY_MS 400
#define KEY_REPEAT_RATE_MS 80
static unsigned rep_code;            /* the key being repeated, 0 for none */
static unsigned long rep_next;       /* when it repeats next, in counter milliseconds */
static unsigned long key_ms(void) {
    unsigned long f, c; __asm__ volatile ("mrs %0, cntfrq_el0\n mrs %1, cntpct_el0" : "=r"(f), "=r"(c));
    return c / ((f ? f : 54000000) / 1000);
}
static void app_key(unsigned code) {   /* a key down for the app in front, as the codes user/jtsys.h gives every program */
    static const unsigned short map[][2] = { {1, 259}, {28, 258}, {96, 258}, {14, 8}, {15, 9}, {103, 256}, {108, 257},
                                             {105, 261}, {106, 262}, {102, 305}, {107, 306}, {111, 307} };
    int k = 0;
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) if (map[i][0] == code) k = map[i][1];
    if (!k && (ctrl_held || gui_held)) k = code == 46 ? 302 : code == 45 ? 303 : code == 47 ? 304 : code == 31 ? 308 : code == 30 ? 315 : 0;   /* Ctrl or Cmd with C X V S A */
    else if (!k) k = ask_char(code);
    if (k) app_push(1, k, 0);
}
void input_event(struct input_event e) {
#ifdef PI_BUILD
    if (e.type == 1 || e.type == 3 || (e.type == 2 && e.value)) screen_activity();   /* any key or button or a move wakes the screen */
#endif
    if (e.type == 1 && e.value < 2) {                                                                      /* a real press or release; value 2 is our own repeat */
        if (!e.value) { if (e.code == rep_code) rep_code = 0; }
        else if ((e.code == 14 || (e.code < 58 && ask_char(e.code))) && !(brick_live && !spot_live) && !ctrl_held && !gui_held && !alt_held) {
            rep_code = e.code; rep_next = key_ms() + KEY_REPEAT_DELAY_MS;
        } else rep_code = 0;
    }
    if (login_live) { login_event(e); return; }                                                            /* the login screen has every key; the mouse is dropped */
    if (e.type == 1 && e.code >= 272 && e.code <= 274) {                                                 /* BTN_LEFT, RIGHT, MIDDLE */
        con_quiet = 1;   /* the UART only */
        uart_puts("key "); uart_dec(e.code); uart_puts(e.value ? " down\n" : " up\n");
        if (e.code == 272 && e.value) pointer_click();
        else if (e.code == 272) drag_end();
        return;
    }
    if (e.type == 1 && (e.code == 29 || e.code == 97)) ctrl_held = e.value != 0;                          /* Ctrl, either side; the key still echoes below */
    if (e.type == 1 && (e.code == 125 || e.code == 126)) gui_held = e.value != 0;   /* Cmd (left 0x08, right 0x80 in a boot report) */
    if (e.type == 1 && (e.code == 56 || e.code == 100)) alt_held = e.value != 0;    /* Alt, Option on a Mac board */
    if (e.type == 1 && e.value == 1) kd_key(e.code);                                                       /* not our repeats: the debug line shows the real press */
    if (e.type == 1 && (e.code == 42 || e.code == 54)) ask_key(e.code, e.value);   /* Shift, before ui_key: Spotlight and the Calculator read its state from ask.c too, and take every key */
    if (e.type == 1 && e.code == 58) {                                                                     /* Caps Lock: flips ask.c's state, nothing else sees it */
        ask_key(e.code, e.value);
        con_quiet = 1;
        uart_puts("key 58 "); uart_puts(e.value ? "down\n" : "up\n");
        return;
    }
    if (e.type == 1 && e.value && ui_key(e.code)) {                                                      /* Spotlight, the dock keys, F2 and Ctrl+T */
        con_quiet = 1;
        if (!set_secret()) { uart_puts("key "); uart_dec(e.code); uart_puts(" down\n"); }   /* 4.14.0: not while Settings' Users pane takes a password */
        return;
    }
    if (e.type == 1 && brick_live && !spot_live) { brick_key(e.code, e.value); return; }
    if (e.type == 1 && app_front && e.code != 42 && e.code != 54) {   /* the app window in front: its keys go to its queue (Shift still to ask.c) */
        if (e.value) { con_quiet = 1; uart_puts("key "); uart_dec(e.code); uart_puts(" down\n"); app_key(e.code); }
        return;
    }
    if (e.type == 1 && e.value && calc_front && e.code != 42 && e.code != 54) {                           /* a key for the Calculator in front (Shift still goes to ask.c) */
        con_quiet = 1;
        uart_puts("key "); uart_dec(e.code); uart_puts(" down\n");
        if (e.code == 1) { calc_close(); return; }                                                          /* Esc */
        int ch = e.code == 15 ? '\t' : e.code == 14 ? '\b' : e.code == 28 || e.code == 96 ? '\n' : ask_char(e.code);
        if (ch) calc_did(calc_type(ch));
        return;
    }
    if (e.type == 1 && calc_front && e.code != 42 && e.code != 54) return;                                 /* its key ups */
    if (e.type == 1) {                                                                                     /* EV_KEY: keys */
        int held = cur_hold(), term = term_front();
        if (!con_key(e.code, e.value)) {   /* the scroll keys are not logged: that would add lines to the picture they move */
            /* Only the Terminal takes typing; Shift always reaches ask.c, as the Calculator reads its state there */
            if (term || e.code == 42 || e.code == 54) ask_key(e.code, e.value);
            con_quiet = 1;   /* typed keys show on the ask> row, or nowhere; the echo stays on the UART and off the Console */
            if (!(term && ask_secret()) && !set_secret()) { uart_puts("key "); uart_dec(e.code); uart_puts(e.value ? " down\n" : " up\n"); }   /* 4.10.0: not while a password is being typed */
        }
        cur_release(held);
    }
    else if (e.type == 3) {                                                                                /* EV_ABS: the tablet, 0..32767 */
        if (e.code == 0) mouse_x = e.value * fb_w / 32768; else if (e.code == 1) mouse_y = e.value * fb_h / 32768;
        mouse_moved = 1;
    } else if (e.type == 2) {                                                                              /* EV_REL: a mouse, clamped to the screen */
        int v = (int)e.value;
        if (e.code == 8) { wheel_acc += v; return; }   /* REL_WHEEL: only the Browser reads it, once per report (the SYN below), so a burst of notches is one scroll and one paint */
        if (e.code == 0) { int x = (int)mouse_x + v; mouse_x = x < 0 ? 0 : x >= (int)fb_w ? (int)fb_w - 1 : (unsigned)x; }
        else if (e.code == 1) { int y = (int)mouse_y + v; mouse_y = y < 0 ? 0 : y >= (int)fb_h ? (int)fb_h - 1 : (unsigned)y; }
        mouse_moved = 1;
    } else if (e.type == 0 && (mouse_moved || wheel_acc)) {                                                /* EV_SYN: one report done */
        if (wheel_acc) { int v = wheel_acc; wheel_acc = 0; browser_wheel(v); }
        if (!mouse_moved) return;
        con_quiet = 1;   /* the UART only: every report would scroll the Console */
        uart_puts("mouse "); uart_dec(mouse_x); uart_putc(','); uart_dec(mouse_y); uart_putc('\n');
        mouse_moved = 0;
        pointer_moved();
    }
}
void kinput(unsigned type, unsigned code, int value) { input_event((struct input_event){ (unsigned short)type, (unsigned short)code, (unsigned)value }); }
static void key_repeat_tick(void) {   /* the main loop, right after the polls: a release that queued while the loop was busy is seen first */
    if (!rep_code || key_ms() < rep_next) return;
    rep_next = key_ms() + KEY_REPEAT_RATE_MS;   /* from now, not from the last due time: a late loop never bursts */
    input_event((struct input_event){ 1, (unsigned short)rep_code, 2 });
}
int usb_init(void);    /* xhci.c */
int sd_mount(void);    /* emmc.c */
int wifi_init(void);
#ifdef PI_BUILD
#define WIFI_ON_PI 1
#else
#define WIFI_ON_PI 0
#endif   /* wifi.c: M4 Wi-Fi stage 1, polled, every wait bounded */
#ifdef WIN_GATE
/* wingate-kernel8.img only (arm64-windows-check.py): three apps booted, slot 1 is Toroid. A second after all three have drawn,
   the backquote it crashes on is queued for it; two seconds later the UART says which slots still have an app and how many
   frames each has presented since, so the check can see the other two kept running. */
static void wingate_tick(void) {
    static unsigned long t0; static int phase; static unsigned p0[APP_SLOTS];
    unsigned long now = cnt_now();
    if (phase == 0) {
        if (!(apps[0].shown && apps[1].shown && apps[2].shown)) return;
        t0 = now; phase = 1;
    } else if (phase == 1 && now > t0 + cnt_hz()) {
        struct app *keep = ap; ap = &apps[1]; app_push(1, '`', 0); ap = keep;
        for (unsigned i = 0; i < APP_SLOTS; i++) p0[i] = apps[i].presents;
        t0 = now; phase = 2;
    } else if (phase == 2 && now > t0 + 3 * cnt_hz()) {
        for (unsigned i = 0; i < 3; i++) { struct app *keep = ap; ap = &apps[i]; if (i != 1) app_push(1, ' ', 0); ap = keep; }   /* poke the survivors */
        t0 = now; phase = 3;
    } else if (phase == 3 && now > t0 + 2 * cnt_hz()) {
        con_quiet = 1; uart_puts("wingate:");
        for (unsigned i = 0; i < 3; i++) { uart_puts(" slot"); uart_dec(i); uart_puts(apps[i].cur ? " live " : " gone "); uart_dec(apps[i].presents - p0[i]); }
        uart_putc('\n'); phase = 4;
    }
}
#endif
#ifdef HANG_TEST
/* hangtest builds only (arm64-watchdog-check.py). Slot 0's Calendar spins on its first instruction; the watchdog must end it
   alone. Once it is gone, slot 1's Notes is poked and counted, and the UART says whether it kept presenting. */
static void hang_tick(void) {
    static unsigned long t0; static int phase; static unsigned p0;
    unsigned long now = cnt_now();
    if (phase == 0) { if (!apps[0].cur && apps[1].cur) { p0 = apps[1].presents; t0 = now; phase = 1; } }
    else if (phase == 1 && now > t0 + cnt_hz()) {
        struct app *keep = ap; ap = &apps[1]; app_push(1, ' ', 0); ap = keep;
        t0 = now; phase = 2;
    } else if (phase == 2 && now > t0 + cnt_hz()) {
        con_quiet = 1; uart_puts("hang: slot0 gone slot1 "); uart_puts(apps[1].cur ? "live " : "gone "); uart_dec(apps[1].presents - p0); uart_putc('\n'); phase = 3;
    }
}
#endif
void usb_poll(void);
#ifdef NOTES_SELFTEST
/* notesw-kernel8.img and notesr-kernel8.img only (arm64-notes-check.py). QEMU's raspi4b has no USB, so made-up boot
   keyboard reports go through xhci.c's hid_kbd, the parser a real keyboard's reports take, one key every 150 ms of the
   generic counter (never `ticks`). Right five times and Enter walk the dock to its fifth tile, Notes, the way a Pi with
   only a keyboard opens it. '|' waits for Notes' first frame. notesw then types n (a new note), a line and Esc (Esc
   saves it and goes back to the list); notesr presses Enter (the note opens in the editor). Then `notestest: done`, and
   Notes stays open for the check's screendump. */
void hid_kbd(const unsigned char *r, unsigned len, unsigned char *p);
static void notes_tick(void) {
    static const char *const script = NOTES_SELFTEST == 1 ? ">>>>>\n|nhello from the pi\033" : NOTES_SELFTEST == 2 ? ">>>>>\n|\n"
        : NOTES_SELFTEST == 3 ? ">>>>>>\n|buy milk\ncall mom\n^^ " : NOTES_SELFTEST == 5 ? ">>>>>\n|nautosaved with no clock!@" : ">>>>>>\n|";   /* 3, 4: Reminders (arm64-reminders-check.py): the sixth tile, two adds, Up Up, Space on the first; then just open it; 5: Notes with no clock set, a line typed and left, '!' waits four seconds, '@' clicks the red dot (arm64-notes-check.py) */
    static unsigned at; static unsigned long next; static unsigned char last[8]; static int done;
    unsigned long now = cnt_now();
    if (!next) next = now + cnt_hz();   /* a second after the loop starts: the desktop has settled */
    if (done || now < next) return;
    if (!script[at]) { done = 1; uart_puts("notestest: done\n"); return; }   /* a second after the last key: it has drawn */
    next = now + cnt_hz() / 1000 * 150;
    char c = script[at];
    if (c == '|') {
        if (!app_cur || !app_shown) return;   /* not up yet: ask again on the next step */
        uart_puts("notestest: notes is up\n");
        at++; next = now + 2 * cnt_hz();   /* two seconds to take a screendump of the list before the next key */
        return;
    }
    if (c == '!') { uart_puts("notestest: waiting\n"); at++; next = now + 4 * cnt_hz(); return; }   /* long enough for the two second autosave to fire, with Esc never pressed */
    if (c == '@') {   /* the red dot of the front app window, through the pointer's own click path */
        int s = (int)window_scale();
        mouse_x = (app_lx + 24) * s; mouse_y = (app_ly + 16) * s;
        uart_puts("notestest: red dot\n"); pointer_click();
        at++; next = now + 2 * cnt_hz(); return;
    }
    unsigned char down[8] = { 0 }, none[8] = { 0 };
    down[2] = c == '>' ? 0x4F : c == '\n' ? 0x28 : c == '\033' ? 0x29 : c == ' ' ? 0x2C : c == '^' ? 0x52 : (unsigned char)(0x04 + c - 'a');   /* HID usages: Right, Enter, Esc, Space, a to z */
    hid_kbd(down, 8, last); hid_kbd(none, 8, last);
    if (!script[++at]) next = now + cnt_hz();
}
#endif


void net_stack_demo(void);
void net_clock_sync(void);
int bt_starting(void);   /* bt.c: the radio is still loading its patch */
void tls_demo(void);
unsigned hear_fake(short *pcm, unsigned max) {   /* ask.c's `hear` on the heartest build: half a second of a 440 Hz square wave instead of a microphone; (0, 0) asks whether this is that build */
#ifdef HEAR_TEST
    if (!pcm) return 1;
    unsigned n = 8000 < max ? 8000 : max;
    for (unsigned i = 0; i < n; i++) pcm[i] = (short)(((i * 440 / 16000) & 1) ? 6000 : -6000);
    return n;
#else
    (void)pcm; (void)max; return 0;
#endif
}
void input_drain(void) { usb_poll(); virtio_input_poll(); key_repeat_tick(); audio_pump(); }   /* ask.c, between agent steps and in every wait while `sam` thinks: the keys that queued, a held key repeating, and the sound */

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
    if (virtio_net_init()) virtio_net_probe();
    if (virtio_blk_init()) virtio_blk_probe();
    net_stack_demo();   /* ip.c: DHCP and an HTTP POST through the shared stack, silent with no card */
    tls_demo();         /* ip.c: the HTTPS proof, only in a TLSPORT= build */
#ifndef QUIET_BOOT
    fb_diag();   /* last, so it is the newest line on the screen */
#endif   /* quiet boot: splash_finish prints it, once text_init has measured the font */
    int inputs = virtio_input_init();
    if (inputs) { uart_puts("M2 input ready, devices "); uart_dec((unsigned)inputs); uart_putc('\n'); }
    int usb_ok = usb_init();   /* USB first: the keyboard is the way in, and Wi-Fi bring-up is a blocking stretch of seconds on the real Pi */
#ifdef USB_AUDIO_TEST   /* the usbaudiotest build: a USB speaker plays 440 Hz for half a second at boot (arm64-usbaudio-check.py) */
    { int usb_spk_tone(int freq, int ms); usb_spk_tone(440, 500); }
#endif
    sd_mount();                /* emmc.c: the SD card's FAT partition for ls and cat; one `sd ...` line, every wait bounded */
    accounts_load();           /* accounts.c: JT/USERS.TXT, the names `users` lists */
    sudo_load();               /* sudo.c: JT/SUDOERS.TXT, who may sudo */
    settings_load();           /* settings.c: JT/SETTINGS.TXT, before the Pi's desktop is first painted (the splash waits) */
    SPLASH(2, "sd");
#ifdef SD_SELFTEST   /* the sd builds (arm64-sd-check.py) run the Terminal's own commands at boot on the check's card */
    {
        void cmd_run(const char *q, unsigned n, void (*out)(const char *));
        int emmc_read(unsigned lba, unsigned count, void *buf); int emmc_write(unsigned lba, unsigned count, const void *buf);
        unsigned long emmc_blocks(void); int blockdev_write_sector(unsigned lba, const void *buf);
        static char chain[1040] = "write CHAIN.TXT ";   /* 1020 digits: two one-sector clusters, linked */
        for (unsigned i = 0; i < 1020; i++) chain[16 + i] = (char)('0' + i % 10);
#if SD_SELFTEST == 1   /* sdtest: ls and cat, then two reads that must be refused */
        static const char *const line[] = { "ls", "cat config.txt", "cat BIG.TXT", "cat BAD.TXT", "cat NOPE.TXT" };
#elif SD_SELFTEST == 2   /* sdwtest: write, replace, delete, a folder, a chain and the refusals, then read it back */
        static const char *const line[] = { "cat JT/NOTE.TXT", "write NOTE.TXT the first line", "write NOTE.TXT it survives a reboot",
                                            "write GONE.TXT bye", "rm GONE.TXT", "mkdir LOGS", chain, "write config.txt hello",
                                            "write kernel8.img x", "write ../config.txt x", "rm start4.elf", "mkdir overlays",
                                            "ls JT", "cat JT/NOTE.TXT", "cat JT/GONE.TXT" };
#else   /* sdrtest: the same card after a reboot */
        static const char *const line[] = { "cat JT/NOTE.TXT", "rm NOTE.TXT", "rm CHAIN.TXT", "cat JT/NOTE.TXT", "ls JT" };
#endif
        for (unsigned i = 0; i < sizeof line / sizeof line[0]; i++) {
            unsigned n = 0; while (line[i][n]) n++;
            uart_puts("sdtest> "); uart_puts(line[i] == chain ? "write CHAIN.TXT (1020 digits)" : line[i]); uart_puts("\n");
            cmd_run(line[i], n, 0);
        }
        static unsigned char blk[4096], back[4096];
        unsigned last = (unsigned)emmc_blocks();
#if SD_SELFTEST == 1
        uart_puts("sdtest> read past the card\n");
        emmc_read(last, 1, blk);
#elif SD_SELFTEST == 2   /* CMD25: eight blocks at the card's very end, past the partition, only on the check's own card */
        uart_puts("sdtest> write past the card, and the boot sector\n");
        emmc_write(last, 1, blk);
        blockdev_write_sector(0, blk);
        uart_puts("sdtest> multi-block write\n");
        if (!emmc_read(2048, 1, blk) || blk[3] != 'J' || blk[4] != 'T' || blk[5] != 'C' || blk[6] != 'H') uart_puts("sdtest not the check's card, no raw write\n");
        else {
            for (unsigned i = 0; i < sizeof blk; i++) blk[i] = (unsigned char)(i * 7 + (i >> 9));
            int ok = emmc_write(last - 8, 8, blk) && emmc_read(last - 8, 8, back);
            for (unsigned i = 0; ok && i < sizeof blk; i++) if (back[i] != blk[i]) ok = 0;
            uart_puts(ok ? "sdtest multi-block write ok\n" : "sdtest multi-block write FAIL\n");
        }
#endif
        (void)last; (void)back;
        uart_puts("sdtest done\n");
    }
#endif
#ifdef BT_NOWIFI   /* the radio test build: Wi-Fi stays off so Bluetooth has the shared antenna to itself */
    menubar_wifi(0);
#else
    if (!wifi_init()) menubar_wifi(0);
#endif
#ifdef PI_BUILD
    if (wifi_nic_up()) { net_stack_demo(); net_clock_sync(); menubar_wifi(2); menubar_notify("Wi-Fi joined"); }   /* the address from the router, then the time */
#endif   /* prints `wifi ...` lines; on QEMU it ends at `wifi no host` and the desktop carries on */
    SPLASH(3, "wifi");
    SPLASH(4, "clock");   /* net_clock_sync ran inside the join above, or there was no network to ask */
    bt_init();   /* bt.c: powers the radio and returns; the main loop's bt_poll does the rest */
#ifdef QUIET_BOOT
    if (splash_on) {   /* the splash waits for the radio to load its patch (about half a minute on a Pi), or to say it will not: 60 s at most */
        unsigned long t0 = key_ms();
        while (bt_starting() && key_ms() - t0 < 60000) { bt_poll(); splash_creep(); }
    }
    SPLASH(5, "bt");
    splash_finish();
#endif
    login_show();   /* login_ui.h: with an account on the card the desktop waits behind the login screen; with none this does nothing */
#ifdef NOTICE_SELFTEST
    menubar_notify("Note kept in memory");
#endif
#if defined(WIFI_ICON_TEST) && WIFI_ICON_TEST == 2
    menubar_wifi(2);   /* a joined fan, which QEMU never reaches on its own */
#endif
#ifdef POWER_SELFTEST
    void hid_kbd(const unsigned char *, unsigned, unsigned char *);
    unsigned char previous[8] = {0};
    const unsigned char chord[8] = { 0x05, 0, POWER_SELFTEST == 1 ? 0x4D : 0x4C };
    const unsigned char none[8] = {0}, escape[8] = { 0, 0, 0x29 }, enter[8] = { 0, 0, 0x28 };
    hid_kbd(chord, 8, previous); hid_kbd(none, 8, previous);
    hid_kbd(escape, 8, previous); hid_kbd(none, 8, previous);
    uart_puts("power: cancellation survived\n");
    hid_kbd(chord, 8, previous); hid_kbd(none, 8, previous);
    hid_kbd(enter, 8, previous);
#endif
#ifdef POWER_FILE_TEST   /* tools/checks/arm64-power-check.py: boot 1 leaves a file unsaved and types `reboot`; boot 2 reads it back and types `shutdown` */
    {
        void cmd_run(const char *q, unsigned n, void (*out)(const char *));
        void ufile_selftest_dirty(const char *name, const char *text);
        cmd_run("cat JT/PWRBOOT.TXT", 18, grab);
        if (!has("boot-one")) {
            cmd_run("write PWRBOOT.TXT boot-one", 26, 0);
            ufile_selftest_dirty("PWR.TXT", "kept-by-flush");
            uart_puts("power: file left unsaved, typing reboot\n");
            cmd_run("reboot", 6, 0);
        } else {
            seen[0] = 0; cmd_run("cat JT/PWR.TXT", 15, grab);
            uart_puts(has("kept-by-flush") ? "power: file survived the reboot\n" : "power: file LOST in the reboot\n");
            cmd_run("shutdown", 8, 0);
        }
    }
#endif
#ifdef TREEMENU_SELFTEST   /* 1 restart, 2 shut down: the tree clicked open and driven by the board's own key events, then the power prompt confirmed */
    {
        int px = sg(12), py = sg(MENUBAR_H) / 2;
        kinput(3, 0, px * 32768 / (int)fb_w + 1); kinput(3, 1, py * 32768 / (int)fb_h + 1); kinput(0, 0, 0);
        kinput(1, 272, 1); kinput(1, 272, 0);
        kinput(1, 1, 1);   /* Esc closes the card */
        uart_puts(spot_live ? "treemenu: esc FAILED\n" : "treemenu: esc survived\n");
        kinput(1, 68, 1);   /* F10 opens it again */
        kinput(1, 108, 1); if (TREEMENU_SELFTEST == 2) kinput(1, 108, 1);
        kinput(1, 28, 1);   /* Enter on Restart or Shut Down: the Terminal's prompt */
        kinput(1, 28, 1);   /* Enter confirms: the watchdog */
    }
#endif
#ifdef KEY_SELFTEST
    key_selftest();
#endif
#ifdef PI_BUILD
    screen_last = notice_now();
#ifdef SCREEN_SELFTEST
    screen_last -= SCREEN_IDLE_CS; screen_tick();
    kinput(1, 42, 1); kinput(1, 42, 0);
    screen_last -= SCREEN_IDLE_CS; screen_tick();
    kinput(2, 0, 1); kinput(0, 0, 0);
    uart_puts("screen: input selftest done\n");
#endif
#endif
#if defined(NOTES_SELFTEST) || defined(APP_BOOT)
    usb_ok |= !inputs;   /* QEMU's raspi4b has no USB to find: the loop runs anyway, for notes_tick's made-up keys */
#endif
    if (usb_ok || inputs) {
        /* USB is polled, so nothing interrupts on its own: the virtual timer (INTID 27) wakes wfi every 2 ms. IRQs stay, so nothing interrupts on its own: the virtual timer (INTID 27) wakes wfi every 2 ms. IRQs stay
           masked around wfi (a pending one still wakes it) and the timer is stopped before they are let through again,
           so its handler never runs; the short unmask is for the virtio devices, whose handler acknowledges them. */
        *(volatile unsigned char *)(GICD_BASE + 0x400 + 27) = 0x80;
        GICD(0x100) = 1u << 27;
        unsigned long step = timer_step / 25;   /* timer_step is 50 ms */
#ifdef PI_BUILD
        (void)step;
        for (;;) {   /* nothing on the Pi sleeps: no wake source to trust yet, so spin and poll */
            usb_poll(); virtio_input_poll(); key_repeat_tick(); ask_poll(); bt_poll(); brick_tick(); menubar_tick(); keydbg_tick(); app_tick(); audio_pump(); screen_tick(); login_tick();   /* only the display sleeps: USB stays polled */
#ifdef NOTES_SELFTEST
            notes_tick();
#endif
#ifdef WIN_GATE
            wingate_tick();
#endif
#ifdef HANG_TEST
            hang_tick();
#endif
        }
#else
        for (;;) {   /* ask_pending: a `sam` queued by Enter is run by the next pass, and a wfi with no timer would sleep through it (the key-up IRQ was already taken) */
            if (usb_ok || brick_live || notice.text[0] || rep_code || bt_busy() || apps_live() || audio_busy() || ask_pending()) __asm__ volatile ("msr daifset, #2\n msr cntv_tval_el0, %0\n msr cntv_ctl_el0, %1\n isb\n wfi\n"
                              " msr cntv_ctl_el0, xzr\n isb\n msr daifclr, #2\n isb" :: "r"(step), "r"(1UL) : "memory");
            else __asm__ volatile ("wfi");
            usb_poll(); virtio_input_poll(); key_repeat_tick(); ask_poll(); bt_poll(); brick_tick(); menubar_tick(); keydbg_tick(); app_tick(); login_tick();   /* an app parked on yield runs again here */
#ifdef HANG_TEST
            hang_tick();
#endif
            audio_pump();   /* M3e: the app's sound, from its ring into the PWM's, never waiting */
        }
#endif
    }
    for (;;) __asm__ volatile ("wfe");
}
