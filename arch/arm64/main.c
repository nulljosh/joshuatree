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
static void uart_putc(char c) {
    con_tx(c);
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
    static const char *const skip[] = { "tick", "M0 ", "M1 ", "M1a ", "M1b ", "M1c fb ok", "M1d dock ", "M3 ", "M3b ", "M3c ", "EL0", "EL1", "booted at ",
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
#define ARENA_BLOCKS 8   /* M3e: the EL0 arena is 16 MiB, eight of those blocks... */
static unsigned long l3[ARENA_BLOCKS * 512] __attribute__((aligned(4096)));   /* ...each one a table of 4 KiB pages, back to back, so l3[page] works across all eight */
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
#define USER_BASE (HEAP_END <= ARENA_LOW ? ARENA_LOW : (HEAP_END + (2UL << 20) - 1) & ~((2UL << 20) - 1))
#define UP_CODE  0x0000UL   /* EL0 code, read-only and executable */
#define UP_STACK 0x2000UL   /* EL0 stack, one page; sp starts at its top */
#define UP_KERN  0x4000UL   /* a page in the same arena that only EL1 may touch */
#define UP_APP    0x10000UL /* M3c: the app window, where user_load puts a program from the app table: code pages, data pages, zeroed pages */
#define UP_ASTACK 0x80000UL /* ...and the app's stack: 16 pages (M3e: minimp3 keeps about 16 KB of scratch there), nothing mapped below them */
#define APP_STACK_PAGES 16
#define APP_ZERO_PAGES  8   /* zeroed pages after the image (and the rest of its last page): the pool user/calendar.c keeps its events in, and user/notes.c its 28 KB arena (M3d) */
#define UAPP_MAX 0x40000UL  /* the largest image user_load takes, and what user_c.ld asserts */
#define UP_WIN   0x90000UL  /* M3c part 2: the app's window buffer, read-write and never executable, a free page above the stack */
#define UWIN_MAX (0x200000UL - UP_WIN)   /* the rest of the arena's first 2 MiB: room for 560x360 four times over */
#define UP_HEAP  0x200000UL /* M3e: the app's brk heap, from the second block to the arena's end (14 MiB), EL0 read-write, never executable */
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
    for (int i = 0; i < ARENA_BLOCKS * 512; i++) l3[i] = 0;
    for (int k = 0; k < ARENA_BLOCKS; k++) l2[((USER_BASE - ((unsigned long)RAM_GIB << 30)) >> 21) + k] = (unsigned long)(l3 + k * 512) | 3;
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
   The arena is mapped 4 KiB pages inside the identity map. M3a: the test program's code (EL0 read and execute, nobody
   writes), its stack (EL0 read and write, never executable) and a kernel-only page (AP=00: EL1 only) holding a secret. The
   program is user.S, copied in. Syscalls use svc #0 with the number in x8 (Linux's: write 64, exit 93, and since M3b the
   Joshua Tree numbers of user/jtsys.h), arguments in x0 to x2 and the result in x0. A write only reads user memory that
   really is the program's own; anything else gets -EFAULT. A fault in EL0 kills the program and the kernel carries on.
   M3c: the same arena also holds the app window (UP_APP) and an app stack (UP_ASTACK). user_load puts a program from the app
   table there: code pages read-only and executable, then data pages and a zeroed pool read-write and never executable. */
extern char _start[];
extern char user_start[], user_end[], user_entry_a[], user_entry_b[], user_entry_c[], user_entry_d[], user_entry_e[];
extern unsigned long enter_user(unsigned long entry, unsigned long sp, unsigned long arg, unsigned long arg2);
extern void leave_user(unsigned long status) __attribute__((noreturn));
extern unsigned long resume_user(const struct frame *f, unsigned long sp, const unsigned long *fp);   /* vectors.S: back into a parked app (its FP registers too), returns like enter_user */
#define SYS_WRITE 64   /* the Linux numbers the user.S self-test uses */
#define SYS_EXIT 93
#define JT_SYS_EXIT 1    /* Joshua Tree's own, the ones user/jtsys.h and docs/SYSCALL-ABI.md give every program (M3b) */
#define JT_SYS_READ 3    /* M3d: the file calls, answered by ufile.c */
#define JT_SYS_WRITE 4
#define JT_SYS_TIME 13    /* M3c */
#define JT_SYS_GETPID 20
#define JT_SYS_SCHED_YIELD 158   /* M3c part 2 */
#define JT_SYS_WINDOW_OPEN 384
#define JT_SYS_WINDOW_POLL 385
#define JT_SYS_AUDIO 393         /* M3e: play, status, stop (audio.c holds the queue) */
#define JT_SYS_BRK 397           /* M3e: the heap top */
struct user_app { const char *name, *start, *end; };   /* userapps.S: one row per program, ended by a zero name */
extern const struct user_app user_apps[];
static unsigned long user_status;
static int user_faulted;    /* the last EL0 program ended on a fault, not an exit */
static int user_loud;       /* userrun: what the program writes reaches the screen too; the boot self-test keeps it on the UART */
static unsigned user_pid;   /* programs started so far, what JT_SYS_GETPID answers */
static unsigned long app_len, app_code, app_img;   /* the loaded app: bytes of window mapped, how many of those are code, and the image's own length */
static unsigned long cal_utc(void);   /* the clock the Calendar tile reads, further down: 0 until the network has told us the time */
/* M3c part 2, the scheduler's state: one app at a time, since the arena has one app window. app_cur is the app it holds,
   running or parked; app_frame and app_sp are where it parked (every register, its pc and its stack). Its window buffer is
   app_wbytes at UP_WIN, 0 with no window, and app_q is the ring of events the desktop queued for it. */
static const struct user_app *app_cur;
static struct frame app_frame;
static unsigned long app_fp[66];                 /* M3e: its q0-q31, FPCR and FPSR where it parked (vectors.S's save_fp layout) */
static unsigned long app_sp, app_last, app_wbytes;
static unsigned long app_brk;                    /* M3e: the heap top JT_SYS_BRK set, 0 while it has none */
static unsigned long app_close_at;               /* the red dot was clicked: past this count, an app that did not close itself is ended */
static unsigned long cnt_now(void) { unsigned long c; __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(c)); return c; }
static unsigned long cnt_hz(void) { unsigned long f; __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(f)); return f ? f : 54000000; }
static int app_running, app_parked, app_kick;   /* inside resume_user now; it yielded rather than ended; run it on the next pass */
static unsigned app_runs;                        /* times the desktop's loop resumed it, said when it ends */
#define APP_EVQ 16
static struct { unsigned kind; int a, b; } app_q[APP_EVQ];
static unsigned app_qh, app_qt;
static long app_window_open(unsigned long info);                    /* the window half, with the other windows further down */
static long app_window_poll(unsigned long ev, unsigned long flags);
static void app_window_close(void);
static long app_audio(unsigned long op, unsigned long arg, unsigned long size);   /* M3e: JT_SYS_AUDIO, with the app loader's byte readers */
static int span(unsigned long a, unsigned long n, unsigned long base, unsigned long len) {   /* [a, a+n) inside [base, base+len), written so a + n can never wrap */
    return n <= len && a >= base && a - base <= len - n;
}
long ufile_syscall(unsigned long n, unsigned long a0, unsigned long a1, unsigned long a2);   /* ufile.c, M3d: open, read, write (fd 3 and up), close, lseek, readdir, readfile, mkdir, unlink */
void ufile_release(void);                 /* ...every descriptor the app left open, closed and written back */
void ufile_mkdirs(const char *path);      /* ...folders inside JT/ an app expects */
static unsigned long heap_len(void) { return app_brk ? app_brk - (USER_BASE + UP_HEAP) : 0; }   /* M3e: the heap below its top */
int user_in_range(unsigned long a, unsigned long n) {   /* memory the program may have the kernel read: the M3a code and stack pages, and the loaded app's whole window, stack and heap */
    return span(a, n, USER_BASE + UP_CODE, 4096) || span(a, n, USER_BASE + UP_STACK, 4096)
        || span(a, n, USER_BASE + UP_APP, app_len) || span(a, n, USER_BASE + UP_ASTACK, app_len ? APP_STACK_PAGES * 4096UL : 0)
        || span(a, n, USER_BASE + UP_WIN, app_wbytes) || span(a, n, USER_BASE + UP_HEAP, heap_len());
}
int user_writable(unsigned long a, unsigned long n) {   /* memory the kernel may store into for the program: data, zeroed pool, stacks and heap, never code (read-only at EL1 too: a store there would be a kernel fault) */
    return span(a, n, USER_BASE + UP_STACK, 4096) || span(a, n, USER_BASE + UP_APP + app_code, app_len - app_code)
        || span(a, n, USER_BASE + UP_ASTACK, app_len ? APP_STACK_PAGES * 4096UL : 0) || span(a, n, USER_BASE + UP_WIN, app_wbytes)
        || span(a, n, USER_BASE + UP_HEAP, heap_len());
}
/* M3e: JT_SYS_BRK with i386's semantics (docs/SYSCALL-ABI.md, kernel/brk.c): 0 reads the top, a fresh app's is the heap's
   base; any other value sets it, to the byte. Pages between the old and new top (rounded up to whole pages) are mapped EL0
   read-write and never executable, then zeroed, so no earlier app's bytes are ever seen; shrinking unmaps them. A top below
   the base or past the arena's end is -EINVAL and nothing moves. The memory is the arena's own (identity mapped, 14 MiB),
   so there is nothing to run out of below that cap. app_unmap drops the whole heap when the app ends. */
static long app_brk_set(unsigned long want) {
    const unsigned long PAGE = 0x3UL | (1UL << 2) | (3UL << 8) | (1UL << 10), AP_EL0_RW = 1UL << 6, PXN = 1UL << 53, UXN = 1UL << 54;
    unsigned long base = USER_BASE + UP_HEAP, top = app_brk ? app_brk : base;
    if (!want) return (long)top;
    if (want < base || want > USER_BASE + ARENA_SIZE) return -22;
    unsigned long have = (top + 4095) & ~4095UL, need = (want + 4095) & ~4095UL;
    for (unsigned long v = have; v < need; v += 4096) l3[(v - USER_BASE) >> 12] = v | PAGE | AP_EL0_RW | PXN | UXN;
    for (unsigned long v = need; v < have; v += 4096) l3[(v - USER_BASE) >> 12] = 0;
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
                unsigned long t = cal_utc();
                r = (int)t;
                if (a0) {
                    if (!user_writable(a0, 4)) r = -14;   /* -EFAULT, like kernel/syscall.c's sys_time */
                    else for (int i = 0; i < 4; i++) ((unsigned char *)a0)[i] = (unsigned char)(t >> (8 * i));
                }
            } else if (n == JT_SYS_GETPID) r = user_pid ? user_pid : 1;
            else if (n == JT_SYS_WINDOW_OPEN) r = app_window_open(a0);
            else if (n == JT_SYS_AUDIO) r = app_audio(a0, f->x[1], f->x[2]);   /* M3e */
            else if (n == JT_SYS_BRK) r = app_brk_set(a0);
            else r = ufile_syscall(n, a0, f->x[1], f->x[2]);   /* M3d: the file calls; -ENOSYS (-38) for every number the ARM kernel does not answer yet, JT_SYS_AUDIO_RECORD (394) among them */
            if (n != JT_SYS_READ && n != JT_SYS_AUDIO) svc_log(n, a0, r);   /* a read is every few hundred bytes of a file, audio a hundred calls a second: the UART would drown */
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
    l3[UP_CODE >> 12] = (USER_BASE + UP_CODE) | PAGE | AP_EL0_RW | PXN | UXN;   /* writable for now, so the kernel can copy the program in */
    l3[UP_STACK >> 12] = (USER_BASE + UP_STACK) | PAGE | AP_EL0_RW | PXN | UXN;
    l3[UP_KERN >> 12] = (USER_BASE + UP_KERN) | PAGE | PXN | UXN;               /* AP=00: EL1 only */
    __asm__ volatile ("dsb ishst\n tlbi vmalle1\n dsb ish\n isb" ::: "memory");
    char *code = (char *)(USER_BASE + UP_CODE);
    for (unsigned long i = 0; i < len; i++) code[i] = img[i];
    const char *secret = "KERNEL-ONLY-SECRET";
    for (int i = 0; secret[i]; i++) ((char *)(USER_BASE + UP_KERN))[i] = secret[i];
    for (unsigned long a = (unsigned long)code & ~63UL; a < (unsigned long)code + len; a += 64)
        __asm__ volatile ("dc cvau, %0" :: "r"(a) : "memory");
    __asm__ volatile ("dsb ish\n ic iallu\n dsb ish\n isb" ::: "memory");
    l3[UP_CODE >> 12] = (USER_BASE + UP_CODE) | PAGE | AP_RO | PXN;             /* now read-only for everyone, executable by EL0 only */
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
/* M3e: JT_SYS_AUDIO, i386's contract (docs/SYSCALL-ABI.md): op 1 queues a struct jt_audio_play's PCM (at most 8192 bytes a
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
    app_len = app_code = app_img = app_wbytes = app_brk = 0;
    for (unsigned long i = UP_APP >> 12; i < ARENA_BLOCKS * 512; i++) l3[i] = 0;
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
    char *base = (char *)(USER_BASE + UP_APP);
    app_unmap();
    for (unsigned long i = 0; i < pages; i++) l3[(UP_APP >> 12) + i] = (USER_BASE + UP_APP + i * 4096) | PAGE | AP_EL0_RW | PXN | UXN;   /* writable while the kernel fills it in */
    for (unsigned long i = 0; i < APP_STACK_PAGES; i++) l3[(UP_ASTACK >> 12) + i] = (USER_BASE + UP_ASTACK + i * 4096) | PAGE | AP_EL0_RW | PXN | UXN;
    __asm__ volatile ("dsb ishst\n tlbi vmalle1\n dsb ish\n isb" ::: "memory");
    for (unsigned long i = 0; i < pages * 512; i++) ((unsigned long *)base)[i] = 0;   /* a stale program's bytes never reach this one */
    for (unsigned long i = 0; i < APP_STACK_PAGES * 512; i++) ((unsigned long *)(USER_BASE + UP_ASTACK))[i] = 0;
    for (unsigned long i = 0; i < len; i++) base[i] = a->start[i];
    for (unsigned long r = rela_s; r < rela_e; r += 24) {
        unsigned long off = rd64(base + r), info = rd64(base + r + 8), add = rd64(base + r + 16);
        if (info != 1027 /* R_AARCH64_RELATIVE */ || (off & 7) || off + 8 > len - 16 || add > pages * 4096) {
            uart_puts("M3c load FAIL "); uart_puts(a->name); uart_puts(": bad relocation\n"); app_unmap(); return 0;
        }
        *(unsigned long *)(base + off) = (unsigned long)base + add;
    }
    for (unsigned long p = (unsigned long)base & ~63UL; p < (unsigned long)base + code_end; p += 64) __asm__ volatile ("dc cvau, %0" :: "r"(p) : "memory");
    __asm__ volatile ("dsb ish\n ic iallu\n dsb ish\n isb" ::: "memory");
    for (unsigned long i = 0; i < code_end / 4096; i++) l3[(UP_APP >> 12) + i] = (USER_BASE + UP_APP + i * 4096) | PAGE | AP_RO | PXN;   /* code: read-only, EL0 may execute */
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
    user_pid++; user_faulted = 0;
    user_status = enter_user(entry, USER_BASE + UP_ASTACK + APP_STACK_PAGES * 4096UL, 0, 0);
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
static void app_end(unsigned long status, const char *how) {   /* an exit, a fault or a kill: the window goes, then the pages */
    const struct user_app *a = app_cur;
    app_cur = 0; app_parked = app_kick = 0; app_close_at = 0; user_status = status;
    ufile_release();   /* M3d: its open files are written back and closed, after an exit, a fault or its red dot alike */
    audio_app_stop();  /* M3e: and its sound stops (what is already in the PWM ring plays out) */
    app_window_close();
    app_unmap();
    uart_puts("M3c app "); uart_puts(a->name);
    if (how) { uart_putc(' '); uart_puts(how); } else { uart_puts(" exit "); uart_dec((unsigned)status); }
    uart_puts(", ran "); uart_dec(app_runs); uart_puts(" times\n");
}
static void app_kill(const char *how) { if (app_cur && !app_running) app_end((unsigned long)-1, how); }
static void app_tick(void) {
    if (!app_cur || app_running) return;
    unsigned long now = cnt_now();
    if (app_close_at && now > app_close_at) { app_kill("closed by its red dot"); return; }   /* it never read the close click */
    if (!app_kick && now - app_last < cnt_hz() / 100) return;
    app_kick = 0; app_last = now; app_runs++;
    app_parked = 0; user_faulted = 0; app_running = 1;
    APP_FP_CLOBBER();
    unsigned long st = resume_user(&app_frame, app_sp, app_fp);
    app_running = 0;
    if (!app_parked) app_end(st, user_faulted ? "stopped by a fault" : 0);
}
static int app_start(const struct user_app *a) {   /* 0 started (it runs on the next pass), -2 it would not load, -3 one is already running */
    if (app_cur) return -3;
    unsigned long entry = user_load(a);
    if (!entry) return -2;
    if (a->name[0] == 'n' && a->name[1] == 'o' && a->name[2] == 't' && a->name[3] == 'e' && a->name[4] == 's' && !a->name[5])
        ufile_mkdirs("NOTES/NOTES");   /* M3d: Notes' default folder in JT/, made before it looks, as i386's notes_check_support does */
    if (a->name[0] == 'm' && a->name[1] == 'u' && a->name[2] == 's' && a->name[3] == 'i' && a->name[4] == 'c' && !a->name[5])
        ufile_mkdirs("MUSIC");         /* M3e: Music's library, JT/MUSIC/, there for songs dropped onto the card from a Mac */
    if (a->name[0] == 'b' && a->name[1] == 'o' && a->name[2] == 'o' && a->name[3] == 'k' && a->name[4] == 's' && !a->name[5])
        ufile_mkdirs("BOOKS");         /* 2.66: Books' library, JT/BOOKS/, the books the card ships with and the ones dropped there */
    for (int i = 0; i < 31; i++) app_frame.x[i] = 0;   /* argc 0, argv 0, like user_run */
    for (int i = 0; i < 66; i++) app_fp[i] = 0;        /* M3e: its FP registers start at 0, FPCR 0 */
    app_frame.elr = entry; app_sp = USER_BASE + UP_ASTACK + APP_STACK_PAGES * 4096UL;
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
    user_loud = 1;
    app_tick();
    user_loud = 0;
    if (app_cur == a) return app_wbytes ? 2 : 1;
    *status = user_status; *faulted = user_faulted;
    return 0;
}
#ifndef PI_BUILD
/* Probe the pages of the app that is loaded, from EL0: user.S's entry D gets { bss word, data page, code base, window size }
   in a block at the bottom of its own stack page and checks them (see user.S). It ends on a store to the code page, which must
   fault; the kernel survives and says so. */
static void user_probe(int pass) {
    unsigned long base = USER_BASE + UP_APP, *args = (unsigned long *)(USER_BASE + UP_STACK);
    args[0] = base + ((app_img + 15) & ~15UL); args[1] = base + app_code; args[2] = base; args[3] = UAPP_MAX;
    uart_puts("M3c probe "); uart_dec((unsigned)pass); uart_puts(" starts\n");
    user_faulted = 0;
    enter_user(USER_BASE + UP_CODE + (unsigned long)(user_entry_d - user_start), USER_BASE + UP_STACK + 4096, (unsigned long)args, 0);
    uart_puts(user_faulted ? "M3c probe survived the code write\n" : "M3c probe FAIL: the code write did not fault\n");
}
/* Branch to an address inside the loaded app that is not code: user.S's entry C does the `br`, which must be an instruction
   abort (the pages are never executable), and the kernel carries on. */
static void user_exec_probe(const char *what, unsigned long target) {
    uart_puts("M3c exec "); uart_puts(what); uart_putc(' '); uart_hex(target); uart_putc('\n');
    user_faulted = 0;
    enter_user(USER_BASE + UP_CODE + (unsigned long)(user_entry_c - user_start), USER_BASE + UP_STACK + 4096, USER_BASE + UP_KERN, target);
    uart_puts(user_faulted ? "M3c exec refused\n" : "M3c exec FAIL: it ran\n");
}
#endif
static void user_demo(void) {
    user_init(user_start, (unsigned long)(user_end - user_start));
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
        unsigned long base = USER_BASE + UP_APP;   /* never executable: a data page, the first whole page of the zeroed pool, the stack */
        user_exec_probe("data", base + app_code);
        user_exec_probe("pool", base + (app_img + 4095) / 4096 * 4096);
        user_exec_probe("stack", USER_BASE + UP_ASTACK);
    }
#endif
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
int wall_paint(unsigned *fb, unsigned pitch, int w, int h, int swap);   /* arch/arm64/wall.c: the flat Navy wallpaper */
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
static int con_key(unsigned code, unsigned value) {   /* Page Up 104, Page Down 109, Home 102, End 107: 1 if the console took the key */
    if (code != 104 && code != 109 && code != 102 && code != 107) return 0;
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
const char *ask_prompt(void);   /* ask.c: "joshua@tree % " */
/* The Terminal's bottom row: the prompt and the line being typed (ask.c), its tail when it is longer than the row, then a cursor. */
void con_clear(void) {         /* /clear: the Terminal's window empties, its text stays in the scrollback */
    if (!con_live || calc_front || app_front) return;
    int held = cur_hold();
    con_wipe(); cp->anchor = cp->len; cp->scrolled = 0; con_hint();
    cur_release(held);
}
void con_prompt(const char *s, unsigned n) {
    if (!con_live || calc_front || app_front || cp != &term_p) return;
    int held = cur_hold();
    const char *p = ask_prompt();
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
        for (const char *p = crash_txt[r]; *p; p++) con_tx(*p);
        con_tx('\n');
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
static unsigned mark_lerp(const unsigned char *m, int x0, int y0, int wx, int wy) {
    int x1 = x0 + 1 < MENU_MARK_W ? x0 + 1 : x0, y1 = y0 + 1 < MENU_MARK_H ? y0 + 1 : y0;
    return (unsigned)(((m[y0 * MENU_MARK_W + x0] * (256 - wx) + m[y0 * MENU_MARK_W + x1] * wx) * (256 - wy)
                     + (m[y1 * MENU_MARK_W + x0] * (256 - wx) + m[y1 * MENU_MARK_W + x1] * wx) * wy) >> 16);
}
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
static void wall_fill(int x, int y, int w, int h) {   /* physical px: the flat wallpaper colour (wall.c); there is no picture to save, so a window can leave from anywhere */
    unsigned c = fb_color(0x00B5502C);
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) fb[(unsigned)(y + j) * fb_pitch + (unsigned)(x + i)] = c;
}
static void console_close(void) {
    if (!con_live || !fb) return;
    int s = (int)window_scale(), x = win_lx * s, y = win_ly * s, w = win_lw * s, h = win_lh * s;
    cur_hide();
    wall_fill(x, y, w, h);
    fb_flush(x, y, w, h);
    con_live = 0;
    uart_puts(cp == &term_p ? "terminal closed\n" : "console closed\n");
    cur_show();
    if (calc_live) calc_raise();   /* the wallpaper just went back over the part of it the window covered */
    if (app_wbytes) app_raise();   /* the same for an app's window */
}
void ask_redraw(void);   /* ask.c: the line being typed, back on the ask> row */
static void pane_open(struct pane *p) {   /* the Console or the Terminal in front, its newest lines in view */
    int raise = con_live && cp == p;   /* already open, behind the Calculator: bring it up and give it the keys */
    if (raise && !calc_front && !app_front) return;
    if (!fb) { cp = p; uart_puts(p == &term_p ? "terminal open\n" : "console open\n"); return; }   /* no screen: the keys still follow */
    int s = (int)window_scale();
    cur_hide();
    calc_front = app_front = 0;   /* before any drawing: the pane's own draws are off while the Calculator or an app is on top */
    cp = p;
    console_frame();
    con_live = 1; cp->scrolled = 0;
    unsigned nl = con_nlines(), keep = con_rows > 1 ? (unsigned)con_rows - 1 : 1;
    cp->anchor = con_line_off(nl > keep ? nl - keep : 0);
    con_wipe();
    for (unsigned k = cp->anchor; k < cp->len; k++) con_draw(k);
    con_summary(); con_hint(); ask_redraw();
    fb_flush(win_lx * s, win_ly * s, win_lw * s, win_lh * s);
    if (raise) { con_quiet = 1; uart_puts(p == &term_p ? "focus terminal\n" : "focus console\n"); }
    else uart_puts(p == &term_p ? "terminal open\n" : "console open\n");
    cur_show();
}
static void console_open(void) { pane_open(&con_p); }
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
static int calc_lx, calc_ly, calc_lw, calc_lh;   /* its frame on the logical grid, set in fb_init */
static unsigned *calc_under;                    /* the wallpaper under it, saved at boot before any window was drawn */
static int clock_live, brick_live, spot_live, browser_live;
static void browser_open(void);
static void browser_close(void);
static int browser_key(unsigned code);
static void browser_click(int lx, int ly);
static void browser_wheel(int v);
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
    brick_close();
    clock_close();
    browser_close();
    app_kill("closed for the Calculator");   /* the two never share the screen, so there is no third window to stack */
    if (!calc_under) return;   /* no saved wallpaper (a full heap): nothing to close it back to */
    if (calc_live) { calc_raise(); return; }
    calc_live = calc_front = 1;
    calc_paint();
    uart_puts("calc open\n");
}
static void calc_close(void) {
    if (!calc_live) return;
    int s = (int)window_scale(), x = calc_lx * s, y = calc_ly * s, w = calc_lw * s, h = calc_lh * s;
    cur_hide();
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) fb[(unsigned)(y + j) * fb_pitch + (unsigned)(x + i)] = calc_under[j * w + i];
    fb_flush(x, y, w, h);
    calc_live = calc_front = 0;
    uart_puts("calc closed\n");
    if (con_live) { con_live = 0; pane_open(cp); }   /* the wallpaper went over its corner too: draw it whole, in front */
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
static int app_lx, app_ly, app_lw, app_lh;   /* the frame on the logical grid, set in fb_init */
static unsigned *app_under;                  /* the wallpaper under it, saved at boot */
static unsigned app_cw, app_ch;              /* the content, the app's buffer, pixel for pixel */
static int app_shown;                        /* the app has presented a frame since its window opened */
static void spot_close(void);
static void app_blit(void) {
    int s = (int)window_scale(), x0 = (app_lx + 8) * s, y0 = (app_ly + 32) * s;
    const unsigned *src = (const unsigned *)(USER_BASE + UP_WIN);
    for (unsigned y = 0; y < app_ch; y++) for (unsigned x = 0; x < app_cw; x++) {
        unsigned c = fb_color(src[y * app_cw + x] & 0x00FFFFFFu), *d = fb + (unsigned)(y0 + (int)y * s) * fb_pitch + (unsigned)(x0 + (int)x * s);
        for (int j = 0; j < s; j++) for (int i = 0; i < s; i++) d[(unsigned)j * fb_pitch + (unsigned)i] = c;
    }
}
static void app_paint(void) {   /* the frame, then the last frame the app presented */
    char t[16]; unsigned n = 0;
    for (const char *p = app_cur ? app_cur->name : "app"; *p && n < sizeof t - 1; p++, n++) t[n] = n ? *p : (char)(*p & ~32);   /* "calendar" is titled Calendar */
    t[n] = 0;
    int s = (int)window_scale();
    cur_hide();
    gui_draw_window_frame(app_lx, app_ly, app_lw, app_lh, t);
    if (app_shown) app_blit();
    fb_flush(app_lx * s, app_ly * s, app_lw * s, app_lh * s);
    cur_show();
}
static void app_raise(void) {   /* in front of the Console or the Terminal, with the keys */
    if (!app_wbytes) return;
    int was = app_front;
    app_front = 1;
    app_paint();
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
static long app_window_open(unsigned long info) {
    if (!app_running || !fb || !app_under || !app_cw) return -19;
    if (!user_writable(info, 24)) return -14;
    if (!app_wbytes) {
        const unsigned long PAGE = 0x3UL | (1UL << 2) | (3UL << 8) | (1UL << 10), AP_EL0_RW = 1UL << 6, PXN = 1UL << 53, UXN = 1UL << 54;
        unsigned long bytes = ((unsigned long)app_cw * app_ch * 4 + 4095) & ~4095UL;
        if (bytes > UWIN_MAX) return -12;
        for (unsigned long i = 0; i < bytes / 4096; i++) l3[(UP_WIN >> 12) + i] = (USER_BASE + UP_WIN + i * 4096) | PAGE | AP_EL0_RW | PXN | UXN;
        __asm__ volatile ("dsb ishst\n tlbi vmalle1\n dsb ish\n isb" ::: "memory");
        for (unsigned long i = 0; i < bytes / 8; i++) ((unsigned long *)(USER_BASE + UP_WIN))[i] = 0;
        app_wbytes = bytes; app_shown = 0;
        spot_close(); calc_close(); browser_close();
        app_front = 1;
        app_paint();
        uart_puts("M3c window "); uart_puts(app_cur->name); uart_putc(' '); uart_dec(app_cw); uart_putc('x'); uart_dec(app_ch);
        uart_puts(" at "); uart_dec((unsigned)app_lx + 8); uart_putc(','); uart_dec((unsigned)app_ly + 32); uart_putc('\n');
    }
    st32(info, app_cw); st32(info + 4, app_ch); st32(info + 8, app_cw * 4);
    st32(info + 12, 0); st32(info + 16, (unsigned)(USER_BASE + UP_WIN)); st32(info + 20, (unsigned)((USER_BASE + UP_WIN) >> 32));
    return 0;
}
/* JT_SYS_WINDOW_POLL, i386's semantics: with JT_POLL_PRESENT the buffer goes to the screen first (only while the window is
   in front and Spotlight is not over it; a raise draws it later), then one event (the LP64 struct jt_event: kind, a, b)
   and 1, or -EAGAIN when there is none. It never waits: the app yields and asks again. */
static long app_window_poll(unsigned long ev, unsigned long flags) {
    if (!app_running || !app_wbytes) return -9;
    if (!user_writable(ev, 12)) return -14;
    if (flags & ~1UL) return -22;
    if (flags & 1) {
        if (!app_shown) { con_quiet = 1; uart_puts("M3c window "); uart_puts(app_cur->name); uart_puts(" first frame\n"); }
        app_shown = 1;
        if (app_front && !spot_live) {
            int s = (int)window_scale();
            cur_hide(); app_blit(); fb_flush((app_lx + 8) * s, (app_ly + 32) * s, (int)app_cw * s, (int)app_ch * s); cur_show();
        }
    }
    if (app_qh == app_qt) return -11;
    unsigned i = app_qh++ % APP_EVQ;
    st32(ev, app_q[i].kind); st32(ev + 4, (unsigned)app_q[i].a); st32(ev + 8, (unsigned)app_q[i].b);
    con_quiet = 1; uart_puts("M3c event "); uart_puts(app_q[i].kind == 1 ? "key " : "click "); uart_dec((unsigned)app_q[i].a);
    if (app_q[i].kind != 1) { int y = app_q[i].b; uart_putc(','); if (y < 0) { uart_putc('-'); y = -y; } uart_dec((unsigned)y); }   /* a close click is above the content */
    uart_puts(" to "); uart_puts(app_cur->name); uart_putc('\n');
    return 1;
}
static void app_window_close(void) {
    if (!app_wbytes) return;
    int s = (int)window_scale(), x = app_lx * s, y = app_ly * s, w = app_lw * s, h = app_lh * s;
    cur_hide();
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) fb[(unsigned)(y + j) * fb_pitch + (unsigned)(x + i)] = app_under[j * w + i];
    fb_flush(x, y, w, h);
    app_front = app_shown = 0;
    if (con_live) { con_live = 0; pane_open(cp); }   /* the wallpaper went over its part of the Console too: draw it whole */
    cur_show();
}
/* A dock tile whose name is an app in the table (Calendar, and any later one): start it, or bring its window up. */
static int app_dock(const char *name) {
    for (const struct user_app *a = user_apps; a->name; a++) {
        unsigned i = 0;
        while (name[i] && (name[i] | 32) == a->name[i]) i++;
        if (name[i] || a->name[i]) continue;
        if (app_cur == a) app_raise();
        else if (app_cur) {   /* one app at a time: a tile for another app swaps to it (the one in front is closed, its files written back), as clicking a second app does on any desktop */
            con_quiet = 1; uart_puts("M3c app swap: "); uart_puts(app_cur->name); uart_puts(" closed for "); uart_puts(a->name); uart_putc('\n');
            app_kill("closed for another app");
            if (!app_cur) app_start(a);
        }
        else app_start(a);
        return 1;
    }
    return 0;
}
/* ---- Window drag. The Console, the Terminal, Clock, Brick and the Browser all live in one rectangle (win_lx, win_ly, win_lw,
   win_lh), so one function moves any of them: win_move_to copies the window's pixels to the new place (the contents come
   along as they are, nothing is drawn again), fills what it uncovered with the flat wallpaper, shifts the Console's text
   origin by the same amount, and cleans both rectangles out of the data cache. The window stays whole on the screen, below
   the menu bar and above the dock's band and the line over it, so no band is ever under it. A press on the title band (not
   on a dot) starts a drag, the grab offset kept; the left button going up ends it. The Calculator and an app's window keep
   their boot places, and a Console with one of them open is not dragged (there is no stacking to repaint). ---- */
static int in_box(int x, int y, int bx, int by, int bw, int bh);
static int drag_on, drag_dx, drag_dy;   /* a title-band press is moving the window; where inside it the pointer took hold, logical px */
static int win_draggable(void) { return !spot_live && (brick_live || clock_live || browser_live || (con_live && !calc_live && !app_wbytes)); }
static int on_title_band(int x, int y) {   /* the top 30 px of the window, but not the three dots (radius 7, a pixel spare) */
    if (!in_box(x, y, win_lx, win_ly, win_lw, 30)) return 0;
    for (int d = 0; d < 3; d++) { int dx = x - (win_lx + 24 + 22 * d), dy = y - (win_ly + 16); if (dx * dx + dy * dy <= 8 * 8) return 0; }
    return 1;
}
static void win_move_to(int nx, int ny) {
    if (!fb) return;
    int s = (int)window_scale(), top = sg(MENUBAR_H) / s + 8, bottom = gui_dock_band_top() - 22, right = (int)fb_w / s - win_lw;
    if (nx > right) nx = right;
    if (nx < 0) nx = 0;
    if (ny > bottom - win_lh) ny = bottom - win_lh;
    if (ny < top) ny = top;
    if (nx == win_lx && ny == win_ly) return;
    int ox = win_lx * s, oy = win_ly * s, w = win_lw * s, h = win_lh * s, px = nx * s, py = ny * s, dx = px - ox, dy = py - oy;
    cur_hide();
    for (int j = 0; j < h; j++) {   /* a memmove by rows: the far rows first when moving down, the far columns first when moving right */
        int r = dy > 0 ? h - 1 - j : j;
        unsigned *src = fb + (unsigned)(oy + r) * fb_pitch + (unsigned)ox, *dst = fb + (unsigned)(py + r) * fb_pitch + (unsigned)px;
        if (dx > 0) for (int i = w - 1; i >= 0; i--) dst[i] = src[i];
        else for (int i = 0; i < w; i++) dst[i] = src[i];
    }
    unsigned wall = fb_color(0x00B5502C);   /* the old rectangle minus the new one, row by row */
    for (int j = 0; j < h; j++) {
        int y = oy + j, inside = y >= py && y < py + h, a = ox, b = ox + w;
        if (inside) { if (dx > 0) b = px < b ? px : b; else a = px + w > a ? px + w : a; }
        for (int x = a; x < b; x++) fb[(unsigned)y * fb_pitch + (unsigned)x] = wall;
    }
    win_lx = nx; win_ly = ny;
    con_x += dx; con_y += dy; con_wx += dx; con_wy += dy;   /* con_layout's origin and the title bar's "lines N-M" follow; the size, and so the rows and columns, are unchanged */
    int ux = ox < px ? ox : px, uy = oy < py ? oy : py;
    fb_flush(ux, uy, w + (dx < 0 ? -dx : dx), h + (dy < 0 ? -dy : dy));
}
static void drag_end(void) {
    if (!drag_on) return;
    drag_on = 0;
    con_quiet = 1; uart_puts("window at "); uart_dec((unsigned)win_lx); uart_putc(','); uart_dec((unsigned)win_ly); uart_putc('\n');
}
static void pointer_moved(void) {   /* one report done: the arrow to its new place, the dock's label to the slot under it */
    if (!fb) return;
    int s = (int)window_scale(), slot = gui_dock_hit_test((int)mouse_x / s, (int)mouse_y / s);
    if (drag_on) { if (win_draggable()) win_move_to((int)mouse_x / s - drag_dx, (int)mouse_y / s - drag_dy); else drag_on = 0; }
    if (brick_live) brick_mouse((int)mouse_x / s, (int)mouse_y / s);
    cur_wanted = 1;
    cur_hide();
    if (slot != hover_slot) { hover_slot = slot; dock_hover(slot); }
    cur_show();
}
static int in_box(int x, int y, int bx, int by, int bw, int bh) { return x >= bx && x < bx + bw && y >= by && y < by + bh; }
static int on_close_dot(int x, int y, int wx, int wy) { int dx = x - (wx + 24), dy = y - (wy + 16); return dx * dx + dy * dy <= 8 * 8; }   /* gui_draw_window_frame's red dot, radius 7 */
static void dock_activate(int slot);
#define SPOT_MUSIC (GUI_ICON_COUNT + 2)   /* M3e: Music is in Spotlight (and the app table) but not on the dock, a decision */
#define SPOT_BROWSER (GUI_ICON_COUNT + 3) /* the Browser is in Spotlight only too: the dock's 11 slots stay as they are */
#define SPOT_BOOKS (GUI_ICON_COUNT + 4)   /* 2.66: Books, the same: Spotlight only, not on the dock */
static void pointer_click(void) {   /* the left button went down */
    if (!fb) return;
    int s = (int)window_scale(), lx = (int)mouse_x / s, ly = (int)mouse_y / s, slot = gui_dock_hit_test(lx, ly);
    if (spot_live && slot < 0) return;   /* the bar is over the windows: a raise under it would leave its saved pixels stale */
    int on_win = in_box(lx, ly, win_lx, win_ly, win_lw, win_lh), on_calc = calc_live && in_box(lx, ly, calc_lx, calc_ly, calc_lw, calc_lh);
    int on_app = app_wbytes && in_box(lx, ly, app_lx, app_ly, app_lw, app_lh);
    if (slot < 0 && win_draggable() && on_title_band(lx, ly)) { drag_on = 1; drag_dx = lx - win_lx; drag_dy = ly - win_ly; return; }   /* the title band takes hold of the window */
    if (slot == 0) calc_open();   /* Apps tile opens Calculator; Brick and Clock are in Spotlight. */
    else if (brick_live && slot < 0) {
        if (spot_live) return;
        if (on_close_dot(lx, ly, win_lx, win_ly)) brick_close();
        else if (on_win && ly >= win_ly + 32) brick_key(57, 1);
    } else if (clock_live && slot < 0) {
        if (on_close_dot(lx, ly, win_lx, win_ly)) clock_close();
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
static void dock_activate(int slot) {   /* a dock tile, from a click, a dock key or Spotlight: slot 0 is handled by the caller's calc_open */
    if (slot == 0) { calc_open(); return; }
    if (slot == GUI_ICON_COUNT) { clock_open(); return; }
    if (slot == GUI_ICON_COUNT + 1) { brick_open(); return; }
    if (slot == SPOT_BROWSER) { browser_open(); return; }
    brick_close();
    clock_close();
    browser_close();
    if (app_dock(slot == SPOT_MUSIC ? "Music" : slot == SPOT_BOOKS ? "Books" : dock_names[slot])) return;   /* Calendar, Notes, Music and Books: user/NAME.c at EL0, in its own window */
    if (slot == 6 || slot == 7) { pane_open(&term_p); return; }   /* dock_names[6], Terminal: the command line, in front of an open Calculator */
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
#define SPOT_ROW 22
#define SPOT_MAX 5                    /* matches shown at most */
static char spot_text[24]; static unsigned spot_len;
static int spot_sel, spot_hits[SPOT_BOOKS + 1], spot_n, dock_sel = -1, ctrl_held, gui_held, alt_held;   /* Ctrl, Cmd (the GUI key), Alt or Option: either side */
static unsigned *spot_under;          /* the pixels under the bar's largest size, saved on open */
static int spot_lx, spot_ly, spot_lh;   /* the bar on the logical grid; its height is what is drawn now */
static int spot_match(const char *name) {   /* the typed text, case folded, anywhere in the name */
    if (!spot_len) return 1;
    for (unsigned i = 0; name[i]; i++) {
        unsigned k = 0;
        while (k < spot_len && name[i + k] && (name[i + k] | 32) == (spot_text[k] | 32)) k++;
        if (k == spot_len) return 1;
    }
    return 0;
}
static const char *spot_name(int slot) { return slot == GUI_ICON_COUNT ? "Clock" : slot == GUI_ICON_COUNT + 1 ? "Brick" : slot == SPOT_MUSIC ? "Music" : slot == SPOT_BROWSER ? "Browser" : slot == SPOT_BOOKS ? "Books" : dock_names[slot]; }
static int spot_hmax(void) { return 34 + SPOT_ROW * SPOT_MAX + 6; }
static void spot_paint(void) {
    int s = (int)window_scale();
    spot_n = 0;
    for (int i = 0; i <= SPOT_BOOKS && spot_n < SPOT_MAX; i++) if (spot_match(spot_name(i))) spot_hits[spot_n++] = i;
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
static void spot_open(void) {
    if (spot_live || !fb) return;
    int s = (int)window_scale(), w = SPOT_W * s, h = spot_hmax() * s;
    spot_lx = ((int)fb_w / s - SPOT_W) / 2; spot_ly = sg(MENUBAR_H) / s + 24;
    if (!spot_under) spot_under = kmalloc((unsigned)(w * h) * 4);
    if (!spot_under) { uart_puts("oom spotlight\n"); return; }
    cur_hide();
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) spot_under[j * w + i] = fb[(unsigned)(spot_ly * s + j) * fb_pitch + (unsigned)(spot_lx * s + i)];
    spot_live = 1; spot_len = 0; spot_text[0] = 0; spot_sel = 0;
    con_quiet = 1; uart_puts("spotlight open\n");
    spot_paint();
}
static void spot_close(void) {
    if (!spot_live) return;
    int s = (int)window_scale(), w = SPOT_W * s, h = spot_hmax() * s, x = spot_lx * s, y = spot_ly * s;
    cur_hide();
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) fb[(unsigned)(y + j) * fb_pitch + (unsigned)(x + i)] = spot_under[j * w + i];
    fb_flush(x, y, w, h);
    spot_live = 0;
    con_quiet = 1; uart_puts("spotlight close\n");
    cur_show();
    if (app_front && app_shown) app_paint();   /* frames the app presented under the bar were held back */
}
static int spot_key(unsigned code) {   /* a key down with the bar open: every key is Spotlight's */
    if (code == 1) spot_close();
    else if (code == 28 || code == 96) { int slot = spot_n ? spot_hits[spot_sel] : -1; spot_close(); if (slot >= 0) dock_activate(slot); }
    else if (code == 103) { if (spot_sel > 0) spot_sel--; spot_paint(); }
    else if (code == 108) { if (spot_sel + 1 < spot_n) spot_sel++; spot_paint(); }
    else if (code == 14) { if (spot_len) spot_text[--spot_len] = 0; spot_sel = 0; spot_paint(); }
    else { int ch = ask_char(code); if (ch >= ' ' && ch < 127 && spot_len + 1 < sizeof spot_text) { spot_text[spot_len++] = (char)ch; spot_text[spot_len] = 0; spot_sel = 0; spot_paint(); } }
    return 1;
}
static void dock_select(int slot) { dock_sel = slot; hover_slot = slot; dock_hover(slot); }
#ifdef PI_BUILD
#include "power.h"
void power_run(int off) {   /* the reboot and shutdown commands: the same action the confirmed chord runs, refused while Samantha answers */
    if (ask_pending()) power_message("Wait for Samantha to finish, then try again.\n"); else power_apply(off);
}
#else
void power_run(int off) { uart_puts(off ? "power: shutdown (this build has no power control)\n" : "power: restart (this build has no power control)\n"); }   /* the virt test build: only the Pi has a watchdog to pull */
#endif
int volume_key(unsigned code);   /* cmd.c: F11 and F12, Volume Down, Up and Mute */
static int ui_key(unsigned code) {   /* a key down, before any pane sees it: 1 if the desktop took it */
    int chord = ctrl_held || gui_held || alt_held;
#ifdef PI_BUILD
    if (power_key(code)) return 1;
#endif
    if (volume_key(code)) return 1;   /* before Spotlight and every pane, like the Spotlight shortcut */
    if (code == 59 || (gui_held && code == 57)) { if (spot_live) spot_close(); else spot_open(); return 1; }   /* F1 or Cmd+Space toggle, like a Mac; Ctrl/Alt+Space and a bare Space do nothing (Joshua, 2026-10-11, on the board) */
    if (code == 60 || (ctrl_held && code == 20)) { spot_close(); brick_close(); clock_close(); browser_close(); pane_open(&term_p); return 1; }   /* F2, Ctrl+T: the Terminal in front, an open Calculator behind it */
    if (spot_live) return spot_key(code);
    if (brick_live) return brick_key(code, 1);
    if (clock_live) { if (code == 1) clock_close(); return 1; }
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
static void clock_local(unsigned long utc, unsigned *hh, unsigned *mm, unsigned *mon, unsigned *day) {
    long y; unsigned m, d; civil_from_days(utc / 86400, &y, &m, &d);
    int dst = utc >= civil_days(2026, 3, 8) * 86400UL + 10 * 3600UL
           || (utc >= nth_sunday_utc(y, 3, 2, 10) && utc < nth_sunday_utc(y, 11, 1, 9));
    unsigned long loc = utc - (dst ? 7 : 8) * 3600UL;
    civil_from_days(loc / 86400, &y, mon, day);
    clock_year = y;
    *hh = (unsigned)(loc % 86400) / 3600; *mm = (unsigned)(loc % 3600) / 60;
}
#include "clock_ui.h"
#include "brick_ui.h"
#include "browser_ui.h"
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
    int nx = mb_x0 + sg(12), nw = ox - R - sg(16) - nx;
    if (notice.text[0] && nw > 0)
        text_draw(2, notice.text, nx, (mb + sg(8)) / 2, sg(120), fb_color(0x001C1C1E), fb, fb_pitch, nx + nw, H);
    fb_flush(mb_x0, 0, W - mb_x0, mb);
}
void menubar_notify(const char *message) {
    notice_set(&notice, message, notice_now());
    cur_hide(); menubar_wifi(mb_wifi); cur_show();
    uart_puts("notice: "); uart_puts(notice.text); uart_putc('\n');
}
static void cal_tick(void) {   /* the day has changed (or the clock just arrived): the Calendar tile again, the hover band's copy with it */
    if (!fb || cal_today() == cal_shown) return;
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
    if (!wall_ok) fb_rect(0, 0, W, H, 0x00203040);   /* Navy (clrs.cc), see wall.c; this fallback is now unreachable */
    for (int y = 0; y < mb - 1; y++) for (int x = 0; x < W; x++) {   /* menu bar: half wallpaper, half white, per pixel */
        unsigned c = fb[(unsigned)y * fb_pitch + (unsigned)x];
        fb[(unsigned)y * fb_pitch + (unsigned)x] = ((c >> 1) & 0x007F7F7Fu) + 0x00808080u;   /* each colour lane: half itself plus half of 255 */
    }
    fb_rect(0, mb - 1, W, 1, MENUBAR_RULE);               /* closed by a one pixel rule */
    mb_h = mb - 1; mb_x0 = W / 2;                          /* keep the bare right half of the bar, for menubar_status() */
    mb_save = kmalloc((unsigned)(mb_h * (W - mb_x0)) * 4);
    if (mb_save) for (int y = 0; y < mb_h; y++) for (int x = mb_x0; x < W; x++) mb_save[y * (W - mb_x0) + (x - mb_x0)] = fb[(unsigned)y * fb_pitch + (unsigned)x];
    text_ok = text_init();                                /* before the window (its title) */
    dock_paint();                                         /* the i386 dock, while only the wallpaper is under it */
    int band_y = gui_dock_band_top() * (int)window_scale();   /* the top of the dock's band, room for a hover label */
    /* Slice 3: the Console wears the i386 window frame (gui_paint.c): rounded cream body on the wallpaper, traffic
       lights, centred name, a hairline under the title band. Its content well is white for the log. */
    int s = (int)window_scale(), lx = win_x / s, ly = win_y / s, lw = win_w / s, lh = win_h / s;
    win_lx = lx; win_ly = ly; win_lw = lw; win_lh = lh;
    /* Slice 5: the Calculator's own frame, over the right of the Console's so both show, clear of the line above the dock */
    calc_lw = 280; calc_lx = lx + lw - 150; if (calc_lx + calc_lw > W / s - 8) calc_lx = W / s - 8 - calc_lw;
    calc_ly = ly + 40; calc_lh = gui_dock_band_top() - 22 - calc_ly; if (calc_lh > 300) calc_lh = 300;
    calc_under = kmalloc((unsigned)(calc_lw * s * calc_lh * s) * 4);
    if (calc_under) { for (int j = 0; j < calc_lh * s; j++) for (int i = 0; i < calc_lw * s; i++) calc_under[j * calc_lw * s + i] = fb[(unsigned)(calc_ly * s + j) * fb_pitch + (unsigned)(calc_lx * s + i)]; }
    else uart_puts("oom calculator\n");                    /* the Apps tile then opens nothing */
    /* M3c part 2: an EL0 app's window, placed like the Calculator's, then moved up and cut to fit above the dock's band */
    app_lw = 576; if (app_lw > W / s - 16) app_lw = W / s - 16;
    app_lx = lx + lw - 150; if (app_lx + app_lw > W / s - 8) app_lx = W / s - 8 - app_lw;
    {   int top = mb / s + 8, bottom = gui_dock_band_top() - 22;
        app_lh = 400; app_ly = ly + 40;
        if (app_ly + app_lh > bottom) app_ly = bottom - app_lh;
        if (app_ly < top) { app_ly = top; app_lh = bottom - top; }
    }
    app_cw = (unsigned)(app_lw - 16); app_ch = (unsigned)(app_lh - 40);
    app_under = kmalloc((unsigned)(app_lw * s * app_lh * s) * 4);
    if (app_under) { for (int j = 0; j < app_lh * s; j++) for (int i = 0; i < app_lw * s; i++) app_under[j * app_lw * s + i] = fb[(unsigned)(app_ly * s + j) * fb_pitch + (unsigned)(app_lx * s + i)]; }
    else uart_puts("oom app window\n");                    /* an app then gets -ENODEV for its window and says so */
    console_frame();
    if (text_ok) {
        menu_mark_paint(sg(24), mb / 2, sg(20), 0x001C1C1E);   /* the mark in the corner, the i386 menu bar's */
        text_draw(1, "Joshua Tree", sg(40), (mb + sg(8)) / 2, sg(130), fb_color(0x001C1C1E), fb, fb_pitch, W, H);   /* menu bar title: bold sans, 13 on the grid, ink */
        menubar_wifi(1);   /* replaces the ARM64 badge: wifi.c moves it along as the chip comes up */
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
    /* Blank spots, clear of any text: the bottom of the window's white well, the menu bar's rule, and the wallpaper (the
       design system's dark fill at three corners, not the old 0x203040 fallback). Grey and white read the same either
       byte order; the rule and the fill are checked in fb_color order. */
    int ok = fb[(unsigned)((ly + lh - 8) * s - 2) * fb_pitch + fb_w / 2] == 0x00ffffff && fb[(unsigned)(mb - 1) * fb_pitch + fb_w / 2] == fb_color(MENUBAR_RULE);
    unsigned w0 = fb[(unsigned)sc(300) * fb_pitch + 50], w1 = fb[(unsigned)(H - sc(8)) * fb_pitch + fb_w - 50], w2 = fb[(unsigned)(mb + sc(40)) * fb_pitch + fb_w - 50];
    ok = ok && wall_ok && w0 == fb_color(0x00B5502C) && w1 == w0 && w2 == w0;
    uart_puts(ok ? "M1c fb ok\n" : "M1c fb FAIL\n");
#ifdef DRAG_TEST   /* the dragtest build: a drag of 100 right and 60 down at boot, clamped like a real one, for arm64-drag-check.py at 1080p (scale 2) */
    drag_on = 1; win_move_to(win_lx + 100, win_ly + 60); drag_end();
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

/* ask.c's agent loop: [[open APP]] opens a dock app by name. Calculator, Console, Clock and Brick are available; other names return 0. */
int app_open_name(const char *name) {
    static const char *const openable[] = {"calculator", "console", "clock", "brick", "browser"};
    for (unsigned a = 0; a < 5; a++) {
        const char *w = openable[a]; unsigned i = 0;
        for (; w[i] && name[i] && (name[i] | 32) == w[i]; i++) ;
        if (!w[i] && !name[i]) { if (a == 0) calc_open(); else if (a == 2) clock_open(); else if (a == 3) brick_open(); else if (a == 4) browser_open(); return 1; }
    }
    return 0;
}

/* ---- Input, from any driver: Linux evdev events (virtio input speaks them natively; the USB HID driver in xhci.c
   translates its reports into them). The pointer starts mid-screen; a tablet sets it, a mouse moves it. ---- */
struct input_event { unsigned short type, code; unsigned value; };
int ask_key(unsigned code, unsigned value);   /* ask.c: the prompt's line editor */
int ask_char(unsigned code);                  /* ask.c: a key code as the character it types, Shift included */
void ask_poll(void);
#ifdef PI_BUILD
#include "screen_idle.h"
#endif
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
    if (!k && ctrl_held) k = code == 46 ? 302 : code == 45 ? 303 : code == 47 ? 304 : code == 31 ? 308 : code == 30 ? 315 : 0;   /* Ctrl+C X V S A */
    else if (!k) k = ask_char(code);
    if (k) app_push(1, k, 0);
}
static void input_event(struct input_event e) {
#ifdef PI_BUILD
    if (e.type == 1 || e.type == 3 || (e.type == 2 && e.value)) screen_activity();   /* any key or button or a move wakes the screen */
#endif
    if (e.type == 1 && e.value < 2) {                                                                      /* a real press or release; value 2 is our own repeat */
        if (!e.value) { if (e.code == rep_code) rep_code = 0; }
        else if ((e.code == 14 || (e.code < 58 && ask_char(e.code))) && !(brick_live && !spot_live) && !ctrl_held && !gui_held && !alt_held) {
            rep_code = e.code; rep_next = key_ms() + KEY_REPEAT_DELAY_MS;
        } else rep_code = 0;
    }
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
        uart_puts("key "); uart_dec(e.code); uart_puts(" down\n");
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
            uart_puts("key "); uart_dec(e.code); uart_puts(e.value ? " down\n" : " up\n");
        }
        cur_release(held);
    }
    else if (e.type == 3) {                                                                                /* EV_ABS: the tablet, 0..32767 */
        if (e.code == 0) mouse_x = e.value * fb_w / 32768; else if (e.code == 1) mouse_y = e.value * fb_h / 32768;
        mouse_moved = 1;
    } else if (e.type == 2) {                                                                              /* EV_REL: a mouse, clamped to the screen */
        int v = (int)e.value;
        if (e.code == 8) { browser_wheel(v); return; }   /* REL_WHEEL: only the Browser reads it */
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
    static const char *const script = NOTES_SELFTEST == 1 ? ">>>>>\n|nhello from the pi\033" : ">>>>>\n|\n";
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
    unsigned char down[8] = { 0 }, none[8] = { 0 };
    down[2] = c == '>' ? 0x4F : c == '\n' ? 0x28 : c == '\033' ? 0x29 : c == ' ' ? 0x2C : (unsigned char)(0x04 + c - 'a');   /* HID usages: Right, Enter, Esc, Space, a to z */
    hid_kbd(down, 8, last); hid_kbd(none, 8, last);
    if (!script[++at]) next = now + cnt_hz();
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
   and one status byte the device writes back. Polled, one request at a time. M3d: writes too (type 1, and the buffer is
   the device's to read), and emmc.c takes the disk as its card through vblk_*, so the FAT code runs on virt as on a Pi. */
struct blk_hdr { unsigned type, reserved; unsigned long sector; };
static struct vq blk_q;
static int blk_ok;
static unsigned long blk_sectors;
static int blk_init(void) {
    unsigned long b = vio_find(2, 0);
    if (!b || !vio_start(b, 0) || !vq_setup(&blk_q, b, 0)) return 0;
    VR(b, 0x70) = 1 | 2 | 8 | 4;                    /* driver ok */
    blk_sectors = (unsigned long)VR(b, 0x100) | (unsigned long)VR(b, 0x104) << 32;   /* config space: capacity */
    uart_puts("M2 blk sectors "); uart_dec((unsigned)blk_sectors); uart_putc('\n');
    blk_ok = 1;
    return 1;
}
static int blk_rw(unsigned long sector, void *buf, int write) {   /* one 512-byte sector, 1 on success */
    static struct blk_hdr hdr __attribute__((aligned(16)));
    static volatile unsigned char status __attribute__((aligned(16)));
    if (!blk_ok) return 0;
    hdr.type = write ? 1 : 0; hdr.reserved = 0; hdr.sector = sector;   /* 0 = read, 1 = write */
    status = 0xFF;
    dcache_clean(&hdr, sizeof hdr); dcache_clean(buf, 512); dcache_clean((void *)&status, 1);   /* QEMU is coherent; a real DMA engine is not */
    blk_q.d[0] = (struct vq_desc){ (unsigned long)&hdr, sizeof hdr, 1 /* next */, 1 };
    blk_q.d[1] = (struct vq_desc){ (unsigned long)buf, 512, (unsigned short)(write ? 1 : 1 | 2) /* next; device writes on a read */, 2 };
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
static int blk_read(unsigned long sector, void *buf) { return blk_rw(sector, buf, 0); }
int vblk_ready(void) { return blk_ok; }
unsigned long vblk_sectors(void) { return blk_sectors; }
int vblk_rw(unsigned long sector, void *buf, int write) { return blk_rw(sector, buf, write); }
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
void input_drain(void) { usb_poll(); input_poll(); key_repeat_tick(); audio_pump(); }   /* ask.c, between agent steps and in every wait while `sam` thinks: the keys that queued, a held key repeating, and the sound */

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
    sd_mount();                /* emmc.c: the SD card's FAT partition for ls and cat; one `sd ...` line, every wait bounded */
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
    bt_init();   /* bt.c: powers the radio and returns; the main loop's bt_poll does the rest */
#ifdef NOTICE_SELFTEST
    menubar_notify("Note kept in memory");
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
#ifdef NOTES_SELFTEST
    usb_ok |= !inputs;   /* QEMU's raspi4b has no USB to find: the loop runs anyway, for notes_tick's made-up keys */
#endif
#ifdef PI_BUILD
    {   /* The desktop is up: the boot log steps aside, so the board boots to a clean desktop (it stays one dock click away, and
           the Terminal opens as its own window). Anything that went wrong keeps it on screen: a FAIL or an oom line in the log. */
        int trouble = 0;
        for (unsigned i = 0, ls = 0; i + 4 <= con_p.len; i++) {   /* ls: where this line began; a Wi-Fi line is skipped, the menu bar says when Wi-Fi is down */
            if (con_p.log[i] == '\n') { ls = i + 1; continue; }
            const char *q = con_p.log + i;
            int wifi = con_p.log[ls] == 'w' || con_p.log[ls] == 'W';
            if (!wifi && ((q[0] == 'F' && q[1] == 'A' && q[2] == 'I' && q[3] == 'L') || (q[0] == 'o' && q[1] == 'o' && q[2] == 'm' && q[3] == ' '))) { trouble = 1; break; }
        }
        if (!trouble) console_close();
    }
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
            usb_poll(); input_poll(); key_repeat_tick(); ask_poll(); bt_poll(); brick_tick(); menubar_tick(); keydbg_tick(); app_tick(); audio_pump(); screen_tick();   /* only the display sleeps: USB stays polled */
#ifdef NOTES_SELFTEST
            notes_tick();
#endif
        }
#else
        for (;;) {
            if (usb_ok || brick_live || notice.text[0] || rep_code || bt_busy() || app_cur || audio_busy()) __asm__ volatile ("msr daifset, #2\n msr cntv_tval_el0, %0\n msr cntv_ctl_el0, %1\n isb\n wfi\n"
                              " msr cntv_ctl_el0, xzr\n isb\n msr daifclr, #2\n isb" :: "r"(step), "r"(1UL) : "memory");
            else __asm__ volatile ("wfi");
            usb_poll(); input_poll(); key_repeat_tick(); ask_poll(); bt_poll(); brick_tick(); menubar_tick(); keydbg_tick(); app_tick();   /* an app parked on yield runs again here */
            audio_pump();   /* M3e: the app's sound, from its ring into the PWM's, never waiting */
        }
#endif
    }
    for (;;) __asm__ volatile ("wfe");
}
