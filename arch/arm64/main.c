/* M0: serial hello. QEMU's virt machine has a PL011 UART at 0x09000000. A Raspberry Pi 4 has one at
   0xFE201000 (make PI=1 sets UART_BASE and PI_BUILD) and needs its two GPIO pins and its baud rate
   set first; QEMU's virt machine comes up ready. Since 2.51.0 the Pi image (CONSOLE_MINI_UART, set by the Makefile)
   prints on the mini UART instead, on the same header pins: the PL011 belongs to the Bluetooth radio (bt.c). */
#include "kmain.h"   /* the platform addresses, and what main.c shares with the parts cut out of it (boot.c, virtio.c): declared once, checked here */
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
void con_tx(char c) {
    if (con_tx_dead) return;
    for (unsigned n = 0; CON_TX_FULL(); n++) if (n > 2000000) { con_tx_dead = 1; return; }
    CON_TX(c);
}
void uart_init(void) {
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
void console_putc(char c);   /* the same text, on the screen once there is one */
int con_quiet;   /* the rest of this line goes to the UART only: key echoes stay off the screen while the Terminal has the keys */
static int con_line_start = 1;   /* the next character begins a line */
int con_to_term;          /* a Terminal command is running: what it prints goes to the Terminal, not the Console */
void term_output(int on) { con_to_term = on; }   /* ask.c, around one command */
#ifdef QUIET_BOOT
int splash_on, splash_bad;   /* the boot splash is up; a FAIL or oom line is being printed under it */
void splash_abort(void);
#endif
void uart_putc(char c) {
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
void uart_puts(const char *s) {
    if (con_line_start && con_noise(s)) con_quiet = 1;
#ifdef QUIET_BOOT
    if (splash_on) {   /* the splash never hides a failure: any FAIL or oom line brings the desktop up */
        if (s[0] == 'o' && s[1] == 'o' && s[2] == 'm' && s[3] == ' ') splash_bad = 1;
        for (const char *p = s; *p; p++) if (p[0] == 'F' && p[1] == 'A' && p[2] == 'I' && p[3] == 'L') splash_bad = 1;
    }
#endif
    while (*s) uart_putc(*s++);
}
void uart_hex(unsigned long v) {
    uart_puts("0x");
    for (int i = 60; i >= 0; i -= 4) uart_putc("0123456789abcdef"[(v >> i) & 15]);
}
void uart_dec(unsigned v) {
    char b[12]; int n = 0;
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) uart_putc(b[--n]);
}
/* M4: the same printing for the drivers in their own files (pci.c, xhci.c) */
int term_front(void);   /* the Terminal window is open: it has the keyboard */
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
extern char vectors[];
volatile unsigned irq_ticks;   /* the timer IRQ's count; it stops at 3 (never wait on it) */
unsigned long timer_step;
#define GICD(o) REG(GICD_BASE + (o))
#define GICC(o) REG(GICC_BASE + (o))
#define TIMER_INTID 30   /* the EL1 physical timer, a private interrupt on every GIC */
static volatile int wd_live;           /* 4.5: an app is inside resume_user with the watchdog armed */
static void wd_kill(void) __attribute__((noreturn));

void crash(const struct frame *f);   /* the crash screen, further down: it needs the framebuffer code */
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
        irq_ticks++;
        uart_puts("tick "); uart_dec(irq_ticks); uart_puts("\n");
        if (irq_ticks >= 3) __asm__ volatile ("msr cntp_ctl_el0, %0" :: "r"(0UL));   /* enough, stop the timer */
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
unsigned long l3[APP_SLOTS * ARENA_BLOCKS * 512] __attribute__((aligned(4096)));   /* ...each one a table of 4 KiB pages, back to back, so l3[page] works across all eight */
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
unsigned long heap_next;
unsigned heap_oom;   /* allocations refused so far: fb_init tells an out-of-memory font failure from a bad font */
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
void m1b_selftest(void) {
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
extern const struct user_app user_apps[];
static unsigned long user_status;
static int user_faulted;    /* the last EL0 program ended on a fault, not an exit */
#ifdef HANG_TEST
static int hang_arm;       /* app_start sets it for slot 0, so the boot self-tests' own loads are left alone */
#endif
static int user_loud;       /* userrun: what the program writes reaches the screen too; the boot self-test keeps it on the UART */
static unsigned user_pid;   /* programs started so far, what JT_SYS_GETPID answers */
static unsigned long app_len, app_code, app_img;   /* the loaded app: bytes of window mapped, how many of those are code, and the image's own length */
unsigned long cal_local(void);   /* cal_utc shifted to Vancouver wall time, what SYS_TIME returns (below) */
unsigned long cal_utc(void);   /* the clock the Calendar tile reads, further down: 0 until the network has told us the time */
/* M3c part 2, the scheduler's state: one app at a time, since the arena has one app window. app_cur is the app it holds,
   running or parked; app_frame and app_sp are where it parked (every register, its pc and its stack). Its window buffer is
   app_wbytes at UP_WIN, 0 with no window, and app_q is the ring of events the desktop queued for it. */
struct app apps[APP_SLOTS];
struct app *ap = &apps[0];         /* the app whose state the code below means: the one resume_user is in, else the one in front */
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
unsigned long cnt_now(void) { unsigned long c; __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(c)); return c; }
unsigned long cnt_hz(void) { unsigned long f; __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(f)); return f ? f : 54000000; }
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
int app_top = -1;
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
unsigned char app_z[APP_SLOTS];
unsigned app_zn;
void app_z_drop(unsigned sl) {
    unsigned o = 0;
    for (unsigned i = 0; i < app_zn; i++) if (app_z[i] != sl) app_z[o++] = app_z[i];
    app_zn = o;
}
void app_z_front(unsigned sl) {
    app_z_drop(sl);
    for (unsigned i = app_zn; i > 0; i--) app_z[i] = app_z[i - 1];
    app_z[0] = (unsigned char)sl; app_zn++;
    app_top = (int)sl; ap = &apps[sl];
}
int apps_live(void) { for (unsigned i = 0; i < APP_SLOTS; i++) if (apps[i].cur) return 1; return 0; }
long app_window_open(unsigned long info, unsigned long flags);                    /* the window half, with the other windows further down */
long app_window_poll(unsigned long ev, unsigned long flags);
void app_window_close(void);
long app_text(unsigned long req);   /* 3.9.0: JT_SYS_TEXT2 */
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
void app_kill_one(const char *how);
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
void app_kill_one(const char *how) { if (app_cur && !app_running) app_end((unsigned long)-1, how); }
void app_kill(const char *how) {   /* every app ends: a kernel-drawn window is opening where the apps' windows were */
    for (unsigned i = 0; i < APP_SLOTS; i++) { ap = &apps[i]; if (app_cur && !app_running) app_end((unsigned long)-1, how); }
    ap = &apps[app_top >= 0 ? app_top : 0];
}
void app_tick(void) {   /* every app with something to do gets its turn: it runs until it yields, exits or faults, and a fault ends that app alone */
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
int app_start(const struct user_app *a) {   /* 0 started (it runs on the next pass), -2 it would not load, -3 every slot is taken */
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
void user_demo(void) {
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
unsigned fb_w = FB_BOOT_W, fb_h = FB_BOOT_H;
#else
unsigned fb_w = 800, fb_h = 600;   /* the screen we really got */
#endif
unsigned disp_w, disp_h;           /* what the firmware said the monitor is, 0x0 when nobody asked */
unsigned int *fb;
unsigned fb_pitch;   /* in pixels */
int fb_swap;         /* red and blue the other way round in memory */
int sc(int v) { return v * (int)fb_h / 600; }   /* a length on the 800x600 design, at this screen's size */
int sg(int v) { return v * (int)fb_h / 540; }   /* a length on the real desktop's 960x540 grid (docs/ARCHITECTURE.md), at this screen's size */
void dcache_clean(void *p, unsigned long n) {   /* push lines out to RAM, where a GPU or DMA engine reads */
    for (unsigned long a = (unsigned long)p & ~63UL; a < (unsigned long)p + n; a += 64) __asm__ volatile ("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile ("dsb sy" ::: "memory");
}
/* 4.3.0: a back buffer. fb is a RAM copy of the screen (FB_RAM_BASE, same size and pitch as the scanout) and every painter
   draws there; fb_scan is the real scanout, and fb_flush is the one place that writes it: it copies the rectangle over and
   cleans it out of the data cache (the GPU reads RAM, not the cache). Without the RAM (a screen bigger than FB_RAM_MAX)
   fb_scan stays 0, fb is the scanout and fb_flush only cleans, as before 4.3.0. Between fb_hold and fb_release the flushes
   only add to one dirty rectangle, copied once at the outermost release: a pointer report that moves a window and the arrow
   reaches the screen as one copy, never as the half-drawn steps. fb_mute drops flushes while win_set names the damage. */
unsigned *fb_scan;
int fb_defer, fb_mute, fb_tag, fd_x0, fd_y0, fd_x1, fd_y1;   /* fb_tag: the batch moved a window (FB_TRACE prints it) */
int off_on;   /* off_begin's buffer is the target: a flush waits for off_end, as fb points into that buffer */
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
void fb_flush(int x, int y, int w, int h) {
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
void fb_hold(void) { fb_defer++; }
void fb_release(void) {
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
int fb_setup(void) {
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
volatile unsigned mbox[36] __attribute__((aligned(64)));
int mbox_call(void) {
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
int fb_setup(void) {
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
int screen_blank(unsigned blank) {
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
void fb_rect(int x, int y, int w, int h, unsigned c) {
    if (fb_swap) c = (c & 0xFF00FF00u) | (c >> 16 & 0xFF) | (c & 0xFF) << 16;
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) fb[j * fb_pitch + i] = c;
}
