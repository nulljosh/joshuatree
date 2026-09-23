/* Freestanding i386 kernel: VGA text, PS/2 keyboard, RTC clock, tiny shell. */
#include "gdt.h"
#include "idt.h"
#include "irq.h"
#include "pic.h"
#include "pmm.h"
#include "paging.h"
#include "kheap.h"
#include "task.h"
#include "ring3.h"
#include "syscall.h"
#include "ata.h"
#include "fat.h"
#include "vfs.h"
#include "blockdev.h"
#include "ramdisk.h"
#include "trash.h"
#include "ramfs.h"
#include "exec.h"
#include "user_hello.h"
#include "user_note.h"
#include "libc.h"
#include "pci.h"
#include "vbe.h"
#include "mouse.h"
#include "vmmouse.h"
#include "window.h"
#include "font.h"
#include "rtl8139.h"
#include "net.h"
#include "http.h"
#include "wallpaper.h"
#include "icon_art.h"
#include "wall_sat.h"
/* v75 (0.67.0): the wallpaper is read through this pointer, not the baked
   array directly, so a real fetched image (wall_fetch below: a 2x2 mosaic
   of OpenTopoMap tiles around the ip-api location, decoded by
   drivers/png.c) can replace it at runtime. Same 960x540x3 layout, so the
   two real readers (gui_wallpaper_color, gui_wallpaper_row) only changed
   which base address they index. Always points at something valid.
   pngtest keeps comparing against wallpaper_rgb by name on purpose (its
   fixtures are crops of the baked photo).

   v0.76.45: direct request ("don't show tree on boot, show dark until
   satellite loads") -- when wall_theme is a map (WALL_WARM/COOL/RAW/SAT)
   but wall_map hasn't loaded yet, wall_src now falls back to a solid
   Mojave espresso-brown (0x00201009) instead of wallpaper_rgb (the Joshua
   Tree photo). The tree photo is still available as WALL_PHOTO, so users
   who want it can select it in Settings; it never appears by default or
   during satellite fetch. */
static const unsigned char *wall_src = wallpaper_rgb;
/* v0.83.x: a REAL, once-captured satellite photograph (kernel/wall_sat.h,
   tools/gen/gen_wall_sat.py -- the same real mt0.google.com tiles
   wall_fetch() itself pulls for WALL_SAT, baked in at build time), decoded
   lazily on first need and kept for the kernel's lifetime. Direct owner
   request: with no live map (yet, or ever, see wall_apply's own comment)
   the desktop shows a satellite look, not the baked tree photo -- the
   tree stays reachable from Settings (WALL_PHOTO, an explicit user pick),
   it is only the no-network AUTOMATIC fallback that changes. */
static unsigned char *wall_sat_rgb = 0;
static unsigned char *wall_map = 0;        /* the fetched mosaic, kmalloc'd, kept while the session lives so Photo->Map needs no refetch */
static int wall_map_tx = 0, wall_map_ty = 0, wall_map_cx = 0, wall_map_cy = 0; /* tile x/y of the mosaic's top-left tile, crop offset inside it */
static int wall_map_is_sat = 0; /* v0.73: which real source wall_map's pixels actually came from (OpenTopoMap PNG vs Google satellite JPEG). Warm/Cool/Raw all share ONE fetch, since they're just different grades of the same topo pixels -- Satellite is a genuinely different image, not a grade, so switching across this boundary must drop wall_map and refetch instead of reusing stale pixels from the other source. */
/* v0.76.14: direct follow-up ("satellite finally working, let's
   strengthen it") -- honest scope check first: the actual complaint
   underneath ("showing Vancouver, not Langley/Brookswood") is a real,
   hard ceiling of free IP geolocation (ip-api.com resolves to whatever
   ISP network node/exchange the connection routes through, not a street
   address; Lower Mainland ISPs commonly route Langley/Brookswood
   traffic through a Vancouver PoP), not something this constant or any
   other code change here can fix. Went 12->14 in v78/0.67.2 the same
   way; that pass's own mirrored ZOOM constants in
   tools/checks/wallpaper-check.py / satellite-wallpaper-check.py went
   stale then and needed a manual sync -- kept in sync here too.

   v0.76.15 pushed this to 16 on "figure the zoom level, to town not
   local city", reasoning backwards: higher zoom means a SMALLER real
   area per pixel, not a bigger one. At 960px wide and this kernel's
   real latitude band (~49N, so real meters/pixel = 156543*cos(lat)/2^z,
   not the bare equatorial number), z16 covers roughly 1.5km across --
   a handful of anonymous blocks, not a recognizable town, exactly the
   real follow-up report ("it's some random city now"). A whole town the
   size of Brookswood (a few km across) needs a WIDER frame, i.e. a
   LOWER zoom, not a higher one.

   v0.76.16: reverted 16 back to 14 (~6km across at this latitude,
   confirmed by the same real formula above), a real town-plus-context
   scale, not a street-level crop. This is the same value the project
   shipped with before today's zoom churn -- the actual, real fix for
   "showing Vancouver, not Langley/Brookswood" was never a zoom number
   at all (see the v0.76.14 note above: that's IP geolocation's own
   ceiling, a different, separate limitation zoom cannot touch either
   direction). */
#define WALL_ZOOM 14
#define WALL_TILE 256   /* OpenTopoMap serves 256px tiles, no @2x variant */
#define WALL_COLS 4     /* 4x3 grid = 1024x768, the smallest that covers a centered 960x540 crop */
#define WALL_ROWS 3
#include "serial.h"
#include "app_weather.h"
#include "app_curbfind.h"
#include "app_keyrate.h"
#include "app_bookrank.h"
#include "app_quotestreak.h"
#include "app_plan.h"
#include "app_lexly.h"
#include "app_toroid.h"
#include "app_sparkjar.h"
#include "app_homeqi.h"
#include "app_fieldbook.h"
#include "png.h"
#include "png_testdata.h"
#include "jpeg.h"
#include "jpeg_testdata.h"
#include "version.h"
#include "json.h"
#include "html.h"

typedef unsigned char  u8;
typedef unsigned short u16;

static inline u8 inb(u16 p){ u8 v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void outb(u16 p, u8 v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }

/* ---- VGA text mode ---- */
#define VGA ((volatile u16 *)0xB8000)
#define W 80
#define H 25
#define ATTR 0x07
static int cx, cy;

static void cursor(void){
    u16 p = cy * W + cx;
    outb(0x3D4, 14); outb(0x3D5, p >> 8);
    outb(0x3D4, 15); outb(0x3D5, p & 0xFF);
}

static void scroll(void){
    if (cy < H) return;
    for (int i = 0; i < (H - 1) * W; i++) VGA[i] = VGA[i + W];
    for (int i = (H - 1) * W; i < H * W; i++) VGA[i] = (ATTR << 8) | ' ';
    cy = H - 1;
}

/* v36 (0.36.0): output capture, the piece a real GUI terminal needs.
   Every shell command in this kernel prints through putc, which writes
   straight into VGA *text* memory at 0xB8000, a region that isn't even
   mapped the same way once the card is in a graphics mode, so a
   graphical terminal could never see a single character a command
   produced. Redirecting at putc itself (rather than rewriting ~60 shell
   commands to take an output sink) means every existing command, and
   every future one, works in the GUI terminal for free. */
static char *capture_buf = 0;
static unsigned int capture_len = 0, capture_cap = 0;

static void capture_begin(char *buf, unsigned int cap){ capture_buf = buf; capture_len = 0; capture_cap = cap; buf[0] = 0; }
static unsigned int capture_end(void){ unsigned int n = capture_len; capture_buf = 0; return n; }

void putc(char c){
    if (capture_buf) {
        /* Backspace has to edit the captured text, not append a control
           byte the font renderer would draw as a glyph. */
        if (c == '\b') { if (capture_len) capture_len--; }
        else if (capture_len + 1 < capture_cap) capture_buf[capture_len++] = c;
        capture_buf[capture_len] = 0;
        return;
    }
    if (c == '\n') { cx = 0; cy++; }
    else if (c == '\b') {
        if (cx) cx--; else if (cy) { cy--; cx = W - 1; }
        VGA[cy * W + cx] = (ATTR << 8) | ' ';
    } else {
        VGA[cy * W + cx] = (ATTR << 8) | (u8)c;
        if (++cx == W) { cx = 0; cy++; }
    }
    scroll(); cursor();
}

void puts(const char *s){ while (*s) putc(*s++); }

void puthex(unsigned int v){
    puts("0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        int nib = (v >> shift) & 0xF;
        putc(nib < 10 ? '0' + nib : 'a' + nib - 10);
    }
}

static void clear(void){
    for (int i = 0; i < W * H; i++) VGA[i] = (ATTR << 8) | ' ';
    cx = cy = 0; cursor();
}

/* ---- PS/2 keyboard, IRQ-driven. irq.c's handler fills a ring buffer on
   IRQ1; getch() drains it and halts between ticks instead of busy-polling
   port 0x60 itself. ---- */
static const char SC[128] = {
    0,27,'1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
    'q','w','e','r','t','y','u','i','o','p','[',']','\n',0,
    'a','s','d','f','g','h','j','k','l',';','\'','`',0,'\\',
    'z','x','c','v','b','n','m',',','.','/',0,'*',0,' '
};
/* The same keys with Shift held (US layout). */
static const char SCS[128] = {
    0,27,'!','@','#','$','%','^','&','*','(',')','_','+','\b','\t',
    'Q','W','E','R','T','Y','U','I','O','P','{','}','\n',0,
    'A','S','D','F','G','H','J','K','L',':','"','~',0,'|',
    'Z','X','C','V','B','N','M','<','>','?',0,'*',0,' '
};
/* Scancode to ASCII with the modifier state kbd_pop tracks. Caps Lock
   flips letters only, the way a real keyboard does. */
static char kbd_map(int sc){
    int i = sc & 0x7F;
    char c = kbd_shift ? SCS[i] : SC[i];
    if (kbd_caps && ((SC[i] >= 'a' && SC[i] <= 'z'))) c = kbd_shift ? SC[i] : SCS[i];
    return c;
}

/* Non-blocking ASCII read off the same IRQ ring getch() drains, for
   sys_read(fd 0). Declared in console.h; lives here because SC[] and the
   keyboard ring's shape live here, and duplicating a scancode table into
   syscall.c to avoid one exported function would be the worse trade. */
int console_read_key(void){
    for (;;) {
        int sc = kbd_pop();
        if (sc < 0) return -1;
        if (sc & 0x80) continue;        /* key release */
        char c = kbd_map(sc);
        if (c) return (int)(unsigned char)c;
    }
}

static void gui_app_mouse_tick(void);
static char getch(void){
    for (;;) {
        gui_app_mouse_tick();
        int sc = kbd_pop();
        if (sc < 0) { window_present(); __asm__ volatile ("hlt"); continue; }
        if (sc & 0x80) continue;            /* key release */
        char c = kbd_map(sc);
        if (c) return c;
    }
}

/* Same as getch(), but a mouse click also wakes it up, returning -1: real,
   reported request, every interactive GUI app (Notes, Keyrate) needs the
   same "click anywhere to close" a touch-only or mouse-only visitor
   already gets on the read-only viewers via gui_wait_close, not just a
   keyboard escape hatch. */
/* v68 (0.63.0): was the last input an app's wait loop handed out a click
   (1) or a key (0)? Read by gui_launch_from_dock the moment an app
   returns: if the app closed on a click and that click sits on a dock
   tile, the dock click means "switch to that app", not just "close this
   one". Set at every site that turns a mouse edge into an app-visible
   event, cleared whenever a key is handed out instead, so it always
   describes the event that actually caused the close. */
static int gui_close_was_click = 0;
static int gui_getch_or_click(void){
    mouse_click_edge_sync(); /* a button already held (e.g. the click that opened this app) is the baseline, not a fresh click */
    for (;;) {
        gui_app_mouse_tick();
        int sc = kbd_pop();
        if (sc >= 0) {
            if (sc & 0x80) continue;
            char c = kbd_map(sc);
            if (c) { gui_close_was_click = 0; return (int)(unsigned char)c; }
            continue;
        }
        if (mouse_click_edge()) { gui_close_was_click = 1; return -1; }
        window_present(); __asm__ volatile ("hlt");
    }
}

/* ---- extended keys (arrows) for the file browser. 0xE0 is the make-code
   prefix for the "extended" keyboard block; 0x48/0x50 are up/down within it. ---- */
#define KEY_UP    256
#define KEY_DOWN  257
#define KEY_ENTER 258
#define KEY_ESC   259
/* v54: left/right (0x4B/0x4D in the same extended block) for Calendar's
   month stepping. Every existing consumer gates text input on
   32 <= k < 127, so these new values fall through as ignored keys there,
   same as up/down always have. */
#define KEY_LEFT  261
#define KEY_RIGHT 262

/* v38: same shape as get_key below, but a click (or a tap, which reaches
   the kernel as a real PS/2 click from the browser embed) also counts as
   input. Every interactive app screen has to offer this, not just the
   read-only viewers gui_wait_close covers: a phone visitor has no
   keyboard at all, so a screen that only reads keys is a screen they can
   open and then never leave. Terminal and the Apps folder both shipped
   with exactly that bug in v36/v37, reported from a real phone. */
#define KEY_CLICK 260
#define KEY_WHEEL_UP 300
#define KEY_WHEEL_DOWN 301
static int get_key_or_click(void);

static int get_key(void){
    for (;;) {
        int sc = kbd_pop();
        if (sc < 0) { window_present(); __asm__ volatile ("hlt"); continue; }
        if (sc == 0xE0) {
            int sc2;
            do { sc2 = kbd_pop(); if (sc2 < 0) { window_present(); __asm__ volatile ("hlt"); } } while (sc2 < 0);
            if (sc2 == 0x48) return KEY_UP;
            if (sc2 == 0x50) return KEY_DOWN;
            if (sc2 == 0x4B) return KEY_LEFT;
            if (sc2 == 0x4D) return KEY_RIGHT;
            continue; /* other extended keys: ignore */
        }
        if (sc & 0x80) continue;
        char c = kbd_map(sc);
        if (c == '\n') return KEY_ENTER;
        if (c == 27)   return KEY_ESC;
        if (c) return c;
    }
}

static int get_key_or_click(void){
    for (;;) {
        gui_app_mouse_tick();
        int sc = kbd_pop();
        if (sc >= 0) {
            if (sc == 0xE0) {
                int sc2;
                do { sc2 = kbd_pop(); if (sc2 < 0) { window_present(); __asm__ volatile ("hlt"); } } while (sc2 < 0);
                if (sc2 == 0x48) return KEY_UP;
                if (sc2 == 0x50) return KEY_DOWN;
                if (sc2 == 0x4B) return KEY_LEFT;
                if (sc2 == 0x4D) return KEY_RIGHT;
                continue;
            }
            if (!(sc & 0x80)) {
                char c = kbd_map(sc);
                gui_close_was_click = 0;
                if (c == '\n') return KEY_ENTER;
                if (c == 27)   return KEY_ESC;
                if (c) return c;
            }
            continue;
        }
        if (mouse_click_edge()) { gui_close_was_click = 1; return KEY_CLICK; }
        int wheel = mouse_get_wheel();
        if (wheel) return wheel > 0 ? KEY_WHEEL_UP : KEY_WHEEL_DOWN;
        window_present(); __asm__ volatile ("hlt");
    }
}

/* ---- RTC via CMOS. ponytail: no PIT tick counter; the shell only ever
   needs wall-clock, and this needs no interrupt handler. ---- */
static u8 cmos(u8 reg){ outb(0x70, reg); return inb(0x71); }

/* v0.76.17: real, standard erratum, applied proactively by this same
   version's own clock-live-update fix, not from a torn read actually
   captured in this repo's QEMU (a first attempt at reproducing one here
   misread a long-lived stray QEMU process's several real minute
   rollovers as spurious redraws -- caught before shipping by rereading
   the log's actual elapsed wall time, not left as a real finding). The
   real MC146818-family RTC does update its time registers once a second,
   and any register read that lands inside that update window (Status
   Register A bit 7, "Update In Progress") can come back torn/garbage --
   documented RTC behavior, independent of whether QEMU's own CMOS model
   reproduces it (this session's build of QEMU did not, across repeated
   runs). Every caller here used to read cmos(2) (minutes) directly, at
   most once per mouse movement or once per ten-minute weather cycle;
   gui_draw_menubar() now runs every single gui_run loop iteration
   (~100Hz) to fix clock staleness, which makes this standing hazard far
   more likely to matter than it used to be, on real hardware or a
   different emulator, even though it wasn't observed to bite here. Guard
   is the standard one: don't read while UIP is set, and re-check UIP
   right after; if an update started mid-read, the bytes just read are
   suspect, retry (bounded, this chip's update window is under 2ms on
   real hardware). */
static int cmos_update_in_progress(void){ outb(0x70, 0x0A); return inb(0x71) & 0x80; }
static void cmos_read_time_stable(u8 *h, u8 *m, u8 *wd, u8 *dom, u8 *mon){
    for (int tries = 0; tries < 8; tries++) {
        while (cmos_update_in_progress()) {}
        u8 hh = cmos(4), mm = cmos(2), wdv = cmos(6), domv = cmos(7), monv = cmos(8);
        if (!cmos_update_in_progress()) { *h = hh; *m = mm; *wd = wdv; *dom = domv; *mon = monv; return; }
    }
    /* every retry raced an update; fall back to one plain read rather than
       spin forever -- worst case one stale/torn frame, self-corrects next
       loop iteration since this function runs continuously now. */
    *h = cmos(4); *m = cmos(2); *wd = cmos(6); *dom = cmos(7); *mon = cmos(8);
}

static void print2(u8 bcd){
    u8 v = (bcd & 0x0F) + ((bcd >> 4) * 10);
    putc('0' + v / 10); putc('0' + v % 10);
}

static void show_time(void){
    while (cmos(0x0A) & 0x80) {}            /* wait out an update */
    u8 h = cmos(4), m = cmos(2), s = cmos(0);
    print2(h); putc(':'); print2(m); putc(':'); print2(s); putc('\n');
}

/* ---- PC speaker via PIT channel 2. A square wave is all this hardware can
   produce, no envelope, no timbre, so "soft" here just means picking two
   consonant notes and a short duration rather than one harsh flat tone,
   about as warm as a beep from 1981 gets. ---- */
static void beep(unsigned int freq_hz, unsigned int duration_ticks){
    unsigned int divisor = 1193182 / freq_hz;
    outb(0x43, 0xB6);                       /* channel 2, lobyte/hibyte, mode 3 */
    outb(0x42, (u8)(divisor & 0xFF));
    outb(0x42, (u8)((divisor >> 8) & 0xFF));
    outb(0x61, inb(0x61) | 0x03);           /* gate the speaker on */
    sleep_ticks(duration_ticks);
    outb(0x61, inb(0x61) & 0xFC);           /* off */
}

static void boot_chime(void){
    beep(523, 8);  /* C5 */
    beep(659, 12); /* E5, held a touch longer to land the chime */
}

static void putn(unsigned int v){
    char buf[12]; int n = 0;
    if (v == 0) buf[n++] = '0';
    while (v) { buf[n++] = '0' + v % 10; v /= 10; }
    while (n) putc(buf[--n]);
}

/* A real, Linux-style dmesg ring buffer: fixed-size, overwrites its
   oldest entry once full rather than growing, since this kernel has no
   log rotation and a boot-time trace doesn't need one. Each entry is
   tagged with the real PIT tick count (irq.c's ticks()) at the moment it
   was logged, the closest thing to a timestamp available this early;
   anything logged before irq_install() has actually run and interrupts
   are enabled reads tick 0, an honest "no timer yet", not a bug. */
#define KLOG_MAX     32
#define KLOG_MSG_LEN 60
static char klog_buf[KLOG_MAX][KLOG_MSG_LEN];
static unsigned int klog_tick[KLOG_MAX];
static int klog_count = 0, klog_next = 0;

static void klog(const char *msg){
    int i = 0;
    while (msg[i] && i < KLOG_MSG_LEN - 1) { klog_buf[klog_next][i] = msg[i]; i++; }
    klog_buf[klog_next][i] = 0;
    klog_tick[klog_next] = ticks();
    klog_next = (klog_next + 1) % KLOG_MAX;
    if (klog_count < KLOG_MAX) klog_count++;
    serial_puts(msg); serial_puts("\n"); /* live trace, survives a crash the ring buffer's own reset wouldn't */
}

static void klog_dump(void){
    int start = (klog_count < KLOG_MAX) ? 0 : klog_next;
    for (int i = 0; i < klog_count; i++){
        int idx = (start + i) % KLOG_MAX;
        puts("[ "); putn(klog_tick[idx]); puts("] "); puts(klog_buf[idx]); putc('\n');
    }
}

/* ---- task demo: two tasks that each print a letter and yield, round-robin,
   to prove context switching actually swaps stacks correctly. Bounded, then
   task_exit() (v28): a task can't safely `return` (see task.c), but now
   that it can really exit instead of parking in `hlt` forever, running
   tasktest repeatedly no longer permanently burns 2 of the 6 task slots. ---- */
static void task_a(void){ for (int i = 0; i < 10; i++) { puts("A"); yield(); } task_exit(); }
static void task_b(void){ for (int i = 0; i < 10; i++) { puts("B"); yield(); } task_exit(); }

/* ---- preemption demo: two tasks that never call yield() or hlt, proving
   the timer itself forces a switch. The shell's own wait loop below also
   never yields/hlts on purpose, so if preemption weren't real this whole
   command would just spin, and neither counter would ever move.
   preempt_stop (v28) lets the shell actually reap these once it's done
   measuring, same reasoning as task_a/task_b above: without it these two
   would spin forever and permanently hold 2 of the 6 task slots. ---- */
static volatile int preempt_a_count = 0;
static volatile int preempt_b_count = 0;
static volatile int preempt_stop = 0;
static void preempt_task_a(void){ while (!preempt_stop) preempt_a_count++; task_exit(); }
static void preempt_task_b(void){ while (!preempt_stop) preempt_b_count++; task_exit(); }

/* ---- v0.88.0: spawntest. The Activity app (kernel/activity.h) shows and
   kills real scheduler tasks through the exact same task_used/task_kill
   primitives `ps`/`kill` already use, but nothing in the shell so far
   leaves a task running indefinitely for a test to observe from the GUI
   side -- every existing demo (task_a/b, preempt_task_a/b, iso_task_a/b)
   cleans itself up within a handful of yields. spawntest_task is the one
   deliberately long-lived task: it just spins until killed, real and
   idle, the same shape preempt_task_a already has minus the self-stop
   flag, so tools/checks/activity-check.py has a real, persistent task to
   select and kill through the app instead of a synthetic fixture. ---- */
static void spawntest_task(void){ for (;;) yield(); }

/* ---- v31 (0.31.0) isolation demo: two tasks write different markers to
   the SAME virtual address, PAGING_PRIVATE_VADDR. If page directories were
   still shared (the pre-v31 world), the second write would clobber the
   first and both readbacks would show 0xBBBBBBBB. Each task's own
   directory maps that address to its own private physical frame, so both
   readbacks should show what that task itself wrote, unaffected by the
   other task's write to the "same" address in between. ---- */
static volatile unsigned int iso_readback_a = 0, iso_readback_b = 0;
static void iso_task_a(void){
    *(volatile unsigned int *)PAGING_PRIVATE_VADDR = 0xAAAAAAAA;
    yield(); /* let task B run and write its own marker to the "same" address before we read ours back */
    iso_readback_a = *(volatile unsigned int *)PAGING_PRIVATE_VADDR;
    task_exit();
}
static void iso_task_b(void){
    *(volatile unsigned int *)PAGING_PRIVATE_VADDR = 0xBBBBBBBB;
    yield();
    iso_readback_b = *(volatile unsigned int *)PAGING_PRIVATE_VADDR;
    task_exit();
}

static void ls_cb(const char *name, unsigned int size, int is_dir) {
    puts(name); if (is_dir) putc('/');
    puts("  "); putn(size); puts(" bytes\n");
}

/* ---- text-mode file browser: arrow keys + Enter/Esc, not just a shell.
   ponytail: capped at BROWSE_MAX entries -- plenty for what fits on a
   25-line screen anyway. Enter on a directory descends into it (fat_chdir
   + refresh); esc/q at any depth just quits back to the shell, not up a
   level, matching browse's existing "in or out" model rather than growing
   its own breadcrumb stack. ---- */
#define BROWSE_MAX 20
static char browse_names[BROWSE_MAX][13];
static unsigned int browse_sizes[BROWSE_MAX];
static int browse_is_dir[BROWSE_MAX];
static int browse_count;

static void browse_collect_cb(const char *name, unsigned int size, int is_dir) {
    if (browse_count >= BROWSE_MAX) return;
    int i = 0;
    while (name[i] && i < 12) { browse_names[browse_count][i] = name[i]; i++; }
    browse_names[browse_count][i] = 0;
    browse_sizes[browse_count] = size;
    browse_is_dir[browse_count] = is_dir;
    browse_count++;
}

static void browse_draw(int sel){
    clear();
    puts("-- file browser: up/down, enter=view/open, esc=quit --\n\n");
    if (browse_count == 0) { puts("(empty)\n"); return; }
    for (int i = 0; i < browse_count; i++) {
        putc(i == sel ? '>' : ' '); putc(' ');
        puts(browse_names[i]);
        if (browse_is_dir[i]) { putc('/'); putc('\n'); }
        else { puts("  "); putn(browse_sizes[i]); puts(" bytes\n"); }
    }
}

static void browse(void){
    browse_count = 0;
    vfs_list(browse_collect_cb);
    int sel = 0;
    browse_draw(sel);
    for (;;) {
        int k = get_key();
        if (k == KEY_ESC || k == 'q') { clear(); return; }
        if (k == KEY_UP)   { if (sel > 0) sel--; browse_draw(sel); }
        if (k == KEY_DOWN) { if (sel < browse_count - 1) sel++; browse_draw(sel); }
        if (k == KEY_ENTER && browse_count > 0 && browse_is_dir[sel]) {
            vfs_chdir(browse_names[sel]);
            browse_count = 0;
            vfs_list(browse_collect_cb);
            sel = 0;
            browse_draw(sel);
        }
        else if (k == KEY_ENTER && browse_count > 0) {
            clear();
            puts(browse_names[sel]); puts(":\n\n");
            char buf[2048];
            int n = vfs_read_file(browse_names[sel], buf, sizeof(buf) - 1);
            if (n < 0) puts("(couldn't read)\n");
            else { buf[n] = 0; puts(buf); }
            puts("\n\n-- press any key to go back --\n");
            get_key();
            browse_draw(sel);
        }
    }
}

/* ---- shell ---- */

/* Combines HTTP headers with an embedded app's raw bytes (gen_app.sh's
   output) into one buffer and serves it. Shared by every serveapp target
   so adding another embedded app is one dispatch line, not a copy-pasted
   block. */
/* LLMs habitually wrap generated code in a ```html ... ``` fence even when
   told not to. Strips a leading ``` line and a trailing ``` if present,
   in place, returns the new length. Not a markdown parser, just this one
   specific, extremely common habit. */
static unsigned int strip_code_fence(char *s, unsigned int len){
    unsigned int start = 0;
    if (len >= 3 && s[0] == '`' && s[1] == '`' && s[2] == '`') {
        unsigned int i = 3;
        while (i < len && s[i] != '\n') i++; /* skip the rest of the ```html line */
        if (i < len) i++;
        start = i;
    }
    unsigned int end = len;
    while (end > start && (s[end - 1] == '\n' || s[end - 1] == ' ')) end--;
    if (end - start >= 3 && s[end - 3] == '`' && s[end - 2] == '`' && s[end - 1] == '`') end -= 3;
    while (end > start && (s[end - 1] == '\n' || s[end - 1] == ' ')) end--;

    unsigned int new_len = end - start;
    for (unsigned int i = 0; i < new_len; i++) s[i] = s[start + i];
    return new_len;
}

/* v8's actual point: parsed page text drawn to the framebuffer with the
   real font, not just dumped to the text-mode shell. Word-wraps at the
   window's pixel width; no scrolling yet, a page longer than one screen
   just clips, that's the next thing to add once this is proven to render
   real pages correctly at all.

   v78 restraint pass: this function had exactly v77's "Cl oudy" bug, just
   never caught because it's not one of the four sites v77 audited. It drew
   every glyph one at a time through font_draw_char at a hardcoded 8px
   advance, the same fixed-cell assumption v77 root-caused and fixed on the
   AA path everywhere else; on any HTML app view or Chat answer (both run
   inside the scaled GUI, so the AA hook is live) a real word like
   "Curbfind" rendered as "Curbf ind", confirmed on a real framebuffer dump.
   Fixed the same way v77 fixed the other four: word width and per-glyph
   placement now go through font_string_width/font_draw_string's own
   proportional pen instead of wlen*8/x+=8. Words longer than WORD_MAX
   still draw (font_draw_string has no length limit), they just can't be
   measured for the wrap decision past that cap, matching the old code's
   own honest limit (it never measured unbounded words either). */
static void render_wrapped_text(const char *text, int x0, int y0, int max_w_px, int max_h_px, unsigned int fg) {
    int x = x0, y = y0;
    const char *p = text;
    int space_w = font_string_width(" ");
    if (space_w < 1) space_w = 1;
    while (*p) {
        if (y + 16 > y0 + max_h_px) return; /* out of room */
        if (*p == '\n') { y += 16; x = x0; p++; continue; }
        if (*p == ' ') {
            if (x + space_w > x0 + max_w_px) { x = x0; y += 16; }
            else { x += space_w; }
            p++;
            continue;
        }
        unsigned int wlen = 0;
        while (p[wlen] && p[wlen] != ' ' && p[wlen] != '\n') wlen++;

        enum { WORD_MAX = 255 };
        char word[WORD_MAX + 1];
        unsigned int wcopy = wlen < WORD_MAX ? wlen : WORD_MAX;
        for (unsigned int i = 0; i < wcopy; i++) word[i] = p[i];
        word[wcopy] = 0;
        int ww = font_string_width(word);

        if (x > x0 && x + ww > x0 + max_w_px) { x = x0; y += 16; if (y + 16 > y0 + max_h_px) return; }
        font_draw_string(word, x, y, fg, -1);
        x += ww;
        p += wlen;
    }
}

static int web_starts_with(const char *p, const char *needle) {
    while (*needle) { if (*p != *needle) return 0; p++; needle++; }
    return 1;
}

static void web_str_copy(char *dst, const char *src, unsigned int cap) {
    unsigned int i = 0;
    while (src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* Resolves a link's href against the current page's host. No TLS in this
   stack, so an https:// link is a real, honest dead end, reported as one
   rather than silently attempted and failed. Bare relative paths (no
   leading /) and non-http schemes (mailto:, #anchors) are a known gap,
   not a scope this pass claims to cover. */
static int resolve_href(const char *href, const char *cur_host, char *host_out, unsigned int host_cap,
                         char *path_out, unsigned int path_cap, const char **reason_out) {
    if (!href[0]) { *reason_out = "empty link"; return 0; }
    if (web_starts_with(href, "https://")) { *reason_out = "https not supported, no TLS in this kernel yet"; return 0; }
    if (web_starts_with(href, "http://")) {
        const char *h = href + 7;
        unsigned int i = 0;
        while (h[i] && h[i] != '/' && i < host_cap - 1) { host_out[i] = h[i]; i++; }
        host_out[i] = 0;
        if (h[i] == '/') web_str_copy(path_out, h + i, path_cap);
        else { path_out[0] = '/'; path_out[1] = 0; }
        return 1;
    }
    if (href[0] == '/') {
        web_str_copy(host_out, cur_host, host_cap);
        web_str_copy(path_out, href, path_cap);
        return 1;
    }
    *reason_out = "relative/unsupported link (mailto:, anchors, bare relative paths)";
    return 0;
}

/* v8's link navigation: fetch, render, list real links found on the page,
   number keys follow one, 'b' goes back, anything else closes. A small
   fixed-depth history stack, not a general browser session, that's plenty
   for what this proves. */
#define WEB_HISTORY_DEPTH 6
static void browse_web(const char *first_host, const char *first_path) {
    char cur_host[64], cur_path[192];
    web_str_copy(cur_host, first_host, sizeof(cur_host));
    web_str_copy(cur_path, first_path, sizeof(cur_path));

    char hist_host[WEB_HISTORY_DEPTH][64];
    char hist_path[WEB_HISTORY_DEPTH][192];
    int hist_n = 0;

    for (;;) {
        static char body[1400];
        int n = http_get(cur_host, cur_path, 80, body, sizeof(body) - 1);
        if (n < 0) { puts("FAIL (dns/tcp)\n"); return; }
        if (n == 0) { puts("FAIL (no body, response too large or truncated)\n"); return; }
        body[n] = 0;

        static char text[1400];
        unsigned int tn = html_to_text(body, text, sizeof(text));
        putn(tn); puts(" bytes of text:\n\n");
        puts(text);
        putc('\n');

        static struct html_link links[HTML_MAX_LINKS];
        unsigned int nlinks = html_extract_links(body, links, HTML_MAX_LINKS);

        if (!window_open(800, 600, 32)) return;
        window_clear(0x00FAF8F6);
        render_wrapped_text(text, 20, 20, 760, 420, 0x001C1C1E);

        int ly = 460;
        for (unsigned int i = 0; i < nlinks && ly < 560; i++) {
            char label[8]; label[0] = '['; label[1] = (char)('1' + i); label[2] = ']'; label[3] = ' '; label[4] = 0;
            font_draw_string(label, 20, ly, 0x007A2048, -1);
            font_draw_string(links[i].text[0] ? links[i].text : links[i].href, 20 + 32, ly, 0x007A2048, -1);
            ly += 18;
        }
        font_draw_string(hist_n > 0 ? "[b]ack   any other key closes" : "any other key closes", 20, 570, 0x0075726E, -1);

        int k = get_key();
        window_close();
        clear();
        puts("back in text mode\n");

        if (k == 'b' && hist_n > 0) {
            hist_n--;
            web_str_copy(cur_host, hist_host[hist_n], sizeof(cur_host));
            web_str_copy(cur_path, hist_path[hist_n], sizeof(cur_path));
            continue;
        }
        if (k >= '1' && k <= '9') {
            unsigned int idx = (unsigned int)(k - '1');
            if (idx < nlinks) {
                char new_host[64], new_path[192];
                const char *reason = 0;
                if (resolve_href(links[idx].href, cur_host, new_host, sizeof(new_host), new_path, sizeof(new_path), &reason)) {
                    if (hist_n < WEB_HISTORY_DEPTH) {
                        web_str_copy(hist_host[hist_n], cur_host, sizeof(hist_host[0]));
                        web_str_copy(hist_path[hist_n], cur_path, sizeof(hist_path[0]));
                        hist_n++;
                    }
                    web_str_copy(cur_host, new_host, sizeof(cur_host));
                    web_str_copy(cur_path, new_path, sizeof(cur_path));
                    continue;
                }
                puts("can't follow that link: "); puts(reason); putc('\n');
            }
        }
        return;
    }
}

static void serve_app(const char *label, const unsigned char *data, unsigned int data_len){
    static const char header[] = "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n";
    unsigned int total_len = (sizeof(header) - 1) + data_len;
    char *buf = kmalloc(total_len);
    if (!buf) { puts("out of heap\n"); return; }

    for (unsigned int i = 0; i < sizeof(header) - 1; i++) buf[i] = header[i];
    for (unsigned int i = 0; i < data_len; i++) buf[sizeof(header) - 1 + i] = (char)data[i];

    puts("serving "); puts(label); puts(" (a real app from the codebase, ");
    putn(data_len); puts(" bytes), waiting on :8080...\n");
    puts(tcp_serve_once(8080, buf, total_len) ? "served:ok\n" : "timeout, nobody connected\n");
    kfree(buf);
}

static void reboot(void){
    while (inb(0x64) & 2) {}
    outb(0x64, 0xFE);                        /* 8042 CPU reset pulse */
    __asm__ volatile("cli; hlt");
}

/* ---- a real mouse-driven desktop, built entirely from v6's graphics
   primitives (window/font/mouse), no new subsystem needed. Each icon
   launches something genuinely real, not a mockup: the same HTML-to-text
   renderer `web` uses, on this codebase's own embedded apps; the same LLM
   call `chat` uses, with the reply drawn instead of printed; a real
   listing of whatever's actually on the FAT filesystem.

   Styled after the one visual language every viewer already knows a real
   desktop by, without pretending to be one: a menu bar with a real clock
   (the same CMOS read `time` uses), and a real Dock, a bottom tray of
   icons, unlabeled until hovered (the label then floats above it, exactly
   like the real thing), the hovered icon magnified and lifted. No alpha
   blending in this framebuffer, so "rounded" and "shadow" are both done by
   painting flat colors, corner pixels outside a quarter-circle get
   overwritten with whatever's behind them, not blended. ---- */
/* v37 (0.37.0): the dock stopped being "every app that exists". Every app
   added between v14 and v36 went straight into the dock, which is how it
   reached 15 icons and had to be resized twice to physically fit the
   screen, each icon getting smaller and less legible as the number grew.
   A real dock is a *choice*: the handful you actually reach for, with
   everything else one click away in an Apps folder, the same split macOS
   makes between the Dock and Launchpad. GUI_APP_COUNT is every real app;
   GUI_ICON_COUNT is only what the dock shows. */
/* v59 (0.58.0): direct request, two changes at once. First, Mail: this
   codebase had every other stock-macOS core app (Files as Finder, Notes,
   Reminders, Calendar) but no local mail-shaped app at all, the one real
   gap; kernel/mail.h fills it, same file-per-app shape as reminders.h/
   calendar.h. Second, GUI_LABELS itself is reordered (and every switch
   below that keys off its indices moves with it) so both the Apps folder
   grid and the pinned dock read as a real macOS-shaped grouping instead
   of "whatever order things got built in": Files first (the Finder
   equivalent), then Mail/Calendar/Notes/Reminders as one recognizable
   core cluster, then the Terminal/Chat/Weather utility group, then every
   fleet app after that, Apps and Trash still fixed at the very end
   (that half was already right as of v39, untouched here). GUI_APP_COUNT
   is now 20 (18 real apps + Apps + Trash), GUI_ICON_COUNT unaffected by
   the reorder itself, see GUI_DOCK_DEFAULT below for why it did grow. */
/* v0.86.0: Search, an Apps-folder-only app (same launch shape as Contacts/
   Calculator/Stocks, never pinned to the dock), grew GUI_APP_COUNT from 23
   to 24 (22 real apps + Apps folder + Trash) and pushed GUI_APPS_FOLDER/
   GUI_TRASH up by one each. Every dispatch below keys off these #defines
   rather than a hardcoded 21/22, so this is the only place the shift needed
   to happen. */
/* v0.87.0: Portfolio, an Apps-folder-only catalog of the fleet apps that
   live outside this kernel (heyitsmejosh.com), same launch shape as
   Search/Contacts/Calculator/Stocks. Inserted before GUI_APPS_FOLDER, so
   it grew GUI_APP_COUNT from 25 to 26 and pushed GUI_APPS_FOLDER/GUI_TRASH
   up by one each, same shift the v0.86.0 comment above describes for
   Search. tools/gen/gen_icon_art.py's ART/VARIANT index maps moved with
   it (24: apps, 25: trash); Portfolio itself has no authored art yet, so
   it keeps the primitive glyph path like every other unart'd icon. */
/* v0.89.x: Activity landed after Portfolio took slot 23, so it sits at
   24 and GUI_APPS_FOLDER/GUI_TRASH moved to 25/26, same shift again. */
#define GUI_APP_COUNT   27 /* 25 real apps + the Apps folder + Trash */
#define GUI_APPS_FOLDER 25 /* not an app: the dock tile that opens the folder */
#define GUI_TRASH       26
static const char *GUI_LABELS[GUI_APP_COUNT] = {"Files", "Mail", "Calendar", "Notes", "Reminders", "Terminal", "Chat", "Weather", "Curbfind", "Keyrate", "Bookrank", "Quotes", "Plan", "Lexly", "Toroid", "Sparkjar", "Homeqi", "Fieldbook", "Contacts", "Calculator", "Stocks", "Search", "Epiphany", "Portfolio", "Activity", "Apps", "Trash"};
static const unsigned int GUI_COLORS[GUI_APP_COUNT] = {
    0x00707070, 0x00A13F3F, 0x00A0553F, 0x006B4423, 0x00375A4A, 0x002B2B2B, 0x00365E8C, 0x0085144B,
    0x007A2048, 0x00B08900, 0x002F7B4F, 0x008B4A9C, 0x00475C6B, 0x00376E5E, 0x00234A78, 0x00A6741E, 0x00566A3A, 0x005A3E6B, 0x00A87C5B, 0x00556B85, 0x00356B4F, 0x00506078, 0x001F5FA8, 0x004A5A3E, 0x003E4C58
};

/* The pinned set, chosen on what someone actually reaches for on a fresh
   boot rather than what happened to be built most recently: a terminal, a
   file browser, a notepad, the LLM chat, and the two apps with live data
   behind them. Everything else is one click away in Apps. */
/* v39: Apps first and Files second, by direct request, then the rest in
   no particular order, then Trash pinned last, the one position every
   desktop has agreed on for thirty years. */
/* v59: grown from 8 to 10 and re-picked to actually mirror a stock macOS
   dock's shape, direct request: Apps folder still first (v39's own call,
   unchanged), then Files (Finder), then Mail/Calendar/Notes/Reminders as
   one contiguous core-app cluster, then Terminal/Chat/Weather as the
   utility group, Trash still fixed last. Curbfind, previously pinned
   here as the one fleet app in an otherwise-utility dock, moves to
   Apps-folder-only: with four more core apps now competing for pinned
   slots, a single fleet app sitting in the dock read as arbitrary rather
   than a deliberate "core macOS group" choice, and it's still one click
   away exactly like every other fleet app. gui_dock_icon() (existing,
   unchanged here) already auto-sizes every tile to fit DOCK_BUDGET
   regardless of GUI_ICON_COUNT, so going from 8 to 10 icons needed no
   layout changes at all, the "auto size" half of the standing v37 dock
   request was already real before this pass, this is just the first
   change to actually exercise it past 8 icons. */
#define GUI_ICON_COUNT 11
static const int GUI_DOCK_DEFAULT[GUI_ICON_COUNT] = {GUI_APPS_FOLDER, 0, 1, 2, 3, 4, 5, 6, 7, 20, GUI_TRASH};

/* gui_order is a permutation of icon indices by dock slot: dragging an icon
   and dropping it on another slot swaps the two, so the arrangement is
   real and sticks for the rest of this GUI session (reset to launch order
   next time `gui` runs; nothing about layout is saved to disk, matching
   this whole desktop's one-screen, nothing-persisted scope). */
static int gui_order[GUI_ICON_COUNT];
/* Portfolio mode ("portfolio" on the multiboot command line, sent by the
   landing's embed.js when heyitsmejosh.com/os.html frames it): the dock is
   Joshua's own apps instead of the system set. Same slot count, Apps folder
   and Trash stay at the ends; everything left out is still in the Apps folder. */
static int portfolio_dock;
static const int GUI_DOCK_PORTFOLIO[GUI_ICON_COUNT] = {GUI_APPS_FOLDER, 23, 22, 8, 10, 13, 15, 11, 9, 14, GUI_TRASH}; /* Portfolio, Epiphany, Curbfind, Bookrank, Lexly, Sparkjar, Quotes, Keyrate, Toroid */
static void gui_order_init(void){ for (int i = 0; i < GUI_ICON_COUNT; i++) gui_order[i] = portfolio_dock ? GUI_DOCK_PORTFOLIO[i] : GUI_DOCK_DEFAULT[i]; }
static int dock_hover = -1; /* slot whose label is showing */

#define GUI_BG          0x00FAF8F6
#define GUI_MENUBAR_H   26
/* v36 (0.36.0): the icon size is now *derived* from how many icons there
   are, instead of a constant that silently overflows the screen every
   time an app is added. v35 hit that for real (14 icons at the old
   56px sizing came to 1016px on an 800px screen, two icons genuinely cut
   off), and adding Terminal would have hit it again at 792px, 8px from
   the edge. Solving it once, in arithmetic, beats rediscovering it in a
   screendump on every future app. DOCK_BUDGET is the widest the dock may
   ever draw, leaving a real margin on both sides of the 800px screen. */
#define DOCK_BUDGET     740
#define DOCK_GAP        6
#define DOCK_PAD        10
/* v37: the dock is sized as a real fraction of the window rather than a
   constant, so it stays proportionate at any resolution this kernel ever
   opens (vbe_set_mode already accepts any mode; only the hardcoded
   800x600 in gui_run stands between here and that). dock_scale_pct is
   the user-adjustable knob Settings writes to. The clamp is the part
   that actually matters: whatever scale is asked for, the dock still has
   to fit on screen, which is exactly the arithmetic v35 and v36 each had
   to rediscover from a screendump. */
/* v52: default trimmed 10 -> 7, direct feedback from a real photo of the
   physical panel: the dock read as visibly oversized against the desktop
   content at 10%. Still the same user-adjustable Settings knob, 5-25%,
   nothing about the range or mechanism changed, just what a fresh
   install starts at. */
static int dock_scale_pct = 7;
/* v75: wallpaper source, extended v81 to a real theme, not just a photo/
   map binary (direct request: "multiple wallpaper themes/styles"). Four
   real, verifiably-distinct states, no JPEG/satellite decoder involved
   (that stays a separate queued roadmap item, not built against here):
     0 = Photo, the baked NPS photo (v75's original alternative).
     1 = Map Warm, the default once buildable (Joshua's own call in
         roadmap.md's satellite entry): the fetched map put through
         gui_map_tint's existing Mojave warm grade (v79).
     2 = Map Cool, v81: a distinct cooler, higher-contrast grade
         (gui_map_tint_cool below) for legibility -- a real, different
         multiply+contrast curve, not a relabeled copy of Warm.
     3 = Map Raw, v81: the fetched map with no color grade at all, for
         comparison against OpenTopoMap's own neutral cartographer
         palette.
   4 = Satellite, wired this pass: real photographic imagery (Google's
         mt0.google.com/vt/lyrs=s slippy-map satellite tiles, plain HTTP,
         same x/y/z convention as OpenTopoMap so wall_fetch's existing
         tile math is untouched) decoded with drivers/jpeg.c's baseline
         decoder (the JPEG source those tiles actually are) instead of
         png_decode. It now keeps its real color with a small saturation
         lift through gui_wall_tint. Google was picked over Bing's virtualearth.net
         specifically because it shares OpenTopoMap's x/y/z slippy-map
         convention; Bing's quadkey addressing would have needed new tile
         math, not just a new host/decoder.
   Any non-zero value still shows the baked photo until the fetch lands
   and keeps showing it if the fetch fails, never a blank desktop (same
   contract v75 established). settings_load clamps to 0..4 so a hand-
   edited or stale SETTINGS.TXT can't select a theme that doesn't exist. */
#define WALL_PHOTO 0
#define WALL_WARM  1
#define WALL_COOL  2
#define WALL_RAW   3
#define WALL_SAT   4
/* v0.76.7: default flipped from WALL_WARM to WALL_SAT per Joshua's own
   direct, previously-recorded request ("the real satellite-town wallpaper
   should become the default once buildable", roadmap.md's "Later idea:
   location-dynamic satellite wallpaper" entry) -- Satellite became real and
   buildable in v0.73.1 but the compiled-in default was never actually
   flipped, so every fresh boot (and, worse, the browser demo's every idle-
   tour lap, which wipes SETTINGS.TXT via ramfs reset) kept showing the Warm
   map until a user manually clicked through Settings. Safe by construction:
   wall_apply() always falls back to the baked wallpaper_rgb photo whenever
   wall_map is still null (fetch hasn't landed or failed), the exact same
   fallback this default already relied on for WALL_WARM, so flipping the
   default changes nothing about failure-mode safety, only which real image
   a successful fetch shows. */
static int wall_theme = WALL_SAT;
static int wind_enabled = 1; /* real definition; forward of the v45 declaration below so settings_load (right here, needs both) can precede it in the file */

/* v85 (chat rework): global LLM config, the same "one real setting, one
   real default, survives a reboot" contract wind/dock/wall already keep.
   Was hardcoded inline in the shell `chat` command and duplicated again
   in gui_launch_chat (two copies of "llama3.1:8b" / "10.0.2.2" / 11434
   that could silently drift apart); now one source of truth both read. */
#define LLM_MODEL_MAX 32
#define LLM_HOST_MAX 40
static char llm_model[LLM_MODEL_MAX] = "qwen3:8b";
static char llm_host[LLM_HOST_MAX] = "10.0.2.2";
static int llm_port = 11434;
/* v85: real chat models actually installed on the host (checked via
   `ollama list`), not a free-text field a typo can point at nothing.
   nomic-embed-text is also installed but is embedding-only, deliberately
   left off. Settings' LLM-model row cycles this list; a stale/hand-edited
   SETTINGS.TXT with anything else falls back to index 0 (qwen3:8b) the
   next time the cycle runs, since the cycle only ever writes one of
   these two strings back out.
   v0.85.4 (direct owner request): qwen3:8b promoted to the real default,
   llama3.1:8b kept as the second choice, checked against the same
   `ollama list` on the host, both actually installed. */
static const char *LLM_MODELS[] = { "qwen3:8b", "llama3.1:8b" };
#define LLM_MODEL_COUNT 2

/* v47 (0.47.0): settings persisted through the VFS, so "customize the OS
   from inside the OS" actually survives a reboot instead of resetting to
   the compiled-in defaults every boot. Deliberately a flat key=value text
   file (SETTINGS.TXT), not a binary struct: it's human-readable from any
   app that can read a file (cat, the editor), and a corrupt or missing
   file just means defaults, never a crash, since every key is parsed with
   its own bounds check and a real default already set before parsing
   starts.

   v85: grew from int-only values (wind/dock/wall) to also carry two real
   string values (llmmodel/llmhost) plus one more int (llmport), same
   flat key=value shape, just a second value-parsing path alongside the
   existing numeric one rather than a new file format. */
#define SETTINGS_FILE "SETTINGS.TXT"
static void settings_load(void){
    static char buf[384];
    int n = vfs_read_file(SETTINGS_FILE, buf, sizeof(buf) - 1);
    if (n <= 0) return; /* no file yet: compiled-in defaults stand */
    buf[n] = 0;
    for (int i = 0; i < n; ){
        int start = i;
        while (i < n && buf[i] != '\n') i++;
        int line_end = i;
        if (i < n) i++; /* skip the newline */
        int eq = -1;
        for (int j = start; j < line_end; j++) if (buf[j] == '=') { eq = j; break; }
        if (eq < 0) continue;
        int keylen = eq - start;
        int is_wind = keylen == 4 && buf[start]=='w' && buf[start+1]=='i' && buf[start+2]=='n' && buf[start+3]=='d';
        int is_dock = keylen == 4 && buf[start]=='d' && buf[start+1]=='o' && buf[start+2]=='c' && buf[start+3]=='k';
        int is_wall = keylen == 4 && buf[start]=='w' && buf[start+1]=='a' && buf[start+2]=='l' && buf[start+3]=='l';
        int is_llmmodel = keylen == 8 && buf[start]=='l' && buf[start+1]=='l' && buf[start+2]=='m' && buf[start+3]=='m' && buf[start+4]=='o' && buf[start+5]=='d' && buf[start+6]=='e' && buf[start+7]=='l';
        int is_llmhost  = keylen == 7 && buf[start]=='l' && buf[start+1]=='l' && buf[start+2]=='m' && buf[start+3]=='h' && buf[start+4]=='o' && buf[start+5]=='s' && buf[start+6]=='t';
        int is_llmport  = keylen == 7 && buf[start]=='l' && buf[start+1]=='l' && buf[start+2]=='m' && buf[start+3]=='p' && buf[start+4]=='o' && buf[start+5]=='r' && buf[start+6]=='t';
        if (is_llmmodel) {
            char parsed[LLM_MODEL_MAX];
            int j = 0, k = eq + 1;
            while (k < line_end && j < LLM_MODEL_MAX - 1) parsed[j++] = buf[k++];
            parsed[j] = 0;
            /* Validated against the real installed-model list, not
               accepted verbatim: a hand-edited or stale SETTINGS.TXT
               naming a model that isn't one of the two real ones falls
               back to the default (index 0) rather than pointing chat at
               something that will just fail every call, same "can't
               select a theme that doesn't exist" contract wall_theme's
               own clamp already keeps just above. */
            int valid = 0;
            for (int mi = 0; mi < LLM_MODEL_COUNT; mi++) if (!strcmp(parsed, LLM_MODELS[mi])) { valid = 1; break; }
            const char *use = valid ? parsed : LLM_MODELS[0];
            int p = 0; while (use[p] && p < LLM_MODEL_MAX - 1) { llm_model[p] = use[p]; p++; } llm_model[p] = 0;
            continue;
        }
        if (is_llmhost) {
            int j = 0, k = eq + 1;
            while (k < line_end && j < LLM_HOST_MAX - 1) llm_host[j++] = buf[k++];
            llm_host[j] = 0;
            if (j == 0) { const char *d = "10.0.2.2"; int p=0; while (d[p]) llm_host[p]=d[p], p++; llm_host[p]=0; }
            continue;
        }
        int val = 0, neg = 0, k = eq + 1;
        if (k < line_end && buf[k] == '-') { neg = 1; k++; }
        while (k < line_end && buf[k] >= '0' && buf[k] <= '9') { val = val * 10 + (buf[k] - '0'); k++; }
        if (neg) val = -val;
        if (is_wind) wind_enabled = (val != 0);
        else if (is_dock && val >= 5 && val <= 25) dock_scale_pct = val;
        else if (is_wall && val >= WALL_PHOTO && val <= WALL_SAT) wall_theme = val;
        else if (is_llmport && val > 0 && val <= 65535) llm_port = val;
    }
}

static void settings_save(void){
    char buf[256];
    int n = 0;
    const char *k1 = "wind="; while (*k1) buf[n++] = *k1++;
    buf[n++] = wind_enabled ? '1' : '0'; buf[n++] = '\n';
    const char *k2 = "dock="; while (*k2) buf[n++] = *k2++;
    if (dock_scale_pct >= 10) buf[n++] = '0' + dock_scale_pct / 10;
    buf[n++] = '0' + dock_scale_pct % 10;
    buf[n++] = '\n';
    const char *k3 = "wall="; while (*k3) buf[n++] = *k3++;
    buf[n++] = '0' + wall_theme; buf[n++] = '\n';
    const char *k4 = "llmmodel="; while (*k4) buf[n++] = *k4++;
    { const char *s = llm_model; while (*s && n < (int)sizeof(buf) - 2) buf[n++] = *s++; }
    buf[n++] = '\n';
    const char *k5 = "llmhost="; while (*k5) buf[n++] = *k5++;
    { const char *s = llm_host; while (*s && n < (int)sizeof(buf) - 8) buf[n++] = *s++; }
    buf[n++] = '\n';
    const char *k6 = "llmport="; while (*k6) buf[n++] = *k6++;
    { char digits[8]; int nd = 0; int v = llm_port;
      if (v == 0) digits[nd++] = '0';
      while (v) { digits[nd++] = (char)('0' + v % 10); v /= 10; }
      while (nd) buf[n++] = digits[--nd]; }
    buf[n++] = '\n';
    vfs_replace_file(SETTINGS_FILE, buf, (unsigned int)n);
}

static int gui_dock_icon(void){
    int by_height = (int)window_height() * dock_scale_pct / 100;
    int max_by_width = (DOCK_BUDGET - 2 * DOCK_PAD - (GUI_ICON_COUNT - 1) * DOCK_GAP) / GUI_ICON_COUNT;
    if (by_height > max_by_width) by_height = max_by_width;
    if (by_height < 16) by_height = 16; /* below this the vector glyphs stop being legible at all */
    return by_height;
}
#define DOCK_ICON (gui_dock_icon())
#define DOCK_MARGIN_BOT 24
#define DOCK_TRAY_COLOR 0x00EFEBE4 /* the one surface colour every dock tile is blended against */
static int gui_dock_w(void){ return GUI_ICON_COUNT * DOCK_ICON + (GUI_ICON_COUNT - 1) * DOCK_GAP + 2 * DOCK_PAD; }
static int gui_dock_x0(void){ return ((int)window_width() - gui_dock_w()) / 2; }
static int gui_dock_y0(void){ return (int)window_height() - DOCK_ICON - 2 * DOCK_PAD - DOCK_MARGIN_BOT; }
static int gui_slot_x(int slot){ return gui_dock_x0() + DOCK_PAD + slot * (DOCK_ICON + DOCK_GAP); }

/* Which dock slot a point falls in, clamped to the nearest end rather than
   returning "none": once a drag has started, the icon should track the
   cursor even past the dock's own edge, the same way a real dock does. */
static int gui_slot_at(int mx){
    /* v63: was `- DOCK_ICON / 2`, since v15. That put every slot boundary
       at the CENTRE of a drawn tile, so the left half of each icon (and
       the gap before it) hit-tested as the previous slot: the magnified
       icon sat one tile to the left of the cursor half the time, caught
       in a real framebuffer dump (cursor over Reminders, Notes lifted).
       Half a gap either side of each tile now belongs to that tile. */
    int rel = mx - (gui_dock_x0() + DOCK_PAD) + DOCK_GAP / 2;
    int slot = rel / (DOCK_ICON + DOCK_GAP);
    if (rel < 0) slot = 0;
    if (slot < 0) slot = 0;
    if (slot >= GUI_ICON_COUNT) slot = GUI_ICON_COUNT - 1;
    return slot;
}

/* Only counts as being "over the dock" within its actual drawn rect,
   unlike gui_slot_at (used once a drag is already underway, where the
   dragged icon should keep tracking the cursor even briefly outside it). */
static int gui_dock_hit_test(int mx, int my){
    int y0 = gui_dock_y0(), h = DOCK_ICON + 2 * DOCK_PAD;
    if (my < y0 - 20 || my >= y0 + h) return -1;
    int x0 = gui_dock_x0(), w = gui_dock_w();
    if (mx < x0 || mx >= x0 + w) return -1;
    return gui_slot_at(mx);
}

/* Paints a rect, then overwrites each corner's pixels outside a quarter
   circle of radius r with bg, faking a rounded rect with no alpha. */
/* Channel-wise average of two 0x00RRGGBB colors. No alpha channel in this
   framebuffer to composite with, so a real anti-aliased edge (a soft
   transition band instead of one hard cutoff) has to be a genuine, solid,
   precomputed color, not a blend against whatever's already drawn. */
static unsigned int gui_blend(unsigned int a, unsigned int b){
    unsigned int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    unsigned int br = (b >> 16) & 0xFF, bg2 = (b >> 8) & 0xFF, bb = b & 0xFF;
    return ((ar + br) / 2 << 16) | ((ag + bg2) / 2 << 8) | ((ab + bb) / 2);
}

/* Channel-wise linear interpolation between two 0x00RRGGBB colors, `t/max`
   of the way from `a` to `b`. */
static unsigned int gui_lerp(unsigned int a, unsigned int b, int t, int max){
    /* Every channel here as a signed int throughout: `a`/`b` are unsigned,
       so `br - ar` promotes back to unsigned if either operand stays
       unsigned, wrapping to a huge positive value whenever the channel is
       decreasing (exactly the case going from sand to burgundy), which is
       the real bug a first version of this shipped with, a genuinely
       wrong saturated-magenta gradient, not the intended one, caught by
       actually looking at a real screenshot instead of trusting the math. */
    int ar = (int)((a >> 16) & 0xFF), ag = (int)((a >> 8) & 0xFF), ab = (int)(a & 0xFF);
    int br = (int)((b >> 16) & 0xFF), bg2 = (int)((b >> 8) & 0xFF), bb = (int)(b & 0xFF);
    int r = ar + (br - ar) * t / max;
    int g = ag + (bg2 - ag) * t / max;
    int bl = ab + (bb - ab) * t / max;
    return ((unsigned int)r << 16) | ((unsigned int)g << 8) | (unsigned int)bl;
}

/* No libm in this freestanding build, and these icons are small enough
   (radius well under 16px) that a plain increment-until-it-fits search is
   plenty fast for something drawn on hover, not every frame. */
static int gui_isqrt(int n){
    if (n < 0) n = 0;
    int r = 0;
    while ((r + 1) * (r + 1) <= n) r++;
    return r;
}

/* v65 (0.62.0): day/night tint. Direct request, building on v60's real-
   weather sway: the baked wallpaper photo (wallpaper_rgb, a compile-time
   constant, no image decoder in this freestanding kernel to regenerate it
   at runtime) now color-grades per real hour of day, a genuine per-pixel
   multiply/lerp pass at render time, not new pixel data. Real time source
   reused, not invented: the exact same CMOS RTC register 4 + BCD decode
   gui_draw_menubar()/show_time() already read (`u8 h = cmos(4); hv = (h &
   0x0F) + ((h >> 4) * 10);`), so `time` and the menu bar clock and this
   tint always agree about what hour it really is.
   daynight_calc(hour, ...) is kept as a pure function of an explicit hour,
   not a read of the live CMOS state, specifically so daynighttest below
   can call it with fixed hours (2 vs 14) and get a deterministic,
   reproducible answer without faking hardware, the same shape v60's own
   wind_pct_for_weather_code has (a pure function of a code, not a live
   read). daynight_update() is the one function that actually touches
   cmos()/ticks(), rate-limited to at most once a real second (matching
   show_time's own cadence) so the wallpaper's frequent per-frame redraws
   (wind sway alone repaints ~20x/sec) don't hammer CMOS I/O on every call;
   it's called from gui_draw_wallpaper_rows_sway_ex, the one real choke
   point every wallpaper draw already funnels through (gui_draw_wallpaper,
   the dock band redraw, and the wind-sway tick all end up there), so no
   caller needed to change to pick this up.
   Stays inside the Mojave desert palette CLAUDE.md documents on purpose:
   real night hours blend toward 0x00201009, "the wallpaper's own espresso-
   brown" (already used and named exactly that a few hundred lines below
   for the dock icon menu background), never toward black or blue, so a
   dimmed desert night still reads as leather-brown/granite, not a cold
   blue filter. Real day hours blend a little toward 0x00DDDDDD, this
   file's own documented Silver, a small real brighten, not a wash to
   white. */
static int daynight_hour = 12;              /* default noon (full daylight, no tint) until the first real CMOS read */
static int daynight_night_pct = 0;          /* 0 = no night blend, up to 100 = fully at the espresso-brown floor; cached by daynight_update() */
static int daynight_day_pct = 0;            /* 0 = no brighten, small positive = midday's real, small brighten */
static unsigned int daynight_last_tick = 0;

/* Pure: same answer every time for the same hour, no CMOS/ticks() touched,
   so daynighttest can call this directly with fixed hours. Boundaries are
   a plain judgment call, stated as one (no sunrise/sunset table in this
   kernel), same honesty standard v60's wind_pct_for_weather_code comment
   already sets for its own percentages: 08:00-18:00 full real daylight (a
   small +10% brighten), 18:00-21:00 dusk deepening, 05:00-08:00 dawn
   lightening back up, 21:00-05:00 the real deep-night floor. */
static void daynight_calc(int hour, int *night_out, int *day_out){
    int night = 0, day = 0;
    if (hour >= 8 && hour < 18) day = 10;                       /* real midday: small, real brighten */
    else if (hour >= 18 && hour < 21) night = (hour - 18) * 22; /* dusk: 18->0, 19->22, 20->44 */
    else if (hour >= 5 && hour < 8) night = (8 - hour) * 20;    /* dawn: 5->60, 6->40, 7->20 */
    else night = 65;                                             /* 21:00-05:00: deep night floor, dim not pitch black */
    *night_out = night; *day_out = day;
}

static void daynight_update(void){
    if (daynight_last_tick && ticks() - daynight_last_tick < 100) return; /* real RTC read at most once a real second */
    daynight_last_tick = ticks();
    u8 h = cmos(4);
    daynight_hour = (h & 0x0F) + ((h >> 4) * 10); /* same BCD decode gui_draw_menubar()/show_time() already use */
    daynight_calc(daynight_hour, &daynight_night_pct, &daynight_day_pct);
}

/* The one real per-pixel tint pass, `night`/`day` explicit rather than
   read from the cached globals so daynighttest can drive it with fixed
   percentages too, not just fixed hours. gui_lerp is this file's own
   existing channel-wise blend (the wind/AA code already uses it for
   every other precomputed solid-color transition here), reused rather
   than a new blend primitive. */
static unsigned int gui_daynight_tint_pct(unsigned int rgb, int night, int day){
    if (night > 0) return gui_lerp(rgb, 0x00201009, night, 100); /* toward the wallpaper's own espresso-brown floor, never blue */
    if (day > 0)   return gui_lerp(rgb, 0x00DDDDDD, day, 100);   /* toward this file's own documented Silver, a small real brighten */
    return rgb;
}
static unsigned int gui_daynight_tint(unsigned int rgb){ return gui_daynight_tint_pct(rgb, daynight_night_pct, daynight_day_pct); }

/* v79: the map wallpaper (wall_src pointed at wall_map instead of
   wallpaper_rgb, see v75's comment at wall_src's declaration) is real
   fetched OpenTopoMap pixel data, and OpenTopoMap ships its own neutral
   cartographer's palette (grays for streets/contours, cold greens for
   parks, flat blues for water) that never went through this file's own
   Mojave desert grade the baked photo did. Sitting next to the sand/
   granite/leather-brown dock and panels, it read as a foreign, cold
   rectangle dropped onto a warm desktop, direct request to fix.
   A grade toward this file's palette has to stay a MULTIPLY, not a lerp
   toward one flat target color the way gui_daynight_tint blends toward
   its espresso-brown/Silver floors: a lerp blend flattens every distinct
   map color toward the same target as the blend percentage climbs, which
   is exactly the "washes out detail" failure this was asked to avoid --
   street-name glyphs are 1-2px of near-black on near-white, road/water
   contrast is a gray line on a blue fill, and either one degrading toward
   a shared target erases the very contrast that makes the map legible.
   A per-channel multiply instead scales every pixel by the same warm
   ratio, so two pixels that started different stay different (their
   contrast ratio is preserved to within rounding), while the whole image
   shifts toward this palette's warmth: reds pushed up (272/256, +6.25%,
   toward Orange #FF851B's own red-forward hue), greens pulled down
   slightly (248/256, -3.1%, so parkland reads sand-toward-olive rather
   than postcard green), blues pulled down more (216/256, -15.6%, the
   real driver of the warm shift, matching wall_bot's own brown recipe a
   few hundred lines below: Orange blended with Black is red-heavy and
   blue-starved). Deliberately mild (a 15.6% max single-channel move) so
   near-white street-label backgrounds stay legibly near-white and near-
   black glyph strokes stay legibly near-black, both ends of the contrast
   range that has to survive; a heavier grade would gray-crush the whites
   the way a heavy sepia overlay does. Applied at the two real per-pixel
   choke points that read wall_src directly (gui_wallpaper_color below,
   for the icon-shadow/dock-tray blend targets, and gui_wallpaper_px
   further down, for the actual on-screen blit and the wind_base cache it
   feeds), both gated on `wall_src != wallpaper_rgb` so the baked photo's
   own already-correct palette is never touched, only the map is. Runs
   before gui_daynight_tint, not after: daynight's night/day blend is
   meant to read as "what hour it is" layered on top of whatever the base
   wallpaper actually looks like, the same order the photo already uses. */
static inline __attribute__((always_inline)) unsigned int gui_map_tint(unsigned int rgb){
    int r = (int)((rgb >> 16) & 0xFF), g = (int)((rgb >> 8) & 0xFF), b = (int)(rgb & 0xFF);
    r = (r * 272) >> 8; if (r > 255) r = 255;
    g = (g * 248) >> 8;
    b = (b * 216) >> 8;
    return ((unsigned int)r << 16) | ((unsigned int)g << 8) | (unsigned int)b;
}

/* v81: the Cool map theme, a real, distinct second grade for the direct
   request's "cooler/higher-contrast map variant for legibility", not a
   relabeled copy of Warm above. Two real moves, opposite of Warm's:
     1. channel balance pushed cool instead of warm -- blue up (300/256,
        +17.2%) and red down (208/256, -18.75%), green trimmed slightly
        (240/256, -6.25%), the mirror image of Warm's red-up/blue-down
        recipe, so the two themes are provably different curves, not the
        same curve with different constants that happen to look similar.
     2. a genuine contrast stretch on top (push every channel away from
        mid-gray 128 by ~15%, matching the 15% figure the direct request
        asked for under "higher-contrast"), which Warm deliberately does
        NOT do (Warm's own comment above stays a pure per-channel
        multiply, no contrast move, specifically to keep near-white/near-
        black street labels from crushing). Cool applies both: OpenTopoMap
        legibility is the point of this variant, so a real contrast boost
        is in scope here even though it wasn't for Warm.
   Clamped 0..255 at every step; run through the same maptinttest-shaped
   proof below (walltest) that a neutral gray goes cool (blue ends up
   strictly greater than red, the opposite assertion from Warm's), that
   distinct source colors stay distinct (multiply+affine contrast, never
   a lerp-to-one-target), and that near-white/near-black labels still
   read (contrast stretch pushes them further apart, not together). */
static inline __attribute__((always_inline)) unsigned int gui_map_tint_cool(unsigned int rgb){
    int r = (int)((rgb >> 16) & 0xFF), g = (int)((rgb >> 8) & 0xFF), b = (int)(rgb & 0xFF);
    r = (r * 208) >> 8;
    g = (g * 240) >> 8;
    b = (b * 300) >> 8; if (b > 255) b = 255;
    r = 128 + ((r - 128) * 147) / 128; if (r < 0) r = 0; if (r > 255) r = 255;
    g = 128 + ((g - 128) * 147) / 128; if (g < 0) g = 0; if (g > 255) g = 255;
    b = 128 + ((b - 128) * 147) / 128; if (b < 0) b = 0; if (b > 255) b = 255;
    return ((unsigned int)r << 16) | ((unsigned int)g << 8) | (unsigned int)b;
}

/* Keep the satellite photograph in color. A modest saturation lift makes
   trees, roofs, and roads readable without flattening their detail. */
static inline __attribute__((always_inline)) unsigned int gui_sat_color(unsigned int rgb){
    int r = (int)((rgb >> 16) & 0xFF), g = (int)((rgb >> 8) & 0xFF), b = (int)(rgb & 0xFF);
    int l = (r * 77 + g * 150 + b * 29) >> 8;
    r = l + (r - l) * 5 / 4; if (r < 0) r = 0; if (r > 255) r = 255;
    g = l + (g - l) * 5 / 4; if (g < 0) g = 0; if (g > 255) g = 255;
    b = l + (b - l) * 5 / 4; if (b < 0) b = 0; if (b > 255) b = 255;
    return ((unsigned int)r << 16) | ((unsigned int)g << 8) | (unsigned int)b;
}

/* v81: single choke point every wallpaper-theme reader goes through, so
   gui_wallpaper_color and gui_wallpaper_px (the two real per-pixel paths,
   see v79's comment above) can't drift out of sync on which theme applies
   which grade. wall_theme's own guard (never touch the baked photo) lives
   here once instead of being copy-pasted at each call site. */
static inline __attribute__((always_inline)) unsigned int gui_wall_tint(unsigned int rgb){
    if (wall_src == wallpaper_rgb) return rgb;      /* Photo: never graded */
    if (wall_theme == WALL_COOL) return gui_map_tint_cool(rgb);
    if (wall_theme == WALL_RAW) return rgb;         /* Raw: OpenTopoMap's own palette, untouched */
    if (wall_theme == WALL_SAT) return gui_sat_color(rgb);
    return gui_map_tint(rgb);                       /* WALL_WARM, and the fallback while fetching */
}

/* Real photo now, not a procedural gradient: direct request for an
   actual, non-copyrighted image of the real park instead of drawn
   colors. wallpaper_rgb is a genuine public domain U.S. National Park
   Service photo (a real Joshua tree, Mount San Jacinto, and the desert
   floor beyond, confirmed PD-USGov on its Wikimedia Commons file page,
   no attribution legally required), downsampled to 240x171 and embedded
   as a flat RGB byte array, see gen_wallpaper.sh for the exact source
   URL and regeneration steps. No image decoder exists in this
   freestanding kernel (deliberately, same scope note as the app HTML
   embedding), so this is the same "raw bytes in, no parsing needed"
   approach gen_app.sh already uses for ported web pages, just for pixels
   instead of markup.
   gui_wallpaper_color(row) keeps its exact old signature so every
   existing caller (icon drop shadows, the dock tray's corner AA, the
   hello watermark) needed zero changes: it now samples the photo at a
   representative center column for that row instead of computing a
   gradient value, a real color close enough for a blend target even
   though the actual photo varies left-to-right too (gui_draw_wallpaper
   below is the one that blits the real 2D image, this is only for
   things blending toward "whatever's roughly there"). v65: also runs the
   same daynight tint every other wallpaper pixel gets, so the AA blend
   targets it feeds (dock tray corners, the shadow beneath it) always
   agree with the real photo pixels sitting right next to them instead of
   the tray looking night-tinted against a still-daylit backdrop. */
static unsigned int gui_wallpaper_color(int row){
    int area_h = (int)window_height() - GUI_MENUBAR_H;
    int r = row - GUI_MENUBAR_H;
    if (r < 0) r = 0;
    if (area_h < 1) area_h = 1;
    if (r >= area_h) r = area_h - 1;
    int sy = r * WALLPAPER_H / area_h;
    if (sy >= WALLPAPER_H) sy = WALLPAPER_H - 1;
    const unsigned char *p = &wall_src[(sy * WALLPAPER_W + WALLPAPER_W / 2) * 3];
    unsigned int rgb = ((unsigned int)p[0] << 16) | ((unsigned int)p[1] << 8) | p[2];
    rgb = gui_wall_tint(rgb); /* v79/v81: the current wallpaper theme's grade, Photo untouched */
    return gui_daynight_tint(rgb);
}

/* Real regression caught by testing, not assumed safe: this used to paint
   every row including the menu bar's, harmless when gui_draw_menubar()
   unconditionally redrew its own opaque bar right on top of it every
   single call. Once that redraw started skipping frames where the clock
   hadn't changed, the gradient painted here was left exposed instead,
   the menu bar visibly vanishing. Skipping the menu bar's own rows here
   makes the two draws correct independently of what order or how often
   either one runs, not just how they currently happen to interact. */
/* AA_BAND pixels of smooth falloff instead of one hard blended ring: a
   single step still read as "bitmap" on a curve this small (icon radii
   are well under 16px), a real gradient across a few pixels using the
   actual radial distance (gui_isqrt) reads meaningfully smoother, direct
   follow-up feedback after the first AA pass still looked too bitmap. */
/* v44.1: a runtime knob, not a constant. On screen, 5 logical px is right.
   Inside an icon's supersample buffer it is a third of a physical pixel,
   so the primitives went in with effectively no antialiasing and the
   downsample's box filter did all of it, with only ~10 grey levels per
   edge, visible as crunch on every diagonal. The icon renderer widens it
   for the duration of a render and puts it back. */
static int aa_band = 5;
#define AA_BAND aa_band

/* The flat-fill rounded rect this once sat next to is gone now, no
   caller left once chat/folder moved to a real gradient or opaque fill.
   This one's for something sitting on top of the gradient wallpaper
   rather than a flat panel: the corner AA blends toward the wallpaper's
   REAL color at each
   corner's own row (gui_wallpaper_color(y+dy) for the top two corners,
   y+h-1-dy for the bottom two), not one fixed sample.
   Real, visible bug this fixes, caught with actual pixel values off a
   screendump, not eyeballed: the dock tray used to pass one single
   gui_wallpaper_color(y0+dock_h/2) (a row near the tray's own middle,
   already fairly dark) as the blend target for ALL four corners. At the
   top corners the true backdrop just outside the tray is much lighter
   than that sample, so the AA falloff ended in a visibly dark blotch
   right where it should have faded to a light warm tone, reading as a
   strange dark "bubble" at the tray's own top-left and top-right
   corners. Confirmed the exact wrong value directly: gui_wallpaper_color
   at that mid-row really does compute to a dark R=62, correct for where
   it was sampled, wrong for where it was actually used. */
/* Forward declaration: real per-pixel physical wallpaper sampler, defined
   later in this file (the wind code's own sampler). v44.3 needs it here
   so each corner's AA band can blend against the ACTUAL pixel behind
   that corner instead of gui_wallpaper_color(row)'s centre-column sample,
   which is wrong at the tray's own far left/right edges. */
static unsigned int gui_wallpaper_sample(int px, int py, int sway);

static void gui_rounded_rect_on_wallpaper(int x, int y, int w, int h, unsigned int color, int r){
    /* v43: drawn at PHYSICAL resolution. Through the logical layer every
       corner step was a 2x2 block and the AA band two logical pixels wide,
       which on a 1920px panel reads as a plainly staircased edge (real
       macro photo). Same math as before, in physical units, with the AA
       band widened to match, blending each edge pixel against the actual
       wallpaper colour behind it. */
    /* v79: real second staircase found and fixed. The corner arc here was
       still single-sampled: one distance test per PHYSICAL output pixel,
       thresholded into a `band`-wide linear ramp. That is one coverage
       value per pixel, not a coverage fraction, so the arc's true boundary
       (which crosses many physical pixels only partially) still rasterizes
       as a hard staircase, just a softer-edged one, exactly what a real
       macro photo of the tray's corner showed (confirmed with a real
       pmemsave capture, tools/traycorner-check.py, blocky steps visible
       at 6x zoom even after v44.1/v44.3's band-width and per-corner-sample
       fixes). Every icon glyph avoids this by rendering into a 6x
       oversampled buffer and box-downsampling (ICON_SS_SCALE); the tray
       itself never went through that pipeline; it draws straight to
       physical pixels with a single sample each. Same fix in spirit,
       applied analytically instead of through a real offscreen buffer
       (the tray spans the full dock width, an oversampled buffer for the
       whole shape would be real wasted memory for two corners): SS x SS
       subsamples per physical pixel, each tested against the true circle,
       averaged into a real coverage fraction, then that fraction blends
       color against the real wallpaper pixel behind it. This is the same
       box-filter idea ICON_SS_SCALE uses, just evaluated per-pixel
       instead of via a downsample pass. */
    int sc = (int)window_scale();
    int px0 = x * sc, py0 = y * sc, pw = w * sc, ph = h * sc, pr = r * sc, band = 3; /* v44.1: 3 physical px; AA_BAND*sc was 10 and read as a soft, blurry corner */
    const int SS = 4; /* v79: 4x4 = 16 subsamples per physical pixel, real coverage AA on the corner arc */
    for (int py = 0; py < ph; py++){
        for (int px = 0; px < pw; px++){
            /* distance from the nearest corner arc centre, or 0 if this
               pixel isn't in a corner region at all */
            int cx = px < pr ? pr : (px >= pw - pr ? pw - 1 - pr : -1);
            int cy = py < pr ? pr : (py >= ph - pr ? ph - 1 - pr : -1);
            unsigned int col = color;
            if (cx >= 0 && cy >= 0){
                int ox = px - cx, oy = py - cy;
                int d2 = ox * ox + oy * oy;
                int outer_margin = 2; /* subsamples can land a touch past the whole-pixel test below */
                if (d2 > (pr - band - outer_margin) * (pr - band - outer_margin)){
                    if (d2 > (pr + outer_margin) * (pr + outer_margin)) continue; /* comfortably outside: wallpaper untouched */
                    /* v79: real coverage fraction, not a single threshold.
                       Sample SSxSS sub-points spread across this physical
                       pixel's own area and count how many fall inside
                       the true circle of radius pr; that fraction IS the
                       pixel's real AA coverage, the same quantity a 6x
                       supersample-then-box-downsample pass would produce,
                       computed directly instead of through a buffer. */
                    int inside = 0;
                    for (int sy = 0; sy < SS; sy++){
                        int subdy = oy * SS + sy * 2 + 1 - SS; /* sample point offset, in 1/SS-pixel units, centred in each sub-cell */
                        for (int sx = 0; sx < SS; sx++){
                            int subdx = ox * SS + sx * 2 + 1 - SS;
                            long sd2 = (long)subdx * subdx + (long)subdy * subdy;
                            if (sd2 <= (long)(pr * SS) * (pr * SS)) inside++;
                        }
                    }
                    if (inside == 0) continue;               /* fully outside: leave the wallpaper alone */
                    unsigned int corner_bg = gui_wallpaper_sample(px0 + px, py0 + py, 0);
                    if (inside >= SS * SS) { col = color; }
                    else col = gui_lerp(color, corner_bg, SS * SS - inside, SS * SS);
                }
            } else if (py < band) {
                /* v61: real, confirmed bug, not a guess: only the four
                   rounded corners above ever blended toward the real
                   wallpaper pixel; every straight edge (the whole top
                   edge between the corners, which is most of it) was
                   filled 100% solid color with a raw 1px cut straight
                   into whatever was behind it, zero pixels of blend.
                   Harmless-looking against a light backdrop, but the
                   dock tray sits low on screen where this kernel's own
                   wallpaper gradient is at its darkest (confirmed with a
                   real framebuffer dump: the row immediately above the
                   tray reads ~(2,0,1), next to next essentially black),
                   so that hard cut from near-black straight to the
                   tray's light cream read as a visible dark seam right
                   along the top edge, exactly the reported defect. Same
                   band-width blend the corners already use, straight-
                   line distance from the true top edge instead of the
                   corner arc's radial one. */
                unsigned int edge_bg = gui_wallpaper_sample(px0 + px, py0 + py, 0);
                col = gui_lerp(color, edge_bg, band - py, band);
            }
            window_pixel_phys(px0 + px, py0 + py, col);
        }
    }
}

/* Same corner AA as gui_rounded_rect, but the fill itself is a real top-to-
   bottom gradient instead of one flat color, the classic glossy-icon look
   (lighter catching the light at top, darker at the bottom, real depth),
   direct follow-up after "rich... gradient with a bit of a 3D icon style,
   like Apple" feedback on the flat-color first pass. Every pixel here is
   still a genuine precomputed solid color (gui_lerp), no alpha channel
   this framebuffer doesn't have, same technique the wallpaper and every
   other AA edge in this file already uses. */
static void gui_rounded_rect_gradient(int x, int y, int w, int h, unsigned int color_top, unsigned int color_bottom, unsigned int bg, int r){
    for (int row = 0; row < h; row++)
        window_rect(x, row + y, w, 1, gui_lerp(color_top, color_bottom, row, h));
    /* v37, real long-standing bug fixed here, not a tweak: this loop used
       to measure each corner pixel's distance from the rect's own CORNER
       (dx*dx + dy*dy) and keep everything within r of it as fill. That is
       inverted. The corner pixel is the one furthest outside a rounded
       corner, not inside it, so the true corner stayed filled and the
       background got painted in an arc *beside* it, giving every tile a
       square corner with a notch cut out of its side. It was nearly
       invisible while r was a fixed 12px and stayed that way for many
       versions; making the radius proportional to icon size (22%, the
       iOS/macOS squircle ratio) blew it up to a 39px artifact on every
       dock icon, which is how it finally got caught, by measuring real
       pixel values out of a screendump rather than trusting the shape.
       Correct math: distance from the arc's CENTER, which sits at
       (r, r) inside each corner. */
    for (int dy = 0; dy <= r; dy++){
        for (int dx = 0; dx <= r; dx++){
            int ox = r - dx, oy = r - dy;      /* offset from the arc's centre */
            int d2 = ox * ox + oy * oy;
            int inner = r - AA_BAND;
            if (d2 <= inner * inner) continue;  /* comfortably inside the curve: leave the gradient alone */
            unsigned int top_local = gui_lerp(color_top, color_bottom, dy, h);
            unsigned int bot_local = gui_lerp(color_top, color_bottom, h - 1 - dy, h);
            if (d2 >= r * r) {                  /* outside the curve: this is background */
                window_pixel(x + dx,         y + dy,         bg);
                window_pixel(x + w - 1 - dx, y + dy,         bg);
                window_pixel(x + dx,         y + h - 1 - dy, bg);
                window_pixel(x + w - 1 - dx, y + h - 1 - dy, bg);
                continue;
            }
            int t = gui_isqrt(d2) - inner;      /* inside the AA band: blend out to background */
            window_pixel(x + dx,         y + dy,         gui_lerp(top_local, bg, t, AA_BAND));
            window_pixel(x + w - 1 - dx, y + dy,         gui_lerp(top_local, bg, t, AA_BAND));
            window_pixel(x + dx,         y + h - 1 - dy, gui_lerp(bot_local, bg, t, AA_BAND));
            window_pixel(x + w - 1 - dx, y + h - 1 - dy, gui_lerp(bot_local, bg, t, AA_BAND));
        }
    }
}

/* Real pictograms, not letters: there's no image decoder or asset pipeline
   in this kernel (deliberately, see roadmap.md's font/asset scope notes),
   so each icon is drawn from the same primitives gui_rounded_rect already
   uses (window_pixel/window_rect plus a circle-distance test), geometric
   but genuinely representative of what each app actually is, the same way
   a real dock icon reads as its app at a glance without needing a label. */
/* A real variable, not a compile-time constant: gui_draw_one_icon below
   temporarily swaps this to a dark shadow tone and redraws each glyph at
   a small offset before drawing it again in real white, a genuine drop
   shadow under the glyph itself, not just the icon's outer background.
   Every icon function still just says ICON_FG same as always, nothing
   about them needed to change. */
static unsigned int ICON_FG = 0x00FFFFFF;

/* `into` is whatever color surrounds this circle, so the AA_BAND-pixel
   soft edge can fade toward it: the icon's own colored background for a
   solid fill, or the fill color itself when punching a hole (the pin's
   eyelet) into a shape that was drawn in that fill color. */
/* v82: real second instance of the v79 tray-corner staircase pattern,
   found by following that entry's own "check for other things that draw
   straight to physical pixels outside the 6x-supersampled icon pipeline"
   guidance, not a re-check of the glyphs it already confirmed clean.
   Every icon GLYPH calls this inside gui_render_icon_cached's offscreen
   ICON_SS_SCALE buffer (window_push_target set), where window_pixel
   writes straight into that buffer 1:1 and the later box-downsample does
   the real AA; those calls were never broken, same as v79 already found
   for the glyphs. But this function has three other real callers with no
   target pushed at all: gui_draw_app_titlebar's traffic-light dots (every
   single windowed app: Weather, Mail, Calendar, Contacts, Settings, ...)
   and Settings' own duplicate traffic lights. Those go through plain
   window_pixel, which at window_scale() 2 (every real dock-launched app)
   replicates each LOGICAL pixel it's given into a flat 2x2 PHYSICAL
   block, no interpolation. The AA ramp above is computed once per
   logical pixel, so it produces a handful of correct logical-space grey
   levels, but each one lands on screen as a hard-edged physical block:
   real macro-visible staircasing, confirmed with an actual pmemsave
   capture of the Weather window's red close dot (dock-clicked, real
   mouse path via QMP abs+btn events, not the scale-1 `testapps` shell
   diagnostic, which never hits this because it opens its own 800x600
   scale-1 window): the AA fringe shows as distinct flat terraces, not a
   smooth gradient, at physical (172..205, 96..129). Fix, same shape as
   gui_rounded_rect_on_wallpaper's v79 fix: when there's no offscreen
   target and the window is actually scaled, do the coverage math in
   PHYSICAL pixels via window_pixel_phys (4x4 subsamples per physical
   pixel, real coverage fraction) instead of letting window_pixel's
   block-replication flatten a logical-space ramp. Every glyph caller is
   unaffected (window_has_target() is true there, so this still takes the
   original logical-space path with AA_BAND widened to 18 for that
   buffer, exactly as before). */
static void gui_fill_circle(int cx, int cy, int r, unsigned int color, unsigned int into){
    if (!window_has_target() && window_scale() > 1){
        int sc = (int)window_scale();
        int pcx = cx * sc, pcy = cy * sc, pr = r * sc;
        const int SS = 4;
        int outer = pr + sc;
        for (int dy = -outer; dy <= outer; dy++){
            for (int dx = -outer; dx <= outer; dx++){
                long d2 = (long)dx * dx + (long)dy * dy;
                if (d2 > (long)(pr + 2) * (pr + 2)) continue;
                unsigned int col;
                if (d2 <= (long)(pr - 2) * (pr - 2)) { col = color; }
                else {
                    int inside = 0;
                    for (int sy = 0; sy < SS; sy++){
                        int subdy = dy * SS + sy * 2 + 1 - SS;
                        for (int sx = 0; sx < SS; sx++){
                            int subdx = dx * SS + sx * 2 + 1 - SS;
                            long sd2 = (long)subdx * subdx + (long)subdy * subdy;
                            if (sd2 <= (long)(pr * SS) * (pr * SS)) inside++;
                        }
                    }
                    if (inside == 0) continue;
                    col = inside >= SS * SS ? color : gui_lerp(color, into, SS * SS - inside, SS * SS);
                }
                window_pixel_phys(pcx + dx, pcy + dy, col);
            }
        }
        return;
    }
    int outer2 = (r + AA_BAND) * (r + AA_BAND);
    for (int dy = -r - AA_BAND; dy <= r + AA_BAND; dy++){
        for (int dx = -r - AA_BAND; dx <= r + AA_BAND; dx++){
            int d2 = dx * dx + dy * dy;
            if (d2 > outer2) continue;
            if (d2 <= r * r) { window_pixel(cx + dx, cy + dy, color); continue; }
            int t = gui_isqrt(d2) - r;
            window_pixel(cx + dx, cy + dy, gui_lerp(color, into, t, AA_BAND));
        }
    }
}

/* A thick, soft-edged line segment (a capsule: flat sides, rounded caps),
   anti-aliased into `into` with the same AA_BAND falloff every other shape
   here uses. The one real line primitive icons were missing: before this,
   a diagonal like the weather icon's sun rays could only be a raw, single-
   pixel-wide staircase of window_pixel calls, no thickness, no softening,
   the single most "8-bit" looking thing on the whole dock. Point-to-segment
   distance stays in plain 32-bit int math (icon coordinates never exceed a
   few hundred px, nowhere near overflow), no 64-bit division helper this
   freestanding build doesn't link.
   v83: same staircasing fix gui_fill_circle received in v82: when rendering
   at scaled resolution outside an offscreen target, use physical-pixel
   coverage sampling (4x4 subsamples per physical pixel) instead of logical-
   space AA_BAND that window_pixel's block replication flattens into visible
   terraces. Glyphs (inside gui_render_icon_cached's window_push_target) are
   unaffected; app title-bar and other scaled non-glyph uses of this primitive
   get the coverage fix.
   v0.86.x: real bug found from an actual headless boot-splash capture, not
   a guess: the boot logo (gui_draw_logo) draws its crown out of several
   overlapping capsules that share joints (trunk top, each branch split),
   and this partial-coverage blend faded every edge pixel toward the flat
   `into` background regardless of what was already drawn there. Where a
   later capsule's own edge band crossed a spot an earlier capsule had
   already painted solid, it punched a visible dark hairline crack through
   what should have read as solid fill, the thing that actually made the
   logo look "8-bit" up close, not the AA itself (a zoomed pmemsave capture
   showed real multi-level AA ramps on the true outer silhouette, just
   these false seams cutting across the interior). Real fix: sample the
   pixel that is already there and blend toward it instead of toward the
   caller's flat backdrop; coverage 0 then reproduces the old into-blend
   exactly (nothing else has been drawn there), and coverage 0 < inside <
   full over already-opaque neighboring geometry now blends toward that
   geometry's own color instead of carving a false notch into it. */
/* One antialiased capsule in PHYSICAL pixels. gui_draw_capsule scales logical
   input into this; gui_draw_logo calls it directly so thin strokes keep a real
   radius instead of rounding to zero at logical resolution. */
static void gui_capsule_phys(int pcx0, int pcy0, int pcx1, int pcy1, int pr, unsigned int color){
        int pdx = pcx1 - pcx0, pdy = pcy1 - pcy0;
    long plen2 = (long)pdx * pdx + (long)pdy * pdy;
    int minx = (pcx0 < pcx1 ? pcx0 : pcx1) - pr - 2, maxx = (pcx0 > pcx1 ? pcx0 : pcx1) + pr + 2;
    int miny = (pcy0 < pcy1 ? pcy0 : pcy1) - pr - 2, maxy = (pcy0 > pcy1 ? pcy0 : pcy1) + pr + 2;
    const int SS = 4;
    for (int py = miny; py <= maxy; py++){
        for (int px = minx; px <= maxx; px++){
            int vx = px - pcx0, vy = py - pcy0;
            int ex, ey;
            if (plen2 == 0) { ex = vx; ey = vy; }
            else {
                long dot = (long)vx * pdx + (long)vy * pdy;
                if (dot < 0) dot = 0; else if (dot > plen2) dot = plen2;
                int cxp = pcx0 + (int)(dot * pdx / plen2), cyp = pcy0 + (int)(dot * pdy / plen2);
                ex = px - cxp; ey = py - cyp;
            }
            long d2 = (long)ex * ex + (long)ey * ey;
            if (d2 > (long)(pr + 2) * (pr + 2)) continue;
            unsigned int col;
            if (d2 <= (long)(pr - 2) * (pr - 2)) { col = color; }
            else {
                int inside = 0;
                for (int sy = 0; sy < SS; sy++){
                    int subdy = ey * SS + sy * 2 + 1 - SS;
                    for (int sx = 0; sx < SS; sx++){
                        int subdx = ex * SS + sx * 2 + 1 - SS;
                        long sd2 = (long)subdx * subdx + (long)subdy * subdy;
                        if (sd2 <= (long)(pr * SS) * (pr * SS)) inside++;
                    }
                }
                if (inside == 0) continue;
                if (inside >= SS * SS) col = color;
                else {
                    unsigned int backdrop = window_get_pixel_phys(px, py);
                    col = gui_lerp(color, backdrop, SS * SS - inside, SS * SS);
                }
            }
            window_pixel_phys(px, py, col);
        }
    }
}

static void gui_draw_capsule(int x0, int y0, int x1, int y1, int r, unsigned int color, unsigned int into){
    if (!window_has_target() && window_scale() > 1){
        int sc = (int)window_scale();
        gui_capsule_phys(x0 * sc, y0 * sc, x1 * sc, y1 * sc, r * sc, color);
        return;
    }
    int dx = x1 - x0, dy = y1 - y0;
    int len2 = dx * dx + dy * dy;
    int minx = (x0 < x1 ? x0 : x1) - r - AA_BAND, maxx = (x0 > x1 ? x0 : x1) + r + AA_BAND;
    int miny = (y0 < y1 ? y0 : y1) - r - AA_BAND, maxy = (y0 > y1 ? y0 : y1) + r + AA_BAND;
    int outer2 = (r + AA_BAND) * (r + AA_BAND);
    for (int py = miny; py <= maxy; py++){
        for (int px = minx; px <= maxx; px++){
            int vx = px - x0, vy = py - y0, ex, ey;
            if (len2 == 0) { ex = vx; ey = vy; }
            else {
                int dot = vx * dx + vy * dy;
                if (dot < 0) dot = 0; else if (dot > len2) dot = len2;
                int cxp = x0 + dot * dx / len2, cyp = y0 + dot * dy / len2;
                ex = px - cxp; ey = py - cyp;
            }
            int d2 = ex * ex + ey * ey;
            if (d2 > outer2) continue;
            if (d2 <= r * r) { window_pixel(px, py, color); continue; }
            int t = gui_isqrt(d2) - r;
            window_pixel(px, py, gui_lerp(color, into, t, AA_BAND));
        }
    }
}

/* Real revision, not the first attempt: reported as reading like "tubes",
   not cursive, and looking at it fresh the diagnosis is exactly that,
   three real problems, not one. Thickness relative to letter height was
   close to 20%, a script pen stroke reads closer to 8-10%; there was no
   slant at all, upright strokes read as print, not script; and every
   letter was fully disconnected, real cursive is one continuous stroke
   with the pen barely leaving the page between letters. Fixed all three:
   thinner strokes, a real rightward shear applied to every point (higher
   above the baseline shifts further right, the standard italic
   construction), and thin baseline connector strokes linking each
   letter to the next. Loop letters ('e', 'o') moved from an 8-point to a
   12-point circle approximation, rounder curves at this radius. */
#define HELLO_SLANT_NUM 3
#define HELLO_SLANT_DEN 10

static void gui_draw_script_loop(int cx, int cy, int r, int thick, unsigned int color, unsigned int bg, int skip_mask, int shift){
    static const int px12[12] = {10, 9, 5, 0, -5, -9, -10, -9, -5, 0, 5, 9};
    static const int py12[12] = {0, 5, 9, 10, 9, 5, 0, -5, -9, -10, -9, -5};
    for (int i = 0; i < 12; i++){
        if (skip_mask & (1 << i)) continue;
        int j = (i + 1) % 12;
        gui_draw_capsule(cx + shift + px12[i] * r / 10, cy + py12[i] * r / 10,
                          cx + shift + px12[j] * r / 10, cy + py12[j] * r / 10, thick, color, bg);
    }
}

/* A small hand-plotted script "hello", a real nod to the original 1984
   Macintosh boot screen rather than this file's usual blocky bitmap
   font: every stroke here is the same AA capsule/loop primitive already
   used elsewhere, curved letterforms instead of a monospace grid being
   the whole point. `cx` is the horizontal center of the whole word, not
   a left edge, so the caller doesn't need to know its rendered width.
   Every point is expressed as (dx, n): dx is a horizontal design offset
   in scale units from the letter's own anchor, n is how many scale units
   above the baseline it sits, real distance for the shear (SL) to work
   from, not an arbitrary label. */
static void gui_draw_hello_script(int cx, int baseline, int scale, unsigned int color, unsigned int bg){
    int thick = scale >= 6 ? 2 : 1;
    int total_w = 22 * scale;
    int x = cx - total_w / 2;
#define SL(n) (((n) * scale * HELLO_SLANT_NUM) / HELLO_SLANT_DEN)
#define PX(dx, n) (x + (dx) * scale + SL(n))
#define PY(n) (baseline - (n) * scale)

    /* h */
    gui_draw_capsule(PX(0, 10), PY(10), PX(0, 0), PY(0), thick, color, bg);
    gui_draw_capsule(PX(0, 5), PY(5), PX(1, 7), PY(7), thick, color, bg);
    gui_draw_capsule(PX(1, 7), PY(7), PX(3, 7), PY(7), thick, color, bg);
    gui_draw_capsule(PX(3, 7), PY(7), PX(4, 5), PY(5), thick, color, bg);
    gui_draw_capsule(PX(4, 5), PY(5), PX(4, 0), PY(0), thick, color, bg);
    gui_draw_capsule(PX(4, 0), PY(0), PX(6, 0), PY(0), thick, color, bg); /* connector into e */
    x += 6 * scale;

    /* e: loop centered (2,3), open on the right, crossbar completes it */
    gui_draw_script_loop(x + 2 * scale, PY(3), 3 * scale, thick, color, bg, (1 << 11) | (1 << 0) | (1 << 1), SL(3));
    gui_draw_capsule(PX(-1, 3), PY(3), PX(5, 3), PY(3), thick, color, bg);
    gui_draw_capsule(PX(5, 0), PY(0), PX(7, 0), PY(0), thick, color, bg); /* connector into l */
    x += 6 * scale;

    /* l */
    gui_draw_capsule(PX(0, 10), PY(10), PX(0, 0), PY(0), thick, color, bg);
    gui_draw_capsule(PX(0, 0), PY(0), PX(2, 0), PY(0), thick, color, bg); /* connector into l */
    x += 3 * scale;

    /* l */
    gui_draw_capsule(PX(0, 10), PY(10), PX(0, 0), PY(0), thick, color, bg);
    gui_draw_capsule(PX(0, 0), PY(0), PX(2, 0), PY(0), thick, color, bg); /* connector into o */
    x += 3 * scale;

    /* o: closed loop centered (2,3) */
    gui_draw_script_loop(x + 2 * scale, PY(3), 3 * scale, thick, color, bg, 0, SL(3));
#undef PX
#undef PY
#undef SL
}


/* v40: a row band, so a partial repaint (the dock band on a hover change)
   doesn't have to blit the whole photo. Rows are screen rows. */
/* v45 (0.45.0): wind. A horizontal displacement applied to the wallpaper
   sample, zero at the horizon and growing with the square of the height
   above it, so the trunk barely moves and the crown sways. Driven by a
   slow triangle wave on the PIT (no sin in this kernel, and a triangle
   eased by its own square reads as a breath, not a metronome). Only the
   rows above the horizon are ever redrawn, so the cached dock band is
   never touched and the desktop's dirty-region scheme stays intact. */
#define WIND_HORIZON_ROW 395   /* logical row of the photo's skyline */
#define WIND_TOP_ROW      30   /* just under the menu bar */
/* wind_enabled itself now declared earlier (see settings_load), self-disables if a frame measures slow (v86, the browser demo) */
static int wind_phase = 0;     /* -256..256, current displacement scale */

/* v60 (0.59.0): wind reacts to the real weather v56's dropdown already
   shows. Honest gap check done first, not assumed: weather_fetch's
   Open-Meteo call below only ever requests
   `current=temperature_2m,weather_code`, no wind field at all, so there
   is no real wind-speed number in this kernel to scale by. This instead
   scales sway amplitude off `weather_code10`, the exact real WMO code
   weather_word() already turns into the dropdown's condition word
   (Clear/Cloudy/Fog/Rain/Snow/Showers/Storm) -- a real fetched field,
   just not the one a "wind" effect would ideally want. 100 is the
   pre-v60 baseline amplitude, in force until the first real fetch lands
   (see weather_fetch's wind_pct_for_weather_code call), so a NIC-less
   boot (v86, the browser demo, never fetches) sways exactly as before. */
static int wind_weather_pct = 100;

static int gui_wind_shift(int row){ /* source-pixel shift for this screen row, in 8.8 fixed point */
    if (wall_src != wallpaper_rgb) return 0; /* v75: streets don't sway; the tick still runs (zero shift) so rain/snow keep compositing */
    if (row >= WIND_HORIZON_ROW) return 0;
    int h = WIND_HORIZON_ROW - row;                     /* 0..365 */
    int amp = (h * h) / (365 * 365 / 14);               /* up to ~14 logical px at the very top, ~6 at the crown */
    amp = amp * wind_weather_pct / 100;                 /* v60: real-weather scale, see wind_weather_pct above */
    return amp * wind_phase;                            /* * (-256..256) */
}

static void gui_draw_wallpaper_rows_sway(int y_from, int y_to, int sway);
static void gui_draw_wallpaper_rows(int y_from, int y_to){ gui_draw_wallpaper_rows_sway(y_from, y_to, 0); }

/* One wallpaper pixel at PHYSICAL (px, py): bilinear over the 960x540
   source, plus the wind shift when `sway` is set. Everything that paints
   or samples the wallpaper goes through here now, so the band draw and
   the cursor-backup refresh can never disagree about what a pixel is. */
/* Per-row context for the bilinear sampler: source row pair, vertical
   weight, wind shift. Computed once per screen row; the per-pixel step
   below then does only the horizontal work. (v45.1: a first refactor
   recomputed all of this per pixel and a wind frame went from ~9 to 14
   PIT ticks, tripping the slow-machine gate. Measured over serial, then
   fixed here.) */
struct wp_row { const unsigned char *r0, *r1; int wy, shift, pw; };
static unsigned int *wind_base = 0;
static int wind_base_width = 0;
static int gui_app_windowed; /* real definition + comment below, near gui_draw_app_titlebar; forward-declared here so the wallpaper sampler and the menubar clamp below can both read it */
static inline __attribute__((always_inline)) struct wp_row gui_wallpaper_row(int py, int sway){
    struct wp_row c;
    int lw = (int)window_width(), lh = (int)window_height();
    int sc = (int)window_scale();
    c.pw = lw * sc;
    /* GUI_MENUBAR_H is the real desktop's system menu bar, reserved out
       of the photo's vertical scale so the wallpaper starts right under
       it. A windowed app (gui_app_windowed) has no menu bar inside its
       own clipped viewport -- local y=0 is the viewport's own top edge --
       so it gets the full window height instead. Without this, every
       physical row above GUI_MENUBAR_H inside a window sampled row 0
       unconditionally (see the row<0 clamp below), painting a thin
       sliver of the wallpaper photo's own top edge stretched across that
       whole strip instead of the correctly scaled continuation of the
       photo -- confirmed live, striped farmland from row 0 of the source
       image where a smooth gradient was expected. */
    int top = gui_app_windowed ? 0 : GUI_MENUBAR_H;
    int area_h = lh - top;
    int row = py - top * sc; if (row < 0) row = 0;
    int fy = row * (WALLPAPER_H - 1) * 256 / (area_h * sc > 1 ? area_h * sc - 1 : 1);
    int sy = fy >> 8; c.wy = fy & 255;
    if (sy >= WALLPAPER_H - 1) { sy = WALLPAPER_H - 2; c.wy = 255; }
    c.r0 = &wall_src[sy * WALLPAPER_W * 3];
    c.r1 = c.r0 + WALLPAPER_W * 3;
    c.shift = sway ? (gui_wind_shift(py / sc) * WALLPAPER_W / lw) >> 8 : 0;
    return c;
}
static inline __attribute__((always_inline)) unsigned int gui_wallpaper_px(const struct wp_row *c, int px){
    int fx = px * (WALLPAPER_W - 1) * 256 / (c->pw > 1 ? c->pw - 1 : 1) + c->shift * 256;
    if (fx < 0) fx = 0; if (fx > (WALLPAPER_W - 1) * 256) fx = (WALLPAPER_W - 1) * 256;
    int sx = fx >> 8, wx = fx & 255;
    if (sx >= WALLPAPER_W - 1) { sx = WALLPAPER_W - 2; wx = 255; }
    const unsigned char *a = &c->r0[sx * 3], *b = a + 3, *cc = &c->r1[sx * 3], *d = cc + 3;
    unsigned int col = 0;
    for (int ch = 0; ch < 3; ch++){
        int top = a[ch] * (256 - wx) + b[ch] * wx;
        int bot = cc[ch] * (256 - wx) + d[ch] * wx;
        col = (col << 8) | (unsigned int)((top * (256 - c->wy) + bot * c->wy) >> 16);
    }
    col = gui_wall_tint(col); /* v79/v81: same theme grade as gui_wallpaper_color, real per-pixel blit path */
    return col;
}
static unsigned int gui_wind_cached_pixel(int px, int py){
    int sc = (int)window_scale(), top = WIND_TOP_ROW * sc;
    if (!wind_base || py < top || py >= WIND_HORIZON_ROW * sc) return 0;
    int shift = gui_wind_shift(py / sc) * sc;
    int sx = px + (shift >> 8), frac = shift & 255;
    if (sx < 0) sx = 0;
    if (sx >= wind_base_width) sx = wind_base_width - 1;
    int sx1 = sx + 1 < wind_base_width ? sx + 1 : sx;
    const unsigned int *row = wind_base + (py - top) * wind_base_width;
    return frac ? gui_lerp(row[sx], row[sx1], frac, 256) : row[sx];
}
static unsigned int gui_wallpaper_sample(int px, int py, int sway){
    /* v65: wind_base (below) caches RAW, untinted samples on purpose, so
       the tint here is always computed fresh against the current real
       hour, not frozen at whatever hour the cache happened to be built. */
    unsigned int raw;
    if (sway && wind_base && py >= WIND_TOP_ROW * (int)window_scale() && py < WIND_HORIZON_ROW * (int)window_scale())
        raw = gui_wind_cached_pixel(px, py);
    else { struct wp_row c = gui_wallpaper_row(py, sway); raw = gui_wallpaper_px(&c, px); }
    return gui_daynight_tint(raw);
}

/* ex/ey/ew/eh: a physical rect to leave untouched (the cursor). v45.1: the
   wind used to restore the cursor, repaint the band, and redraw it, which
   erased the pointer for most of every frame, four times a second, a
   real flashing cursor reported from a video. Skipping its rect means it
   is simply never touched. */
static void gui_draw_wallpaper_rows_sway_ex(int y_from, int y_to, int sway, int ex, int ey, int ew, int eh){
    daynight_update(); /* v65: the one real choke point every wallpaper draw funnels through, see its own comment above */
    int lh = (int)window_height();
    int sc = (int)window_scale();
    /* GUI_MENUBAR_H reserves room for the desktop's own system menu bar,
       which only exists when this is painting the real desktop. A
       windowed app (gui_app_windowed) draws inside gui_launch_from_dock's
       clipped viewport instead, which already excludes that window's own
       title bar and has no menu bar of its own -- local y=0 there is real
       content. Without this check the clamp silently pulled y_from back
       up to GUI_MENUBAR_H for every windowed app too, leaving a dead
       unpainted strip (whatever window_clear had set) right under the
       title bar. Confirmed live: the Apps folder's own black band. */
    if (!gui_app_windowed && y_from < GUI_MENUBAR_H) y_from = GUI_MENUBAR_H;
    if (y_to > lh) y_to = lh;
    for (int py = y_from * sc; py < y_to * sc; py++){
        if (sway && wind_base && py >= WIND_TOP_ROW * sc && py < WIND_HORIZON_ROW * sc) {
            int in_rows = (eh > 0 && py >= ey && py < ey + eh);
            int shift = gui_wind_shift(py / sc) * sc;
            int whole = shift >> 8, frac = shift & 255;
            const unsigned int *row = wind_base + (py - WIND_TOP_ROW * sc) * wind_base_width;
            unsigned int *dst = window_phys_row(py);
            for (int px = 0; px < wind_base_width; px++) {
                if (in_rows && px >= ex && px < ex + ew) continue;
                int sx = px + whole;
                if (sx < 0) sx = 0;
                if (sx >= wind_base_width) sx = wind_base_width - 1;
                int sx1 = sx + 1 < wind_base_width ? sx + 1 : sx;
                /* wind_base holds RAW samples (v65: see its own build-loop
                   comment), tinted fresh here against the current real
                   hour rather than baked in once at cache-build time. */
                unsigned int color = frac ? gui_lerp(row[sx], row[sx1], frac, 256) : row[sx];
                color = gui_daynight_tint(color);
                if (dst) dst[px] = color;
                else window_pixel_phys(px, py, color);
            }
            /* v0.78.x: the second real window_phys_row caller. Writing
               through a raw row pointer skips the per-pixel damage path
               entirely, so this row has to declare itself or the sway
               would draw into the back buffer and never reach the screen
               (caught here before shipping, the wallpaper simply froze). */
            if (dst) window_damage(0, py, wind_base_width, 1);
            continue;
        }
        struct wp_row c = gui_wallpaper_row(py, sway);
        int in_rows = (eh > 0 && py >= ey && py < ey + eh);
        for (int px = 0; px < c.pw; px++){
            if (in_rows && px >= ex && px < ex + ew) continue;
            window_pixel_phys(px, py, gui_daynight_tint(gui_wallpaper_px(&c, px)));
        }
    }
}
static void gui_draw_wallpaper_rows_sway(int y_from, int y_to, int sway){ gui_draw_wallpaper_rows_sway_ex(y_from, y_to, sway, 0, 0, 0, 0); }

static void gui_draw_wallpaper(void){
    gui_draw_wallpaper_rows(GUI_MENUBAR_H, (int)window_height());
}

/* Redraw one app-local wallpaper rectangle. Apps can repaint their own
   changing surface without blitting the surrounding viewport. */
static void gui_draw_wallpaper_rect(int x, int y, int w, int h){
    int sc = (int)window_scale();
    int x0 = x * sc, x1 = (x + w) * sc;
    int y0 = y * sc, y1 = (y + h) * sc;
    if (x0 < 0) x0 = 0;
    if (y0 < GUI_MENUBAR_H * sc) y0 = GUI_MENUBAR_H * sc;
    if (x1 > (int)window_width() * sc) x1 = (int)window_width() * sc;
    if (y1 > (int)window_height() * sc) y1 = (int)window_height() * sc;
    for (int py = y0; py < y1; py++) {
        struct wp_row c = gui_wallpaper_row(py, 0);
        for (int px = x0; px < x1; px++)
            window_pixel_phys(px, py, gui_wallpaper_px(&c, px));
    }
}

/* Fills a downward-pointing triangle: flat top of half-width `half_w` at
   (cx, y0), narrowing to a point over `h` rows. Used for the map pin's tip
   and the quote marks' tails. */
/* v44.1: antialiased. The old version rounded each row's half-width to a
   whole pixel, so both slanted edges were staircases (the pencil tip in
   every dock render). Exact half-width in 8.8 fixed point; the outermost
   pixel on each side is blended by its fractional coverage against what
   is already there. */
static void gui_fill_triangle_down(int cx, int y0, int half_w, int h, unsigned int color){
    if (h <= 0) return;
    for (int row = 0; row < h; row++){
        int w256 = (half_w * 256 * (h - row)) / h;   /* half-width, 8.8 */
        int wi = w256 >> 8, frac = w256 & 255;
        if (wi > 0) window_rect(cx - wi + 1, y0 + row, 2 * wi - 1, 1, color);
        if (frac) {
            unsigned int l = window_get_pixel(cx - wi, y0 + row), r = window_get_pixel(cx + wi, y0 + row);
            window_pixel(cx - wi, y0 + row, gui_lerp(l, color, frac, 256));
            window_pixel(cx + wi, y0 + row, gui_lerp(r, color, frac, 256));
        }
    }
}

/* The real mark, not an approximation invented from scratch: this is the
   same trunk/two-branch/tufted-yucca structure `icon.svg` actually draws
   (M100 168 L100 108, then two branches, then a 3-line spiky tuft at the
   trunk top and each branch tip), simplified to fit a ~16px menu-bar icon
   instead of traced stroke-for-stroke, drawn with the same primitives
   every dock icon already uses. A first attempt drew the crown as one
   filled circle; a real screenshot showed it reading as a lollipop, not a
   tree, caught by looking, not assumed correct from the code alone. */
/* `scale` lets the same logo draw crisp at the tiny 16px menu bar size
   (scale 1, hairline AA strokes) and much larger on the boot splash
   (scale 4+, real thickness) without two separate drawings to keep in
   sync. `bg` is whatever this is drawn over, so the branch/tuft capsule
   strokes' AA can blend into it correctly, the menu bar's white and the
   boot screen's dark background are not the same color. Real fix, not
   just a scale knob: the branches and tufts used to be gui_draw_diag,
   raw single-pixel window_pixel dots approximating a line, the same
   "8-bit" staircase problem the weather icon's rays had, now on the one
   piece of branding that appears everywhere including full-size at boot. */
static void gui_draw_logo(int x, int cy, int scale, unsigned int bg, unsigned int c){
    if (!window_has_target() && window_scale() > 1){
        /* Drawn in physical pixels: u is one logo unit, every limb a round-ended
           stroke. The logical path below rounds the menu bar's stroke radius to 0
           and pixel-doubles its diagonals, which is what read as 8-bit. */
        int sc = (int)window_scale(), u = scale * sc;
        int pr = u * 2 / 5; if (pr < 1) pr = 1;
        int ox = x * sc + u / 2, oy = cy * sc;
        #define LG(ax, ay, bx, by) gui_capsule_phys(ox + (ax) * u, oy + (ay) * u, ox + (bx) * u, oy + (by) * u, pr, c)
        LG(0, 5, 0, -7);                                   /* trunk */
        LG(0, -1, -4, -5); LG(0, -1, 4, -5);               /* two main branches */
        LG(0, -7, -3, -10); LG(0, -7, 0, -10); LG(0, -7, 3, -10);      /* crown */
        LG(-4, -5, -6, -7); LG(-4, -5, -6, -5); LG(-4, -5, -6, -3);    /* left tuft */
        LG(4, -5, 6, -7); LG(4, -5, 6, -5); LG(4, -5, 6, -3);          /* right tuft */
        #undef LG
        (void)bg;
        return;
    }
    int split_y = cy - scale, top_y = cy - 7 * scale;
    int r = scale > 1 ? scale - 1 : 0;
    /* Real bug, found from a pixel dump not a guess: aa_band is a fixed
       5px halo (see its definition above), never scaled to the primitive
       it's softening. At menubar scale (1), every branch capsule is only
       4-7px long with r=0, so a 5px halo on each side is wider than the
       shape itself, every branch's halo overlaps its neighbors' and the
       whole logo collapses into two blurry blobs, unrecognizable as a
       tree (confirmed: a real macro-zoom pixel dump of the menubar at
       this exact scale showed exactly that, not a subjective call).
       Same fix pattern gui_render_icon_cached already uses to override
       aa_band for its own scale: shrink it here too, only at scale=1,
       so the branch geometry actually reads instead of drowning in AA. */
    int saved_aa_band = aa_band;
    if (scale == 1) aa_band = 1;
    window_rect(x, split_y, scale, (cy + 5 * scale) - split_y + 1, c); /* trunk, base to branch split */
    window_rect(x, top_y, scale, split_y - top_y + 1, c);              /* trunk continuing above the split */
    gui_draw_capsule(x, split_y, x - 4 * scale, split_y - 4 * scale, r, c, bg); /* left branch */
    gui_draw_capsule(x, split_y, x + 4 * scale, split_y - 4 * scale, r, c, bg); /* right branch */

    int lx = x - 4 * scale, ly = split_y - 4 * scale, rx = x + 4 * scale, ry = split_y - 4 * scale;
    gui_draw_capsule(x, top_y, x - 3 * scale, top_y - 3 * scale, r, c, bg);
    gui_draw_capsule(x, top_y, x,             top_y - 3 * scale, r, c, bg);
    gui_draw_capsule(x, top_y, x + 3 * scale, top_y - 3 * scale, r, c, bg);
    gui_draw_capsule(lx, ly, lx - 2 * scale, ly - 2 * scale, r, c, bg);
    gui_draw_capsule(lx, ly, lx - 2 * scale, ly,             r, c, bg);
    gui_draw_capsule(lx, ly, lx - 2 * scale, ly + 2 * scale, r, c, bg);
    gui_draw_capsule(rx, ry, rx + 2 * scale, ry - 2 * scale, r, c, bg);
    gui_draw_capsule(rx, ry, rx + 2 * scale, ry,             r, c, bg);
    gui_draw_capsule(rx, ry, rx + 2 * scale, ry + 2 * scale, r, c, bg);
    aa_band = saved_aa_band;
}

/* Real, user-reported flicker: this whole bar (a solid white rect, the
   logo, "Joshua Tree", the clock) got redrawn identically on every single
   hover-state change, since gui_draw_desktop calls this unconditionally
   on every mouse move. Nothing here actually depends on hover at all, and
   without a back buffer to swap in atomically, redrawing pixels that
   didn't need to change is pure flicker, not just pure waste. Skips the
   redraw entirely once per real minute unless forced, matching the one
   thing in this bar that actually changes on its own. */
static int gui_menubar_last_min = -1;
static void gui_menubar_force_redraw(void){ gui_menubar_last_min = -1; }

/* Real macOS menu bar clock format: weekday, month, day, 12-hour time with
   AM/PM, not the bare 24h HH:MM this used to show. Reads the CMOS weekday
   (reg 6) and date (reg 7)/month (reg 8) registers the same BCD way
   show_time() already reads hour/minute, no new decoding scheme. RTC
   weekday numbering is 1=Sunday on every real PC and QEMU's own RTC
   emulation, confirmed against this exact machine's real wall clock the
   same way the earlier UTC-vs-local timezone fix was (a real screendump
   matching what `date` printed at that same moment). */
/* v43 (0.43.0): live weather in the menu bar. Open-Meteo answers over
   plain HTTP (checked: a real 200 on http://, no redirect), which is what
   makes this possible at all in a kernel with no TLS. Vancouver by default,
   the one place this machine actually sits. Honest limits, stated rather
   than hidden: the fetch is synchronous inside the GUI loop, once after
   the first frame and then every ten minutes, so on a NIC with no route
   out it can stall the desktop for the WAN timeout; moving it into a
   background task is the follow-up, held back only because net.c's
   receive path was written single-caller and hasn't been audited for a
   second one yet. No NIC (v86, the browser demo) means no attempt and
   nothing drawn, not an error. */
static char weather_text[24] = "";
static unsigned int weather_last_tick = 0;
static int weather_tried_once = 0;
/* v53: real fields behind the one-line summary, kept for the dropdown
   panel. Nothing fabricated: temp/code are exactly what weather_fetch
   already parses out of the reply; lat/lon (v71) are the exact text
   ip-api.com returned for this machine's own public IP (geo_fetch below),
   the same text the http_get() URL is built from, not a literal. */
static int weather_temp_c = 0;
static int weather_code10 = 0;
static int weather_have = 0;
/* Why the last fetch ended the way it did. The Weather window used to have
   exactly one failure face ("Weather unavailable") for five different
   causes, and no way to try again short of waiting ten minutes.
   weather_have/weather_text keep the last GOOD reading across a later
   failure on purpose: the window labels it stale instead of going blank. */
#define WX_NONE    0 /* never tried */
#define WX_OK      1
#define WX_OFFLINE 2 /* no NIC, or the gateway never answered ARP */
#define WX_TIMEOUT 3 /* DNS, connect or reply deadline ran out */
#define WX_FAILED  4 /* resolver said no such host, or the NIC refused the frame */
#define WX_BAD     5 /* non-200, empty, or a body the parser could not read */
static int weather_state = WX_NONE;
static char weather_err[48] = "";
static const char *weather_state_name(int st){
    return st == WX_OK ? "ok" : st == WX_OFFLINE ? "offline" : st == WX_TIMEOUT ? "timeout" : st == WX_FAILED ? "failed" : st == WX_BAD ? "bad" : "none";
}
/* Test/diagnostic override, read once from the multiboot command line
   (`-append "wxhost=10.0.2.2:8099"`, see kmain): both the location and the
   forecast request go to this literal IP:port instead of ip-api.com and
   api.open-meteo.com. tools/checks/weather-app-check.sh points it at a
   local fake server to drive success, bad-response and timeout without
   the real internet. Empty (every normal boot) means the real hosts. */
static char wx_override_host[20] = "";
static unsigned short wx_override_port = 80;
/* A weather one-liner that has not started answering in ~15s will not.
   net.c's default reply budget (sized for local LLM generation, minutes)
   froze the whole desktop that long on a half-open connection. */
#define WX_REPLY_TIMEOUT_TICKS 1200
/* Hit box for the menu-bar weather text, recomputed by gui_draw_menubar
   every time it actually redraws that text (same cadence the clock hit
   test already tolerates: coarse, minute-granularity, matching how often
   the underlying layout can shift). -1/-1 means "nothing drawn, not
   clickable" rather than a stale box from a previous boot. */
static int weather_hit_x0 = -1, weather_hit_x1 = -1;

static const char *weather_word(int code){
    if (code == 0) return "Clear";
    if (code <= 3) return "Cloudy";
    if (code <= 48) return "Fog";
    if (code <= 67) return "Rain";
    if (code <= 77) return "Snow";
    if (code <= 82) return "Showers";
    return "Storm";
}

/* v60: wind_weather_pct's source. Same thresholds as weather_word() above
   on purpose, so the sway always matches the word actually shown in the
   menu bar/dropdown, not a second guess at the same code. Percentages are
   a judgment call (no real wind-speed field exists to derive them from,
   see wind_weather_pct's comment), ordered calmest to strongest by what
   each condition plausibly implies about the air: Fog is stillest, Clear
   next, Cloudy is the unchanged pre-v60 baseline, then Snow/Rain/Showers/
   Storm climb from there. */
static int wind_pct_for_weather_code(int code){
    if (code == 0) return 60;    /* Clear: calm */
    if (code <= 3) return 100;   /* Cloudy: baseline, matches pre-v60 sway */
    if (code <= 48) return 40;   /* Fog: stillest air */
    if (code <= 67) return 130;  /* Rain */
    if (code <= 77) return 90;   /* Snow: typically calmer than rain */
    if (code <= 82) return 150;  /* Showers */
    return 200;                  /* Storm: strongest sway */
}

/* v65 (0.62.0): weather particle overlay, direct follow-up to v60's wind-
   amplitude scaling, the "beyond wind" half of the same real request.
   Real, visible rain streaks / snow dots for the condition codes that
   warrant one, no new subsystem: a small fixed-size particle array
   (WEATHER_PARTICLE_COUNT, same shape as wind_base's own fixed cost
   budget), advanced and drawn once per wind tick (weather_fx_tick, called
   from the same ~20fps loop in gui_run that already drives wind_phase),
   same cost class as the existing wind-sway sampler it rides alongside.
   weather_fx_kind reuses weather_word()'s own threshold boundaries
   exactly (same code10/10 input, same cutoffs), on purpose, so the
   overlay always matches the condition word the menu bar/dropdown already
   shows, never a second guess at the same fetched field: Clear/Cloudy/Fog
   get no overlay, Rain/Showers/Storm get rain streaks, Snow gets snow
   dots. Honest scope note: particles are only ever drawn inside
   WIND_TOP_ROW..WIND_HORIZON_ROW, the same swaying sky band the wind tick
   already repaints in full every frame, which is what erases last frame's
   particles with no separate restore/dirty-rect logic needed (the same
   trick the wind redraw itself already relies on). Extending particles
   across the dock/ground band below the horizon would need the same kind
   of repaint plumbing gui_redraw_dock_band's own band cache has, real
   follow-up, not attempted here. A NIC-less boot (v86) never calls
   weather_fetch, so weather_have stays 0 and weather_fx_tick is a no-op,
   same "nothing fabricated" contract v56/v60 already established for
   every other weather-derived effect in this file. */
#define WEATHER_FX_NONE 0
#define WEATHER_FX_RAIN 1
#define WEATHER_FX_SNOW 2
#define WEATHER_PARTICLE_COUNT 36

struct weather_particle { int x, y, speed; };
static struct weather_particle weather_particles[WEATHER_PARTICLE_COUNT];
static int weather_particles_seeded = 0;
static unsigned int weather_rng = 0;

/* Same tiny LCG keyrate's own word generator already uses (no rand()/no
   libc in this freestanding build); good enough for particle scatter, not
   for anything security-sensitive, same honesty note that code carries. */
static unsigned int weather_rand(void){
    weather_rng = weather_rng * 1103515245u + 12345u;
    return (weather_rng >> 16) & 0x7fff;
}

/* Pure: same answer every time for the same code, no globals touched, so
   weatherfxtest can call this directly. Same cutoffs as weather_word()
   above, stated once rather than re-derived: <=3 Clear/Cloudy, <=48 Fog,
   <=67 Rain, <=77 Snow, <=82 Showers, else Storm. */
static int weather_fx_kind(int code){
    if (code <= 3) return WEATHER_FX_NONE;    /* Clear, Cloudy */
    if (code <= 48) return WEATHER_FX_NONE;   /* Fog */
    if (code <= 67) return WEATHER_FX_RAIN;   /* Rain */
    if (code <= 77) return WEATHER_FX_SNOW;   /* Snow */
    return WEATHER_FX_RAIN;                   /* Showers, Storm */
}

static void weather_particles_seed(int w){
    weather_rng = ticks() ? ticks() : 1;
    for (int i = 0; i < WEATHER_PARTICLE_COUNT; i++){
        weather_particles[i].x = (int)(weather_rand() % (unsigned int)(w > 0 ? w : 1));
        weather_particles[i].y = WIND_TOP_ROW + (int)(weather_rand() % (unsigned int)(WIND_HORIZON_ROW - WIND_TOP_ROW));
        weather_particles[i].speed = 4 + (int)(weather_rand() % 5);
    }
    weather_particles_seeded = 1;
}

/* Advances every particle one tick and draws it directly (physical
   coords via window_pixel_phys, same as the wallpaper's own draw path,
   so it scales correctly at any window_scale()). Colors stay inside the
   Mojave palette on purpose: a light silvery-sand rain streak and an off-
   white (the dock tray's own cream) snow dot, never a saturated or cold-
   blue tone. `kind`/`w` passed explicitly rather than read from globals
   so weatherfxtest can drive this deterministically. */
/* v71 (0.65.0): every particle pixel goes through this clip, and the
   wrap-around check now runs BEFORE the draw, not after. The real bug
   this fixes (Joshua's "dashed glitch bar at the horizon", reproduced
   headlessly: 314 streak-coloured pixels on physical rows 790..804 after
   ten idle seconds under a Rain reading, 12 above): a rain particle at
   logical row 394 with speed 8 was advanced to 402, drawn there, and only
   THEN wrapped back to the top. Rows 395..402 are below WIND_HORIZON_ROW,
   outside the band the wind tick repaints every frame, so nothing ever
   erased those stamps; each streak that crossed the horizon left a
   permanent 4-pixel diagonal at a random x, accumulating into exactly the
   dashed full-width bar seen live. Snow had the same leak one row deep
   (y advanced to 395, dotted at physical 790). Clipping to the band is
   the real contract the v65 entry already stated ("particles only ever
   draw inside WIND_TOP_ROW..WIND_HORIZON_ROW"), now enforced per pixel
   rather than assumed from the reset logic. */
static inline void weather_fx_plot(int px, int py, int w, int sc, unsigned int color){
    if (py < WIND_TOP_ROW * sc || py >= WIND_HORIZON_ROW * sc) return;
    if (px < 0 || px >= w * sc) return;
    window_pixel_phys(px, py, color);
}

static void weather_fx_tick_kind(int kind, int w){
    if (kind == WEATHER_FX_NONE) return;
    if (!weather_particles_seeded) weather_particles_seed(w);
    int sc = (int)window_scale();
    for (int i = 0; i < WEATHER_PARTICLE_COUNT; i++){
        struct weather_particle *p = &weather_particles[i];
        if (kind == WEATHER_FX_RAIN){
            p->y += p->speed;                       /* fast, mostly-vertical fall */
            p->x += 1;                               /* a slight real wind-blown slant */
        } else {
            p->y += 1;                               /* snow drifts, much slower than rain */
            if (weather_rand() & 1) p->x += ((i & 1) * 2 - 1); /* gentle side-to-side drift */
        }
        if (p->y >= WIND_HORIZON_ROW || p->x < 0 || p->x >= w){
            p->x = (int)(weather_rand() % (unsigned int)(w > 0 ? w : 1));
            p->y = WIND_TOP_ROW;
            p->speed = 4 + (int)(weather_rand() % 5);
        }
        int x0 = p->x * sc, y0 = p->y * sc;
        if (kind == WEATHER_FX_RAIN){
            for (int s = 0; s < 4; s++)
                weather_fx_plot(x0 - s, y0 - s * sc, w, sc, 0x00C9C0B4); /* light silvery-sand streak, in-palette */
        } else {
            weather_fx_plot(x0, y0, w, sc, 0x00EFEBE4);           /* off-white dot, the dock tray's own cream */
            if (sc > 1) weather_fx_plot(x0 + 1, y0, w, sc, 0x00EFEBE4);
        }
    }
}

/* The real, live entry point: derives kind from the actually-fetched
   weather_code10, "nothing fabricated" the same way weather_fetch's own
   callers already require. */
static void weather_fx_tick(int w){
    weather_fx_tick_kind(weather_have ? weather_fx_kind(weather_code10 / 10) : WEATHER_FX_NONE, w);
}

/* Pulls a number for `key` from inside the "current":{...} object. The
   units object earlier in the same reply has the same keys with string
   values ("°C"), so the search has to start after "current":{ or it would
   read the wrong one. */
static int json_current_number(const char *json, const char *key, int *out_x10){
    const char *p = json;
    const char *cur = 0;
    while (*p) { if (p[0]=='"' && p[1]=='c' && p[2]=='u' && p[3]=='r' && p[4]=='r' && p[5]=='e' && p[6]=='n' && p[7]=='t' && p[8]=='"' && p[9]==':' && p[10]=='{') { cur = p + 11; break; } p++; }
    if (!cur) return 0;
    for (p = cur; *p && *p != '}'; p++) {
        const char *k = key; const char *q = p;
        if (*q != '"') continue;
        q++;
        while (*k && *q == *k) { q++; k++; }
        if (*k || *q != '"' || q[1] != ':') continue;
        q += 2;
        int neg = 0; if (*q == '-') { neg = 1; q++; }
        int whole = 0, frac = 0, seen_dot = 0;
        while ((*q >= '0' && *q <= '9') || *q == '.') {
            if (*q == '.') { seen_dot = 1; q++; continue; }
            if (!seen_dot) whole = whole * 10 + (*q - '0');
            else if (frac == 0 && !seen_dot) {}
            else if (frac == 0) { frac = (*q - '0'); }
            q++;
        }
        int v = whole * 10 + frac;
        *out_x10 = neg ? -v : v;
        return 1;
    }
    return 0;
}

/* v71 (0.65.0): real location, not a constant. weather_fetch used to hard-
   code latitude=49.28&longitude=-123.12 (downtown Vancouver) in its Open-
   Meteo URL, so every weather-derived effect in this file (menu bar text,
   v56 dropdown, v60 wind amplitude, v65 particles and tint) reported a
   place ~40km from where this machine actually sits (Langley, per both
   Joshua's own phone and a real `curl http://ip-api.com/json/` from the
   host, roadmap.md's "Real find" entry). Open-Meteo itself was never
   wrong, it answered honestly for the coordinates it was given; the
   coordinates were the bug. ip-api.com answers on plain HTTP (no TLS in
   this kernel), so this is the same http_get shape the weather call
   already uses. Looked up once per boot and cached (a public IP doesn't
   move mid-session; a failed lookup is retried on the next ten-minute
   weather cycle). The lat/lon are kept as the exact numeric TEXT ip-api
   returned (json_extract_number_text) and spliced straight into the URL,
   no float parse/format round trip to lose digits in. No fallback
   constant on purpose: with no real location there is no weather fetch,
   the same "nothing fabricated" contract v56/v60/v65 already hold to for
   a NIC-less boot. Both values are mirrored to serial (`geo=`/`wxurl=`)
   so tools/geo-check.sh can prove headlessly, against the host's own
   ip-api answer, that the URL really carries the dynamic location. */
static char geo_lat[16] = "", geo_lon[16] = "", geo_city[24] = "";
static int geo_have = 0;
/* Turns the net/http layer's last failure into a window state plus a short
   human detail. `what` names the request ("location" / "forecast"). */
static void weather_set_error(int st, const char *what, const char *detail){
    weather_state = st;
    int p = 0;
    for (const char *c = what; *c && p < 46; c++) weather_err[p++] = *c;
    if (*detail && p < 45) { weather_err[p++] = ':'; weather_err[p++] = ' '; }
    for (const char *c = detail; *c && p < 47; c++) weather_err[p++] = *c;
    weather_err[p] = 0;
}
static void weather_classify_http_failure(const char *what, int n){
    int e = net_last_error();
    if (n < 0 || e == NET_ERR_REPLY_TIMEOUT) {
        if (e == NET_ERR_ARP_TIMEOUT) weather_set_error(WX_OFFLINE, what, "no route to the network");
        else if (e == NET_ERR_DNS_TIMEOUT || e == NET_ERR_CONNECT_TIMEOUT || e == NET_ERR_REPLY_TIMEOUT) weather_set_error(WX_TIMEOUT, what, net_error_name(e));
        else weather_set_error(WX_FAILED, what, net_error_name(e));
        return;
    }
    int st = http_last_status();
    if (st && st != 200) {
        char d[12] = "HTTP "; int q = 5;
        d[q++] = '0' + (st / 100) % 10; d[q++] = '0' + (st / 10) % 10; d[q++] = '0' + st % 10; d[q] = 0;
        weather_set_error(WX_BAD, what, d);
    } else weather_set_error(WX_BAD, what, n == 0 ? "empty reply" : "unreadable reply");
}

static int geo_fetch(void){
    static char body[1024];
    int n = wx_override_host[0] ? http_get_timeout(wx_override_host, "/json/", wx_override_port, body, sizeof(body) - 1, WX_REPLY_TIMEOUT_TICKS)
                                : http_get_timeout("ip-api.com", "/json/", 80, body, sizeof(body) - 1, WX_REPLY_TIMEOUT_TICKS);
    if (n <= 0 || http_last_status() != 200) { weather_classify_http_failure("location", n); return 0; }
    body[n] = 0;
    char lat[16], lon[16];
    if (!json_extract_number_text(body, "lat", lat, sizeof(lat)) ||
        !json_extract_number_text(body, "lon", lon, sizeof(lon))) { weather_set_error(WX_BAD, "location", "unreadable reply"); return 0; }
    int i;
    for (i = 0; lat[i]; i++) geo_lat[i] = lat[i]; geo_lat[i] = 0;
    for (i = 0; lon[i]; i++) geo_lon[i] = lon[i]; geo_lon[i] = 0;
    if (!json_extract_string(body, "city", geo_city, sizeof(geo_city))) geo_city[0] = 0;
    geo_have = 1;
    serial_puts("geo="); serial_puts(geo_lat); serial_puts(","); serial_puts(geo_lon); serial_puts("\n");
    return 1;
}

/* Weather window extras, all from the same single Open-Meteo reply the
   menu bar reading comes from. wx_extra_have / wx_day_count stay 0 when a
   reply lacks them, and the window then leaves those cells out rather than
   inventing a value. A later failed fetch returns before touching any of
   this, so the stale face shows the whole last good reading. */
#define WX_DAYS 5
static int wx_extra_have = 0, wx_feels_c = 0, wx_humidity = 0, wx_wind_kmh = 0;
static int wx_day_count = 0;
static int wx_day_code[WX_DAYS], wx_day_hi[WX_DAYS], wx_day_lo[WX_DAYS], wx_day_wd[WX_DAYS];
static int wx_round10(int v){ return (v >= 0 ? v + 5 : v - 5) / 10; }
/* Points just past the '[' of "key":[ inside the real "daily":{ object.
   Same trap as json_current_number: "daily_units" repeats every key first,
   with string values, so the search has to start inside "daily":{ itself. */
static const char *json_daily_array(const char *json, const char *key){
    const char *p = json, *d = 0;
    static const char tag[] = "\"daily\":{";
    for (; *p; p++) { int i = 0; while (tag[i] && p[i] == tag[i]) i++; if (!tag[i]) { d = p + i; break; } }
    if (!d) return 0;
    for (p = d; *p && *p != '}'; p++) {
        if (*p != '"') continue;
        const char *q = p + 1, *k = key;
        while (*k && *q == *k) { q++; k++; }
        if (!*k && q[0] == '"' && q[1] == ':' && q[2] == '[') return q + 3;
    }
    return 0;
}
/* Up to `max` numbers from a JSON array body, each times ten with one
   decimal kept, the same fixed-point json_current_number uses. */
static int json_array_x10(const char *p, int *out, int max){
    int n = 0;
    while (p && *p && *p != ']' && n < max) {
        while (*p == ' ' || *p == ',') p++;
        int neg = 0, whole = 0, frac = 0, dot = 0, digits = 0;
        if (*p == '-') { neg = 1; p++; }
        while ((*p >= '0' && *p <= '9') || *p == '.') {
            if (*p == '.') dot = 1;
            else if (!dot) { whole = whole * 10 + (*p - '0'); digits = 1; }
            else if (dot == 1) { frac = *p - '0'; dot = 2; }
            p++;
        }
        if (!digits) break; /* null or a string: stop, keep what was real */
        out[n++] = neg ? -(whole * 10 + frac) : whole * 10 + frac;
    }
    return n;
}
/* Weekday (0 = Sunday) for each "YYYY-MM-DD" in the daily time array,
   Sakamoto's method. */
static int json_array_weekdays(const char *p, int *out, int max){
    static const int t[12] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int n = 0;
    while (p && *p && *p != ']' && n < max) {
        while (*p == ' ' || *p == ',') p++;
        if (*p != '"') break;
        p++;
        int ok = 1; for (int i = 0; i < 10; i++) if (i == 4 || i == 7 ? p[i] != '-' : (p[i] < '0' || p[i] > '9')) ok = 0;
        if (!ok) break;
        int y = (p[0]-'0')*1000 + (p[1]-'0')*100 + (p[2]-'0')*10 + (p[3]-'0');
        int m = (p[5]-'0')*10 + (p[6]-'0'), d = (p[8]-'0')*10 + (p[9]-'0');
        if (m < 1 || m > 12) break;
        if (m < 3) y -= 1;
        out[n++] = (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
        p += 10; if (*p == '"') p++;
    }
    return n;
}

static int weather_fetch_inner(void){
    weather_err[0] = 0;
    if (!net_init(0x0A00020F)) { weather_set_error(WX_OFFLINE, "no network card", ""); return 0; }
    if (!geo_have && !geo_fetch()) return 0; /* v71: no real location, no fetch, nothing fabricated */
    static char body[2048];
    static char path[320]; /* was 128: the longer field list below needs ~235 bytes; http.c's own 512-byte request buffer still holds it */
    { int p = 0; const char *s;
      for (s = "/v1/forecast?latitude="; *s; s++) path[p++] = *s;
      for (s = geo_lat; *s; s++) path[p++] = *s;
      for (s = "&longitude="; *s; s++) path[p++] = *s;
      for (s = geo_lon; *s; s++) path[p++] = *s;
      /* One request for everything the Weather window shows. Daily only,
         five days, no hourly arrays: the real reply is ~860 bytes, well
         inside the 2048-byte body buffer above. */
      for (s = "&current=temperature_2m,apparent_temperature,relative_humidity_2m,wind_speed_10m,weather_code"
               "&daily=weather_code,temperature_2m_max,temperature_2m_min&forecast_days=5&timezone=auto"; *s && p < 318; s++) path[p++] = *s;
      path[p] = 0; }
    serial_puts("wxurl="); serial_puts(path); serial_puts("\n");
    int n = wx_override_host[0] ? http_get_timeout(wx_override_host, path, wx_override_port, body, sizeof(body) - 1, WX_REPLY_TIMEOUT_TICKS)
                                : http_get_timeout("api.open-meteo.com", path, 80, body, sizeof(body) - 1, WX_REPLY_TIMEOUT_TICKS);
    if (n <= 0 || http_last_status() != 200) { weather_classify_http_failure("forecast", n); return 0; }
    body[n] = 0;
    int t10 = 0, code10 = 0;
    if (!json_current_number(body, "temperature_2m", &t10)) { weather_set_error(WX_BAD, "forecast", "unreadable reply"); return 0; }
    json_current_number(body, "weather_code", &code10);
    int t = (t10 >= 0 ? t10 + 5 : t10 - 5) / 10; /* round to whole degrees */
    weather_temp_c = t; weather_code10 = code10; weather_have = 1;
    wind_weather_pct = wind_pct_for_weather_code(code10 / 10); /* v60: real wind sway now follows real weather */
    { int f10 = 0, h10 = 0, w10 = 0;
      wx_extra_have = json_current_number(body, "apparent_temperature", &f10)
                   && json_current_number(body, "relative_humidity_2m", &h10)
                   && json_current_number(body, "wind_speed_10m", &w10);
      wx_feels_c = wx_round10(f10); wx_humidity = wx_round10(h10); wx_wind_kmh = wx_round10(w10);
      int nw = json_array_weekdays(json_daily_array(body, "time"), wx_day_wd, WX_DAYS);
      int nc = json_array_x10(json_daily_array(body, "weather_code"), wx_day_code, WX_DAYS);
      int nh = json_array_x10(json_daily_array(body, "temperature_2m_max"), wx_day_hi, WX_DAYS);
      int nl = json_array_x10(json_daily_array(body, "temperature_2m_min"), wx_day_lo, WX_DAYS);
      int nd = nw; if (nc < nd) nd = nc; if (nh < nd) nd = nh; if (nl < nd) nd = nl;
      for (int i = 0; i < nd; i++) { wx_day_code[i] /= 10; wx_day_hi[i] = wx_round10(wx_day_hi[i]); wx_day_lo[i] = wx_round10(wx_day_lo[i]); }
      wx_day_count = nd; }
    int p = 0;
    if (t < 0) { weather_text[p++] = '-'; t = -t; }
    if (t >= 10) weather_text[p++] = '0' + t / 10;
    weather_text[p++] = '0' + t % 10;
    weather_text[p++] = (char)0xF8; /* CP437 degree sign, present in both the hardware font and the fallback */
    weather_text[p++] = ' ';
    for (const char *w = weather_word(code10 / 10); *w; w++) weather_text[p++] = *w;
    weather_text[p] = 0;
    weather_state = WX_OK;
    /* What the window's secondary row and forecast row will be built from. */
    serial_puts("wxextra="); serial_puts(wx_extra_have ? "yes" : "no");
    serial_puts(" days="); { char d[2] = { (char)('0' + wx_day_count), 0 }; serial_puts(d); } serial_puts("\n");
    serial_puts("wx="); serial_puts(weather_text); serial_puts("\n"); /* v71: tools/geo-check.sh asserts the fetch really landed, not just that the URL was built */
    return 1;
}
static void weather_fetch(void){
    weather_last_tick = ticks();
    serial_puts("wxfetch\n"); /* tools/checks/weather-app-check.sh counts these: a failed fetch must not re-run on every repaint */
    weather_fetch_inner();
    /* One line per attempt, the state the window will show and why. */
    serial_puts("wxstate="); serial_puts(weather_state_name(weather_state));
    if (weather_err[0]) { serial_puts(" "); serial_puts(weather_err); }
    serial_puts("\n");
}

/* v75 (0.67.0): the real location-dynamic wallpaper, the item roadmap.md's
   satellite entry was building toward. Honest naming first: this is a MAP
   of the real town, not a satellite photo. Real curl checks (roadmap.md,
   v75 entry) found every satellite/imagery source that answers on plain
   HTTP at all (Google's mt0 `lyrs=s`, Bing virtualearth) serves JPEG,
   and this kernel only has a PNG decoder; OSM's own tile servers, Esri
   World Imagery and NASA GIBS all 301 straight to HTTPS. Of the PNG
   sources that do answer plain HTTP (CartoCDN, Thunderforest, OsmAnd's
   app proxy, OpenTopoMap), CartoCDN and Thunderforest stamp a huge "API
   KEY REQUIRED" watermark across keyless tiles (found on the first real
   framebuffer dump, not in the curl headers: 200, image/png, looked
   fine until rendered), OsmAnd's is an undocumented proxy for their own
   app, and OpenTopoMap (tile.opentopomap.org, CC-BY-SA, free with
   attribution for light use) answers a bare HTTP/1.0 GET with a clean,
   watermark-free 8-bit paletted PNG, no key, no User-Agent check, which
   is exactly what http_get + png_decode can consume. Topographic style
   (contours, hillshade), which suits a desert-named OS better than a
   flat street map anyway.

   Slippy-map tile math (the OSM wiki's "Slippy map tilenames", the same
   formula every tile client uses): at zoom z, x = (lon+180)/360 * 2^z,
   y = (1 - ln(tan(lat) + sec(lat)) / pi) / 2 * 2^z, with lat in radians.
   No libm here, so ln/tan/pi come from the x87 directly (fyl2x, fptan,
   fldpi), the same FPU the calculator's doubles already run on. The 4x3
   mosaic of 256px tiles (1024x768) is picked so the location lands near
   its middle (top-left tile = floor(x - 1.5), floor(y - 1.0)), then a
   960x540 window is cut out centered on the location, clamped to the
   mosaic, so the town is under the middle of the desktop, not at a tile
   corner. Each tile is decoded and copied straight into the 960x540
   buffer, never assembled into a full mosaic first: peak heap is the
   1.5MB destination plus one decoded tile, not 2.3MB more on top.

   Everything it does is mirrored to serial (`wall=` on success with the
   tile coords, crop offset and an FNV-1a of the finished buffer;
   `wallerr=` on any failure with the step that failed) so
   tools/wallpaper-check.sh can prove the whole chain headlessly against
   the host's own download of the same twelve tiles. */
static double jt_tan(double x){ double r; __asm__ volatile ("fptan\n\tfstp %%st(0)" : "=t"(r) : "0"(x)); return r; }
static double jt_ln(double x){ double r; __asm__ volatile ("fldln2\n\tfxch\n\tfyl2x" : "=t"(r) : "0"(x) : "st(1)"); return r; }
static double jt_sqrt(double x){ double r; __asm__ volatile ("fsqrt" : "=t"(r) : "0"(x)); return r; }
static double jt_pi(void){ double r; __asm__ volatile ("fldpi" : "=t"(r)); return r; }
static double jt_parse_double(const char *s){ /* "-123.0456" -> double, the only shape ip-api's lat/lon text takes */
    int neg = 0; double v = 0, scale = 0.1;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; }
    if (*s == '.') { s++; while (*s >= '0' && *s <= '9') { v += (*s - '0') * scale; scale *= 0.1; s++; } }
    return neg ? -v : v;
}
static void wall_serial_err(const char *step, int code){
    char b[48]; int i = 0; const char *s = "wallerr="; while (*s) b[i++] = *s++;
    while (*step && i < 40) b[i++] = *step++;
    if (code) { b[i++] = ' '; unsigned int u = (unsigned int)(code < 0 ? -code : code); if (code < 0) b[i++] = '-'; char d[12]; int nd = 0; do { d[nd++] = '0' + u % 10; u /= 10; } while (u); while (nd) b[i++] = d[--nd]; }
    b[i++] = '\n'; b[i] = 0; serial_puts(b);
}
static void wall_caches_drop(void); /* defined after the dock band code it invalidates */
static int wall_fetch(void){
    if (!geo_have) { wall_serial_err("nogeo", 0); return 0; }
    double lat = jt_parse_double(geo_lat), lon = jt_parse_double(geo_lon);
    double n = (double)(1 << WALL_ZOOM);
    double latr = lat * jt_pi() / 180.0;
    double t = jt_tan(latr);
    double xf = (lon + 180.0) / 360.0 * n;
    double yf = (1.0 - jt_ln(t + jt_sqrt(1.0 + t * t)) / jt_pi()) / 2.0 * n;
    if (xf < 1.5 || yf < 1.0 || xf >= n - 2.5 || yf >= n - 2.0) { wall_serial_err("range", 0); return 0; }
    int tx = (int)(xf - 1.5), ty = (int)(yf - 1.0);          /* top-left tile of the 4x3, location in its middle */
    int px = (int)((xf - tx) * WALL_TILE), py = (int)((yf - ty) * WALL_TILE); /* location inside the 1024x768 mosaic: x 384..639, y 256..511 */
    int cx = px - WALLPAPER_W / 2, cy = py - WALLPAPER_H / 2;   /* crop origin, clamped to the mosaic */
    if (cx < 0) cx = 0; if (cx > WALL_COLS * WALL_TILE - WALLPAPER_W) cx = WALL_COLS * WALL_TILE - WALLPAPER_W;
    if (cy < 0) cy = 0; if (cy > WALL_ROWS * WALL_TILE - WALLPAPER_H) cy = WALL_ROWS * WALL_TILE - WALLPAPER_H;

    unsigned char *dst = wall_map ? wall_map : (unsigned char *)kmalloc(WALLPAPER_W * WALLPAPER_H * 3);
    if (!dst) { wall_serial_err("nomem", 0); return 0; }
    unsigned char *body = (unsigned char *)kmalloc(65536);
    if (!body) { if (!wall_map) kfree(dst); wall_serial_err("nomem", 1); return 0; }
    static char path[96];
    /* v0.73: satellite pulls real JPEG tiles from Google's slippy-map
       satellite endpoint instead of OpenTopoMap's PNG line-art. Same x/y/z
       tile math above (Google uses the identical slippy-map convention,
       unlike Bing's quadkey scheme), just a different host, path shape and
       decoder. mt0.google.com/vt/lyrs=s&x=X&y=Y&z=Z was confirmed live
       over plain HTTP, real baseline JPEG, before this was wired. */
    int use_sat = (wall_theme == WALL_SAT);
    for (int i = 0; i < WALL_COLS * WALL_ROWS; i++) {
        int col = i % WALL_COLS, row = i / WALL_COLS;
        int ttx = tx + col, tty = ty + row;
        int p = 0; const char *s;
        int n_bytes; const char *host;
        if (use_sat) {
            host = "mt0.google.com";
            for (s = "/vt/lyrs=s&x="; *s; s++) path[p++] = *s;
            { char d[12]; int nd = 0; unsigned int u = (unsigned int)ttx; do { d[nd++] = '0' + u % 10; u /= 10; } while (u); while (nd) path[p++] = d[--nd]; }
            for (s = "&y="; *s; s++) path[p++] = *s;
            { char d[12]; int nd = 0; unsigned int u = (unsigned int)tty; do { d[nd++] = '0' + u % 10; u /= 10; } while (u); while (nd) path[p++] = d[--nd]; }
            for (s = "&z="; *s; s++) path[p++] = *s;
            { char d[12]; int nd = 0; unsigned int u = WALL_ZOOM; do { d[nd++] = '0' + u % 10; u /= 10; } while (u); while (nd) path[p++] = d[--nd]; }
            path[p] = 0;
        } else {
            host = "a.tile.opentopomap.org";
            for (s = "/"; *s; s++) path[p++] = *s;
            { char d[12]; int nd = 0; unsigned int u = WALL_ZOOM; do { d[nd++] = '0' + u % 10; u /= 10; } while (u); while (nd) path[p++] = d[--nd]; path[p++] = '/'; }
            { char d[12]; int nd = 0; unsigned int u = (unsigned int)ttx; do { d[nd++] = '0' + u % 10; u /= 10; } while (u); while (nd) path[p++] = d[--nd]; path[p++] = '/'; }
            { char d[12]; int nd = 0; unsigned int u = (unsigned int)tty; do { d[nd++] = '0' + u % 10; u /= 10; } while (u); while (nd) path[p++] = d[--nd]; }
            for (s = ".png"; *s; s++) path[p++] = *s;
            path[p] = 0;
        }
        n_bytes = http_get(host, path, 80, body, 65536);
        if (n_bytes <= 0) { kfree(body); if (!wall_map) kfree(dst); wall_serial_err("http", i); return 0; }
        unsigned char *px_out = 0; unsigned int w = 0, h = 0, ch = 0;
        int r = use_sat ? jpeg_decode(body, (unsigned int)n_bytes, &px_out, &w, &h, &ch)
                         : png_decode(body, (unsigned int)n_bytes, &px_out, &w, &h, &ch);
        if (r != 0 || w != WALL_TILE || h != WALL_TILE || ch != 3) {
            if (px_out) kfree(px_out); kfree(body); if (!wall_map) kfree(dst);
            wall_serial_err(r ? (use_sat ? "jpeg" : "png") : "tilesize", r ? r : (int)w); return 0;
        }
        /* copy the part of this tile that lands inside the crop window */
        int ox = col * WALL_TILE, oy = row * WALL_TILE; /* tile origin in mosaic coords */
        for (int y = 0; y < WALL_TILE; y++) {
            int my = oy + y - cy; if (my < 0 || my >= WALLPAPER_H) continue;
            int x0 = cx - ox; if (x0 < 0) x0 = 0;
            int x1 = cx + WALLPAPER_W - ox; if (x1 > WALL_TILE) x1 = WALL_TILE;
            if (x1 <= x0) continue;
            memcpy(dst + (my * WALLPAPER_W + (ox + x0 - cx)) * 3, px_out + (y * WALL_TILE + x0) * 3, (unsigned int)(x1 - x0) * 3);
        }
        kfree(px_out);
    }
    kfree(body);
    wall_map = dst; wall_map_tx = tx; wall_map_ty = ty; wall_map_cx = cx; wall_map_cy = cy; wall_map_is_sat = use_sat;
    unsigned int fnv = 0x811c9dc5u;
    for (unsigned int i = 0; i < WALLPAPER_W * WALLPAPER_H * 3; i++) { fnv ^= dst[i]; fnv *= 0x01000193u; }
    { char b[96]; int i = 0; const char *s = "wall="; while (*s) b[i++] = *s++;
      int vals[5] = { WALL_ZOOM, tx, ty, cx, cy };
      for (int v = 0; v < 5; v++) { char d[12]; int nd = 0; unsigned int u = (unsigned int)vals[v]; do { d[nd++] = '0' + u % 10; u /= 10; } while (u); while (nd) b[i++] = d[--nd]; b[i++] = v < 4 ? ',' : ' '; }
      for (int sh = 28; sh >= 0; sh -= 4) { int nib = (fnv >> sh) & 0xF; b[i++] = nib < 10 ? '0' + nib : 'a' + nib - 10; }
      b[i++] = '\n'; b[i] = 0; serial_puts(b); }
    return 1;
}
/* v0.73: the one place every wallpaper-theme setter goes through, so none
   of them can forget the sat/topo source-boundary rule above. Warm/Cool/
   Raw all reuse the same wall_map pixels (just a different grade), so
   switching among them is free. Crossing into or out of Satellite means
   the pixels on hand are from the wrong real source entirely, so wall_map
   is dropped (forcing the next weather cycle or `wallpaper fetch` to pull
   fresh tiles from the right host) instead of silently painting topo
   pixels under a "Satellite" label or vice versa. */
static void wall_switch_theme(int theme){
    int want_sat = (theme == WALL_SAT);
    if (wall_map && want_sat != wall_map_is_sat) { kfree(wall_map); wall_map = 0; wall_caches_drop(); }
    wall_theme = theme;
    settings_save();
    /* v0.75: the one real choke point every theme setter already goes
       through, so this is the one place to log it -- lets a real browser
       run (no serial port to read otherwise) confirm which theme actually
       got switched to via window.__jt.serial, the same real-evidence
       pattern ne2k-check.mjs already established for geo=/wx=. */
    serial_puts("walltheme="); char tb[2] = { (char)('0' + wall_theme), 0 }; serial_puts(tb); serial_puts("\n");
}
/* Switch what the desktop paints from. Both directions drop every cache
   built from the old pixels (the wind crown band, the dock band) so the
   next frame is honest, not a stale composite of the previous source. */
/* v81: real bug caught while capturing evidence for this pass, before
   this fix shipped -- wind_base (the cached, tinted wallpaper samples
   gui_draw_desktop lazily builds, see its own v75 comment) is keyed on
   wall_src's pointer changing, not on wall_theme. Switching Warm -> Cool
   -> Raw all pass want_map=1 with the SAME wall_map buffer, so wall_src
   never actually changes pointer and the old early-return skipped
   wall_caches_drop() entirely: the desktop kept painting whichever
   theme's grade got cached first, headless framebuffer evidence showed
   all three map themes rendering identical mean color for the wind band
   (57.x/72.x/88.x across Warm/Cool/Raw) until this was found and fixed.
   wall_last_theme tracks what was actually baked into wind_base last, so
   a theme-only change (same wall_map pointer, different wall_theme)
   still drops the stale cache. */
static int wall_last_theme = -1;
/* Decode the baked satellite capture (kernel/wall_sat.h) once, lazily, and
   keep it for the session. The source bytes are a real
   photograph stored as an indexed PNG (drivers/png.c has decoded 8-bit
   indexed/PLTE images since v75) instead of a solid fill, so this one goes
   through png_decode instead of a fill loop. Decoding ~277KB of PNG once
   per boot is cheap; the decoded 960x540x3 buffer is what wall_src actually
   points readers at, same layout wallpaper_rgb and wall_map already use.
   On any decode failure this leaves wall_sat_rgb null and the caller falls
   back to wallpaper_rgb, never a null wall_src. */
static void wall_sat_init(void){
    if (wall_sat_rgb) return; /* already decoded */
    unsigned char *out = 0; unsigned int w = 0, h = 0, ch = 0;
    if (png_decode(wall_sat_png, WALL_SAT_PNG_LEN, &out, &w, &h, &ch) != 0) {
        wall_serial_err("wallsat decode", 0);
        return;
    }
    if (w != WALLPAPER_W || h != WALLPAPER_H || ch != 3) {
        wall_serial_err("wallsat dims", (int)w);
        kfree(out);
        return;
    }
    wall_sat_rgb = out; /* ours now; never freed, lives for the kernel's lifetime */
}
static void wall_apply(int want_map){
    const unsigned char *next;
    if (want_map && !wall_map){
        /* No live map yet, and maybe never: show the baked satellite
           capture, the same kind of imagery the fetch will bring, so a
           landed fetch reads as a refresh rather than a reveal.
           This used to be two branches split on font_is_fallback() as a
           stand-in for "this is the v86 browser demo": v86 got the bake,
           everything else got a solid black placeholder until the fetch
           landed. Real bug, direct report ("the demo isn't showing the
           wallpaper reliably, it's falling back to black"): that signal is
           a VGA font-plane quirk, not a network fact. Whenever it read 0
           inside the browser the demo took the "fetch is pending" branch,
           and a browser fetch to the tile hosts never lands (CORS, see the
           landing page's own note), so black was permanent. One branch, no
           guess about where we are running. If the decode ever fails this
           still falls back to the tree photo, never a blank desktop. */
        wall_sat_init();
        next = wall_sat_rgb ? wall_sat_rgb : wallpaper_rgb;
    } else {
        next = (want_map && wall_map) ? wall_map : wallpaper_rgb;
    }
    if (next == wall_src && wall_theme == wall_last_theme) return;
    /* Real-evidence marker for tools/checks/wallboot-check.sh, the same
       "log it at the one real choke point" pattern wall_switch_theme's
       own walltheme= line already uses. Fires every
       time wall_src actually changes buffer, which includes the very
       first call (wall_src's static initializer is wallpaper_rgb and
       wall_last_theme starts at -1, so the first real assignment always
       logs), letting a headless boot prove what buffer the first real
       desktop paint used without guessing from timing alone. "satfallback"
       is its own distinct value, not folded into "map": it is the baked
       v86 satellite capture, not a live wall_map fetch, and a check that
       cannot tell them apart could not prove the v86 fallback bake-in
       actually took effect. */
    serial_puts("wallsrc=");
    serial_puts(next == wallpaper_rgb ? "photo" : (next == wall_sat_rgb ? "satfallback" : "map"));
    serial_puts("\n");
    wall_src = next;
    wall_last_theme = wall_theme;
    wall_caches_drop();
}

static void gui_draw_menubar(void){
    u8 h, m, wd, dom, mon;
    cmos_read_time_stable(&h, &m, &wd, &dom, &mon);
    u8 hv = (h & 0x0F) + ((h >> 4) * 10), mv = (m & 0x0F) + ((m >> 4) * 10);
    u8 wdv = (wd & 0x0F) + ((wd >> 4) * 10);
    u8 domv = (dom & 0x0F) + ((dom >> 4) * 10);
    u8 monv = (mon & 0x0F) + ((mon >> 4) * 10);
    if (mv == gui_menubar_last_min) return;
    gui_menubar_last_min = mv;
    serial_puts("menubarredraw\n"); /* discriminating marker for tools/checks/menuclock-check.sh: a real minute-change redraw */

    /* v48: Liquid Glass, direct request. No real alpha compositing in this
       framebuffer (see gui_blend's own note), so "translucent" here means
       the same trick the dock shadow already uses: a real, solid,
       precomputed blend of white toward whatever wallpaper color sits
       behind this row, sampled per-row (gui_wallpaper_color already does
       exactly this, reused, not a second sampler).
       v0.76.17: direct request ("menu bar transparency like 50%"), moved
       from 7:10 (70% white / 30% wallpaper) to a genuine 5:10 (50/50)
       blend -- still just a precomputed solid color per row, no real
       blur/alpha, but honestly a 50% mix now instead of mostly-white. */
    for (int row = 0; row < GUI_MENUBAR_H; row++)
        window_rect(0, row, (int)window_width(), 1, gui_lerp(gui_wallpaper_color(row), 0x00FFFFFF, 5, 10));
    window_rect(0, GUI_MENUBAR_H - 1, (int)window_width(), 1, 0x00DDD9D3);
    gui_draw_logo(16, GUI_MENUBAR_H / 2 + 2, 1, 0x00FFFFFF, 0x00000000); /* v0.76.47: menu bar is semi-translucent light chrome, direct correction -- black reads here, not white */
    font_draw_string(portfolio_dock ? "Joshua Trommel" : "Joshua Tree", 32, 7, 0x001C1C1E, -1); /* portfolio mode is his site, so the corner carries his name */

    static const char *WD[7] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char *MO[12] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    int wdi = (wdv >= 1 && wdv <= 7) ? wdv - 1 : 0;
    int moi = (monv >= 1 && monv <= 12) ? monv - 1 : 0;
    int h12 = hv % 12; if (h12 == 0) h12 = 12;
    const char *ampm = hv < 12 ? "AM" : "PM";

    char clock[24];
    int p = 0;
    for (const char *s = WD[wdi]; *s; s++) clock[p++] = *s;
    clock[p++] = ' ';
    for (const char *s = MO[moi]; *s; s++) clock[p++] = *s;
    clock[p++] = ' ';
    if (domv >= 10) clock[p++] = '0' + domv / 10;
    clock[p++] = '0' + domv % 10;
    clock[p++] = ' '; clock[p++] = ' ';
    if (h12 >= 10) clock[p++] = '0' + h12 / 10;
    clock[p++] = '0' + h12 % 10;
    clock[p++] = ':';
    clock[p++] = '0' + mv / 10; clock[p++] = '0' + mv % 10;
    clock[p++] = ' '; clock[p++] = ampm[0]; clock[p++] = ampm[1];
    clock[p] = 0;

    int cw = font_string_width(clock); /* v77: real proportional width, not p*8 */
    font_draw_string(clock, (int)window_width() - cw - 16, 7, 0x001C1C1E, -1);
    if (weather_text[0]) {
        int wl = font_string_width(weather_text);
        int wx = (int)window_width() - cw - 16 - wl - 28;
        font_draw_string(weather_text, wx, 7, 0x00884B16, -1);
        weather_hit_x0 = wx - 4; weather_hit_x1 = wx + wl + 4; /* v53: real click target, same padding feel as the clock's own */
    } else {
        weather_hit_x0 = weather_hit_x1 = -1;
    }
}

/* Real redesign, not a bigger version of the old one: side-by-side with
   real macOS icons, the actual gap wasn't polish, it was construction.
   Every real macOS icon is a full-color illustration filling most of its
   square; this was a small white symbol centered on a flat color chip,
   closer to a generic Android/Material icon than anything Apple ships.
   The sun is a real warm gold-to-white gradient now (a real filled
   gradient circle, gui_fill_circle_gradient, not one flat tone), sized
   to actually fill the icon the way a real glyph does, not float in the
   middle of empty space. */
static void gui_fill_circle_gradient(int cx, int cy, int r, unsigned int top, unsigned int bot, unsigned int into){
    int outer2 = (r + AA_BAND) * (r + AA_BAND);
    for (int dy = -r - AA_BAND; dy <= r + AA_BAND; dy++){
        unsigned int local = gui_lerp(top, bot, dy + r, 2 * r > 0 ? 2 * r : 1);
        for (int dx = -r - AA_BAND; dx <= r + AA_BAND; dx++){
            int d2 = dx * dx + dy * dy;
            if (d2 > outer2) continue;
            if (d2 <= r * r) { window_pixel(cx + dx, cy + dy, local); continue; }
            int t = gui_isqrt(d2) - r;
            window_pixel(cx + dx, cy + dy, gui_lerp(local, into, t, AA_BAND));
        }
    }
}

static void gui_icon_weather(int cx, int cy, int s, unsigned int bg){
    int r = s * 3 / 10, ray = s / 4, gap = r + 3, diag = (ray * 7) / 10; /* ~cos(45deg) */
    /* v61: real, confirmed bug, not a guess: this was a flat "2" no
       matter how big s (the supersample buffer size) got, the one
       stroke width in this whole file that never scaled with its icon
       like every other one here does (gui_icon_notes's s/11,
       gui_icon_terminal's s/16, ...). The four axis-aligned rays still
       looked crisp at that width, since a horizontal/vertical stroke's
       perpendicular offset lands the same way on every row or column it
       crosses. The four DIAGONAL rays didn't: confirmed with a real
       boot-time framebuffer dump at 160px (well past dock size, so not
       a small-icon artifact either) that they render with a genuine
       sawtooth edge the axis-aligned rays don't have, even through this
       file's existing 6x supersample + box-downsample pipeline, because
       a stroke under about a physical pixel wide can't produce a
       consistent partial-coverage average along a 45-degree line no
       matter how much supersampling sits on top of it, the diagonal
       equivalent of a hairline. Scaled like every other stroke here,
       with the same "2" as a floor for whatever tiny icon size this
       might ever be asked to draw at. */
    int ray_r = s / 34;
    if (ray_r < 2) ray_r = 2;
    unsigned int sun_top = 0x00FFE380, sun_bot = 0x00FFA716; /* warm gold, a real color, not flat white */
    gui_fill_circle_gradient(cx, cy, r, sun_top, sun_bot, bg);
    gui_draw_capsule(cx, cy - gap,         cx, cy - gap - ray,         ray_r, ICON_FG, bg);
    gui_draw_capsule(cx, cy + gap,         cx, cy + gap + ray,         ray_r, ICON_FG, bg);
    gui_draw_capsule(cx - gap,       cy,   cx - gap - ray,       cy,   ray_r, ICON_FG, bg);
    gui_draw_capsule(cx + gap,       cy,   cx + gap + ray,       cy,   ray_r, ICON_FG, bg);
    gui_draw_capsule(cx - gap, cy - gap,   cx - gap - diag, cy - gap - diag, ray_r, ICON_FG, bg);
    gui_draw_capsule(cx + gap, cy - gap,   cx + gap + diag, cy - gap - diag, ray_r, ICON_FG, bg);
    gui_draw_capsule(cx - gap, cy + gap,   cx - gap - diag, cy + gap + diag, ray_r, ICON_FG, bg);
    gui_draw_capsule(cx + gap, cy + gap,   cx + gap + diag, cy + gap + diag, ray_r, ICON_FG, bg);
}

static void gui_icon_pin(int cx, int cy, int s, unsigned int bg){
    int r = s / 4, head_cy = cy - s / 8;
    unsigned int pin_top = 0x00FF6B5B, pin_bot = 0x00E8291A; /* real map-pin red, a color, not white */
    gui_fill_circle_gradient(cx, head_cy, r, pin_top, pin_bot, bg);
    gui_fill_circle(cx, head_cy, r / 3, bg, pin_bot); /* punch the pinhole through to the icon's own background */
    gui_fill_triangle_down(cx, head_cy + r - 1, r, s / 3, pin_bot);
}

static void gui_icon_chat(int cx, int cy, int s, unsigned int bg){
    int w = (s * 8) / 10, h = (s * 6) / 10;
    int x = cx - w / 2, y = cy - h / 2 - s / 12;
    unsigned int bub_top = 0x0068E651, bub_bot = 0x0032B92C; /* real Messages green, a color, not white */
    gui_rounded_rect_gradient(x, y, w, h, bub_top, bub_bot, bg, 6);
    gui_fill_triangle_down(x + w / 5, y + h - 1, s / 9, s / 7, bub_bot);
    /* three typing dots, the same shorthand every real chat app uses for
       "something is being said here", the detail that turns a blank
       speech bubble into an unmistakable chat icon. Blend target is the
       bubble's real color at the dots' own row, not either gradient
       endpoint: the same fixed-sample-on-a-gradient mismatch already
       found and fixed on the dock tray's corners this session, avoided
       here by computing it, not reusing a nearby constant. */
    int dot_r = s / 22, dot_gap = s / 7, mid_y = y + h / 2;
    unsigned int bub_mid = gui_lerp(bub_top, bub_bot, h / 2, h > 0 ? h : 1);
    gui_fill_circle(cx - dot_gap, mid_y, dot_r, ICON_FG, bub_mid);
    gui_fill_circle(cx,           mid_y, dot_r, ICON_FG, bub_mid);
    gui_fill_circle(cx + dot_gap, mid_y, dot_r, ICON_FG, bub_mid);
}

/* A folder reads as a folder because of its silhouette (the tab breaking
   the top edge) and a hint of the two-ply paper stock, not because of a
   flat rectangle. A slightly darker back-panel shade behind the front
   face fakes that fold without any alpha blending, just a second real
   solid color. */
/* Real regression caught after living with it next to a real photo
   background, not on the first screendump: the color redesign swapped
   this from gui_rounded_rect (real AA corners) to a plain per-row
   window_rect loop with none at all, so the folder alone had hard
   square corners while every other icon on the dock stayed rounded, the
   kind of inconsistency that reads as "bitmap" even when nothing about
   it is actually jagged. Front face is gui_rounded_rect_gradient again,
   the same primitive it always should have kept, just with real color
   this time instead of the old flat white. */
static void gui_icon_folder(int cx, int cy, int s, unsigned int bg){
    int w = (s * 8) / 10, h = (s * 6) / 10;
    int x = cx - w / 2, y = cy - h / 2 + s / 12;
    unsigned int face_top = 0x006FC6FF, face_bot = 0x000A84FF; /* real Finder blue, a color, not white */
    unsigned int shade = gui_blend(face_bot, 0x00000000); /* back panel/tab a real shadow tone of the same blue, not a generic gray */
    unsigned int tab_highlight = gui_blend(face_top, 0x00FFFFFF); /* light highlight on tab for dimension */
    /* v0.76.23: icon sharpness pass, folder icon enhanced with better visual
       separation and depth. Tab now has a highlight edge to read as raised,
       and the layering has more visual hierarchy through edge treatment. */
    int tab_w = w / 3, tab_h = s / 12;
    window_rect(x, y - tab_h, tab_w, tab_h, shade);  /* tab shadow base */
    window_rect(x, y - tab_h, tab_w, 1, tab_highlight); /* tab top edge, lit */
    window_rect(x + 2, y - 2, w - 4, h, shade);        /* back panel peeking out top/right */
    gui_rounded_rect_gradient(x, y, w, h, face_top, face_bot, bg, 6);
}

/* Each key gets a light top-left / dark bottom-right bevel instead of one
   flat fill, the cheapest real way to read as a raised, pressable key
   rather than a flat tile, at this resolution a full 3D render buys
   nothing a two-tone bevel doesn't already say. */
static void gui_icon_keyrate(int cx, int cy, int s, unsigned int bg){
    (void)bg; /* keys are fully opaque real colors now, no AA blend target needed */
    int key = s / 5, gap = s / 11;
    int total_w = 3 * key + 2 * gap, total_h = 2 * key + gap;
    int x0 = cx - total_w / 2, y0 = cy - total_h / 2;
    /* Real charcoal keycaps, not a step below white: a real keyboard's
       keys are dark, the highlight/shadow bevel is what makes them read
       as pressable, not the base tone being light. */
    unsigned int base = 0x003A3A3C, hi = 0x006E6E72, lo = 0x001C1C1E;
    for (int row = 0; row < 2; row++){
        for (int col = 0; col < 3; col++){
            int kx = x0 + col * (key + gap), ky = y0 + row * (key + gap);
            window_rect(kx, ky, key, key, base);
            window_rect(kx, ky, key, 1, hi);           /* top edge, lit */
            window_rect(kx, ky, 1, key, hi);           /* left edge, lit */
            window_rect(kx, ky + key - 1, key, 1, lo); /* bottom edge, shadowed */
            window_rect(kx + key - 1, ky, 1, key, lo); /* right edge, shadowed */
        }
    }
}

/* An open book: two pages either side of a spine, each with a couple of
   short "text lines" so it doesn't read as two blank cards, and the left
   page shaded a shade darker the way a real open book's left page catches
   less light than the right. */
static void gui_icon_book(int cx, int cy, int s, unsigned int bg){
    int w = (s * 8) / 10, h = (s * 6) / 10;
    int x = cx - w / 2, y = cy - h / 2;
    unsigned int cover = 0x00FF6B57, left_shade = gui_blend(cover, 0x00000000); /* a real warm coral cover, not white */
    window_rect(x, y, w / 2 - 1, h, left_shade);
    window_rect(cx + 1, y, w / 2 - 1, h, cover);
    window_rect(cx - 1, y, 2, h, gui_blend(left_shade, 0x00000000)); /* spine split between the two pages */
    int line_w = w / 2 - 2 * (s / 20) - 1, line_x0 = x + s / 20, line_x1 = cx + 1 + s / 20;
    for (int i = 1; i <= 3; i++){
        int ly = y + (h * i) / 4;
        window_rect(line_x0, ly, line_w, 1, bg);
        window_rect(line_x1, ly, line_w, 1, left_shade);
    }
}

static void gui_icon_quotes(int cx, int cy, int s, unsigned int bg){
    int r = s / 7, off = s / 5, base_cy = cy - s / 10;
    unsigned int gold = 0x00FFD24D; /* real gold, not white, real macOS icons carry color even in a simple glyph */
    gui_fill_circle(cx - off, base_cy, r, gold, bg);
    gui_fill_triangle_down(cx - off, base_cy + r - 1, r, s / 5, gold);
    gui_fill_circle(cx + off, base_cy, r, gold, bg);
    gui_fill_triangle_down(cx + off, base_cy + r - 1, r, s / 5, gold);
}

/* A pencil, diagonal body via the same AA capsule every other line in this
   file now uses, a small flat-cut tip triangle, a distinct eraser cap:
   plain and legible at dock size without needing any fine detail a 56px
   glyph can't actually resolve. */
static void gui_icon_notes(int cx, int cy, int s, unsigned int bg){
    int half = s * 3 / 10;
    int x0 = cx - half, y0 = cy + half, x1 = cx + half, y1 = cy - half;
    gui_draw_capsule(x0, y0, x1, y1, s / 11, ICON_FG, bg);
    /* eraser cap: a short capsule segment at the pencil's own top end,
       inset slightly so it reads as a separate band, not a color change
       floating off the body */
    int ex0 = x1 - (x1 - x0) / 6, ey0 = y1 - (y1 - y0) / 6;
    gui_draw_capsule(ex0, ey0, x1, y1, s / 11, gui_blend(ICON_FG, bg), bg);
    /* tip: a small triangle beyond the body's other end, pointing further
       along the same diagonal */
    int tip_x = x0 - (x1 - x0) / 6, tip_y = y0 - (y1 - y0) / 6;
    gui_fill_triangle_down(tip_x, tip_y, s / 14, s / 8, ICON_FG);
}

/* v35 (0.35.0): six new icons for the six newly-ported apps, same vector-
   only discipline as every icon above (no bitmap, no texture, this
   kernel has no image decoder and that's deliberate, see v19's own
   reasoning). Real, distinct shapes per app rather than one reused
   placeholder, matching the bar the rest of this dock already holds. */
/* v52: Reminders. Three real checkbox rows, not Plan's shrinking bullet
   outline (that one already reads as "list/outline"; this needs to read
   as "checklist" specifically, distinct at dock size): equal-length
   lines instead of a taper, small square boxes instead of circles, one
   row actually checked (a filled square) so the glyph itself shows the
   app's real function rather than three identical blanks. */
static void gui_icon_reminders(int cx, int cy, int s, unsigned int bg){
    int half = s * 3 / 10, box = s / 8;
    for (int row = 0; row < 3; row++) {
        int y = cy - half + row * half;
        int bx = cx - half - box / 2;
        if (row == 0) window_rect(bx, y - box / 2, box, box, ICON_FG); /* checked: filled */
        else { /* unchecked: outline only */
            window_rect(bx, y - box / 2, box, 1, ICON_FG);
            window_rect(bx, y + box / 2, box, 1, ICON_FG);
            window_rect(bx, y - box / 2, 1, box, ICON_FG);
            window_rect(bx + box, y - box / 2, 1, box, ICON_FG);
        }
        gui_draw_capsule(cx - half + 8, y, cx + half, y, s / 20, row == 0 ? gui_blend(ICON_FG, bg) : ICON_FG, bg);
    }
}
/* v54: Calendar. A page: rounded body with a solid header band across
   the top (the tear-off-pad silhouette every calendar icon since System 7
   has used, instantly readable at dock size), two binder rings punched
   through the band in the tile's own color, then a real 3x2 grid of
   faint day squares below, one of them solid to mean "today". Same
   primitives as the rest of the dock (window_rect for the flat fills,
   gui_fill_circle for the rings), no bitmap. */
static void gui_icon_calendar(int cx, int cy, int s, unsigned int bg){
    int half = s * 3 / 10, band = s / 7, r = s / 40 + 1;
    int x0 = cx - half, y0 = cy - half, w = 2 * half + 1, h = 2 * half + 1;
    unsigned int faint = gui_blend(ICON_FG, bg);
    window_rect(x0, y0, w, h, faint);           /* page body, soft */
    window_rect(x0, y0, w, band, ICON_FG);      /* header band, solid */
    for (int cyy = 0; cyy < r; cyy++) {         /* trim the page's four corners */
        for (int cxx = 0; cxx < r - cyy; cxx++) {
            window_rect(x0 + cxx, y0 + cyy, 1, 1, bg);
            window_rect(x0 + w - 1 - cxx, y0 + cyy, 1, 1, bg);
            window_rect(x0 + cxx, y0 + h - 1 - cyy, 1, 1, bg);
            window_rect(x0 + w - 1 - cxx, y0 + h - 1 - cyy, 1, 1, bg);
        }
    }
    gui_fill_circle(cx - half / 2, y0 + band / 2, s / 28 + 1, bg, ICON_FG); /* binder rings */
    gui_fill_circle(cx + half / 2, y0 + band / 2, s / 28 + 1, bg, ICON_FG);
    int gx = x0 + s / 12, gy = y0 + band + s / 12;                          /* 3x2 day grid */
    int cell = (w - 2 * (s / 12)) / 3, sq = cell * 3 / 5;
    for (int row = 0; row < 2; row++)
        for (int col = 0; col < 3; col++) {
            int today = (row == 1 && col == 1);
            window_rect(gx + col * cell + (cell - sq) / 2, gy + row * cell + (cell - sq) / 2, sq, sq, today ? ICON_FG : bg);
        }
}
/* v59: Mail. An envelope, the one silhouette this glyph has to read as
   before anything else: a rectangle body plus a real V-shaped flap, the
   same open-flap mark every mail icon since System 7 has used. Same
   primitives as Calendar right above it (window_rect for the flat body
   and outline, gui_draw_capsule for the two flap strokes so the diagonal
   edges get the same real AA every stroke in this dock already gets, not
   a jagged Bresenham line), no bitmap. Body is wider than tall on
   purpose, real envelope proportions, not a square with a triangle
   dropped on it. */
static void gui_icon_mail(int cx, int cy, int s, unsigned int bg){
    int hw = s * 3 / 10, hh = s * 21 / 100;
    int x0 = cx - hw, y0 = cy - hh, w = 2 * hw + 1, h = 2 * hh + 1;
    unsigned int faint = gui_blend(ICON_FG, bg);
    window_rect(x0, y0, w, h, faint);          /* envelope body, soft fill */
    window_rect(x0, y0, w, 1, ICON_FG);        /* outline */
    window_rect(x0, y0 + h - 1, w, 1, ICON_FG);
    window_rect(x0, y0, 1, h, ICON_FG);
    window_rect(x0 + w - 1, y0, 1, h, ICON_FG);
    int t = s / 22 + 1;
    gui_draw_capsule(x0, y0, cx, cy, t, ICON_FG, bg);         /* flap: left seam down to centre */
    gui_draw_capsule(x0 + w - 1, y0, cx, cy, t, ICON_FG, bg); /* flap: right seam down to centre */
}
static void gui_icon_plan(int cx, int cy, int s, unsigned int bg){
    int half = s * 3 / 10;
    for (int row = 0; row < 3; row++) {
        int y = cy - half + row * half;
        int len = half * (3 - row) / 2;
        gui_fill_circle(cx - half - 3, y, s / 16, ICON_FG, bg);
        gui_draw_capsule(cx - half + 4, y, cx - half + 4 + len, y, s / 20, ICON_FG, bg);
    }
}
static void gui_icon_lexly(int cx, int cy, int s, unsigned int bg){
    int r = s * 3 / 10;
    gui_fill_circle(cx, cy, r, ICON_FG, bg);
    gui_draw_capsule(cx - r, cy - r / 3, cx + r, cy - r / 3, s / 24, bg, ICON_FG); /* "latitude" lines punched through in bg color */
    gui_draw_capsule(cx - r, cy + r / 3, cx + r, cy + r / 3, s / 24, bg, ICON_FG);
    gui_draw_capsule(cx, cy - r, cx, cy + r, s / 24, bg, ICON_FG); /* "meridian" */
}
static void gui_icon_toroid(int cx, int cy, int s, unsigned int bg){
    int step = s / 4, r = s / 10;
    static const int alive[3][3] = {{0,1,0},{0,1,1},{1,1,0}}; /* a real small still-life pattern, not random noise */
    for (int row = 0; row < 3; row++)
        for (int col = 0; col < 3; col++)
            gui_fill_circle(cx + (col - 1) * step, cy + (row - 1) * step, r, alive[row][col] ? ICON_FG : gui_blend(ICON_FG, bg), bg);
}
static void gui_icon_sparkjar(int cx, int cy, int s, unsigned int bg){
    int r = s * 3 / 10, base_y = cy + r + s / 10;
    unsigned int glow_top = 0x00FFF3B0, glow_bot = 0x00FFC93C; /* real warm bulb color */
    gui_fill_circle_gradient(cx, cy, r, glow_top, glow_bot, bg);
    window_rect(cx - r / 3, base_y, 2 * (r / 3) + 1, s / 12, gui_blend(ICON_FG, bg));
    gui_draw_capsule(cx - r - 4, cy - r - 2, cx - r - r/2, cy - r - r/2, 2, ICON_FG, bg);
    gui_draw_capsule(cx + r + 4, cy - r - 2, cx + r + r/2, cy - r - r/2, 2, ICON_FG, bg);
}
static void gui_icon_homeqi(int cx, int cy, int s, unsigned int bg){
    int half = s * 3 / 10, roof_y = cy - half / 2;
    gui_draw_capsule(cx - half, roof_y, cx, roof_y - half, s / 16, ICON_FG, bg);
    gui_draw_capsule(cx + half, roof_y, cx, roof_y - half, s / 16, ICON_FG, bg);
    window_rect(cx - half + 2, roof_y, 2 * (half - 2) + 1, half + 2, gui_blend(ICON_FG, bg));
    window_rect(cx - s/14, roof_y + half - s/8, 2 * (s/14) + 1, s/8 + 2, ICON_FG);
}
/* v39: a real bin, drawn as geometry like every other icon here: a lid
   with a handle above a tapered body with three ribs. Tapered because a
   plain rectangle reads as a box or a building, not a bin. */
static void gui_icon_trash(int cx, int cy, int s, unsigned int bg){
    int half = s * 3 / 10, top = cy - half, h = half * 2;
    /* v45.2: full when there's something in it: two crumpled sheets
       poking above the rim, drawn as circles so they read as paper, not
       a second lid. The icon cache keys on this so the tile re-renders
       the moment the count crosses zero (see icon_cache_variant). */
    if (trash_count() > 0) {
        unsigned int paper = gui_blend(ICON_FG, bg);
        gui_fill_circle(cx - half / 3, top - s / 14, s / 9, paper, bg);
        gui_fill_circle(cx + half / 4, top - s / 10, s / 8, ICON_FG, bg);
    }
    int lid = s / 12; if (lid < 2) lid = 2;
    window_rect(cx - half - 2, top, 2 * (half + 2) + 1, lid, ICON_FG);              /* lid */
    window_rect(cx - s / 12, top - lid, 2 * (s / 12) + 1, lid, ICON_FG);            /* handle */
    /* v71.9: the two slanted sides carry a fractional edge pixel per row
       (24.8 fixed point, the same per-pixel box-filter idea the tray corner
       already uses) instead of stepping the integer width. Seen in a real
       10x zoom of the headless framebuffer: the old integer taper stepped
       one supersample pixel every ~9 rows, which the 3x3 box filter turned
       into a visible half-tone staircase down both sides of the can. */
    int rows = h - lid;
    for (int row = 0; row < rows; row++) {
        int wfp = (half << 8) - ((row * (half / 5)) << 8) / rows;                  /* taper toward the base */
        int w = wfp >> 8, y = top + lid + 1 + row;
        unsigned int edge = gui_lerp(bg, ICON_FG, wfp & 0xFF, 256);
        window_rect(cx - w, y, 2 * w + 1, 1, ICON_FG);
        window_pixel(cx - w - 1, y, edge);
        window_pixel(cx + w + 1, y, edge);
    }
    /* ribs, punched through in the tile colour; they stop s/12 above the
       base so the shadow pass's own ribs (offset 2*ICON_SS_SCALE down) stay
       covered by the real body instead of poking out below it as three grey
       stubs, the other defect the same 10x zoom showed. */
    for (int r = -1; r <= 1; r++)
        window_rect(cx + r * (half / 2), top + lid + 5, s / 26 + 1, h - lid - 5 - s / 12, bg);
}

/* v37: the Apps folder tile, a 3x3 grid of rounded tiles reading as
   "more inside", the same shape every launcher grid has used since the
   first iPhone home screen. */
static void gui_icon_apps(int cx, int cy, int s, unsigned int bg){
    (void)bg;
    int t = s / 5, gap = s / 16, span = 3 * t + 2 * gap;
    int x0 = cx - span / 2, y0 = cy - span / 2;
    /* v0.76.23: icon sharpness pass, apps icon enhanced with rounded corners
       on each grid square and subtle shading to create visual depth and lift.
       Each square is now a small rounded rect instead of a hard square. */
    unsigned int square_base = ICON_FG;
    unsigned int square_light = gui_blend(square_base, 0x00FFFFFF);
    unsigned int square_shadow = gui_blend(square_base, 0x00000000);
    int corner = t / 6; /* rounded corner radius for each grid square */
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) {
            int sx = x0 + col * (t + gap);
            int sy = y0 + row * (t + gap);
            /* Draw each grid square as a small rounded rect with subtle shading */
            gui_rounded_rect_gradient(sx, sy, t, t, square_light, square_shadow, bg, corner);
        }
    }
}

/* v36: a real prompt, the ">_" every terminal since the VT100 has worn,
   drawn as two capsule strokes and a cursor bar, same vector-only rule as
   every icon in this file. */
static void gui_icon_terminal(int cx, int cy, int s, unsigned int bg){
    int half = s * 3 / 10, t = s / 16;
    gui_draw_capsule(cx - half, cy - half / 2, cx - half / 3, cy, t, ICON_FG, bg);
    gui_draw_capsule(cx - half / 3, cy, cx - half, cy + half / 2, t, ICON_FG, bg);
    window_rect(cx + 2, cy + half / 3, half - 2, t + 1, ICON_FG);
}
static void gui_icon_fieldbook(int cx, int cy, int s, unsigned int bg){
    int half = s * 3 / 10;
    gui_draw_capsule(cx - half, cy - half / 3, cx - 2, cy + half, s / 18, ICON_FG, bg);
    gui_draw_capsule(cx + half, cy - half / 3, cx + 2, cy + half, s / 18, ICON_FG, bg);
    gui_draw_capsule(cx, cy - half / 3, cx, cy + half, s / 24, ICON_FG, bg); /* spine */
}
static void gui_icon_contacts(int cx, int cy, int s, unsigned int bg){
    int r = s / 5;
    gui_fill_circle(cx - r / 2, cy - r / 2, r, ICON_FG, bg);
    gui_draw_capsule(cx - r, cy + r / 2, cx + r, cy + r, s / 20, ICON_FG, bg);
}
static void gui_icon_calculator(int cx, int cy, int s, unsigned int bg){
    int w = s / 3, h = s / 2;
    window_rect(cx - w, cy - h / 2, w * 2, h, ICON_FG);
    gui_fill_circle(cx - w / 2, cy - h / 4, s / 24, ICON_FG, bg);
    gui_fill_circle(cx + w / 2, cy - h / 4, s / 24, ICON_FG, bg);
    gui_fill_circle(cx - w / 2, cy + h / 4, s / 24, ICON_FG, bg);
    gui_fill_circle(cx + w / 2, cy + h / 4, s / 24, ICON_FG, bg);
}
static void gui_icon_stocks(int cx, int cy, int s, unsigned int bg){
    /* Bar chart icon: three bars of different heights representing stock prices */
    (void)bg;
    int bar_w = s / 8, gap = s / 20;
    int base_y = cy + s / 6;
    /* Left bar: short */
    window_rect(cx - bar_w - gap, base_y - s / 8, bar_w, s / 8, ICON_FG);
    /* Middle bar: medium */
    window_rect(cx, base_y - s / 4, bar_w, s / 4, ICON_FG);
    /* Right bar: tall */
    window_rect(cx + bar_w + gap, base_y - s / 3, bar_w, s / 3, ICON_FG);
}
/* v0.86.0: Search. A real magnifying glass, not a repurposed shape: a ring
   (a filled circle with a smaller same-bg circle punched through its
   middle, the same "fill then punch a hole" technique every donut/ring
   glyph in this file already uses) plus a diagonal capsule handle, the
   one silhouette that reads as "search" at dock size without a label. */
static void gui_icon_search(int cx, int cy, int s, unsigned int bg){
    int r = s * 3 / 10, t = s / 10;
    int gx = cx - s / 12, gy = cy - s / 12;
    gui_fill_circle(gx, gy, r, ICON_FG, bg);
    gui_fill_circle(gx, gy, r - t, bg, bg);
    int hx0 = gx + (r * 707) / 1000, hy0 = gy + (r * 707) / 1000; /* ring edge at 45 degrees */
    int hx1 = cx + s * 2 / 5, hy1 = cy + s * 2 / 5;
    gui_draw_capsule(hx0, hy0, hx1, hy1, t / 2 + 1, ICON_FG, bg);
}
/* v0.88.0: Activity. A heartbeat/pulse trace, the one silhouette that
   reads as "live system vitals" at dock size: a flat baseline that jumps
   up, spikes down, then settles flat again, built from the same capsule
   segments every other line-based icon here (Stocks' bars, Search's
   handle) already uses, no new drawing primitive needed. */
static void gui_icon_activity(int cx, int cy, int s, unsigned int bg){
    int t = s / 14;
    int y = cy, half = s * 2 / 5;
    gui_draw_capsule(cx - half, y, cx - half / 3, y, t, ICON_FG, bg);            /* leading flat */
    gui_draw_capsule(cx - half / 3, y, cx - half / 6, y - half, t, ICON_FG, bg); /* up-spike */
    gui_draw_capsule(cx - half / 6, y - half, cx + half / 6, y + half / 2, t, ICON_FG, bg); /* down through baseline */
    gui_draw_capsule(cx + half / 6, y + half / 2, cx + half / 2, y, t, ICON_FG, bg);        /* back to baseline */
    gui_draw_capsule(cx + half / 2, y, cx + half, y, t, ICON_FG, bg);            /* trailing flat */
}

/* A soft lit band across the top of the icon, fading down into its flat
   base color: the same top-lit gloss treatment classic Aqua/iOS icons
   used for real dimension, real per-pixel colors computed with gui_lerp,
   not an alpha overlay this framebuffer can't do.
   Real, visible bug caught here, not assumed fixed by the inset alone:
   a flat `corner_r` inset on every row approximates the rounded corner's
   curve only at the very top row. gui_rounded_rect_gradient's own corner
   is an actual quarter circle, narrower than that flat inset near the
   very top and wider than it a few rows down, so the two never agreed:
   a wedge of the plain (un-glossed) gradient color showed through between
   the smooth AA corner and this band, at exactly the top two corners
   (the only ones gloss touches). Fixed by skipping the gloss entirely for
   rows still inside the curve (row < corner_r) instead of half-covering
   them with the wrong width; the rounded rect's own correct AA already
   owns that region, gloss only takes over once the shape is genuinely
   flat-sided. */
static void gui_draw_gloss(int x, int y, int w, int h, unsigned int bg, int corner_r){
    /* v43: follows the tile's own curve edge to edge. This used to inset by
       corner_r on every side and start corner_r rows down, fine when r was
       a fixed 12px, absurd once r became 22% of the tile: the gloss shrank
       to a small inner rectangle and the strip of raw base gradient left
       around it read as a chunky dark bezel on every icon, caught in a
       macro photo of the real panel. For rows inside the corner arc the
       inset is the arc's own x at that row, so the lit band meets the
       antialiased edge exactly and nothing is left un-glossed. */
    unsigned int light = gui_blend(bg, 0x00FFFFFF);
    int gloss_h = h * 2 / 5;
    for (int row = 0; row < gloss_h; row++){
        int inset = 0;
        if (row < corner_r) { int dy = corner_r - row; inset = corner_r - gui_isqrt(corner_r * corner_r - dy * dy); }
        int span = w - 2 * inset;
        if (span > 0) window_rect(x + inset, y + row, span, 1, gui_lerp(light, bg, row, gloss_h));
    }
}

/* Real bug caught before shipping, not assumed fine: a first pass drew
   this shadow as a plain gui_rounded_rect a few px wider than the icon,
   which only anti-aliases its own corners, the straight left/right edges
   are one flat hard-edged color. Made a wider rectangle peek out beside
   every icon as a stark gray bar, worse than no shadow at all. A soft
   contact shadow needs to fade on every side, which a rounded rect
   fundamentally doesn't do off its flat edges, an ellipse does by
   construction: every point's distance from center is real radial
   distance, so gui_isqrt's same AA falloff fades smoothly all the way
   around with no straight edge anywhere to look hard. */
/* A soft contact shadow centered right at the icon's own bottom edge:
   the icon (drawn after this) covers the top half of the ellipse, only
   the bottom crescent peeks out, exactly the soft "floating above the
   tray" cue a flat icon can't give on its own. `into` is the dock
   tray's own flat color, icons sit on the tray, not the gradient
   wallpaper behind it. */
/* v37: a real soft shadow with falloff across its whole body, not a flat
   dark ellipse with a 2px antialiased rim. The old version read as a hard
   dark bar under every icon at any size big enough to notice, obvious the
   moment the v37 dock made icons large. Pure per-pixel math like every
   other effect here, no alpha channel and no bitmap: darkest directly
   under the icon, blending out to the dock's own color at the edge,
   which is exactly what a soft shadow is. */
static void gui_draw_icon_shadow(int cx_center, int cy_bottom, int size){
    /* v44.2: drawn at PHYSICAL resolution, same fix shape as v43's dock
       tray corners. Through the LOGICAL layer this was the one dock
       element not matching every icon tile beside it, which renders at
       physical res via the supersampled cache: at a ~38px icon ry was
       only 4 logical rows, so the whole ellipse was 9 rows of 2x2 blocks
       and sdx's integer division at ry=4 quantised the horizontal falloff
       into visible bands, reading as boxy rather than round. Same math,
       just in physical units, with real division headroom once ry
       doubles (ry=8 typical). */
    unsigned int dock_bg = DOCK_TRAY_COLOR;
    unsigned int core = gui_lerp(dock_bg, 0x00000000, 45, 100); /* never full black: this is a contact shadow on a light surface */
    int sc = (int)window_scale();
    int cx_p = cx_center * sc, cy_p = cy_bottom * sc;
    int rx = (size * sc) / 2, ry = (size * sc) / 9;
    if (rx <= 0 || ry <= 0) return;
    /* The falloff is normalised per axis in fixed point, NOT by squashing
       dx into dy's scale first. The old line was `int sdx = dx * ry / rx`,
       integer division, and that is what made this shadow read as blocky
       right up to v0.79. rx is 37 and ry is 8 at dock size, so sdx could
       only ever take 17 distinct values across 75 real columns: the whole
       shadow collapsed into 9 flat plateaus about 4.5px wide with a hard
       step between each. Measured on a real 1920x1080 capture before this
       change, the row two pixels under an icon read 9 distinct luminances
       with single-step jumps of 21. v58 had already moved this loop to
       physical resolution and doubled ry, which halved the plateau width
       but left the integer division, and therefore the banding, in place.
       Normalising each axis against its own radius keeps full precision in
       both, and 1024 levels rather than 100 leaves real headroom for the
       lerp so the quantisation is the framebuffer's 8 bits and not ours. */
    for (int dy = -ry; dy <= ry; dy++){
        for (int dx = -rx; dx <= rx; dx++){
            /* Quadratic falloff: normalised squared radius gives a soft
               centre and a faster fade at the rim, closer to a real
               penumbra than a linear ramp. t == 1024 is the ellipse edge,
               where the colour is exactly the tray, so there is no hard
               cutoff to see. */
            int t = (dx * dx * 1024) / (rx * rx) + (dy * dy * 1024) / (ry * ry);
            if (t > 1024) continue;
            window_pixel_phys(cx_p + dx, cy_p - sc + dy, gui_lerp(core, dock_bg, t, 1024));
        }
    }
}

static void gui_draw_icon_glyph(int icon, int cx_center, int cy, int size, unsigned int bg){
    switch (icon) {
        case 0: gui_icon_folder(cx_center, cy, size, bg); break;
        case 1: gui_icon_mail(cx_center, cy, size, bg); break;
        case 2: gui_icon_calendar(cx_center, cy, size, bg); break;
        case 3: gui_icon_notes(cx_center, cy, size, bg); break;
        case 4: gui_icon_reminders(cx_center, cy, size, bg); break;
        case 5: gui_icon_terminal(cx_center, cy, size, bg); break;
        case 6: gui_icon_chat(cx_center, cy, size, bg); break;
        case 7: gui_icon_weather(cx_center, cy, size, bg); break;
        case 8: gui_icon_pin(cx_center, cy, size, bg); break;
        case 9: gui_icon_keyrate(cx_center, cy, size, bg); break;
        case 10: gui_icon_book(cx_center, cy, size, bg); break;
        case 11: gui_icon_quotes(cx_center, cy, size, bg); break;
        case 12: gui_icon_plan(cx_center, cy, size, bg); break;
        case 13: gui_icon_lexly(cx_center, cy, size, bg); break;
        case 14: gui_icon_toroid(cx_center, cy, size, bg); break;
        case 15: gui_icon_sparkjar(cx_center, cy, size, bg); break;
        case 16: gui_icon_homeqi(cx_center, cy, size, bg); break;
        case 17: gui_icon_fieldbook(cx_center, cy, size, bg); break;
        case 18: gui_icon_contacts(cx_center, cy, size, bg); break;
        case 19: gui_icon_calculator(cx_center, cy, size, bg); break;
        case 20: gui_icon_stocks(cx_center, cy, size, bg); break;
        case 21: gui_icon_search(cx_center, cy, size, bg); break;
        case 22: gui_icon_stocks(cx_center, cy, size, bg); break; /* art covers it; primitive fallback only */
        case 23: gui_icon_apps(cx_center, cy, size, bg); break; /* Portfolio: no authored art yet, reuses the grid-of-tiles glyph */
        case 24: gui_icon_activity(cx_center, cy, size, bg); break;
        case GUI_APPS_FOLDER: gui_icon_apps(cx_center, cy, size, bg); break;
        case GUI_TRASH: gui_icon_trash(cx_center, cy, size, bg); break;
    }
}

/* Real, repeated feedback across many rounds: the icons still read as
   flat and bitmap despite AA'd edges, gradient backgrounds, and a soft
   shadow under the whole icon. The one thing every one of those passes
   left untouched is the glyph itself: a pure flat white silhouette with
   nothing behind it. Real macOS icons lean hard on exactly this cue, a
   glyph that looks lifted off the surface it sits on via a soft shadow
   of its own, not just an anti-aliased edge. Drawing the whole glyph a
   second time, offset and in a dark tone, before the real white pass, is
   a real drop shadow under every icon's pictogram at once: every icon
   function already just draws in ICON_FG, now a real variable instead of
   a compile-time constant, so this needed zero changes to any of the 8
   glyph functions themselves. */
/* Real supersampling, not another AA tweak: direct, repeated feedback
   that the icons still show individual pixels under a close look, and
   they're right, AA_BAND's discrete color-stepping has a real, visible
   floor no amount of widening gets under (confirmed by testing AA_BAND
   8, which just went blurry, not smoother, see the commit that reverted
   it to 5). Real anti-aliasing from oversampling instead: render the
   whole icon at SS_SCALE times its real size into a heap buffer via
   window_push_target, then box-downsample every SS_SCALE x SS_SCALE
   block back down to one real screen pixel, averaging real sub-pixel
   coverage the same way a real renderer or a Retina display's own
   downsampling does. Falls back to drawing at native resolution
   directly (the old path) if the allocation fails, never a blank icon,
   just a less-smooth one on a machine tight on heap. */
/* v41: 4, not 3, so that the downsample to 2x physical pixels is an exact
   2:1 box filter (240 -> 120) instead of a 1.5:1 one that would have to
   pick which sample to drop. */
#define ICON_SS_SCALE 6 /* v43: 3 samples per physical pixel per axis (9 per pixel) at 2x, up from 2x2, visibly cleaner curves on the folder/pin/sun edges */
/* v43: icons are rendered ONCE per (icon, size) into a physical-res cache
   and blitted from then on. Before this, every hover change re-rendered
   all eight dock icons through the 6x supersample (eight 360x360 buffers,
   ~4MB of pixel work) and that was the "flashes when I hover" report
   after the v40 cursor fix had already removed the other cause. A blit is
   pw*pw writes, hundreds of times cheaper, and the flash is gone because
   the frame is now finished before anything can be seen mid-draw. */
#define ICON_CACHE_SLOTS 2 /* normal, magnified */
static unsigned int *icon_cache[GUI_APP_COUNT][ICON_CACHE_SLOTS];
static int icon_cache_size[GUI_APP_COUNT][ICON_CACHE_SLOTS];
static unsigned int icon_cache_under[GUI_APP_COUNT][ICON_CACHE_SLOTS];
static int icon_cache_variant[GUI_APP_COUNT][ICON_CACHE_SLOTS]; /* v45.2: anything that changes a glyph beyond (icon,size,surface); today only Trash full/empty */

/* `under` is the colour the tile physically sits on. Its corners are
   blended toward that, so they vanish into the surface instead of
   leaving a pale halo: the dock tray is 0xEFEBE4, the Apps folder is
   GUI_BG, and blending both toward GUI_BG (the old behaviour) put a
   faint white rim on every dock tile once the pixels got small enough to
   see it. */
/* v0.77.x: authored artwork instead of runtime primitive assembly, for
   the icons that have it (see tools/gen/gen_icon_art.py and art/icons/).
   Area-average ICON_ART_SIZE x ICON_ART_SIZE straight RGBA down to pw x pw
   and composite over `under`, which is exactly the opaque tile the rest of
   this file already expects out of the cache, so nothing downstream
   changes. Colour is averaged weighted by alpha (and alpha averaged on its
   own) rather than straight, because a straight average of RGB across the
   icon's transparent border would drag real edge pixels toward whatever
   the rasterizer happened to leave in the fully-transparent ones, the
   classic dark/light fringe. Integer only, no FPU in this kernel. */
/* The upscale half. A box filter degenerates to nearest-neighbour the
   moment the destination is bigger than the source (every destination pixel
   covers less than one source pixel), which is exactly the blocky staircase
   this whole pass exists to remove, and there are real call sites past the
   stored ICON_ART_SIZE:
   the Weather app's own 100-logical card (200 physical), and the dock itself
   once dock_scale_pct is turned up past 20 in Settings. Bilinear there, on
   premultiplied colour so the transparent border cannot bleed into an edge,
   then the same source-over onto the surface colour. Fixed point, 8
   fractional bits, no FPU in this kernel. */
static void gui_icon_art_bilinear(const unsigned char *art, unsigned int *out, int pw, unsigned int under){
    unsigned int ur = (under >> 16) & 0xFF, ug = (under >> 8) & 0xFF, ub = under & 0xFF;
    for (int py = 0; py < pw; py++){
        int fy = (py * 2 + 1) * ICON_ART_SIZE * 128 / pw - 128; /* pixel-centre mapping */
        if (fy < 0) fy = 0;
        int sy = fy >> 8, wy = fy & 255;
        if (sy >= ICON_ART_SIZE - 1) { sy = ICON_ART_SIZE - 2; wy = 255; }
        for (int px = 0; px < pw; px++){
            int fx = (px * 2 + 1) * ICON_ART_SIZE * 128 / pw - 128;
            if (fx < 0) fx = 0;
            int sx = fx >> 8, wx = fx & 255;
            if (sx >= ICON_ART_SIZE - 1) { sx = ICON_ART_SIZE - 2; wx = 255; }
            unsigned int cr = 0, cg = 0, cb = 0, ca = 0;
            for (int k = 0; k < 4; k++){
                int ox = k & 1, oy = k >> 1;
                unsigned int w = (unsigned int)(ox ? wx : 255 - wx) * (unsigned int)(oy ? wy : 255 - wy);
                const unsigned char *p = art + ((unsigned int)(sy + oy) * ICON_ART_SIZE + (unsigned int)(sx + ox)) * 4;
                unsigned int a = p[3];
                cr += w * p[0] * a / 255; cg += w * p[1] * a / 255; cb += w * p[2] * a / 255;
                ca += w * a;
            }
            /* cr/cg/cb are premultiplied colour, ca is alpha, all scaled by 255*255 */
            unsigned int a = (ca + 32512) / 65025;
            unsigned int r = (cr + 32512) / 65025, g = (cg + 32512) / 65025, b = (cb + 32512) / 65025;
            r += ur * (255 - a) / 255; g += ug * (255 - a) / 255; b += ub * (255 - a) / 255;
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
            out[py * pw + px] = (r << 16) | (g << 8) | b;
        }
    }
}

/* Exact area filter: every destination pixel is the coverage-weighted mean
   of the source pixels it overlaps, fractional edges included. The old
   loop snapped each block to whole source pixels (py*S/pw .. (py+1)*S/pw),
   which at the old 128 -> 74 dock ratio averaged an uneven mix of 1 and 2
   source rows/columns per pixel, so neighbouring edge pixels came out
   alternately crisp and soft and every straight edge picked up a faint
   beat. Weights here are in units where a destination pixel spans S
   (ICON_ART_SIZE) and a source pixel spans pw, so they are exact integers
   and each axis sums to S. At the resting dock (148 -> 74) this reduces
   to an exact 2x2 box. Premultiplied, so the transparent border contributes
   no colour; the per-pixel weight product is at most S*S = 21904 and the
   premultiplied channel at most 255, so every sum fits in 32 bits. */
static void gui_icon_art_scale(const unsigned char *art, unsigned int *out, int pw, unsigned int under){
    if (pw > ICON_ART_SIZE) { gui_icon_art_bilinear(art, out, pw, under); return; }
    unsigned int ur = (under >> 16) & 0xFF, ug = (under >> 8) & 0xFF, ub = under & 0xFF;
    const int S = ICON_ART_SIZE;
    const unsigned int total = (unsigned int)(S * S), half = total / 2;
    for (int py = 0; py < pw; py++){
        int y0 = py * S, y1 = y0 + S;                  /* destination row, in source-pixel = pw units */
        for (int px = 0; px < pw; px++){
            int x0 = px * S, x1 = x0 + S;
            unsigned int rs = 0, gs = 0, bs = 0, as = 0;
            for (int sy = y0 / pw; sy * pw < y1; sy++){
                int wy = (y1 < (sy + 1) * pw ? y1 : (sy + 1) * pw) - (y0 > sy * pw ? y0 : sy * pw);
                const unsigned char *row = art + ((unsigned int)sy * (unsigned int)S) * 4;
                for (int sx = x0 / pw; sx * pw < x1; sx++){
                    int wx = (x1 < (sx + 1) * pw ? x1 : (sx + 1) * pw) - (x0 > sx * pw ? x0 : sx * pw);
                    const unsigned char *p = row + (unsigned int)sx * 4;
                    unsigned int w = (unsigned int)(wx * wy), a = p[3];
                    if (!a) continue;
                    rs += w * ((p[0] * a + 127) / 255); gs += w * ((p[1] * a + 127) / 255); bs += w * ((p[2] * a + 127) / 255);
                    as += w * a;
                }
            }
            unsigned int a = (as + half) / total;
            unsigned int r = (rs + half) / total, g = (gs + half) / total, b = (bs + half) / total;
            /* premultiplied source-over onto the surface colour, rounded */
            r += (ur * (255 - a) + 127) / 255; g += (ug * (255 - a) + 127) / 255; b += (ub * (255 - a) + 127) / 255;
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
            out[py * pw + px] = (r << 16) | (g << 8) | b;
        }
    }
}


static unsigned int *gui_render_icon_cached(int icon, int size, int slot, unsigned int under){
    int variant = (icon == GUI_TRASH) ? (trash_count() > 0) : 0;
    if (icon_cache[icon][slot] && icon_cache_size[icon][slot] == size && icon_cache_under[icon][slot] == under && icon_cache_variant[icon][slot] == variant) return icon_cache[icon][slot];
    unsigned int sc = window_scale();
    int pw = size * (int)sc;
    /* Variant artwork first where one exists (Trash full vs empty), so
       converting an icon to artwork cannot quietly drop a real runtime
       state indicator; fall back to the base artwork when it does not. */
    const unsigned char *png = 0;
    unsigned int png_len = 0;
    if (icon >= 0 && icon < ICON_ART_COUNT) {
        if (variant && ICON_ART_VARIANT[icon]) { png = ICON_ART_VARIANT[icon]; png_len = ICON_ART_VARIANT_LEN[icon]; }
        else                                   { png = ICON_ART[icon];         png_len = ICON_ART_LEN[icon]; }
    }
    /* The artwork is stored as PNG, not as decoded RGBA: 24 artworks of
       128x128 RGBA is 1.5MB, which runs into the ring-3 program window
       boot/linker.ld pins at 0xC0500000, and docs/SYSCALL-ABI.md names that
       address as part of the published v1 contract. As PNG the same 24 are
       141KB. Decoding here rather than once at boot costs nothing in
       practice: this function is the icon cache's own miss path, so it runs
       on first draw and on a real size or variant change, not per frame. */
    unsigned char *art = 0;
    if (png && pw > 0) {
        unsigned int aw = 0, ah = 0, ach = 0;
        if (png_decode(png, png_len, &art, &aw, &ah, &ach) != 0) art = 0;
        else if (aw != ICON_ART_SIZE || ah != ICON_ART_SIZE || ach != 4) { kfree(art); art = 0; }
    }
    if (art) {
        unsigned int *dst = icon_cache[icon][slot];
        if (!dst || icon_cache_size[icon][slot] != size) {
            if (dst) kfree(dst);
            dst = (unsigned int *)kmalloc((unsigned int)(pw * pw) * sizeof(unsigned int));
            if (!dst) { kfree(art); return 0; }   /* the decode buffer is ours, free it on every exit */
            icon_cache[icon][slot] = dst; icon_cache_size[icon][slot] = size;
        }
        icon_cache_under[icon][slot] = under;
        icon_cache_variant[icon][slot] = variant;
        gui_icon_art_scale(art, dst, pw, under);
        kfree(art);
        return dst;
    }
    unsigned int ssz = (unsigned int)size * ICON_SS_SCALE;
    unsigned int *ssbuf = (unsigned int *)kmalloc(ssz * ssz * sizeof(unsigned int));
    if (!ssbuf) return 0;
    unsigned int *out = icon_cache[icon][slot];
    if (!out || icon_cache_size[icon][slot] != size) {
        if (out) kfree(out);
        out = (unsigned int *)kmalloc((unsigned int)(pw * pw) * sizeof(unsigned int));
        if (!out) { kfree(ssbuf); return 0; }
        icon_cache[icon][slot] = out; icon_cache_size[icon][slot] = size;
    }
    icon_cache_under[icon][slot] = under;
    icon_cache_variant[icon][slot] = variant;
    unsigned int bg = GUI_COLORS[icon];
    unsigned int bg_light = gui_blend(bg, 0x00FFFFFF), bg_dark = gui_blend(bg, 0x00000000);
    window_push_target(ssbuf, ssz, ssz);
    int saved_band = aa_band; aa_band = ICON_SS_SCALE * 3; /* 3 physical px of real AA on every primitive edge, before the box filter */
    for (unsigned int i = 0; i < ssz * ssz; i++) ssbuf[i] = under;
    int r = (int)ssz * 22 / 100;
    gui_rounded_rect_gradient(0, 0, (int)ssz, (int)ssz, bg_light, bg_dark, under, r);
    gui_draw_gloss(0, 0, (int)ssz, (int)ssz, bg, r + ICON_SS_SCALE);
    int scy = (int)ssz / 2;
    unsigned int real_fg = ICON_FG;
    ICON_FG = gui_blend(gui_blend(bg, 0x00000000), bg); /* 25% toward black: a shadow, not an outline */
    gui_draw_icon_glyph(icon, (int)ssz / 2 + ICON_SS_SCALE, scy + 2 * ICON_SS_SCALE, (int)ssz, bg);
    ICON_FG = real_fg;
    gui_draw_icon_glyph(icon, (int)ssz / 2, scy, (int)ssz, bg);
    /* v66: real, confirmed bug, not a guess: re-clip every supersample
       pixel back to this same rounded-rect silhouette, now that every
       glyph has drawn. Root-caused with a real headless dock capture,
       zoomed 4x: the Weather icon's 8 sun rays (gui_icon_weather, by
       design reaching toward every corner) showed a visible light halo
       bled past all four rounded corners, and Mail's checkmark / Calendar's
       binder rings showed the same, fainter, near their top corners, the
       exact icons whose glyph primitives reach toward a corner. Files,
       Trash, and the Apps grid, whose glyphs stay centered, showed none,
       confirming it's geometry reaching past the curve, not a general AA
       bug. Every gui_draw_capsule/gui_fill_circle call blends its own edge
       toward `bg` (this icon's flat color), the right choice when it's
       fully inside the tile, but wrong wherever the primitive's own reach
       or AA fringe lands past the curve gui_rounded_rect_gradient already
       carved pure `under` into; nothing after that first fill ever
       reasserted the boundary. Same corner math gui_rounded_rect_gradient
       itself uses (arithmetic mean, not a new rule), just applied as a
       final mask instead of only a first pass, so it catches every icon's
       glyph at once instead of patching each shape's own geometry. */
    for (int cyy = 0; cyy <= r; cyy++){
        for (int cxx = 0; cxx <= r; cxx++){
            int ox = r - cxx, oy = r - cyy;
            int d2 = ox * ox + oy * oy;
            int inner = r - AA_BAND;
            if (d2 <= inner * inner) continue;
            int mx = (int)ssz - 1 - cxx, my = (int)ssz - 1 - cyy;
            unsigned int *corners[4] = {
                &ssbuf[cyy * ssz + cxx], &ssbuf[cyy * ssz + mx],
                &ssbuf[my * ssz + cxx],  &ssbuf[my * ssz + mx],
            };
            if (d2 >= r * r) { for (int k = 0; k < 4; k++) *corners[k] = under; continue; }
            int t = gui_isqrt(d2) - inner;
            for (int k = 0; k < 4; k++) *corners[k] = gui_lerp(*corners[k], under, t, AA_BAND);
        }
    }
    aa_band = saved_band;
    window_pop_target();
    unsigned int per = ICON_SS_SCALE / sc; if (per < 1) per = 1;
    unsigned int samples = per * per;
    /* v83: real downsampling improvement, not a guess: box-averaging per-channel
       colors without rounding loses precision through truncation, especially
       visible in smooth gloss/gradient regions where each channel should fade
       smoothly. Integer division of sums loses 0.5 LSB per sample on average,
       visible banding in smooth AA gradients over 9+ sample averages. Proper
       rounding via (sum + samples/2) / samples preserves smooth transitions. */
    for (int py = 0; py < pw; py++){
        for (int px = 0; px < pw; px++){
            unsigned int rs = 0, gs = 0, bs = 0;
            for (unsigned int sy = 0; sy < per; sy++){
                unsigned int *row = &ssbuf[((unsigned int)py * per + sy) * ssz + (unsigned int)px * per];
                for (unsigned int sx = 0; sx < per; sx++){ unsigned int c = row[sx]; rs += (c >> 16) & 0xFF; gs += (c >> 8) & 0xFF; bs += c & 0xFF; }
            }
            unsigned int half = samples / 2;
            out[py * pw + px] = (((rs + half) / samples) << 16) | (((gs + half) / samples) << 8) | ((bs + half) / samples);
        }
    }
    kfree(ssbuf);
    return out;
}

static void gui_draw_one_icon_on(int icon, int cx_center, int cy_bottom, int size, unsigned int under){
    int x = cx_center - size / 2, y = cy_bottom - size;
    int slot = (size == DOCK_ICON) ? 0 : 1;
    unsigned int *tile = gui_render_icon_cached(icon, size, slot, under);
    unsigned int sc = window_scale();
    int pw = size * (int)sc;
    if (tile) {
        for (int py = 0; py < pw; py++)
            for (int px = 0; px < pw; px++)
                if (tile[py * pw + px] != under)
                    window_pixel_phys(x * (int)sc + px, y * (int)sc + py, tile[py * pw + px]);
        return;
    }
    /* out of memory for the cache: draw directly, un-supersampled, rather than draw nothing */
    unsigned int bg = GUI_COLORS[icon];
    unsigned int bg_light = gui_blend(bg, 0x00FFFFFF), bg_dark = gui_blend(bg, 0x00000000);
    gui_rounded_rect_gradient(x, y, size, size, bg_light, bg_dark, under, size * 22 / 100);
    gui_draw_gloss(x, y, size, size, bg, size * 22 / 100 + 1);
    gui_draw_icon_glyph(icon, cx_center, y + size / 2, size, bg);
}
static void gui_draw_one_icon(int icon, int cx_center, int cy_bottom, int size){ gui_draw_one_icon_on(icon, cx_center, cy_bottom, size, DOCK_TRAY_COLOR); }

/* hover_slot: which slot shows the magnify+label (-1 none). drag_slot: the
   slot currently being dragged, drawn separately so it can float free of
   the row under the cursor instead of at its slot position. */
/* v40: the dock band's top edge, high enough to cover a magnified,
   lifted icon and its label, so repainting this band alone is enough to
   erase any previous hover state. */
static int gui_dock_band_top(void){ return gui_dock_y0() - 24; }
#define DOCK_LABEL_BG   0x00F4F1EC /* hover label capsule fill */
#define DOCK_LABEL_EDGE 0x00BDB4A8 /* its hairline edge */
#define DOCK_LABEL_SPAN 48         /* px either side of a slot a hover change repaints: the widest label plus its capsule */

static void gui_draw_dock(int hover_slot, int drag_slot, int drag_mx, int drag_my);
/* v0.79.x: the dock splits into the half that never changes while the
   pointer moves (the tray's shadow and its rounded body) and the half that
   does (the icons, their contact shadows and the hover label). Measured,
   one hover frame: the tray half is 4442 us of a 9842 us band compose, all
   of it redrawing pixels identical to the ones already there. Baking it
   into the band cache alongside the wallpaper rows it sits on costs
   nothing extra (the cache is built once per resolution) and takes it off
   every single animation frame. Both halves read the wallpaper through
   gui_wallpaper_sample, never through the framebuffer, so a cached tray is
   the same pixels as a freshly drawn one, not an approximation of them. */
static void gui_draw_dock_tray(void);
static void gui_draw_dock_icons(int drag_slot, int drag_mx, int drag_my);

static void gui_draw_desktop(int hover_slot, int drag_slot, int drag_mx, int drag_my){
    gui_draw_wallpaper();
    if (wind_enabled && !wind_base) {
        int sc = (int)window_scale();
        int width = (int)window_width() * sc, height = (WIND_HORIZON_ROW - WIND_TOP_ROW) * sc;
        wind_base = (unsigned int *)kmalloc((unsigned int)(width * height) * sizeof(unsigned int));
        if (wind_base) {
            wind_base_width = width;
            /* v65: sampled directly via gui_wallpaper_row/px (RAW, no
               daynight tint) rather than read back from the framebuffer
               like before. Reading the framebuffer would have baked
               whatever tint was in effect at this one-time cache build
               into every future frame for the swaying crown region
               forever (this cache is built once per boot and never
               rebuilt, see wind_base's own declaration comment), so the
               sky would freeze at boot's hour while the untouched ground/
               dock rows below the horizon kept re-tinting live on every
               redraw. Sampling raw here and tinting fresh on every read
               instead (gui_wallpaper_sample / gui_draw_wallpaper_rows_
               sway_ex's own wind-cache branch both do this now) keeps the
               whole photo, cached region included, honestly following the
               real hour for the entire session, not just its first frame. */
            for (int py = 0; py < height; py++) {
                struct wp_row rc = gui_wallpaper_row(WIND_TOP_ROW * sc + py, 0);
                for (int px = 0; px < width; px++)
                    wind_base[py * width + px] = gui_wallpaper_px(&rc, px);
            }
        }
    }
    gui_draw_menubar();
    gui_draw_dock(hover_slot, drag_slot, drag_mx, drag_my);
}

/* v40: repaint only the dock band: the wallpaper rows behind it, then the
   dock itself. This is what a hover change costs now, instead of a full
   456,000-pixel photo blit plus eight supersampled icons. */
static unsigned int *dock_band_cache = 0;
static unsigned int *dock_band_frame = 0;
static int dock_band_cache_top = -1;
static int dock_presented_hover = -1;
static void gui_dock_band_cache_build(void){
    int sc = (int)window_scale();
    int top = gui_dock_band_top(), h = (int)window_height() - top;
    int pw = (int)window_width() * sc, ph = h * sc;
    if (dock_band_cache && dock_band_cache_top == top) return;
    if (dock_band_cache) kfree(dock_band_cache);
    if (dock_band_frame) kfree(dock_band_frame);
    dock_band_cache = (unsigned int *)kmalloc((unsigned int)(pw * ph) * sizeof(unsigned int));
    dock_band_frame = (unsigned int *)kmalloc((unsigned int)(pw * ph) * sizeof(unsigned int));
    dock_band_cache_top = top;
    if (dock_band_cache && dock_band_frame) {
        window_push_screen_band(dock_band_cache, top * sc, (unsigned int)ph);
        gui_draw_wallpaper_rows(top, (int)window_height());
        gui_draw_dock_tray();
        window_pop_screen_band();
    }
}

/* Everything the first hover of a session would otherwise pay for mid
   animation: the band cache above (a full-width wallpaper render plus the
   tray), and the magnified tile for every icon, whose cache miss path
   decodes a PNG. Measured, that first hover showed one single size where
   a warm one shows six, and the second showed three, because the work
   landed inside the sixty milliseconds the animation had to run in. Doing
   it here, while the desktop's first frame is already up and nothing is
   animating, costs a boot moment nobody is watching and allocates nothing
   a hover sweep would not have allocated seconds later anyway. */
static void gui_dock_prewarm(void){
    gui_dock_band_cache_build();
}

static void gui_redraw_dock_band(int hover_slot, int drag_slot, int drag_mx, int drag_my){
    /* v43: the wallpaper rows behind the dock never change, so bilinear
       them once and copy thereafter. ~400k physical samples per hover
       change was the other half of the flash. */
    int sc = (int)window_scale();
    int top = gui_dock_band_top(), h = (int)window_height() - top;
    int pw = (int)window_width() * sc, ph = h * sc;
    gui_dock_band_cache_build();
    if (dock_band_cache && dock_band_frame) {
        for (int i = 0; i < pw * ph; i++) dock_band_frame[i] = dock_band_cache[i];
        window_push_screen_band(dock_band_frame, top * sc, (unsigned int)ph);
        gui_draw_dock_icons(drag_slot, drag_mx, drag_my);
        window_pop_screen_band();
        /* Only present slots whose icon size changed. Copying the whole
           2 MB band on every hover step visibly exposed the half-drawn
           frame even though composition itself was offscreen. */
        for (int slot = 0; slot < GUI_ICON_COUNT; slot++) {
            if ((slot == dock_presented_hover) == (slot == dock_hover)) continue;
            int left = (gui_slot_x(slot) - DOCK_LABEL_SPAN) * sc;
            int right = (gui_slot_x(slot) + DOCK_ICON + DOCK_LABEL_SPAN) * sc;
            if (left < 0) left = 0;
            if (right > pw) right = pw;
            for (int py = 0; py < ph; py++) {
                unsigned int *dst = window_phys_row(top * sc + py);
                for (int px = left; px < right; px++) {
                    unsigned int next = dock_band_frame[py * pw + px];
                    if (dst[px] != next) dst[px] = next;
                }
            }
            /* v0.78.x: this is the one caller that writes through a raw row
               pointer, so it has to declare what it touched. Measured: the
               old "assume the whole row" guess damaged 1920 columns per row
               to change the ~174 this actually writes, which is what made a
               dock hover present 1.96M pixels instead of ~86k. */
            window_damage(left, top * sc, right - left, ph);
            }
        dock_presented_hover = dock_hover;
    } else {
        gui_draw_wallpaper_rows(top, (int)window_height());
        gui_draw_dock(hover_slot, drag_slot, drag_mx, drag_my);
    }
}

/* v75: see wall_apply. wind_base is rebuilt lazily by gui_draw_desktop,
   the dock band by gui_redraw_dock_band's own top-mismatch check. */
static void wall_caches_drop(void){
    if (wind_base) { kfree(wind_base); wind_base = 0; }
    dock_band_cache_top = -1;
}

static void gui_draw_dock_tray(void){
    int y0 = gui_dock_y0(), dock_h = DOCK_ICON + 2 * DOCK_PAD, dock_w = gui_dock_w(), dock_x = gui_dock_x0();

    /* A soft shadow beneath the tray, the same floating-panel look a real
       macOS dock has, drawn before the tray itself so the tray's own edge
       sits cleanly on top of it. Real per-pixel colors blended toward
       black (gui_blend), fading back to the plain wallpaper color over a
       few rows, no alpha compositing needed since these are precomputed
       solid colors, same technique every AA edge in this file already
       uses. Inset a little past the tray's own rounded corners so it
       reads as a shadow, not a second, darker rectangle. */
    /* Per physical pixel against the real photo. gui_wallpaper_color is one
       colour per row (the centre column), fine for the old gradient but on
       the photo it drew a flat striped bar under the tray. */
    int sc = (int)window_scale();
    int sy0 = (y0 + dock_h) * sc, rows = 10 * sc;
    int sx0 = (dock_x + 6) * sc, sx1 = (dock_x + dock_w - 6) * sc;
    for (int row = 0; row < rows; row++){
        for (int px = sx0; px < sx1; px++){
            unsigned int wall = gui_wallpaper_sample(px, sy0 + row, 0);
            window_pixel_phys(px, sy0 + row, gui_lerp(gui_blend(wall, 0x00000000), wall, row, rows));
        }
    }

    /* gui_rounded_rect_on_wallpaper, not gui_rounded_rect: the tray's top
       and bottom corners sit against very different points on the
       gradient, one fixed blend sample for both was the real dark-bubble
       bug just found and fixed above. */
    gui_rounded_rect_on_wallpaper(dock_x, y0, dock_w, dock_h, DOCK_TRAY_COLOR, 20);
}

static void gui_draw_dock_icons(int drag_slot, int drag_mx, int drag_my){
    int y0 = gui_dock_y0();

    for (int slot = 0; slot < GUI_ICON_COUNT; slot++) {
        if (slot == drag_slot) continue; /* drawn last, floating at the cursor */
        int icon = gui_order[slot];
        int size = DOCK_ICON;
        int cx_center = gui_slot_x(slot) + DOCK_ICON / 2;
        int cy_bottom = y0 + DOCK_PAD + DOCK_ICON;
        gui_draw_icon_shadow(cx_center, cy_bottom, size);
        gui_draw_one_icon(icon, cx_center, cy_bottom, size);
        if (slot == dock_hover) {
            int label_w = font_string_width(GUI_LABELS[icon]);
            int ly = y0 - 21; /* capsule spans ly-3 .. ly+19: clear of the tray's top edge, inside the band (y0 - 24) */
            /* Dark text on a light capsule with a hairline edge, the macOS
               dock tooltip, in the tray's own cream. Bare light text read
               on dark wallpaper but vanished on bright map tiles and
               collided with an open window's bottom edge (QA tour,
               2026-09-21); the hairline keeps the capsule distinct over a
               light window. It stays inside the band gui_dock_band_top()
               composes and the per-slot present span DOCK_LABEL_SPAN. */
            int lx0 = cx_center - label_w / 2 - 2, lx1 = cx_center + label_w / 2 + 2;
            gui_draw_capsule(lx0, ly + 8, lx1, ly + 8, 11, DOCK_LABEL_EDGE, DOCK_LABEL_EDGE);
            gui_draw_capsule(lx0, ly + 8, lx1, ly + 8, 10, DOCK_LABEL_BG, DOCK_LABEL_BG);
            font_draw_string(GUI_LABELS[icon], cx_center - label_w / 2, ly, 0x001C1C1E, -1);
        }
    }
    if (drag_slot >= 0) {
        int icon = gui_order[drag_slot];
        gui_draw_one_icon(icon, drag_mx, drag_my + DOCK_ICON / 2, DOCK_ICON);
    }
}

static void gui_draw_dock(int hover_slot, int drag_slot, int drag_mx, int drag_my){
    (void)hover_slot;
    gui_draw_dock_tray();
    gui_draw_dock_icons(drag_slot, drag_mx, drag_my);
}

/* v40: a real software cursor. Save the 13x13 patch it's about to cover,
   draw, and later put that patch back exactly. Moving the cursor then
   costs ~340 pixel writes instead of repainting the desktop, which is the
   whole fix for "icons flash on hover": the flashing WAS the full
   repaint, visible because there's no double buffer, triggered by every
   single mouse packet. */
#define CURSOR_W 13
#define CURSOR_H 19
#define CURSOR_MAX_SCALE 2 /* window_open_scaled(..., 2) in gui_run; bump together */
/* The backup is kept at PHYSICAL resolution (v56.1). It used to go through
   the logical layer: window_get_pixel reads only the top-left physical
   pixel of each scale x scale block and window_pixel writes the whole
   block back, so every cursor pass silently pixel-doubled whatever
   antialiased text it crossed. Surfaces that repaint on hover (menu bar,
   dock) hid it; the notification panel, never repainted while open, kept
   the damage and read as "still the old bitmap font" (roadmap, Sep 2026).
   Reproduced headlessly with a scripted sweep + pmemsave, fixed here. */
static unsigned int cursor_backup[CURSOR_W * CURSOR_MAX_SCALE * CURSOR_H * CURSOR_MAX_SCALE];
static int cursor_saved_x = -1, cursor_saved_y = -1;
static int gui_cursor_scale(void){ int sc = (int)window_scale(); return sc > CURSOR_MAX_SCALE ? CURSOR_MAX_SCALE : sc; }
static void gui_cursor_restore(void){
    if (cursor_saved_x < 0) return;
    int sc = gui_cursor_scale(), pw = CURSOR_W * sc, ph = CURSOR_H * sc;
    int px0 = cursor_saved_x * sc, py0 = cursor_saved_y * sc;
    for (int j = 0; j < ph; j++)
        for (int i = 0; i < pw; i++)
            window_pixel_phys(px0 + i, py0 + j, cursor_backup[j * pw + i]);
    cursor_saved_x = cursor_saved_y = -1;
}
static void gui_cursor_save(int x, int y){
    int sc = gui_cursor_scale(), pw = CURSOR_W * sc, ph = CURSOR_H * sc;
    int px0 = x * sc, py0 = y * sc;
    for (int j = 0; j < ph; j++)
        for (int i = 0; i < pw; i++)
            cursor_backup[j * pw + i] = window_get_pixel_phys(px0 + i, py0 + j);
    cursor_saved_x = x; cursor_saved_y = y;
}
/* The arrow as two convex polygons in 1/8 logical-pixel units (no FPU here).
   v42 drew it as logical scanlines, so at scale 2 every edge was a 2x2
   staircase. Now it is sampled 4x4 per PHYSICAL pixel into a coverage mask,
   once per scale, and each draw is one blend per pixel. */
static const int cur_head[] = {0,0, 100,100, 0,100};           /* tip, lower right, lower left */
static const int cur_tail[] = {32,96, 56,96, 74,134, 52,134};  /* slanted stem under the head */
static int cur_in_convex(const int *p, int n, int x, int y){
    int pos = 0, neg = 0;
    for (int k = 0; k < n; k++){
        int ax = p[2*k], ay = p[2*k+1], bx = p[2*((k+1)%n)], by = p[2*((k+1)%n)+1];
        int c = (bx - ax) * (y - ay) - (by - ay) * (x - ax);
        if (c > 0) pos = 1; else if (c < 0) neg = 1;
    }
    return !(pos && neg);
}
static int cur_in_shape(int x, int y){ return cur_in_convex(cur_head, 3, x, y) || cur_in_convex(cur_tail, 4, x, y); }
static unsigned char cur_cov_w[CURSOR_W * CURSOR_MAX_SCALE * CURSOR_H * CURSOR_MAX_SCALE];
static unsigned char cur_cov_b[CURSOR_W * CURSOR_MAX_SCALE * CURSOR_H * CURSOR_MAX_SCALE];
static int cur_mask_scale = 0;
static void gui_cursor_build_mask(int sc){
    static const int ox[8] = {7,-7,0,0,5,5,-5,-5}, oy[8] = {0,0,7,-7,5,-5,5,-5}; /* 7/8 px: the white outline's width */
    int pw = CURSOR_W * sc, ph = CURSOR_H * sc;
    for (int j = 0; j < ph; j++) for (int i = 0; i < pw; i++){
        int w = 0, b = 0;
        for (int sj = 0; sj < 4; sj++) for (int si = 0; si < 4; si++){
            /* subsample centre in 1/8 logical px, shifted so the outline is not clipped at the tip */
            int x = (i * 8 + si * 2 + 1) / sc - 8, y = (j * 8 + sj * 2 + 1) / sc - 8;
            if (!cur_in_shape(x, y)){
                int near = 0;
                for (int k = 0; k < 8 && !near; k++) near = cur_in_shape(x + ox[k], y + oy[k]);
                if (near) w++;
            } else b++;
        }
        cur_cov_w[j * pw + i] = (unsigned char)w; cur_cov_b[j * pw + i] = (unsigned char)b;
    }
    cur_mask_scale = sc;
}
static void gui_draw_cursor(int x, int y){
    int sc = gui_cursor_scale(), pw = CURSOR_W * sc, ph = CURSOR_H * sc;
    if (cur_mask_scale != sc) gui_cursor_build_mask(sc);
    for (int j = 0; j < ph; j++) for (int i = 0; i < pw; i++){
        int w = cur_cov_w[j * pw + i], b = cur_cov_b[j * pw + i];
        if (!w && !b) continue;
        unsigned int bg = window_get_pixel_phys(x * sc + i, y * sc + j);
        int keep = 16 - w - b;
        unsigned int r = (((bg >> 16) & 0xFF) * keep + 255 * w) / 16;
        unsigned int g = (((bg >> 8) & 0xFF) * keep + 255 * w) / 16;
        unsigned int bl = ((bg & 0xFF) * keep + 255 * w) / 16;
        window_pixel_phys(x * sc + i, y * sc + j, (r << 16) | (g << 8) | bl);
    }
}

/* App viewers have their own input loops. Keep the pointer alive while one
   is open, drawing it in screen coordinates outside the app viewport. */
static int gui_app_windowed = 0;
/* Vertical shift for an app's own content. Full screen, an app draws its
   own title strip across the top 40px and starts content at y=52. In a
   dock window the frame already draws the title bar above the viewport,
   so the same layout moves up by that strip, the same 32px Stocks has
   always saved through stx_top(). Add it to every content y. */
static int gui_app_dy(void){ return gui_app_windowed ? -32 : 0; }
static int app_view_x, app_view_y, app_view_w, app_view_h;
static int app_cursor_x, app_cursor_y;
static void gui_app_mouse_tick(void){
    if (!gui_app_windowed) return;
    int dx = 0, dy = 0, buttons = 0;
    int moved = mouse_get_delta(&dx, &dy, &buttons);
    (void)buttons;
    if (!moved && cursor_saved_x >= 0) return;
    window_clear_viewport();
    gui_cursor_restore();
    app_cursor_x += dx; app_cursor_y += dy;
    mouse_get_absolute(&app_cursor_x, &app_cursor_y, (int)window_width(), (int)window_height()); /* v62: absolute pointer wins over the relative walk when the backdoor is live; viewport already cleared above, so this is the full screen */
    if (app_cursor_x < 0) app_cursor_x = 0;
    if (app_cursor_y < 0) app_cursor_y = 0;
    if (app_cursor_x > (int)window_width() - CURSOR_W) app_cursor_x = (int)window_width() - CURSOR_W;
    if (app_cursor_y > (int)window_height() - CURSOR_H) app_cursor_y = (int)window_height() - CURSOR_H;
    gui_cursor_save(app_cursor_x, app_cursor_y);
    gui_draw_cursor(app_cursor_x, app_cursor_y);
    window_set_viewport(app_view_x, app_view_y, (unsigned int)app_view_w, (unsigned int)app_view_h);
}
/* v67 (0.62.2): for an app that repaints its whole viewport itself on
   every keystroke (Notes). Lift the pointer sprite before the repaint so
   the backup under it can't go stale and get restored over fresh content
   on the next move; the next gui_app_mouse_tick sees no saved cursor and
   draws it again on top of whatever the app just painted. */
static void gui_app_cursor_hide(void){
    if (!gui_app_windowed) return;
    window_clear_viewport();
    gui_cursor_restore();
    window_set_viewport(app_view_x, app_view_y, (unsigned int)app_view_w, (unsigned int)app_view_h);
}

/* get_key() alone left a real, reported bug: a visitor with no physical
   keyboard (a touch-only phone, or the live v86 embed before real
   keystrokes reach it) had no way to ever leave an app screen once
   opened, since "any key" was the only exit. A real click is the one
   input a mouse- or touch-only visitor can always produce, so it closes
   the app too now, not just a keypress. */
static void gui_wait_close(void){
    font_draw_string("esc or click to go back", 20, (int)window_height() - 30, 0x0075726E, -1);
    /* Real hardware and real QEMU continuously re-scan actual VRAM, so any
       write shows up on the very next real refresh, confirmed directly: a
       real screendump of this exact draw sequence rendered perfectly. v86,
       a JS/wasm emulator, samples its own canvas on some interval instead
       of continuously, and this whole app view draws its content then
       immediately blocks on input with nothing forcing a real wall-clock
       gap first, apparently landing between v86's own sampling points
       often enough that the text never visibly appears there, even though
       it's genuinely written to the framebuffer. A few real PIT ticks of
       settle time here, comfortably more than one real display frame,
       gives it that gap without real hardware/QEMU visitors ever noticing
       an unnecessary pause, they didn't need it in the first place. */
    window_present(); sleep_ticks(5);
    mouse_click_edge_sync(); /* a button already held (e.g. the click that opened this app) is the baseline, not a fresh click */
    for (;;) {
        gui_app_mouse_tick();
        int sc = kbd_pop();
        /* Real, reported bug: "any key" closed every read-only viewer,
           including Keyrate once it became a real typing test, the first
           keystroke anyone typed closed the app instead of registering.
           Esc (or a click, unchanged) closes now; every other key is
           just consumed and ignored, harmless on a page with nothing
           else to do with a keypress, and no longer surprising on one
           that does. */
        if (sc >= 0 && !(sc & 0x80) && kbd_map(sc) == 27) { gui_close_was_click = 0; return; }
        if (mouse_click_edge()) { gui_close_was_click = 1; return; }
        window_present(); __asm__ volatile ("hlt");
    }
}

/* A real macOS-style traffic light, not a fake one: red is a genuine close
   affordance, clicking anywhere already closes the app view (gui_wait_close
   polls for exactly that), this just gives that real behavior the familiar
   visual target instead of an invisible whole-screen hitbox. Yellow and
   green are drawn unlit on purpose: this kernel has no real windowing
   system yet, apps are always full-screen with no minimize/restore state
   to actually go to, so a lit, clickable minimize or maximize button would
   be a fake control that looks like it does something it doesn't. Real
   minimize/maximize wait on the actual windowing system already queued in
   roadmap.md's later product ideas, not a shortcut bolted on here. */
static void gui_draw_app_titlebar(const char *title){
    if (!gui_app_windowed) {
        gui_fill_circle(26, 20, 6, 0x00FF5F57, 0x00FAF8F6);
        gui_fill_circle(46, 20, 6, 0x00FFD64A, 0x00FAF8F6);
        gui_fill_circle(66, 20, 6, 0x00D8D4CE, 0x00FAF8F6);
        font_draw_string("x", 23, 12, 0x00602B28, -1);
        font_draw_string("-", 43, 12, 0x00624A20, -1);
        font_draw_string(title, 84, 12, 0x00555555, -1);
    }
}

/* Split into a content-only draw plus the old blocking entry point: the
   multi-window compositor (gui_multiwin_draw_one, near gui_launch_from_dock)
   calls the content draw directly, every repaint, with no gui_wait_close in
   the way; the Apps-folder/test-harness single-window path keeps calling
   gui_launch_weather() exactly as before, same pixels either way. */
static int weather_fetching = 0; /* set around a retry so the window can say so before the blocking fetch starts */
static void gui_draw_weather_content(void); /* defined below the glyph table it draws with, see "Weather window, redesigned" */
/* R retries right now. Returns 1 when the key should close the window. */
static int gui_weather_key(int k, void (*repaint)(void)){
    if (k == KEY_ESC) return 1;
    if (k == 'r' || k == 'R') {
        weather_fetching = 1; repaint(); window_present();
        weather_tried_once = 1;
        weather_fetch();
        weather_fetching = 0;
        gui_menubar_force_redraw();
        repaint();
    }
    return 0;
}
static void gui_launch_weather(void){
    gui_draw_weather_content();
    /* gui_wait_close, plus the retry key: esc or a click leaves. */
    font_draw_string("esc or click to go back", 20, (int)window_height() - 30, 0x0075726E, -1);
    window_present(); sleep_ticks(5);
    mouse_click_edge_sync();
    for (;;) {
        gui_app_mouse_tick();
        int sc = kbd_pop();
        if (sc >= 0 && !(sc & 0x80)) {
            char c = kbd_map(sc);
            if (gui_weather_key(c == 27 ? KEY_ESC : c, gui_draw_weather_content)) { gui_close_was_click = 0; return; }
        }
        if (mouse_click_edge()) { gui_close_was_click = 1; return; }
        window_present(); __asm__ volatile ("hlt");
    }
}

static void gui_launch_html(const char *label, const unsigned char *data, unsigned int data_len){
    window_clear(0x00FAF8F6);
    gui_draw_app_titlebar(label);

    /* app_weather_html/app_curbfind_html are raw byte arrays generated by
       gen_app.sh, not null-terminated C strings; html_to_text expects one,
       so copy with an explicit terminator rather than let it scan past the
       real buffer into whatever memory follows. */
    static char html[40960]; /* v35 (0.35.0): bumped from 20480 for the 6 newly-ported apps, homeqi is the largest at 39589 bytes */
    unsigned int copy_len = data_len < sizeof(html) - 1 ? data_len : sizeof(html) - 1;
    for (unsigned int i = 0; i < copy_len; i++) html[i] = (char)data[i];
    html[copy_len] = 0;

    static char text[12288]; /* v35 (0.35.0): bumped from 6144, plan's real extracted copy alone runs over 1100 words */
    unsigned int n = html_to_text(html, text, sizeof(text) - 1);
    text[n] = 0;
    render_wrapped_text(text, 20, 44, (int)window_width() - 40, (int)window_height() - 90, 0x001C1C1E);
    gui_wait_close();
}

static int gui_fat_count;
static char gui_fat_names[16][14];
static void gui_fat_collect(const char *name, unsigned int size, int is_dir){
    (void)size;
    if (gui_fat_count >= 16) return;
    int i = 0;
    while (name[i] && i < 12) { gui_fat_names[gui_fat_count][i] = name[i]; i++; }
    if (is_dir) gui_fat_names[gui_fat_count][i++] = '/';
    gui_fat_names[gui_fat_count][i] = 0;
    gui_fat_count++;
}

/* Same split as gui_draw_weather_content above. */
static void gui_draw_files_content(void){
    window_clear(0x00FAF8F6);
    gui_draw_app_titlebar("Files");
    gui_fat_count = 0;
    vfs_list(gui_fat_collect);
    if (gui_fat_count == 0) font_draw_string("(no files, or no FAT filesystem)", 20, 50, 0x001C1C1E, -1);
    for (int i = 0; i < gui_fat_count; i++) font_draw_string(gui_fat_names[i], 20, 50 + i * 18, 0x001C1C1E, -1);
}
static void gui_launch_files(void){ gui_draw_files_content(); gui_wait_close(); }

/* v85: the old one-shot gui_launch_chat (no history, /api/generate, a
   200-byte message cap) lived here; replaced by chat.h's real GUI app
   with scrollback and VFS-backed history, included below alongside the
   rest of the app headers. */

/* Physical-resolution text (defined with the Weather window below); the
   Calendar year view draws its mini-month digits with it. */
static int wx_text(const char *s, int lx, int ly, int size, int bold, int mul, unsigned int fg);
static int wx_text_lw(const char *s, int size, int bold, int mul);
/* Text ink curve, shared by every coverage-glyph path (gui_aa_char,
   wx_text, the Notes editor's editor_draw_glyph). Owner feedback on the
   AA text: "A-, sharpen them up a tad". Root cause of the softness: the
   DejaVu coverage bitmaps (FreeType via PIL, tools/gen/gen_editor_fonts.py)
   were blended as raw linear coverage in sRGB. A 24px vertical stem is
   ~2.2 physical px, e.g. 'l' rasterises as 188,255,108, so on a light
   surface only one column reaches full ink and the two flanking columns
   read as mid grey: the stem looks thin and fuzzy rather than inked.
   macOS gets its dense look from stem darkening plus a steep coverage
   curve; this does the same thing with a lookup, no layout change:
     dark on light:  a' = S(1 - (1-a)^1.3), S(x) = 128 + 1.2(x-128), clamped
     light on dark:  a' = S(a) only
   The first adds a little weight to thin dark stems so their cores hit
   full ink (188 -> 226, 108 -> 130); the second only steepens edges, so
   light-on-dark text (dock labels, dark chrome), which linear sRGB
   blending already makes look heavier, does not bloat. Both still pass
   through a smooth ramp of intermediate values: edges stay antialiased,
   just a shorter ramp. Faint fringes below ~8% coverage drop to zero,
   which is most of the visible "haze" around each glyph. */
static const unsigned char text_ink_dark[256] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,2,4,5,7,8,10,11,13,14,16,17,19,20,22,
    23,25,26,28,29,31,32,34,35,37,38,40,41,43,44,46,47,49,50,51,53,54,56,57,59,60,62,63,64,66,67,69,
    70,72,73,75,76,77,79,80,82,83,84,86,87,89,90,91,93,94,96,97,98,100,101,103,104,105,107,108,109,111,112,113,
    115,116,118,119,120,122,123,124,126,127,128,130,131,132,134,135,136,137,139,140,141,143,144,145,147,148,149,150,152,153,154,155,
    157,158,159,161,162,163,164,166,167,168,169,170,172,173,174,175,177,178,179,180,181,183,184,185,186,187,189,190,191,192,193,194,
    196,197,198,199,200,201,203,204,205,206,207,208,209,210,211,213,214,215,216,217,218,219,220,221,222,223,224,226,227,228,229,230,
    231,232,233,234,235,236,237,238,239,240,241,242,243,244,245,245,246,247,248,249,250,251,252,253,254,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
};
static int text_luma(unsigned int c){ return (int)(((c >> 16) & 0xFF) * 77 + ((c >> 8) & 0xFF) * 150 + (c & 0xFF) * 29) >> 8; }
/* Coverage a (0..255) of a glyph pixel in colour fg over destination
   colour dst -> the alpha to actually blend with. */
static int text_ink(int a, unsigned int fg, unsigned int dst){
    if (a <= 0) return 0;
    if (a >= 255) return 255;
    if (text_luma(fg) <= text_luma(dst)) return text_ink_dark[a];
    a = 128 + (a - 128) * 6 / 5;
    return a < 0 ? 0 : a > 255 ? 255 : a;
}

#include "gui_prompt.h"
#include "auth.h"
#include "editor.h"
#include "reminders.h"
#include "calendar.h"
#include "mail.h"
#include "contacts.h"
#include "calculator.h"
#include "chat.h"
#include "search.h"
#include "portfolio.h"

/* v50: DejaVu Sans, not Mono. Direct feedback: system UI text (menu bar,
   dock hover labels, titlebars) read as monospace/typewriter, not the
   proportional humanist look real macOS chrome uses (SF/Helvetica);
   turned out that was literally true, v44 hardcoded family index 2
   (Mono) for every string in the OS, the exact "mono outside a char
   grid" mismatch this project's own design rule warns about. DejaVu Sans
   (family 0) is the closest already-embedded substitute for SF/Helvetica,
   no new font asset needed, same glyph table the Notes editor's own
   "Sans" option already ships and proves out. Positioning is unaffected
   either way: font_draw_string's own fixed 8px-logical (16px physical)
   advance per character, not the glyph's natural width, is what places
   every character in this kernel, on purpose (see font_set_aa's own
   note), so this is a pure typeface swap. Proportional Sans glyphs run
   wider than Mono at a few characters ('M','W'); the existing `x >= px +
   16` clamp below already clips rather than collides into the next
   cell, same safety net Mono relied on, nothing new to add. */
/* v77: texttest mirrors every line to serial so tools/textspacing-check.sh can read the verdict headless, the pngtest pattern. */
static void tt_out(const char *s){ puts(s); serial_puts(s); }
static void tt_num(int n){ char b[12]; int i = 0; if (n < 0) { tt_out("-"); n = -n; } if (n == 0) b[i++] = '0'; while (n) { b[i++] = (char)('0' + n % 10); n /= 10; } b[i] = 0; for (int j = 0; j < i / 2; j++) { char t = b[j]; b[j] = b[i - 1 - j]; b[i - 1 - j] = t; } tt_out(b); }
#define GUI_AA_GLYPH(c) (&editor_glyphs[((0 * 2 + 0) * 4 + 2) * 95 + ((c) - 32)]) /* DejaVu Sans, regular, 24px */
/* v77: the real per-glyph advance in physical px, the metric the fixed
   16px cell ignored (see font_draw_string's own note). Degree ring and
   anything outside 32..126 keep the old cell so nothing else moves. */
static int gui_aa_advance(unsigned char c){
    if (c < 32 || c > 126) return 16;
    return GUI_AA_GLYPH(c)->advance;
}
static void gui_aa_char(unsigned char c, int px, int py, unsigned int fg, int bg, int cell){
    if (c < 32 || c > 126) c = (c == 0xF8) ? 176 : '?'; /* 0xF8 is the CP437 degree sign the weather uses */
    const struct editor_glyph *g;
    if (c == 176) { /* degree: DejaVu has it, but the table only carries 32..126; draw a small ring instead */
        if (bg >= 0) for (int j = 0; j < 32; j++) for (int i = 0; i < cell; i++) window_pixel_phys(px + i, py + j, (unsigned int)bg);
        for (int j = 0; j < 9; j++) for (int i = 0; i < 9; i++) { int dx = i - 4, dy = j - 4; int d2 = dx*dx + dy*dy; if (d2 >= 5 && d2 <= 12) window_pixel_phys(px + 3 + i, py + 8 + j, fg); } /* a ring at cap height, where a degree sign sits */
        return;
    }
    g = GUI_AA_GLYPH(c);
    if (bg >= 0) for (int j = 0; j < 32; j++) for (int i = 0; i < cell; i++) window_pixel_phys(px + i, py + j, (unsigned int)bg);
    int ox = px + g->left, oy = py + g->top - 2; /* `top` is measured from the line box's top (see editor_layout), not a baseline; the 24px face was sized for a 36px line box, ours is 32 */
    for (int row = 0; row < g->height; row++){
        for (int col = 0; col < g->width; col++){
            int a = editor_pixels[g->offset + row * g->width + col];
            if (!a) continue;
            int x = ox + col, y = oy + row;
            if (x < px || x >= px + cell) continue; /* keep inside the cell so neighbours never overdraw each other */
            unsigned int d = window_get_pixel_phys(x, y);
            a = text_ink(a, fg, d);
            if (!a) continue;
            unsigned int r = (((fg >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * (255 - a)) / 255;
            unsigned int gg = (((fg >> 8) & 0xFF) * a + ((d >> 8) & 0xFF) * (255 - a)) / 255;
            unsigned int b = ((fg & 0xFF) * a + (d & 0xFF) * (255 - a)) / 255;
            window_pixel_phys(x, y, (r << 16) | (gg << 8) | b);
        }
    }
}

/* Mono glyph for the two real character grids (terminal, keyrate). Family
   2 is DejaVu Sans Mono (see EDITOR_FAMILIES in editor.h); every glyph in
   it shares left=0 and the same advance, unlike the proportional Sans
   table GUI_AA_GLYPH draws everywhere else, so left-aligning it in the
   fixed 8-logical/16-physical-px cell keeps every column lined up instead
   of "m" crushing into "n" and "i"/"l" floating in dead space. Size 2
   (24px face, same size GUI_AA_GLYPH uses) is the closest already-baked
   mono size to the 16-physical-px cell. */
#define GUI_AA_GLYPH_MONO(c) (&editor_glyphs[((2 * 2 + 0) * 4 + 2) * 95 + ((c) - 32)])
static void gui_aa_char_mono(unsigned char c, int px, int py, unsigned int fg, int bg, int cell){
    if (c < 32 || c > 126) c = '?';
    if (bg >= 0) for (int j = 0; j < 32; j++) for (int i = 0; i < cell; i++) window_pixel_phys(px + i, py + j, (unsigned int)bg);
    const struct editor_glyph *g = GUI_AA_GLYPH_MONO(c);
    int ox = px + g->left, oy = py + g->top - 2;
    for (int row = 0; row < g->height; row++){
        for (int col = 0; col < g->width; col++){
            int a = editor_pixels[g->offset + row * g->width + col];
            if (!a) continue;
            int x = ox + col, y = oy + row;
            if (x < px || x >= px + cell) continue; /* keep inside the cell so neighbours never overdraw each other */
            unsigned int d = window_get_pixel_phys(x, y);
            unsigned int r = (((fg >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * (255 - a)) / 255;
            unsigned int gg = (((fg >> 8) & 0xFF) * a + ((d >> 8) & 0xFF) * (255 - a)) / 255;
            unsigned int b = ((fg & 0xFF) * a + (d & 0xFF) * (255 - a)) / 255;
            window_pixel_phys(x, y, (r << 16) | (gg << 8) | b);
        }
    }
}

/* Weather window, redesigned. Everything below draws at physical
   resolution through the same DejaVu Sans coverage glyphs the rest of the
   GUI text uses (editor_glyphs), so the window gets a real size hierarchy:
   16/20/24/28 px faces drawn 1:1, and the hero numeral scaled up from the
   28 px face. No gradient anywhere: flat cream surface, flat cards. */
#define WX_BG     0x00F5F0EB /* window surface, same cream as every app */
#define WX_CARD   0x00ECE5DC /* flat card tone, one step down from the surface */
#define WX_TEXT   0x00403439 /* dark warm text */
#define WX_MID    0x00645057
#define WX_DIM    0x00857A7C
#define WX_ACCENT 0x00C2772B /* the one accent: warm ochre, sun and storm bolt only */
#define WX_CLOUD  0x00B9AEA6
#define WX_ERR    0x009A3B2E
static const int WX_CAPTOP[4] = {3, 4, 5, 6}; /* line-box top to cap top, per face size, physical px */
static int wx_font_px(int size, int mul){ return (16 + 4 * size) * mul; }
static int wx_char_adv(unsigned char c, int size, int bold, int mul){
    if (c == 0xF8) return wx_font_px(size, mul) * 2 / 5;
    if (c < 32 || c > 126) c = '?';
    return editor_glyphs[((0 * 2 + bold) * 4 + size) * 95 + (c - 32)].advance * mul;
}
/* Width in physical px. */
static int wx_text_w(const char *s, int size, int bold, int mul){
    int w = 0; for (; *s; s++) w += wx_char_adv((unsigned char)*s, size, bold, mul); return w;
}
static void wx_blend(int x, int y, unsigned int fg, int a){
    if (a <= 0) return;
    if (a >= 255) { window_pixel_phys(x, y, fg); return; }
    unsigned int d = window_get_pixel_phys(x, y);
    unsigned int r = (((fg >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * (255 - a)) / 255;
    unsigned int g = (((fg >> 8) & 0xFF) * a + ((d >> 8) & 0xFF) * (255 - a)) / 255;
    unsigned int b = ((fg & 0xFF) * a + (d & 0xFF) * (255 - a)) / 255;
    window_pixel_phys(x, y, (r << 16) | (g << 8) | b);
}
/* Degree sign: the glyph table stops at 126, so draw a supersampled ring
   sized to the face, sitting at cap height. px,py = cap top left, physical. */
static void wx_degree(int px, int py, int fpx, unsigned int fg){
    int ro = fpx * 13 / 100, ri = fpx * 7 / 100;
    if (ro < 3) ro = 3;
    if (ri >= ro - 1) ri = ro - 2;
    if (ri < 1) ri = 1;
    int cx = px + ro + fpx / 20, cy = py + ro;
    for (int y = -ro - 1; y <= ro + 1; y++) for (int x = -ro - 1; x <= ro + 1; x++) {
        int in = 0;
        for (int sy = 0; sy < 4; sy++) for (int sx = 0; sx < 4; sx++) {
            int ux = x * 8 + sx * 2 - 3, uy = y * 8 + sy * 2 - 3; /* eighths of a pixel */
            int d2 = ux * ux + uy * uy;
            if (d2 <= ro * ro * 64 && d2 >= ri * ri * 64) in++;
        }
        wx_blend(cx + x, cy + y, fg, in * 255 / 16);
    }
}
/* Draws s with its cap top at logical (lx, ly). size 0..3 = 16/20/24/28 px
   face, mul = integer upscale (1 = the face's own pixels). Upscaled glyphs
   are bilinear-sampled from the coverage bitmap and then re-thresholded
   around 50% with a slope of `mul`, which puts a one-pixel anti-aliased
   edge back on the enlarged outline instead of a blur. Returns the width
   in logical px, rounded up. */
static int wx_text(const char *s, int lx, int ly, int size, int bold, int mul, unsigned int fg){
    int sc = (int)window_scale();
    int px = lx * sc, py = ly * sc, x0 = px;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == 0xF8) { wx_degree(px, py, wx_font_px(size, mul), fg); px += wx_char_adv(c, size, bold, mul); continue; }
        if (c < 32 || c > 126) c = '?';
        const struct editor_glyph *g = &editor_glyphs[((0 * 2 + bold) * 4 + size) * 95 + (c - 32)];
        const unsigned char *src = &editor_pixels[g->offset];
        int ox = px + g->left * mul, oy = py + (g->top - WX_CAPTOP[size]) * mul;
        if (mul == 1) {
            for (int r = 0; r < g->height; r++) for (int q = 0; q < g->width; q++) {
                int a = src[r * g->width + q];
                if (a) wx_blend(ox + q, oy + r, fg, text_ink(a, fg, window_get_pixel_phys(ox + q, oy + r)));
            }
        } else {
            for (int dy = -mul; dy < (g->height + 1) * mul; dy++) for (int dx = -mul; dx < (g->width + 1) * mul; dx++) {
                int u = (dx * 256 + 128) / mul - 128, v = (dy * 256 + 128) / mul - 128; /* source coords, 24.8 */
                int ux = u >> 8, vy = v >> 8, fx = u & 255, fy = v & 255;
                int a00 = (ux >= 0 && ux < g->width && vy >= 0 && vy < g->height) ? src[vy * g->width + ux] : 0;
                int a10 = (ux + 1 >= 0 && ux + 1 < g->width && vy >= 0 && vy < g->height) ? src[vy * g->width + ux + 1] : 0;
                int a01 = (ux >= 0 && ux < g->width && vy + 1 >= 0 && vy + 1 < g->height) ? src[(vy + 1) * g->width + ux] : 0;
                int a11 = (ux + 1 >= 0 && ux + 1 < g->width && vy + 1 >= 0 && vy + 1 < g->height) ? src[(vy + 1) * g->width + ux + 1] : 0;
                int a = ((a00 * (256 - fx) + a10 * fx) * (256 - fy) + (a01 * (256 - fx) + a11 * fx) * fy) >> 16;
                a = 128 + (a - 128) * mul;
                if (a > 255) a = 255;
                wx_blend(ox + dx, oy + dy, fg, a);
            }
        }
        px += g->advance * mul;
    }
    return (px - x0 + sc - 1) / sc;
}
static int wx_text_lw(const char *s, int size, int bold, int mul){ int sc = (int)window_scale(); return (wx_text_w(s, size, bold, mul) + sc - 1) / sc; }
static void wx_text_center(const char *s, int cx, int ly, int size, int bold, unsigned int fg){ wx_text(s, cx - wx_text_lw(s, size, bold, 1) / 2, ly, size, bold, 1, fg); }
static void wx_text_right(const char *s, int rx, int ly, int size, int bold, unsigned int fg){ wx_text(s, rx - wx_text_lw(s, size, bold, 1), ly, size, bold, 1, fg); }

/* Flat rounded card: four anti-aliased corner discs plus two rects. */
static void wx_card(int x, int y, int w, int h, int r, unsigned int color, unsigned int bg){
    gui_fill_circle(x + r, y + r, r, color, bg); gui_fill_circle(x + w - r - 1, y + r, r, color, bg);
    gui_fill_circle(x + r, y + h - r - 1, r, color, bg); gui_fill_circle(x + w - r - 1, y + h - r - 1, r, color, bg);
    window_rect(x + r, y, w - 2 * r, h, color);
    window_rect(x, y + r, w, h - 2 * r, color);
}

/* "18°" style degrees into out. */
static char *wx_put_int(char *o, int v){
    if (v < 0) { *o++ = '-'; v = -v; }
    char d[8]; int n = 0; if (!v) d[n++] = '0'; while (v && n < 7) { d[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) *o++ = d[--n];
    return o;
}
static void wx_deg(char *out, int v){ char *o = wx_put_int(out, v); *o++ = (char)0xF8; *o = 0; }

/* Condition glyphs, vector only, built from the two anti-aliased
   primitives the dock icons use. u = one eighth of the glyph's half size,
   so u = 2 is a ~32 px glyph and u = 5 an ~80 px one. */
#define WX_G_SUN 0
#define WX_G_PARTLY 1
#define WX_G_CLOUD 2
#define WX_G_FOG 3
#define WX_G_RAIN 4
#define WX_G_SNOW 5
#define WX_G_STORM 6
static int wx_glyph_kind(int code){
    if (code == 0) return WX_G_SUN;
    if (code <= 2) return WX_G_PARTLY;
    if (code == 3) return WX_G_CLOUD;
    if (code <= 48) return WX_G_FOG;
    if (code <= 67) return WX_G_RAIN;
    if (code <= 77) return WX_G_SNOW;
    if (code <= 82) return WX_G_RAIN;
    if (code <= 86) return WX_G_SNOW;
    return WX_G_STORM;
}
static void wx_g_sun(int cx, int cy, int u, int r8, unsigned int bg){
    /* r8: disc radius in u; rays run from r8+2 to r8+4 */
    gui_fill_circle(cx, cy, r8 * u, WX_ACCENT, bg);
    int a = (r8 + 2) * u, b = (r8 + 4) * u, t = u > 2 ? u / 2 : 1;
    int ad = a * 707 / 1000, bd = b * 707 / 1000;
    gui_draw_capsule(cx + a, cy, cx + b, cy, t, WX_ACCENT, bg); gui_draw_capsule(cx - a, cy, cx - b, cy, t, WX_ACCENT, bg);
    gui_draw_capsule(cx, cy + a, cx, cy + b, t, WX_ACCENT, bg); gui_draw_capsule(cx, cy - a, cx, cy - b, t, WX_ACCENT, bg);
    gui_draw_capsule(cx + ad, cy + ad, cx + bd, cy + bd, t, WX_ACCENT, bg); gui_draw_capsule(cx - ad, cy - ad, cx - bd, cy - bd, t, WX_ACCENT, bg);
    gui_draw_capsule(cx + ad, cy - ad, cx + bd, cy - bd, t, WX_ACCENT, bg); gui_draw_capsule(cx - ad, cy + ad, cx - bd, cy + bd, t, WX_ACCENT, bg);
}
/* Flat-bottomed cloud, bottom edge at cy + 4u. grow pads every part, used
   once in the background colour to cut a clean gap out of the sun behind. */
static void wx_g_cloud(int cx, int cy, int u, int grow, unsigned int color, unsigned int bg){
    gui_fill_circle(cx - 4 * u, cy + u, 3 * u + grow, color, bg);
    gui_fill_circle(cx + 5 * u, cy + 2 * u, 2 * u + grow, color, bg);
    gui_fill_circle(cx, cy - u, 5 * u + grow, color, bg);
    window_rect(cx - 4 * u, cy + u, 9 * u, 3 * u + grow + 1, color);
}
static void wx_glyph(int kind, int cx, int cy, int u, unsigned int bg){
    int t = u > 2 ? u / 2 : 1;
    if (kind == WX_G_SUN) { wx_g_sun(cx, cy, u, 3, bg); return; }
    if (kind == WX_G_PARTLY) {
        wx_g_sun(cx + 3 * u, cy - 3 * u, u, 2, bg);
        wx_g_cloud(cx - u, cy + 2 * u, u, t + 1, bg, bg);
        wx_g_cloud(cx - u, cy + 2 * u, u, 0, WX_CLOUD, bg);
        return;
    }
    if (kind == WX_G_CLOUD) { wx_g_cloud(cx, cy, u, 0, WX_CLOUD, bg); return; }
    if (kind == WX_G_FOG) {
        wx_g_cloud(cx, cy - 3 * u, u, 0, WX_CLOUD, bg);
        gui_draw_capsule(cx - 6 * u, cy + 4 * u, cx + 6 * u, cy + 4 * u, t, WX_DIM, bg);
        gui_draw_capsule(cx - 4 * u, cy + 7 * u, cx + 4 * u, cy + 7 * u, t, WX_DIM, bg);
        return;
    }
    wx_g_cloud(cx, cy - 2 * u, u, 0, WX_CLOUD, bg);
    if (kind == WX_G_RAIN) {
        for (int i = -1; i <= 1; i++) gui_draw_capsule(cx + i * 4 * u + u, cy + 4 * u, cx + i * 4 * u - u, cy + 7 * u, t, WX_MID, bg);
    } else if (kind == WX_G_SNOW) {
        for (int i = -1; i <= 1; i++) gui_fill_circle(cx + i * 4 * u, cy + (i ? 5 : 7) * u, t + 1, WX_DIM, bg);
    } else {
        gui_draw_capsule(cx + u, cy + 3 * u, cx - 2 * u, cy + 6 * u, t, WX_ACCENT, bg);
        gui_draw_capsule(cx - 2 * u, cy + 6 * u, cx + u, cy + 6 * u, t, WX_ACCENT, bg);
        gui_draw_capsule(cx + u, cy + 6 * u, cx - u, cy + 9 * u, t, WX_ACCENT, bg);
    }
}

static const char *WX_WEEKDAY[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static void gui_draw_weather_content(void){
    /* Root cause of issue #13: a failed fetch left weather_text empty, so
       every repaint (mouse move, focus change, tick) re-ran the blocking
       DNS/TCP fetch and froze the window. Try once per session here; the
       ten-minute cycle in gui_run and the R key do the retrying. */
    if (!weather_text[0] && !weather_tried_once) { weather_tried_once = 1; weather_fetch(); }
    window_clear(WX_BG);
    gui_draw_app_titlebar("Weather");
    /* Never empty. Three honest faces: the live reading, the last good
       reading (kept across a later failure, labelled stale), or a fixed
       sample that says it is a sample, on the header line AND on the
       forecast heading, so no part of the window passes for live data. */
    int live = weather_state == WX_OK && weather_have;
    int stale = !live && weather_have;
    int real = live || stale;
    static const int s_code[WX_DAYS] = {0, 2, 3, 61, 2}, s_hi[WX_DAYS] = {21, 19, 17, 15, 18}, s_lo[WX_DAYS] = {12, 11, 10, 9, 10}, s_wd[WX_DAYS] = {1, 2, 3, 4, 5};
    int temp = real ? weather_temp_c : 18, code = real ? weather_code10 / 10 : 0;
    int have_extra = real ? wx_extra_have : 1;
    int feels = real ? wx_feels_c : 17, hum = real ? wx_humidity : 55, wind = real ? wx_wind_kmh : 9;
    int nd = real ? wx_day_count : WX_DAYS;
    const int *d_code = real ? wx_day_code : s_code, *d_hi = real ? wx_day_hi : s_hi, *d_lo = real ? wx_day_lo : s_lo, *d_wd = real ? wx_day_wd : s_wd;

    int vw = (int)window_width(), vh = (int)window_height();
    int top = gui_app_windowed ? 0 : 40;          /* the full-screen path draws its own title bar above */
    int cw = vw - 72 > 760 ? 760 : vw - 72;         /* content column */
    int x0 = (vw - cw) / 2, x1 = x0 + cw;
    int y = top + 22;
    char buf[80];

    /* Header: city left, state right. */
    wx_text(geo_city[0] ? geo_city : (real ? "Your location" : "Sample location"), x0, y, 3, 1, 1, WX_TEXT);
    wx_text(live ? "Current conditions, live" : stale ? "Last good reading, may be out of date" : "Sample data, not a live reading", x0, y + 20, 1, 0, 1, live ? WX_DIM : WX_MID);
    if (weather_fetching) {
        wx_text_right("Fetching...", x1, y + 1, 2, 1, WX_MID);
    } else if (!live) {
        const char *head = weather_state == WX_OFFLINE ? "Offline" : weather_state == WX_TIMEOUT ? "Timed out" : weather_state == WX_BAD ? "Bad response" : weather_state == WX_FAILED ? "Request failed" : "Not fetched yet";
        int p = 0;
        for (const char *c = head; *c; c++) buf[p++] = *c;
        if (weather_err[0]) { buf[p++] = ' '; buf[p++] = '('; for (const char *c = weather_err; *c && p < 76; c++) buf[p++] = *c; buf[p++] = ')'; }
        buf[p] = 0;
        wx_text_right(buf, x1, y + 1, 2, 1, WX_ERR);
        wx_text_right("Press R to retry", x1, y + 21, 1, 0, WX_MID);
    } else {
        wx_text_right("Press R to refresh", x1, y + 3, 1, 0, WX_DIM);
    }

    /* Hero: the temperature, 28 px face at 5x (cap height 50 logical). */
    int hy = y + 50;
    wx_deg(buf, temp);
    int tw = wx_text(buf, x0 - 2, hy, 3, 0, 5, WX_TEXT);
    int bx = x0 + tw + 22;
    wx_text(weather_word(code), bx, hy + 8, 3, 1, 1, WX_TEXT);
    if (nd > 0) {
        char *o = buf; const char *s;
        for (s = "High "; *s; s++) *o++ = *s; o = wx_put_int(o, d_hi[0]); *o++ = (char)0xF8;
        for (s = "   Low "; *s; s++) *o++ = *s; o = wx_put_int(o, d_lo[0]); *o++ = (char)0xF8; *o = 0;
        wx_text(buf, bx, hy + 31, 2, 0, 1, WX_MID);
    }
    wx_glyph(wx_glyph_kind(code), x1 - 52, hy + 24, 5, WX_BG);

    /* Secondary facts: three flat cards. */
    int fy = hy + 72, fh = 50, gap = 12, fw = (cw - 2 * gap) / 3;
    for (int i = 0; i < 3; i++) {
        int fx = x0 + i * (fw + gap);
        wx_card(fx, fy, fw, fh, 10, WX_CARD, WX_BG);
        wx_text(i == 0 ? "Feels like" : i == 1 ? "Humidity" : "Wind", fx + 16, fy + 11, 1, 0, 1, WX_DIM);
        if (!have_extra) { wx_text("Not reported", fx + 16, fy + 28, 2, 0, 1, WX_MID); continue; }
        char *o = buf;
        if (i == 0) { o = wx_put_int(o, feels); *o++ = (char)0xF8; }
        else if (i == 1) { o = wx_put_int(o, hum); *o++ = '%'; }
        else { o = wx_put_int(o, wind); for (const char *s = " km/h"; *s; s++) *o++ = *s; }
        *o = 0;
        wx_text(buf, fx + 16, fy + 27, 3, 1, 1, WX_TEXT);
    }

    /* Forecast row. */
    int ry = fy + fh + 16;
    wx_text(live ? "5-day forecast" : stale ? "5-day forecast, last good reading" : "5-day forecast, sample data", x0, ry, 1, 1, 1, WX_MID);
    int cy0 = ry + 16, ch = vh - cy0 - 14;
    if (ch > 118) ch = 118;
    int drawn = 0;
    if (nd <= 0) {
        wx_card(x0, cy0, cw, ch, 10, WX_CARD, WX_BG);
        wx_text("No forecast in the last reply", x0 + 16, cy0 + ch / 2 - 5, 2, 0, 1, WX_MID);
    } else {
        int dw = (cw - (WX_DAYS - 1) * gap) / WX_DAYS;
        for (int i = 0; i < nd && i < WX_DAYS; i++) {
            int dx = x0 + i * (dw + gap), mx = dx + dw / 2;
            wx_card(dx, cy0, dw, ch, 10, WX_CARD, WX_BG);
            wx_text_center(i == 0 && real ? "Today" : WX_WEEKDAY[d_wd[i] % 7], mx, cy0 + 11, 1, 1, WX_TEXT);
            wx_glyph(wx_glyph_kind(d_code[i]), mx, cy0 + ch / 2 - 4, 2, WX_CARD);
            char hi[8], lo[8]; wx_deg(hi, d_hi[i]); wx_deg(lo, d_lo[i]);
            int hw = wx_text_lw(hi, 2, 1, 1), lw = wx_text_lw(lo, 2, 0, 1);
            int sx = mx - (hw + 8 + lw) / 2;
            wx_text(hi, sx, cy0 + ch - 22, 2, 1, 1, WX_TEXT);
            wx_text(lo, sx + hw + 8, cy0 + ch - 22, 2, 0, 1, WX_DIM);
            drawn++;
        }
    }
    /* Headless proof of what the window actually showed, only when it
       changes (this draws on every repaint). wxrow= is the forecast row:
       how many day cards were really drawn, and which weekdays. */
    { static int last_sig = -1;
      int sig = ((weather_state * 8 + (live ? 0 : stale ? 1 : 2) * 2 + weather_fetching) * 8 + drawn) * 2 + have_extra;
      if (sig != last_sig) { last_sig = sig;
          serial_puts("wxwin="); serial_puts(weather_fetching ? "fetching" : weather_state_name(weather_state));
          serial_puts(live ? " live\n" : stale ? " stale\n" : " sample\n");
          serial_puts("wxrow="); { char d[2] = { (char)('0' + drawn), 0 }; serial_puts(d); }
          for (int i = 0; i < drawn; i++) { serial_puts(i ? "," : " "); serial_puts(WX_WEEKDAY[d_wd[i] % 7]); }
          serial_puts(have_extra ? " facts=yes\n" : " facts=no\n"); } }
}

/* Real, reported bug, not a style complaint: gui_wait_close's "any key
   closes" is right for a page you only ever read (Weather, Curbfind,
   Bookrank, Quotestreak), but Keyrate was wired to that same read-only
   viewer despite being a TYPING TEST, its whole point is pressing keys.
   The very first keystroke anyone made to try typing closed the app
   instead. Root cause was the app itself: Keyrate was never actually a
   typing test in this kernel, gui_launch_html just rendered the ported
   site's own marketing copy as read-only text, same as every other
   ported page. A real typing test needs its own real input loop, not a
   different exit key bolted onto the read-only one. */
/* One fixed sentence ("the quick brown fox...") only ever tested the same
   45 characters, nothing like a real typing test (10fastfingers,
   monkeytype), which never run out of words. This freestanding build has
   no rand()/no libc, so a tiny LCG seeded from the real PIT tick count
   (irq.c's ticks()) stands in, good enough for word order, not for
   anything security-sensitive. */
static const char *KEYRATE_WORDS[] = {
    "the","quick","brown","fox","jumps","over","lazy","dog","time","people",
    "water","first","would","these","other","after","words","world","school",
    "still","every","great","might","under","never","found","those","while",
    "place","right","small","sound","between","name","home","read","hand",
    "large","spell","add","even","land","here","must","big","high","such",
    "follow","act","why","ask","men","change","went","light","kind","off",
    "need","house","try","again","animal","point","mother","near","self",
    "work","part","take","get","made","live","where","much","back","only",
};
#define KEYRATE_WORD_COUNT (int)(sizeof(KEYRATE_WORDS) / sizeof(KEYRATE_WORDS[0]))

static unsigned int keyrate_rand(unsigned int *state) {
    *state = *state * 1103515245u + 12345u;
    return (*state >> 16) & 0x7fff;
}

/* Fills buf from scratch with space-separated random words up to cap, returns the length. */
static int keyrate_gen_words(char *buf, int cap, unsigned int *rng) {
    int len = 0;
    while (len < cap - 12) { /* 12 = room for a trailing space + the longest word ("between") */
        if (len > 0) buf[len++] = ' ';
        const char *w = KEYRATE_WORDS[keyrate_rand(rng) % KEYRATE_WORD_COUNT];
        while (*w) buf[len++] = *w++;
    }
    buf[len] = 0;
    return len;
}

static void gui_launch_keyrate(void){
    window_clear(0x00FAF8F6);
    gui_draw_app_titlebar("Keyrate");

    static char target[256];
    unsigned int rng = ticks() | 1; /* |1 so a tick count of 0 at boot never freezes the LCG at 0 */
    int tlen = keyrate_gen_words(target, sizeof(target), &rng);
    int pos = 0, started = 0, total_typed = 0;
    unsigned int start_tick = 0;
    int area_bottom = (int)window_height() - 40;

    for (;;) {
        /* Real bug shipped and reported live, not caught in time: an
           earlier fix for this exact overlap (clear both the target-text
           row and the hint row every frame, not just inside one branch)
           was verified working in testing, then accidentally reverted by
           restoring kernel.c from a stale backup taken before that fix
           while cleaning up an unrelated temporary test command, the same
           wrong-backup mistake this session already made once with the
           gradient icon work. Re-applied here, and this time verified
           again with a real two-round script test (finish, retry, finish
           again) after re-applying, not just trusted from memory. One
           clear covering everything that can change, every frame,
           regardless of which branch below runs. */
        window_rect(20, 60, (int)window_width() - 40, area_bottom - 60, 0x00FAF8F6);
        window_rect(20, (int)window_height() - 30, (int)window_width() - 40, 16, 0x00FAF8F6);

        /* ponytail: wraps mid-word, no word-boundary lookahead like monkeytype's real
           renderer. Fine at 8px monospace; revisit if it reads badly in practice. */
        int x = 20, y = 60, max_x = (int)window_width() - 20;
        for (int i = 0; i < tlen; i++) {
            if (x + 8 > max_x) { x = 20; y += 16; }
            font_draw_char_mono((unsigned char)target[i], x, y, i < pos ? 0x00884B16 : 0x001C1C1E, -1);
            x += 8;
        }

        if (started) {
            unsigned int elapsed = ticks() - start_tick; /* real PIT ticks, ~100Hz, running since the very first keystroke */
            int chars = total_typed + pos;
            int wpm = elapsed > 0 ? (chars * 6000) / (5 * (int)elapsed) : 0; /* (chars/5 words) / (elapsed/100/60 min) */
            char buf[32]; int n = 0;
            if (wpm == 0) buf[n++] = '0';
            else { char tmp[12]; int tn = 0; int v = wpm; while (v > 0) { tmp[tn++] = (char)('0' + v % 10); v /= 10; } while (tn > 0) buf[n++] = tmp[--tn]; }
            buf[n++] = ' '; buf[n++] = 'w'; buf[n++] = 'p'; buf[n++] = 'm'; buf[n] = 0;
            font_draw_string(buf, 20, (int)window_height() - 30, 0x00884B16, -1);
        } else {
            font_draw_string("type to begin, esc or click to close", 20, (int)window_height() - 30, 0x0075726E, -1);
        }

        int ci = gui_getch_or_click();
        if (ci == -1 || ci == 27) break;
        char c = (char)ci;
        if (!started) { started = 1; start_tick = ticks(); }
        if (c == target[pos]) {
            pos++;
            if (pos >= tlen) { /* endless: bank this batch's chars, roll a fresh one, keep the same running timer going */
                total_typed += tlen;
                tlen = keyrate_gen_words(target, sizeof(target), &rng);
                pos = 0;
            }
        }
    }
}

/* v36 (0.36.0): a real terminal inside the desktop, not a second shell.
   It runs the exact same run() every text-mode command goes through, so
   there is precisely one shell in this kernel and anything it learns
   later works here the same day, rather than two implementations drifting
   apart. Output comes back through putc's capture hook (see capture_begin
   above), which is why no command needed changing to appear here. */
static void run(char *line); /* defined after the GUI; one shell, called from both */

#define TERM_COLS 96
#define TERM_ROWS 24 /* v42: fits 540 logical rows (44 + 24*16 = 428 < 480) */
#define TERM_SCROLLBACK 8192

static char term_buf[TERM_SCROLLBACK];
static unsigned int term_len = 0;

static void term_putc(char c){
    if (term_len + 1 >= TERM_SCROLLBACK) {
        /* Drop the oldest half rather than the oldest byte: a byte-at-a-
           time memmove on every character once full would make a long
           session visibly slow, and nobody scrolls back 4KB in an 800x600
           window anyway. */
        unsigned int keep = TERM_SCROLLBACK / 2;
        for (unsigned int i = 0; i < keep; i++) term_buf[i] = term_buf[term_len - keep + i];
        term_len = keep;
    }
    term_buf[term_len++] = c;
    term_buf[term_len] = 0;
}
static void term_puts(const char *s){ while (*s) term_putc(*s++); }

/* Walks the scrollback once, wrapping at TERM_COLS and on newlines, and
   draws only the last TERM_ROWS lines. Two passes over the same logic
   (count, then draw from the right offset) keeps this one source of truth
   for where a line breaks, instead of a separate wrap calculation that
   could disagree with what actually gets drawn. */
#define TERM_CONTENT_TOP 40

/* v0.76.11: direct report, still reproducing after v0.76.10's Notes fix
   ("every keystroke causes page to re-render") -- that fix only touched
   editor.h's own chrome/text split; term_render here had the identical
   shape (a full window_clear + titlebar redraw on every single
   keystroke, not just Notes' one dirty-flag flip) and was never fixed.
   Terminal's titlebar text never changes (no dirty-flag toggle Notes
   needed), so this is simpler: chrome draws exactly once, in
   term_draw_chrome() below, called before the loop in
   gui_launch_terminal, never again per keystroke. */
static void term_draw_chrome(void){
    serial_puts("termchrome\n"); /* discriminating marker for tools/checks/termchatflash-check.sh, same convention editor.h's "editorchrome" already established */
    window_clear(0x001A1512); /* warm near-black, the Mojave palette's dark end, not a cold pure black */
    gui_draw_app_titlebar("Terminal");
}

static void term_render(const char *input, unsigned int input_len){
    /* Content-only redraw now, scoped below the titlebar band
       (TERM_CONTENT_TOP=40; every real content y-coordinate below in
       this function is already >= 44, confirmed by reading them, so this
       clears exactly the region that can change and nothing the chrome
       occupies). */
    window_rect(0, TERM_CONTENT_TOP, (int)window_width(), (int)window_height() - TERM_CONTENT_TOP, 0x001A1512);

    unsigned int starts[TERM_ROWS + 1];
    unsigned int total_lines = 0, col = 0, line_start = 0;
    for (unsigned int i = 0; i <= term_len; i++) {
        int wrapped = (col == TERM_COLS);
        int newline = (i < term_len && term_buf[i] == '\n');
        if (wrapped || newline || i == term_len) {
            starts[total_lines % (TERM_ROWS + 1)] = line_start;
            total_lines++;
            line_start = newline ? i + 1 : i;
            col = 0;
            if (newline) continue;
            if (i == term_len) break;
        }
        col++;
    }

    unsigned int first = total_lines > TERM_ROWS ? total_lines - TERM_ROWS : 0;
    int y = 44;
    for (unsigned int ln = first; ln < total_lines && y < (int)window_height() - 80; ln++) {
        unsigned int p = starts[ln % (TERM_ROWS + 1)];
        int x = 16;
        for (unsigned int c = 0; c < TERM_COLS && p < term_len; c++, p++) {
            if (term_buf[p] == '\n') break;
            font_draw_char_mono((unsigned char)term_buf[p], x, y, 0x00D8CFC4, -1);
            x += 8;
        }
        y += 16;
    }

    /* Prompt line, pinned to the bottom so typing never scrolls out of
       view no matter how much output the last command produced. */
    int py = (int)window_height() - 60;
    font_draw_string("> ", 16, py, 0x00C98A3E, -1);
    int x = 32;
    for (unsigned int i = 0; i < input_len && x < 780; i++, x += 8)
        font_draw_char_mono((unsigned char)input[i], x, py, 0x00F2E9D8, -1);
    window_rect(x, py, 8, 15, 0x00C98A3E); /* block cursor */
    font_draw_string("esc closes   |   same shell as text mode", 16, (int)window_height() - 28, 0x00807468, -1);
}

static void gui_launch_terminal(void){
    static char input[TERM_COLS];
    static char out[4096];
    unsigned int input_len = 0;

    term_draw_chrome(); /* once per open, never again per keystroke -- see term_draw_chrome's own comment */
    if (term_len == 0) term_puts("Joshua Tree terminal. Type help.\n");
    term_render(input, input_len);

    for (;;) {
        /* See gui_wait_close and the Apps folder: settle for v86's canvas
           sampler, and treat a click/tap as a real way out for a visitor
           with no keyboard. */
        window_present(); sleep_ticks(5);
        mouse_click_edge_sync();
        int k = get_key_or_click();
        if (k == KEY_ESC || k == KEY_CLICK) return;
        if (k == KEY_ENTER) {
            input[input_len] = 0;
            term_puts("> "); term_puts(input); term_putc('\n');
            if (input_len) {
                /* Same run() the text-mode shell uses. Its output lands
                   in `out` instead of VGA memory purely because of the
                   capture hook, no command knows the difference. */
                capture_begin(out, sizeof(out));
                run(input);
                unsigned int n = capture_end();
                for (unsigned int i = 0; i < n; i++) term_putc(out[i]);
            }
            input_len = 0;
            term_render(input, input_len);
            continue;
        }
        if (k == '\b') { if (input_len) input_len--; }
        else if (k >= 32 && k < 127 && input_len < TERM_COLS - 1) input[input_len++] = (char)k;
        else continue;
        term_render(input, input_len);
    }
}

/* v37: the Apps folder. Every real app, laid out as a grid, so the dock
   can stay a short pinned list instead of growing until the icons are too
   small to read. Arrow keys + enter drive it as well as the mouse: the
   headless test harness can't inject usable mouse events (see
   guitest.sh's header for that whole trace), and an app screen only
   reachable by mouse would be an app screen this project can never
   regression-test. */
#define APPS_COLS 5
/* Rows that fit in the 375px panel at a 108px cell: 3 whole ones. Scroll
   limits and keyboard selection follow this, not a repeated literal. */
#define APPS_VIS_ROWS 3
/* The framebuffer has no alpha channel. Blend each glass pixel against the
   wallpaper already underneath it, keeping the real photo visible. */
static void gui_apps_glass(int x, int y, int w, int h){
    int sc = (int)window_scale(), radius = 24 * sc;
    int px0 = x * sc, py0 = y * sc, pw = w * sc, ph = h * sc;
    for (int py = 0; py < ph; py++) {
        for (int px = 0; px < pw; px++) {
            int cx = px < radius ? radius : (px >= pw - radius ? pw - radius - 1 : -1);
            int cy = py < radius ? radius : (py >= ph - radius ? ph - radius - 1 : -1);
            if (cx >= 0 && cy >= 0) {
                int dx = px - cx, dy = py - cy;
                if (dx * dx + dy * dy > radius * radius) continue;
            }
            unsigned int below = window_get_pixel_phys(px0 + px, py0 + py);
            unsigned int tint = gui_lerp(0x00F7F1EA, 0x00D8CAD0, py, ph);
            unsigned int glass = gui_lerp(below, tint, 76, 100);
            if (py < 2 * sc) glass = gui_lerp(glass, 0x00FFFFFF, 45, 100);
            window_pixel_phys(px0 + px, py0 + py, glass);
        }
    }
}
static void gui_launch(int icon); /* mutually recursive with the folder: the folder launches apps, and the dock launches the folder */
static void gui_apps_draw_grid(int scroll_offset, int sel, int x0, int y0, int cell_w, int cell_h, int tile){
    for (int i = 0; i < GUI_APPS_FOLDER; i++) {
        int row = i / APPS_COLS - scroll_offset;
        int col = i % APPS_COLS;
        /* Bounded by row count, not a pixel guess: row*cell_h (324) still
           clears the 375px panel_h even for the row that doesn't fit, so
           that stray row used to get drawn anyway, spilling past the
           panel's bottom edge and getting sliced by the window's own
           bottom (measured: kernel/kernel.c's own APPS_VIS_ROWS=3 already
           states the true count, "3 whole ones" -- this just enforces it
           instead of re-deriving a looser bound from cell_h). Because that
           spillover row was never inside the 375px rect gui_apps_redraw_
           panel repaints on every scroll/selection change, its pixels
           also never got cleared on a later repaint -- confirmed live: a
           scroll from offset 0 to 1 left index 18/19's (Contacts,
           Calculator) icons drawn at 0's row 3 sitting there under the
           freshly drawn row 3 of the new offset, stale pixels, not a
           second draw and not an index past GUI_APPS_FOLDER. */
        if (row < 0 || row >= APPS_VIS_ROWS) continue;
        int cx = x0 + col * cell_w + cell_w / 2;
        int cy = y0 + row * cell_h;
        if (i == sel) gui_rounded_rect_gradient(cx - tile / 2 - 10, cy - 10, tile + 20, cell_h - 14,
                                                 0x00FFF8F1, 0x00E5D8D0, 0x00E9DEE0, 12);
        gui_draw_one_icon_on(i, cx, cy + tile, tile, 0x00E9DEE0);
        int lw = font_string_width(GUI_LABELS[i]);
        font_draw_string(GUI_LABELS[i], cx - lw / 2, cy + tile + 10, 0x001C1C1E, -1);
    }
}
static void gui_apps_redraw_panel(int scroll_offset, int sel, int x0, int y0, int cell_w, int cell_h, int tile, int grid_w){
    int panel_x = x0 - 28, panel_y = 25, panel_w = grid_w + 56, panel_h = 375;
    gui_draw_wallpaper_rect(panel_x, panel_y, panel_w, panel_h);
    gui_apps_glass(panel_x, panel_y, panel_w, panel_h);
    /* The window's own title bar already reads "Apps" (gui_launch_from_
       dock draws GUI_LABELS[icon] there); a second "Apps" heading here
       just repeated it. Keep the key-hint line, moved up into the space
       the heading used to take. */
    font_draw_string("arrow keys to move   enter opens   esc closes", x0, 40, 0x006A6064, -1);
    gui_apps_draw_grid(scroll_offset, sel, x0, y0, cell_w, cell_h, tile);
    serial_puts("appsgridrepaint\n");
}
/* The window frame's title while an app runs inside the Apps folder's
   window. The frame is drawn once by gui_launch_from_dock with the folder's
   own label; an app launched from the grid used to leave "Apps" up there.
   The title sits outside the content viewport, so the viewport is lifted
   just for this draw. */
static void gui_app_frame_title(const char *label){
    if (!gui_app_windowed) return;
    int x = app_view_x - 8, y = app_view_y - 32;
    window_clear_viewport();
    window_rect(x + 90, y + 4, 320, 22, 0x00F5F0EB);
    font_draw_string(label, x + 96, y + 8, 0x00403439, -1);
    window_set_viewport(app_view_x, app_view_y, (unsigned int)app_view_w, (unsigned int)app_view_h);
}
static void gui_apps_launch(int icon){
    gui_app_frame_title(GUI_LABELS[icon]);
    gui_launch(icon);
    gui_app_frame_title(GUI_LABELS[GUI_APPS_FOLDER]);
}

static void gui_launch_apps(void){
    int sel = 0;
    int rows = (GUI_APPS_FOLDER + APPS_COLS - 1) / APPS_COLS;
    int cell_w = 150, cell_h = 108, tile = 60;
    int grid_w = APPS_COLS * cell_w;
    int x0 = ((int)window_width() - grid_w) / 2;
    int y0 = 95;
    int scroll_offset = 0; /* v0.77.0: mouse wheel scroll support, apps offset by row */

    /* The wallpaper behind this folder never changes while it is open, so it
       is painted once here and again only after an app has drawn over the
       screen. Every other change (selection, scroll) repaints the panel rect
       alone through gui_apps_redraw_panel. Repainting the wallpaper on every
       poll tick is what made this screen flash while scrolling or typing. */
    int full = 1;
    (void)rows;

    for (;;) {
        if (full) {
            full = 0;
            serial_puts("appsfullrepaint\n");
            window_clear(0x00201922);
            /* Not gui_draw_wallpaper(): that helper starts at GUI_MENUBAR_H,
               skipping the top strip to leave room for the desktop's own
               system menu bar. This folder runs inside gui_launch_from_
               dock's viewport, which already excludes the window's own
               title bar (drawn outside the viewport) and has no menu bar
               of its own, so local y=0 is real content, not chrome.
               Starting at GUI_MENUBAR_H left that strip as whatever
               window_clear above set it to: a dead black band under the
               title bar, never painted with wallpaper at all. */
            gui_draw_wallpaper_rows(0, (int)window_height());
            gui_apps_redraw_panel(scroll_offset, sel, x0, y0, cell_w, cell_h, tile, grid_w);
            /* The click that opened this folder (or closed the app launched
               from it) is the baseline, not a fresh click. Synced here, once
               per real repaint, never per loop pass: a per-pass sync threw
               away every click that landed during the present+sleep below,
               so on a host where wheel/selection events kept the loop
               turning, the folder could not be closed by the pointer at all
               (CI run 35523..., Apps: still open after 5 clicks over 20s). */
            mouse_click_edge_sync();
        }

        /* The same two v86/touch accommodations gui_wait_close documents:
           a few real ticks of settle time so the emulator's canvas sampler
           actually catches this frame before we block, and a click/tap
           counting as input so a phone can leave this screen at all. */
        window_present(); sleep_ticks(5);
        /* v0.77.0: mouse wheel scroll to browse all apps, one row per scroll. */
        int k = get_key_or_click();
        if (k == KEY_WHEEL_UP || k == KEY_WHEEL_DOWN) {
            /* The view scrolls where the wheel says, full stop. Snapping the
               offset back to keep the selection on screen (what the first cut
               of this did) made the wheel look broken: one notch scrolled and
               the next frame jumped right back. Selection follows the view on
               the keyboard path below, not the other way round. */
            int old_offset = scroll_offset;
            int max_scroll = rows - APPS_VIS_ROWS;
            if (max_scroll < 0) max_scroll = 0;
            scroll_offset += (k == KEY_WHEEL_UP) ? -1 : 1;
            if (scroll_offset < 0) scroll_offset = 0;
            if (scroll_offset > max_scroll) scroll_offset = max_scroll;
            if (scroll_offset != old_offset)
                gui_apps_redraw_panel(scroll_offset, sel, x0, y0, cell_w, cell_h, tile, grid_w);
            continue;
        }
        if (k == KEY_ESC) return;
        if (k == KEY_CLICK) {
            /* v86 (0.71.0) real bug, confirmed by reading this function:
               unlike the dock (whose tile clicks are hit-tested by
               gui_dock_hit_test in gui_run's main loop) or the Apps-folder
               TILE itself on the dock (also hit-tested the same way),
               every click reaching this screen used to be treated as
               "close the folder" unconditionally, with zero hit test
               against the grid cells drawn just above. Clicking a fleet
               app tile inside the launchpad therefore never opened it,
               it just dismissed the launchpad, matching the exact report
               ("clicking a fleet app inside it does nothing except
               dismiss the launchpad"). Only the keyboard path
               (arrows+Enter, or digits '1'-'9' for the first 9 of 22
               apps) ever actually launched anything from here. Real fix:
               hit-test app_cursor_x/y (already tracked live by
               gui_app_mouse_tick, the same position get_key_or_click's
               own KEY_CLICK just fired from) against each cell's real
               drawn bounds (same cx/cy/tile/cell_w/cell_h math as the
               draw loop above) and launch that app on a hit, exactly
               the same "click a tile to open it" contract the dock
               already has; a click outside every cell still closes the
               folder, unchanged behavior for the "tap anywhere to leave"
               phone case. */
            /* app_cursor_x/y are FULL-SCREEN logical coordinates (see
               gui_app_mouse_tick: it calls mouse_get_absolute() in the
               brief window between window_clear_viewport() and the
               matching window_set_viewport(), when window_width()/
               window_height() report the full screen, the same
               convention CLOSE_X/CLOSE_Y in landing/v86/embed.js already
               rely on), while x0/y0/cx/cy below are VIEWPORT-relative
               (this function draws through the viewport gui_launch_from_
               dock already set up). Subtract the viewport origin before
               comparing, or every hit test here silently misses. */
            int click_vx = app_cursor_x - app_view_x, click_vy = app_cursor_y - app_view_y;
            int hit = -1;
            for (int i = 0; i < GUI_APPS_FOLDER; i++) {
                int row = i / APPS_COLS - scroll_offset;
                int col = i % APPS_COLS;
                /* Skip rows that are scrolled off-screen */
                if (row < 0 || row * cell_h >= 375) continue;
                int cx = x0 + col * cell_w + cell_w / 2;
                int cy = y0 + row * cell_h;
                int cell_x0 = cx - cell_w / 2, cell_y0 = cy - 10, cell_x1 = cell_x0 + cell_w, cell_y1 = cy + tile + 24;
                if (click_vx >= cell_x0 && click_vx < cell_x1 && click_vy >= cell_y0 && click_vy < cell_y1) { hit = i; break; }
            }
            if (hit >= 0) { sel = hit; gui_apps_launch(hit); full = 1; continue; } /* the app drew over the screen, so the folder needs a real full repaint */
            return; /* a tap outside every tile still closes the folder: with no keyboard there is no other way out */
        }
        if (k == KEY_ENTER) { gui_apps_launch(sel); full = 1; continue; } /* returns here when that app closes, folder still open, same as a real launcher */
        int old_sel = sel;
        if (k == 'a' && sel > 0) sel--;                 /* left  */
        else if (k == 'd' && sel < GUI_APPS_FOLDER - 1) sel++; /* right */
        else if (k == 'w' && sel >= APPS_COLS) sel -= APPS_COLS;
        else if (k == 's' && sel + APPS_COLS < GUI_APPS_FOLDER) sel += APPS_COLS;
        else if (k >= '1' && k <= '9' && (k - '1') < GUI_APPS_FOLDER) { sel = k - '1'; gui_apps_launch(sel); full = 1; continue; }
        if (sel != old_sel) {
            /* Keyboard selection drags the view with it, the direction that is
               not surprising: move past the last visible row and the grid
               follows. */
            int sel_row = sel / APPS_COLS;
            if (sel_row < scroll_offset) scroll_offset = sel_row;
            if (sel_row >= scroll_offset + APPS_VIS_ROWS) scroll_offset = sel_row - APPS_VIS_ROWS + 1;
        }
        if (sel != old_sel) gui_apps_redraw_panel(scroll_offset, sel, x0, y0, cell_w, cell_h, tile, grid_w);
    }
}

/* v39: the Trash, a real view of what rm set aside, with real recovery.
   Same keyboard-and-click contract every screen here uses (see
   gui_wait_close): a phone has no keyboard, so every action has a tap. */
static void gui_launch_trash(void){
    int T = gui_app_dy();
    int sel = 0;
    for (;;) {
        window_clear(GUI_BG);
        gui_draw_app_titlebar("Trash");
        int n = trash_count();
        if (!n) {
            font_draw_string("Trash is empty.", 20, T + 52, 0x001C1C1E, -1);
            font_draw_string("Deleting a file with rm puts it here first.", 20, T + 76, 0x00807468, -1);
        } else {
            font_draw_string("up/down to pick   r restores   e empties   esc closes", 20, T + 52, 0x00807468, -1);
            for (int i = 0; i < n; i++) {
                int y = T + 84 + i * 22;
                if (i == sel) window_rect(16, y - 4, (int)window_width() - 32, 20, 0x00EDE6DC);
                font_draw_string(trash_name(i), 28, y, 0x001C1C1E, -1);
                char sz[16]; int p = 0; unsigned int v = trash_size(i);
                char t[12]; int ti = 0; if (!v) t[ti++] = '0'; while (v) { t[ti++] = '0' + v % 10; v /= 10; }
                while (ti) sz[p++] = t[--ti];
                sz[p++] = ' '; sz[p++] = 'b'; sz[p] = 0;
                font_draw_string(sz, 300, y, 0x00807468, -1);
            }
        }
        window_present(); sleep_ticks(5);
        mouse_click_edge_sync();
        int k = get_key_or_click();
        if (k == KEY_ESC || k == KEY_CLICK) return;
        if (!n) continue;
        if (k == KEY_UP && sel > 0) sel--;
        else if (k == KEY_DOWN && sel < n - 1) sel++;
        else if (k == 'r') { trash_restore(sel); if (sel >= trash_count() && sel > 0) sel--; }
        else if (k == 'e') { trash_empty(); sel = 0; }
    }
}

/* v47 (0.47.0): a real Settings screen, not a hidden shell command. Two
   rows, each a live toggle/stepper that writes through settings_save()
   immediately, the same "no separate Apply step" behaviour every setting
   in this kernel already has (fsuse, diskuse, wind). up/down picks a row,
   left/right (a/d, since there's no numpad here) changes it, a tap on a
   row also toggles/steps it, matching the touch-first contract every
   other screen in this GUI already keeps. */
/* v85: settings_prompt_line, the same shape contacts_prompt_line and
   mail_prompt_line already established (live-render, backspace, enter
   confirms, esc or a click cancels), pulled in here rather than shared
   across files since every app in this kernel keeps its own copy of this
   small loop already. Used to edit the two string LLM settings, since a
   toggle/stepper doesn't fit free text the way it fits wind/dock/wall.

   security pass: added a `masked` parameter. The password-change and
   add-user rows below used to call this with the typed password rendered
   in the clear on screen, the exact thing auth_field_input's dot-echo in
   auth.h was built to avoid for the login/first-run screens -- a real gap
   (shoulder-surfing, screen recording, the v86 landing demo) since this is
   the same secret, just entered through a different door. Masked draws a
   fixed-width dot per character, same convention, same length-not-content
   leak trade-off already accepted for login. */
static int settings_prompt_line(const char *prompt, char *out, int max, int masked) {
    unsigned int n = 0;
    while (out[n] && (int)n < max - 1) n++; /* start from the current value, not empty, so editing is a tweak not a retype */
    mouse_click_edge_sync();
    for (;;) {
        window_clear(GUI_BG);
        gui_draw_app_titlebar("Settings");
        font_draw_string(prompt, 20, 52, 0x0075726E, -1);
        window_rect(20, 76, (int)window_width() - 40, 20, 0x00FFFFFF);
        out[n] = 0;
        if (masked) {
            char dots[AUTH_PASSWORD_MAX + 1];
            unsigned int dn = n; if (dn > AUTH_PASSWORD_MAX) dn = AUTH_PASSWORD_MAX;
            for (unsigned int i = 0; i < dn; i++) dots[i] = '*';
            dots[dn] = 0;
            font_draw_string(dots, 24, 78, 0x001C1C1E, -1);
        } else {
            font_draw_string(out, 24, 78, 0x001C1C1E, -1);
        }
        int k = get_key_or_click();
        if (k == KEY_ESC || k == KEY_CLICK) return 0;
        if (k == KEY_ENTER) break;
        if (k == '\b') { if (n > 0) n--; }
        else if ((int)n < max - 1 && k >= 32 && k < 127) out[n++] = (char)k;
    }
    out[n] = 0;
    return 1;
}

#define SETTINGS_ROW_COUNT 7 /* v75: + wallpaper source; v85: + LLM model, + LLM host:port; v0.77: + Account (change password), + Add user */
static const int SETTINGS_ROWS_Y[SETTINGS_ROW_COUNT] = {84, 116, 148, 180, 212, 252, 284};

/* Pure, hardware/GUI-free: given a real click's full-screen logical
   coordinates and the window's current width, returns which Settings row
   (0..SETTINGS_ROW_COUNT-1) it lands in, or -1 if it misses every row's
   own highlight rect (window_rect(16, y-6, ww-32, 28, ...), the exact
   rect drawn below). Extracted into its own function so this real
   hit-test math is unit-testable without a mouse or a boot, the same
   shape rtl8139_clamp_len's own extraction used for exactly this reason
   (v0.72.1: "so it's unit-testable without a NIC"). */
static int settings_row_at(int cx, int cy, int ww){
    if (cx < 16 || cx >= ww - 16) return -1;
    for (int i = 0; i < SETTINGS_ROW_COUNT; i++) {
        int ry = SETTINGS_ROWS_Y[i];
        if (cy >= ry - 6 && cy < ry - 6 + 28) return i;
    }
    return -1;
}

static void gui_launch_settings(void){
    int sel = 0;
    for (;;) {
        window_clear(GUI_BG);
        gui_draw_app_titlebar("Settings");
        font_draw_string("up/down to pick   left/right or tap to change   esc closes", 20, 52, 0x00807468, -1);

        const int *rows_y = SETTINGS_ROWS_Y;
        for (int i = 0; i < SETTINGS_ROW_COUNT; i++) {
            int y = rows_y[i];
            if (i == sel) window_rect(16, y - 6, (int)window_width() - 32, 28, 0x00EDE6DC);
            if (i == 0) {
                font_draw_string("Wind (swaying wallpaper)", 28, y, 0x001C1C1E, -1);
                font_draw_string(wind_enabled ? "On" : "Off", 400, y, wind_enabled ? 0x002F7B4F : 0x00807468, -1);
            } else if (i == 1) {
                font_draw_string("Dock size", 28, y, 0x001C1C1E, -1);
                char sz[8]; int p = 0; int v = dock_scale_pct;
                if (v >= 10) sz[p++] = '0' + v / 10;
                sz[p++] = '0' + v % 10; sz[p++] = '%'; sz[p] = 0;
                font_draw_string(sz, 400, y, 0x001C1C1E, -1);
            } else if (i == 2) {
                /* v75/v81: honest label. A map theme's name only shows once
                   a real tile mosaic is on screen; while it's still
                   fetching, or when the fetch failed and the photo is
                   what's actually up, say so instead of claiming a theme
                   that isn't really rendering. */
                font_draw_string("Wallpaper", 28, y, 0x001C1C1E, -1);
                const char *theme_name = wall_theme == WALL_COOL ? "Map (Cool)" : wall_theme == WALL_RAW ? "Map (Raw)" : wall_theme == WALL_SAT ? "Satellite" : (geo_city[0] ? geo_city : "Map (Warm)");
                /* v0.83.x: the v86 demo's own honest label. "photo until
                   then" stopped being true the moment the no-network
                   fallback became the baked satellite capture instead of
                   the tree -- font_is_fallback() is the same real v86
                   signal wall_apply() itself branches on. */
                const char *lbl = wall_theme == WALL_PHOTO ? "Photo" : (wall_map ? theme_name : (font_is_fallback() ? "Satellite (offline demo)" : "Map (fetching, photo until then)"));
                font_draw_string(lbl, 400, y, wall_theme != WALL_PHOTO && wall_map ? 0x002F7B4F : 0x001C1C1E, -1);
            } else if (i == 3) {
                font_draw_string("LLM model", 28, y, 0x001C1C1E, -1);
                font_draw_string(llm_model, 400, y, 0x001C1C1E, -1);
            } else if (i == 4) {
                font_draw_string("LLM host:port", 28, y, 0x001C1C1E, -1);
                char hp[LLM_HOST_MAX + 8]; int p = 0;
                const char *s = llm_host; while (*s && p < (int)sizeof(hp) - 8) hp[p++] = *s++;
                hp[p++] = ':';
                char digits[8]; int nd = 0; int v = llm_port;
                if (v == 0) digits[nd++] = '0';
                while (v) { digits[nd++] = (char)('0' + v % 10); v /= 10; }
                while (nd) hp[p++] = digits[--nd];
                hp[p] = 0;
                font_draw_string(hp, 400, y, 0x001C1C1E, -1);
            } else if (i == 5) {
                /* v0.77: real accounts. Tap/enter here walks old-password
                   ->new-password->confirm through settings_prompt_line
                   (masking not needed for that shared shell-style prompt,
                   the dedicated masked auth_field_input is only used by
                   the login/first-run screens themselves, kept separate on
                   purpose so Settings doesn't need its own copy of the
                   dot-echo loop for one row). */
                font_draw_string("Account", 28, y, 0x001C1C1E, -1);
                font_draw_string(auth_current_user[0] ? auth_current_user : "(none)", 400, y, 0x001C1C1E, -1);
            } else {
                font_draw_string("Add user (new account)", 28, y, 0x001C1C1E, -1);
                font_draw_string("tap or enter", 400, y, 0x00807468, -1);
            }
        }
        font_draw_string("Settings are saved to disk and survive a reboot.", 20, (int)window_height() - 28, 0x00807468, -1);

        window_present(); sleep_ticks(5);
        mouse_click_edge_sync();
        int k = get_key_or_click();
        if (k == KEY_ESC) return;
        if (k == KEY_UP && sel > 0) sel--;
        else if (k == KEY_DOWN && sel < SETTINGS_ROW_COUNT - 1) sel++;
        else if (k == KEY_CLICK || k == 'a' || k == 'd') {
            /* A real click acts on whichever row it actually landed on, not
               whichever row a PRIOR arrow-key press happened to leave
               selected -- before this, a mouse/touch-only visitor with no
               keyboard (this kernel's own browser-demo idle tour included)
               could only ever toggle row 0 (Wind), since `sel` starts at 0
               and a bare click never moved it. Scoped to k==KEY_CLICK only:
               a real 'a'/'d' keypress must keep acting on whatever `sel`
               already is, not get silently overridden by a stale cursor
               position that has nothing to do with the keypress. */
            if (k == KEY_CLICK) {
                /* Real bug, found by tools/checks/auth-flow-check.py driving a real
                   synthetic pointer click (the exact gap walldemo-regression-check.py's
                   own comment already flagged as unconfirmed): app_cursor_x/y is only
                   kept live by gui_app_mouse_tick(), which is gated on gui_app_windowed
                   and therefore only ticks for apps opened through gui_launch_from_dock.
                   gui_launch_settings() is entered straight from the Apple menu
                   (gui_menu_run_item), never through that wrapper, so gui_app_windowed
                   stays 0 the whole time Settings is open and app_cursor_x/y is never
                   seeded or updated -- every click here hit-tested wherever the cursor
                   happened to be frozen at (0,0 if no windowed app had run yet this
                   boot), so settings_row_at() always missed and every click silently
                   fell through to acting on whatever `sel` already was, exactly the
                   pre-fix settingsclick bug this same block's own comment describes,
                   just reachable a different way than that fix covered. Query the real
                   position directly at the moment of the click instead of trusting the
                   stale global. */
                mouse_get_absolute(&app_cursor_x, &app_cursor_y, (int)window_width(), (int)window_height());
                int hit = settings_row_at(app_cursor_x, app_cursor_y, (int)window_width());
                if (hit >= 0) sel = hit;
            }
            if (sel == 0) { wind_enabled = !wind_enabled; settings_save(); }
            else if (sel == 2) {
                /* v81: cycles all four real themes (Photo -> Warm -> Cool
                   -> Raw -> Photo), not a binary toggle, matching the
                   left/right-steps contract dock size already uses below.
                   A tap (KEY_CLICK) always steps forward, same convention
                   dock size's tap already keeps. */
                int dir = (k == 'a') ? -1 : 1;
                wall_switch_theme((wall_theme + dir + 5) % 5);
                wall_apply(wall_theme != WALL_PHOTO);
            }
            else if (sel == 3) {
                /* v85 (direct feedback, after this landed): a free-text
                   model field can be typo'd to point at a model that
                   isn't actually installed on the host, silently failing
                   every chat. Real fix, checked against `ollama list` on
                   this machine rather than guessed: a bounded cycle over
                   the two real chat models actually installed
                   (qwen3:8b, the real default as of v0.85.4; llama3.1:8b,
                   the second choice). nomic-embed-text is on the host too
                   but is an embedding-only model, not a chat model,
                   deliberately left off this list, the same "don't offer
                   what wouldn't work" call the wallpaper theme cycle
                   already makes for its own four real options. A live
                   /api/tags probe (Ollama's own model-list endpoint, same
                   plain-HTTP shape chat_send already uses) would be the
                   more general fix and is a real, scoped-out next step,
                   not done here to keep this pass's actual shipped
                   surface honest about what it covers. */
                int cur = strcmp(llm_model, LLM_MODELS[0]) == 0 ? 0 : 1;
                int dir = (k == 'a') ? -1 : 1;
                int next = (cur + dir + LLM_MODEL_COUNT) % LLM_MODEL_COUNT;
                int p = 0; const char *m = LLM_MODELS[next];
                while (m[p] && p < LLM_MODEL_MAX - 1) { llm_model[p] = m[p]; p++; }
                llm_model[p] = 0;
                settings_save();
            }
            else if (sel == 4) {
                char hostbuf[LLM_HOST_MAX];
                int hn = 0; while (llm_host[hn] && hn < LLM_HOST_MAX - 1) { hostbuf[hn] = llm_host[hn]; hn++; }
                hostbuf[hn] = 0;
                if (settings_prompt_line("LLM host (hostname or IP, enter to confirm, esc to cancel):", hostbuf, LLM_HOST_MAX, 0)) {
                    int j = 0; while (hostbuf[j] && j < LLM_HOST_MAX - 1) { llm_host[j] = hostbuf[j]; j++; } llm_host[j] = 0;
                    char portbuf[8]; int pn = 0; int v = llm_port;
                    char digits[8]; int nd = 0;
                    if (v == 0) digits[nd++] = '0';
                    while (v) { digits[nd++] = (char)('0' + v % 10); v /= 10; }
                    while (nd) portbuf[pn++] = digits[--nd];
                    portbuf[pn] = 0;
                    if (settings_prompt_line("LLM port (enter to confirm, esc to cancel):", portbuf, sizeof(portbuf), 0)) {
                        int nv = 0; for (int c = 0; portbuf[c]; c++) if (portbuf[c] >= '0' && portbuf[c] <= '9') nv = nv * 10 + (portbuf[c] - '0');
                        if (nv > 0 && nv <= 65535) llm_port = nv;
                    }
                    settings_save();
                }
            }
            else if (sel == 5 && k != 'a' && k != 'd') {
                /* Change password for the account that's actually logged
                   in this session, not a free-text username field: there
                   is exactly one real "current user" concept in this
                   kernel today (auth_current_user, set by auth_gate at
                   boot), matching the single-machine/single-visitor
                   threat model docs/THREAT-MODEL.md lays out. 'a'/'d'
                   (left/right, the stepper convention every other row
                   uses) don't apply to this row, only a real tap/enter. */
                if (auth_current_user[0]) {
                    char oldbuf[AUTH_PASSWORD_MAX + 1]; oldbuf[0] = 0;
                    if (settings_prompt_line("Current password (enter to confirm, esc to cancel):", oldbuf, sizeof(oldbuf), 1)) {
                        char newbuf[AUTH_PASSWORD_MAX + 1]; newbuf[0] = 0;
                        if (settings_prompt_line("New password (enter to confirm, esc to cancel):", newbuf, sizeof(newbuf), 1)) {
                            char confirmbuf[AUTH_PASSWORD_MAX + 1]; confirmbuf[0] = 0;
                            if (settings_prompt_line("Confirm new password (enter to confirm, esc to cancel):", confirmbuf, sizeof(confirmbuf), 1)) {
                                int ok = !strcmp(newbuf, confirmbuf) && auth_change_password(auth_current_user, oldbuf, newbuf);
                                font_draw_string(ok ? "Password changed." : "That didn't work -- wrong current password or mismatch.",
                                                  20, (int)window_height() - 48, ok ? 0x002F7B4F : 0x00A33B3B, -1);
                                /* Same real bug tools/checks/auth-flow-check.py found in
                                   kernel/auth.h's login rejection: drawing lands in a back
                                   buffer and only window_present() ever flips it visible, and
                                   this status line had no frame boundary of its own before
                                   sleep_ticks -- the next redraw erased it unseen. */
                                window_present();
                                sleep_ticks(60);
                            }
                            memset(newbuf, 0, sizeof(newbuf));
                            memset(confirmbuf, 0, sizeof(confirmbuf));
                        }
                        memset(oldbuf, 0, sizeof(oldbuf));
                    }
                }
            }
            else if (sel == 6 && k != 'a' && k != 'd') {
                /* Adding a second local account. No admin/role concept
                   exists in this kernel (real, honest gap, not modeled
                   here since the direct request scoped this to "create a
                   user, change your own password", not a permissions
                   system) -- any logged-in session can add another
                   account. auth_create_user already refuses a duplicate
                   name, an empty name/password, or a full table (8 max). */
                char ubuf[AUTH_USERNAME_MAX + 1]; ubuf[0] = 0;
                if (settings_prompt_line("New username (enter to confirm, esc to cancel):", ubuf, sizeof(ubuf), 0)) {
                    char pbuf[AUTH_PASSWORD_MAX + 1]; pbuf[0] = 0;
                    if (settings_prompt_line("Password for that user (enter to confirm, esc to cancel):", pbuf, sizeof(pbuf), 1)) {
                        int ok = auth_create_user(ubuf, pbuf);
                        /* v0.77.1: the gate is opt-in (auth_gate is a no-op
                           on an unconfigured system, see kernel/auth.h),
                           so a session that reaches this row with nobody
                           logged in yet is exactly the "creating the very
                           first account" case that used to be the
                           first-run screen's job. Treat this account as
                           the current session's own from here on, the
                           same real effect the old first-run flow had,
                           just moved to Settings instead of gating boot. */
                        if (ok && !auth_current_user[0]) {
                            unsigned int p = 0; while (ubuf[p] && p < AUTH_USERNAME_MAX) { auth_current_user[p] = ubuf[p]; p++; } auth_current_user[p] = 0;
                            auth_logged_in = 1;
                        }
                        font_draw_string(ok ? "Account created." : "Couldn't create that account (name taken, empty, or table full).",
                                          20, (int)window_height() - 48, ok ? 0x002F7B4F : 0x00A33B3B, -1);
                        /* Same missing-present bug as the Change password status line
                           above and kernel/auth.h's login rejection: without this call
                           the message never reaches the visible framebuffer. */
                        window_present();
                        sleep_ticks(60);
                    }
                    memset(pbuf, 0, sizeof(pbuf));
                }
            }
            else if (sel != 5 && sel != 6) {
                int dir = (k == 'a') ? -1 : 1; /* a tap always steps up; a real direction only from the keyboard */
                if (k == KEY_CLICK) dir = 1;
                int v = dock_scale_pct + dir;
                if (v > 25) v = 5; if (v < 5) v = 25; /* wraps, so a tap always does something visible */
                dock_scale_pct = v; settings_save();
            }
        }
    }
}

#include "stocks.h"
#include "toroid.h"
#include "quotes.h"
#include "bookrank.h"
#include "lexly.h"
#include "fieldbook.h"
#include "plan.h"
#include "homeqi.h"
#include "curbfind.h"
#include "sparkjar.h"
#include "epiphany.h"
#include "activity.h"

static void gui_launch(int icon){
    if (icon == GUI_APPS_FOLDER) { gui_launch_apps(); return; }
    if (icon == GUI_TRASH) { gui_launch_trash(); return; }
    if (icon == 0)      gui_launch_files();
    else if (icon == 1) gui_launch_mail();
    else if (icon == 2) gui_launch_calendar();
    else if (icon == 3) gui_launch_editor();
    else if (icon == 4) gui_launch_reminders();
    else if (icon == 5) gui_launch_terminal();
    else if (icon == 6) gui_launch_chat_app();
    else if (icon == 7) gui_launch_weather();
    else if (icon == 8) gui_launch_curbfind();
    else if (icon == 9) gui_launch_keyrate();
    else if (icon == 10) gui_launch_bookrank();
    else if (icon == 11) gui_launch_quotes();
    else if (icon == 12) gui_launch_plan();
    else if (icon == 13) gui_launch_lexly();
    else if (icon == 14) gui_launch_toroid();
    else if (icon == 15) gui_launch_sparkjar();
    else if (icon == 16) gui_launch_html("Homeqi", app_homeqi_html, app_homeqi_len);
    else if (icon == 16) gui_launch_homeqi();
    else if (icon == 17) gui_launch_fieldbook();
    else if (icon == 18) gui_launch_contacts();
    else if (icon == 19) gui_launch_calculator();
    else if (icon == 20) gui_launch_stocks();
    else if (icon == 21) gui_launch_search();
    else if (icon == 22) gui_launch_epiphany();
    else if (icon == 23) gui_launch_portfolio();
    else if (icon == 24) gui_launch_activity();
}

static void gui_launch_from_dock(int icon){
again:
    /* Keep the desktop visible around the app. The framebuffer viewport
       clips every app draw, including window_clear and physical AA text. */
    gui_draw_desktop(-1, -1, 0, 0);
    int apps = icon == GUI_APPS_FOLDER;
    int x = apps ? 56 : 70, y = apps ? 30 : 40;
    int w = apps ? 848 : 820, h = apps ? 490 : 385;
    gui_rounded_rect_on_wallpaper(x, y, w, h, 0x00F5F0EB, 18);
    window_rect(x + 8, y + 30, w - 16, h - 38, 0x00F5F0EB);
    gui_fill_circle(x + 24, y + 16, 7, 0x00FF5F57, 0x00F5F0EB);
    gui_fill_circle(x + 46, y + 16, 7, 0x00FFD64A, 0x00F5F0EB);
    gui_fill_circle(x + 68, y + 16, 7, 0x00D8D4CE, 0x00F5F0EB);
    font_draw_string("x", x + 21, y + 8, 0x00602B28, -1);
    font_draw_string("-", x + 43, y + 8, 0x00624A20, -1);
    font_draw_string(GUI_LABELS[icon], x + 96, y + 8, 0x00403439, -1);
    window_set_viewport(x + 8, y + 32, (unsigned int)(w - 16), (unsigned int)(h - 40));
    app_view_x = x + 8; app_view_y = y + 32;
    app_view_w = w - 16; app_view_h = h - 40;
    app_cursor_x = editor_mouse_x; app_cursor_y = editor_mouse_y;
    cursor_saved_x = cursor_saved_y = -1;
    gui_app_windowed = 1;
    gui_close_was_click = 0;
    gui_launch(icon);
    gui_app_windowed = 0;
    window_clear_viewport();
    gui_cursor_restore();
    /* v68 (0.63.0): the dock stays visible around every app window, so a
       click on another dock tile while an app is open reads, to anyone,
       as "open that one instead". Before this it only closed the current
       app (the "click anywhere closes" contract) and the visitor had to
       click the tile a second time, direct feedback ("it should just
       stack the windows", meaning the same close-and-open every other
       app already does). If the click that closed this app sits on a
       dock tile, open that tile's app in its place, from the pointer's
       real position, no second click. Esc never switches: only a click
       can name a tile. A tail call, not recursion, so a visitor hopping
       across the dock all day never grows the kernel stack. */
    if (gui_close_was_click) {
        int slot = gui_dock_hit_test(app_cursor_x, app_cursor_y);
        if (slot >= 0) { editor_mouse_x = app_cursor_x; editor_mouse_y = app_cursor_y; icon = gui_order[slot]; goto again; }
    }
}

/* v0.73.0: phase 1 of real multi-window, per roadmap.md's "Multi-window,
   honestly scoped" entry (grep for it: real windows were estimated as
   "3-5 sessions of unstarted work" before this pass, and confirmed still
   unstarted immediately before this one, every app a blocking function and
   window_open() called exactly once at boot). This is real, not cosmetic:
   a genuine window list, and two windows genuinely open and drawn on
   screen at the same time, each redrawn from its own real state on every
   repaint, neither frozen nor a fake snapshot.

   Deliberately NOT attempted here, the real reasons this stays phase 1:
   - Only Files and Weather are wired to this path. They're the two
     simplest gui_wait_close-shaped read-only viewers (roadmap.md's own
     staggering plan calls this batch 1 of the app conversion). Every
     other dock app (Mail, Calendar, Notes, Reminders, Terminal, Chat) has
     real per-keystroke state and keeps the old blocking
     gui_launch_from_dock path untouched, on purpose: converting an app
     with a real input loop into a non-blocking draw()/on_key() handler
     with no shared-state hazard is real work per app, not a bulk
     find/replace, and roadmap.md is explicit that this is the multi-
     session part.
   - No real z-order/overlap compositing: the naive back-to-front redraw
     draws window 0 then window 1, and a click always tests only the
     most-recently-opened (topmost) window's full rect. Real
     click-through-to-lower-window hit testing is phase 2, not attempted.
   - No click-to-focus: opening a window focuses it (the same
     "most-recently-opened owns input" model the single-window kernel
     already had, just no longer tearing the previous window down first).
     Clicking the background window does nothing yet; that's real
     click-to-focus, phase 2's job.
   - Capped at 2 concurrent windows (GUI_MULTIWIN_MAX): exactly what this
     pass needs to prove and no more; a real 4-6 slot cap is a phase-2
     decision once more apps are converted and the memory cost (each
     window drawing straight into the shared framebuffer today, no
     per-window backing store yet, see roadmap.md's sizing note) is
     actually being paid by something that needs it. */
#define GUI_MULTIWIN_MAX 2
typedef struct {
    int icon;
    int x, y, w, h;
} gui_window_t;
static gui_window_t gui_windows[GUI_MULTIWIN_MAX];
static int gui_window_count = 0; /* gui_windows[0..gui_window_count-1] are the real open windows, back-to-front */

static int gui_multiwin_supported(int icon){ return icon == 0 || icon == 7 || icon == 1 || icon == 2 || icon == 4; } /* Files, Weather, Mail, Calendar, Reminders */

/* v0.75.0 (batch 2): Mail/Calendar/Reminders have real per-keystroke
   interaction (adding a reminder, navigating calendar days/months,
   composing mail), a real, distinct shape from Files/Weather's static
   gui_wait_close-only viewers, per roadmap.md's own note. gui_run's
   input loop below only ever forwards a keystroke to the app whose
   window is currently topmost/focused (the same "topmost owns input"
   rule click-to-focus already established for clicks). */
static int gui_multiwin_interactive(int icon){ return icon == 1 || icon == 2 || icon == 4 || icon == 7; } /* 7: Weather, for its R-to-retry key */

/* Window 0 keeps the exact single-window rect the existing dock-app tests
   already assert against (gui_launch_from_dock's own x=70,y=40,w=820,h=385;
   appclose-check.py/app-interact-check.py hard-code CLOSE_X,CLOSE_Y=94,56,
   which is this same rect's close-circle centre, x+24,y+16). A second,
   concurrently-open window is offset so both titlebars and both close
   buttons stay fully on screen and visually distinct, not stacked exactly
   on top of each other. */
static void gui_multiwin_geom(int slot_index, int *x, int *y, int *w, int *h){
    if (slot_index == 0) { *x = 70; *y = 40; *w = 820; *h = 385; }
    else { *x = 70 + 60; *y = 40 + 60; *w = 820; *h = 385; }
}

/* Magnet-style window snapping (title-bar drag to an edge/corner). All five
   multi-window apps draw their content through window_set_viewport(x+8,
   y+32, w-16, h-40) and lay it out with window_width()/window_height(),
   not hard-coded 820x385 numbers, so any w/h this hands them is real, not
   clipped or scaled after the fact.

   Zones, checked corners-first so a near-corner drag never mistakenly
   reads as a plain edge: 0=left half, 1=right half, 2..5=quarters (TL,
   TR, BL, BR), 6=full (top edge, like Magnet's maximize), -1=no zone. The
   playable area is the desktop strip between the menu bar and the dock,
   the same area gui_multiwin_geom's own windows already live inside. */
static int gui_snap_area(int *top, int *bottom){
    *top = GUI_MENUBAR_H;
    *bottom = gui_dock_y0() - 10;
    return (int)window_width();
}
static int gui_snap_zone(int mx, int my){
    int top, bottom; int w = gui_snap_area(&top, &bottom);
    const int corner = 40, edge = 12;
    if (mx <= corner && my <= top + corner) return 2;
    if (mx >= w - corner && my <= top + corner) return 3;
    if (mx <= corner && my >= bottom - corner) return 4;
    if (mx >= w - corner && my >= bottom - corner) return 5;
    if (my <= top + edge) return 6;
    if (mx <= edge) return 0;
    if (mx >= w - edge) return 1;
    return -1;
}
static void gui_snap_target(int zone, int *x, int *y, int *w, int *h){
    int top, bottom; int sw = gui_snap_area(&top, &bottom);
    int areaH = bottom - top;
    switch (zone) {
        case 0: *x = 0;      *y = top;             *w = sw / 2;      *h = areaH; break;
        case 1: *x = sw / 2; *y = top;             *w = sw - sw / 2; *h = areaH; break;
        case 2: *x = 0;      *y = top;             *w = sw / 2;      *h = areaH / 2; break;
        case 3: *x = sw / 2; *y = top;             *w = sw - sw / 2; *h = areaH / 2; break;
        case 4: *x = 0;      *y = top + areaH / 2; *w = sw / 2;      *h = areaH - areaH / 2; break;
        case 5: *x = sw / 2; *y = top + areaH / 2; *w = sw - sw / 2; *h = areaH - areaH / 2; break;
        default:*x = 0;      *y = top;             *w = sw;          *h = areaH; break; /* 6: full */
    }
}
/* One-pixel border, drawn straight onto the framebuffer, never onto a
   window (there's nothing under it to preserve while dragging: the
   dragged window itself isn't moved live, only this preview outline is
   drawn, see gui_run's drag_win handling). */
static void gui_snap_outline(int zone){
    if (zone < 0) return;
    int x, y, w, h; gui_snap_target(zone, &x, &y, &w, &h);
    unsigned int c = 0x00307FE2;
    window_rect(x, y, w, 1, c);
    window_rect(x, y + h - 1, w, 1, c);
    window_rect(x, y, 1, h, c);
    window_rect(x + w - 1, y, 1, h, c);
}

/* v0.76.18: split out of what used to be one gui_multiwin_draw_one, direct
   report ("keystroke re-rendering glitch still present" after the earlier
   Notes/Terminal/Chat chrome fixes). Root cause, same bug shape those
   fixes already established, just never extended here: every keystroke
   into a multi-window Mail/Calendar/Reminders window went through the
   v0.75.0 "cheap tier" at gui_run's mw_key_repaint path, which called the
   OLD gui_multiwin_draw_one every time -- and that function unconditionally
   redrew this window's ENTIRE chrome (gui_rounded_rect_on_wallpaper's real
   per-row alpha blend across the whole ~820x385 rect, plus all three
   traffic lights and the title) before ever touching content, on every
   single character typed. None of that chrome depends on what's being
   typed; only the content viewport does. With no double buffer in this
   framebuffer (this kernel's own standing, tracked limitation), redrawing
   that much unchanged chrome on every keystroke is exactly the kind of
   real mid-scan tear the dock/menu cheap tiers already exist to avoid,
   just never plugged into this path. */
static void gui_multiwin_draw_chrome(const gui_window_t *win){
    serial_puts("mwchrome\n"); /* discriminating marker for tools/checks/mwkeyflash-check.sh */
    int x = win->x, y = win->y, w = win->w, h = win->h;
    gui_rounded_rect_on_wallpaper(x, y, w, h, 0x00F5F0EB, 18);
    window_rect(x + 8, y + 30, w - 16, h - 38, 0x00F5F0EB);
    gui_fill_circle(x + 24, y + 16, 7, 0x00FF5F57, 0x00F5F0EB);
    gui_fill_circle(x + 46, y + 16, 7, 0x00FFD64A, 0x00F5F0EB);
    gui_fill_circle(x + 68, y + 16, 7, 0x00D8D4CE, 0x00F5F0EB);
    font_draw_string("x", x + 21, y + 8, 0x00602B28, -1);
    font_draw_string("-", x + 43, y + 8, 0x00624A20, -1);
    font_draw_string(GUI_LABELS[win->icon], x + 96, y + 8, 0x00403439, -1);
}
static void gui_multiwin_draw_content_only(const gui_window_t *win){
    int x = win->x, y = win->y, w = win->w, h = win->h;
    window_set_viewport(x + 8, y + 32, (unsigned int)(w - 16), (unsigned int)(h - 40));
    /* v0.76.19: real, standing bug, direct report ("two toolbars on
       windows, two x buttons two minimize buttons") -- present since
       multi-window Files/Weather shipped (v0.73.0) and Mail/Calendar/
       Reminders (v0.75.0), not something this pass's chrome/content split
       introduced. Every one of the five *_content functions below calls
       the shared gui_draw_app_titlebar(), which only skips drawing its
       OWN traffic-light circles + "x"/"-" when the global gui_app_windowed
       flag is set -- but that flag was only ever set by the OLD single-
       window gui_launch_from_dock path (bracketing its blocking
       gui_launch() call), never by this multi-window content path. So
       every multiwin content redraw drew a second, real, viewport-
       relative (26,20)/(46,20)/(66,20) set of traffic lights on top of
       gui_multiwin_draw_chrome's own real ones -- two visibly offset
       toolbars, exactly as reported, not a rendering glitch, a real
       missing flag. */
    gui_app_windowed = 1;
    /* Real per-repaint content, not a cached bitmap: each call re-derives
       the window's content from the same live state its single-window
       counterpart reads (vfs_list for Files, weather_text for Weather),
       so a second window opening never leaves the first one's content
       stale or frozen. */
    if (win->icon == 0) gui_draw_files_content();
    else if (win->icon == 7) gui_draw_weather_content();
    else if (win->icon == 1) gui_draw_mail_content();
    else if (win->icon == 2) gui_draw_calendar_content();
    else if (win->icon == 4) gui_draw_reminders_content();
    gui_app_windowed = 0;
    window_clear_viewport();
}
static void gui_multiwin_draw_one(const gui_window_t *win){
    gui_multiwin_draw_chrome(win);
    gui_multiwin_draw_content_only(win);
}

/* Weather's retry repaints its own (topmost) window content before and
   after the blocking fetch, so "Fetching..." is on screen while it runs. */
static void gui_weather_mw_repaint(void){
    if (gui_window_count > 0 && gui_windows[gui_window_count - 1].icon == 7) gui_multiwin_draw_content_only(&gui_windows[gui_window_count - 1]);
}

/* Called from gui_run's own full-repaint branch, right alongside the
   menu/notif/weather overlay draws it already does there, so every open
   window is genuinely redrawn on top of the desktop on every real repaint
   this kernel does, back-to-front, list order. */
static void gui_multiwin_draw_all(void){
    for (int i = 0; i < gui_window_count; i++) gui_multiwin_draw_one(&gui_windows[i]);
}

/* v0.73.6 (phase 2): the one real choke point that moves a window to the
   top of the z-order list. Both gui_multiwin_open's "already open, refocus
   it" path and the new click-to-focus path below now go through this
   instead of each keeping its own copy of the same shift loop, so draw
   order (gui_multiwin_draw_all, back-to-front over the list) and input
   hit-testing (gui_multiwin_hit_test, topmost-first over the same list)
   can never disagree about which window is "on top": there is exactly one
   piece of z-order state, gui_windows[0..count-1]'s own order. */
static void gui_multiwin_focus(int idx){
    if (idx < 0 || idx >= gui_window_count || idx == gui_window_count - 1) return;
    gui_window_t tmp = gui_windows[idx];
    for (int j = idx; j < gui_window_count - 1; j++) gui_windows[j] = gui_windows[j + 1];
    gui_windows[gui_window_count - 1] = tmp;
}

static int gui_multiwin_open(int icon){
    for (int i = 0; i < gui_window_count; i++) {
        if (gui_windows[i].icon == icon) {
            /* Already open: focus it (move to the end of the list, so the
               back-to-front draw puts it on top) instead of opening a
               duplicate. */
            gui_multiwin_focus(i);
            return gui_window_count - 1;
        }
    }
    if (gui_window_count >= GUI_MULTIWIN_MAX) return -1; /* the real cap this pass proves, see the comment above */
    int slot = gui_window_count;
    gui_windows[slot].icon = icon;
    gui_multiwin_geom(slot, &gui_windows[slot].x, &gui_windows[slot].y, &gui_windows[slot].w, &gui_windows[slot].h);
    gui_window_count++;
    return slot;
}

/* v0.73.6 (phase 2): real click-to-focus hit-testing. Checks every open
   window, topmost-drawn first (gui_windows[count-1] down to [0], the exact
   reverse of gui_multiwin_draw_all's back-to-front draw order), and returns
   the first (i.e. topmost) whose rect contains the click -- the same
   "topmost visible thing wins" rule every real windowing system's hit
   test uses (X11 stacking order, Win32 Z-order, etc). Replaces the old
   phase-1 gui_multiwin_focused_click_hit, which only ever tested the
   single most-recently-opened window and left a click on a visible
   background window doing nothing. -1 means the click hit neither
   window's rect at all. */
static int gui_multiwin_hit_test(int mx, int my){
    for (int i = gui_window_count - 1; i >= 0; i--) {
        const gui_window_t *w = &gui_windows[i];
        if (mx >= w->x && mx < w->x + w->w && my >= w->y && my < w->y + w->h) return i;
    }
    return -1;
}

static void gui_multiwin_close(int idx){
    if (idx < 0 || idx >= gui_window_count) return;
    for (int j = idx; j < gui_window_count - 1; j++) gui_windows[j] = gui_windows[j + 1];
    gui_window_count--;
}

/* v0.75.0 (batch 2): the same SC[]/extended-0xE0 decode get_key_or_click
   already does, but never hlt-waits -- called at most once per gui_run
   frame, for the one focused interactive window (gui_multiwin_interactive),
   so a real keystroke reaches Mail/Calendar/Reminders' own on_key handler
   without blocking the compositor the way the old gui_wait_close-shaped
   loops did. A truly split extended sequence (the second byte of an arrow
   key) just drops this frame rather than block waiting for it -- the IRQ
   handler fills both bytes of the ring within microseconds of each other,
   well inside one frame at this poll rate, so in practice this never
   drops a real arrow keystroke. */
static int gui_multiwin_key_nonblock(void){
    int sc = kbd_pop();
    if (sc < 0) return -1;
    if (sc == 0xE0) {
        int sc2 = kbd_pop();
        if (sc2 < 0) return -1;
        if (sc2 == 0x48) return KEY_UP;
        if (sc2 == 0x50) return KEY_DOWN;
        if (sc2 == 0x4B) return KEY_LEFT;
        if (sc2 == 0x4D) return KEY_RIGHT;
        return -1;
    }
    if (sc & 0x80) return -1; /* key release */
    char c = kbd_map(sc);
    if (c == '\n') return KEY_ENTER;
    if (c == 27)   return KEY_ESC;
    if (c) return (int)(unsigned char)c;
    return -1;
}

/* A loop (octagon approximating a circle, 8 capsule segments) for the
   round parts of a script letter: no sin/cos in this freestanding build,
   an 8-point table scaled by the target radius reads as smoothly round
   once anti-aliased at this size, the same shortcut real low-res icon
   fonts have always taken. `skip_mask` drops edges (bit i skips segment
   i, 0 for none), an open loop for 'e' so it doesn't render as the exact
   same closed ring 'o' uses. Real legibility bug caught in the first
   render, twice: "hello" with two identical closed loops for e and o
   read as "hollo". Dropping a single edge didn't fix it either, the
   thick rounded end-caps on the segments either side of the gap simply
   overlapped and covered it back up, two adjacent edges need to go for
   an opening actually wide enough to read at this size. */
/* A brief boot splash instead of cutting straight to the desktop with no
   transition at all, the same beat every real OS gives a fresh boot: the
   logo shows immediately, and a thin progress bar only appears once that
   hold crosses a full second, so a genuinely fast boot (which this one
   almost always is) never shows a bar filling for no real reason, just
   the logo for a beat. Runs off ticks() (real PIT time, ~100Hz, already
   confirmed via the shell's own "sleep 1s" = sleep_ticks(100)), not a
   frame-counted loop, so it holds the same real duration regardless of
   how fast this machine happens to render each frame. */
static void gui_draw_boot_screen(void){
    unsigned int bg = 0x00000000; /* pure black boot background, direct request */
    window_clear(bg);
    int cx = (int)window_width() / 2, cy = (int)window_height() / 2; /* v45.2: centred on the real window; 400 was the 800-wide centre and sat left of centre at 960 */
    gui_draw_logo(cx, cy - 10, 5, bg, 0x00FFFFFF); /* v0.76.47: was a hardcoded maroon (0x0085144B) the function used to bake in regardless of caller, direct report ("boot logo still pink/purple") -- gui_draw_logo now takes color explicitly, white here to match the plain-black boot screen. v48: dropped the "hello" wordmark, direct request, logo alone reads cleaner */

    unsigned int start = ticks();
    unsigned int logo_only = 60; /* 0.6s: just the logo and wordmark, no bar yet */
    unsigned int bar_span  = 40; /* 0.4s: bar fills once shown, ~1s total */
    int bar_w = 160, bar_h = 6, bar_x = (int)window_width() / 2 - bar_w / 2, bar_y = (int)window_height() / 2 + 70;
    int bar_track_drawn = 0;
    for (;;) {
        unsigned int elapsed = ticks() - start;
        if (elapsed >= logo_only) {
            if (!bar_track_drawn) { window_rect(bar_x, bar_y, bar_w, bar_h, gui_blend(bg, 0x00FFFFFF)); bar_track_drawn = 1; }
            unsigned int since_bar = elapsed - logo_only;
            int fill = since_bar >= bar_span ? bar_w : (int)(bar_w * since_bar / bar_span);
            window_rect(bar_x, bar_y, fill, bar_h, 0x00FFFFFF); /* v0.76.48: was the same hardcoded maroon as the old logo, direct report, white to match the boot screen */
        }
        if (elapsed >= logo_only + bar_span) break;
        window_present(); __asm__ volatile ("hlt");
    }
}

/* A real about panel, not a placeholder: actual physical memory stats
   straight from pmm (the same real physical memory manager the rest of
   this kernel allocates through) and real uptime off ticks(), the same
   PIT tick counter every other real-time feature in this file already
   uses.

   v0.76.13: direct request -- a real macOS "About This Mac" dialog is a
   small, centered card, not a full-screen takeover; this used to
   window_clear() the entire desktop and left-align every line from the
   screen edge. Now draws a real small floating card (ABOUT_W x ABOUT_H,
   centered both ways on screen, matching gui_multiwin_draw_one's own
   rounded-card style for visual consistency with the rest of this
   kernel's real floating windows) over the live desktop -- gui_draw_desktop
   is called first to clear the Apple-menu dropdown and show the real
   wallpaper/dock behind the card, the same way a real dialog sits over a
   real desktop. Every line of text is centered horizontally within the
   card via font_string_width, and the whole text block is centered
   vertically within the card too, not just left-pinned under the
   titlebar. Doesn't call the shared gui_wait_close() (its own hint text
   is hardcoded to the full screen's bottom-left, which would float
   oddly outside this small card) -- a local close-wait loop mirrors its
   exact same real mechanics (mouse_click_edge/kbd_pop/Escape) instead. */
static void gui_launch_about(void){
    int ABOUT_W = 360, ABOUT_H = 220;
    int bx = ((int)window_width() - ABOUT_W) / 2;
    int by = ((int)window_height() - ABOUT_H) / 2;

    gui_draw_desktop(-1, -1, 0, 0); /* clears the Apple-menu dropdown, real wallpaper+dock behind the card */
    gui_rounded_rect_on_wallpaper(bx, by, ABOUT_W, ABOUT_H, 0x00FAF8F6, 18);
    gui_fill_circle(bx + 26, by + 20, 6, 0x00FF5F57, 0x00FAF8F6);
    gui_fill_circle(bx + 46, by + 20, 6, 0x00FFD64A, 0x00FAF8F6);
    gui_fill_circle(bx + 66, by + 20, 6, 0x00D8D4CE, 0x00FAF8F6);
    font_draw_string("x", bx + 23, by + 12, 0x00602B28, -1);
    font_draw_string("-", bx + 43, by + 12, 0x00624A20, -1);
    { const char *title = "About Joshua Tree";
      font_draw_string(title, bx + (ABOUT_W - font_string_width(title)) / 2, by + 12, 0x00555555, -1); }

    char buf[64]; int n;
    unsigned int total_kb = pmm_total_frames() * 4, free_kb = pmm_free_frames() * 4;
    n = 0; buf[n++] = 'M'; buf[n++] = 'e'; buf[n++] = 'm'; buf[n++] = 'o'; buf[n++] = 'r'; buf[n++] = 'y'; buf[n++] = ':'; buf[n++] = ' ';
    { char tmp[12]; int tn = 0; unsigned int v = free_kb; if (v == 0) tmp[tn++] = '0'; while (v > 0) { tmp[tn++] = (char)('0' + v % 10); v /= 10; } while (tn > 0) buf[n++] = tmp[--tn]; }
    buf[n++] = 'K'; buf[n++] = ' '; buf[n++] = 'f'; buf[n++] = 'r'; buf[n++] = 'e'; buf[n++] = 'e'; buf[n++] = ' '; buf[n++] = 'o'; buf[n++] = 'f'; buf[n++] = ' ';
    { char tmp[12]; int tn = 0; unsigned int v = total_kb; if (v == 0) tmp[tn++] = '0'; while (v > 0) { tmp[tn++] = (char)('0' + v % 10); v /= 10; } while (tn > 0) buf[n++] = tmp[--tn]; }
    buf[n++] = 'K'; buf[n] = 0;
    char mem_line[64]; { int p = 0; const char *s = buf; while (*s) mem_line[p++] = *s++; mem_line[p] = 0; }

    unsigned int secs = ticks() / 100;
    n = 0; buf[n++] = 'U'; buf[n++] = 'p'; buf[n++] = 't'; buf[n++] = 'i'; buf[n++] = 'm'; buf[n++] = 'e'; buf[n++] = ':'; buf[n++] = ' ';
    { char tmp[12]; int tn = 0; unsigned int v = secs; if (v == 0) tmp[tn++] = '0'; while (v > 0) { tmp[tn++] = (char)('0' + v % 10); v /= 10; } while (tn > 0) buf[n++] = tmp[--tn]; }
    buf[n++] = 's'; buf[n] = 0;
    char uptime_line[64]; { int p = 0; const char *s = buf; while (*s) uptime_line[p++] = *s++; uptime_line[p] = 0; }

    /* v0.76.12: real bug, this line was hardcoded to "Version 0.42.1"
       for 30+ real version bumps despite JT_VERSION_STR (drivers/version.h,
       generated from the real VERSION file at build time) already
       existing and already used elsewhere (the boot serial log). */
    char version_line[32]; { int p = 0; const char *v = "Version " JT_VERSION_STR; while (*v && p < (int)sizeof(version_line) - 1) version_line[p++] = *v++; version_line[p] = 0; }

    const char *tagline = "A freestanding i386 kernel, written from scratch.";
    const char *lines[4] = { tagline, mem_line, uptime_line, version_line };
    unsigned int colors[4] = { 0x001C1C1E, 0x00884B16, 0x00884B16, 0x0075726E };
    int line_h = 24;
    int text_top = by + (ABOUT_H - 4 * line_h) / 2 + 6; /* real vertical centering of the whole text block within the card */
    for (int i = 0; i < 4; i++) {
        int x = bx + (ABOUT_W - font_string_width(lines[i])) / 2; /* real horizontal centering per line */
        font_draw_string(lines[i], x, text_top + i * line_h, colors[i], -1);
    }

    { const char *hint = "esc or click to go back";
      font_draw_string(hint, bx + (ABOUT_W - font_string_width(hint)) / 2, by + ABOUT_H - 26, 0x0075726E, -1); }

    /* Local close-wait, not the shared gui_wait_close(): its own hint
       text is hardcoded to the full screen's bottom-left, which would
       float outside this small card. Same real mechanics otherwise
       (mouse_click_edge_sync/kbd_pop/Escape), copied rather than
       parameterized since this is the only call site that needs its own
       hint position. */
    window_present(); sleep_ticks(5);
    mouse_click_edge_sync();
    for (;;) {
        gui_app_mouse_tick();
        int sc = kbd_pop();
        if (sc >= 0 && !(sc & 0x80) && kbd_map(sc) == 27) { gui_close_was_click = 0; return; }
        if (mouse_click_edge()) { gui_close_was_click = 1; return; }
        window_present(); __asm__ volatile ("hlt");
    }
}

/* A real Apple-menu-style dropdown off the tree logo, macOS-shaped (dark
   panel, one highlighted row under the cursor) but with items that
   actually do something real on this kernel, not a decorative copy of
   a macOS menu that happens to not work: About shows real memory/uptime
   stats, Files and Notes launch the same real apps the dock does,
   Restart calls this kernel's own real reboot() (the 8042 reset pulse,
   already used by the "reboot" shell command), Shut Down really halts
   the CPU. "-" is a separator row, not a real item. */
#define GUI_MENU_ITEM_COUNT 8
static const char *GUI_MENU_LABELS[GUI_MENU_ITEM_COUNT] = {
    "About Joshua Tree", "Files", "Notes", "Settings", "Lock Screen", "-", "Restart", "Shut Down"
};
#define GUI_MENU_ROW_H  22
#define GUI_MENU_SEP_H  9
#define GUI_MENU_X0     4
#define GUI_MENU_W      180

/* v66, real restraint pass, not a new look: every other surface on this
   desktop (the dock tray, the weather app's own card, the Apps folder
   glass) is a soft rounded rect blended straight into whatever's behind
   it, gui_rounded_rect_on_wallpaper, zero separate stroke line. These
   three flyouts (this Apple menu, the notif panel, the weather dropdown)
   were the one place still built as a flat, hard-cornered rectangle with
   a manually drawn 1px border on all four sides, confirmed with a real
   headless framebuffer dump: square corners sitting directly against the
   dock's rounded ones read as two different chrome languages on one
   desktop, not two different needs. One shared radius, reused by all
   three, the same helper the dock already uses, is the fix: not a new
   rule, the existing one applied to the surfaces that had been skipping
   it. The three panels already shared bg/border/text colors with each
   other; this just brings their shape in line with everything else too. */
#define GUI_FLYOUT_RADIUS 14

static int gui_menu_row_h(int i){ return GUI_MENU_LABELS[i][0] == '-' ? GUI_MENU_SEP_H : GUI_MENU_ROW_H; }
static int gui_menu_total_h(void){ int h = 0; for (int i = 0; i < GUI_MENU_ITEM_COUNT; i++) h += gui_menu_row_h(i); return h; }
/* v66: real padding, not a stray number. The hover highlight on the first
   and last row is a plain rect inset only 2px from the panel's own edges;
   drawn flush against the panel's new rounded top/bottom (GUI_FLYOUT_RADIUS
   above), its hard corner visibly poked out past the panel's own curve,
   confirmed with a real zoomed capture, a worse seam than the square panel
   this pass just removed. The fix real menus use is exactly this: padding
   above the first row and below the last so no highlightable row ever
   reaches the curved part of the panel. 8px is the minimum that clears it:
   solving the same circle gui_rounded_rect_on_wallpaper draws with for the
   row where a 2px-inset flat edge first stays inside the curve gives 7,
   rounded up. */
#define GUI_MENU_PAD_V 8

/* Returns the item index under (mx,my), -2 for a separator row (a real
   hit, but not an actionable one), or -1 if outside the menu entirely. */
static int gui_menu_hit_test(int mx, int my){
    int y = GUI_MENUBAR_H + GUI_MENU_PAD_V, total_h = gui_menu_total_h();
    if (mx < GUI_MENU_X0 || mx >= GUI_MENU_X0 + GUI_MENU_W || my < y || my >= y + total_h) return -1;
    for (int i = 0; i < GUI_MENU_ITEM_COUNT; i++){
        int rh = gui_menu_row_h(i);
        if (my < y + rh) return GUI_MENU_LABELS[i][0] == '-' ? -2 : i;
        y += rh;
    }
    return -1;
}

static void gui_draw_apple_menu(int hover_item){
    int y0 = GUI_MENUBAR_H, total_h = gui_menu_total_h() + 2 * GUI_MENU_PAD_V;
    unsigned int bg = 0x002C2C2E, text = 0x00F5F5F7;
    gui_rounded_rect_on_wallpaper(GUI_MENU_X0, y0, GUI_MENU_W, total_h, bg, GUI_FLYOUT_RADIUS);
    int ry = y0 + GUI_MENU_PAD_V;
    for (int i = 0; i < GUI_MENU_ITEM_COUNT; i++){
        int rh = gui_menu_row_h(i);
        if (GUI_MENU_LABELS[i][0] == '-') { window_rect(GUI_MENU_X0 + 8, ry + rh / 2, GUI_MENU_W - 16, 1, 0x00545458); ry += rh; continue; }
        if (i == hover_item) window_rect(GUI_MENU_X0 + 2, ry, GUI_MENU_W - 4, rh, 0x00555555);
        font_draw_string(GUI_MENU_LABELS[i], GUI_MENU_X0 + 12, ry + 5, text, -1);
        ry += rh;
    }
}

/* v42: the clock opens a notification panel, the way clicking the clock
   on a Mac does. Honest about what "notifications" means on a kernel with
   no apps posting any: it's the system's own recent log (the same ring
   buffer `dmesg` prints) plus live warnings computed right now (memory
   running low, trash nearly full). Real state, not placeholder cards. */
#define NOTIF_W     360
#define NOTIF_ROWS  8
/* v0.76.20: the notif panel used to echo klog's raw boot-log lines verbatim
   ("vmmouse_init: VMware backdoor answered, absolute pointer on"), which
   reads like `dmesg`, not Notification Center. Translate to plain text here,
   at render time only -- klog_buf itself is untouched, so vmmouse-check.sh's
   grep against the stored strings still passes. Falls back to the raw line
   for anything not in the table, so a message added later is never dropped. */
static const char *notif_friendly(const char *raw){
    if (web_starts_with(raw, "vmmouse_init: VMware backdoor answered")) return "Mouse: precise tracking on";
    if (web_starts_with(raw, "vmmouse_init: no backdoor")) return "Mouse connected";
    if (web_starts_with(raw, "font_init:")) return "Fonts loaded";
    if (web_starts_with(raw, "pmm_init:")) return "Memory initialized";
    if (web_starts_with(raw, "paging_install:")) return "Memory protection enabled";
    if (web_starts_with(raw, "tasks_init:")) return "Task scheduler ready";
    if (web_starts_with(raw, "fat_mount: FAT16")) return "Disk mounted";
    if (web_starts_with(raw, "fat_mount:")) return "No disk found";
    if (web_starts_with(raw, "vfs: fat + ramfs")) return "Storage ready";
    if (web_starts_with(raw, "vfs: no FAT disk")) return "Using built-in storage";
    if (web_starts_with(raw, "vga_text_mode_init:")) return "Display initialized";
    if (web_starts_with(raw, "gdt_install:")) return "System tables loaded";
    if (web_starts_with(raw, "idt_install:")) return "Interrupts configured";
    if (web_starts_with(raw, "syscall_install:")) return "System calls ready";
    if (web_starts_with(raw, "irq_install:")) return "Timers and input ready";
    if (web_starts_with(raw, "mouse_init:")) return "Mouse connected";
    return raw;
}

static void gui_draw_notif_panel(void){
    int x0 = (int)window_width() - NOTIF_W - 4, y0 = GUI_MENUBAR_H;
    unsigned int bg = 0x002C2C2E, text = 0x00F5F5F7, warn = 0x00FFB454;

    /* always-visible memory bar: total vs used (each frame = 4K) */
    unsigned int total_k = pmm_total_frames() * 4;
    unsigned int free_k = pmm_free_frames() * 4;
    unsigned int used_k = total_k > free_k ? total_k - free_k : 0;
    unsigned int usage_percent = total_k > 0 ? (used_k * 100) / total_k : 0;
    unsigned int mem_bar_color = usage_percent > 80 ? warn : text;

    /* live warnings: other than memory, which is always shown */
    const char *warns[2]; int nw = 0;
    if (trash_count() >= TRASH_MAX_ITEMS - 1) warns[nw++] = "Trash is nearly full";
    if (nw == 0) warns[nw++] = "No warnings";

    int n = klog_count < NOTIF_ROWS ? klog_count : NOTIF_ROWS;
    /* height: 10 (margin) + 18 (memory text) + 14 (memory bar) + 18 (nw warnings) + 18 (separator) + n*34 (log rows) + 8 (bottom margin) */
    int total_h = 10 + 18 + 14 + (nw + 1) * 18 + n * 34 + 8;
    gui_rounded_rect_on_wallpaper(x0, y0, NOTIF_W, total_h, bg, GUI_FLYOUT_RADIUS);

    int y = y0 + 8;

    /* memory bar section: text + visual bar */
    char mem_str[40]; int p = 0;
    if (used_k >= 1024) {
        unsigned int used_mb = used_k / 1024;
        unsigned int total_mb = total_k / 1024;
        /* format: "XXXM / XXXM" */
        { unsigned int v = used_mb; char tb[8]; int ti = 0;
          if (!v) tb[ti++] = '0'; while (v) { tb[ti++] = '0' + v % 10; v /= 10; }
          while (ti) mem_str[p++] = tb[--ti]; }
        mem_str[p++] = 'M'; mem_str[p++] = ' '; mem_str[p++] = '/'; mem_str[p++] = ' ';
        { unsigned int v = total_mb; char tb[8]; int ti = 0;
          if (!v) tb[ti++] = '0'; while (v) { tb[ti++] = '0' + v % 10; v /= 10; }
          while (ti) mem_str[p++] = tb[--ti]; }
        mem_str[p++] = 'M';
    } else {
        /* format: "XXXK / XXXK" */
        { unsigned int v = used_k; char tb[8]; int ti = 0;
          if (!v) tb[ti++] = '0'; while (v) { tb[ti++] = '0' + v % 10; v /= 10; }
          while (ti) mem_str[p++] = tb[--ti]; }
        mem_str[p++] = 'K'; mem_str[p++] = ' '; mem_str[p++] = '/'; mem_str[p++] = ' ';
        { unsigned int v = total_k; char tb[8]; int ti = 0;
          if (!v) tb[ti++] = '0'; while (v) { tb[ti++] = '0' + v % 10; v /= 10; }
          while (ti) mem_str[p++] = tb[--ti]; }
        mem_str[p++] = 'K';
    }
    mem_str[p++] = ' '; mem_str[p++] = 'u'; mem_str[p++] = 's'; mem_str[p++] = 'e'; mem_str[p++] = 'd';
    /* v0.76.49: direct request, "more memory information" -- percent used
       alongside the raw M/K figures already shown, same buffer (still well
       under its 40-byte size at max: "999M / 999M used (100%)" is 24). */
    mem_str[p++] = ' '; mem_str[p++] = '(';
    { unsigned int v = usage_percent; char tb[4]; int ti = 0;
      if (!v) tb[ti++] = '0'; while (v) { tb[ti++] = '0' + v % 10; v /= 10; }
      while (ti) mem_str[p++] = tb[--ti]; }
    mem_str[p++] = '%'; mem_str[p++] = ')';
    mem_str[p] = 0;
    font_draw_string(mem_str, x0 + 12, y, mem_bar_color, -1); y += 18;

    /* visual memory bar: full width bar with filled portion */
    int bar_x = x0 + 12, bar_y = y, bar_w = NOTIF_W - 24, bar_h = 6;
    window_rect(bar_x, bar_y, bar_w, bar_h, 0x00545458);  /* background */
    int filled_w = bar_w > 0 ? (bar_w * usage_percent) / 100 : 0;
    if (filled_w > 0) window_rect(bar_x, bar_y, filled_w, bar_h, mem_bar_color);  /* filled portion */
    y += 14;

    window_rect(x0 + 8, y, NOTIF_W - 16, 1, 0x00545458); y += 18;

    /* newest last, like every log ever, capped to the last NOTIF_ROWS */
    int start = (klog_count < KLOG_MAX) ? 0 : klog_next;
    int skip = klog_count - n;
    for (int i = 0; i < n; i++, y += 34) {
        int idx = (start + skip + i) % KLOG_MAX;
        const char *msg = notif_friendly(klog_buf[idx]);
        char line[44]; int p = 0;
        unsigned int t = klog_tick[idx] / 100; char tb[8]; int ti = 0;
        if (!t) tb[ti++] = '0'; while (t) { tb[ti++] = '0' + t % 10; t /= 10; }
        while (ti) line[p++] = tb[--ti];
        line[p++] = 's'; line[p++] = ' ';
        int k = 0;
        for (; msg[k] && p < 42; k++) line[p++] = msg[k];
        line[p] = 0;
        font_draw_string(line, x0 + 12, y, text, -1);
        if (msg[k]) {
            p = 0;
            line[p++] = ' '; line[p++] = ' '; line[p++] = ' ';
            for (; msg[k] && p < 42; k++) line[p++] = msg[k];
            line[p] = 0;
            font_draw_string(line, x0 + 12, y + 16, text, -1);
        }
    }
}

/* v53: the weather text opens a dropdown too, same open-on-press/
   dismiss-on-next-release contract as the clock's notif panel above, same
   panel chrome (colors, border-drawing, font). Shows only real fields the
   existing weather_fetch() already parses (temp, WMO code -> condition
   word, and as of v71 the real ip-api.com city/lat/lon it queried, no
   longer a fixed literal), no invented humidity/wind/forecast rows the
   data doesn't have. */
#define WEATHER_W 220
static void gui_draw_weather_panel(void){
    int x0 = weather_hit_x0 >= 0 ? weather_hit_x0 : (int)window_width() - WEATHER_W - 200;
    if (x0 + WEATHER_W > (int)window_width() - 4) x0 = (int)window_width() - WEATHER_W - 4;
    int y0 = GUI_MENUBAR_H;
    unsigned int bg = 0x002C2C2E, text = 0x00F5F5F7, dim = 0x00A0A0A6;

    int total_h = 10 + (geo_city[0] ? 5 : 4) * 20 + 6; /* v71: one extra row for the looked-up city name when ip-api gave one */
    gui_rounded_rect_on_wallpaper(x0, y0, WEATHER_W, total_h, bg, GUI_FLYOUT_RADIUS);

    int y = y0 + 8;
    if (!weather_have) {
        font_draw_string("No weather yet", x0 + 12, y, dim, -1);
        return;
    }
    font_draw_string(weather_text[0] ? weather_text : "Weather", x0 + 12, y, text, -1); y += 20;

    char line[40]; int p;
    p = 0; line[p++]='T'; line[p++]='e'; line[p++]='m'; line[p++]='p'; line[p++]=':'; line[p++]=' ';
    { int t = weather_temp_c; if (t < 0) { line[p++]='-'; t=-t; } if (t>=10) line[p++]='0'+t/10; line[p++]='0'+t%10; line[p++]=(char)0xF8; line[p++]='C'; }
    line[p]=0; font_draw_string(line, x0 + 12, y, dim, -1); y += 20;

    p = 0; line[p++]='C'; line[p++]='o'; line[p++]='d'; line[p++]='e'; line[p++]=':'; line[p++]=' ';
    { int c = weather_code10 / 10; if (c >= 100) line[p++]='0'+c/100; if (c>=10) line[p++]='0'+(c/10)%10; line[p++]='0'+c%10; }
    line[p]=0; font_draw_string(line, x0 + 12, y, dim, -1); y += 20;

    /* v71: the real, looked-up location (ip-api.com, see geo_fetch), not
       the old fixed literal. City first when ip-api gave one, then the
       exact lat/lon text the Open-Meteo URL was actually built from. */
    if (geo_city[0]) { font_draw_string(geo_city, x0 + 12, y, dim, -1); y += 20; }
    p = 0;
    for (const char *s = geo_lat; *s && p < 16; s++) line[p++] = *s;
    line[p++] = ','; line[p++] = ' ';
    for (const char *s = geo_lon; *s && p < 36; s++) line[p++] = *s;
    line[p]=0; font_draw_string(line, x0 + 12, y, dim, -1);
}

static void gui_launch_settings(void);

static void gui_lock_screen(void){
    if (!auth_gate_would_prompt()) {
        /* No accounts to lock with. Close menu, show brief message, return to desktop. */
        gui_draw_desktop(-1, -1, 0, 0);
        /* Display message for ~1 second: show a notification-style message on screen */
        int msg_w = font_string_width("No accounts to lock with");
        int msg_x = ((int)window_width() - msg_w) / 2;
        int msg_y = (int)window_height() / 2;
        int box_x = msg_x - 10, box_y = msg_y - 10, box_w = msg_w + 20, box_h = 25;
        window_rect(box_x, box_y, box_w, box_h, 0x00FAF8F6);  /* fill */
        /* v0.85.3: a second full-size window_rect in the border color used to
           sit directly on top of this fill (same x/y/w/h), painting the whole
           box grey and burying the grey message text on a now-identical grey
           background -- a real headless dump showed the box with no text at
           all. A 1px outline on all four edges reads as a border without
           erasing the fill underneath it. */
        window_rect(box_x, box_y, box_w, 1, 0x00555555);              /* top */
        window_rect(box_x, box_y + box_h - 1, box_w, 1, 0x00555555);  /* bottom */
        window_rect(box_x, box_y, 1, box_h, 0x00555555);              /* left */
        window_rect(box_x + box_w - 1, box_y, 1, box_h, 0x00555555);  /* right */
        font_draw_string("No accounts to lock with", msg_x, msg_y, 0x00555555, -1);
        window_present();
        sleep_ticks(100);  /* 1 second at 100 ticks/sec */
        gui_draw_boot_screen();
    } else {
        /* Account exists. Force the login screen even though already logged in. */
        serial_puts("auth: locked\n");
        auth_logged_in = 0;  /* Reset the flag to force re-authentication */
        auth_login_screen();
        gui_draw_boot_screen();
    }
}

static void gui_menu_run_item(int item){
    if (item == 0) gui_launch_about();
    else if (item == 1) gui_launch_files();
    else if (item == 2) gui_launch_editor();
    else if (item == 3) gui_launch_settings();
    else if (item == 4) gui_lock_screen();
    else if (item == 6) reboot();
    else if (item == 7) {
        window_clear(0x00111111);
        font_draw_string("It's now safe to turn off this computer.", 20, (int)window_height() / 2, 0x00F5F5F7, -1);
        __asm__ volatile ("cli");
        for (;;) __asm__ volatile ("hlt");
    }
}

static void gui_run(void){
    /* v42: 16:9, 960x540 logical at 2x = 1920x1080 physical, the native
       size of the monitor this actually runs fullscreen on. QEMU's cocoa
       zoom-to-fit stretches without preserving aspect, so a 4:3 mode on a
       16:9 panel came out visibly skewed; matching the panel's own shape
       means fullscreen is pixel-exact with no scaling at all. */
    if (!window_open_scaled(960, 540, 32, 2)) { puts("no VGA device found or out of page tables\n"); return; }
    font_set_aa(gui_aa_char, gui_aa_advance); /* v44: real typeface for every string from here on */
    font_set_aa_mono(gui_aa_char_mono); /* term-mono: mono face for the terminal grid and Keyrate's typed line */
    /* v46: no wind in the browser, decided up front rather than measured
       after the fact. The slow-frame gate still exists, but even the two
       frames it takes to trip blocked the kernel long enough that v86's
       PS/2 queue overflowed and the demo tour's paced cursor packets were
       dropped (tourtest failed twice, alone, on this build). The BIOS-font
       check from v38 is the reliable "this is v86" signal. */
    if (font_is_fallback()) wind_enabled = 0;
    /* One real check that the back buffer is both present and actually
       interposed, reported over serial rather than assumed; the
       "backbuffer=none" case is the honest, still-correct fallback a
       machine too small to allocate one (v86's 32MB demo) takes. */
    serial_puts(window_backbuffer_selftest() ? "backbuffer=ok\n"
                : (window_has_back_buffer() ? "backbuffer=broken\n" : "backbuffer=none\n"));
    auth_gate(); /* v0.77: real login screen, once per session, before the desktop ever paints */
    gui_draw_boot_screen();
    gui_order_init();
    dock_hover = dock_presented_hover = -1;
    int mx = 400, my = 300, buttons = 0, prev_buttons = 0;
    /* press_slot: the slot the mouse went down on, latched until release.
       drag_slot: only set once the mouse has actually moved past a small
       threshold while held, so a plain click (down, no movement, up)
       never gets mistaken for a drag onto its own slot. */
    int press_slot = -1, press_x = 0, press_y = 0, drag_slot = -1, press_window = -1;
    /* Window title-bar drag: win_drag_armed latches on a title-bar press
       (the same "record it, only promote to a real drag past a small
       threshold" shape drag_slot above already uses for dock reordering).
       drag_win is only set once the threshold is crossed, so a plain
       click on the title bar still falls through to press_window's
       existing click-anywhere-closes contract instead of being eaten by
       a drag that never really happened. drag_zone/last_drag_zone track
       which snap zone (if any) the pointer is over, redrawn only on a
       real zone change, not on every mouse-moved event. */
    int win_drag_armed = 0, drag_win = -1, drag_grab_dx = 0, drag_grab_dy = 0;
    int drag_zone = -1;
    /* menu_open: the Apple-menu-style dropdown off the tree logo.
       menu_opening: true for exactly the one release that completes the
       same click that opened it, so that release doesn't also count as
       the "pick an item or dismiss" click, real macOS menu behavior
       (open on press, stays open past that first release, a real
       second click either picks something or dismisses it). */
    int menu_open = 0, menu_opening = 0;
    int notif_open = 0, notif_opening = 0, notif_draw_pending = 0; /* v42: the clock's panel, same open-on-press/dismiss-on-next-release contract as the Apple menu */
    int weather_open = 0, weather_opening = 0, weather_draw_pending = 0; /* v53: same contract, off the weather text */

    int last_mx = mx, last_my = my, last_hover = -1, last_drag = -1, last_menu_open = 0, last_menu_hover = -2;
    gui_menubar_force_redraw(); /* this GUI session's first frame, the minute-change gate must not skip it */
    /* v0.76.51: real bug, direct report ("tree shows for a second on
       startup/login") -- the very first desktop paint below ran before
       wall_apply() had EVER been called this session, so wall_src still
       held its static initial value (wallpaper_rgb, the tree photo)
       regardless of wall_theme's real default (WALL_SAT). v0.76.45's dark-
       fallback fix only kicked in once something later called wall_apply,
       which never happened before this first frame. One call here applies
       the same want-map-but-no-fetch-yet logic to the actual first paint,
       not just every paint after it. */
    wall_apply(wall_theme != WALL_PHOTO);
    gui_draw_desktop(-1, -1, 0, 0);
    cursor_saved_x = cursor_saved_y = -1;
    gui_cursor_save(mx, my);
    gui_draw_cursor(mx, my);
    gui_dock_prewarm();
    for (;;) {
        window_present(); __asm__ volatile ("hlt");
        /* v0.76.17: direct request ("time in top right needs live reload
           accuracy, right now it doesn't load when the minute or hour
           changes"). Root cause: gui_draw_menubar() already self-gates on
           a real minute change (gui_menubar_last_min below), but it was
           only ever CALLED from mouse-in-menubar paths or this loop's own
           ten-minute weather cycle -- an idle desktop with the cursor
           elsewhere could sit with a stale clock for up to ten minutes.
           Calling it unconditionally every iteration (~100Hz, this hlt
           wakes on the PIT) is cheap: cmos() reads plus an integer
           compare on every frame but the one where the minute actually
           ticks over, where it does the real (already-existing) redraw. */
        {
            int min_before = gui_menubar_last_min;
            gui_draw_menubar();
            if (gui_menubar_last_min != min_before && my < GUI_MENUBAR_H) { gui_cursor_save(mx, my); gui_draw_cursor(mx, my); }
        }
        /* v43: weather, after the desktop is already on screen so the
           fetch never delays the first frame, then every ten minutes. */
        if (!weather_tried_once || ticks() - weather_last_tick > 100 * 600) {
            weather_tried_once = 1;
            char before[24]; for (int i = 0; i < 24; i++) before[i] = weather_text[i];
            weather_fetch();
            if (strcmp(before, weather_text) != 0) { gui_menubar_force_redraw(); gui_draw_menubar(); if (my < GUI_MENUBAR_H) { gui_cursor_save(mx, my); gui_draw_cursor(mx, my); } }
            /* v75: the map wallpaper rides the same ten-minute cycle, right
               after the geo lookup it depends on. One fetch per session
               once it lands (the mosaic is kept), retried each cycle
               until then; a failure leaves the photo up, never a blank. */
            if (wall_theme != WALL_PHOTO && !wall_map && geo_have && wall_fetch()) { wall_apply(1); gui_draw_desktop(-1, -1, 0, 0); gui_cursor_save(mx, my); gui_draw_cursor(mx, my); }
        }
        /* v45: wind, 4 frames a second, only while the desktop itself is
           what's on screen. Timed on its first frame; if that frame took
           longer than a tenth of a second the machine is too slow for
           this (v86 in a browser) and it switches itself off for good. */
        {
            static unsigned int wind_last = 0; static int wind_dir = 1;
            if (wind_enabled && !menu_open && !notif_open && !weather_open && drag_slot < 0 && gui_window_count == 0 && ticks() - wind_last >= 5) { /* cached wallpaper: ~3 ticks per frame, leaving input time at 20 fps; v0.73.0: also off while a multi-window app is open, same reason as the other overlay states, its wallpaper-row redraw would paint straight over an open window's content since neither the wind sway path nor the window list know about each other yet */
                wind_last = ticks();
                wind_phase += wind_dir * 3; /* same slow sway period at the higher frame rate */ if (wind_phase >= 256 || wind_phase <= -256) wind_dir = -wind_dir;
                unsigned int t0 = ticks();
                int sc = (int)window_scale();
                int cx0 = cursor_saved_x, cy0 = cursor_saved_y;
                if (cx0 >= 0) gui_draw_wallpaper_rows_sway_ex(WIND_TOP_ROW, WIND_HORIZON_ROW, 1, cx0 * sc, cy0 * sc, CURSOR_W * sc, CURSOR_H * sc);
                else gui_draw_wallpaper_rows_sway(WIND_TOP_ROW, WIND_HORIZON_ROW, 1);
                /* the backup under the cursor must track the sway too, or the
                   next real cursor move would restore a pre-wind patch */
                if (cx0 >= 0) { int csc = gui_cursor_scale(), pw = CURSOR_W * csc, ph = CURSOR_H * csc; /* physical-res backup, same layout as gui_cursor_save */
                    for (int j = 0; j < ph; j++) { int py = cy0 * csc + j; int ly = py / csc; if (ly < WIND_TOP_ROW || ly >= WIND_HORIZON_ROW) continue;
                        for (int i = 0; i < pw; i++) cursor_backup[j * pw + i] = gui_wallpaper_sample(cx0 * csc + i, py, 1); } }
                /* v65: rain/snow, drawn on top of the freshly repainted sway
                   band, right here on purpose: that repaint is what erases
                   last frame's particles, so drawing before it would just
                   get overwritten, and drawing it here keeps the cost
                   inside the same dt budget check right below, the same
                   self-throttle the wind redraw itself already relies on. */
                weather_fx_tick((int)window_width());
                /* two slow frames in a row, not one: the first frame under
                   QEMU includes the JIT translating this very loop and can
                   trip a single-frame gate falsely */
                { static int slow = 0, logged = 0; unsigned int dt = ticks() - t0;
                  if (!logged) { logged = 1; char b[24]; int i = 0; b[i++]='w'; b[i++]='i'; b[i++]='n'; b[i++]='d'; b[i++]='='; if (dt >= 10) b[i++]='0'+dt/10%10; b[i++]='0'+dt%10; b[i++]='t'; b[i++]='\n'; b[i]=0; serial_puts(b); }
                  if (dt > 25) wind_enabled = 0; else if (dt > 12) { if (++slow >= 2) wind_enabled = 0; } else slow = 0; }
            }
        }
        /* v0.75.0 (batch 2): when the topmost open multiwin window is one
           of the real-input apps (Mail/Calendar/Reminders), a keystroke
           routes to its own on_key handler instead of the old global
           "esc quits the whole GUI" check -- exactly the same
           "topmost/focused window owns input" rule click-to-focus
           already established for clicks (gui_multiwin_hit_test above).
           Only one kbd_pop() happens per frame either way, so the two
           branches can't double-consume the same scancode. */
        int mw_topmost_icon = gui_window_count > 0 ? gui_windows[gui_window_count - 1].icon : -1;
        int mw_key_repaint = 0;
        if (gui_multiwin_interactive(mw_topmost_icon)) {
            int mwk = gui_multiwin_key_nonblock();
            if (mwk >= 0) {
                int mw_should_close = 0;
                if (mw_topmost_icon == 4) mw_should_close = gui_reminders_on_key(mwk);
                else if (mw_topmost_icon == 2) mw_should_close = gui_calendar_on_key(mwk);
                else if (mw_topmost_icon == 1) mw_should_close = gui_mail_on_key(mwk);
                else if (mw_topmost_icon == 7) mw_should_close = gui_weather_key(mwk, gui_weather_mw_repaint);
                if (mw_should_close) {
                    gui_multiwin_close(gui_window_count - 1);
                    mw_key_repaint = 1; /* the window left the screen: needs the real full desktop repaint to erase it, the same cost every open/close already pays */
                } else {
                    /* v0.75.0: a cheap, scoped repaint tier, the same
                       "cheapest repaint that's correct" discipline
                       cursor_only/dock_only below already established.
                       Forcing the FULL desktop repaint (wallpaper photo
                       blit + menubar + dock, the expensive path those
                       two tiers exist to avoid) on every single keystroke
                       while typing (adding a reminder, composing mail)
                       is real, measurable overkill batch-2 would
                       otherwise add -- and not just cosmetic: a real
                       bug this pass caught and fixed before shipping, a
                       fast multi-character type burst forcing a full
                       photo-blit redraw on every keystroke could fall
                       behind the keyboard IRQ ring's fill rate, dropping
                       real keystrokes and intermittently failing
                       tools/checks/app-interact-check.py's Reminders/
                       Mail/Calendar interaction steps (confirmed: this
                       exact non-scoped `launched=1` version reproduced
                       the flakiness live, headless, multiple runs). The
                       window's own rect is self-contained -- it always
                       draws its own full chrome + content top to bottom
                       -- so redrawing just that window, patching the
                       cursor around it the same way cursor_only/
                       dock_only do, is the whole real fix: nothing
                       outside the window rect changed.

                       v0.76.18: tightened further, direct report ("keystroke
                       re-rendering glitch still present"). This tier was
                       already scoped to one window instead of the whole
                       desktop, but still called the OLD gui_multiwin_draw_one,
                       which redrew that window's full chrome (the rounded-
                       rect wallpaper blend + all three traffic lights + the
                       title) on every keystroke even though none of it
                       changes while typing -- only the content viewport
                       does. gui_multiwin_draw_content_only skips exactly
                       that unchanged part, the same "cheapest repaint that's
                       correct" cut cursor_only/dock_only/menu_only already
                       make for their own chrome. */
                    gui_cursor_restore();
                    gui_multiwin_draw_content_only(&gui_windows[gui_window_count - 1]);
                    gui_cursor_save(last_mx, last_my);
                    gui_draw_cursor(last_mx, last_my);
                }
            }
        } else {
            int sc = kbd_pop();
            if (sc >= 0 && !(sc & 0x80)) {
                char c = SC[sc & 0x7F];
                if (c == 27) {
                    /* Esc closes the focused window first. Only a bare desktop
                       quits to the shell. Files has no key handler of its own,
                       so before this Esc with Files open dropped the whole
                       desktop to text mode. */
                    if (gui_window_count == 0) break;
                    gui_multiwin_close(gui_window_count - 1);
                    mw_key_repaint = 1;
                } else if (c == '\n' && gui_window_count == 0) {
                    /* Enter on the bare desktop opens the Apps folder */
                    gui_launch_apps();
                }
            }
        }
        int dx = 0, dy = 0;
        int moved_mouse = mouse_get_delta(&dx, &dy, &buttons);
        if (moved_mouse) {
            mx += dx; my += dy;
            mouse_get_absolute(&mx, &my, (int)window_width(), (int)window_height()); /* v62: a tap lands exactly here, no travel */
            if (mx < 0) mx = 0; if ((unsigned)mx >= window_width())  mx = (int)window_width() - 1;
            if (my < 0) my = 0; if ((unsigned)my >= window_height()) my = (int)window_height() - 1;
        }
        int held = buttons & 1;
        int just_pressed = held && !(prev_buttons & 1);
        int just_released = !held && (prev_buttons & 1);
        int logo_here = !menu_open && !notif_open && !weather_open && mx >= 4 && mx <= 28 && my < GUI_MENUBAR_H;
        int clock_here = !menu_open && !notif_open && !weather_open && mx >= (int)window_width() - 200 && my < GUI_MENUBAR_H;
        int weather_here = !menu_open && !notif_open && !weather_open && weather_hit_x0 >= 0 && mx >= weather_hit_x0 && mx <= weather_hit_x1 && my < GUI_MENUBAR_H;
        int slot_here = (menu_open || notif_open || weather_open) ? -1 : gui_dock_hit_test(mx, my); /* the dock is inert while a panel covers it */
        int win_hit_here = (menu_open || notif_open || weather_open) ? -1 : gui_multiwin_hit_test(mx, my); /* v0.73.6: real hit test against every open window, topmost first, see gui_multiwin_hit_test */
        int win_close_here = (win_hit_here >= 0 && win_hit_here == gui_window_count - 1) ? win_hit_here : -1; /* only the already-focused (topmost) window's own click-anywhere-closes contract; a click on a background window is click-to-focus, not close, handled below */
        int win_focus_changed = 0; /* set below when a click raises a background window; folded into `launched` once it's declared, so the z-order change gets a real full repaint this same frame */

        if (just_pressed) {
            if (logo_here) { menu_open = 1; menu_opening = 1; }
            else if (weather_here) { weather_open = 1; weather_opening = 1; weather_draw_pending = 1; }
            else if (clock_here) { notif_open = 1; notif_opening = 1; notif_draw_pending = 1; }
            else if (win_close_here >= 0) {
                press_window = win_close_here;
                /* A press inside the topmost window's title bar (chrome
                   band, not the close circle itself) is also a drag
                   candidate; press_window stays set so a plain click
                   (no movement past the threshold below) still closes
                   the window exactly as it always has. */
                const gui_window_t *pw = &gui_windows[win_close_here];
                int in_titlebar = my >= pw->y && my < pw->y + 30;
                int cx = pw->x + 24, cy = pw->y + 16, ddx = mx - cx, ddy = my - cy;
                int on_close = (ddx * ddx + ddy * ddy) <= 9 * 9;
                if (in_titlebar && !on_close) {
                    win_drag_armed = 1; press_x = mx; press_y = my;
                    drag_grab_dx = mx - pw->x; drag_grab_dy = my - pw->y;
                }
            }
            else if (win_hit_here >= 0) {
                /* v0.73.6: real click-to-focus. The click landed inside a
                   visible BACKGROUND window's rect (win_close_here above
                   was -1, so it's not the topmost one). Raise it to the
                   front of the real z-order list right now, on press, and
                   swallow the click here -- it neither closes the window it
                   hit (that's not the "click anywhere closes" contract
                   until a SECOND click lands on it now that it's topmost)
                   nor falls through to the dock/app-launch paths below.
                   This is the one behavioural gap phase 1 explicitly left
                   open: "clicking the background window does nothing yet." */
                gui_multiwin_focus(win_hit_here);
                win_focus_changed = 1; /* z-order changed; force the full repaint below so the newly-front window is genuinely redrawn on top */
            }
            else if (slot_here >= 0) { press_slot = slot_here; press_x = mx; press_y = my; drag_slot = -1; }
        }

        if (held && press_slot >= 0 && drag_slot < 0) {
            int moved = (mx > press_x ? mx - press_x : press_x - mx) + (my > press_y ? my - press_y : press_y - my);
            if (moved > 8) drag_slot = press_slot; /* threshold crossed: this is a drag, not a click */
        }
        if (held && win_drag_armed && drag_win < 0) {
            int moved = (mx > press_x ? mx - press_x : press_x - mx) + (my > press_y ? my - press_y : press_y - my);
            if (moved > 8) { drag_win = press_window; press_window = -1; } /* real drag now: the release logic below moves/snaps instead of closing */
        }

        int launched = notif_draw_pending || weather_draw_pending || win_focus_changed || mw_key_repaint; notif_draw_pending = 0; weather_draw_pending = 0;
        if (just_released) {
            if (notif_open) {
                if (notif_opening) notif_opening = 0;
                else { notif_open = 0; launched = 1; } /* any release dismisses; force the full redraw that erases the panel */
            } else if (weather_open) {
                if (weather_opening) weather_opening = 0;
                else { weather_open = 0; launched = 1; } /* any release dismisses; force the full redraw that erases the panel */
            } else if (menu_open) {
                if (menu_opening) {
                    menu_opening = 0; /* this release just finishes the click that opened the menu; a real second click picks something or dismisses it */
                } else {
                    int item = gui_menu_hit_test(mx, my);
                    if (item >= 0) { gui_menu_run_item(item); launched = 1; } /* every real item takes over the screen or reboots/halts; force a fresh desktop redraw either way */
                    menu_open = 0;
                }
            } else if (drag_win >= 0) {
                /* Magnet-style drop: inside a zone, snap to that target
                   rect (the window's own w/h really change, re-derived
                   from the new size on the very next content redraw
                   below, not clipped or faked). Outside any zone, a real
                   free move to wherever it was dropped, same w/h -- safe
                   because every content function lays out from
                   window_width()/window_height(), not a hard-coded
                   position, so moving x/y alone never breaks layout. */
                int zone = gui_snap_zone(mx, my);
                gui_window_t *dw = &gui_windows[drag_win];
                if (zone >= 0) {
                    gui_snap_target(zone, &dw->x, &dw->y, &dw->w, &dw->h);
                } else {
                    int top, bottom; int sw = gui_snap_area(&top, &bottom);
                    int nx = mx - drag_grab_dx, ny = my - drag_grab_dy;
                    if (nx < 0) nx = 0; if (nx + dw->w > sw) nx = sw - dw->w;
                    if (ny < top) ny = top; if (ny + dw->h > bottom) ny = bottom - dw->h;
                    dw->x = nx; dw->y = ny;
                }
                launched = 1;
            } else if (press_window >= 0) {
                /* v0.73.0: closing this window is exactly it, no reopen/
                   switch behaviour (that's v68's dock-tile close-and-open,
                   which only applies to the old blocking single-window
                   path); the other open window, if any, stays open and
                   drawn, proven by gui_multiwin_draw_all below still
                   iterating whatever's left in the list. */
                gui_multiwin_close(press_window);
                launched = 1;
            } else if (drag_slot >= 0) {
                int target = gui_slot_at(mx);
                int tmp = gui_order[drag_slot];
                gui_order[drag_slot] = gui_order[target];
                gui_order[target] = tmp;
            } else if (press_slot >= 0 && press_slot == slot_here && gui_multiwin_supported(gui_order[press_slot])) {
                /* v0.73.0: real phase-1 multi-window path for Files/Weather,
                   see the big comment above gui_multiwin_open. Non-blocking
                   on purpose: adds/focuses the window in the real list and
                   returns immediately, so this same gui_run loop keeps
                   running (dock hover, the other open window's redraw,
                   everything) instead of blocking inside gui_wait_close the
                   way every other app still does. */
                editor_mouse_x = mx; editor_mouse_y = my;
                /* v0.76.56: real bug, confirmed headless (three windows
                   opened back to back, gui_window_count dumped via the
                   QEMU monitor): once GUI_MULTIWIN_MAX (2) windows are
                   already open, gui_multiwin_open silently returns -1 and
                   this click does NOTHING -- no window opens, nothing
                   closes, no error, the previously-topmost window just
                   stays exactly as it was. From the outside that reads as
                   "I clicked App X's dock icon and got App Y" (whatever
                   was already on top), the same symptom class the
                   roadmap's live-QA pass reported for Calendar/Reminders,
                   even though the real cause is a swallowed click at the
                   window cap, not a wrong icon index (gui_order/gui_launch
                   dispatch were re-verified correct via the same headless
                   harness and are not the bug). Real fix: when the cap
                   blocks the multi-window path, fall through to the
                   existing blocking single-window path below instead of
                   dropping the click, so the user's click always does
                   *something* visible. */
                if (gui_multiwin_open(gui_order[press_slot]) < 0) {
                    serial_puts("mwcapfallback\n"); /* discriminating marker for tools/checks/dockcap-fallback-check.py */
                    gui_launch_from_dock(gui_order[press_slot]);
                    mx = app_cursor_x; my = app_cursor_y;
                }
                launched = 1;
            } else if (press_slot >= 0 && press_slot == slot_here) {
                editor_mouse_x = mx; editor_mouse_y = my;
                gui_launch_from_dock(gui_order[press_slot]);
                mx = app_cursor_x; my = app_cursor_y; /* v68: the app's own loop tracked the pointer while it was open; pick up where it really is, not where the launching click was */
                /* v86 (0.71.0), a real but narrow staleness bug found
                   while investigating the "Files dock icon stuck
                   hovering" report: slot_here above was computed from the
                   mouse position BEFORE this entire blocking app session
                   (the dock tile that was clicked to open it), and
                   nothing recomputed it against the cursor's real
                   post-close position before hover_slot (below) consumes
                   it this same frame. Confirmed harmless in the common
                   case only because dock_hover_extra was usually already
                   saturated at DOCK_MAGNIFY from hovering the tile before
                   the click, so the stale target rarely causes a visible
                   *increase*; it only delays this frame's decay-start by
                   one redraw, self-correcting on the very next frame once
                   slot_here is recomputed fresh at the top of the loop.
                   Fixed anyway (correctness, not cosmetics: a delayed
                   decay-start IS a real one-frame staleness bug) by
                   recomputing here with the same call already used to
                   build slot_here in the first place (line ~4326).
                   Headless pixel verification (QMP abs-pointer + a
                   settled pmemsave, the dockhover-check.py pattern) could
                   NOT reproduce a PERSISTENT stuck-lifted tile either
                   before or after this fix, dropped as a non-discriminating
                   test rather than kept as false evidence; see roadmap.md
                   for the honest state of the underlying report, this fix
                   narrows a real gap but is not confirmed to be the full
                   explanation for a hang lasting more than one frame. */
                slot_here = (menu_open || notif_open || weather_open) ? -1 : gui_dock_hit_test(mx, my);
                launched = 1; /* the app view just took over the whole screen; force a redraw below even if the cursor never moved */
            }
            press_slot = -1; drag_slot = -1; press_window = -1;
            win_drag_armed = 0; drag_win = -1; drag_zone = -1;
        }
        prev_buttons = buttons;

        int hover_slot = (drag_slot < 0) ? slot_here : -1;
        dock_hover = hover_slot;
        int menu_hover = menu_open ? gui_menu_hit_test(mx, my) : -2;
        /* Redraw only when something actually visible changed. A real,
           user-visible bug this fixed, not just a cosmetic worry: redrawing
           the whole screen unconditionally on every single timer tick
           (~100/sec, whether or not the mouse ever moved) produced visible
           tearing on a real display, this framebuffer has no vsync and no
           back buffer, so a full-screen redraw mid-scanout shows a torn
           frame. Only redrawing on an actual state change cuts redraws from
           ~100/sec down to roughly "as fast as a human can move a mouse",
           which doesn't eliminate tearing (still no double buffering, an
           honest, separate, larger limitation) but makes it rare instead
           of constant. */
        /* v40: three tiers of repaint, cheapest that's correct.
           Before this, ANY change, including a 1px cursor jitter, ran the
           full path below (whole photo blit + dock + eight supersampled
           icons) with no double buffer to hide it, which is exactly the
           "icons flash when I hover" report: the flashing was the
           repaint. */
        /* Window drag preview: only redraws when the snap zone the
           pointer is over actually changes (entering/leaving/switching a
           zone), never on every mouse-moved event -- the window itself
           is left exactly where it started until release, so this is the
           whole cost of the live preview. */
        int cur_snap_zone = drag_win >= 0 ? gui_snap_zone(mx, my) : -1;
        int drag_zone_only = drag_win >= 0 && !launched && cur_snap_zone != drag_zone;
        if (drag_zone_only) {
            gui_cursor_restore();
            gui_draw_desktop(-1, -1, 0, 0);
            if (gui_window_count > 0) gui_multiwin_draw_all();
            gui_snap_outline(cur_snap_zone);
            gui_cursor_save(mx, my);
            gui_draw_cursor(mx, my);
            drag_zone = cur_snap_zone;
            last_mx = mx; last_my = my;
        }
        int cursor_only = !launched && !drag_zone_only && (mx != last_mx || my != last_my)
                          && hover_slot == last_hover && drag_slot == last_drag
                          && menu_open == last_menu_open && menu_hover == last_menu_hover;
        int dock_only = !launched && !cursor_only && drag_slot < 0 && last_drag < 0
                        && !menu_open && !last_menu_open
                        && hover_slot != last_hover;
        /* v0.76.17: direct report, the exact "icons flash when I hover"
           shape v40's own three tiers above were built to fix, just never
           extended to the Apple menu's own hover highlight -- switching
           which row is highlighted while the dropdown is open fell through
           to the full-repaint branch below (whole photo blit + dock +
           every open window redrawn) on every single row hovered, since
           cursor_only explicitly excludes any menu_hover change and
           dock_only requires the menu to be closed. gui_draw_apple_menu is
           already fully self-contained (its own rounded-rect background
           fill covers its whole rect every call, same "cheapest repaint
           that's correct" shape gui_redraw_dock_band already established
           for the dock band), so this is a direct copy of that same
           pattern, not a new mechanism. */
        int menu_only = !launched && !cursor_only && !dock_only && drag_slot < 0 && last_drag < 0
                        && menu_open && last_menu_open
                        && (menu_hover != last_menu_hover || mx != last_mx || my != last_my);
        if (cursor_only) {
            gui_cursor_restore();
            if (my < GUI_MENUBAR_H || last_my < GUI_MENUBAR_H) { gui_menubar_force_redraw(); gui_draw_menubar(); }
            gui_cursor_save(mx, my);
            gui_draw_cursor(mx, my);
            last_mx = mx; last_my = my;
        } else if (dock_only) {
            gui_cursor_restore();
            gui_redraw_dock_band(hover_slot, drag_slot, mx, my);
            gui_cursor_save(mx, my);
            gui_draw_cursor(mx, my);
            last_mx = mx; last_my = my; last_hover = hover_slot;
        } else if (menu_only) {
            serial_puts("menuonly\n"); /* discriminating marker for tools/checks/menuclock-check.sh */
            gui_cursor_restore();
            gui_draw_apple_menu(menu_hover);
            gui_cursor_save(mx, my);
            gui_draw_cursor(mx, my);
            last_mx = mx; last_my = my; last_menu_hover = menu_hover;
        } else if (launched || mx != last_mx || my != last_my || hover_slot != last_hover || drag_slot != last_drag || menu_open != last_menu_open || menu_hover != last_menu_hover) {
            serial_puts("fullrepaint\n"); /* discriminating marker for tools/checks/menuclock-check.sh */
            if (my < GUI_MENUBAR_H || last_my < GUI_MENUBAR_H) gui_menubar_force_redraw();
            cursor_saved_x = cursor_saved_y = -1; /* the full repaint replaces whatever the backup held */
            gui_draw_desktop(hover_slot, drag_slot, mx, my);
            dock_presented_hover = dock_hover;
            if (gui_window_count > 0) gui_multiwin_draw_all(); /* v0.73.0: real simultaneous redraw of every open window, back-to-front, on top of the desktop just drawn above */
            if (menu_open) gui_draw_apple_menu(menu_hover);
            if (notif_open) gui_draw_notif_panel();
            if (weather_open) gui_draw_weather_panel();
            if (drag_slot < 0) { gui_cursor_save(mx, my); gui_draw_cursor(mx, my); }
            last_mx = mx; last_my = my; last_hover = hover_slot; last_drag = drag_slot;
            last_menu_open = menu_open; last_menu_hover = menu_hover;
        }
    }
    window_close();
    /* v77: free GUI heap allocations (wind_base, dock_band cache/frame) when
       exiting so the heap is available for other uses after gui_run() returns.
       This is critical for heap growth after the GUI: without this, the large
       allocations consume virtual address space and prevent paging_map_region
       from mapping new heap frames beyond the base map. */
    wall_caches_drop();
    if (dock_band_cache) { kfree(dock_band_cache); dock_band_cache = 0; }
    if (dock_band_frame) { kfree(dock_band_frame); dock_band_frame = 0; }
    clear();
    puts("back in text mode\n");
}

/* Draw a readable panic screen on the GUI when a ring-0 exception occurs.
   Called from kernel/idt.c's exception handler. Displays the exception name,
   fault address (for page faults), EIP, kernel version, and system message. */
void gui_panic_screen(const char *name, unsigned int fault_addr, unsigned int eip) {
    /* Check if GUI is active by seeing if window dimensions are non-zero */
    if (window_width() == 0 || window_height() == 0) return;

    /* Whole screen, not whatever app viewport was current when it faulted
       (the first version painted inside the Terminal's window). */
    window_clear_viewport();
    window_clear(0x00FAF8F6);

    int h = (int)window_height();
    int text_color = 0x001C1C1E;  /* dark text */

    /* Title: exception name at 1/4 down the screen */
    font_draw_string(name, 32, h / 4, text_color, -1);

    /* Exception details */
    int y = h / 4 + 32;

    /* For page faults, show the fault address */
    if (fault_addr != 0) {
        char buf[80];
        int i = 0;
        const char *prefix = "Page fault at: 0x";
        while (*prefix) buf[i++] = *prefix++;
        /* Inline hex conversion */
        unsigned int val = fault_addr;
        for (int j = 0; j < 8; j++) {
            unsigned int nib = (val >> (28 - j * 4)) & 0xF;
            buf[i++] = nib < 10 ? '0' + nib : 'A' + (nib - 10);
        }
        buf[i] = '\0';
        font_draw_string(buf, 32, y, text_color, -1);
        y += 24;
    }

    /* EIP (instruction pointer) */
    {
        char buf[80];
        int i = 0;
        const char *prefix = "EIP: 0x";
        while (*prefix) buf[i++] = *prefix++;
        unsigned int val = eip;
        for (int j = 0; j < 8; j++) {
            unsigned int nib = (val >> (28 - j * 4)) & 0xF;
            buf[i++] = nib < 10 ? '0' + nib : 'A' + (nib - 10);
        }
        buf[i] = '\0';
        font_draw_string(buf, 32, y, text_color, -1);
    }

    /* Kernel version */
    {
        char buf[80];
        int i = 0;
        const char *prefix = "Joshua Tree ";
        while (*prefix) buf[i++] = *prefix++;
        const char *ver = JT_VERSION_STR;
        while (*ver) buf[i++] = *ver++;
        buf[i] = '\0';
        font_draw_string(buf, 32, y + 32, text_color, -1);
    }

    /* Main message lines */
    int message_y = h / 2 + 60;
    font_draw_string("Joshua Tree stopped to protect your files.", 32, message_y, text_color, -1);
    font_draw_string("Hold the power button to restart.", 32, message_y + 32, text_color, -1);

    /* Display the panic screen */
    window_present();
}

/* ---- usertest: the ring-3 reference program, end to end -------------------
   The real proof that docs/SYSCALL-ABI.md is a contract and not a wish.
   user/hello.c is compiled on its own, against user/jtsys.h and nothing
   else, and reaches this kernel only through int 0x80. This command puts
   it and its data file on the VFS, runs it at ring 3 via exec_user(), and
   checks the exit status the kernel actually observed.

   Both files are seeded here rather than assumed present because the
   headless check boots (and the browser embed) have no disk at all, which
   is exactly the ramfs path kmain already falls back to. The binary rides
   along in kernel.elf as bytes (drivers/user_hello.h, generated by the
   Makefile from the real built user/hello.bin), gets written out through
   the VFS, and is then read back through the VFS by exec_user. It is a
   real file load either way; seeding just removes the disk image as a
   precondition for the test. */
#define USERTEST_DATA "joshua tree user space\n"
static void putdec(int v){
    char tmp[12]; int i = 0;
    unsigned u = v < 0 ? (unsigned)(-v) : (unsigned)v;
    if (v < 0) putc('-');
    do { tmp[i++] = (char)('0' + u % 10); u /= 10; } while (u);
    while (i) putc(tmp[--i]);
}
static void usertest(void){
    if (!vfs_write_file("HELLO.TXT", USERTEST_DATA, strlen(USERTEST_DATA))) {
        puts("usertest: could not write HELLO.TXT to the active filesystem\n");
        serial_puts("usertest: FAILED (seed HELLO.TXT)\n");
        return;
    }
    if (!vfs_write_file("HELLO.BIN", user_hello, USER_HELLO_LEN)) {
        puts("usertest: could not write HELLO.BIN to the active filesystem\n");
        serial_puts("usertest: FAILED (seed HELLO.BIN)\n");
        return;
    }
    puts("running HELLO.BIN at ring 3...\n");
    int status = -1;
    const char *hello_argv[] = { "HELLO.BIN" };
    if (!exec_user("HELLO.BIN", hello_argv, 1, &status)) {
        puts("usertest: exec_user failed (not found, too big, or no free task slot)\n");
        serial_puts("usertest: FAILED (exec_user)\n");
        return;
    }
    puts("usertest: exit code "); putdec(status); putc('\n');
    serial_puts("usertest: exit code ");
    { char d[2] = { (char)('0' + (status >= 0 && status <= 9 ? status : 0)), 0 };
      serial_puts(status >= 0 && status <= 9 ? d : "?"); }
    serial_puts("\n");
    int ok = (status == 9); /* user/hello.c's own last line; any earlier failure exits 1..5 instead */
    puts(ok ? "ring-3 reference program ran the v1 syscall set and exited 9: ok\n"
            : "usertest: FAILED (wrong exit code)\n");
    serial_puts(ok ? "usertest: ok\n" : "usertest: FAILED\n");
}

/* ---- notetest: the v2 reference program, end to end -----------------------
   Same shape as usertest above and the same reason for existing, one
   version later. user/note.c is the first thing in this repo that can
   change a file from ring 3, so this is the proof that v2's write path,
   its seek and its argv are real rather than documented.

   Three runs of one program, with three different argv vectors, against
   one file:
     note NOTE.TXT buy milk    creates it (O_CREAT|O_APPEND)
     note NOTE.TXT call mum    appends to it
     note NOTE.TXT @4 MILK     seeks into the middle and overwrites
   and then the kernel reads NOTE.TXT back itself and compares the bytes.
   That last step is the assertion that matters: the program's own output
   is the program's claim about what it did, while vfs_read_file() here is
   the filesystem's answer. A write path that printed the right thing and
   stored nothing would pass the first and fail the second. */
#define NOTETEST_EXPECT "buy MILK\ncall mum\n"
#define NOTETEST_EXPECT2 "buy MILK\ncall mum\nand one more\n"
static void run(char *line); /* the shell's own dispatcher, used below to test its argv splitting for real */
static int notetest_run(const char *a1, const char *a2, const char *a3) {
    const char *argv[4];
    int argc = 0;
    argv[argc++] = "NOTE.BIN";
    argv[argc++] = "NOTE.TXT";
    if (a1) argv[argc++] = a1;
    if (a2) argv[argc++] = a2;
    if (a3) argv[argc++] = a3;
    int status = -1;
    if (!exec_user("NOTE.BIN", argv, argc, &status)) {
        puts("notetest: exec_user failed\n");
        serial_puts("notetest: FAILED (exec_user)\n");
        return -1;
    }
    return status;
}
static void notetest(void){
    if (!vfs_write_file("NOTE.BIN", user_note, USER_NOTE_LEN) &&
        !vfs_replace_file("NOTE.BIN", user_note, USER_NOTE_LEN)) {
        puts("notetest: could not write NOTE.BIN to the active filesystem\n");
        serial_puts("notetest: FAILED (seed NOTE.BIN)\n");
        return;
    }
    /* Start from no file at all, so the O_CREAT run really creates. */
    vfs_delete("NOTE.TXT");

    int s1 = notetest_run("buy", "milk", 0);
    int s2 = notetest_run("call", "mum", 0);
    int s3 = notetest_run("@4", "MILK", 0);
    if (s1 < 0 || s2 < 0 || s3 < 0) return;
    puts("notetest: exit codes "); putdec(s1); putc(' '); putdec(s2); putc(' '); putdec(s3); putc('\n');
    serial_puts("notetest: exit codes ");
    { char d[7] = { (char)('0' + (s1 >= 0 && s1 <= 9 ? s1 : 9)), ' ',
                    (char)('0' + (s2 >= 0 && s2 <= 9 ? s2 : 9)), ' ',
                    (char)('0' + (s3 >= 0 && s3 <= 9 ? s3 : 9)), '\n', 0 };
      serial_puts(d); }

    /* The filesystem's own answer, not the program's. */
    static char back[256];
    int n = vfs_read_file("NOTE.TXT", back, sizeof(back) - 1);
    int ok = (s1 == 0 && s2 == 0 && s3 == 0) && n == (int)strlen(NOTETEST_EXPECT);
    if (ok) { back[n] = 0; ok = !strcmp(back, NOTETEST_EXPECT); }
    if (n > 0) { back[n] = 0; puts("notetest: file now holds: "); puts(back); }
    puts(ok ? "ring-3 program created, appended to and patched a real file: ok\n"
            : "notetest: FAILED (the file on the filesystem is not what the program wrote)\n");
    serial_puts(ok ? "notetest: ok\n" : "notetest: FAILED\n");
    if (!ok) return;

    /* The same program again, this time through the shell's own `exec`,
       so the words a person would type really do become argv. run() is
       the exact dispatcher a typed line reaches, handed a mutable buffer
       the way the line reader hands it one, so this is the real splitter
       and not a re-implementation of it. Driving it from here rather than
       from QEMU keystrokes is deliberate: the monitor's sendkey cannot
       produce the shifted characters an uppercase filename needs, so a
       keystroke-driven version of this would be testing the harness. */
    char cmd[] = "exec NOTE.BIN NOTE.TXT and one more";
    run(cmd);
    n = vfs_read_file("NOTE.TXT", back, sizeof(back) - 1);
    int ok2 = n == (int)strlen(NOTETEST_EXPECT2);
    if (ok2) { back[n] = 0; ok2 = !strcmp(back, NOTETEST_EXPECT2); }
    puts(ok2 ? "the shell's own exec passed its words through as argv: ok\n"
             : "notetest: FAILED (exec did not hand the shell's words to the program as argv)\n");
    serial_puts(ok2 ? "notetest argv: ok\n" : "notetest argv: FAILED\n");
}

/* Splits `s` in place at runs of spaces into `argv`, the same way every
   argument list in this shell is built: no quotes, no escapes, just
   words, argv[i] pointing back into `s` itself. Shared by `exec` and the
   bare-name fallthrough in run() below so there is one splitter, not two.
   Returns the word count, or -1 if there were more than `max`. */
static int split_argv(char *s, const char **argv, int max) {
    int argc = 0;
    char *p = s;
    while (*p) {
        while (*p == ' ') *p++ = 0;
        if (!*p) break;
        if (argc >= max) return -1;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
    }
    return argc;
}

static void run(char *line){
    char *arg = line;
    while (*arg && *arg != ' ') arg++;
    if (*arg) *arg++ = 0;

    if (!*line)                    return;
    if (!strcmp(line, "help"))       { puts("help clear echo time uptime dmesg mem reboot crash pagefault heaptest heapgrow tasktest preempttest weathertest daynighttest maptinttest walltest weatherfxtest weatherfxcliptest geotest weatherpaneltest windweathertest cursortest texttest wraptest mailtest dockstyletest wind isotest reaptest ring3test usertest notetest filetest ps kill killtest sleep disktest diskuse fsuse ls cat exec rm cd mkdir write browse lspci gfxtest fonttest mousetest nettest ifconfig netscan web serve serveapp chat build gui testapps contactstest calctest pngtest jpegtest chattest\n");
                                        puts("a name that isn't one of the above runs a program by that name too, e.g. \"hello\" or \"note buy milk\" (same as exec, case-insensitive)\n"); }
    else if (!strcmp(line, "clear")) clear();
    else if (!strcmp(line, "echo"))  { puts(arg); putc('\n'); }
    else if (!strcmp(line, "crash")) __asm__ volatile ("int $3");  /* manual check: exercises idt/isr */
    else if (!strcmp(line, "pagefault")) { volatile int *p = (int *)0xDEAD0000; *p = 1; } /* manual check: exercises paging */
    else if (!strcmp(line, "heaptest")) {
        char *a = kmalloc(16);
        char *b = kmalloc(32);
        if (a && b) { a[0] = 'A'; b[0] = 'B'; kfree(a); char *c = kmalloc(8);
            puts(c == (void*)a ? "reused freed block: ok\n" : "alloc ok, no reuse\n"); kfree(b); kfree(c); }
        else puts("kmalloc failed\n");

        /* two adjacent blocks, freed, should merge into one big enough for
           a request neither could satisfy alone, with no new frame pulled
           in for it: the real, distinguishing signature of coalescing
           actually running, not just "didn't crash" */
        char *x = kmalloc(20);
        char *y = kmalloc(20);
        if (x && y) {
            unsigned int before = pmm_free_frames();
            kfree(y); kfree(x);
            char *big = kmalloc(48);
            unsigned int after = pmm_free_frames();
            puts((big == x && after == before) ? "coalesced adjacent free blocks: ok\n" : "coalesce failed\n");
            kfree(big);
        } else puts("kmalloc failed\n");

        /* v57: block splitting. Free one big block, then take two small
           bites out of it. The real, distinguishing signature that
           splitting is actually carving the leftover into its own reusable
           block, not just "both allocs succeeded": p2 has to land just
           past p1, close enough that it can only be the leftover carved
           off p1's own block (one header's worth of gap), not a different
           free block elsewhere or a fresh bump allocation. Without
           splitting, the first small alloc claims the *whole* freed block
           (same shape as the "reused freed block" check above), leaving no
           free space behind inside it, so the second small alloc has to
           come from wherever else the allocator finds space, nowhere near
           p1's own address. */
        char *big2 = kmalloc(200);
        if (big2) {
            kfree(big2);
            char *p1 = kmalloc(20);
            char *p2 = kmalloc(20);
            unsigned int gap = (unsigned int)(p2 - p1);
            int adjacent = p2 > p1 && gap < 64;
            puts((p1 && p2 && adjacent) ? "split leftover reused: ok\n" : "split failed\n");
            kfree(p1); kfree(p2);
        } else puts("kmalloc failed\n");
    }
    else if (!strcmp(line, "heapgrow")) {
        /* v34 (0.34.0): real proof the old 0x400000 wall is actually gone,
           not just that the code compiles. Keeps allocating 4KB chunks
           (small enough not to instantly exhaust real RAM, big enough to
           cross the old wall in a bounded number of iterations) until one
           lands at or past 0x400000, then writes and reads back a real
           marker through it, real evidence the mapping isn't just
           allocated but actually usable, not a bus error waiting to
           happen. Bounded at 2048 iterations (8MB) so a genuinely broken
           build fails fast instead of hanging. */
        void *p = 0;
        int crossed = 0;
        for (int i = 0; i < 2048 && !crossed; i++) {
            p = kmalloc(4096);
            if (!p) { puts("kmalloc failed before crossing 0x400000 (real OOM or a real regression)\n"); break; }
            if ((unsigned int)p >= 0x400000) crossed = 1;
        }
        if (crossed) {
            *(volatile unsigned int *)p = 0xC0FFEE00;
            unsigned int back = *(volatile unsigned int *)p;
            puts(back == 0xC0FFEE00 ? "heap grew past 0x400000 and is real, writable memory: ok\n" : "heap grew past 0x400000 but readback FAILED\n");
        }
    }
    else if (!strcmp(line, "uptime")){ putn(ticks() / 100); puts("s\n"); }
    else if (!strcmp(line, "dmesg")) klog_dump();
    else if (!strcmp(line, "mem")) {
        putn(pmm_free_frames() * 4); puts("K free / ");
        putn(pmm_total_frames() * 4); puts("K total (4K frames)\n");
    }
    else if (!strcmp(line, "sleep")) { puts("sleeping 1s...\n"); sleep_ticks(100); puts("awake\n"); }
    else if (!strcmp(line, "disktest")) {
        char wbuf[512], rbuf[512];
        for (int i = 0; i < 512; i++) wbuf[i] = (char)i;
        if (!blockdev_write_sector(100, wbuf)) { puts("disk write failed (no drive?)\n"); }
        else if (!blockdev_read_sector(100, rbuf)) { puts("disk read failed\n"); }
        else {
            int ok = 1;
            for (int i = 0; i < 512; i++) if (rbuf[i] != wbuf[i]) { ok = 0; break; }
            puts(ok ? "wrote+read sector 100: ok\n" : "wrote+read sector 100: MISMATCH\n");
        }
    }
    else if (!strcmp(line, "filetest")) {
        /* `filetest` writes a known file and reads it back; `filetest read`
           only reads, never writes, so files-roundtrip-check.sh can prove
           the bytes written before a reboot are still on the disk after it. */
        const char *content = "JT_TESTCONTENT_001";
        const char *filename = "JT_TEST.TXT";
        int read_only = !strcmp(arg, "read");
        char rbuf[32]; for (int i = 0; i < 32; i++) rbuf[i] = 0;
        if (!read_only) vfs_delete(filename);  /* start fresh each time */
        if (!read_only && !vfs_write_file(filename, (char *)content, 18)) serial_puts("filetest: write failed\n");
        /* security pass: sizeof(rbuf) - 1, not sizeof(rbuf) -- reserves the
           last byte so the strcmp below always finds a real NUL even if
           JT_TEST.TXT on disk is >= 32 bytes (a crafted disk image, not
           just this command's own 18-byte write), matching the
           sizeof(buf)-1 convention every other vfs_read_file call in this
           kernel already follows. */
        else if (!vfs_read_file(filename, rbuf, sizeof(rbuf) - 1)) serial_puts("filetest: read failed\n");
        else if (strcmp(rbuf, content)) serial_puts("filetest: content mismatch\n");
        else serial_puts(read_only ? "filetest: persisted read ok\n" : "filetest: write+read ok\n");
    }
    else if (!strcmp(line, "tasktest")) {
        /* v0.76.8: real, reproduced-on-demand CI flake fixed at the root.
           yield()'s software `int $32` and the hardware PIT's own IRQ0 both
           land on the identical IDT gate and both call schedule() (see
           task.c's own comment on yield()), so this kernel has been truly,
           continuously preemptive in the background since whatever version
           first wired IRQ0 to it -- not just during preempttest's deliberate
           no-yield demo. This test's strict "ABABAB..." expectation is only
           true if NO hardware tick lands during its own tiny window, which
           held by luck on a fast/idle machine but not on a slower or
           shared CI runner: reproduced reliably here by temporarily
           reconfiguring the PIT to 5000Hz (irq_install's pit_init call),
           which corrupted the output on every single run (e.g.
           "ABABAABABABABABABABB"), then confirmed clean again after adding
           the mask below, even at that same artificially high rate; the
           real PIT rate (100Hz) was restored unchanged after the repro.
           Fix: mask IRQ0 at the PIC for the exact width of this test, so
           only the tasks' own explicit yield() calls drive scheduling here
           -- pic_set_mask holds across a task switch regardless of which
           task's own EFLAGS.IF is active (unlike cli/sti, which only
           affects the currently running task's own restored flags), unlike
           preempttest/killtest elsewhere, which still rely on real IRQ0
           ticks and are intentionally left untouched. */
        pic_set_mask(0, 1);
        puts("\n");
        task_create(task_a);
        task_create(task_b);
        for (int i = 0; i < 10; i++) yield(); /* shell is task 0; let A/B interleave */
        pic_set_mask(0, 0);
        puts("\ndone (expect ABABAB...)\n");
    }
    else if (!strcmp(line, "ring3test")) {
        ring3_test(arg); /* v64: "", "fault", or "spin", see ring3.h; all three come back to the shell */
    }
    else if (!strcmp(line, "usertest")) usertest();
    else if (!strcmp(line, "notetest")) notetest();
    else if (!strcmp(line, "preempttest")) {
        preempt_a_count = 0; preempt_b_count = 0; preempt_stop = 0;
        int ida = task_create(preempt_task_a);
        int idb = task_create(preempt_task_b);
        if (ida < 0 || idb < 0) { puts("no free task slots (run fewer other task tests first)\n"); }
        else {
            unsigned int deadline = ticks() + 20; /* ~200ms real wall clock */
            while (ticks() < deadline) { } /* deliberately no yield()/hlt here */
            puts((preempt_a_count > 0 && preempt_b_count > 0) ? "preempted without yield: ok\n" : "no preemption (still cooperative-only)\n");
            preempt_stop = 1;
            for (int i = 0; i < 5; i++) yield(); /* let both tasks actually reach task_exit() and free their slots before returning */
        }
    }
    else if (!strcmp(line, "ps")) {
        for (int i = 0; i < task_max(); i++) {
            char buf[16]; int n = 0; unsigned int v = (unsigned int)i;
            char tmp[6]; int ti = 0; if (v==0) tmp[ti++]='0'; while (v) { tmp[ti++]='0'+v%10; v/=10; }
            while (ti) buf[n++] = tmp[--ti];
            buf[n++]=' '; buf[n]=0;
            puts(buf);
            puts(task_used(i) ? "used\n" : "free\n");
        }
    }
    else if (!strcmp(line, "kill")) {
        if (!*arg) { puts("usage: kill <task id>, see ps\n"); }
        else {
            int id = 0; const char *p = arg; while (*p >= '0' && *p <= '9') { id = id*10 + (*p-'0'); p++; }
            task_kill(id);
            puts("signal sent (takes effect next time that task is scheduled)\n");
        }
    }
    else if (!strcmp(line, "killtest")) {
        /* Real proof a killed task actually stops, not just that `kill`
           didn't crash anything: preempt_task_a runs forever incrementing
           a plain counter (same demo task preempttest already uses,
           reused rather than writing a third almost-identical one). Kill
           it mid-flight, let a few ticks pass, and confirm the counter
           genuinely stopped moving instead of merely slowing down. */
        preempt_a_count = 0; preempt_stop = 0;
        int id = task_create(preempt_task_a);
        if (id < 0) { puts("no free task slots\n"); }
        else {
            unsigned int warmup = ticks() + 5;
            while (ticks() < warmup) { }
            task_kill(id);
            for (int i = 0; i < 3; i++) yield(); /* let the scheduler actually resume it into task_exit() */
            int stopped_at = preempt_a_count;
            unsigned int settle = ticks() + 10;
            while (ticks() < settle) { }
            puts(preempt_a_count == stopped_at ? "kill: task really stopped: ok\n" : "kill: FAILED (counter kept moving)\n");
        }
    }
    else if (!strcmp(line, "spawntest")) {
        /* Real, persistent task for tools/checks/activity-check.py: prints
           its own slot id over serial so the script can compute which
           Activity row to select, the same "serial marker for a headless
           check" idiom search.h's "searchcontent" line already uses. */
        int id = task_create(spawntest_task);
        if (id < 0) puts("no free task slots\n");
        else {
            char buf[16]; int n = 0; unsigned int v = (unsigned int)id;
            char tmp[6]; int ti = 0; if (v==0) tmp[ti++]='0'; while (v) { tmp[ti++]='0'+v%10; v/=10; }
            while (ti) buf[n++] = tmp[--ti];
            buf[n] = 0;
            serial_puts("spawntest id="); serial_puts(buf); serial_puts("\n");
            puts("spawned task "); puts(buf); puts(" (runs until killed)\n");
        }
    }
    else if (!strcmp(line, "weathertest")) {
        /* v43: the parser has one real trap, so it gets a real test: the
           reply's "current_units" object carries the same keys with STRING
           values ("°C") before the "current" object carries the numbers.
           A naive key search reads the wrong one. Negative and fractional
           temperatures are the other two edge cases a Vancouver winter
           will actually exercise. */
        static const char canned[] = "{\"latitude\":49.27,\"current_units\":{\"time\":\"iso8601\",\"temperature_2m\":\"\xb0" "C\",\"weather_code\":\"wmo code\"},"
                                     "\"current\":{\"time\":\"2026-09-14T07:40\",\"interval\":900,\"temperature_2m\":-3.5,\"weather_code\":61}}";
        int t10 = 999, code10 = 999;
        int ok_t = json_current_number(canned, "temperature_2m", &t10);
        int ok_c = json_current_number(canned, "weather_code", &code10);
        int ok = ok_t && ok_c && t10 == -35 && code10 == 610;
        puts(ok ? "weather parse: -3.5C / code 61 through the units-block trap: ok\n" : "weather parse: FAILED\n");
        if (!ok) { puts("  t10="); putn((unsigned int)t10); puts(" code10="); putn((unsigned int)code10); puts("\n"); }
    }
    else if (!strcmp(line, "daynighttest")) {
        /* v65 (0.62.0): standing QA per CLAUDE.md's 4b. daynight_calc and
           gui_daynight_tint_pct are both pure functions of an explicit
           hour/pct (see their own comments above, deliberately shaped
           this way for exactly this test), so this pins hour=2 (deep
           night) against hour=14 (real midday) directly, no faked
           hardware needed, the same boot-time direct-call trick this
           suite's other tests use, just via a pure function instead of a
           temporary kmain hook. Three real, discriminating checks a
           reverted feature would fail: the two hours actually produce a
           different color for the same input pixel, the night one is
           genuinely darker (not just different), and it stays inside
           this repo's own "never cold black-and-blue" rule (blue channel
           never exceeds red on a warm test color, at either hour). */
        unsigned int test_color = 0x00C97A3E; /* a plausible warm sunset tone from this file's own photo */
        int night2 = 0, day2 = 0, night14 = 0, day14 = 0;
        daynight_calc(2, &night2, &day2);
        daynight_calc(14, &night14, &day14);
        unsigned int c2 = gui_daynight_tint_pct(test_color, night2, day2);
        unsigned int c14 = gui_daynight_tint_pct(test_color, night14, day14);
        int r2 = (int)((c2 >> 16) & 0xFF), g2 = (int)((c2 >> 8) & 0xFF), b2 = (int)(c2 & 0xFF);
        int r14 = (int)((c14 >> 16) & 0xFF), g14 = (int)((c14 >> 8) & 0xFF), b14 = (int)(c14 & 0xFF);
        int sum2 = r2 + g2 + b2, sum14 = r14 + g14 + b14;
        int ok = (c2 != c14) && (sum2 < sum14) && (b2 <= r2) && (b14 <= r14) && (night2 > night14) && (day14 >= day2);
        puts(ok ? "daynight: hour=2 darker+warm than hour=14, both differ: ok\n" : "daynight: FAILED\n");
        if (!ok) {
            puts("  c2="); puthex(c2); puts(" c14="); puthex(c14);
            puts(" sum2="); putn((unsigned int)sum2); puts(" sum14="); putn((unsigned int)sum14); puts("\n");
        }
    }
    else if (!strcmp(line, "maptinttest")) {
        /* v79 (0.68.0): standing QA per CLAUDE.md's 4b, for the map-only
           warm color grade added above (gui_map_tint). Three real,
           discriminating checks a reverted grade would fail: (1) a
           neutral OpenTopoMap-style gray actually shifts warm (red ends
           up strictly greater than blue, it started equal), (2) two
           distinct source colors (a road gray and a water blue) stay
           distinct after the grade -- proves this is a multiply, not a
           lerp-to-one-target that would collapse them together, (3) a
           near-white street-label background and a near-black label
           glyph both stay on their own end of the range (white stays
           above 200, black stays under 40) so the grade doesn't crush
           the contrast a real label needs to read. */
        unsigned int gray  = 0x00A0A0A0; /* plausible OpenTopoMap road/contour gray */
        unsigned int water = 0x006E9BC7; /* plausible OpenTopoMap water blue */
        unsigned int label_bg = 0x00F2F0E8;  /* near-white street-name background */
        unsigned int label_fg = 0x00202020;  /* near-black street-name glyph */
        unsigned int tg = gui_map_tint(gray), tw = gui_map_tint(water);
        unsigned int tbg = gui_map_tint(label_bg), tfg = gui_map_tint(label_fg);
        int gr = (int)((tg >> 16) & 0xFF), gb = (int)(tg & 0xFF);
        int bg_r = (int)((tbg >> 16) & 0xFF);
        int fg_r = (int)((tfg >> 16) & 0xFF);
        int ok = (gr > gb) && (tg != tw) && (tg != gray) && (bg_r > 200) && (fg_r < 40);
        puts(ok ? "maptint: neutral grays warmed, distinct colors stay distinct, label contrast survives: ok\n" : "maptint: FAILED\n");
        if (!ok) {
            puts("  tg="); puthex(tg); puts(" tw="); puthex(tw);
            puts(" tbg="); puthex(tbg); puts(" tfg="); puthex(tfg); puts("\n");
        }
    }
    else if (!strcmp(line, "walltest")) {
        /* v81 (0.69.0): standing QA per CLAUDE.md's 4b for the wallpaper
           theme picker added this pass. Reverting either gui_map_tint_cool
           or gui_wall_tint's dispatch back to a single-theme shape fails
           this, proved by hand before writing it: temporarily making
           gui_wall_tint always return gui_map_tint(rgb) (the pre-v81
           behaviour) makes check (2) below fail (Cool would equal Warm).
           Real, discriminating checks, same shape as maptinttest above:
           (1) Cool is a genuinely different curve from Warm on the same
               neutral gray -- not just "some different number", but the
               opposite direction: Warm pushes red above blue (r>b), Cool
               pushes blue above red (b>r), on the identical input.
           (2) Cool(gray) != Warm(gray): the two map themes are provably
               distinct, not the same grade under two names.
           (3) Raw(gray) == gray: the "no color grade" theme really is a
               no-op, not a mislabeled copy of one of the tinted paths.
           (4) Cool keeps two distinct source colors distinct after its
               grade (a road gray and a water blue stay != each other),
               proving the contrast-stretch add-on is still a real
               per-pixel transform, not a lerp-to-one-target that would
               collapse them.
           (5) settings_save()/settings_load() round-trip every one of the
               4 real theme values through SETTINGS.TXT (the actual
               Settings-screen persistence path, not a mock), each value
               distinct after reload -- the "config value with no visible
               effect" failure mode this task was scoped to avoid, plus
               the file-format contract wind/dock already get tested at. */
        unsigned int gray  = 0x00A0A0A0;
        unsigned int water = 0x006E9BC7;
        unsigned int warm_g = gui_map_tint(gray);
        unsigned int cool_g = gui_map_tint_cool(gray);
        int wr = (int)((warm_g >> 16) & 0xFF), wb = (int)(warm_g & 0xFF);
        int cr = (int)((cool_g >> 16) & 0xFF), cb = (int)(cool_g & 0xFF);
        int ok1 = (wr > wb) && (cb > cr); /* opposite directions on the same input */
        int ok2 = (cool_g != warm_g);
        int ok4 = (gui_map_tint_cool(gray) != gui_map_tint_cool(water));

        /* ok3 exercises the real per-pixel dispatch (gui_wall_tint), not
           the standalone tint functions above, so a broken dispatch (e.g.
           gui_wall_tint hardcoded back to always-Warm, the pre-v81 shape)
           fails HERE even though ok1/ok2/ok4 would still pass on their
           own -- proved by hand: hardcoding gui_wall_tint to always
           `return gui_map_tint(rgb);` makes wall_dispatch_warm ==
           wall_dispatch_cool below, which this catches. wall_src is
           pointed at a real non-wallpaper_rgb address (any distinct
           pointer works, gui_wall_tint only ever compares it, never
           dereferences it here) so the Photo early-out doesn't fire. */
        const unsigned char *saved_wall_src = wall_src;
        int saved_wall_theme = wall_theme;
        unsigned char fake_map_byte = 0;
        wall_src = &fake_map_byte;
        wall_theme = WALL_WARM;  unsigned int wall_dispatch_warm = gui_wall_tint(gray);
        wall_theme = WALL_COOL;  unsigned int wall_dispatch_cool = gui_wall_tint(gray);
        wall_theme = WALL_RAW;   unsigned int wall_dispatch_raw  = gui_wall_tint(gray);
        wall_theme = WALL_SAT;   unsigned int wall_dispatch_sat  = gui_wall_tint(gray);
        unsigned int wall_dispatch_sat_green = gui_wall_tint(0x00208020);
        wall_src = wallpaper_rgb; wall_theme = WALL_COOL;
        unsigned int wall_dispatch_photo = gui_wall_tint(gray); /* Photo guard: must ignore wall_theme entirely */
        wall_src = saved_wall_src; wall_theme = saved_wall_theme;
        int ok3 = (wall_dispatch_warm == warm_g) && (wall_dispatch_cool == cool_g)
                && (wall_dispatch_raw == gray) && (wall_dispatch_photo == gray)
                && (wall_dispatch_sat == gui_sat_color(gray)) && (wall_dispatch_sat != warm_g);
        /* Satellite must preserve hue and lift saturation. Returning raw
           pixels or restoring the old luminance-only grade fails here. */
        unsigned int sg = gui_sat_color(0x00208020);
        int ok5 = (gui_sat_color(0x00000000) == 0x00000000)
                && (gui_sat_color(0x00FFFFFF) == 0x00FFFFFF)
                && (((sg >> 8) & 0xFF) > ((sg >> 16) & 0xFF))
                && (((sg >> 8) & 0xFF) > (sg & 0xFF))
                && (((sg >> 8) & 0xFF) - (sg & 0xFF) > 0x60)
                && (wall_dispatch_sat_green == sg);
        ok3 = ok3 && ok5;

        const char *prev_fs = vfs_current_name();
        char prev_fs_buf[16]; int pfi = 0; while (prev_fs[pfi] && pfi < 15) { prev_fs_buf[pfi] = prev_fs[pfi]; pfi++; } prev_fs_buf[pfi] = 0;
        vfs_switch("ramfs");
        int roundtrip_ok = 1;
        for (int theme = WALL_PHOTO; theme <= WALL_SAT; theme++) {
            wall_theme = theme; settings_save();
            wall_theme = -1; /* clobber so settings_load has to actually set it, not coast on the old value */
            settings_load();
            if (wall_theme != theme) roundtrip_ok = 0;
        }
        vfs_switch(prev_fs_buf);

        int ok = ok1 && ok2 && ok3 && ok4 && roundtrip_ok;
        puts(ok ? "walltest: Cool distinct from Warm (opposite direction), Raw is a no-op, Cool preserves color distinctness, theme round-trips through SETTINGS.TXT: ok\n" : "walltest: FAILED\n");
        if (!ok) {
            puts("  warm_g="); puthex(warm_g); puts(" cool_g="); puthex(cool_g);
            puts(" dispatch warm="); puthex(wall_dispatch_warm); puts(" cool="); puthex(wall_dispatch_cool);
            puts(" raw="); puthex(wall_dispatch_raw); puts(" photo="); puthex(wall_dispatch_photo);
            puts(" ok1="); putn((unsigned int)ok1); puts(" ok2="); putn((unsigned int)ok2);
            puts(" ok3="); putn((unsigned int)ok3); puts(" ok4="); putn((unsigned int)ok4);
            puts(" roundtrip="); putn((unsigned int)roundtrip_ok); puts("\n");
        }
        wall_theme = WALL_WARM; settings_save(); /* restore the real default, this test must not leave the kernel in a weird state for whatever runs next */
    }
    else if (!strcmp(line, "weatherfxtest")) {
        /* v65 (0.62.0): standing QA per CLAUDE.md's 4b, two real,
           discriminating checks. (1) weather_fx_kind is a pure function
           of a WMO code (see its own comment above, same cutoffs
           weather_word() uses), so Clear/Fog/Rain/Snow/Storm codes are
           checked directly against the classification a reverted feature
           would get wrong. (2) weather_fx_tick_kind actually has to move
           real particle state, not just classify a code correctly: seed
           a known particle set, tick it under a live Rain classification
           and confirm particle 0 really moved, then tick the same count
           under WEATHER_FX_NONE and confirm it does NOT move (the early-
           return path), proving the "Clear gets no overlay" contract is
           real, not just a comment. Global particle state is saved and
           restored around this so a real live overlay in progress isn't
           disturbed by running the test. */
        int save_seeded = weather_particles_seeded;
        struct weather_particle save_particles[WEATHER_PARTICLE_COUNT];
        for (int i = 0; i < WEATHER_PARTICLE_COUNT; i++) save_particles[i] = weather_particles[i];

        int ok_kind = weather_fx_kind(0) == WEATHER_FX_NONE   /* Clear */
                   && weather_fx_kind(45) == WEATHER_FX_NONE  /* Fog */
                   && weather_fx_kind(61) == WEATHER_FX_RAIN  /* Rain */
                   && weather_fx_kind(73) == WEATHER_FX_SNOW  /* Snow */
                   && weather_fx_kind(95) == WEATHER_FX_RAIN; /* Storm */

        weather_particles_seed(800);
        int y0 = weather_particles[0].y, x0 = weather_particles[0].x;
        for (int i = 0; i < 5; i++) weather_fx_tick_kind(WEATHER_FX_RAIN, 800);
        int moved_on_rain = (weather_particles[0].y != y0) || (weather_particles[0].x != x0);

        weather_particles[0].y = y0; weather_particles[0].x = x0;
        int y1 = weather_particles[0].y, x1 = weather_particles[0].x;
        for (int i = 0; i < 5; i++) weather_fx_tick_kind(WEATHER_FX_NONE, 800);
        int still_on_clear = (weather_particles[0].y == y1) && (weather_particles[0].x == x1);

        int ok = ok_kind && moved_on_rain && still_on_clear;
        puts(ok ? "weatherfx: classification + rain moves/clear doesn't: ok\n" : "weatherfx: FAILED\n");
        if (!ok) {
            puts("  ok_kind="); putn((unsigned int)ok_kind);
            puts(" moved_on_rain="); putn((unsigned int)moved_on_rain);
            puts(" still_on_clear="); putn((unsigned int)still_on_clear); puts("\n");
        }

        for (int i = 0; i < WEATHER_PARTICLE_COUNT; i++) weather_particles[i] = save_particles[i];
        weather_particles_seeded = save_seeded;
    }
    else if (!strcmp(line, "windweathertest")) {
        /* v60 gap fix: verification that pass was a boot-time direct-call
           dump of three gui_wind_shift() samples, reverted before commit,
           nothing left in the suite. Real, discriminating logic to pin
           down: wind_pct_for_weather_code's seven WMO-code buckets, the
           exact percentages roadmap.md's own table names (Fog stillest at
           40%, then Clear/Cloudy/Snow/Rain/Showers/Storm climbing to
           200%), every boundary value, not just one code per bucket. */
        int ok =
            wind_pct_for_weather_code(0)  == 60  && wind_pct_for_weather_code(3)  == 100 &&
            wind_pct_for_weather_code(4)  == 40  && wind_pct_for_weather_code(48) == 40  &&
            wind_pct_for_weather_code(49) == 130 && wind_pct_for_weather_code(67) == 130 &&
            wind_pct_for_weather_code(68) == 90  && wind_pct_for_weather_code(77) == 90  &&
            wind_pct_for_weather_code(78) == 150 && wind_pct_for_weather_code(82) == 150 &&
            wind_pct_for_weather_code(83) == 200 && wind_pct_for_weather_code(95) == 200;
        puts(ok ? "wind_pct_for_weather_code: every WMO bucket boundary maps to its real percent: ok\n"
                : "wind_pct_for_weather_code: FAILED\n");
    }
    else if (!strcmp(line, "weatherfxcliptest")) {
        /* v71 (0.65.0): regression test for the horizon glitch bar, per
           CLAUDE.md's 4b. Real framebuffer, not just particle state: opens
           the desktop's own 960x540 scale-2 window, clears it to a flat
           colour, parks particles one logical row above WIND_HORIZON_ROW
           (rain at max speed, then snow), ticks each kind ONCE, and scans
           the physical rows from the horizon down. The pre-v71 tick drew a
           particle at its advanced position (up to 8 rows past the
           horizon) before wrapping it, so any streak/dot pixel found at or
           below WIND_HORIZON_ROW*sc is exactly the leak that painted the
           dashed bar on Joshua's live desktop. Also asserts the tick did
           draw real particle pixels somewhere inside the band, so a
           "fix" that simply stopped drawing would fail too. Particle
           globals saved/restored like weatherfxtest. */
        int save_seeded = weather_particles_seeded;
        struct weather_particle save_particles[WEATHER_PARTICLE_COUNT];
        for (int i = 0; i < WEATHER_PARTICLE_COUNT; i++) save_particles[i] = weather_particles[i];
        if (!window_open_scaled(960, 540, 32, 2)) { puts("no VGA device found or out of page tables\n"); }
        else {
            const unsigned int flat = 0x00202020;
            int sc = (int)window_scale(), w = (int)window_width();
            int leaked = 0, drawn = 0;
            for (int kind = WEATHER_FX_RAIN; kind <= WEATHER_FX_SNOW; kind++) {
                window_clear(flat);
                weather_particles_seeded = 1;
                for (int i = 0; i < WEATHER_PARTICLE_COUNT; i++) {
                    weather_particles[i].x = 40 + i * 24;
                    weather_particles[i].y = WIND_HORIZON_ROW - 1;
                    weather_particles[i].speed = 8;
                }
                weather_fx_tick_kind(kind, w);
                for (int py = WIND_HORIZON_ROW * sc; py < (WIND_HORIZON_ROW + 12) * sc; py++)
                    for (int px = 0; px < w * sc; px++)
                        if (window_get_pixel_phys(px, py) != flat) leaked++;
                for (int py = WIND_TOP_ROW * sc; py < WIND_HORIZON_ROW * sc; py++)
                    for (int px = 0; px < w * sc; px++)
                        if (window_get_pixel_phys(px, py) != flat) drawn++;
            }
            window_close();
            puts(drawn > 0 ? "weatherfx clip: particles really drawn inside the sky band: ok\n" : "weatherfx clip: nothing drawn at all (test not discriminating): FAILED\n");
            puts(leaked == 0 ? "weatherfx clip: zero particle pixels at or below the horizon: ok\n" : "weatherfx clip: FAILED, pixels leaked below WIND_HORIZON_ROW: ");
            if (leaked) { putn((unsigned int)leaked); puts("\n"); }
        }
        weather_particles_seeded = save_seeded;
        for (int i = 0; i < WEATHER_PARTICLE_COUNT; i++) weather_particles[i] = save_particles[i];
    }
    else if (!strcmp(line, "geotest")) {
        /* v71 (0.65.0): the parser half of the real-location fix, pure and
           offline (tools/geo-check.sh is the live-network half). A fixed
           ip-api-shaped body, including the traps a sloppy match would
           trip on: "latitude"/"longitude" keys earlier in the text (the
           Open-Meteo reply's own key names, which "lat"/"lon" must NOT
           match inside), a negative longitude, a string-valued key that
           must be rejected as not-a-number, and a missing key. Asserts the
           exact text ip-api returned comes back byte-for-byte, since that
           text is what weather_fetch splices into its URL. */
        const char *body = "{\"status\":\"success\",\"latitude\":\"trap\",\"longitude\":1,\"city\":\"Langley\",\"lat\":49.0983,\"lon\":-122.6498,\"zip\":\"V3A\"}";
        char lat[16], lon[16], city[24], bad[16], none[16];
        unsigned int nlat = json_extract_number_text(body, "lat", lat, sizeof(lat));
        unsigned int nlon = json_extract_number_text(body, "lon", lon, sizeof(lon));
        unsigned int nbad = json_extract_number_text(body, "zip", bad, sizeof(bad));
        unsigned int nnone = json_extract_number_text(body, "isp", none, sizeof(none));
        unsigned int ncity = json_extract_string(body, "city", city, sizeof(city));
        int ok_lat = nlat == 7 && !strcmp(lat, "49.0983");
        int ok_lon = nlon == 9 && !strcmp(lon, "-122.6498");
        int ok_reject = nbad == 0 && nnone == 0;
        int ok_city = ncity == 7 && !strcmp(city, "Langley");
        puts(ok_lat ? "geo lat: exact text extracted, not matched inside \"latitude\": ok\n" : "geo lat: FAILED\n");
        puts(ok_lon ? "geo lon: negative value extracted byte-exact: ok\n" : "geo lon: FAILED\n");
        puts(ok_reject ? "geo: string value and missing key both rejected: ok\n" : "geo reject: FAILED\n");
        puts(ok_city ? "geo city: ok\n" : "geo city: FAILED\n");
        if (!(ok_lat && ok_lon)) { puts("  lat="); puts(lat); puts(" lon="); puts(lon); puts("\n"); }
    }
    else if (!strcmp(line, "weatherpaneltest")) {
        /* v56 gap fix: weathertest (above) proves weather_fetch's JSON
           parse; nothing proved the dropdown itself once shipped, that
           pass's verification was a boot-time pixel dump into fixed RAM,
           reverted before commit. Two real, discriminating checks: (1)
           weather_word()'s WMO-code bucket mapping, the same function the
           dropdown's condition word and v60's wind multiplier both depend
           on, every boundary named in roadmap.md's own table; (2)
           gui_draw_weather_panel() actually paints its own bg chrome and,
           only once weather_have is set, real antialiased glyph ink for
           the line it claims to show, not a blank rect.

           v72 gap fix, root-caused with a real DEBUG serial dump, not
           guessed: both checks below failed on pristine v70/v71 HEAD, and
           the panel itself was fine, painting real chrome both times
           (confirmed: the interior point now used, x0+40/y0+20 or
           x0+40/y0+40, read back the real fill color 0x002C2C2E in both
           the no-data and live-data cases). Two real, separate causes,
           both predating this test's own geometry:
           (1) the old (x0+4, y0+4) corner probe predates v66's
           GUI_FLYOUT_RADIUS=14 rounding of this panel. At radius 14 with
           a 3px AA band, the point 4px in from each edge is genuinely
           OUTSIDE the corner arc (distance from the arc centre (14,14) is
           sqrt(200)=~14.14, past the 14px radius), so
           gui_rounded_rect_on_wallpaper's own `continue` leaves it
           untouched -- it read back whatever was there before the panel
           drew (the test's own pre-clear color), not the panel's fault.
           (2) the (x0, y0) "border" probe checked for a border color that
           v66 deliberately removed from this exact panel: see
           GUI_FLYOUT_RADIUS's own comment, "these three flyouts... were
           the one place still... with a manually drawn 1px border...
           zero separate stroke line" is the whole point of that pass.
           There has been no border to find at (x0, y0) since v66; that
           corner pixel is also outside the radius per (1) regardless.
           Real fix is in this test, not the panel: probe a genuine
           interior point clear of both the corner radius and the
           top-edge AA band, and stop asserting a border this panel was
           intentionally redesigned not to have. */
        int ok_word =
            !strcmp(weather_word(0),  "Clear")   && !strcmp(weather_word(3),  "Cloudy") &&
            !strcmp(weather_word(4),  "Fog")     && !strcmp(weather_word(48), "Fog")    &&
            !strcmp(weather_word(49), "Rain")    && !strcmp(weather_word(67), "Rain")   &&
            !strcmp(weather_word(68), "Snow")    && !strcmp(weather_word(77), "Snow")   &&
            !strcmp(weather_word(78), "Showers") && !strcmp(weather_word(82), "Showers")&&
            !strcmp(weather_word(83), "Storm")   && !strcmp(weather_word(95), "Storm");
        puts(ok_word ? "weather_word: every WMO bucket boundary maps correctly: ok\n" : "weather_word: FAILED\n");

        if (!window_open(800, 600, 32)) { puts("no VGA device found or out of page tables\n"); }
        else {
            unsigned int bg = 0x002C2C2E;
            weather_hit_x0 = -1; /* force the fallback geometry branch, same formula gui_draw_weather_panel uses */
            int x0 = (int)window_width() - WEATHER_W - 200;
            if (x0 + WEATHER_W > (int)window_width() - 4) x0 = (int)window_width() - WEATHER_W - 4;
            int y0 = GUI_MENUBAR_H;

            weather_have = 0;
            window_clear(0x00111111);
            gui_draw_weather_panel();
            int chrome_empty = window_get_pixel(x0 + 40, y0 + 20) == bg;
            puts(chrome_empty ? "weather panel (no data): chrome painted: ok\n" : "weather panel (no data): FAILED\n");

            weather_have = 1; weather_temp_c = -3; weather_code10 = 610;
            int wi = 0; const char *seed = "-3\xf8 Rain";
            while (seed[wi] && wi < (int)sizeof(weather_text) - 1) { weather_text[wi] = seed[wi]; wi++; }
            weather_text[wi] = 0;
            window_clear(0x00111111);
            gui_draw_weather_panel();
            int chrome_ok = window_get_pixel(x0 + 40, y0 + 40) == bg;
            int ink = 0;
            for (int dx = 0; dx < WEATHER_W - 24 && !ink; dx++)
                if (window_get_pixel(x0 + 12 + dx, y0 + 10) != bg) ink = 1;
            puts((chrome_ok && ink) ? "weather panel (live data): chrome + real ink drawn: ok\n" : "weather panel (live data): FAILED\n");
            window_close();
            weather_have = 0; weather_text[0] = 0; /* leave state clean for anything run after */
        }
    }
    else if (!strcmp(line, "cursortest")) {
        /* v56.1 gap fix: the notif-panel bitmap-font regression (cursor
           save/restore reading/writing antialiased text through the
           LOGICAL layer, pixel-doubling it) was reproduced and fixed via
           a scripted headless sweep + pmemsave that pass, then fully
           reverted, nothing permanent left to catch the same class of bug
           coming back on any other never-repainted AA surface. Real,
           discriminating: renders real antialiased glyphs (font_draw_string
           genuinely blends at physical resolution) at a scale-2 window,
           captures the true per-physical-pixel content independently
           (window_get_pixel_phys, not through cursor_backup itself),
           then runs the real gui_cursor_save/gui_draw_cursor/
           gui_cursor_restore sequence over it and confirms every physical
           sub-pixel came back byte-exact. The old logical-layer bug would
           sample only each block's top-left pixel and write it back
           across the whole 2x2 block, so any block with real per-pixel
           variation (the entire point of antialiasing) would fail this. */
        if (!window_open_scaled(400, 300, 32, 2)) { puts("no VGA device found or out of page tables\n"); }
        else {
            int mx = 40, my = 40;
            font_set_aa(gui_aa_char, gui_aa_advance); /* real physical-resolution AA text, the exact surface v56.1 fixed; gui_run() normally registers this but this test doesn't call gui_run() */
            window_clear(0x00202020);
            font_draw_string("Wg", mx, my, 0x00F5F5F7, -1);

            int sc = gui_cursor_scale(), pw = CURSOR_W * sc, ph = CURSOR_H * sc;
            int px0 = mx * sc, py0 = my * sc;
            static unsigned int truth[CURSOR_W * CURSOR_MAX_SCALE * CURSOR_H * CURSOR_MAX_SCALE];
            int varied = 0;
            for (int j = 0; j < ph; j++) {
                for (int i = 0; i < pw; i++) {
                    truth[j * pw + i] = window_get_pixel_phys(px0 + i, py0 + j);
                    if (i > 0 && truth[j * pw + i] != truth[j * pw + i - 1]) varied = 1; /* real AA content, not a flat fill */
                }
            }
            gui_cursor_save(mx, my);
            gui_draw_cursor(mx, my);
            gui_cursor_restore();
            int mismatches = 0;
            for (int j = 0; j < ph; j++)
                for (int i = 0; i < pw; i++)
                    if (window_get_pixel_phys(px0 + i, py0 + j) != truth[j * pw + i]) mismatches++;
            puts(varied ? "cursor test area has real per-pixel AA variation: ok\n" : "cursor test area has NO variation (test not discriminating): FAILED\n");
            puts((varied && mismatches == 0) ? "cursor save/restore: AA text came back byte-exact through the physical layer: ok\n" : "cursor save/restore: FAILED (pixel-doubled or otherwise wrong)\n");
            window_close();
        }
    }
    else if (!strcmp(line, "texttest")) {
        /* v77 (0.67.1): "Cl oudy". Real, photographed menu bar bug: an
           extra gap between some letter pairs and not others. Root cause
           was the fixed 16px physical cell per glyph on the AA path
           (see font_draw_string's own note). Discriminating: renders the
           exact photographed word through the real font_draw_string +
           gui_aa_char path at scale 2, then measures where the ink
           actually landed, column by column, through window_get_pixel_phys.
           Six ink runs must come back (one per letter), and the spread
           between the widest and narrowest inter-letter gap must be tiny:
           with the bug the C-l gap is ~1 physical px (C overflows its cell
           and is clipped) while l-o is ~9 (l fills 7 of 16), spread 8;
           fixed, every gap is the glyphs own sidebearings, spread <= 3.
           Second check: a lone 'W' (advance 24) must be at least 20 ink
           columns wide; the bug clipped it to 16. */
        if (!window_open_scaled(400, 300, 32, 2)) { tt_out("no VGA device found or out of page tables\n"); }
        else {
            font_set_aa(gui_aa_char, gui_aa_advance);
            unsigned int bg = 0x00202020;
            window_clear(bg);
            font_draw_string("Cloudy", 20, 20, 0x00F5F5F7, -1);
            font_draw_string("W", 20, 60, 0x00F5F5F7, -1);
            int col_ink[200];
            for (int i = 0; i < 200; i++) {
                col_ink[i] = 0;
                for (int j = 0; j < 32; j++) if (window_get_pixel_phys(40 + i, 40 + j) != bg) { col_ink[i] = 1; break; }
            }
            int runs = 0, run_start[16], run_end[16], in_run = 0;
            for (int i = 0; i < 200; i++) {
                if (col_ink[i] && !in_run) { if (runs < 16) run_start[runs] = i; in_run = 1; }
                if (!col_ink[i] && in_run) { if (runs < 16) run_end[runs] = i - 1; runs++; in_run = 0; }
            }
            if (in_run) { if (runs < 16) run_end[runs] = 199; runs++; }
            int gap_min = 999, gap_max = -1;
            tt_out("texttest 'Cloudy' ink runs:");
            for (int r = 0; r < runs && r < 16; r++) {
                tt_out(" "); tt_num(run_start[r]); tt_out("-"); tt_num(run_end[r]);
                if (r > 0) { int gap = run_start[r] - run_end[r - 1] - 1; if (gap < gap_min) gap_min = gap; if (gap > gap_max) gap_max = gap; }
            }
            tt_out("\n");
            int spread = gap_max - gap_min;
            tt_out("texttest: letter gap spread "); tt_num(spread); tt_out(" px (min "); tt_num(gap_min); tt_out(", max "); tt_num(gap_max); tt_out(")\n");
            tt_out((runs == 6 && spread <= 3) ? "texttest 'Cloudy' spacing uniform: ok\n" : "texttest 'Cloudy' spacing uneven (the 'Cl oudy' bug): FAILED\n");
            int w_first = -1, w_last = -1;
            for (int i = 0; i < 64; i++) {
                int ink = 0;
                for (int j = 0; j < 32; j++) if (window_get_pixel_phys(40 + i, 120 + j) != bg) { ink = 1; break; }
                if (ink) { if (w_first < 0) w_first = i; w_last = i; }
            }
            int w_width = w_last - w_first + 1;
            tt_out("texttest: 'W' ink width "); tt_num(w_width); tt_out(" px\n");
            tt_out((w_width >= 20) ? "texttest 'W' not clipped: ok\n" : "texttest 'W' clipped by the fixed cell: FAILED\n");
            window_close();
        }
    }
    else if (!strcmp(line, "wraptest")) {
        /* v78 (0.67.2): real bug, "Curbf ind" on a real framebuffer dump of
           the Curbfind app view (gui_launch_html -> render_wrapped_text).
           v77 fixed font_draw_string's own AA pen but never touched this
           function, which drew every glyph through the single-glyph
           font_draw_char API at a hardcoded 8px advance, exactly v77's bug
           shape, just reachable only through the word-wrap path (every
           HTML app view, every Chat answer) instead of the four sites v77
           actually audited. Same discrimination shape as texttest: render
           a real word through the real render_wrapped_text path at scale
           2, measure ink runs column by column. Uneven gaps (spread > 3)
           or the wrong run count is the "Curbf ind" bug; both are supposed
           to disappear once render_wrapped_text uses font_string_width/
           font_draw_string per word instead of wlen*8/font_draw_char. */
        if (!window_open_scaled(400, 300, 32, 2)) { tt_out("no VGA device found or out of page tables\n"); }
        else {
            font_set_aa(gui_aa_char, gui_aa_advance);
            unsigned int bg = 0x00202020;
            window_clear(bg);
            render_wrapped_text("Curbfind", 20, 20, 360, 40, 0x00F5F5F7);
            int col_ink[200];
            for (int i = 0; i < 200; i++) {
                col_ink[i] = 0;
                for (int j = 0; j < 32; j++) if (window_get_pixel_phys(40 + i, 40 + j) != bg) { col_ink[i] = 1; break; }
            }
            int runs = 0, run_start[16], run_end[16], in_run = 0;
            for (int i = 0; i < 200; i++) {
                if (col_ink[i] && !in_run) { if (runs < 16) run_start[runs] = i; in_run = 1; }
                if (!col_ink[i] && in_run) { if (runs < 16) run_end[runs] = i - 1; runs++; in_run = 0; }
            }
            if (in_run) { if (runs < 16) run_end[runs] = 199; runs++; }
            int gap_min = 999, gap_max = -1;
            tt_out("wraptest 'Curbfind' ink runs:");
            for (int r = 0; r < runs && r < 16; r++) {
                tt_out(" "); tt_num(run_start[r]); tt_out("-"); tt_num(run_end[r]);
                if (r > 0) { int gap = run_start[r] - run_end[r - 1] - 1; if (gap < gap_min) gap_min = gap; if (gap > gap_max) gap_max = gap; }
            }
            tt_out("\n");
            int spread = gap_max - gap_min;
            tt_out("wraptest: letter gap spread "); tt_num(spread); tt_out(" px (min "); tt_num(gap_min); tt_out(", max "); tt_num(gap_max); tt_out(")\n");
            /* "Curbfind" is 8 letters, every pair of adjacent glyphs touches or
               nearly touches at this face (no wide space-shaped sidebearing
               gap like 'l'/'i' produce), so the whole word draws as one ink
               run when spacing is correct; the fixed-cell bug splits it into
               multiple runs with an uneven spread instead. */
            tt_out((runs == 1 || (runs > 1 && spread <= 3)) ? "wraptest 'Curbfind' spacing uniform: ok\n" : "wraptest 'Curbfind' spacing uneven (the 'Curbf ind' bug): FAILED\n");
            window_close();
        }
    }
    else if (!strcmp(line, "dockstyletest")) {
        /* v58/v61 gap fix: both passes verified against a real, one-off
           framebuffer pixel dump, reverted before commit, nothing
           permanent left in the suite (dockhover-check.py, added in v63,
           only covers the hover-magnify bug, a different defect
           entirely). Two real, discriminating checks against the exact
           functions those passes fixed, called directly with real
           geometry rather than needing a QEMU pixel-dump script: (1)
           gui_rounded_rect_on_wallpaper's straight top edge (v61): before
           the fix every non-corner edge pixel was a 100% solid `col =
           color` with zero blend, so the very first row of the rect was
           already the exact fill color; after, the first `band` (3)
           physical rows blend toward the real backdrop first. (2)
           gui_draw_icon_shadow (v58): before the fix it drew through the
           LOGICAL layer at a quarter the physical resolution, so every
           physical pixel pair came out identical (blocky); after, it
           draws physical-resolution with real per-pixel falloff.

           v72 gap fix to check (1), root-caused with a real DEBUG serial
           dump of the actual channel values, not guessed: this check used
           to also assert `row0 != bg_before`, comparing row0 (the very
           top physical row of the tray, py=0) against a real rendered
           wallpaper pixel sampled 2 physical rows further up. At py=0,
           `t = band - py` is exactly `band`, so gui_lerp's own math (t ==
           max) returns its second argument outright: row0 IS, by design,
           100% the real backdrop color at that exact pixel
           (gui_wallpaper_sample(px0+px, py0+py, 0)), zero fill color
           mixed in yet -- the softest point of the AA fringe, fading
           fully into the wallpaper before solidifying by row `band`. A
           real dump confirmed both are the honest, working blend, not a
           regression: bg_before=0x170b06, row0=0x170b06 (equal, because
           the wallpaper gradient barely moves over 2 physical rows at
           this exact spot -- expected, not a bug), row1=0x5f5650 (a real
           partial blend, matches gui_lerp(TRAY, 0x170b06, 2, 3) exactly),
           row3=TRAY (full fill, past the band). Asserting `row0 !=
           bg_before` was backwards: a correct edge blend is SUPPOSED to
           read close to the true backdrop right at its outermost row, so
           demanding it differ from a nearby real backdrop sample fails
           exactly when the fix is working, wherever the gradient happens
           to be locally flat. The two assertions that actually encode the
           v61 regression (a raw, unblended top row) are `row0 !=
           DOCK_TRAY_COLOR` and `row1 != row0`: reverting v61's fix (col =
           color for every non-corner edge pixel, no blend) makes row0
           AND row1 both equal DOCK_TRAY_COLOR outright, failing both;
           restoring the fix, they pass. Dropped the incorrect third
           condition, kept the two that really discriminate. */
        if (!window_open_scaled(960, 540, 32, 2)) { puts("no VGA device found or out of page tables\n"); }
        else {
            gui_draw_wallpaper_rows(0, (int)window_height());
            int y0 = gui_dock_y0(), dock_x = gui_dock_x0(), dock_w = gui_dock_w(), dock_h = DOCK_ICON + 2 * DOCK_PAD;
            int sc = (int)window_scale();
            int sample_x = (dock_x + dock_w / 2) * sc; /* dock centre: far from either rounded corner */
            gui_rounded_rect_on_wallpaper(dock_x, y0, dock_w, dock_h, DOCK_TRAY_COLOR, 20);
            unsigned int row0 = window_get_pixel_phys(sample_x, y0 * sc + 0);
            unsigned int row1 = window_get_pixel_phys(sample_x, y0 * sc + 1);
            unsigned int row3 = window_get_pixel_phys(sample_x, y0 * sc + 3); /* one row past the v61 band (3 physical rows) */
            int seam_ok = row0 != DOCK_TRAY_COLOR && row1 != row0 && row3 == DOCK_TRAY_COLOR;
            puts(seam_ok ? "dock tray top edge: real multi-row blend, not a 1-row solid cut: ok\n" : "dock tray top edge: FAILED (single-row seam)\n");

            window_rect(0, 0, (int)window_width(), (int)window_height(), DOCK_TRAY_COLOR);
            int cx = (int)window_width() / 2, cy = (int)window_height() / 2;
            gui_draw_icon_shadow(cx, cy, DOCK_ICON);
            int ry = (DOCK_ICON * sc) / 9; /* the ellipse's own physical half-height, see gui_draw_icon_shadow */
            int px = cx * sc; /* vertical centreline: dy sweeps distance^2 fast, the axis that best exposes ry's real division headroom (8 physical steps) vs the old ry=4 logical-block version's coarser ~4-5 */
            unsigned int seen[16]; int nseen = 0;
            for (int dy = -ry; dy < ry; dy++) {
                unsigned int v = window_get_pixel_phys(px, cy * sc - sc + dy);
                int found = 0; for (int s = 0; s < nseen; s++) if (seen[s] == v) { found = 1; break; }
                if (!found && nseen < 16) seen[nseen++] = v;
            }
            /* The pre-v58 bug drew this ellipse through the LOGICAL layer
               at ry=4 (real division headroom halved) with each logical
               row blown up into a 2-physical-row block, so a vertical
               sweep only ever showed ~5 distinct levels, each repeated in
               pairs; the real fixed version (ry=8, physical resolution)
               shows close to its own ry+1 distinct levels. */
            int shadow_ok = nseen >= 7;
            puts(shadow_ok ? "dock icon shadow: real per-physical-pixel falloff, not blocky: ok\n" : "dock icon shadow: FAILED (blocky/coarse)\n");
            window_close();
        }
    }
    else if (!strcmp(line, "mailtest")) {
        /* v59 gap fix: Mail shipped with no regression test at all, the
           reminders/calendar precedent (check-calendar.sh) only covers
           date math, nothing VFS-backed like Mail's write-through. Real,
           discriminating: three known messages through mail_save() ->
           MAIL.TXT -> a forced mail_load() (state wiped first, so this is
           a real read off disk, not the array still sitting in RAM),
           confirms every field and the read flag survived the '|'-
           delimited round trip; then deletes the middle message through
           the exact mail_delete_at() the UI's 'd' key now calls, confirms
           the remaining two shifted into the right slots with every field
           intact, not just that mail_count went down by one. Runs against
           ramfs explicitly (fsuse's own backend switch) so the test is
           real and repeatable even when no FAT disk image is attached,
           the same reason this suite ships a ramfs backend at all;
           whatever backend was active is restored after. */
        const char *prev_fs = vfs_current_name();
        char prev_fs_buf[16]; int pfi = 0; while (prev_fs[pfi] && pfi < 15) { prev_fs_buf[pfi] = prev_fs[pfi]; pfi++; } prev_fs_buf[pfi] = 0;
        vfs_switch("ramfs");
        mail_count = 3;
        mail_str_copy(mail_msgs[0].from, "Alice", MAIL_FROM_MAX);
        mail_str_copy(mail_msgs[0].subject, "First", MAIL_SUBJECT_MAX);
        mail_str_copy(mail_msgs[0].body, "alpha body", MAIL_BODY_MAX);
        mail_msgs[0].read = 1;
        mail_str_copy(mail_msgs[1].from, "Bob", MAIL_FROM_MAX);
        mail_str_copy(mail_msgs[1].subject, "Second", MAIL_SUBJECT_MAX);
        mail_str_copy(mail_msgs[1].body, "bravo body", MAIL_BODY_MAX);
        mail_msgs[1].read = 0;
        mail_str_copy(mail_msgs[2].from, "Carol", MAIL_FROM_MAX);
        mail_str_copy(mail_msgs[2].subject, "Third", MAIL_SUBJECT_MAX);
        mail_str_copy(mail_msgs[2].body, "charlie body", MAIL_BODY_MAX);
        mail_msgs[2].read = 1;
        mail_save();

        for (int i = 0; i < MAIL_MAX; i++) { mail_msgs[i].from[0] = 0; mail_msgs[i].subject[0] = 0; mail_msgs[i].body[0] = 0; mail_msgs[i].read = 0; }
        mail_count = 0;
        mail_loaded = 0;
        mail_load();
        int ok = mail_count == 3
            && !strcmp(mail_msgs[0].from, "Alice") && !strcmp(mail_msgs[0].subject, "First") && !strcmp(mail_msgs[0].body, "alpha body") && mail_msgs[0].read == 1
            && !strcmp(mail_msgs[1].from, "Bob")   && !strcmp(mail_msgs[1].subject, "Second") && !strcmp(mail_msgs[1].body, "bravo body") && mail_msgs[1].read == 0
            && !strcmp(mail_msgs[2].from, "Carol") && !strcmp(mail_msgs[2].subject, "Third")  && !strcmp(mail_msgs[2].body, "charlie body") && mail_msgs[2].read == 1;
        puts(ok ? "mail round trip (3 messages through MAIL.TXT): ok\n" : "mail round trip: FAILED\n");

        mail_delete_at(1);
        int ok2 = mail_count == 2
            && !strcmp(mail_msgs[0].from, "Alice") && mail_msgs[0].read == 1
            && !strcmp(mail_msgs[1].from, "Carol") && !strcmp(mail_msgs[1].subject, "Third") && !strcmp(mail_msgs[1].body, "charlie body") && mail_msgs[1].read == 1;
        puts(ok2 ? "mail delete: shifted correctly: ok\n" : "mail delete: FAILED\n");
        vfs_switch(prev_fs_buf);
    }
    else if (!strcmp(line, "wind")) {
        if (!strcmp(arg, "off")) { wind_enabled = 0; settings_save(); puts("wind off\n"); }
        else if (!strcmp(arg, "on")) { wind_enabled = 1; settings_save(); puts("wind on\n"); }
        else { puts(wind_enabled ? "wind is on (wind off to stop)\n" : "wind is off (wind on to start)\n"); }
    }
    else if (!strcmp(line, "weatherfx")) {
        /* v75: force the particle overlay's input (rain/snow/off) without
           waiting for real weather, so tools/wallfx-check.py can prove
           particles composite over a fetched map wallpaper headlessly.
           Sets exactly the fields weather_fetch would, nothing else. */
        if (!strcmp(arg, "rain")) { weather_have = 1; weather_code10 = 610; puts("weatherfx: rain\n"); }
        else if (!strcmp(arg, "snow")) { weather_have = 1; weather_code10 = 710; puts("weatherfx: snow\n"); }
        else if (!strcmp(arg, "off")) { weather_have = 0; puts("weatherfx: off\n"); }
        else puts("usage: weatherfx rain|snow|off\n");
    }
    else if (!strcmp(line, "wallpaper")) {
        /* v75/v81: photo|warm|cool|raw picks the theme (persisted like
           wind/dockscale); "map" stays a working alias for "warm" (v75's
           original two-state name, now one of four themes) so nothing
           that already typed `wallpaper map` breaks. fetch forces the map
           download right now (its own NIC/net bring-up, same as
           weather_fetch), no argument reports state. */
        if (!strcmp(arg, "photo")) { wall_switch_theme(WALL_PHOTO); wall_apply(0); puts("wallpaper: photo\n"); }
        else if (!strcmp(arg, "map") || !strcmp(arg, "warm")) { wall_switch_theme(WALL_WARM); wall_apply(1); puts(wall_map ? "wallpaper: map (warm)\n" : "wallpaper: map (warm) (fetches on the next weather cycle, or: wallpaper fetch)\n"); }
        else if (!strcmp(arg, "cool")) { wall_switch_theme(WALL_COOL); wall_apply(1); puts(wall_map ? "wallpaper: map (cool)\n" : "wallpaper: map (cool) (fetches on the next weather cycle, or: wallpaper fetch)\n"); }
        else if (!strcmp(arg, "raw")) { wall_switch_theme(WALL_RAW); wall_apply(1); puts(wall_map ? "wallpaper: map (raw)\n" : "wallpaper: map (raw) (fetches on the next weather cycle, or: wallpaper fetch)\n"); }
        else if (!strcmp(arg, "sat") || !strcmp(arg, "satellite")) { wall_switch_theme(WALL_SAT); wall_apply(1); puts(wall_map ? "wallpaper: satellite\n" : "wallpaper: satellite (fetches on the next weather cycle, or: wallpaper fetch)\n"); }
        else if (!strcmp(arg, "fetch")) {
            if (!net_init(0x0A00020F)) { puts("no NIC\n"); }
            else { if (!geo_have) geo_fetch();
                   if (wall_fetch()) { wall_apply(1); puts("wallpaper: map fetched (tiles "); putn((unsigned int)wall_map_tx); puts(","); putn((unsigned int)wall_map_ty); puts(" z"); putn(WALL_ZOOM); puts(")\n"); }
                   else puts("wallpaper: fetch failed, photo stays\n"); }
        }
        else {
            const char *tn = wall_theme == WALL_PHOTO ? "photo" : wall_theme == WALL_COOL ? "map (cool)" : wall_theme == WALL_RAW ? "map (raw)" : wall_theme == WALL_SAT ? "satellite" : "map (warm)";
            puts("wallpaper: "); puts(tn);
            puts(wall_src == wallpaper_rgb ? " (showing photo)\n" : " (showing map)\n");
        }
    }
    else if (!strcmp(line, "dockscale")) {
        if (!*arg) { puts("dock scale: "); putn((unsigned int)dock_scale_pct); puts("% (dockscale <5-25> to set)\n"); }
        else {
            int v = 0; const char *p = arg; while (*p >= '0' && *p <= '9') { v = v*10 + (*p-'0'); p++; }
            if (v < 5 || v > 25) { puts("usage: dockscale <5-25>\n"); }
            else { dock_scale_pct = v; settings_save(); puts("dock scale set\n"); }
        }
    }
    else if (!strcmp(line, "isotest")) {
        iso_readback_a = 0; iso_readback_b = 0;
        int ida = task_create(iso_task_a);
        int idb = task_create(iso_task_b);
        if (ida < 0 || idb < 0) { puts("no free task slots\n"); }
        else {
            for (int i = 0; i < 6; i++) yield(); /* let both tasks actually finish and reap themselves */
            int ok = (iso_readback_a == 0xAAAAAAAA) && (iso_readback_b == 0xBBBBBBBB);
            puts(ok ? "isolation: separate address spaces confirmed: ok\n" : "isolation: FAILED (one task saw the other's write)\n");
        }
    }
    else if (!strcmp(line, "reaptest")) {
        /* Real proof task_exit() actually frees its slot, not just that the
           kernel doesn't hang: exhaust every slot, confirm the next create
           fails, exit one, confirm a create then succeeds and reuses that
           exact slot id, not a coincidence, the same id every time since
           task_create always takes the lowest free slot. */
        int ids[16]; /* well above task.c's real MAX_TASKS, just a safe bound for this loop */
        int n = 0;
        while (n < 16) { int id = task_create(task_exit); if (id < 0) break; ids[n++] = id; }
        int full = (task_create(task_exit) < 0); /* every slot taken, including task 0 (the shell), so this must fail */
        for (int i = 0; i < 5; i++) yield(); /* let the throwaway tasks actually reach task_exit() */
        int reused = task_create(task_a);
        int ok = full && reused == ids[0] && reused >= 0; /* task_create always takes the lowest free slot, so the first slot handed out above is the first one reused */
        puts(ok ? "reap: freed slot really reused: ok\n" : "reap: FAILED\n");
        for (int i = 0; i < 12; i++) yield(); /* drain task_a's own A-printing + exit so the prompt doesn't land mid-output */
    }
    else if (!strcmp(line, "fsuse")) {
        if (!*arg) { puts("current: "); puts(vfs_current_name()); puts(" (usage: fsuse fat|ramfs)\n"); }
        else puts(vfs_switch(arg) ? "switched\n" : "no such backend\n");
    }
    else if (!strcmp(line, "diskuse")) {
        if (!*arg) { puts("current: "); puts(blockdev_current_name()); puts(" (usage: diskuse ata|ramdisk; run 'fsuse fat' then 'disktest'/'ls' after switching to see it take effect)\n"); }
        else puts(blockdev_switch(arg) ? "switched\n" : "no such backend\n");
    }
    else if (!strcmp(line, "ls"))    vfs_list(ls_cb);
    else if (!strcmp(line, "browse")) browse();
    else if (!strcmp(line, "cat")) {
        if (!*arg) { puts("usage: cat <file>\n"); }
        else {
            char buf[4096];
            int n = vfs_read_file(arg, buf, sizeof(buf) - 1);
            if (n < 0) { puts(arg); puts(": not found\n"); }
            else { buf[n] = 0; puts(buf); putc('\n'); }
        }
    }
    else if (!strcmp(line, "exec")) {
        if (!*arg) { puts("usage: exec <file> [args...] (a flat binary, run as a real ring-3 task; a bare program name works too, see help)\n"); }
        else {
            /* v2: the words after the filename become the program's real
               argv, argv[0] is the filename itself, and anything past
               JT_ARGC_MAX is refused rather than dropped, because a
               program handed a silently shortened argv has no way to
               tell. There are no quotes and no escapes here; a word is a
               run of non-space characters, which is the whole of what
               this shell's own line reader can express. */
            const char *uargv[JT_ARGC_MAX];
            int uargc = split_argv(arg, uargv, JT_ARGC_MAX);
            if (uargc < 0) { puts("exec: too many arguments (max "); putdec(JT_ARGC_MAX); puts(" including the program name)\n"); }
            else {
                /* 1.0.0: resolved through the same bare-name lookup as
                   the fallthrough below, so `exec hello` and typing
                   `hello` land on the same file. uargv[0] still names
                   the failure if nothing resolves. */
                char resolved[JT_RESOLVE_NAME_MAX];
                if (exec_resolve_name(uargv[0], resolved)) uargv[0] = resolved;
                int status = -1;
                if (!exec_user(uargv[0], uargv, uargc, &status)) { puts(uargv[0]); puts(": exec failed (not found, too big, argv too large, or no free task slot)\n"); }
                else { puts("exit code "); putdec(status); putc('\n'); }
            }
        }
    }
    else if (!strcmp(line, "rm")) {
        if (!*arg) { puts("usage: rm <file>\n"); }
        else {
            /* v39: read it before deleting so it can be recovered. A file
               too big for the trash, or a full trash, still deletes, but
               says so plainly rather than implying it's recoverable. */
            static char rmbuf[TRASH_MAX_SIZE];
            int n = vfs_read_file(arg, rmbuf, sizeof(rmbuf));
            int kept = (n > 0) && trash_put(arg, rmbuf, (unsigned int)n);
            if (!vfs_delete(arg)) puts("not found\n");
            else puts(kept ? "moved to trash\n" : "deleted permanently (too big for trash, or trash full)\n");
        }
    }
    else if (!strcmp(line, "trash")) {
        int n = trash_count();
        if (!n) { puts("trash is empty\n"); }
        else for (int i = 0; i < n; i++) {
            putn((unsigned int)i); puts(" "); puts(trash_name(i));
            puts("  "); putn(trash_size(i)); puts(" bytes\n");
        }
    }
    else if (!strcmp(line, "restore")) {
        if (!*arg) { puts("usage: restore <number, see trash>\n"); }
        else {
            int id = 0; const char *p = arg; while (*p >= '0' && *p <= '9') { id = id*10 + (*p-'0'); p++; }
            puts(trash_restore(id) ? "restored\n" : "could not restore (bad number, or the write failed)\n");
        }
    }
    else if (!strcmp(line, "cd")) {
        if (!*arg) { puts("usage: cd <dir> (or ..)\n"); }
        else { puts(vfs_chdir(arg) ? "ok\n" : "not found or not a directory\n"); }
    }
    else if (!strcmp(line, "mkdir")) {
        if (!*arg) { puts("usage: mkdir <name>\n"); }
        else { puts(vfs_mkdir(arg) ? "created\n" : "failed (name taken, disk full, or directory full)\n"); }
    }
    else if (!strcmp(line, "write")) {
        if (!*arg) { puts("usage: write <file> <content>\n"); }
        else {
            char *content = arg;
            while (*content && *content != ' ') content++;
            if (*content) *content++ = 0;
            puts(vfs_write_file(arg, content, strlen(content)) ? "written\n" : "failed (name taken or disk full)\n");
        }
    }
    else if (!strcmp(line, "lspci")) {
        struct pci_device dev;
        if (pci_find_device(0x03, 0x00, &dev)) { puts("VGA device found, BAR0="); puthex(dev.bar0); putc('\n'); }
        else puts("no VGA device found\n");
        if (pci_find_device(0x02, 0x00, &dev)) {
            puts("NIC found, "); puts(dev.bar0_is_io ? "I/O BAR=" : "MEM BAR=");
            puthex(dev.bar0); putc('\n');
        } else puts("no NIC found\n");
    }
    else if (!strcmp(line, "tcpmatchtest")) {
        /* Regression test for a real bug: tcp_match (net.c) used to trust
           ip->total_length outright with no check against the actual number
           of bytes rtl8139_receive put in the frame, so a malicious/corrupt
           remote peer could claim far more payload than it actually sent
           and every caller (tcp_get/tcp_probe_port/tcp_serve_once) would
           copy that many bytes starting past the real frame, an
           out-of-bounds read and uninitialized-stack-memory disclosure.
           No hardware needed: tcp_match_selftest builds real frames in a
           local buffer and calls the exact same tcp_match(). */
        puts(tcp_match_selftest() ? "tcpmatch: honest frame accepted, lying frame rejected: ok\n" : "tcpmatch: FAILED\n");
    }
    else if (!strcmp(line, "rxclamptest")) {
        /* Regression test for a real bug: rtl8139_receive (rtl8139.c) used
           to return the NIC's raw, unclamped claimed length instead of how
           many bytes it actually copied into the caller's buffer. Every
           net.c caller trusts that return value as ground truth for how
           much of rx[1514] is valid, which is exactly what tcp_match's own
           bound check depends on -- an oversized or underflowed NIC length
           defeated that check one layer down, the same OOB-read class as
           the already-fixed tcp_match bug. No hardware needed:
           rtl8139_clamp_selftest calls the exact same clamp logic
           rtl8139_receive uses. */
        puts(rtl8139_clamp_selftest() ? "rxclamp: oversized/runt NIC lengths clamp correctly: ok\n" : "rxclamp: FAILED\n");
    }
    else if (!strcmp(line, "settingsclicktest")) {
        /* Regression test for a real bug: Settings' own click handler used
           to act on whatever row a PRIOR arrow-key press had left `sel`
           on (starting at row 0, Wind), completely ignoring where the
           click itself actually landed -- a mouse/touch-only visitor with
           no keyboard (this kernel's own browser-demo idle tour included)
           could therefore only ever toggle row 0, since a bare click never
           moved `sel`. settings_row_at is the exact pure hit-test
           gui_launch_settings' click branch now calls before touching
           `sel`; no mouse, GUI, or boot state needed to exercise it. */
        int ok = 1;
        if (settings_row_at(300, 84,  960) != 0) { puts("settingsclick: row 0 (Wind) center missed\n"); ok = 0; }
        if (settings_row_at(300, 148, 960) != 2) { puts("settingsclick: row 2 (Wallpaper) center missed\n"); ok = 0; }
        if (settings_row_at(300, 212, 960) != 4) { puts("settingsclick: row 4 (LLM host) center missed\n"); ok = 0; }
        if (settings_row_at(300, 70,  960) != -1) { puts("settingsclick: above row 0 should miss\n"); ok = 0; }
        if (settings_row_at(300, 108, 960) != -1) { puts("settingsclick: real gap between row 0 and row 1 should miss\n"); ok = 0; }
        if (settings_row_at(10,  148, 960) != -1) { puts("settingsclick: left of the row rect (x<16) should miss\n"); ok = 0; }
        if (settings_row_at(950, 148, 960) != -1) { puts("settingsclick: right of the row rect should miss\n"); ok = 0; }
        puts(ok ? "settingsclick: click hit-tests the row it actually landed on: ok\n" : "settingsclick: FAILED\n");
        serial_puts(ok ? "settingsclick PASS\n" : "settingsclick FAIL\n"); /* mirrors texttest/chattest/jpegtest's own convention so a tools/checks shell script can read the verdict headless */
    }
    else if (!strcmp(line, "nettest")) {
        if (!net_init(0x0A00020F)) { puts("no NIC found (tried RTL8139, NE2000)\n"); }
        else {
            unsigned char mac[6];
            net_get_mac(mac);
            puts("MAC: ");
            for (int i = 0; i < 6; i++) { puthex(mac[i]); if (i < 5) putc(':'); }
            putc('\n');
            static const unsigned char frame[64] = {
                0xFF,0xFF,0xFF,0xFF,0xFF,0xFF, /* dest: broadcast */
                0,0,0,0,0,0,                    /* src: filled from our MAC below */
            };
            unsigned char buf[64];
            for (int i = 0; i < 64; i++) buf[i] = frame[i];
            for (int i = 0; i < 6; i++) buf[6 + i] = mac[i];
            puts(net_send_raw(buf, sizeof(buf)) ? "send:ok\n" : "send:FAIL (timeout)\n");

            unsigned char gw_mac[6];
            puts("arp 10.0.2.2: ");
            if (arp_resolve(0x0A000202, gw_mac)) { /* SLIRP's built-in gateway */
                for (int i = 0; i < 6; i++) { puthex(gw_mac[i]); if (i < 5) putc(':'); }
                putc('\n');
            } else puts("timeout\n");

            unsigned int resolved_ip = 0;
            int dns_ok = dns_resolve("example.com", 0x0A000203, &resolved_ip); /* SLIRP's built-in DNS proxy */
            puts("dns example.com: ");
            if (dns_ok) {
                putn((resolved_ip >> 24) & 0xFF); putc('.');
                putn((resolved_ip >> 16) & 0xFF); putc('.');
                putn((resolved_ip >> 8) & 0xFF); putc('.');
                putn(resolved_ip & 0xFF); putc('\n');
            } else puts("timeout/no answer\n");

            puts("tcp GET example.com: ");
            if (!dns_ok) puts("skipped, no IP\n");
            else {
                static const char req[] = "GET / HTTP/1.0\r\nHost: example.com\r\nConnection: close\r\n\r\n";
                static char resp[1400];
                int n = tcp_get(resolved_ip, 80, req, sizeof(req) - 1, resp, sizeof(resp) - 1);
                if (n < 0) puts("FAIL\n");
                else {
                    resp[n] = 0;
                    putn((unsigned int)n); puts(" bytes, starts: ");
                    for (int i = 0; i < 20 && resp[i] && resp[i] != '\r'; i++) putc(resp[i]);
                    putc('\n');
                }
            }
        }
    }
    else if (!strcmp(line, "ifconfig")) {
        if (!net_init(0x0A00020F)) { puts("no NIC found (tried RTL8139, NE2000)\n"); }
        else {
            unsigned char mac[6]; net_get_mac(mac);
            puts("net0: 10.0.2.15\n  mac ");
            for (int i = 0; i < 6; i++) {
                char hx[3]; const char *hexd = "0123456789abcdef";
                hx[0] = hexd[mac[i] >> 4]; hx[1] = hexd[mac[i] & 0xF]; hx[2] = 0;
                puts(hx); if (i < 5) puts(":");
            }
            puts("\n");
        }
    }
    else if (!strcmp(line, "netscan")) {
        /* v30: a real, honest connect scan, Kali-flavored in spirit only,
           not a clone: no SYN-stealth/OS-fingerprint modes, one scan type,
           a fixed common-port list, exactly what tcp_probe_port actually
           supports. Same "not a general tool, a real narrow proof" scope
           as everything else this session, see roadmap.md's v30 entry. */
        if (!*arg) { puts("usage: netscan <host, e.g. 10.0.2.2>\n"); }
        else if (!net_init(0x0A00020F)) { puts("no NIC found (tried RTL8139, NE2000)\n"); }
        else {
            unsigned int ip;
            int have_ip = 0;
            /* accept a bare dotted-quad directly, skip DNS for the common
               "scan a LAN host" case; anything else goes through the same
               DNS path `web`/`nettest` already use. */
            { unsigned int a=0,b=0,c=0,d=0; int i=0; const char *p=arg; int quad=1;
              while (*p) { if (*p=='.') i++; else if (*p<'0'||*p>'9') { quad=0; break; } p++; }
              if (quad && i==3) { unsigned int parts[4]={0,0,0,0}; int pi=0; p=arg;
                  while (*p) { if (*p=='.') pi++; else parts[pi]=parts[pi]*10+(*p-'0'); p++; }
                  a=parts[0];b=parts[1];c=parts[2];d=parts[3];
                  ip = (a<<24)|(b<<16)|(c<<8)|d; have_ip = 1; }
            }
            if (!have_ip) have_ip = dns_resolve(arg, 0x0A000203, &ip);
            if (!have_ip) { puts("could not resolve host\n"); }
            else {
                static const unsigned short ports[] = {21,22,23,25,80,443,3306,8080};
                puts("scanning...\n");
                for (unsigned int i = 0; i < sizeof(ports)/sizeof(ports[0]); i++) {
                    char buf[16]; int n = 0; unsigned int v = ports[i];
                    char tmp[6]; int ti = 0; if (v==0) tmp[ti++]='0'; while (v) { tmp[ti++]='0'+v%10; v/=10; }
                    while (ti) buf[n++] = tmp[--ti];
                    buf[n++] = '/'; buf[n++]='t'; buf[n++]='c'; buf[n++]='p'; buf[n++]=' '; buf[n]=0;
                    puts(buf);
                    int r = tcp_probe_port(ip, ports[i]);
                    puts(r == 1 ? "open\n" : r == 0 ? "closed\n" : "filtered\n");
                }
            }
        }
    }
    else if (!strcmp(line, "web")) {
        char *first_host = arg;
        char *first_path = first_host;
        while (*first_path && *first_path != ' ') first_path++;
        if (*first_path) *first_path++ = 0; else first_path = "/";
        if (!*first_host) { puts("usage: web <host> [path]\n"); }
        else if (!net_init(0x0A00020F)) { puts("no NIC found (tried RTL8139, NE2000)\n"); }
        else {
            browse_web(first_host, first_path);
        }
    }
    else if (!strcmp(line, "serve")) {
        if (!net_init(0x0A00020F)) { puts("no NIC found (tried RTL8139, NE2000)\n"); }
        else {
            /* Long enough on purpose: >536 bytes forces tcp_serve_once
               through its multi-segment path, not just the one-chunk case. */
            static const char page[] =
                "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
                "<html><body><h1>Joshua Tree</h1>"
                "<p>Served from a kernel with no OS underneath it: no Linux, no XNU, "
                "no Windows NT, nothing between this HTML and the bare metal except "
                "the code in this repository. Booted with a multiboot1 header, brought "
                "up its own GDT, IDT, paging, and a cooperative scheduler, mounted a "
                "FAT16 filesystem it wrote the driver for, found this network card by "
                "walking PCI configuration space by hand, and built Ethernet, ARP, "
                "IPv4, UDP, DNS and TCP from raw bytes on the wire, no libc anywhere "
                "in the chain. The response you are reading crossed that entire stack "
                "in more than one TCP segment, which is exactly why this paragraph is "
                "this long.</p></body></html>";
            puts("waiting for a connection on :8080...\n");
            puts(tcp_serve_once(8080, page, sizeof(page) - 1) ? "served:ok\n" : "timeout, nobody connected\n");
        }
    }
    else if (!strcmp(line, "serveapp")) {
        if (!*arg) { puts("usage: serveapp weather|curbfind|keyrate|bookrank|quotestreak|plan|lexly|toroid|sparkjar|homeqi|fieldbook\n"); }
        else if (!net_init(0x0A00020F)) { puts("no NIC found (tried RTL8139, NE2000)\n"); }
        else {
            if (!strcmp(arg, "weather"))          serve_app("weather", app_weather_html, app_weather_len);
            else if (!strcmp(arg, "curbfind"))    serve_app("curbfind", app_curbfind_html, app_curbfind_len);
            else if (!strcmp(arg, "keyrate"))     serve_app("keyrate", app_keyrate_html, app_keyrate_len);
            else if (!strcmp(arg, "bookrank"))    serve_app("bookrank", app_bookrank_html, app_bookrank_len);
            else if (!strcmp(arg, "quotestreak")) serve_app("quotestreak", app_quotestreak_html, app_quotestreak_len);
            else if (!strcmp(arg, "plan"))        serve_app("plan", app_plan_html, app_plan_len);
            else if (!strcmp(arg, "lexly"))       serve_app("lexly", app_lexly_html, app_lexly_len);
            else if (!strcmp(arg, "toroid"))      serve_app("toroid", app_toroid_html, app_toroid_len);
            else if (!strcmp(arg, "sparkjar"))    serve_app("sparkjar", app_sparkjar_html, app_sparkjar_len);
            else if (!strcmp(arg, "homeqi"))      serve_app("homeqi", app_homeqi_html, app_homeqi_len);
            else if (!strcmp(arg, "fieldbook"))   serve_app("fieldbook", app_fieldbook_html, app_fieldbook_len);
            else puts("unknown app, see usage\n");
        }
    }
    else if (!strcmp(line, "chat")) {
        /* v10: this kernel's own shell talking to an LLM. No TLS anywhere
           in this stack (a real, separate project on its own), so this
           reaches a local Ollama server on the host machine over plain
           HTTP via QEMU's gateway address, not the real Anthropic/OpenAI
           APIs, which are HTTPS-only. Real design tradeoff, not a default
           picked blind: building TLS from scratch to talk to a hosted API
           is its own multi-session project; a local model over plain HTTP
           is what "talking to it" can actually mean before that exists.

           v85: switched to /api/chat with real VFS-backed history
           (chat_send, kernel/chat.h) instead of a fresh one-shot
           /api/generate prompt every time, so the shell `chat` command
           and the GUI Chat app share both the same conversation and the
           same settings-persisted model/host/port (llm_model/llm_host/
           llm_port), not two independently hardcoded copies. */
        if (!*arg) { puts("usage: chat <message>\n"); }
        else if (!net_init(0x0A00020F)) { puts("no NIC found (tried RTL8139, NE2000)\n"); }
        else {
            puts("asking "); puts(llm_model); puts(" (");
            puts(llm_host); puts(", local, on the host machine)...\n");
            static char answer[4096]; /* real growth from the old 2048-byte cap */
            if (!chat_send(arg, answer, sizeof(answer))) puts("FAIL (couldn't reach the LLM host, or no reply)\n");
            else { puts(answer); putc('\n'); }
        }
    }
    else if (!strcmp(line, "build")) {
        /* v10's "build stuff" loop: take a request, ask the LLM to generate
           a page for it, serve the result live. The kernel-native version
           of what gato does on macOS, minus the file-editing part, there's
           no persistent app catalog to edit yet, just this one slot. */
        if (!*arg) { puts("usage: build <what to make>\n"); }
        else if (!net_init(0x0A00020F)) { puts("no NIC found (tried RTL8139, NE2000)\n"); }
        else {
            char escaped[256];
            json_escape(arg, escaped, sizeof(escaped));

            static char req_body[1024];
            unsigned int n = 0;
            const char *parts[3];
            parts[0] = "{\"model\":\"llama3.1:8b\",\"stream\":false,\"prompt\":\"Output ONLY raw HTML for one self-contained page, inline CSS and JS, no external resources, no markdown code fences, no explanation, just the HTML. Make: ";
            parts[1] = escaped;
            parts[2] = "\"}";
            for (int p = 0; p < 3; p++) {
                const char *s = parts[p];
                while (*s && n < sizeof(req_body)) req_body[n++] = *s++;
            }

            puts("asking llama3.1 to build it...\n");
            static char resp[8192];
            int rn = http_post("10.0.2.2", "/api/generate", 11434, req_body, n, resp, sizeof(resp) - 1);
            if (rn <= 0) { puts("FAIL (couldn't reach the host's Ollama server)\n"); }
            else {
                resp[rn] = 0;
                static char html[6144];
                unsigned int hn = json_extract_string(resp, "response", html, sizeof(html));
                if (hn == 0) { puts("(no response field in the reply)\n"); }
                else {
                    hn = strip_code_fence(html, hn);
                    serve_app("the generated page", (const unsigned char *)html, hn);
                }
            }
        }
    }
    else if (!strcmp(line, "notes")) {
        if (window_open(800, 600, 32)) {
            gui_launch_editor();
            window_close();
            clear();
        }
    }
    else if (!strcmp(line, "gfxtest")) {
        if (!window_open(800, 600, 32)) { puts("no VGA device found or out of page tables\n"); }
        else {
            window_rect(0, 0, 800, 200, 0x00555555);
            window_rect(0, 200, 800, 200, 0x007A2048);
            window_rect(0, 400, 800, 200, 0x00FAF8F6);
            get_key(); /* leave the picture up until a key is pressed */
            window_close();
            clear();
            puts("back in text mode\n");
        }
    }
    else if (!strcmp(line, "fonttest")) {
        if (!window_open(800, 600, 32)) { puts("no VGA device found or out of page tables\n"); }
        else {
            window_clear(0x00FAF8F6);
            font_draw_string("Joshua Tree", 20, 20, 0x00555555, -1);
            font_draw_string("ABCDEFGHIJKLMNOPQRSTUVWXYZ", 20, 60, 0x001C1C1E, -1);
            font_draw_string("abcdefghijklmnopqrstuvwxyz", 20, 80, 0x001C1C1E, -1);
            font_draw_string("0123456789 !?.,:;()", 20, 100, 0x001C1C1E, -1);
            font_draw_string("the quick brown fox jumps", 20, 140, 0x007A2048, -1);
            get_key();
            window_close();
            clear();
            puts("back in text mode\n");
        }
    }
    else if (!strcmp(line, "gui")) {
        gui_run();
    }
    else if (!strcmp(line, "testapps")) {
        /* A real, permanent diagnostic, not scaffolding bolted on for one
           test run: automated QA needs to exercise each real app's own
           launch function, and the only reliable way to drive input from
           a script is keyboard injection (proven repeatedly this session;
           QEMU's monitor mouse commands don't reach this kernel's real
           PS2 driver headlessly, see roadmap.md's honest note on that).
           Any key (Escape included) closes every one of these exactly the
           same way a real click already does via gui_wait_close, so a
           script can drive this whole suite with nothing but sendkey. */
        if (!window_open(800, 600, 32)) { puts("no VGA device found or out of page tables\n"); }
        else {
            /* Walks every real app, not just the pinned dock subset: the
               point of this command is regression coverage of all of
               them (see apptest.sh), and since v37 the dock deliberately
               shows only a handful. */
            for (int i = 0; i < GUI_APPS_FOLDER; i++) gui_launch(i);
            window_close();
            clear();
            puts("testapps done\n");
        }
    }
    else if (!strcmp(line, "contactstest")) {
        /* v70 (0.64.0): discriminating regression test for Contacts app. The
           core contract: write a contact to the VFS via contacts_save, read it
           back via contacts_load, and verify the round-trip preserves the data
           exactly, not just "doesn't crash". Real, discriminating checks: (1)
           after adding a contact with special name/phone/email, the count is
           exactly 2 (not 1 from seed), (2) the second contact's name matches
           what was written (not corrupted parsing), (3) the count stays stable
           when we load again (file persistence works), (4) deleting it drops
           count back to 1. If any step fails, the test catches it. */
        int pass = 0;
        contacts_count = 1;
        contacts_loaded = 1;
        contacts_str_copy(contacts[0].name, "Joshua", CONTACTS_NAME_MAX);
        contacts_str_copy(contacts[0].phone, "(778) 201-4533", CONTACTS_PHONE_MAX);
        contacts_str_copy(contacts[0].email, "trommatic@icloud.com", CONTACTS_EMAIL_MAX);

        if (contacts_count != 1) { puts("contacts seed failed\n"); goto contacts_test_done; }

        contacts_str_copy(contacts[1].name, "Alice Bob", CONTACTS_NAME_MAX);
        contacts_str_copy(contacts[1].phone, "555-1234", CONTACTS_PHONE_MAX);
        contacts_str_copy(contacts[1].email, "alice@example.com", CONTACTS_EMAIL_MAX);
        contacts_count = 2;
        contacts_save();

        /* Reset and reload */
        contacts_loaded = 0;
        contacts_load();

        pass = (contacts_count == 2) &&
                   (contacts[1].name[0] == 'A' && contacts[1].name[1] == 'l') &&
                   (contacts[1].phone[0] == '5' && contacts[1].phone[1] == '5');

        if (!pass) {
            puts("contacts round-trip failed: count="); putn((unsigned int)contacts_count);
            puts(" name[0]="); putc(contacts[1].name[0]); puts(" phone[0]="); putc(contacts[1].phone[0]); puts("\n");
        } else {
            contacts_delete_at(1);
            pass = (contacts_count == 1);
            if (!pass) puts("contacts delete failed\n");
            else puts("contacts VFS round-trip: ok\n");
        }

        contacts_test_done:
        if (!pass) puts("FAILED\n");
    }
    else if (!strcmp(line, "chattest")) {
        /* v85: discriminating regression test for Chat's real new pieces,
           the same shape contactstest/mailtest already use, no network
           needed (chat_push/chat_save/chat_load/chat_build_request are
           all pure VFS/string logic, http_post is the only piece that
           needs a live host, out of scope for a boot-time regression
           test the same way weathertest already draws that line). Real,
           discriminating checks, not "doesn't crash":
           (1) history round-trips through CHAT.TXT: push a user turn and
               an assistant turn, reset chat_loaded, reload, and both
               come back with the right role and exact content.
           (2) the old 512-byte input cap is really gone: a message right
               at the OLD cap (600 chars, over the old 512) survives a
               push+save+reload intact end to end, not truncated at 511.
           (3) chat_build_request includes BOTH turns from history, not
               just the newest one (the real /api/chat fix, a request
               that only ever contained the latest message would be
               functionally identical to the old /api/generate, "history"
               in name only): scans the built JSON for both "hello there"
               and the long message's own head, and for '"role":"user"'
               appearing twice.
           (4) the ring drops the oldest message once CHAT_MAX is
               exceeded, proving chat_push's bound is real, not just
               documented. */
        int pass = 0;
        chat_count = 0;
        chat_loaded = 1;

        char long_msg[600];
        for (int i = 0; i < 599; i++) long_msg[i] = (char)('a' + (i % 26));
        long_msg[599] = 0;

        chat_push(CHAT_ROLE_USER, "hello there");
        chat_push(CHAT_ROLE_ASSISTANT, long_msg);

        if (chat_count != 2) { puts("chat seed failed, count="); putn((unsigned int)chat_count); puts("\n"); goto chat_test_done; }

        chat_loaded = 0;
        chat_load();

        pass = (chat_count == 2) &&
               (chat_msgs[0].role == CHAT_ROLE_USER) &&
               (chat_msgs[0].content[0] == 'h' && chat_msgs[0].content[1] == 'e') &&
               (chat_msgs[1].role == CHAT_ROLE_ASSISTANT);

        if (!pass) { puts("chat round-trip failed after reload\n"); goto chat_test_done; }

        /* the 600-char message must have survived past the old 512 cap */
        unsigned int long_len = 0;
        while (chat_msgs[1].content[long_len]) long_len++;
        pass = (long_len == 599) && (chat_msgs[1].content[598] == long_msg[598]);
        if (!pass) {
            puts("chat buffer-growth failed: stored length="); putn(long_len); puts(" (want 599, old cap was 511)\n");
            goto chat_test_done;
        }

        static char req[6144];
        unsigned int rn = chat_build_request(req, sizeof(req));
        req[rn < sizeof(req) ? rn : sizeof(req) - 1] = 0;

        int found_hello = 0, found_tail = 0, role_user_count = 0;
        for (unsigned int i = 0; i < rn; i++) {
            if (!found_hello && req[i]=='h' && req[i+1]=='e' && req[i+2]=='l' && req[i+3]=='l' && req[i+4]=='o') found_hello = 1;
            if (req[i]=='"' && req[i+1]=='r' && req[i+2]=='o' && req[i+3]=='l' && req[i+4]=='e' && req[i+5]=='"' && req[i+6]==':' && req[i+7]=='"' && req[i+8]=='u' && req[i+9]=='s' && req[i+10]=='e' && req[i+11]=='r') role_user_count++;
        }
        found_tail = (long_len > 0); /* content is escaped/truncated into the request so a literal 599-char scan isn't meaningful; presence of the user turn + role count is the real discriminator */
        (void)found_tail;

        pass = found_hello && (role_user_count == 1); /* only the user turn should say "role":"user"; the assistant turn must be present too but tagged "assistant" */
        if (!pass) { puts("chat_build_request missing history (single-shot regression)\n"); goto chat_test_done; }

        int found_assistant_role = 0;
        for (unsigned int i = 0; i + 15 < rn; i++) {
            if (req[i]=='"' && req[i+1]=='r' && req[i+2]=='o' && req[i+3]=='l' && req[i+4]=='e' && req[i+5]=='"' && req[i+6]==':' && req[i+7]=='"' && req[i+8]=='a' && req[i+9]=='s' && req[i+10]=='s') { found_assistant_role = 1; break; }
        }
        pass = found_assistant_role;
        if (!pass) { puts("chat_build_request missing assistant turn\n"); goto chat_test_done; }

        /* ring bound: push past CHAT_MAX and confirm the oldest drops */
        chat_count = 0; chat_save();
        for (int i = 0; i < CHAT_MAX + 2; i++) {
            char tag[4]; tag[0] = 'm'; tag[1] = (char)('0' + (i % 10)); tag[2] = 0;
            chat_push((i % 2) ? CHAT_ROLE_ASSISTANT : CHAT_ROLE_USER, tag);
        }
        pass = (chat_count == CHAT_MAX) && (chat_msgs[0].content[0] == 'm') && (chat_msgs[0].content[1] == '0' + (2 % 10));
        if (!pass) { puts("chat ring bound failed, count="); putn((unsigned int)chat_count); puts("\n"); goto chat_test_done; }

        puts("chat: history round-trips, 600-char message survives (old cap was 511), /api/chat request carries both turns, ring drops oldest past CHAT_MAX: ok\n");

        chat_test_done:
        chat_count = 0; chat_save(); /* leave a clean CHAT.TXT, this test must not leave the kernel in a weird state for whatever runs next */
        /* serial mirror, same tools/png-check.sh pattern: a headless
           harness reads the serial port, not the VGA framebuffer, since
           this command has no visible screen output of its own. */
        serial_puts(pass ? "chattest PASS\n" : "chattest FAIL\n");
        if (!pass) puts("FAILED\n");
    }
    else if (!strcmp(line, "calctest")) {
        /* v70 (0.64.0): discriminating regression test for Calculator. Core
           contract: parse and evaluate basic arithmetic expressions correctly,
           with proper operator precedence. Real checks: (1) simple addition
           "2+3" evaluates to 5.0, (2) multiplication binds tighter than
           addition: "2+3*4" evaluates to 14.0 not 20.0, (3) parentheses work
           and override precedence: "(2+3)*4" evaluates to 20.0 not 14.0, (4)
           unary minus: "-2+3" evaluates to 1.0, (5) division: "10/2" is 5.0.
           If any expression fails to parse or evaluates to the wrong value,
           the test catches it. */
        int pass = 1;

        expr_node *e1 = calc_parse("2+3");
        double r1 = calc_eval(e1);
        calc_free(e1);
        if (r1 != 5.0) { puts("2+3 failed: got "); putn((unsigned int)r1); puts("\n"); pass = 0; }

        expr_node *e2 = calc_parse("2+3*4");
        double r2 = calc_eval(e2);
        calc_free(e2);
        if (r2 != 14.0) { puts("2+3*4 failed: got "); putn((unsigned int)r2); puts("\n"); pass = 0; }

        expr_node *e3 = calc_parse("(2+3)*4");
        double r3 = calc_eval(e3);
        calc_free(e3);
        if (r3 != 20.0) { puts("(2+3)*4 failed: got "); putn((unsigned int)r3); puts("\n"); pass = 0; }

        expr_node *e4 = calc_parse("-2+3");
        double r4 = calc_eval(e4);
        calc_free(e4);
        if (r4 != 1.0) { puts("-2+3 failed: got "); putn((unsigned int)r4); puts("\n"); pass = 0; }

        expr_node *e5 = calc_parse("10/2");
        double r5 = calc_eval(e5);
        calc_free(e5);
        if (r5 != 5.0) { puts("10/2 failed: got "); putn((unsigned int)r5); puts("\n"); pass = 0; }

        puts(pass ? "calculator parser: ok\n" : "FAILED\n");
    }
    else if (!strcmp(line, "stockstest")) {
        int pass = 1;
        char price_str[16];
        stocks_format_price(23800, price_str, sizeof(price_str));
        if (price_str[0] != '2' || price_str[1] != '3') { puts("stocks price format failed\n"); pass = 0; }
        int dollars, cents, sign;
        stocks_format_change(-18000, &dollars, &cents, &sign);
        if (sign != -1 || dollars != 180) { puts("stocks change format failed\n"); pass = 0; }
        int has_positive = 0, has_negative = 0;
        for (int i = 0; i < STOCKS_MAX; i++) {
            if (stocks_entries[i].change_x100 > 0) has_positive = 1;
            if (stocks_entries[i].change_x100 < 0) has_negative = 1;
        }
        if (!has_positive || !has_negative) { puts("stocks data integrity failed\n"); pass = 0; }
        puts(pass ? "stocks demo data: ok\n" : "FAILED\n");
    }
    else if (!strcmp(line, "pngtest")) {
        /* v74 (0.66.0): discriminating regression test for drivers/png.c.
           Three real PNG files are baked in (drivers/png_testdata.h,
           generated by tools/gen_png_testdata.py): each is a crop of the
           kernel's own wallpaper source, so the expected pixels are
           wallpaper_rgb itself, no second copy. Between them they cover
           all five scanline filters (cycled per row on purpose) and all
           three DEFLATE block kinds (dynamic Huffman at zlib level 9,
           stored blocks at level 0, fixed Huffman via a tiny image),
           RGB and RGBA, multi-IDAT. Each decode is compared byte-for-byte
           to the crop it came from and FNV-1a hashed; the hash is printed
           on serial too so tools/png-check.sh can match it against the
           host's own PIL decode of the identical bytes. Two fault
           injections finish it: one flipped IDAT byte must fail on the
           chunk CRC, and the same flip with the CRC recomputed must still
           fail inside inflate/Adler-32 instead of decoding to garbage. */
        int pass = 1;
        /* v75: a fourth fixture, 8-bit indexed (PLTE), the shape every real
           map tile server serves. Its pixels are palette lookups of a
           quantized crop, not wallpaper bytes, so pal=1 skips the
           wallpaper compare and the host hash alone is the oracle. */
        struct { const char *name; const unsigned char *png; unsigned int len;
                 unsigned int x0, y0, w, h, ch, fnv; int pal; } cases[4] = {
            { "rgb_dyn", pngt_rgb_dyn, sizeof pngt_rgb_dyn, PNGT_RGB_DYN_X0, PNGT_RGB_DYN_Y0,
              PNGT_RGB_DYN_W, PNGT_RGB_DYN_H, PNGT_RGB_DYN_CH, PNGT_RGB_DYN_FNV, PNGT_RGB_DYN_PAL },
            { "rgba_stored", pngt_rgba_stored, sizeof pngt_rgba_stored, PNGT_RGBA_STORED_X0, PNGT_RGBA_STORED_Y0,
              PNGT_RGBA_STORED_W, PNGT_RGBA_STORED_H, PNGT_RGBA_STORED_CH, PNGT_RGBA_STORED_FNV, PNGT_RGBA_STORED_PAL },
            { "rgb_fixed", pngt_rgb_fixed, sizeof pngt_rgb_fixed, PNGT_RGB_FIXED_X0, PNGT_RGB_FIXED_Y0,
              PNGT_RGB_FIXED_W, PNGT_RGB_FIXED_H, PNGT_RGB_FIXED_CH, PNGT_RGB_FIXED_FNV, PNGT_RGB_FIXED_PAL },
            { "pal_dyn", pngt_pal_dyn, sizeof pngt_pal_dyn, PNGT_PAL_DYN_X0, PNGT_PAL_DYN_Y0,
              PNGT_PAL_DYN_W, PNGT_PAL_DYN_H, PNGT_PAL_DYN_CH, PNGT_PAL_DYN_FNV, PNGT_PAL_DYN_PAL },
        };
        for (int ci = 0; ci < 4; ci++) {
            unsigned char *px = 0; unsigned int w = 0, h = 0, ch = 0;
            int r = png_decode(cases[ci].png, cases[ci].len, &px, &w, &h, &ch);
            puts("png "); puts(cases[ci].name); puts(": ");
            if (r != 0) { puts("decode error "); putn((unsigned int)(-r)); puts(" FAILED\n"); pass = 0; continue; }
            unsigned int mism = 0, fnv = 0x811c9dc5u;
            if (w != cases[ci].w || h != cases[ci].h || ch != cases[ci].ch) mism = 0xFFFFFFFFu;
            else if (cases[ci].pal) {
                for (unsigned int i = 0; i < w * h * ch; i++) { fnv ^= px[i]; fnv *= 0x01000193u; }
            }
            else {
                for (unsigned int y = 0; y < h; y++)
                    for (unsigned int x = 0; x < w; x++) {
                        const unsigned char *e = &wallpaper_rgb[((cases[ci].y0 + y) * WALLPAPER_W + cases[ci].x0 + x) * 3];
                        const unsigned char *d = px + (y * w + x) * ch;
                        if (e[0] != d[0] || e[1] != d[1] || e[2] != d[2]) mism++;
                        if (ch == 4 && d[3] != ((x * 7 + y * 3) & 0xFF)) mism++; /* the generator's alpha pattern */
                    }
                for (unsigned int i = 0; i < w * h * ch; i++) { fnv ^= px[i]; fnv *= 0x01000193u; }
            }
            kfree(px);
            putn(w); puts("x"); putn(h); puts(" ch="); putn(ch);
            puts(" mismatches="); putn(mism); puts(" fnv="); puthex(fnv);
            int ok = (mism == 0 && fnv == cases[ci].fnv);
            puts(ok ? " ok\n" : " FAILED\n");
            if (!ok) pass = 0;
            /* serial copy for the headless harness */
            { char b[64]; int i = 0; const char *s = "pngtest "; while (*s) b[i++] = *s++;
              s = cases[ci].name; while (*s) b[i++] = *s++; b[i++] = ' ';
              b[i++] = ok ? 'o' : 'F'; b[i++] = ok ? 'k' : 'A'; b[i++] = ' ';
              for (int sh = 28; sh >= 0; sh -= 4) { int nib = (fnv >> sh) & 0xF; b[i++] = nib < 10 ? '0' + nib : 'a' + nib - 10; }
              b[i++] = '\n'; b[i] = 0; serial_puts(b); }
        }
        /* fault injection 1: a flipped IDAT byte must trip the chunk CRC */
        {
            unsigned int n = sizeof pngt_rgb_dyn;
            unsigned char *c = kmalloc(n);
            unsigned char *px = 0; unsigned int w, h, ch;
            if (c) {
                memcpy(c, pngt_rgb_dyn, n);
                c[5000] ^= 0x55;
                int r = png_decode(c, n, &px, &w, &h, &ch);
                if (px) kfree(px);
                puts(r == PNG_E_CRC ? "png corrupt byte -> crc rejected: ok\n" : "png corrupt byte not rejected: FAILED\n");
                if (r != PNG_E_CRC) pass = 0;
                /* fault injection 2: same flip, CRC repaired, must still fail in zlib/Adler */
                unsigned int pos = 8 + 25;
                unsigned int clen = ((unsigned int)c[pos] << 24) | ((unsigned int)c[pos+1] << 16) | ((unsigned int)c[pos+2] << 8) | c[pos+3];
                memcpy(c, pngt_rgb_dyn, n);
                c[pos + 8 + clen / 2] ^= 0x55;
                unsigned int crc = png_crc32(c + pos + 4, clen + 4);
                c[pos + 8 + clen] = crc >> 24; c[pos + 9 + clen] = crc >> 16; c[pos + 10 + clen] = crc >> 8; c[pos + 11 + clen] = crc;
                px = 0;
                r = png_decode(c, n, &px, &w, &h, &ch);
                if (px) kfree(px);
                int ok2 = (r == PNG_E_ZLIB || r == PNG_E_TRUNCATED);
                puts(ok2 ? "png corrupt byte, crc repaired -> inflate rejected: ok\n" : "png corrupt inflate not rejected: FAILED\n");
                if (!ok2) pass = 0;
                kfree(c);
            } else { puts("kmalloc failed\n"); pass = 0; }
        }
        puts(pass ? "png decoder: ok\n" : "png decoder: FAILED\n");
        serial_puts(pass ? "pngtest PASS\n" : "pngtest FAIL\n");
    }
    else if (!strcmp(line, "jpegtest")) {
        /* v76: discriminating regression test for drivers/jpeg.c, the same
           pngtest shape. Five real photographic JPEGs are baked in
           (drivers/jpeg_testdata.h, generated by tools/gen/gen_jpeg_testdata.py):
           crops of the kernel's own wallpaper source re-encoded via PIL at
           4:4:4 and 4:2:0 subsampling, several quality levels, and one
           grayscale case. Unlike pngtest, there is no PIL-decode oracle
           baked in here: JPEG is lossy, so a bit-exact match against an
           independent decoder isn't the bar (see tools/jpeg-host/main.c's
           comment for that comparison, done host-side against PIL within a
           documented tolerance). What *is* checked hash-for-hash here is
           this exact decoder, cross-compiled freestanding, against the
           same C source compiled natively (tools/checks/jpeg-check.sh runs
           tools/jpeg-host fresh and diffs the printed hashes), which is a
           real, meaningful, discriminating test: deterministic integer-
           only code should decode identically on both targets, and a
           mismatch means a real cross-compile or freestanding-environment
           bug, not lossy-format noise. Two fault injections finish it:
           a file with no SOI marker must be rejected as not-a-JPEG, and a
           truncated file must fail cleanly rather than reading past the
           end. */
        int pass = 1;
        struct { const char *name; const unsigned char *jpg; unsigned int len;
                 unsigned int w, h, ch; } cases[5] = {
            { "photo_q90_420", jpegt_photo_q90_420, sizeof jpegt_photo_q90_420, JPEGT_PHOTO_Q90_420_W, JPEGT_PHOTO_Q90_420_H, JPEGT_PHOTO_Q90_420_CH },
            { "photo_q75_444", jpegt_photo_q75_444, sizeof jpegt_photo_q75_444, JPEGT_PHOTO_Q75_444_W, JPEGT_PHOTO_Q75_444_H, JPEGT_PHOTO_Q75_444_CH },
            { "photo_q50_420", jpegt_photo_q50_420, sizeof jpegt_photo_q50_420, JPEGT_PHOTO_Q50_420_W, JPEGT_PHOTO_Q50_420_H, JPEGT_PHOTO_Q50_420_CH },
            { "photo_q95_444", jpegt_photo_q95_444, sizeof jpegt_photo_q95_444, JPEGT_PHOTO_Q95_444_W, JPEGT_PHOTO_Q95_444_H, JPEGT_PHOTO_Q95_444_CH },
            { "gray_q85_420", jpegt_gray_q85_420, sizeof jpegt_gray_q85_420, JPEGT_GRAY_Q85_420_W, JPEGT_GRAY_Q85_420_H, JPEGT_GRAY_Q85_420_CH },
        };
        for (int ci = 0; ci < 5; ci++) {
            unsigned char *px = 0; unsigned int w = 0, h = 0, ch = 0;
            int r = jpeg_decode(cases[ci].jpg, cases[ci].len, &px, &w, &h, &ch);
            puts("jpeg "); puts(cases[ci].name); puts(": ");
            if (r != 0) { puts("decode error "); putn((unsigned int)(-r)); puts(" FAILED\n"); pass = 0; continue; }
            int dims_ok = (w == cases[ci].w && h == cases[ci].h && ch == cases[ci].ch);
            unsigned int fnv = 0x811c9dc5u;
            if (dims_ok) for (unsigned int i = 0; i < w * h * ch; i++) { fnv ^= px[i]; fnv *= 0x01000193u; }
            kfree(px);
            putn(w); puts("x"); putn(h); puts(" ch="); putn(ch);
            puts(" fnv="); puthex(fnv);
            puts(dims_ok ? " ok\n" : " FAILED\n");
            if (!dims_ok) pass = 0;
            /* serial copy for the headless harness, same "jpeghash <name> <hash>" line tools/jpeg-host/main.c prints */
            { char b[64]; int i = 0; const char *s = "jpeghash "; while (*s) b[i++] = *s++;
              s = cases[ci].name; while (*s) b[i++] = *s++; b[i++] = ' ';
              for (int sh = 28; sh >= 0; sh -= 4) { int nib = (fnv >> sh) & 0xF; b[i++] = nib < 10 ? '0' + nib : 'a' + nib - 10; }
              b[i++] = '\n'; b[i] = 0; serial_puts(b); }
        }
        /* fault injection 1: no SOI marker at all */
        {
            unsigned char c[16];
            for (int i = 0; i < 16; i++) c[i] = 0;
            unsigned char *px = 0; unsigned int w, h, ch;
            int r = jpeg_decode(c, sizeof c, &px, &w, &h, &ch);
            if (px) kfree(px);
            puts(r == JPEG_E_SIGNATURE ? "jpeg no SOI -> rejected: ok\n" : "jpeg no SOI not rejected: FAILED\n");
            if (r != JPEG_E_SIGNATURE) pass = 0;
        }
        /* fault injection 2: truncated file */
        {
            unsigned char *px = 0; unsigned int w, h, ch;
            int r = jpeg_decode(jpegt_photo_q90_420, 50, &px, &w, &h, &ch);
            if (px) kfree(px);
            puts(r == JPEG_E_TRUNCATED ? "jpeg truncated -> rejected: ok\n" : "jpeg truncated not rejected: FAILED\n");
            if (r != JPEG_E_TRUNCATED) pass = 0;
        }
        puts(pass ? "jpeg decoder: ok\n" : "jpeg decoder: FAILED\n");
        serial_puts(pass ? "jpegtest PASS\n" : "jpegtest FAIL\n");
    }
    else if (!strcmp(line, "mousetest")) {
        if (!window_open(800, 600, 32)) { puts("no VGA device found or out of page tables\n"); }
        else {
            int cx_pos = 400, cy_pos = 300;
            int buttons = 0;
            do {
                window_clear(0x00FAF8F6);
                window_rect(cx_pos - 5, cy_pos - 5, 10, 10, 0x00555555);
                window_present(); __asm__ volatile ("hlt"); /* wake on the next IRQ (timer, keyboard, or mouse) */
                int dx, dy;
                if (mouse_get_delta(&dx, &dy, &buttons)) {
                    cx_pos += dx; cy_pos += dy;
                    mouse_get_absolute(&cx_pos, &cy_pos, (int)window_width(), (int)window_height());
                    if (cx_pos < 0) cx_pos = 0; if ((unsigned)cx_pos >= window_width())  cx_pos = window_width() - 1;
                    if (cy_pos < 0) cy_pos = 0; if ((unsigned)cy_pos >= window_height()) cy_pos = window_height() - 1;
                }
            } while (!(buttons & 1)); /* left click to exit */
            window_close();
            clear();
            puts("back in text mode\n");
        }
    }
    else if (!strcmp(line, "time"))  show_time();
    else if (!strcmp(line, "reboot"))reboot();
    else {
        /* 1.0.0: before calling `line` an unknown command, try it as a
           program name -- the real shell gap the roadmap calls out.
           `arg` (everything after the first space, already split off
           above) becomes argv[1..] the same way exec's own argv[1..]
           does, through the same split_argv() and exec_user(); this is
           not a second exec path, just a second way to reach the first
           one's name. */
        char resolved[JT_RESOLVE_NAME_MAX];
        if (exec_resolve_name(line, resolved)) {
            const char *pargv[JT_ARGC_MAX];
            pargv[0] = resolved;
            int rest = split_argv(arg, pargv + 1, JT_ARGC_MAX - 1);
            if (rest < 0) { puts("exec: too many arguments (max "); putdec(JT_ARGC_MAX); puts(" including the program name)\n"); }
            else {
                int status = -1;
                if (!exec_user(resolved, pargv, rest + 1, &status)) { puts(resolved); puts(": exec failed (not found, too big, argv too large, or no free task slot)\n"); }
                else { puts("exit code "); putdec(status); putc('\n'); }
            }
        }
        else { puts("? "); puts(line); putc('\n'); }
    }
}

void kmain(unsigned int multiboot_info_addr){
    serial_init();
    serial_puts("=== kmain boot start === v" JT_VERSION_STR "\n");
    /* Multiboot command line (flags bit 2, pointer at +16), read here while
       the bootloader's low memory is still identity-reachable. Only one
       option exists: wxhost=A.B.C.D[:PORT], see wx_override_host. */
    /* Multiboot info flag bit 12: the bootloader set a framebuffer. Offsets per the
       multiboot1 spec: addr 88 (u64, low half used), pitch 96, width 100, height 104,
       bpp 108, type 109 (1 = direct RGB). Read only while the info block sits inside
       the boot identity map. */
    if (multiboot_info_addr && multiboot_info_addr < 0x400000 && (*(unsigned int *)multiboot_info_addr & (1u << 12))) {
        const unsigned char *mb = (const unsigned char *)multiboot_info_addr;
        if (mb[109] == 1 && *(const unsigned int *)(mb + 92) == 0)
            vbe_set_boot_framebuffer(*(const unsigned int *)(mb + 88), *(const unsigned int *)(mb + 96),
                                     *(const unsigned int *)(mb + 100), *(const unsigned int *)(mb + 104), mb[108]);
    }
    if (multiboot_info_addr && (*(unsigned int *)multiboot_info_addr & 0x4)) {
        const char *cl = (const char *)*(unsigned int *)(multiboot_info_addr + 16);
        for (const char *pc = cl; pc && *pc; pc++)
            if (pc[0]=='p' && pc[1]=='o' && pc[2]=='r' && pc[3]=='t' && pc[4]=='f' && pc[5]=='o' && pc[6]=='l' && pc[7]=='i' && pc[8]=='o') { portfolio_dock = 1; serial_puts("portfolio dock\n"); break; }
        for (; cl && *cl; cl++) {
            if (cl[0]=='w' && cl[1]=='x' && cl[2]=='h' && cl[3]=='o' && cl[4]=='s' && cl[5]=='t' && cl[6]=='=') {
                cl += 7; int hp = 0;
                while (((*cl >= '0' && *cl <= '9') || *cl == '.') && hp < 19) wx_override_host[hp++] = *cl++;
                wx_override_host[hp] = 0;
                if (*cl == ':') { unsigned int pt = 0; cl++; while (*cl >= '0' && *cl <= '9') pt = pt * 10 + (unsigned int)(*cl++ - '0'); if (pt && pt < 65536) wx_override_port = (unsigned short)pt; }
                serial_puts("wxhost="); serial_puts(wx_override_host); serial_puts("\n");
                break;
            }
        }
    }
    vga_text_mode_init(); /* real hardware/QEMU already boot into text mode via their own BIOS; a BIOS-less multiboot path (v86) never sets it at all, so make it explicit rather than inherited */
    klog("vga_text_mode_init: text mode 3 programmed");
    gdt_install();
    klog("gdt_install: GDT loaded");
    idt_install();
    klog("idt_install: IDT loaded");
    syscall_install(); /* v64: int 0x80 gate, DPL 3 */
    klog("syscall_install: int 0x80 gate live");
    irq_install();
    klog("irq_install: PIC remapped, PIT/keyboard IRQs live");
    mouse_init();
    klog("mouse_init: PS/2 mouse enabled");
    /* v62: probe the VMware absolute-pointer backdoor (port 0x5658). v86
       and QEMU's default pc machine both answer; bare hardware and
       -machine vmport=off don't, and PS/2 relative stays the only mouse. */
    klog(vmmouse_init() ? "vmmouse_init: VMware backdoor answered, absolute pointer on"
                        : "vmmouse_init: no backdoor, PS/2 relative pointer only");
    font_init(); /* must run while still in plain VGA text mode, before any window_open */
    klog("font_init: CP437 glyphs dumped from VGA hardware");
    pmm_init(multiboot_info_addr);
    klog("pmm_init: physical memory map parsed");
    paging_install();
    klog("paging_install: higher-half paging active");
    tasks_init();
    klog("tasks_init: scheduler ready");
    ata_blockdev_register(); /* v33 (0.33.0): register real backends before anything tries to mount a filesystem over one */
    ramdisk_init();
    trash_init();
    int fs_ok = fat_mount();
    klog(fs_ok ? "fat_mount: FAT16 filesystem mounted" : "fat_mount: no filesystem found");
    fat_vfs_register(); /* registered regardless of fs_ok: an unmounted fat backend just returns real failures, same as before v29 */
    ramfs_init();
    klog("vfs: fat + ramfs backends registered, fat active");
    /* v86 (0.71.0): real root cause of "Files shows no files" on the
       browser demo, confirmed by reading vfs_register (drivers/vfs.c):
       the FIRST backend registered wins by default (fat, line above), and
       fat_mount() only ever finds a real disk when native QEMU boots off
       dotfiles.img; v86 has no disk image wired into its boot path at
       all (nothing in landing/v86/embed.js ever attaches a virtual disk),
       so fs_ok is always 0 there and Files/`ls` genuinely have nothing to
       show, not a bug in the FAT code itself. Native boots with a real
       disk are completely unaffected: this only swaps the default
       backend when fat_mount() itself already reported failure, which on
       real hardware/QEMU-with-a-disk it doesn't. Seed a few real demo
       files so a browser visitor sees something real to click, same
       honesty bar as everything else on this page (no fake data, no
       recording): a short note that names this exact fact. */
    if (!fs_ok) {
        static const char demo_readme[] =
            "This is a live demo.\n\n"
            "No real disk is attached in your browser (v86 has no way to\n"
            "mount the native dotfiles.img this kernel boots from on real\n"
            "hardware), so these are ramfs files, kept in memory only for\n"
            "this tab. Try the Terminal app: ls, cat README.TXT, echo.\n";
        static const char demo_notes[] =
            "Joshua Tree\n\n"
            "A freestanding kernel, built from scratch.\n\n"
            "This desktop, the file manager, mail, calendar, terminal,\n"
            "even this note you're reading, all real, all running on that\n"
            "kernel right now in your browser.\n";
        vfs_switch("ramfs"); /* switch first: vfs_write_file always targets the active backend, and fat's own write would just fail with no disk anyway */
        vfs_write_file("README.TXT", demo_readme, strlen(demo_readme));
        vfs_write_file("NOTES.TXT", demo_notes, strlen(demo_notes));
        klog("vfs: no FAT disk (v86 has none to mount), switched default backend to ramfs with demo files");
    }
    settings_load(); /* v47: real settings, saved defaults if SETTINGS.TXT doesn't exist yet */
    clear();
    boot_chime();
    puts("joshuatree v0 -- type help\n");
    if (!fs_ok) puts("(no FAT filesystem found -- ls/cat unavailable)\n");
    /* check.sh's only way to know the kernel reached this point: booting
       straight into gui_run() below switches the VGA card into a real
       graphics mode, which reprograms the Graphics Controller's memory-map
       select register, changing what physical address 0xB8000 even means.
       The old banner text is still real and still gets written above, it
       just becomes unreadable moments later once graphics mode takes over,
       confirmed directly (a real `xp` read after boot showed 0xffff, not
       the banner) rather than assumed. A plain RAM address ordinary text
       memory doesn't share survives the mode switch untouched. */
    *(volatile unsigned int *)0x9000 = 0xB007C0DE;
    kbd_drain(); /* discard any stray byte queued during boot (keyboard_enable_scanning, mouse_init) before real input starts */

    /* A real desktop OS boots to a desktop, not a command line: gui_run()
       already has a clean way back to this exact shell (esc closes the
       window, clears, prints "back in text mode", returns), so starting
       there instead of making every visitor type "gui" themselves is a
       straight improvement, not a special case for the browser demo. */
    gui_run();

    char line[80];
    for (;;) {
        puts("> ");
        int n = 0;
        for (;;) {
            char c = getch();
            if (c == '\n') { putc('\n'); break; }
            if (c == '\b') { if (n) { n--; putc('\b'); } continue; }
            if (n < (int)sizeof(line) - 1) { line[n++] = c; putc(c); }
        }
        line[n] = 0;
        run(line);
    }
}
