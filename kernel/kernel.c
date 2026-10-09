/* Freestanding i386 kernel: VGA text, PS/2 keyboard, RTC clock, tiny shell. */
#include "gdt.h"
#include "idt.h"
#include "symtab.h"
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
int ring3app_write_packed(const char *file, const unsigned char *packed, unsigned int clen, unsigned int len, unsigned int sum, int replace); /* ring3app.c: unpack + verify + vfs_write_file */
#include "libc.h"
#include "pci.h"
#include "vbe.h"
#include "mouse.h"
#include "vmmouse.h"
#include "window.h"
#include "ttf.h"
#include "font.h"
#include "rtl8139.h"
#include "net.h"
#include "gui_prims.h"
#include "dock_geom.h"
#include "dock_draw.h"
#include "gui_paint.h"
#include "http.h"
#include "wallpaper.h"
#include "icon_art.h"
#include "boot_mark.h"
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
#include "sb16.h"
#include "speak.h"
#include "app_weather.h"
#include "app_curbfind.h"
#include "app_keyrate.h"
#include "app.h"
#include "app_bookrank.h"
#include "app_quotestreak.h"
#include "app_tonchi.h"
#include "app_toroid.h"
#include "app_hikko.h"
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

void putc(char c){
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
char kbd_map(int sc){
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

void gui_app_mouse_tick(void);
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
int gui_close_was_click = 0;
int gui_getch_or_click(void){
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

/* KEY_UP..KEY_WHEEL_DOWN (arrows, enter, esc, click, wheel) now live in
   app.h: a moved-out app's own unit needs the exact values get_key_or_click
   hands back, same as this file. */
/* v1.0.6: one system-wide clipboard. Every text field that reads through
   get_key/get_key_or_click gets Ctrl+C/X/V for free instead of each app
   decoding scancodes itself: kbd_ctrl (irq.c) plus the plain character scan
   codes for C/X/V (0x2E/0x2D/0x2F) turn into these three synthetic keys.
   editor.h reads raw scancodes below get_key, not through it, so it tests
   kbd_ctrl and the same three scancodes directly. One 4KB buffer plus its
   length is the whole clipboard; every consumer copies at most
   CLIPBOARD_CAP bytes in and truncates a paste at its own field's max
   length, so nothing here can overflow a caller's buffer. */
/* v1.6.23: push-to-talk for Chat. F2's make code (0x3C) is a real,
   unassigned scancode kbd_map never turns into a character (F-keys have
   no entry in SC[]/SCS[]), so it reaches here through get_key_or_click_
   until's ordinary "not a printable key" path and is otherwise silently
   dropped -- same slot every other synthetic key above claims. Chat holds
   F2 to record (user/samantha.c's push-to-talk) and watches for the
   matching break code (0xBC) between DMA chunks to notice release. */
#define KEY_PTT 305
int get_key_or_click(void);

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
        if (kbd_ctrl) {
            int code = sc & 0x7F;
            if (code == 0x2E) return KEY_COPY;
            if (code == 0x2D) return KEY_CUT;
            if (code == 0x2F) return KEY_PASTE;
        }
        char c = kbd_map(sc);
        if (c == '\n') return KEY_ENTER;
        if (c == 27)   return KEY_ESC;
        if (c) return c;
    }
}

int get_key_or_click_until(unsigned int deadline){
    for (;;) {
        if (deadline && (int)(ticks() - deadline) >= 0) return 0;
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
                gui_close_was_click = 0;
                if (sc == 0x3C) return KEY_PTT; /* F2 make code: push-to-talk */
                if (kbd_ctrl) {
                    int code = sc & 0x7F;
                    if (code == 0x2E) return KEY_COPY;
                    if (code == 0x2D) return KEY_CUT;
                    if (code == 0x2F) return KEY_PASTE;
                }
                char c = kbd_map(sc);
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

int get_key_or_click(void) { return get_key_or_click_until(0); }

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

/* 2.2.0: fputest. Two tasks each push a value on the x87 stack, switch away inside the same asm, pop it back; crossed values mean schedule() lost per-task float state. */
static volatile int fpu_bad, fpu_done;
static void fpu_task(double mark) { for (int i = 0; i < 300; i++) { double in = mark + i, out = 0; __asm__ volatile ("fldl %1; int $32; fstpl %0" : "=m"(out) : "m"(in) : "memory"); if (out != in) fpu_bad++; } fpu_done++; task_exit(); }
static void fpu_a(void){ fpu_task(1000.5); } static void fpu_b(void){ fpu_task(7000.25); }

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

/* ---- v0.88.0: spawntest. The Activity app (user/activity.c) shows and
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
#define GUI_APP_COUNT   33 /* 31 real apps + the Apps folder + Trash; 2.2 Music (24) and Movies (25) pushed Apps/Trash to 26/27, 2.7 Hamurapi (26) to 27/28, 2.8 Windgate (27) to 28/29, 2.11 Panes (28) to 29/30, 2.14 Claude (29) to 30/31, Mines (30) to 31/32 */
#define GUI_APP_PANES   28 /* the one app that gets Ctrl chords as KEY_CTL_* (kernel/app.h) */
#define GUI_APP_PORTFOLIO 21 /* hidden from the Apps folder and phone home unless the boot line says "portfolio" (his site embed); the public OS ships without it */
/* Every app's name, color, glyph and hooks live in one table, APPS[],
   defined further down once every hook it points at exists (see "The app
   registry" below). This tentative definition lets the dock and Launchpad
   code above that point read it. */
const struct app APPS[GUI_APP_COUNT];

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
_Static_assert(GUI_ICON_ART_SIZE == ICON_ART_SIZE, "gui_paint.h and icon_art.h disagree on the artwork size");
static const int GUI_DOCK_DEFAULT[GUI_ICON_COUNT] = GUI_DOCK_DEFAULT_ORDER; /* gui_paint.h: the ARM dock draws the same order */

/* gui_order is a permutation of icon indices by dock slot: dragging an icon
   and dropping it on another slot swaps the two, so the arrangement is
   real and sticks for the rest of this GUI session (reset to launch order
   next time `gui` runs; nothing about layout is saved to disk, matching
   this whole desktop's one-screen, nothing-persisted scope). */
int gui_order[GUI_ICON_COUNT];
/* Portfolio mode ("portfolio" on the multiboot command line, sent by the
   landing's embed.js when heyitsmejosh.com/os.html frames it): the dock is
   Joshua's own apps instead of the system set. Same slot count, Apps folder
   and Trash stay at the ends; everything left out is still in the Apps folder. */
static int portfolio_dock;
static int gui_apps_n(void){ return portfolio_dock ? GUI_APPS_FOLDER : GUI_APPS_FOLDER - 1; } /* apps the folder and phone home list */
static int gui_app_at(int k){ return (!portfolio_dock && k >= GUI_APP_PORTFOLIO) ? k + 1 : k; } /* grid position to APPS[] index */
int jt_portfolio_mode(void){ return portfolio_dock; } /* ring3app.c: windowed ring-3 apps get "portfolio" in argv so Samantha wears Joshua's face */
/* "samantha" on the multiboot command line: skip the desktop and open
   ring-3 Samantha's window (user/samantha.c)
   the instant gui_run's first frame would otherwise draw the dock. One
   splash frame still shows (gui_draw_boot_screen runs first, unconditionally);
   this only replaces the icon desktop that would follow it. */
static int boot_to_samantha;
/* "phone" (430x760 portrait, kmain's parse) and "res=WxH" (physical px from embed.js at dpr 1, gui_parse_res; opens W/2 x H/2 at scale 2) pick gui_run's mode. */
static int boot_to_phone, boot_res_w, boot_res_h;
int jt_phone_mode(void){ return boot_to_phone; } /* ring3app.c: windowed ring-3 apps get argv[1]="phone" so they can show libjt/osk */
#include "hint.h"
static void phone_app_titlebar_draw(const char *title); static void phone_back_zone_tick(int buttons, int app_drag_held, int cursor_x, int cursor_y); /* both defined in kernel/phone_home.h, included near gui_run; forward-declared so gui_draw_app_titlebar/gui_app_mouse_tick (both defined above it) can call them */
static const int GUI_DOCK_PORTFOLIO[GUI_ICON_COUNT] = {GUI_APPS_FOLDER, 21, 20, 8, 10, 12, 14, 11, 9, 13, GUI_TRASH}; /* Portfolio, Epiphany, Curbfind, Bookrank, Tonchi, Hikko, Quotes, Keyrate, Toroid */
static void gui_order_init(void){ for (int i = 0; i < GUI_ICON_COUNT; i++) gui_order[i] = portfolio_dock ? GUI_DOCK_PORTFOLIO[i] : GUI_DOCK_DEFAULT[i]; }
int dock_hover = -1; /* slot whose label is showing */

/* GUI_BG lives in app.h now: a moved-out app's own unit clears to it too. */
#define GUI_MENUBAR_H   26
/* v36 (0.36.0): the icon size is now *derived* from how many icons there
   are, instead of a constant that silently overflows the screen every
   time an app is added. v35 hit that for real (14 icons at the old
   56px sizing came to 1016px on an 800px screen, two icons genuinely cut
   off), and adding Terminal would have hit it again at 792px, 8px from
   the edge. Solving it once, in arithmetic, beats rediscovering it in a
   screendump on every future app. DOCK_BUDGET is the widest the dock may
   ever draw, leaving a real margin on both sides of the 800px screen. */
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
int dock_scale_pct = 7; /* non-static: dock_geom.c's gui_dock_icon() reads it */
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

/* v71: real location for weather/map, looked up from the public IP
   (ip-api.com, see geo_fetch further down). Forward of that same v71
   declaration, same reason wind_enabled is forward here: settings_load
   needs it and has to precede it in the file.
   v0.85.5: loc_* is the Settings-entered override (see loc_geocode
   further down) that settings_load copies straight into these three
   fields plus geo_have, so weather_fetch_inner and the map wallpaper
   fetch pick it up through the exact same path as the IP lookup, no
   separate "which source" branch anywhere downstream. */
static char geo_lat[16] = "", geo_lon[16] = "", geo_city[24] = "";
static int geo_have = 0;
#define LOC_NAME_MAX 24
static char loc_name[LOC_NAME_MAX] = "", loc_lat[16] = "", loc_lon[16] = "";
static int loc_have = 0;
static char loc_err[48] = "";

/* v85 (chat rework): global LLM config, the same "one real setting, one
   real default, survives a reboot" contract wind/dock/wall already keep.
   Was hardcoded inline in the shell `chat` command and duplicated again
   in gui_launch_chat (two copies of "llama3.1:8b" / "10.0.2.2" / 11434
   that could silently drift apart); now one source of truth both read.
   1.0.12 (direct owner request, "hook Chat up to our Samantha LLM, the
   Turing project"): the compiled-in default is now the Turing project's
   own Cloudflare Worker (turing.heyitsmejosh.com, plain HTTP port 80,
   model "samantha"), reachable both natively (this file, straight over
   the internet, no VPN/gateway address involved) and from the v86 browser
   demo through worker.js's own `/api/proxy` exception for exactly this
   host+path. Settings (sel == 3/4 below) stays fully editable, same as
   before: anyone who'd rather run a local Ollama server on their own
   machine just points host/port/model back at it. */
#define LLM_MODEL_MAX 32
#define LLM_HOST_MAX 40
static char llm_model[LLM_MODEL_MAX] = "samantha";
static char llm_host[LLM_HOST_MAX] = "turing.heyitsmejosh.com";
static int llm_port = 80;
/* 1.9.26: read-only view for SYS_HTTP_POST (kernel/syscall.c). Settings still owns the write. */
const char *llm_host_get(void) { return llm_host; }
int llm_port_get(void) { return llm_port; }
/* 1.9.27: the Mail token Settings owns (SETTINGS.TXT mailtoken=). Only SYS_HTTP_POST reads it, to
   build the Authorization header for /api/mail/send; no syscall hands it to ring 3. */
#define MAIL_TOKEN_MAX 64
static char mail_token[MAIL_TOKEN_MAX] = "";
const char *mail_token_get(void) { return mail_token; }
/* 2.14.0: the Claude relay (tools/claude-relay/relay.py) Settings owns: SETTINGS.TXT claudehost=,
   claudeport=, claudetoken=. Its own host, never the LLM host: the token must only ever go to a
   relay the owner pointed at, not to turing.heyitsmejosh.com. Empty host or token means "not set
   up", and SYS_HTTP_POST refuses JT_POST_CLAUDE without touching the network. Only SYS_HTTP_POST
   reads the token, to build the bearer for /api/claude; no syscall hands it to ring 3. */
#define CLAUDE_HOST_MAX 40
#define CLAUDE_TOKEN_MAX 64
static char claude_host[CLAUDE_HOST_MAX] = "";
static int claude_port = 8765;
static char claude_token[CLAUDE_TOKEN_MAX] = "";
const char *claude_host_get(void) { return claude_host; }
int claude_port_get(void) { return claude_port; }
const char *claude_token_get(void) { return claude_token; }
/* v85: real chat models actually installed on the host (checked via
   `ollama list`), not a free-text field a typo can point at nothing.
   nomic-embed-text is also installed but is embedding-only, deliberately
   left off. Settings' LLM-model row cycles this list; a stale/hand-edited
   SETTINGS.TXT with anything else falls back to index 0 (samantha) the
   next time the cycle runs, since the cycle only ever writes one of
   these three strings back out.
   v0.85.4 (direct owner request): qwen3:8b promoted to the real default,
   llama3.1:8b kept as the second choice, checked against the same
   `ollama list` on the host, both actually installed.
   1.0.12 (direct owner request): "samantha" (Turing's own Ollama-
   compatible model name, confirmed against its own `/api/chat` reply's
   "model" field) added as the new default, index 0; qwen3:8b and
   llama3.1:8b kept as the local-Ollama alternatives for anyone who sets
   Settings' host back to their own machine. */
static const char *LLM_MODELS[] = { "samantha", "qwen3:8b", "llama3.1:8b" };
#define LLM_MODEL_COUNT 3

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
/* 2.14.0: exact key match for settings_load's newer keys, instead of another letter-by-letter chain. */
static int key_is(const char *k, int len, const char *want){
    int i = 0;
    while (i < len && want[i] && k[i] == want[i]) i++;
    return i == len && !want[i];
}
static void settings_load(void){
    static char buf[1024]; /* 2.14.0: was 512; the Claude relay's host and token would not fit beside the rest */
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
        int is_mailtoken = keylen == 9 && buf[start]=='m' && buf[start+1]=='a' && buf[start+2]=='i' && buf[start+3]=='l' && buf[start+4]=='t' && buf[start+5]=='o' && buf[start+6]=='k' && buf[start+7]=='e' && buf[start+8]=='n';
        int is_loc = keylen == 3 && buf[start]=='l' && buf[start+1]=='o' && buf[start+2]=='c';
        int is_claudehost = key_is(buf + start, keylen, "claudehost");
        int is_claudeport = key_is(buf + start, keylen, "claudeport");
        int is_claudetoken = key_is(buf + start, keylen, "claudetoken");
        if (is_claudehost || is_claudetoken) {
            char *dst = is_claudehost ? claude_host : claude_token;
            int cap = is_claudehost ? CLAUDE_HOST_MAX : CLAUDE_TOKEN_MAX;
            int j = 0, k = eq + 1;
            while (k < line_end && j < cap - 1 && buf[k] > ' ' && buf[k] < 0x7F) dst[j++] = buf[k++];
            dst[j] = 0;
            continue;
        }
        if (is_claudeport) {
            int v = 0, k = eq + 1;
            while (k < line_end && buf[k] >= '0' && buf[k] <= '9' && v < 100000) v = v * 10 + (buf[k++] - '0');
            if (v > 0 && v <= 65535) claude_port = v;
            continue;
        }
        if (is_loc) {
            /* value shape: name;lat;lon -- the same three fields
               loc_geocode fills in, ';'-joined since '=' is already the
               key/value separator and none of the three ever contain a
               ';' (json_extract_string strips escapes, json_extract_number_text
               is digits/./- only). A malformed line (missing a ';', an
               empty lat/lon) is treated as "no override" rather than
               guessed at. */
            int k = eq + 1;
            int f = 0; /* which field: 0=name 1=lat 2=lon */
            char nbuf[LOC_NAME_MAX]; int ni = 0;
            char latbuf[16]; int lai = 0;
            char lonbuf[16]; int loi = 0;
            while (k < line_end) {
                char c = buf[k++];
                if (c == ';') { f++; continue; }
                if (f == 0 && ni < LOC_NAME_MAX - 1) nbuf[ni++] = c;
                else if (f == 1 && lai < 15) latbuf[lai++] = c;
                else if (f == 2 && loi < 15) lonbuf[loi++] = c;
            }
            nbuf[ni] = 0; latbuf[lai] = 0; lonbuf[loi] = 0;
            if (f == 2 && lai > 0 && loi > 0) {
                int j = 0; while (nbuf[j]) { loc_name[j] = nbuf[j]; j++; } loc_name[j] = 0;
                j = 0; while (latbuf[j]) { loc_lat[j] = latbuf[j]; j++; } loc_lat[j] = 0;
                j = 0; while (lonbuf[j]) { loc_lon[j] = lonbuf[j]; j++; } loc_lon[j] = 0;
                loc_have = 1;
                j = 0; while (loc_lat[j]) { geo_lat[j] = loc_lat[j]; j++; } geo_lat[j] = 0;
                j = 0; while (loc_lon[j]) { geo_lon[j] = loc_lon[j]; j++; } geo_lon[j] = 0;
                j = 0; while (loc_name[j] && j < 23) { geo_city[j] = loc_name[j]; j++; } geo_city[j] = 0;
                geo_have = 1;
            }
            continue;
        }
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
        if (is_mailtoken) {
            int j = 0, k = eq + 1;
            while (k < line_end && j < MAIL_TOKEN_MAX - 1) mail_token[j++] = buf[k++];
            mail_token[j] = 0;
            continue;
        }
        if (is_llmhost) {
            int j = 0, k = eq + 1;
            while (k < line_end && j < LLM_HOST_MAX - 1) llm_host[j++] = buf[k++];
            llm_host[j] = 0;
            if (j == 0) { const char *d = "turing.heyitsmejosh.com"; int p=0; while (d[p]) llm_host[p]=d[p], p++; llm_host[p]=0; }
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
    static char buf[1024]; /* 2.14.0: was 512, see settings_load */
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
    if (mail_token[0]) {
        const char *k8 = "mailtoken="; while (*k8) buf[n++] = *k8++;
        { const char *s = mail_token; while (*s && n < (int)sizeof(buf) - 80) buf[n++] = *s++; }
        buf[n++] = '\n';
    }
    if (claude_host[0]) {
        const char *k9 = "claudehost="; while (*k9) buf[n++] = *k9++;
        { const char *s = claude_host; while (*s && n < (int)sizeof(buf) - 200) buf[n++] = *s++; }
        buf[n++] = '\n';
        const char *k10 = "claudeport="; while (*k10) buf[n++] = *k10++;
        { char digits[8]; int nd = 0; int v = claude_port;
          if (v == 0) digits[nd++] = '0';
          while (v) { digits[nd++] = (char)('0' + v % 10); v /= 10; }
          while (nd) buf[n++] = digits[--nd]; }
        buf[n++] = '\n';
    }
    if (claude_token[0]) {
        const char *k11 = "claudetoken="; while (*k11) buf[n++] = *k11++;
        { const char *s = claude_token; while (*s && n < (int)sizeof(buf) - 120) buf[n++] = *s++; }
        buf[n++] = '\n';
    }
    if (loc_have) {
        const char *k7 = "loc="; while (*k7) buf[n++] = *k7++;
        { const char *s = loc_name; while (*s && n < (int)sizeof(buf) - 34) buf[n++] = *s++; }
        buf[n++] = ';';
        { const char *s = loc_lat; while (*s && n < (int)sizeof(buf) - 18) buf[n++] = *s++; }
        buf[n++] = ';';
        { const char *s = loc_lon; while (*s && n < (int)sizeof(buf) - 2) buf[n++] = *s++; }
        buf[n++] = '\n';
    }
    vfs_replace_file(SETTINGS_FILE, buf, (unsigned int)n);
}

/* gui_dock_icon/gui_dock_w/gui_dock_x0/gui_dock_y0/gui_slot_x/gui_slot_at/
   gui_dock_hit_test: dock geometry and hit-testing, moved to
   dock_geom.c/.h. DOCK_ICON/DOCK_MARGIN_BOT/DOCK_TRAY_COLOR now live in
   dock_geom.h too. */

/* Paints a rect, then overwrites each corner's pixels outside a quarter
   circle of radius r with bg, faking a rounded rect with no alpha. */
/* gui_blend, gui_line_sqrt: moved to gui_prims.c/.h (pure color/math
   primitives, no state of their own). */

/* Antialiased line primitive: Wu-style coverage over a segment of given
   `width` (physical px, 1.5-2.0 reads best), drawn straight at PHYSICAL
   resolution via window_pixel_phys/window_get_pixel_phys -- the same
   physical path gui_aa_char uses -- so the coverage fringe blends against
   whatever is actually underneath instead of landing as flat scaled
   blocks. Coordinates are LOGICAL (same convention as stx_chart's other
   callers); scaled to physical internally by window_scale(), matching
   gui_fill_circle's no-offscreen-target physical path. For each physical
   pixel near the segment, coverage is the distance from the pixel center
   to the nearest point on the segment, falling off over a 1px band
   centered on the line's half-width -- exact endpoints included, so the
   line caps flat rather than growing fuzzy stubs past x0,y0/x1,y1. */
static void gui_aa_line_phys(double fx0, double fy0, double fx1, double fy1, unsigned int color, double width){
    double dx = fx1 - fx0, dy = fy1 - fy0;
    double len = gui_line_sqrt(dx * dx + dy * dy);
    double halfw = width / 2.0;
    int pad = (int)halfw + 2;
    if (len < 0.0001) {
        int minx = (int)fx0 - pad, maxx = (int)fx0 + pad;
        int miny = (int)fy0 - pad, maxy = (int)fy0 + pad;
        for (int y = miny; y <= maxy; y++) for (int x = minx; x <= maxx; x++) {
            double ddx = (x + 0.5) - fx0, ddy = (y + 0.5) - fy0;
            double dist = gui_line_sqrt(ddx * ddx + ddy * ddy);
            double cov = halfw + 0.5 - dist;
            if (cov <= 0) continue; if (cov > 1) cov = 1;
            int a = (int)(cov * 255 + 0.5);
            unsigned int d = window_get_pixel_phys(x, y);
            unsigned int r = (((color >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * (255 - a)) / 255;
            unsigned int g = (((color >> 8) & 0xFF) * a + ((d >> 8) & 0xFF) * (255 - a)) / 255;
            unsigned int b = ((color & 0xFF) * a + (d & 0xFF) * (255 - a)) / 255;
            window_pixel_phys(x, y, (r << 16) | (g << 8) | b);
        }
        return;
    }
    double ux = dx / len, uy = dy / len;
    int minx = (int)(fx0 < fx1 ? fx0 : fx1) - pad, maxx = (int)(fx0 > fx1 ? fx0 : fx1) + pad;
    int miny = (int)(fy0 < fy1 ? fy0 : fy1) - pad, maxy = (int)(fy0 > fy1 ? fy0 : fy1) + pad;
    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            double px = x + 0.5, py = y + 0.5;
            double t = (px - fx0) * ux + (py - fy0) * uy;
            if (t < 0) t = 0; if (t > len) t = len;
            double cx = fx0 + ux * t, cy = fy0 + uy * t;
            double ddx = px - cx, ddy = py - cy;
            double dist = gui_line_sqrt(ddx * ddx + ddy * ddy);
            double cov = halfw + 0.5 - dist;
            if (cov <= 0) continue; if (cov > 1) cov = 1;
            int a = (int)(cov * 255 + 0.5);
            if (a <= 0) continue;
            unsigned int d = window_get_pixel_phys(x, y);
            unsigned int r = (((color >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * (255 - a)) / 255;
            unsigned int g = (((color >> 8) & 0xFF) * a + ((d >> 8) & 0xFF) * (255 - a)) / 255;
            unsigned int b = ((color & 0xFF) * a + (d & 0xFF) * (255 - a)) / 255;
            window_pixel_phys(x, y, (r << 16) | (g << 8) | b);
        }
    }
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
#define aa_band gui_aa_band /* one band, owned by gui_paint.c with the circle and capsule painters both builds share */
#define AA_BAND aa_band

/* Same corner AA as gui_rounded_rect, but the fill itself is a real top-to-
   bottom gradient instead of one flat color, the classic glossy-icon look
   (lighter catching the light at top, darker at the bottom, real depth),
   direct follow-up after "rich... gradient with a bit of a 3D icon style,
   like Apple" feedback on the flat-color first pass. Every pixel here is
   still a genuine precomputed solid color (gui_lerp), no alpha channel
   this framebuffer doesn't have, same technique the wallpaper and every
   other AA edge in this file already uses. */
static void gui_rounded_rect_gradient(int x, int y, int w, int h, unsigned int color_top, unsigned int color_bottom, unsigned int bg, int r){
    /* Settings retina pass: this is the same logical-space-then-block-
       replicate staircase gui_fill_circle's v82 fix and gui_draw_capsule's
       matching fix already found and fixed for circles/capsules -- the AA
       ramp below is computed once per LOGICAL pixel, and at window_scale()
       2 (every real dock-launched windowed app, including Settings)
       window_rect/window_pixel replicate each logical pixel into a flat
       2x2 physical block with no interpolation, so the ramp rasterizes as
       distinct flat terraces: the Wind switch's track edge, the grouped
       card's corners and the sidebar highlight's corners, all real macro
       staircasing (confirmed visually, /tmp/jt-settings-4x-*.png). This
       function is the one AA rounded-rect primitive every one of those
       three draws through, so one fix here covers all three. Same
       technique gui_rounded_rect_on_wallpaper's v79 fix uses: work in
       PHYSICAL pixels, SSxSS true subsample coverage per corner pixel
       instead of a single distance threshold, blend straight to `bg`
       (already a flat color for every caller here, no wallpaper sampling
       needed the way the tray's on-wallpaper variant does). The original
       logical-space path stays for callers with an offscreen target
       pushed (window_has_target() true, e.g. gui_render_icon_cached's own
       6x-supersampled icon buffer): that path already gets its real AA
       from the later box-downsample, exactly like every icon glyph. */
    if (!window_has_target() && window_scale() > 1) {
        int sc = (int)window_scale();
        int px0 = x * sc, py0 = y * sc, pw = w * sc, ph = h * sc, pr = r * sc;
        const int SS = 4, band = 3, margin = 2;
        if (ph > 2 * pr) {
            for (int py = pr; py < ph - pr; py++) {
                int ly = py / sc;
                window_fill_rect_phys(px0, py0 + py, pw, 1, gui_lerp(color_top, color_bottom, ly, h));
            }
        }
        for (int py = 0; py < ph; py++) {
            if (py >= pr && py < ph - pr) continue;
            int cy = py < pr ? pr : ph - 1 - pr;
            int oy = py - cy;
            int ly = py / sc;
            unsigned int row_col = gui_lerp(color_top, color_bottom, ly, h);
            for (int px = 0; px < pw; px++) {
                int cx = px < pr ? pr : (px >= pw - pr ? pw - 1 - pr : px);
                int ox = px - cx;
                int d2 = ox * ox + oy * oy;
                unsigned int col = row_col;
                if (d2 > (pr - band) * (pr - band)) {
                    if (d2 > (pr + margin) * (pr + margin)) continue;
                    int inside = 0;
                    for (int sy = 0; sy < SS; sy++) {
                        int subdy = oy * SS + sy * 2 + 1 - SS;
                        for (int sx = 0; sx < SS; sx++) {
                            int subdx = ox * SS + sx * 2 + 1 - SS;
                            long sd2 = (long)subdx * subdx + (long)subdy * subdy;
                            if (sd2 <= (long)(pr * SS) * (pr * SS)) inside++;
                        }
                    }
                    if (inside == 0) continue;
                    col = (inside >= SS * SS) ? row_col : gui_lerp(row_col, bg, SS * SS - inside, SS * SS);
                }
                window_pixel_phys(px0 + px, py0 + py, col);
            }
        }
        return;
    }
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
void gui_draw_wallpaper_rows(int y_from, int y_to){ gui_draw_wallpaper_rows_sway(y_from, y_to, 0); }

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
static int wind_base_width = 0; void music_ring3_open(void); void keyrate_ring3_open(void); void toroid_ring3_open(void); void calculator_ring3_open(void); void quotestreak_ring3_open(void); void bookrank_ring3_open(void); void tonchi_ring3_open(void); void fieldbook_ring3_open(void); void clock_ring3_open(void); void portfolio_ring3_open(void); void activity_ring3_open(void); void contacts_ring3_open(void); void hikko_ring3_open(void); void reminders_ring3_open(void); void curbfind_ring3_open(void); void calendar_ring3_open(void); void search_ring3_open(void); void epiphany_ring3_open(void); void burrow_ring3_open(void); void mail_ring3_open(void); void notes_ring3_open(void); void terminal_ring3_open(void); void samantha_ring3_open(void); void ring3app_autoopen_arm(const char *cl); void r3stress_arm(const char *cl); void r3stress_desktop_round(void); void ring3app_autoopen_run(int mx, int my); void entropy_init(void); void entropy_bytes(void *buf, unsigned int n); void pdestress_desktop_round(void);
void movies_ring3_open(void); void hamurabi_ring3_open(void); void windgate_ring3_open(void); void panes_ring3_open(void); void claude_ring3_open(void); void mines_ring3_open(void);

static int gui_ring3_windowed(int icon);
int gui_app_windowed; /* real definition + comment below, near gui_draw_app_titlebar; forward-declared here so the wallpaper sampler and the menubar clamp below can both read it */
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

/* Issue #14 round two: the ~200ms left after PR #137's window-chrome fix
   was every physical pixel of the desktop wallpaper (menubar to bottom,
   about 1.9M pixels at 1920x1080) running through gui_wallpaper_px's
   bilinear sample plus a daynight tint blend, per pixel, on every single
   window open (gui_launch_from_dock -> gui_draw_desktop -> gui_draw_
   wallpaper). The photo, scale and tint don't change between one present
   and the next, so there's nothing to recompute: this caches the fully
   sampled and tinted result once, keyed on physical size, and later
   draws become a memcpy per row. Same tradeoff gui_dock_band_cache_build
   already makes for the dock tray (this file's own comment on it): tint
   is baked in at build time, not re-sampled live forever, so a real hour
   boundary crossed mid-session won't repaint until wall_caches_drop runs
   (theme switch) or the physical size changes. Windowed apps never use
   this path (gui_app_windowed draws into a clipped viewport with its own
   scale of the photo, not the desktop's), and window_phys_row itself
   already refuses a raw pointer whenever a screen_band or viewport is
   active, so the memcpy path only ever fires for the real desktop
   compositing straight to fb/back. */
static unsigned int *wall_full_cache = 0;
static int wall_full_pw = 0, wall_full_ph = 0, wall_full_night = -1, wall_full_day = -1;
static void gui_wall_full_cache_build(void){
    int sc = (int)window_scale();
    int pw = (int)window_width() * sc;
    int top = GUI_MENUBAR_H * sc;
    int ph = (int)window_height() * sc - top;
    if (pw <= 0 || ph <= 0) return;
    /* keyed on the tint too, so an hour boundary crossed mid-session repaints */
    if (wall_full_cache && wall_full_pw == pw && wall_full_ph == ph
        && wall_full_night == daynight_night_pct && wall_full_day == daynight_day_pct) return;
    if (wall_full_cache) { kfree(wall_full_cache); wall_full_cache = 0; }
    wall_full_pw = pw; wall_full_ph = ph;
    wall_full_night = daynight_night_pct; wall_full_day = daynight_day_pct;
    wall_full_cache = (unsigned int *)kmalloc((unsigned int)(pw * ph) * sizeof(unsigned int));
    if (!wall_full_cache) return;
    for (int py = 0; py < ph; py++){
        struct wp_row c = gui_wallpaper_row(top + py, 0);
        unsigned int *row = wall_full_cache + py * pw;
        for (int px = 0; px < pw; px++)
            row[px] = gui_daynight_tint(gui_wallpaper_px(&c, px));
    }
}
/* Dropped alongside the dock band and wind caches (wall_caches_drop,
   below): a theme switch or resolution change means the baked pixels are
   stale, and a lazy rebuild on next use is cheap (this is a one-time
   compositing cost, not a per-frame one). */
static void gui_wall_full_cache_drop(void){
    if (wall_full_cache) { kfree(wall_full_cache); wall_full_cache = 0; }
    wall_full_pw = 0; wall_full_ph = 0;
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
unsigned int gui_wallpaper_sample(int px, int py, int sway){
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
        int in_rows = (eh > 0 && py >= ey && py < ey + eh);
        /* The cached full-desktop path: only for the real desktop (never
           gui_app_windowed's own clipped viewport), never mid-exclusion
           (sway is the only caller that ever passes a real exclude rect,
           see gui_wall_full_cache_build's own comment), and only when
           window_phys_row actually hands back a raw pointer (it refuses
           one whenever a screen_band or viewport is active, e.g. the dock
           band cache build or a windowed app -- those keep sampling
           fresh, correctly, through the fallback below). */
        if (!sway && !gui_app_windowed && !in_rows) {
            gui_wall_full_cache_build();
            int top = GUI_MENUBAR_H * sc;
            int pw = (int)window_width() * sc;
            if (wall_full_cache && wall_full_pw == pw && py >= top && py - top < wall_full_ph) {
                unsigned int *dst = window_phys_row(py);
                if (dst) {
                    unsigned int *src = wall_full_cache + (py - top) * wall_full_pw;
                    memcpy(dst, src, (unsigned int)pw * sizeof(unsigned int));
                    window_damage(0, py, pw, 1);
                    continue;
                }
            }
        }
        struct wp_row c = gui_wallpaper_row(py, sway);
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
    int top = gui_app_windowed ? 0 : GUI_MENUBAR_H; /* a windowed app has no menu bar inside its viewport */
    if (y0 < top * sc) y0 = top * sc;
    if (x1 > (int)window_width() * sc) x1 = (int)window_width() * sc;
    if (y1 > (int)window_height() * sc) y1 = (int)window_height() * sc;
    for (int py = y0; py < y1; py++) {
        struct wp_row c = gui_wallpaper_row(py, 0);
        for (int px = x0; px < x1; px++)
            window_pixel_phys(px, py, gui_wallpaper_px(&c, px));
    }
}

/* Same rectangle repaint with the day/night tint the desktop itself
   applies (gui_draw_wallpaper_rows_sway_ex's own per-pixel path), so a
   strip uncovered by a window drag matches the wallpaper around it. */
static void gui_daynight_wallpaper_rect(int x, int y, int w, int h){
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
            window_pixel_phys(px, py, gui_daynight_tint(gui_wallpaper_px(&c, px)));
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

/* Real, user-reported flicker: this whole bar (a solid white rect, the
   logo, "Joshua Tree", the clock) got redrawn identically on every single
   hover-state change, since gui_draw_desktop calls this unconditionally
   on every mouse move. Nothing here actually depends on hover at all, and
   without a back buffer to swap in atomically, redrawing pixels that
   didn't need to change is pure flicker, not just pure waste. Skips the
   redraw entirely once per real minute unless forced, matching the one
   thing in this bar that actually changes on its own. */
static int gui_menubar_last_min = -1;
static void gui_menubar_force_redraw(void){ gui_menubar_last_min = -1; } static int gui_bleed_open(void); /* a full-bleed window covers the menu bar */

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
static volatile unsigned jt_data_stamp = 0; /* bumped whenever WEATHER.TXT or STOCKS.TXT is rewritten; windows poll it through SYS_SYSINFO */
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
/* 1.9.26: SYS_SYSINFO fill and the SYS_LAUNCH_REQUEST mailbox (kernel/syscall.c). */
void jt_sysinfo_fill(struct jt_sysinfo *si){
    char *z = (char *)si;
    for (unsigned i = 0; i < sizeof *si; i++) z[i] = 0;
    si->version = JT_SYSINFO_VERSION;
    si->size = sizeof *si;
    si->phone = boot_to_phone ? 1u : 0u;
    si->wx_have = weather_have ? 1u : 0u;
    si->wx_state = (unsigned)weather_state;
    si->wx_temp_c = weather_temp_c;
    si->wx_code10 = weather_code10;
    si->llm_port = (unsigned)llm_port;
    si->data_stamp = jt_data_stamp;
    for (int i = 0; i < JT_WX_TEXT_MAX - 1 && weather_text[i]; i++) si->wx_text[i] = weather_text[i] == (char)0xF8 ? (char)0xB0 : weather_text[i]; /* ring 3 text is Latin-1: the kernel font's CP437 degree is 0xB0 there */
    for (int i = 0; i < JT_SYSINFO_HOST_MAX - 1 && llm_host[i]; i++) si->llm_host[i] = llm_host[i];
}
static volatile int launch_pending = -1;
int jt_launch_request(const char *name){
    if (launch_pending >= 0) return -16; /* EBUSY, as in syscall.c */
    for (int i = 0; i < GUI_APPS_FOLDER; i++) {
        const char *a = APPS[i].name;
        if (!a || (!portfolio_dock && i == GUI_APP_PORTFOLIO)) continue;
        int k = 0;
        while (a[k] && a[k] == name[k]) k++;
        if (!a[k] && !name[k]) { launch_pending = i; return 0; }
    }
    return -22; /* EINVAL */
}
int jt_launch_take(void){ int i = launch_pending; launch_pending = -1; return i; }
/* 398 SYS_REFRESH mailbox: one pending request, recorded in the gate, served by the desktop loop. */
static volatile int refresh_kind = -1, refresh_arg = 0;
int jt_refresh_request(int kind, int arg){
    if (kind != JT_REFRESH_WEATHER && kind != JT_REFRESH_STOCKS) return -22;
    if (kind == JT_REFRESH_STOCKS && ((arg & 0xFF) >= 5 || ((arg >> 8) & 0xFF) >= 8)) return -22;
    if (refresh_kind >= 0) return -16;
    refresh_arg = arg; refresh_kind = kind; return 0;
}
int jt_refresh_take(int *arg){ int k = refresh_kind; if (k >= 0) { *arg = refresh_arg; refresh_kind = -1; } return k; }
/* Test/diagnostic override, read once from the multiboot command line
   (`-append "wxhost=10.0.2.2:8099"`, see kmain): both the location and the
   forecast request go to this literal IP:port instead of ip-api.com and
   api.open-meteo.com. tools/checks/weather-app-check.sh points it at a
   local fake server to drive success, bad-response and timeout without
   the real internet. Empty (every normal boot) means the real hosts. */
static char wx_override_host[20] = "";
static unsigned short wx_override_port = 80;
/* 1.1.1: tilehost=/tileport=, the wall_fetch equivalent of wxhost= above --
   for tools/checks/wallcompose-check.py to point BOTH tile hosts
   (a.tile.opentopomap.org for map themes, mt0.google.com for Satellite)
   at one local fake server instead of the real internet, headlessly.
   Root cause this exists for: wallpaper-check.py/satellite-wallpaper-
   check.py/wallfx-check.py all fetch over the real internet with no way
   to fake the tile bytes, so a hermetic proof of wall_fetch's compose
   path (independent of whether a1.tile.opentopomap.org/mt0.google.com
   are reachable from a given CI runner) needed its own override, the
   same shape as wxhost's. Empty (every normal boot) means the real
   hosts; set means BOTH hosts point here, since a fake server tells map
   and satellite tiles apart by path (png vs jpeg-shaped bytes), not by
   host. */
static char tile_override_host[20] = "";
static unsigned short tile_override_port = 80;
/* 1.1.2: walltheme=map|sat|photo, a one-boot theme override applied AFTER
   settings_load() (same reasoning as llmhost=): the default has been
   WALL_SAT since v0.76.7, and tools/checks/wallpaper-check.py compares
   the first automatic fetch against OpenTopoMap PNGs, so it boots with
   walltheme=map instead of assuming the default. -1 = not given. */
static int wall_theme_override = -1;
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
   ip-api answer, that the URL really carries the dynamic location.
   (geo_lat/geo_lon/geo_city/geo_have and the loc_* Settings-location
   override are declared earlier, alongside wind_enabled, since
   settings_load needs them and settings_load has to come before this
   point in the file.) */
/* Open-Meteo's own geocoding endpoint (the same house the weather forecast
   already comes from), through the exact http_get_timeout/wx_override_host
   plumbing geo_fetch and weather_fetch_inner already use -- one more host
   name, same request shape, nothing new. `query` is bounded the same way
   every other Settings text field is (settings_prompt_line's max param);
   spaces are percent-encoded since a city name is likely to have one.
   No result, a bad reply, or no network all fail cleanly with loc_err set
   and loc_have/geo_have untouched -- never a fabricated coordinate, never
   a panic. */
static int loc_geocode(const char *query){
    loc_err[0] = 0;
    if (!query[0]) { const char *m = "empty"; int i=0; while (m[i]) { loc_err[i]=m[i]; i++; } loc_err[i]=0; return 0; }
    if (!net_init(0x0A00020F)) { const char *m = "no network card"; int i=0; while (m[i]) { loc_err[i]=m[i]; i++; } loc_err[i]=0; return 0; }
    static char path[112];
    int p = 0; const char *s;
    for (s = "/v1/search?name="; *s; s++) path[p++] = *s;
    for (const char *c = query; *c && p < 96; c++) {
        unsigned char ch = (unsigned char)*c; /* percent-encode all but [A-Za-z0-9] so '&', '#', '%' can't reshape the query */
        if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')) path[p++] = (char)ch;
        else { path[p++]='%'; path[p++]="0123456789ABCDEF"[ch >> 4]; path[p++]="0123456789ABCDEF"[ch & 15]; }
    }
    for (s = "&count=1"; *s; s++) path[p++] = *s;
    path[p] = 0;
    static char body[1024];
    int n = wx_override_host[0] ? http_get_timeout(wx_override_host, path, wx_override_port, body, sizeof(body) - 1, WX_REPLY_TIMEOUT_TICKS)
                                 : http_get_timeout("geocoding-api.open-meteo.com", path, 80, body, sizeof(body) - 1, WX_REPLY_TIMEOUT_TICKS);
    if (n <= 0 || http_last_status() != 200) {
        int e = net_last_error();
        const char *m = (n < 0 || e == NET_ERR_REPLY_TIMEOUT || e == NET_ERR_ARP_TIMEOUT || e == NET_ERR_DNS_TIMEOUT || e == NET_ERR_CONNECT_TIMEOUT) ? "network unreachable" : "geocoding request failed";
        int i=0; while (m[i]) { loc_err[i]=m[i]; i++; } loc_err[i]=0;
        return 0;
    }
    body[n] = 0;
    char lat[16], lon[16], name[LOC_NAME_MAX];
    if (!json_extract_number_text(body, "latitude", lat, sizeof(lat)) ||
        !json_extract_number_text(body, "longitude", lon, sizeof(lon))) {
        const char *m = "location not found"; int i=0; while (m[i]) { loc_err[i]=m[i]; i++; } loc_err[i]=0;
        return 0;
    }
    if (!json_extract_string(body, "name", name, sizeof(name))) name[0] = 0;
    int i;
    for (i = 0; lat[i]; i++) loc_lat[i] = lat[i]; loc_lat[i] = 0;
    for (i = 0; lon[i]; i++) loc_lon[i] = lon[i]; loc_lon[i] = 0;
    for (i = 0; name[i] && i < LOC_NAME_MAX - 1; i++) loc_name[i] = name[i]; loc_name[i] = 0;
    loc_have = 1;
    /* Same fields weather_fetch_inner/wall_fetch already read -- setting
       these here means neither one needs to know a manual override even
       exists. */
    for (i = 0; loc_lat[i]; i++) geo_lat[i] = loc_lat[i]; geo_lat[i] = 0;
    for (i = 0; loc_lon[i]; i++) geo_lon[i] = loc_lon[i]; geo_lon[i] = 0;
    for (i = 0; loc_name[i] && i < 23; i++) geo_city[i] = loc_name[i]; geo_city[i] = 0;
    geo_have = 1;
    serial_puts("locgeo="); serial_puts(loc_lat); serial_puts(","); serial_puts(loc_lon); serial_puts(" name="); serial_puts(loc_name); serial_puts("\n");
    return 1;
}
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

#include "weather_extra.h"
static int weather_fetch_inner(void){
    weather_err[0] = 0;
    if (!net_init(0x0A00020F)) { weather_set_error(WX_OFFLINE, "no network card", ""); return 0; }
    if (!geo_have && !geo_fetch()) return 0; /* v71: no real location, no fetch, nothing fabricated */
    static char body[6144]; /* the 24-hour and 7-day reply is ~2.7KB; was 2048 */
    static char path[640]; /* was 320: the field list below is ~330 bytes plus the position; http.c's request buffer is 1024 */
    { int p = 0; const char *s;
      for (s = "/v1/forecast?latitude="; *s; s++) path[p++] = *s;
      for (s = geo_lat; *s; s++) path[p++] = *s;
      for (s = "&longitude="; *s; s++) path[p++] = *s;
      for (s = geo_lon; *s; s++) path[p++] = *s;
      /* One request for everything the Weather window shows: now, the next 24 hours and seven
         days with sunrise, sunset, UV and chance of rain. The real reply is ~2.7KB, inside the
         6144-byte body buffer above; forecast_hours=24 starts at the current hour. */
      for (s = "&current=temperature_2m,apparent_temperature,relative_humidity_2m,wind_speed_10m,weather_code,pressure_msl,visibility,is_day"
               "&hourly=temperature_2m,precipitation_probability,weather_code,is_day"
               "&daily=weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset,uv_index_max,precipitation_probability_max"
               "&forecast_days=7&forecast_hours=24&timezone=auto"; *s && p < 638; s++) path[p++] = *s;
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
    wx_parse_extras(body);
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
    wx_serial_summary();
    serial_puts("wx="); serial_puts(weather_text); serial_puts("\n"); /* v71: tools/geo-check.sh asserts the fetch really landed, not just that the URL was built */
    return 1;
}
static void weather_write_file(void);
static void weather_fetch(void){
    weather_last_tick = ticks();
    serial_puts("wxfetch\n"); /* tools/checks/weather-app-check.sh counts these: a failed fetch must not re-run on every repaint */
    weather_fetch_inner();
    /* One line per attempt, the state the window will show and why. */
    serial_puts("wxstate="); serial_puts(weather_state_name(weather_state));
    if (weather_err[0]) { serial_puts(" "); serial_puts(weather_err); }
    serial_puts("\n");
    weather_write_file();
}

/* 1.9.22: Weather is a ring-3 program (user/weather.c). It cannot see these
   statics, so every fetch, good or not, leaves WEATHER.TXT for it: one
   "key value" line per field, the seven forecast days as "d weekday code hi lo rain uv10 sunrise sunset" and the next 24 hours as hs/ht/hc/hp/hd lines. */
static void weather_write_file(void){
    char b[1536], *o = b; const char *c;
    #define WXPUT(str) do { for (c = (str); *c; c++) *o++ = *c; } while (0)
    #define WXNUM(key, v) do { WXPUT(key " "); o = wx_put_int(o, (v)); *o++ = '\n'; } while (0)
    WXPUT("state "); WXPUT(weather_state_name(weather_state)); *o++ = '\n';
    WXPUT("err "); WXPUT(weather_err); *o++ = '\n';
    WXPUT("city "); WXPUT(geo_city); *o++ = '\n';
    WXPUT("word "); WXPUT(weather_word(weather_code10 / 10)); *o++ = '\n';
    WXNUM("have", weather_have); WXNUM("temp", weather_temp_c); WXNUM("code", weather_code10 / 10);
    WXNUM("extra", wx_extra_have); WXNUM("feels", wx_feels_c); WXNUM("hum", wx_humidity); WXNUM("wind", wx_wind_kmh);
    o = wx_write_extras(o);
    vfs_replace_file("WEATHER.TXT", b, (unsigned int)(o - b));
    jt_data_stamp++;
}

/* The dock's Weather, blocking fallback only (a full window table). The window path in
   user/weather.c asks for refetches with SYS_REFRESH instead; here the app just runs once. */
int weather_ring3_run(void);
void weather_ring3_open(void){
    if (!weather_tried_once) { weather_tried_once = 1; weather_fetch(); gui_menubar_force_redraw(); }
    unsigned int t0 = ticks();
    weather_ring3_run();
    weather_last_tick += ticks() - t0;
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
            host = tile_override_host[0] ? tile_override_host : "mt0.google.com";
            for (s = "/vt/lyrs=s&x="; *s; s++) path[p++] = *s;
            { char d[12]; int nd = 0; unsigned int u = (unsigned int)ttx; do { d[nd++] = '0' + u % 10; u /= 10; } while (u); while (nd) path[p++] = d[--nd]; }
            for (s = "&y="; *s; s++) path[p++] = *s;
            { char d[12]; int nd = 0; unsigned int u = (unsigned int)tty; do { d[nd++] = '0' + u % 10; u /= 10; } while (u); while (nd) path[p++] = d[--nd]; }
            for (s = "&z="; *s; s++) path[p++] = *s;
            { char d[12]; int nd = 0; unsigned int u = WALL_ZOOM; do { d[nd++] = '0' + u % 10; u /= 10; } while (u); while (nd) path[p++] = d[--nd]; }
            path[p] = 0;
        } else {
            host = tile_override_host[0] ? tile_override_host : "a.tile.opentopomap.org";
            for (s = "/"; *s; s++) path[p++] = *s;
            { char d[12]; int nd = 0; unsigned int u = WALL_ZOOM; do { d[nd++] = '0' + u % 10; u /= 10; } while (u); while (nd) path[p++] = d[--nd]; path[p++] = '/'; }
            { char d[12]; int nd = 0; unsigned int u = (unsigned int)ttx; do { d[nd++] = '0' + u % 10; u /= 10; } while (u); while (nd) path[p++] = d[--nd]; path[p++] = '/'; }
            { char d[12]; int nd = 0; unsigned int u = (unsigned int)tty; do { d[nd++] = '0' + u % 10; u /= 10; } while (u); while (nd) path[p++] = d[--nd]; }
            for (s = ".png"; *s; s++) path[p++] = *s;
            path[p] = 0;
        }
        n_bytes = http_get(host, path, tile_override_host[0] ? tile_override_port : 80, body, 65536);
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

static void gui_draw_mark_sized(int cx, int cy, int size, unsigned int ink);
static void gui_draw_menubar(void){
    if (gui_bleed_open()) { gui_menubar_last_min = -1; return; } /* repaints on the first call after it closes */
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
    gui_hairline_h(0, GUI_MENUBAR_H - 1, (int)window_width(), 0x00BDB8B0); /* 2.0: one physical pixel, not a doubled logical row */
    gui_draw_mark_sized(24, GUI_MENUBAR_H / 2, 20, 0x001C1C1E); /* 2.0: the real brand mark (boot_mark) sits in the apple-menu spot */
    font_draw_string(portfolio_dock ? "Joshua Trommel" : "Joshua Tree", 40, 7, 0x001C1C1E, -1); /* portfolio mode is his site, so the corner carries his name */

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
        font_draw_string(weather_text, wx, 7, 0x001C1C1E, -1); /* match the clock's own ink and weight, not a warm brown -- must read on every wallpaper theme, not just the default */
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
static void gui_icon_tonchi(int cx, int cy, int s, unsigned int bg){
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
static void gui_icon_hikko(int cx, int cy, int s, unsigned int bg){
    int r = s * 3 / 10, base_y = cy + r + s / 10;
    unsigned int glow_top = 0x00FFF3B0, glow_bot = 0x00FFC93C; /* real warm bulb color */
    gui_fill_circle_gradient(cx, cy, r, glow_top, glow_bot, bg);
    window_rect(cx - r / 3, base_y, 2 * (r / 3) + 1, s / 12, gui_blend(ICON_FG, bg));
    gui_draw_capsule(cx - r - 4, cy - r - 2, cx - r - r/2, cy - r - r/2, 2, ICON_FG, bg);
    gui_draw_capsule(cx + r + 4, cy - r - 2, cx + r + r/2, cy - r - r/2, 2, ICON_FG, bg);
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

static void gui_draw_icon_glyph(int icon, int cx_center, int cy, int size, unsigned int bg){
    if (icon >= 0 && icon < GUI_APP_COUNT && APPS[icon].icon) APPS[icon].icon(cx_center, cy, size, bg);
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
#define ICON_CACHE_SLOTS 3 /* normal, magnified, Apps-folder grid (its own surface colour, so it never evicts the dock's tile) */
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
       kernel/memmap.h pins at JT_USER_BASE, and docs/SYSCALL-ABI.md names that
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
    unsigned int bg = APPS[icon].color;
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

#include "clockicon.h"

static void gui_draw_one_icon_on(int icon, int cx_center, int cy_bottom, int size, unsigned int under){
    int x = cx_center - size / 2, y = cy_bottom - size;
    int slot = (under != DOCK_TRAY_COLOR) ? 2 : (size == DOCK_ICON) ? 0 : 1;
    unsigned int *tile = gui_render_icon_cached(icon, size, slot, under);
    if (tile) {
        gui_blit_tile(tile, x, y, size, under);
        gui_icon_overlay(icon, cx_center, cy_bottom, size);
        return;
    }
    /* out of memory for the cache: draw directly, un-supersampled, rather than draw nothing */
    unsigned int bg = APPS[icon].color;
    unsigned int bg_light = gui_blend(bg, 0x00FFFFFF), bg_dark = gui_blend(bg, 0x00000000);
    gui_rounded_rect_gradient(x, y, size, size, bg_light, bg_dark, under, size * 22 / 100);
    gui_draw_gloss(x, y, size, size, bg, size * 22 / 100 + 1);
    gui_draw_icon_glyph(icon, cx_center, y + size / 2, size, bg);
    gui_icon_overlay(icon, cx_center, cy_bottom, size);
}
void gui_draw_one_icon(int icon, int cx_center, int cy_bottom, int size){ gui_draw_one_icon_on(icon, cx_center, cy_bottom, size, DOCK_TRAY_COLOR); }

/* hover_slot: which slot shows the magnify+label (-1 none). drag_slot: the
   slot currently being dragged, drawn separately so it can float free of
   the row under the cursor instead of at its slot position. */
/* v40: the dock band's top edge, high enough to cover a magnified,
   lifted icon and its label, so repainting this band alone is enough to
   erase any previous hover state. */
void gui_draw_desktop(int hover_slot, int drag_slot, int drag_mx, int drag_my){
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
    gui_menubar_force_redraw(); gui_draw_menubar(); /* 2.6.34: every full repaint redraws the bar; the Launchpad clears it and the minute gate left it gone */
    gui_draw_dock(hover_slot, drag_slot, drag_mx, drag_my);
}

/* v75: see wall_apply. wind_base is rebuilt lazily by gui_draw_desktop,
   the dock band by gui_redraw_dock_band's own top-mismatch check. */
static void wall_caches_drop(void){
    if (wind_base) { kfree(wind_base); wind_base = 0; }
    dock_band_cache_top = -1;
    gui_wall_full_cache_drop();
}

/* The software cursor (save-under, the antialiased arrow) lives in gui_paint.c, shared with the ARM build. */

/* App viewers have their own input loops. Keep the pointer alive while one
   is open, drawing it in screen coordinates outside the app viewport. */
int gui_app_windowed = 0;
/* Vertical shift for an app's own content. Full screen, an app draws its
   own title strip across the top 40px and starts content at y=52. In a
   dock window the frame already draws the title bar above the viewport,
   so the same layout moves up by that strip, the same 32px Stocks has
   always saved through stx_top(). Add it to every content y. */
int gui_app_dy(void){ return gui_app_windowed ? -32 : 0; }
/* Live title-bar drag for the blocking single-window apps (everything
   that opens through gui_launch_from_dock: Notes, Terminal, Chat, the
   fleet apps, Settings...). Direct request: "app windows should be
   draggable". Only the five multi-window apps could move before, and
   only as a snap-preview outline. These apps block inside their own
   input loops, so the one place that runs while they wait is
   gui_app_mouse_tick below: it watches the left button, arms on a press
   in the title band right of the traffic lights, and on every pointer
   move shifts the whole window (chrome and content) inside the back
   buffer with window_move_rect, repaints the one or two wallpaper strips
   the window just uncovered, and slides the app's own viewport along, so
   whatever the app draws next lands at the new place. The press that
   starts a drag is swallowed from mouse_click_edge, so it is never also
   the app's "click anywhere closes"; a press on the lights or the
   content keeps every contract it had. Clamped to the desktop strip
   (below the menu bar, above the dock band) so the uncovered area is
   always plain wallpaper, nothing else needs redrawing. */
static int app_win_x = 0, app_win_y = 0, app_win_w = 0, app_win_h = 0;
static int app_drag_held = 0, app_drag_on = 0, app_drag_gx = 0, app_drag_gy = 0;
static void gui_daynight_wallpaper_rect(int x, int y, int w, int h);
int app_view_x, app_view_y; int app_view_w, app_view_h;
int app_cursor_x, app_cursor_y;
void gui_app_mouse_tick(void){
    if (!gui_app_windowed) return;
    int dx = 0, dy = 0, buttons = 0;
    int moved = mouse_get_delta(&dx, &dy, &buttons);
    if (!moved && cursor_saved_x >= 0) return;
    window_clear_viewport();
    gui_cursor_restore();
    app_cursor_x += dx; app_cursor_y += dy;
    mouse_get_absolute(&app_cursor_x, &app_cursor_y, (int)window_width(), (int)window_height()); /* v62: absolute pointer wins over the relative walk when the backdoor is live; viewport already cleared above, so this is the full screen */
    if (app_cursor_x < 0) app_cursor_x = 0;
    if (app_cursor_y < 0) app_cursor_y = 0;
    if (app_cursor_x > (int)window_width() - CURSOR_W) app_cursor_x = (int)window_width() - CURSOR_W;
    if (app_cursor_y > (int)window_height() - CURSOR_H) app_cursor_y = (int)window_height() - CURSOR_H;
    if (boot_to_phone) phone_back_zone_tick(buttons, app_drag_held, app_cursor_x, app_cursor_y); /* phone_home.h: back-chevron tap, no Esc key on a phone */ /* Live window drag (app_win_x's comment). Press edge: arm only in the
       title band, right of the three lights (x + 80 on), so the red
       close light and the app's own content keep their click semantics. */
    int held = buttons & 1;
    if (held && !app_drag_held) {
        app_drag_held = 1;
        if (app_win_w > 0 && app_cursor_y >= app_win_y && app_cursor_y < app_win_y + 30
            && app_cursor_x >= app_win_x + 80 && app_cursor_x < app_win_x + app_win_w) {
            app_drag_on = 1;
            app_drag_gx = app_cursor_x - app_win_x; app_drag_gy = app_cursor_y - app_win_y;
            mouse_click_edge(); /* consumed: this press is a grab, not the app's click */
        }
    } else if (!held && app_drag_held) {
        app_drag_held = 0; app_drag_on = 0;
    }
    if (app_drag_on) {
        int nx = app_cursor_x - app_drag_gx, ny = app_cursor_y - app_drag_gy;
        int max_x = (int)window_width() - app_win_w, max_y = gui_dock_band_top() - app_win_h;
        if (nx > max_x) nx = max_x;  if (nx < 0) nx = 0;
        if (ny > max_y) ny = max_y;  if (ny < GUI_MENUBAR_H) ny = GUI_MENUBAR_H;
        int mdx = nx - app_win_x, mdy = ny - app_win_y;
        if (mdx || mdy) {
            window_move_rect(app_win_x, app_win_y, app_win_w, app_win_h, mdx, mdy);
            /* the strips the window no longer covers: one per axis moved */
            if (mdx > 0)      gui_daynight_wallpaper_rect(app_win_x, app_win_y, mdx, app_win_h);
            else if (mdx < 0) gui_daynight_wallpaper_rect(app_win_x + app_win_w + mdx, app_win_y, -mdx, app_win_h);
            if (mdy > 0)      gui_daynight_wallpaper_rect(app_win_x, app_win_y, app_win_w, mdy);
            else if (mdy < 0) gui_daynight_wallpaper_rect(app_win_x, app_win_y + app_win_h + mdy, app_win_w, -mdy);
            app_win_x = nx; app_win_y = ny;
            app_view_x += mdx; app_view_y += mdy;
            serial_puts("windrag\n"); /* marker for tools/checks/windowdrag-check.py */
        }
    }
    if (!boot_to_phone) { gui_cursor_save(app_cursor_x, app_cursor_y); gui_draw_cursor(app_cursor_x, app_cursor_y); } /* phone_home.h: touch has no cursor, never draw the desktop arrow over an open app */
    window_set_viewport(app_view_x, app_view_y, (unsigned int)app_view_w, (unsigned int)app_view_h);
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
void gui_draw_app_titlebar(const char *title){
    if (gui_app_windowed) return;
    if (boot_to_phone) { phone_app_titlebar_draw(title); return; } /* kernel/phone_home.h: back chevron, no Esc key on a phone */
    gui_fill_circle(26, 20, 6, 0x00FF5F57, 0x00FAF8F6);
    gui_fill_circle(46, 20, 6, 0x00FFD64A, 0x00FAF8F6);
    gui_fill_circle(66, 20, 6, 0x00D8D4CE, 0x00FAF8F6);
    font_draw_string("x", 23, 12, 0x00602B28, -1);
    font_draw_string("-", 43, 12, 0x00624A20, -1);
    font_draw_string(title, 84, 12, 0x00555555, -1);
}


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

#include "ttf_render.h"
#include "auth.h"
#include "editor.h"
#include "mail.h"

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

/* Mono glyph for the two real character grids (terminal, keyrate), now
   drawn through the same runtime-TTF path Notes uses (kernel/ttf_render.h)
   instead of a baked PIL bitmap: DejaVu Sans Mono, rasterized at physical
   resolution with the shared glyph cache and coverage/ink blend. The mono
   face's own advance is genuinely fixed-width, so its rounded ttf_advance
   is used to pick the px size that lands on this cell exactly (computed
   once, cached), then every glyph is left-aligned and clipped to the
   caller's fixed cell the same way the old bitmap path was, keeping
   columns perfectly aligned -- the pitch itself is set by the caller
   (term_render/keyrate advance by a fixed logical step), this only has to
   not spill past its own cell. */
static float gui_aa_mono_px(int cell){
    static int cached_cell = -1;
    static float cached_px = 0;
    if (cell != cached_cell) { cached_px = ttfr_mono_px_for_cell(cell); cached_cell = cell; }
    return cached_px;
}
static void gui_aa_char_mono(unsigned char c, int px, int py, unsigned int fg, int bg, int cell){
    if (c < 32 || c > 126) c = '?';
    if (bg >= 0) for (int j = 0; j < 32; j++) for (int i = 0; i < cell; i++) window_pixel_phys(px + i, py + j, (unsigned int)bg);
    float mono_px = gui_aa_mono_px(cell);
    ttf_glyph_t *g = ttfr_glyph(TTF_FACE_MONO, c, mono_px);
    if (!g || !g->coverage) return;
    int base_x = px, base_y = py + ttf_ascent(ttfr_face(TTF_FACE_MONO), mono_px);
    ttfr_blend_glyph(g, base_x, base_y, fg, px, px + cell);
}

/* Physical-resolution text. Everything below draws at physical
   resolution through the same DejaVu Sans coverage glyphs the rest of the
   GUI text uses (editor_glyphs), so the window gets a real size hierarchy:
   16/20/24/28 px faces drawn 1:1, and the hero numeral scaled up from the
   28 px face. No gradient anywhere: flat cream surface, flat cards. */
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

/* gui_paint.h's icon text, through wx_text, the physical-resolution DejaVu path the Weather window uses. The Pi
   draws its window labels with it; nothing on i386 calls it since the Calendar tile became a plain picture. */
void gui_icon_text(const char *s, int lx, int ly, int face, int mul, unsigned int fg){ wx_text(s, lx, ly, face, 1, mul, fg); }
int gui_icon_text_w(const char *s, int face, int mul){ return wx_text_lw(s, face, 1, mul); }

/* "18°" style degrees into out. */
char *wx_put_int(char *o, int v){
    if (v < 0) { *o++ = '-'; v = -v; }
    char d[8]; int n = 0; if (!v) d[n++] = '0'; while (v && n < 7) { d[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) *o++ = d[--n];
    return o;
}

/* v37: the Apps folder. Every real app, laid out as a grid, so the dock
   can stay a short pinned list instead of growing until the icons are too
   small to read. Arrow keys + enter drive it as well as the mouse: the
   headless test harness can't inject usable mouse events (see
   guitest.sh's header for that whole trace), and an app screen only
   reachable by mouse would be an app screen this project can never
   regression-test. */
#include "apps_geom.h"
static int apps_vis_rows = 3; /* rows the open folder shows; set by gui_launch_from_dock, see apps_geom.h */
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
static void gui_apps_draw_grid(const struct apps_geom *g, int scroll_offset, int sel){
    for (int k = 0; k < gui_apps_n(); k++) {
        int i = gui_app_at(k);
        int row = k / APPS_COLS - scroll_offset;
        int col = k % APPS_COLS;
        if (row < 0 || row >= g->vis) continue; /* by row count: a row past the panel would spill and leave stale pixels */
        int cx = g->x0 + col * g->cell_w + g->cell_w / 2;
        int cy = g->y0 + row * g->cell_h;
        if (k == sel) gui_rounded_rect_gradient(cx - g->tile / 2 - 12, cy - 8, g->tile + 24, g->cell_h - 4,
                                                 0x00FFF8F1, 0x00E5D8D0, 0x00E9DEE0, 12);
        gui_draw_one_icon_on(i, cx, cy + g->tile, g->tile, 0x00E9DEE0);
        int lw = font_string_width(APPS[i].name);
        font_draw_string(APPS[i].name, cx - lw / 2, cy + g->tile + 8, 0x001C1C1E, -1);
    }
}
static void gui_apps_redraw_panel(const struct apps_geom *g, int scroll_offset, int sel){
    gui_draw_wallpaper_rect(g->px, g->py, g->pw, g->ph);
    gui_apps_glass(g->px, g->py, g->pw, g->ph);
    /* The title bar already reads "Apps"; only the key hint sits inside the panel. */
    gui_draw_hint(g->x0, g->py + APPS_PAD + 2, "arrow keys to move   enter opens   esc closes", 0x006A6064);
    gui_apps_draw_grid(g, scroll_offset, sel);
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
static int gui_multiwin_open(int icon); static void gui_refuse_open(int icon);
/* 2.0 gate 5: the Apps folder has no window of its own to host an app, so a launch closes the folder and opens the app as a compositor window, exactly a dock click (full table: the same refusal notice). */
static void gui_apps_launch(int icon){ window_clear_viewport(); /* the window is sized and clamped against the whole screen, not the folder's smaller viewport */ if (gui_multiwin_open(icon) < 0) gui_refuse_open(icon); }

/* One serial line per full repaint with the real layout, so the checks measure what was drawn (tools/checks/launchpad-centered-check.py). */
static void gui_apps_log_geom(const struct apps_geom *g){
    const char *k[] = {" vis=", " px=", " py=", " pw=", " ph=", " x0=", " y0=", " cw=", " ch=", " tile=", " vx=", " vy="};
    int v[] = {g->vis, g->px, g->py, g->pw, g->ph, g->x0, g->y0, g->cell_w, g->cell_h, g->tile, app_view_x, app_view_y};
    char b[12];
    serial_puts("appsgeom");
    for (int n = 0; n < 12; n++) { serial_puts(k[n]); app_utoa((unsigned)v[n], b); serial_puts(b); }
    serial_puts("\n");
}
static void gui_launch_apps(void){
    int sel = 0;
    int rows = (gui_apps_n() + APPS_COLS - 1) / APPS_COLS;
    int vw = (int)window_width(), vis, py;
    if (gui_app_windowed) { vis = apps_vis_rows; py = APPS_MARGIN; } /* boxed: the window was sized and centered around the panel */
    else { int top = GUI_MENUBAR_H, bot = gui_dock_y0(); vis = apps_rows_fit(top, bot); py = apps_panel_y(top, bot, vis); } /* bare desktop: centered between menu bar and dock */
    struct apps_geom G = apps_geom_make(vw, vis, py);
    const struct apps_geom *g = &G;
    int scroll_offset = 0; /* v0.77.0: mouse wheel scroll support, apps offset by row */

    /* The wallpaper behind this folder never changes while it is open, so it
       is painted once here and again only after an app has drawn over the
       screen. Every other change (selection, scroll) repaints the panel rect
       alone through gui_apps_redraw_panel. Repainting the wallpaper on every
       poll tick is what made this screen flash while scrolling or typing. */
    int full = 1;
    int clock_min_seen = -1;
    (void)rows;

    for (;;) {
        if (full) {
            full = 0;
            serial_puts("appsfullrepaint\n");
            gui_apps_log_geom(g);
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
            gui_draw_wallpaper_rows(0, (int)window_height()); if (!gui_app_windowed) { gui_menubar_force_redraw(); gui_draw_menubar(); } /* 2.6.34: full-screen Launchpad keeps the bar */
            gui_apps_redraw_panel(g, scroll_offset, sel);
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
        if (gui_clock_tick(&clock_min_seen)) gui_apps_redraw_panel(g, scroll_offset, sel); /* Clock hands follow the minute */
        /* v0.77.0: mouse wheel scroll to browse all apps, one row per scroll. */
        int k = get_key_or_click_until(ticks() + 100); /* wakes each second */
        if (k == KEY_WHEEL_UP || k == KEY_WHEEL_DOWN) {
            /* The view scrolls where the wheel says, full stop. Snapping the
               offset back to keep the selection on screen (what the first cut
               of this did) made the wheel look broken: one notch scrolled and
               the next frame jumped right back. Selection follows the view on
               the keyboard path below, not the other way round. */
            int old_offset = scroll_offset;
            int max_scroll = rows - g->vis;
            if (max_scroll < 0) max_scroll = 0;
            scroll_offset += (k == KEY_WHEEL_UP) ? -1 : 1;
            if (scroll_offset < 0) scroll_offset = 0;
            if (scroll_offset > max_scroll) scroll_offset = max_scroll;
            if (scroll_offset != old_offset)
                gui_apps_redraw_panel(g, scroll_offset, sel);
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
            for (int k = 0; k < gui_apps_n(); k++) {
                int i = gui_app_at(k);
                int row = k / APPS_COLS - scroll_offset;
                int col = k % APPS_COLS;
                /* Skip rows that are scrolled off-screen. Bounded by row
                   count (APPS_VIS_ROWS), not a repeated pixel-height
                   literal: this hit test used to compare against the
                   panel's old, wrong 375px height (see APPS_PANEL_H's own
                   note), a second copy of the exact bug class
                   gui_apps_draw_grid's row bound was already fixed for. */
                if (row < 0 || row >= g->vis) continue;
                int cx = g->x0 + col * g->cell_w + g->cell_w / 2;
                int cy = g->y0 + row * g->cell_h;
                int cell_x0 = cx - g->cell_w / 2, cell_y0 = cy - 8, cell_x1 = cell_x0 + g->cell_w, cell_y1 = cy + APPS_LABEL_H + 4;
                if (click_vx >= cell_x0 && click_vx < cell_x1 && click_vy >= cell_y0 && click_vy < cell_y1) { hit = i; break; }
            }
            if (hit >= 0) { gui_apps_launch(hit); return; }
            return; /* a tap outside every tile still closes the folder: with no keyboard there is no other way out */
        }
        if (k == KEY_ENTER) { gui_apps_launch(gui_app_at(sel)); return; }
        int old_sel = sel;
        if ((k == 'a' || k == KEY_LEFT) && sel > 0) sel--;                 /* left (the hint says arrow keys) */
        else if ((k == 'd' || k == KEY_RIGHT) && sel < gui_apps_n() - 1) sel++; /* right */
        else if ((k == 'w' || k == KEY_UP) && sel >= APPS_COLS) sel -= APPS_COLS;
        else if ((k == 's' || k == KEY_DOWN) && sel + APPS_COLS < gui_apps_n()) sel += APPS_COLS;
        else if (k >= '1' && k <= '9' && (k - '1') < gui_apps_n()) { gui_apps_launch(gui_app_at(k - '1')); return; }
        if (sel != old_sel) {
            /* Keyboard selection drags the view with it, the direction that is
               not surprising: move past the last visible row and the grid
               follows. */
            int sel_row = sel / APPS_COLS;
            if (sel_row < scroll_offset) scroll_offset = sel_row;
            if (sel_row >= scroll_offset + g->vis) scroll_offset = sel_row - g->vis + 1;
        }
        if (sel != old_sel) gui_apps_redraw_panel(g, scroll_offset, sel);
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
            gui_draw_hint(20, T + 52, "up/down to pick   r restores   e empties   esc closes", 0x00807468);
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

#include "settings_ui.h"

#include "stocks.h"
static int fs_ok_global = 0;
#include "bench.h"

/* The shared painters' text (gui_paint.h): the window frame and the dock's hover label live in gui_paint.c, which the
   ARM build links too; on i386 their text is plain font_draw_string. */
void gui_text(const char *s, int x, int y, unsigned int fg){ font_draw_string(s, x, y, fg, -1); }
int gui_text_width(const char *s){ return font_string_width(s); }
static void gui_launch(int icon){
    if (icon >= 0 && icon < GUI_APP_COUNT && APPS[icon].open) APPS[icon].open();
}

static int gui_multiwin_open(int icon);
/* 2.0 gate 5: a full window table or a failed window launch is an honest refusal now, never a
   blocking takeover of the screen. Serial marker plus a notice the next repaint draws. */
static const char *gui_notice_name = 0; static unsigned int gui_notice_until = 0;
static void gui_refuse_open(int icon){
    if (icon < 0 || icon >= GUI_APP_COUNT) return;
    gui_notice_name = APPS[icon].name; gui_notice_until = ticks() + 300;
    serial_puts("winrefuse: "); serial_puts(APPS[icon].name); serial_puts("\n"); /* tools/checks/dockcap-fallback-check.sh */
}
static void gui_notice_draw(void){
    if (!gui_notice_name || (int)(ticks() - gui_notice_until) >= 0) { gui_notice_name = 0; return; }
    char msg[64]; int n = 0; const char *pre = "Close a window to open ";
    for (const char *q = pre; *q && n < 40; q++) msg[n++] = *q;
    for (const char *q = gui_notice_name; *q && n < 62; q++) msg[n++] = *q;
    msg[n] = 0;
    int w = font_string_width(msg) + 32, h = 32, x = ((int)window_width() - w) / 2, y = GUI_MENUBAR_H + 12;
    gui_rounded_rect_on_wallpaper(x, y, w, h, 0x002C2C2E, 14);
    font_draw_string(msg, x + 16, y + 8, 0x00F5F5F7, -1);
}
void gui_launch_from_dock(int icon){
    /* 1.9.23: a ring-3 window app opens as a compositor window from every
       path (dock, keyboard, open= boot flag); the blocking viewport below
       is only the fallback when the launch failed. */
    if (gui_ring3_windowed(icon)) { if (gui_multiwin_open(icon) < 0) gui_refuse_open(icon); return; }
again:
    /* Keep the desktop visible around the app. The framebuffer viewport
       clips every app draw, including window_clear and physical AA text. */
    gui_draw_desktop(-1, -1, 0, 0);
    int apps = icon == GUI_APPS_FOLDER;
    int x = 70, y = 40, w = 820, h = 385;
    if (apps) {
        /* Launchpad: size the window around the glass panel, then center the PANEL on the screen
           and in the strip between the menu bar and the dock (apps_geom.h). */
        int top = GUI_MENUBAR_H, bot = gui_dock_y0();
        apps_vis_rows = apps_rows_fit(top, bot);
        int ph = apps_panel_h(apps_vis_rows), pw = apps_geom_make((int)window_width(), apps_vis_rows, 0).pw;
        w = pw + 2 * APPS_MARGIN + APPS_FRAME_W; h = ph + 2 * APPS_MARGIN + APPS_FRAME_H;
        x = ((int)window_width() - w) / 2;
        y = apps_panel_y(top, bot, apps_vis_rows) - APPS_MARGIN - 32;
    }
    gui_clamp_win_rect(&x, &y, &w, &h); /* phone screens are far narrower than these desktop-tuned numbers */
    gui_draw_window_frame(x, y, w, h, APPS[icon].name);
    window_set_viewport(x + 8, y + 32, (unsigned int)(w - 16), (unsigned int)(h - 40));
    app_view_x = x + 8; app_view_y = y + 32;
    app_view_w = w - 16; app_view_h = h - 40;
    app_cursor_x = editor_mouse_x; app_cursor_y = editor_mouse_y;
    cursor_saved_x = cursor_saved_y = -1;
    app_win_x = x; app_win_y = y; app_win_w = w; app_win_h = h;
    app_drag_held = 1; app_drag_on = 0; /* the click that opened this app is still down; it is not a grab */
    gui_app_windowed = 1;
    gui_close_was_click = 0;
    gui_launch(icon);
    gui_app_windowed = 0;
    app_win_w = app_win_h = 0; app_drag_on = 0;
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

   Phase 2 (click-to-focus, z-order hit testing) landed in v0.73.6; the
   2-window cap (GUI_MULTIWIN_MAX) is the one scoping choice still here. */
#define GUI_MULTIWIN_MAX 2
typedef struct {
    int icon;
    int x, y, w, h;
    int task; /* 1.9.23: the ring-3 task drawing this window, -1 for an in-kernel draw hook */
    int shown; /* 1.9.23: 0 until the compositor has drawn it once (an open= boot launch lands before the first frame) */
} gui_window_t;
/* 1.9.23: ring-3 apps that open as compositor windows (ring3app_launch_window)
   instead of the blocking viewport. Reminders is the proof; one row moves another. */
int ring3app_launch_window(const char *name, unsigned int w, unsigned int h);
void ring3app_window_reaped(int task, int status);
void ring3app_window_blit(int task, int vw, int vh); int ring3app_is_windowable(const char *name);
static int gui_ring3_windowed(int icon){ return icon >= 0 && icon < GUI_APP_COUNT && ring3app_is_windowable(APPS[icon].name); } /* every RING3_APPS row; a full window table or failed launch falls back to the blocking path */
static gui_window_t gui_windows[GUI_MULTIWIN_MAX];
static int gui_window_count = 0; /* gui_windows[0..gui_window_count-1] are the real open windows, back-to-front */

static int gui_multiwin_supported(int icon){ return icon >= 0 && icon < GUI_APP_COUNT && (APPS[icon].draw || gui_ring3_windowed(icon)); }
static int gui_multiwin_dock_ok(int icon){ return gui_multiwin_supported(icon); } /* apps with a draw hook or a ring-3 window */

/* v0.75.0 (batch 2): Mail has real per-keystroke interaction (its list,
   read and compose modes), unlike Files/Weather's static viewers; Reminders
   (1.9.9) and Calendar (1.9.12) left this set when they became ring-3
   programs. gui_run's input loop below only ever forwards a keystroke to
   the app whose window is currently topmost/focused (the same "topmost
   owns input" rule click-to-focus already established for clicks). */
static int gui_multiwin_interactive(int icon){ return icon >= 0 && icon < GUI_APP_COUNT && (APPS[icon].key || gui_ring3_windowed(icon)); } /* apps with a key hook; Files for its 1/2 view-switch keys, Weather for R-to-retry */

/* Window 0 keeps the exact single-window rect the existing dock-app tests
   already assert against (gui_launch_from_dock's own x=70,y=40,w=820,h=385;
   appclose-check.py/app-interact-check.py hard-code CLOSE_X,CLOSE_Y=94,56,
   which is this same rect's close-circle centre, x+24,y+16). A second,
   concurrently-open window is offset so both titlebars and both close
   buttons stay fully on screen and visually distinct, not stacked exactly
   on top of each other. */
static int gui_bleed_fits(void){ return (unsigned)window_width() * (unsigned)window_height() * 4u <= JT_USER_FB_BYTES; } /* a ring-3 window buffer is 2.1 MB (960x540): a bigger screen gets her in an ordinary window instead of a refused open */
static int gui_window_bleed(int icon){ return !boot_to_phone && gui_bleed_fits() && icon >= 0 && icon < GUI_APP_COUNT && APPS[icon].open == samantha_ring3_open; } /* Samantha's window is her face, full bleed (portfolio mode: under the menu bar too); Esc closes it */
static int gui_bleed_open(void){ for (int i = 0; i < gui_window_count; i++) if (gui_window_bleed(gui_windows[i].icon)) return 1; return 0; }
static void gui_multiwin_geom(int slot_index, int *x, int *y, int *w, int *h){
    if (boot_to_phone) { *x = -8; *y = 8; *w = (int)window_width() + 16; *h = (int)window_height(); return; } /* 2.0 gate 5: one window, full screen under the back chevron strip (content rect 0,40,W,H-40) */
    if (slot_index == 0) { *x = 70; *y = 40; *w = 820; *h = 385; }
    else { *x = 70 + 60; *y = 40 + 60; *w = 820; *h = 385; }
    gui_clamp_win_rect(x, y, w, h); /* Mail/Calendar/Reminders/Files/Weather open here, not gui_launch_from_dock -- phone screens need the same clamp */
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

/* v0.76.18: chrome and content split so a keystroke repaints only the
   content viewport, never the whole alpha-blended frame (mwkeyflash-check.sh). */
static void gui_multiwin_draw_chrome(const gui_window_t *win){
    if (gui_window_bleed(win->icon)) return; /* no frame, no traffic lights: his face is the screen */
    serial_puts("mwchrome\n"); /* discriminating marker for tools/checks/mwkeyflash-check.sh */
    int x = win->x, y = win->y, w = win->w, h = win->h;
    gui_draw_window_frame(x, y, w, h, APPS[win->icon].name);
}
static void gui_multiwin_draw_content_only(const gui_window_t *win){
    int x = win->x, y = win->y, w = win->w, h = win->h;
    window_set_viewport(x + 8, y + 32, (unsigned int)(w - 16), (unsigned int)(h - 40));
    /* v0.76.19: gui_draw_app_titlebar skips its own traffic lights only
       under this flag; without it every content redraw drew a second set. */
    gui_app_windowed = 1;
    /* Real per-repaint content, not a cached bitmap: each call re-derives
       the window's content from the same live state its single-window
       counterpart reads (vfs_list for Files, weather_text for Weather),
       so a second window opening never leaves the first one's content
       stale or frozen. */
    if (win->task >= 0) {
        /* 1.9.23: a ring-3 window: blit its private framebuffer through
           window_pixel, the same clipped primitive every in-kernel app
           draws with, at this window's position. The program never
           touches the screen. */
        ring3app_window_blit(win->task, w - 16, h - 40);
    } else if (gui_multiwin_supported(win->icon)) APPS[win->icon].draw();
    gui_app_windowed = 0;
    window_clear_viewport();
}
static void gui_multiwin_draw_one(const gui_window_t *win){
    gui_multiwin_draw_chrome(win);
    gui_multiwin_draw_content_only(win);
}

/* The app registry: the one place an app is wired into the desktop. Its
   index is its identity (dock order, gui_order, icon art slots and the
   window list all key off it), so a new app is one row here plus one
   bump of GUI_APP_COUNT/GUI_APPS_FOLDER/GUI_TRASH above. */
const struct app APPS[GUI_APP_COUNT] = {
    /*  0 */ {"Burrow",     0x00707070, gui_icon_folder,     burrow_ring3_open,     0, 0}, /* ring 3 (user/burrow.c), a compositor window */
    /*  1 */ {"Mail",       0x00A13F3F, gui_icon_mail,       mail_ring3_open,       0, 0}, /* ring 3 (user/mail.c), a compositor window */
    /*  2 */ {"Calendar",   0x00A0553F, gui_icon_calendar,   calendar_ring3_open,   0, 0}, /* 1.9.12: ring 3 (user/calendar.c) */
    /*  3 */ {"Notes",      0x006B4423, gui_icon_notes,      notes_ring3_open,      0, 0}, /* ring 3 (user/notes.c), a compositor window */
    /*  4 */ {"Reminders",  0x00375A4A, gui_icon_reminders,  reminders_ring3_open,  0, 0}, /* 1.9.9: ring 3 (user/reminders.c) */
    /*  5 */ {"Terminal",   0x002B2B2B, gui_icon_terminal,   terminal_ring3_open,   0, 0}, /* ring 3 (user/terminal.c), a compositor window */
    /*  6 */ {"Samantha",   0x00365E8C, gui_icon_chat,       samantha_ring3_open,   0, 0}, /* 1.9.26: ring 3 (user/samantha.c), the last app out of the kernel */
    /*  7 */ {"Weather",    0x0085144B, gui_icon_weather,    weather_ring3_open,    0, 0}, /* 1.9.22: ring 3 (user/weather.c) */
    /*  8 */ {"Curbfind",   0x007A2048, gui_icon_pin,        curbfind_ring3_open,   0, 0}, /* 1.9.11: ring 3 (user/curbfind.c) */
    /*  9 */ {"Keyrate",    0x00B08900, gui_icon_keyrate,    keyrate_ring3_open,    0, 0}, /* 1.7.7: a real ring-3 program (user/keyrate.c), see kernel/ring3app.c */
    /* 10 */ {"Bookrank",   0x002F7B4F, gui_icon_book,       bookrank_ring3_open,   0, 0}, /* 2.0: ring 3 too (user/bookrank.c) */
    /* 11 */ {"Quotes",     0x008B4A9C, gui_icon_quotes,     quotestreak_ring3_open, 0, 0}, /* 1.7.14: ring 3 too (user/quotes.c) */
    /* 12 */ {"Tonchi",     0x00376E5E, gui_icon_tonchi,     tonchi_ring3_open,     0, 0}, /* 1.9.1: ring 3 too (user/tonchi.c) */
    /* 13 */ {"Toroid",     0x00234A78, gui_icon_toroid,     toroid_ring3_open,     0, 0}, /* 1.7.11: ring 3 too (user/toroid.c) */
    /* 14 */ {"Hikko",   0x00A6741E, gui_icon_hikko,   hikko_ring3_open,   0, 0}, /* 1.9.8: ring 3 (user/hikko.c) */
    /* 15 */ {"Fieldbook",  0x005A3E6B, gui_icon_fieldbook,  fieldbook_ring3_open,  0, 0}, /* 1.9.3: ring 3 too (user/fieldbook.c) */
    /* 16 */ {"Contacts",   0x00A87C5B, gui_icon_contacts,   contacts_ring3_open,   0, 0}, /* 1.9.7: ring 3 (user/contacts.c) */
    /* 17 */ {"Calculator", 0x00556B85, gui_icon_calculator, calculator_ring3_open, 0, 0}, /* 1.7.12: ring 3 too (user/calculator.c) */
    /* 18 */ {"Stocks",     0x00356B4F, gui_icon_stocks,     stocks_ring3_open,     0, 0},
    /* 19 */ {"Search",     0x00506078, gui_icon_search,     search_ring3_open,     0, 0}, /* 1.9.13: ring 3 (user/search.c) */
    /* 20 */ {"Epiphany",   0x001F5FA8, gui_icon_stocks,     epiphany_ring3_open,   0, 0}, /* 1.9.17: ring 3 (user/epiphany.c); art covers the icon */
    /* 21 */ {"Portfolio",  0x004A5A3E, gui_icon_apps,       portfolio_ring3_open,  0, 0}, /* no authored art yet, reuses the grid-of-tiles glyph; 1.9.5: ring 3 (user/portfolio.c) */
    /* 22 */ {"Activity",   0x003E4C58, gui_icon_activity,   activity_ring3_open,   0, 0}, /* 1.9.6: ring 3 (user/activity.c) */
    /* 23 */ {"Clock",      0x00565A7A, gui_icon_clock,      clock_ring3_open,      0, 0}, /* live analog face (hands overlay, gui_clock_draw_hands); 1.9.4: ring 3 (user/clock.c) */
    /* 24 */ {"Music",      0x00B5502C, gui_icon_chat,       music_ring3_open,      0, 0}, /* 2.2: ring 3 (user/music.c), Apps folder only like Search */
    /* 25 */ {"Movies",     0x00B5502C, gui_icon_chat,       movies_ring3_open,     0, 0}, /* 2.2: ring 3 (user/movies.c), Apps folder only; authored art (art/icons/movies.svg) covers the icon */
    /* 26 */ {"Hamurapi",   0x00B5502C, gui_icon_chat,       hamurabi_ring3_open,   0, 0}, /* 2.7: ring 3 (user/hamurabi.c; the game is shown as Hamurapi, the store name Hamurabi was taken), Apps folder only; authored art (art/icons/hamurabi.svg) covers the icon */
    /* 27 */ {"Windgate",   0x000B1420, gui_icon_chat,       windgate_ring3_open,   0, 0}, /* 2.8: guided breathing, ring 3 (user/windgate.c), Apps folder only; authored art (art/icons/windgate.svg) covers the icon */
    /* 28 */ {"Panes",      0x00F5F5F8, gui_icon_chat,       panes_ring3_open,      0, 0}, /* 2.11: cmux-style tabs and split panes sharing the Terminal's shell engine, ring 3 (user/panes.c), Apps folder only; authored art (art/icons/panes.svg) covers the icon */
    /* 29 */ {"Claude",     0x00B5502C, gui_icon_chat,       claude_ring3_open,     0, 0}, /* 2.14: Claude Code through the relay (user/claude.c, tools/claude-relay/relay.py), ring 3, Apps folder only; authored art (art/icons/claude.svg) covers the icon */
    /* 30 */ {"Mines",      0x00556B85, gui_icon_apps,       mines_ring3_open,      0, 0}, /* Minesweeper, ring 3 (user/mines.c), Apps folder only; authored art is art/icons/mines.svg, a cream mine on the accent */
    /* Apps and Trash aren't real apps with their own brand color, so their
       tile renders at the tray's own tone (DOCK_TRAY_COLOR) instead of a
       tinted background like every real app above. 2026-09-27: this used
       to read GUI_COLORS[icon] out of bounds (that array only had 25 real
       entries, never Apps/Trash's own), which happened to land on nearby
       static data close enough to pass by luck; a plain 0 here instead
       renders black and the trash can's punched-through ribs show as
       black cuts against the body (iconedge-check.py's "shadow-rib
       stubs" failure). DOCK_TRAY_COLOR is the real, intended value. */
    [GUI_APPS_FOLDER] = {"Apps",  DOCK_TRAY_COLOR, gui_icon_apps,  gui_launch_apps,  0, 0},
    [GUI_TRASH]       = {"Trash", DOCK_TRAY_COLOR, gui_icon_trash, gui_launch_trash, 0, 0},
};

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

/* App switcher: Alt+Tab (Ctrl+Tab too, since QEMU/v86's Alt delivery to a
   guest kernel is the flaky one to bet on) cycles the open windows, the
   same macOS Cmd+Tab shape -- hold the modifier and tap Tab, a small
   centered panel lists every open window with the next one highlighted;
   releasing the modifier focuses it. Walks whatever's really open
   (gui_window_count, capped at GUI_MULTIWIN_MAX like the window list
   itself already is), never a hard-coded count. */
static int gui_switcher_open = 0;
static int gui_switcher_idx = 0;

static void gui_switcher_draw(int hi){
    int rows = gui_window_count;
    if (rows <= 0) return;
    int row_h = 28, pad_v = 10, w = 220;
    int h = pad_v * 2 + rows * row_h;
    int x = ((int)window_width() - w) / 2, y = ((int)window_height() - h) / 2;
    unsigned int bg = 0x002C2C2E, text = 0x00F5F5F7; /* same flyout colors the Apple menu/notif/weather panels already share */
    gui_rounded_rect_on_wallpaper(x, y, w, h, bg, 14);
    int ry = y + pad_v;
    for (int i = 0; i < rows; i++) {
        if (i == hi) window_rect(x + 6, ry, w - 12, row_h - 4, 0x00555555);
        int icon = gui_windows[i].icon;
        const char *label = (icon >= 0 && icon < GUI_APP_COUNT) ? APPS[icon].name : "?";
        font_draw_string(label, x + 18, ry + 6, text, -1);
        ry += row_h;
    }
    window_present();
}

/* Screenshot: Ctrl+Shift+3, the reliable one -- real PrintScreen sends an
   E0-prefixed 4-byte make sequence (E0 2A E0 37) that shares its 0xE0
   lead byte with every arrow key's own extended sequence, which the
   multiwin key readers just above are already mid-decoding whenever a
   window with arrow-key input (Files, Calendar) is open; peeking for it
   here risked eating a real arrow keystroke, so this ships the one
   hotkey that can't collide. Saves the live framebuffer as an
   uncompressed 24-bit BMP (no PNG encoder exists in this tree, drivers/
   png.c only decodes) named SHOT0001.BMP, SHOT0002.BMP, ... in the files
   root, so it shows up in Files like any other saved file. */
static void gui_write_u32le(unsigned char *p, unsigned int v){ p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF; }
static void gui_write_u16le(unsigned char *p, unsigned int v){ p[0]=v&0xFF; p[1]=(v>>8)&0xFF; }

static int gui_screenshot_next_name(char *out /* 13 bytes, "SHOTNNNN.BMP\0" */){
    for (int n = 1; n <= 9999; n++) {
        char name[13];
        int i = 0; name[i++]='S'; name[i++]='H'; name[i++]='O'; name[i++]='T';
        name[i++]='0'+(n/1000)%10; name[i++]='0'+(n/100)%10; name[i++]='0'+(n/10)%10; name[i++]='0'+n%10;
        name[i++]='.'; name[i++]='B'; name[i++]='M'; name[i++]='P'; name[i]=0;
        unsigned char probe[1];
        if (vfs_read_file(name, probe, 1) < 0) { for (int j = 0; j <= i; j++) out[j] = name[j]; return 1; }
    }
    return 0;
}

/* Returns the real byte count written (54-byte header + padded pixel
   rows) on success, 0 on failure -- a headless check can grep the exact
   figure straight out of the serial marker below instead of having to
   parse the FAT image itself to prove the file's real size. */
static unsigned int gui_screenshot_save(char *name_out /* 13 bytes */){
    int sc = (int)window_scale(); if (sc < 1) sc = 1;
    int w = (int)window_width() * sc, h = (int)window_height() * sc;
    if (w <= 0 || h <= 0) return 0;
    unsigned int row_bytes = (unsigned int)w * 3;
    unsigned int pad = (4 - (row_bytes % 4)) % 4;
    unsigned int data_size = (row_bytes + pad) * (unsigned int)h;
    unsigned int file_size = 54 + data_size;
    unsigned char *buf = (unsigned char *)kmalloc(file_size);
    if (!buf) return 0;
    buf[0]='B'; buf[1]='M';
    gui_write_u32le(buf + 2, file_size);
    gui_write_u32le(buf + 6, 0);
    gui_write_u32le(buf + 10, 54);
    gui_write_u32le(buf + 14, 40);
    gui_write_u32le(buf + 18, (unsigned int)w);
    gui_write_u32le(buf + 22, (unsigned int)h); /* positive height: bottom-up rows, standard BMP */
    gui_write_u16le(buf + 26, 1);
    gui_write_u16le(buf + 28, 24);
    gui_write_u32le(buf + 30, 0);
    gui_write_u32le(buf + 34, data_size);
    gui_write_u32le(buf + 38, 2835);
    gui_write_u32le(buf + 42, 2835);
    gui_write_u32le(buf + 46, 0);
    gui_write_u32le(buf + 50, 0);
    unsigned char *px = buf + 54;
    for (int y = 0; y < h; y++) {
        int src_y = h - 1 - y; /* bottom-up */
        unsigned char *row = px + (unsigned int)y * (row_bytes + pad);
        for (int x = 0; x < w; x++) {
            unsigned int c = window_get_pixel_phys(x, src_y);
            row[x*3+0] = (unsigned char)(c & 0xFF);
            row[x*3+1] = (unsigned char)((c >> 8) & 0xFF);
            row[x*3+2] = (unsigned char)((c >> 16) & 0xFF);
        }
        for (unsigned int p = 0; p < pad; p++) row[row_bytes + p] = 0;
    }
    char name[13];
    int ok = gui_screenshot_next_name(name);
    int saved = ok && vfs_write_file(name, buf, file_size) == 1; /* vfs_write_file (drivers/vfs.h), not fat_write_file directly, so this lands in whichever backend is actually active -- the FAT disk when one's attached, the RAM fallback (ramfs.c) in every headless/no-disk boot -- the same choice Files/Notes already make instead of hard-wiring FAT. Returns exactly 1 on success, 0 on failure, never a byte count. */
    kfree(buf);
    if (saved) { for (int j = 0; j < 13; j++) name_out[j] = name[j]; }
    return saved ? file_size : 0;
}

/* A brief centered confirmation banner, same flyout colors as the
   switcher panel above, up for ~0.6s (60 PIT ticks) then erased by the
   next real desktop repaint -- the same "flash" every other save
   confirmation in this kernel (editor.h's status line) already does,
   just as its own overlay instead of a status line, since the desktop
   itself has none. */
static void gui_screenshot_flash(const char *name){
    char msg[24]; int i = 0; const char *s = "Saved "; while (*s) msg[i++] = *s++;
    s = name; while (*s && i < 23) msg[i++] = *s++;
    msg[i] = 0;
    int tw = font_string_width(msg);
    int w = tw + 40, h = 40;
    int x = ((int)window_width() - w) / 2, y = 70;
    gui_rounded_rect_on_wallpaper(x, y, w, h, 0x002C2C2E, 14);
    font_draw_string(msg, x + 20, y + 12, 0x00F5F5F7, -1);
    window_present();
    unsigned int until = ticks() + 60;
    while ((int)(ticks() - until) < 0) __asm__ volatile ("hlt");
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
    gui_windows[slot].task = -1;
    gui_windows[slot].shown = 0;
    gui_multiwin_geom(slot, &gui_windows[slot].x, &gui_windows[slot].y, &gui_windows[slot].w, &gui_windows[slot].h);
    if (gui_window_bleed(icon)) { gui_windows[slot].x = -8; gui_windows[slot].y = -32; gui_windows[slot].w = (int)window_width() + 16; gui_windows[slot].h = (int)window_height() + 40; } /* content rect = the whole screen */
    if (gui_ring3_windowed(icon)) {
        /* 1.9.23: the program is scheduled now and draws into its own
           buffer; this loop keeps running. -1 is refused by the caller
           (gui_refuse_open), never a blocking takeover. */
        if (APPS[icon].open == notes_ring3_open) notes_migrate_legacy(); /* the window path never calls APPS[].open, which is where the one-time NOTES.TXT move into the default folder lives (editor.h) */
        int t = ring3app_launch_window(APPS[icon].name, (unsigned int)(gui_windows[slot].w - 16), (unsigned int)(gui_windows[slot].h - 40));
        if (t < 0) return -1;
        gui_windows[slot].task = t;
    }
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
    int bleed = gui_window_bleed(gui_windows[idx].icon); for (int j = idx; j < gui_window_count - 1; j++) gui_windows[j] = gui_windows[j + 1]; gui_window_count--;
    if (bleed) { gui_menubar_force_redraw(); gui_draw_menubar(); } /* the bar was held back while his face covered it */
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
    /* 2.0.0: the selection keys (Shift+arrow, Ctrl+A) go to a ring-3 window only; an in-kernel app keeps plain arrows */
    int sel_ok = gui_window_count > 0 && gui_windows[gui_window_count - 1].task >= 0;
    if (sc == 0xE0) {
        int sc2 = kbd_pop();
        if (sc2 < 0) return -1; int sh = sel_ok && kbd_shift; /* shift+arrow extends a selection only in a ring-3 window */
        if (sc2 == 0x48) return sh ? KEY_SUP : KEY_UP;    if (sc2 == 0x50) return sh ? KEY_SDOWN : KEY_DOWN;
        if (sel_ok && kbd_ctrl && gui_windows[gui_window_count - 1].icon == GUI_APP_PANES) { if (sc2 == 0x4B) return KEY_CTL_LEFT; if (sc2 == 0x4D) return KEY_CTL_RIGHT; }
        if (sc2 == 0x4B) return sh ? KEY_SLEFT : KEY_LEFT; if (sc2 == 0x4D) return sh ? KEY_SRIGHT : KEY_RIGHT;
        if (sc2 == 0x47) return KEY_HOME;
        if (sc2 == 0x4F) return KEY_END;
        if (sc2 == 0x53) return KEY_DELETE;
        return -1;
    }
    if (sc == 0xBC) return KEY_F2_UP;
    if (sc & 0x80) return -1; /* key release */
    if (sc == 0x3C) return KEY_F2;
    if (kbd_ctrl && (sc & 0x7F) == 0x1F) return KEY_SAVE;
    if (kbd_ctrl) { int k = sc & 0x7F; if (k == 0x2E) return KEY_COPY; if (k == 0x2D) return KEY_CUT; if (k == 0x2F) return KEY_PASTE; if (sel_ok && k == 0x1E) return KEY_SELALL; if (sel_ok && gui_windows[gui_window_count - 1].icon == GUI_APP_PANES && key_ctl_code(k)) return key_ctl_code(k); } /* clipboard and select-all keys reach ring-3 windows too */
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
/* The real landing brand mark (landing/logo.svg), not the old stick-figure
   gui_draw_logo primitive. boot_mark.h (tools/gen/gen_boot_mark.py) carries
   its rasterized 8-bit coverage; blended at PHYSICAL resolution via
   window_pixel_phys, the same pattern gui_aa_char uses for text. cx,cy are
   LOGICAL center coords, converted to physical here. */
static void gui_blend_cov(const unsigned char *cov, int cw, int ch, int cx, int cy, int size, unsigned int ink){
    int sc = window_has_target() ? 1 : (int)window_scale(); if (sc < 1) sc = 1;
    int T = size * sc; /* box-filtered from the stored coverage; T == the stored size is a straight copy */
    int ox = cx * sc - T / 2, oy = cy * sc - T / 2;
    for (int row = 0; row < T; row++){
        for (int col = 0; col < T; col++){
            int c0 = col * cw / T, c1 = (col + 1) * cw / T, r0 = row * ch / T, r1 = (row + 1) * ch / T;
            if (c1 == c0) c1 = c0 + 1; /* drawn bigger than stored: take the nearest source pixel instead of none */
            if (r1 == r0) r1 = r0 + 1;
            int sum = 0, n = (c1 - c0) * (r1 - r0);
            for (int yy = r0; yy < r1; yy++) for (int xx = c0; xx < c1; xx++) sum += cov[yy * cw + xx];
            int a = sum / n;
            if (!a) continue;
            int x = ox + col, y = oy + row;
            unsigned int d = window_get_pixel_phys(x, y);
            unsigned int r = (((ink >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * (255 - a)) / 255;
            unsigned int g = (((ink >> 8) & 0xFF) * a + ((d >> 8) & 0xFF) * (255 - a)) / 255;
            unsigned int b = ((ink & 0xFF) * a + (d & 0xFF) * (255 - a)) / 255;
            window_pixel_phys(x, y, (r << 16) | (g << 8) | b);
        }
    }
}
/* 2.25: small sizes (menu bar, About) take the bold copy, the tree drawn down its middle; the splash's thin
   one-line copy would fade to a grey smudge at 20 px. */
static void gui_draw_mark_sized(int cx, int cy, int size, unsigned int ink){
    if (size <= 48) gui_blend_cov(menu_mark_cov, MENU_MARK_W, MENU_MARK_H, cx, cy, size, ink);
    else gui_blend_cov(boot_mark_cov, BOOT_MARK_W, BOOT_MARK_H, cx, cy, size, ink);
}
static void gui_draw_boot_mark(int cx, int cy, unsigned int ink){
    int sc = window_has_target() ? 1 : (int)window_scale(); if (sc < 1) sc = 1;
    gui_draw_mark_sized(cx, cy, BOOT_MARK_W / sc, ink);
}

static void gui_draw_boot_screen(void){
    unsigned int bg = 0x00000000; /* pure black boot background, direct request */
    window_clear(bg);
    int cx = (int)window_width() / 2, cy = (int)window_height() / 2; /* v45.2: centred on the real window; 400 was the 800-wide centre and sat left of centre at 960 */
    gui_draw_boot_mark(cx, cy - 10, 0x00FFFFFF); /* v1.6.20: the real landing/logo.svg brand mark, replacing the old stick-tree gui_draw_logo primitive here -- white ink to match the plain-black boot screen, same as the primitive it replaces */

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
    n += app_utoa(free_kb, buf + n);
    buf[n++] = 'K'; buf[n++] = ' '; buf[n++] = 'f'; buf[n++] = 'r'; buf[n++] = 'e'; buf[n++] = 'e'; buf[n++] = ' '; buf[n++] = 'o'; buf[n++] = 'f'; buf[n++] = ' ';
    n += app_utoa(total_kb, buf + n);
    buf[n++] = 'K'; buf[n] = 0;
    char mem_line[64]; { int p = 0; const char *s = buf; while (*s) mem_line[p++] = *s++; mem_line[p] = 0; }

    /* v0.76.12: real bug, this line was hardcoded to "Version 0.42.1"
       for 30+ real version bumps despite JT_VERSION_STR (drivers/version.h,
       generated from the real VERSION file at build time) already
       existing and already used elsewhere (the boot serial log). */
    char version_line[32]; { int p = 0; const char *v = "Version " JT_VERSION_STR; while (*v && p < (int)sizeof(version_line) - 1) version_line[p++] = *v++; version_line[p] = 0; }

    const char *tagline = "A freestanding i386 kernel, written from scratch.";
    const char *lines[3] = { tagline, mem_line, version_line };
    unsigned int colors[3] = { 0x001C1C1E, 0x00884B16, 0x0075726E };
    int line_h = 24;
    gui_draw_mark_sized(bx + ABOUT_W / 2, by + 56, 32, 0x001C1C1E); /* 2.25: the mark heads the card, as the logo does on a Mac's About box */
    int text_top = by + (ABOUT_H - 3 * line_h) / 2 + 22; /* the text block sits under the mark */
    for (int i = 0; i < 3; i++) {
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
    "About Joshua Tree", "Burrow", "Notes", "Settings", "Lock Screen", "-", "Restart", "Shut Down"
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
    else if (item == 1) gui_launch_from_dock(0);
    else if (item == 2) gui_launch_from_dock(3);
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
#include "phone_home.h" /* v1.8.0: phone mode's real home screen, see its own header comment */
static void gui_run(void){
    /* v42: 16:9, 960x540 logical at 2x = 1920x1080 physical, the native
       size of the monitor this actually runs fullscreen on. QEMU's cocoa
       zoom-to-fit stretches without preserving aspect, so a 4:3 mode on a
       16:9 panel came out visibly skewed; matching the panel's own shape
       means fullscreen is pixel-exact with no scaling at all. */
    /* "phone" boots a real portrait phone mode at scale 1 instead: Bochs
       VBE takes any size, and 430x760 is a real phone's own logical
       pixels, not the desktop's 960x540 shrunk to fit. */
    if (boot_to_phone) { if (!window_open_scaled(430, 760, 32, 2)) { puts("no VGA device found or out of page tables\n"); return; } }
    else if (!(boot_res_w && window_open_scaled(boot_res_w / 2, boot_res_h / 2, 32, 2)) && !window_open_scaled(960, 540, 32, 2)) { puts("no VGA device found or out of page tables\n"); return; } /* res= (gui_parse_res) first, else the default */
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
    if (boot_to_samantha) { boot_to_samantha = 0; serial_puts("samopen\n"); if (boot_to_phone) { gui_app_windowed = 0; phone_window_run(6); } else gui_launch_from_dock(6); } /* 1.9.26: ring-3 Samantha is the first screen; phone mode lands on the home grid when she closes */
    else if (!boot_to_phone) serial_puts("guidesktop\n"); /* discriminating marker for tools/checks/samantha-boot-check.py: the icon desktop drew first, samantha mode never reaches here before her avatar; phone mode never draws this desktop at all (see below), so it must not claim it did */
    if (boot_to_phone) { phone_home_run(); return; } /* v1.8.0: leaving Samantha lands on a real home screen, not the desktop's dock squeezed into 430px; never returns */
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
    gui_dock_prewarm(); ring3app_autoopen_run(mx, my); /* `open=keyrate` / `open=toroid` boot flag, if set */
    for (;;) {
        window_present(); r3stress_desktop_round(); pdestress_desktop_round(); __asm__ volatile ("hlt"); /* 1.9.23: stress=r3 hook, dead unless armed */
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
        /* 1.9.26: SYS_LAUNCH_REQUEST pickup. The syscall only stored an index; the launch runs here,
           IF on, through the same two paths a dock click takes (window first, blocking fallback). */
        int sys_launched = 0;
        { int ra = 0, rk = jt_refresh_take(&ra); /* SYS_REFRESH pickup: fetch outside the gate, the app sees data_stamp move */
          if (rk == JT_REFRESH_WEATHER) { weather_tried_once = 1; weather_fetch(); gui_menubar_force_redraw(); }
          else if (rk == JT_REFRESH_STOCKS) { stx_range_hint = ra & 0xFF; stx_sel_hint = (ra >> 8) & 0xFF; stocks_fetch(stx_range_hint); } }
        { int li = jt_launch_take();
          if (li >= 0) {
              serial_puts("launchreq=pickup\n");
              if (gui_multiwin_dock_ok(li)) { if (gui_multiwin_open(li) < 0) gui_refuse_open(li); }
              else { gui_launch_from_dock(li); mx = app_cursor_x; my = app_cursor_y; }
              for (int wi = 0; wi < gui_window_count; wi++) gui_windows[wi].shown = 1;
              sys_launched = 1;
          } }
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
        int r3_dirty_top = 0; /* 1.9.23: the focused ring-3 window presented a frame; cheap content-only blit below */
        for (int i = 0; i < gui_window_count; i++) {
            if (!gui_windows[i].shown) { gui_windows[i].shown = 1; mw_key_repaint = 1; }
            if (gui_windows[i].task < 0) continue;
            if (!task_used(gui_windows[i].task)) {
                /* The program exited or was reaped (idt.c): close only its
                   window. Everything else on screen stays. */
                ring3app_window_reaped(gui_windows[i].task, task_last_exit_code());
                gui_multiwin_close(i); i--; mw_key_repaint = 1;
                continue;
            }
            unsigned int fw, fh; int dirty = 0;
            syscall_window_fb(gui_windows[i].task, &fw, &fh, &dirty);
            if (dirty) { if (i == gui_window_count - 1) r3_dirty_top = 1; else mw_key_repaint = 1; }
        }
        /* App switcher hotkey, checked before the per-app dispatch just
           below gets its own single kbd_pop() this frame. kbd_peek()
           (irq.c) only tells us what's next without eating it, so a plain
           Alt+Tab with one or zero windows open -- or any other scancode
           entirely -- falls straight through untouched. Only a real
           Tab-make while Alt/Ctrl is down, or the matching modifier
           release while the panel is open, is ever actually popped here. */
        if (gui_window_count > 1) {
            int sw_pk = kbd_peek();
            if (sw_pk == 0x0F && (kbd_alt || kbd_ctrl)) {
                kbd_pop();
                if (!gui_switcher_open) { gui_switcher_open = 1; gui_switcher_idx = gui_window_count - 1; }
                gui_switcher_idx = (gui_switcher_idx + 1) % gui_window_count;
                gui_switcher_draw(gui_switcher_idx);
                serial_puts("SWITCHER:tab\n");
            } else if (gui_switcher_open && (sw_pk == 0xB8 || sw_pk == 0x9D)) {
                kbd_pop();
                gui_switcher_open = 0;
                gui_multiwin_focus(gui_switcher_idx);
                mw_key_repaint = 1; /* the panel is gone and the z-order changed: needs the real full repaint, same as any other focus/close change */
                serial_puts("SWITCHER:focus\n");
            }
        }
        /* Screenshot: Ctrl+Shift+3. Same peek-first discipline as the
           switcher above -- '3' is only ever consumed here when both
           modifiers are already down, which real typing never does, so a
           lone '3' keystroke elsewhere on the desktop is untouched. */
        if (kbd_ctrl && kbd_shift) {
            int ss_pk = kbd_peek();
            if (ss_pk == 0x04) {
                kbd_pop();
                char shot_name[13];
                unsigned int shot_bytes = gui_screenshot_save(shot_name);
                if (shot_bytes) {
                    char nbuf[12]; int ni = 0; unsigned int v = shot_bytes;
                    do { nbuf[ni++] = (char)('0' + v % 10); v /= 10; } while (v);
                    serial_puts("SHOT:");
                    serial_puts(shot_name);
                    serial_puts(":");
                    while (ni) { char d[2] = { nbuf[--ni], 0 }; serial_puts(d); }
                    serial_puts("\n");
                    gui_screenshot_flash(shot_name);
                    mw_key_repaint = 1; /* the flash banner drew over the desktop; needs a real repaint to erase it */
                }
            }
        }
        /* Text shell: Ctrl+Alt+Backspace, the deliberate way off the
           desktop now that a bare Esc there is a no-op. */
        if (kbd_ctrl && kbd_alt) {
            int shell_pk = kbd_peek();
            if (shell_pk == 0x0E) { kbd_pop(); break; }
        }
        if (gui_multiwin_interactive(mw_topmost_icon)) {
            int mwk = gui_multiwin_key_nonblock();
            if (mwk >= 0 && gui_windows[gui_window_count - 1].task >= 0) {
                /* 1.9.23: the focused window is a ring-3 program: the key
                   goes into its event ring and nowhere else. It redraws
                   through JT_POLL_PRESENT, which the dirty pass below picks up. */
                syscall_window_push_event(gui_windows[gui_window_count - 1].task, JT_EV_KEY, mwk, 0);
            } else if (mwk >= 0) {
                int mw_should_close = 0, mw_count_before_key = gui_window_count;
                mw_should_close = APPS[mw_topmost_icon].key(mwk);
                if (mw_should_close) {
                    gui_multiwin_close(gui_window_count - 1);
                    mw_key_repaint = 1; /* the window left the screen: needs the real full desktop repaint to erase it, the same cost every open/close already pays */
                } else if (gui_window_count != mw_count_before_key) {
                    mw_key_repaint = 1; /* v1.9.0: opened another window as a side effect (Mail's 'c'); its chrome was never drawn, needs a full repaint like any window-count change */
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
                    /* Esc closes the window; a no-op on a bare desktop (Ctrl+Alt+Backspace reaches the shell). */
                    if (gui_window_count == 0) { /* no-op */ }
                    else { gui_multiwin_close(gui_window_count - 1); mw_key_repaint = 1; }
                } else if (c == '\n' && gui_window_count == 0) {
                    gui_launch_apps(); /* direct, not gui_launch_from_dock's boxed frame (moves the a11y close pixel) */
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
        int bleed_now = gui_bleed_open(); /* a full-screen window hides the menu bar and dock, so their hot spots go dead too: its red dot sits where the logo was */
        int logo_here = !bleed_now && !menu_open && !notif_open && !weather_open && mx >= 4 && mx <= 28 && my < GUI_MENUBAR_H;
        int clock_here = !bleed_now && !menu_open && !notif_open && !weather_open && mx >= (int)window_width() - 200 && my < GUI_MENUBAR_H;
        int weather_here = !bleed_now && !menu_open && !notif_open && !weather_open && weather_hit_x0 >= 0 && mx >= weather_hit_x0 && mx <= weather_hit_x1 && my < GUI_MENUBAR_H;
        int slot_here = (bleed_now || menu_open || notif_open || weather_open) ? -1 : gui_dock_hit_test(mx, my); /* the dock is inert while a panel covers it */
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

        int launched = sys_launched || notif_draw_pending || weather_draw_pending || win_focus_changed || mw_key_repaint; notif_draw_pending = 0; weather_draw_pending = 0;
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
            } else if (press_window >= 0 && gui_windows[press_window].task >= 0
                       && !(mx >= gui_windows[press_window].x + 16 && mx <= gui_windows[press_window].x + 32
                            && my >= gui_windows[press_window].y + 8 && my <= gui_windows[press_window].y + 24)) {
                /* 1.9.23: a click inside a ring-3 window's content is the
                   program's, in viewport coordinates; only its red dot closes
                   it (task_kill, reaped on its next turn, window closed below). */
                syscall_window_push_event(gui_windows[press_window].task, JT_EV_CLICK,
                                          mx - (gui_windows[press_window].x + 8), my - (gui_windows[press_window].y + 32));
            } else if (press_window >= 0 && gui_windows[press_window].task >= 0) {
                task_kill(gui_windows[press_window].task);
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
            } else if (press_slot >= 0 && press_slot == slot_here && gui_multiwin_dock_ok(gui_order[press_slot])) {
                /* v0.73.0: real phase-1 multi-window path for Files/Weather,
                   see the big comment above gui_multiwin_open. Non-blocking
                   on purpose: adds/focuses the window in the real list and
                   returns immediately, so this same gui_run loop keeps
                   running (dock hover, the other open window's redraw,
                   everything) instead of blocking inside gui_wait_close the
                   way every other app still does. */
                editor_mouse_x = mx; editor_mouse_y = my;
                /* 2.0 gate 5: at the GUI_MULTIWIN_MAX cap (or a failed window launch) the click is an
                   honest refusal, "winrefuse" on serial and a notice on screen, never a blocking takeover. */
                if (gui_multiwin_open(gui_order[press_slot]) < 0) gui_refuse_open(gui_order[press_slot]);
                /* the launched=1 full repaint below draws this window's chrome; do not let the first-frame check draw it again */
                for (int wi = 0; wi < gui_window_count; wi++) gui_windows[wi].shown = 1;
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
        /* Live move (direct request, "app windows should be draggable"):
           the window itself follows the pointer now, redrawn from its own
           state at the new x/y on every pointer move, with the snap
           outline still drawn on top when a zone is in reach. The clamp
           is the same one the release path applies, so what the visitor
           sees mid-drag is exactly where the window lands. */
        int drag_moved = drag_win >= 0 && !launched && (mx != last_mx || my != last_my);
        if (drag_moved) {
            gui_window_t *dw = &gui_windows[drag_win];
            int top, bottom; int sw = gui_snap_area(&top, &bottom);
            int nx = mx - drag_grab_dx, ny = my - drag_grab_dy;
            if (nx < 0) nx = 0; if (nx + dw->w > sw) nx = sw - dw->w;
            if (ny < top) ny = top; if (ny + dw->h > bottom) ny = bottom - dw->h;
            dw->x = nx; dw->y = ny;
        }
        int drag_zone_only = drag_win >= 0 && !launched && (cur_snap_zone != drag_zone || drag_moved);
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
            if (gui_notice_name) gui_notice_draw();
            if (drag_slot < 0) { gui_cursor_save(mx, my); gui_draw_cursor(mx, my); }
            last_mx = mx; last_my = my; last_hover = hover_slot; last_drag = drag_slot;
            last_menu_open = menu_open; last_menu_hover = menu_hover;
        }
        else if (r3_dirty_top) {
            /* 1.9.23: the focused ring-3 window has a new frame: blit just
               its content, the same cheap tier a Mail keystroke takes. */
            gui_cursor_restore();
            gui_multiwin_draw_content_only(&gui_windows[gui_window_count - 1]);
            gui_cursor_save(mx, my);
            gui_draw_cursor(mx, my);
        }
    }
    window_close();
    /* v77: free GUI heap allocations (wind_base, dock_band cache/frame) when
       exiting so the heap is available for other uses after gui_run() returns.
       This is critical for heap growth after the GUI: without this, the large
       allocations consume virtual address space and prevent paging_map_region
       from mapping new heap frames beyond the base map. */
    wall_caches_drop();
    gui_dock_band_cache_free();
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
    if (ring3app_write_packed("HELLO.BIN", user_hello, USER_HELLO_CLEN, USER_HELLO_LEN, USER_HELLO_SUM, 0) < 0) {
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
    if (ring3app_write_packed("NOTE.BIN", user_note, USER_NOTE_CLEN, USER_NOTE_LEN, USER_NOTE_SUM, 1) < 0) {
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
    if (!strcmp(line, "help"))       { puts("help clear echo time uptime dmesg mem reboot crash pagefault bench heaptest heapgrow tasktest fputest preempttest weathertest daynighttest maptinttest walltest weatherfxtest weatherfxcliptest geotest weatherpaneltest windweathertest cursortest texttest wraptest mailtest dockstyletest wind isotest reaptest ring3test usertest notetest filetest ps kill killtest sleep disktest diskuse fsuse ls cat exec rm cd mkdir write browse lspci gfxtest fonttest mousetest nettest ifconfig netscan web serve serveapp chat build gui testapps calctest pngtest jpegtest chattest beep say listen\n");
                                        puts("a name that isn't one of the above runs a program by that name too, e.g. \"hello\" or \"note buy milk\" (same as exec, case-insensitive)\n"); }
    else if (!strcmp(line, "clear")) clear();
    else if (!strcmp(line, "echo"))  { puts(arg); putc('\n'); }
    else if (!strcmp(line, "crash")) __asm__ volatile ("int $3");  /* manual check: exercises idt/isr */
    else if (!strcmp(line, "pagefault")) { volatile int *p = (int *)0xDEAD0000; *p = 1; } /* manual check: exercises paging */
    else if (!strcmp(line, "bench"))     { bench_run(fs_ok_global); }
    else if (!strcmp(line, "beep"))      puts(sb16_beep(440, 500) ? "beep: played 440Hz\n" : "beep: no sound card\n");
    else if (!strcmp(line, "listen")) {  /* headless test hook for tools/checks/sb16-record-check.py: records 2s at 16kHz, reports bytes captured */
        unsigned int cap = 16000u * 2u;
        unsigned char *buf = kmalloc(cap);
        if (!buf) puts("listen: out of memory\n");
        else {
            int n = sb16_record(buf, cap, 16000u);
            char msg[48]; int p = 0;
            const char *h = "listen: n="; while (*h) msg[p++] = *h++;
            unsigned int v = n < 0 ? 0 : (unsigned int)n;
            unsigned int digs[12]; int dn = 0;
            if (v == 0) msg[p++] = '0';
            while (v) { digs[dn++] = v % 10; v /= 10; }
            while (dn) msg[p++] = (char)('0' + digs[--dn]);
            msg[p++] = '\n'; msg[p] = 0;
            puts(msg);
            kfree(buf);
        }
    }
    else if (!strcmp(line, "say"))       puts(!*arg ? "usage: say <text>\n" : !net_init(0x0A00020F) ? "say: no NIC\n"
                                              : speak_text(llm_host, (unsigned short)llm_port, arg, 1500 /* ~15s at 100Hz */) ? "say: played\n" : "say: nothing played\n");
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
    else if (!strcmp(line, "fonts")) {
        /* 2.27.0 font registry: every embedded face, by name. */
        for (int i = 0; i < TTF_FACE_COUNT; i++) {
            puts(ttf_face_name((ttf_face_t)i));
            puts(ttf_face_is_mono((ttf_face_t)i) ? "  (mono)\n" : "\n");
        }
    }
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
    else if (!strcmp(line, "fputest")) { fpu_bad = fpu_done = 0; task_create(fpu_a); task_create(fpu_b); for (int i = 0; i < 2000 && fpu_done < 2; i++) yield();
        puts(fpu_done == 2 && !fpu_bad ? "fputest: x87 state kept per task: ok\n" : fpu_done == 2 ? "fputest: FAIL x87 values crossed between tasks\n" : "fputest: FAIL tasks did not finish\n"); }
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
           check" idiom the ring-3 apps' own marker lines use. */
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
    else if (!strcmp(line, "loctest")) {
        /* v0.85.5: Settings Location, headless and deterministic --
           tools/checks/location-check.py boots this against a local fake
           geocoding-api.open-meteo.com server (same wxhost= override
           weather-app-check.sh's mock already uses for ip-api/Open-Meteo),
           never real internet, so this never flakes on CI's own network.
           Three real things proven: one, a real geocode through
           loc_geocode lands in the loc and geo globals exactly the way a
           Settings save would; two, settings_save and settings_load
           round-trip it through SETTINGS.TXT, the same file wind/dock/wall
           already prove elsewhere, simulating a reboot the same way the
           shell's own mail round trip test above simulates one for
           MAIL.TXT; three, an unresolvable query fails cleanly (loc_err
           set, nothing overwritten, no crash) rather than fabricating a
           coordinate. */
        serial_puts("loctest start\n");
        char save_name[LOC_NAME_MAX]; int si=0; while (loc_name[si]) { save_name[si]=loc_name[si]; si++; } save_name[si]=0;
        int ok1 = loc_geocode("Langley");
        int ok1b = ok1 && loc_have && loc_lat[0] && loc_lon[0] && loc_name[0]
                   && !strcmp(geo_lat, loc_lat) && !strcmp(geo_lon, loc_lon) && !strcmp(geo_city, loc_name) && geo_have;
        serial_puts(ok1b ? "loc geocode: real lookup lands in loc_*/geo_*: ok\n" : "loc geocode: FAILED\n");

        char want_name[LOC_NAME_MAX], want_lat[16], want_lon[16];
        { int i=0; while (loc_name[i]) { want_name[i]=loc_name[i]; i++; } want_name[i]=0; }
        { int i=0; while (loc_lat[i]) { want_lat[i]=loc_lat[i]; i++; } want_lat[i]=0; }
        { int i=0; while (loc_lon[i]) { want_lon[i]=loc_lon[i]; i++; } want_lon[i]=0; }
        settings_save();
        loc_have = 0; loc_name[0] = 0; loc_lat[0] = 0; loc_lon[0] = 0;
        geo_have = 0; geo_lat[0] = 0; geo_lon[0] = 0; geo_city[0] = 0;
        settings_load();
        int ok2 = loc_have && !strcmp(loc_name, want_name) && !strcmp(loc_lat, want_lat) && !strcmp(loc_lon, want_lon)
                  && geo_have && !strcmp(geo_lat, want_lat) && !strcmp(geo_lon, want_lon) && !strcmp(geo_city, want_name);
        serial_puts(ok2 ? "loc persist: settings_save/settings_load round trip (simulated reboot): ok\n" : "loc persist: FAILED\n");

        loc_err[0] = 0;
        int ok3 = !loc_geocode("Nowhereville") && loc_err[0] && loc_have && !strcmp(loc_name, want_name);
        serial_puts(ok3 ? "loc not-found: fails clean, error set, nothing overwritten: ok\n" : "loc not-found: FAILED\n");

        loc_err[0] = 0;
        int ok4 = !loc_geocode("");
        serial_puts(ok4 ? "loc empty input: rejected, no crash: ok\n" : "loc empty input: FAILED\n");

        int p=0; while (save_name[p]) { loc_name[p]=save_name[p]; p++; } loc_name[p]=0; /* restore whatever was there before this test ran */
        serial_puts((ok1b && ok2 && ok3 && ok4) ? "loctest PASS\n" : "loctest FAIL\n");
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
            /* v1.0.10: a named serial marker with the real returned length,
               so tools/checks/filerobust-check.py can prove an empty file
               reads back as n=0 and an oversized (or corrupted: a lying
               file_size field plus a FAT chain that loops back on itself)
               file comes back clamped to this buffer's own 4095-byte
               capacity, not whatever its directory entry or cluster chain
               claims -- headless, with no screen to scrape. fat_read_file's
               own `remaining = min(file_size, bufsize)` already bounds the
               copy regardless of what the on-disk metadata says, so this
               is a proof marker, not a fix; see that function's own
               comments in drivers/fat.c for why a corrupted size/chain
               can't run past bufsize here. */
            serial_puts("cat "); serial_puts(arg); serial_puts(": ");
            if (n < 0) serial_puts("not found\n");
            else {
                char nb[8]; int ni = 0; unsigned int un = (unsigned int)n;
                if (un == 0) nb[ni++] = '0';
                while (un) { nb[ni++] = (char)('0' + un % 10); un /= 10; }
                nb[ni] = 0;
                for (int j = 0; j < ni / 2; j++) { char t = nb[j]; nb[j] = nb[ni - 1 - j]; nb[ni - 1 - j] = t; }
                serial_puts("n="); serial_puts(nb); serial_puts("\n");
            }
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
            int ok = vfs_write_file(arg, content, strlen(content));
            /* v1.0.10: named serial marker so a full-disk write failure
               (alloc_cluster's free-cluster scan in drivers/fat.c coming up
               empty) can be told apart from a hang, headless, and so the
               very next command in the same boot can be checked for a real
               "ok" -- proof the shell is still responsive, not just that
               this one call returned. See tools/checks/filerobust-check.py. */
            serial_puts("write "); serial_puts(arg); serial_puts(ok ? ": ok\n" : ": failed\n");
            puts(ok ? "written\n" : "failed (name taken or disk full)\n");
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
        /* 1.8: extended for the sidebar+detail-pane redesign. settings_row_at
           now takes which section is showing (a row belonging to a
           different, not-currently-visible section can never be hit) and
           settings_sidebar_at is the same shape for the new sidebar's own
           3 section rows. Also covers the real "does a setting survive a
           reboot" contract settings_save/settings_load promise -- Settings'
           own footer text says so, this proves it round-trips through the
           same save/load path a real reboot uses, not just that the in-
           memory toggle flips. */
        int ok = 1;
        if (settings_row_at(300, 92,  960, 0) != 0) { puts("settingsclick: General row 0 (Wind) center missed\n"); ok = 0; }
        /* 1.8.2 polish pass: Wind's value is now a real switch control,
           right-aligned near the row's own right edge instead of an
           "On"/"Off" text label near the middle -- prove that region of
           the row (where the switch itself actually is, detail_right(940)
           - SETTINGS_SWITCH_W(40) = 900, plus a few px margin) still
           resolves to row 0 like the rest of the row always has, same
           "click anywhere on the row toggles it" contract every row here
           keeps, not just the switch's own bounding box. */
        if (settings_row_at(910, 92,  960, 0) != 0) { puts("settingsclick: General row 0's switch region missed\n"); ok = 0; }
        if (settings_row_at(300, 164, 960, 0) != 2) { puts("settingsclick: General row (Wallpaper) center missed\n"); ok = 0; }
        if (settings_row_at(300, 200, 960, 0) != 7) { puts("settingsclick: General row (Location) center missed\n"); ok = 0; }
        if (settings_row_at(300, 70,  960, 0) != -1) { puts("settingsclick: above the first General row should miss\n"); ok = 0; }
        if (settings_row_at(300, 119, 960, 0) != -1) { puts("settingsclick: real gap between rows should miss\n"); ok = 0; }
        if (settings_row_at(180, 92,  960, 0) != -1) { puts("settingsclick: left of the detail pane (in the sidebar's x range) should miss\n"); ok = 0; }
        if (settings_row_at(950, 92,  960, 0) != -1) { puts("settingsclick: right of the detail pane should miss\n"); ok = 0; }
        if (settings_row_at(300, 92,  960, 1) != 3) { puts("settingsclick: Assistant row (LLM model) center missed\n"); ok = 0; }
        if (settings_row_at(300, 128, 960, 1) != 4) { puts("settingsclick: Assistant row (LLM host:port) center missed\n"); ok = 0; }
        if (settings_row_at(300, 92,  960, 2) != 5) { puts("settingsclick: Account row (Account) center missed\n"); ok = 0; }
        /* Same physical y (92, each section's own first row) resolves to a
           different absolute row index depending which section is showing
           -- proves the hit-test is scoped to what's actually on screen,
           not just "any known row y" independent of section. */
        if (settings_row_at(300, 92, 960, 1) == 0) { puts("settingsclick: Assistant section must not hit General's Wind row\n"); ok = 0; }
        if (settings_sidebar_at(90, 60)  != 0) { puts("settingsclick: sidebar row 0 (General) missed\n"); ok = 0; }
        if (settings_sidebar_at(90, 94)  != 1) { puts("settingsclick: sidebar row 1 (Assistant) missed\n"); ok = 0; }
        if (settings_sidebar_at(90, 128) != 2) { puts("settingsclick: sidebar row 2 (Account) missed\n"); ok = 0; }
        if (settings_sidebar_at(90, 40)  != -1) { puts("settingsclick: above the sidebar's first row should miss\n"); ok = 0; }
        if (settings_sidebar_at(2,  60)  != -1) { puts("settingsclick: left of the sidebar (x<8) should miss\n"); ok = 0; }
        if (settings_sidebar_at(170,60)  != -1) { puts("settingsclick: right of the sidebar should miss\n"); ok = 0; }

        int wind_before = wind_enabled;
        wind_enabled = !wind_enabled;
        settings_save();
        int wind_saved = wind_enabled;
        wind_enabled = !wind_enabled; /* corrupt the in-memory value so a real disk round-trip is what proves it, not just the assignment above */
        settings_load();
        if (wind_enabled != wind_saved) { puts("settingsclick: Wind toggle did not survive settings_save/settings_load\n"); ok = 0; }
        wind_enabled = wind_before; settings_save(); /* restore, no lasting side effect on the rest of this boot */

        puts(ok ? "settingsclick: click hit-tests the row it actually landed on, sidebar sections hit-test correctly, and a toggle survives save/load: ok\n" : "settingsclick: FAILED\n");
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
        if (!*arg) { puts("usage: serveapp weather|curbfind|keyrate|bookrank|quotestreak|tonchi|toroid|hikko|fieldbook\n"); }
        else if (!net_init(0x0A00020F)) { puts("no NIC found (tried RTL8139, NE2000)\n"); }
        else {
            if (!strcmp(arg, "weather"))          serve_app("weather", app_weather_html, app_weather_len);
            else if (!strcmp(arg, "curbfind"))    serve_app("curbfind", app_curbfind_html, app_curbfind_len);
            else if (!strcmp(arg, "keyrate"))     serve_app("keyrate", app_keyrate_html, app_keyrate_len);
            else if (!strcmp(arg, "bookrank"))    serve_app("bookrank", app_bookrank_html, app_bookrank_len);
            else if (!strcmp(arg, "quotestreak")) serve_app("quotestreak", app_quotestreak_html, app_quotestreak_len);
            else if (!strcmp(arg, "tonchi") || !strcmp(arg, "lexly")) serve_app("tonchi", app_tonchi_html, app_tonchi_len);
            else if (!strcmp(arg, "toroid"))      serve_app("toroid", app_toroid_html, app_toroid_len);
            else if (!strcmp(arg, "hikko"))    serve_app("hikko", app_hikko_html, app_hikko_len);
            else if (!strcmp(arg, "fieldbook"))   serve_app("fieldbook", app_fieldbook_html, app_fieldbook_len);
            else puts("unknown app, see usage\n");
        }
    }
    else if (!strcmp(line, "chat") || !strcmp(line, "samantha")) {
        /* 1.9.26: Samantha is a ring-3 program (user/samantha.c); the text shell opens her window like `notes`. */
        puts("chat: Samantha is a desktop window now, run gui and click her in the dock\n"); /* 2.0 gate 5: no blocking launch from the text shell */
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
        puts("notes: Notes is a desktop window now, run gui and click it in the dock\n"); /* 2.0 gate 5: no blocking launch from the text shell */
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
    /* calctest retired: the parser it exercised moved to user/calculator.c,
       a real ring-3 program (1.7.12), whose own math is what
       tools/checks/ring3calc-check.py exercises now. */
    else if (!strcmp(line, "stockstest")) {
        int pass = 1;
        char price_str[16];
        stocks_format_price(23800, price_str, sizeof(price_str));
        if (price_str[0] != '2' || price_str[1] != '3') { puts("stocks price format failed\n"); pass = 0; }
        int dollars, cents, sign;
        stocks_format_change(-18000, &dollars, &cents, &sign);
        if (sign != -1 || dollars != 180) { puts("stocks change format failed\n"); pass = 0; }
        puts(pass ? "stocks formatting: ok\n" : "FAILED\n");
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

static int bench_at_boot = 0;
static int panic_test_at_boot = 0;
void kmain(unsigned int multiboot_info_addr){
    serial_init();
    serial_puts("=== kmain boot start === v" JT_VERSION_STR "\n"); entropy_init(); /* 1.7.10: seed the DRBG before anything asks for a salt */
    /* 1.0.12: llmhost=/llmport= command-line overrides for llm_host/llm_port
       (declared way below), parsed alongside wxhost= but applied AFTER
       settings_load() runs (see its call site) so a stale/persisted
       SETTINGS.TXT can never silently win over an override the boot
       command line explicitly asked for -- the same problem wxhost=
       never has to solve since it feeds a variable settings_load() never
       touches. Empty/zero means "no override," the ordinary compiled-in
       default or whatever Settings has saved stands. */
    char llm_host_override[LLM_HOST_MAX] = "";
    int llm_port_override = 0;
    /* 2.14.0: claudehost=/claudeport=/claudetoken=, the same one-boot override for the Claude
       relay (tools/checks/ring3claude-check.py boots with no disk). Applied after settings_load,
       never saved. The token is never echoed to serial. */
    char claude_host_override[CLAUDE_HOST_MAX] = "";
    char claude_token_override[CLAUDE_TOKEN_MAX] = "";
    int claude_port_override = 0;
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
        const char *cl0 = cl; /* the loop below mutates cl directly; llmhost=/llmport= (further down) need the untouched start */
        for (const char *pc = cl; pc && *pc; pc++)
            if (pc[0]=='p' && pc[1]=='o' && pc[2]=='r' && pc[3]=='t' && pc[4]=='f' && pc[5]=='o' && pc[6]=='l' && pc[7]=='i' && pc[8]=='o') { portfolio_dock = 1; serial_puts("portfolio dock\n"); break; }
        for (const char *pc = cl; pc && *pc; pc++)
        for (const char *pc = cl; pc && *pc; pc++)
            if (pc[0]=='b' && pc[1]=='e' && pc[2]=='n' && pc[3]=='c' && pc[4]=='h' && (pc[5]==' ' || pc[5]==0)) { bench_at_boot = 1; break; }
        for (const char *pc = cl; pc && *pc; pc++)
            if (pc[0]=='s' && pc[1]=='a' && pc[2]=='m' && pc[3]=='a' && pc[4]=='n' && pc[5]=='t' && pc[6]=='h' && pc[7]=='a') { boot_to_samantha = 1; serial_puts("bootsamantha\n"); break; }
        /* "phone" on the command line: Bochs VBE takes any mode, so this
           just swaps gui_run's video mode for a real portrait phone size
           (430x760) instead of the desktop's 960x540@2x. Landing's embed.js
           always pairs this with samantha (a phone visitor gets her full-
           screen view, not the icon desktop laid out for a mouse), but
           phone alone still forces it here so booting with just "phone"
           never lands on a desktop that was never designed for 430px. */
        for (const char *pc = cl; pc && *pc; pc++)
            if (pc[0]=='p' && pc[1]=='h' && pc[2]=='o' && pc[3]=='n' && pc[4]=='e' && (pc[5]==' ' || pc[5]==0)) { boot_to_phone = 1; boot_to_samantha = 1; serial_puts("bootphone\n"); break; }
        if (gui_parse_res(cl, &boot_res_w, &boot_res_h)) serial_puts("bootres\n"); /* dock_geom.c: "res=WxH" */
        /* "panictest" on the multiboot command line (tools/checks/
           panic-symbols-check.py passes it) -- deliberately faults from a
           known, named function right after idt_install() so a headless
           check can prove the crash report's symbol table names it. Never
           set on a normal boot, so this path can never fire outside the
           check that asks for it. */
        for (const char *pc = cl; pc && *pc; pc++)
            if (pc[0]=='p' && pc[1]=='a' && pc[2]=='n' && pc[3]=='i' && pc[4]=='c' && pc[5]=='t' && pc[6]=='e' && pc[7]=='s' && pc[8]=='t') { panic_test_at_boot = 1; break; }
        /* nodhcp: skip the DHCP DISCOVER/OFFER/REQUEST/ACK exchange
           net_init now runs by default (drivers/net.c) and boot straight
           onto the hardcoded 10.0.2.15/SLIRP-gateway config every net_init
           caller already passes. Parsed before anything else in kmain
           could call net_init. */
        for (const char *pc = cl; pc && *pc; pc++)
            if (pc[0]=='n' && pc[1]=='o' && pc[2]=='d' && pc[3]=='h' && pc[4]=='c' && pc[5]=='p' && (pc[6]==' ' || pc[6]==0)) { net_nodhcp = 1; break; }
        for (const char *pc = cl; pc && *pc; pc++)
            if (pc[0]=='d' && pc[1]=='r' && pc[2]=='u' && pc[3]=='n' && pc[4]=='k' && (pc[5]==' ' || pc[5]==0)) { extern void window_set_drunk(int); window_set_drunk(1); serial_puts("drunk\n"); break; }
        ring3app_autoopen_arm(cl); r3stress_arm(cl); /* `open=keyrate` / `open=toroid` boot flag */
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
        /* 1.1.1: tilehost=A.B.C.D[:PORT], the wall_fetch equivalent of
           wxhost= right above -- see tile_override_host's own comment. */
        for (const char *pc = cl0; pc && *pc; pc++)
            if (pc[0]=='t' && pc[1]=='i' && pc[2]=='l' && pc[3]=='e' && pc[4]=='h' && pc[5]=='o' && pc[6]=='s' && pc[7]=='t' && pc[8]=='=') {
                pc += 9; int hp = 0;
                while (((*pc >= '0' && *pc <= '9') || *pc == '.') && hp < 19) tile_override_host[hp++] = *pc++;
                tile_override_host[hp] = 0;
                if (*pc == ':') { unsigned int pt = 0; pc++; while (*pc >= '0' && *pc <= '9') pt = pt * 10 + (unsigned int)(*pc++ - '0'); if (pt && pt < 65536) tile_override_port = (unsigned short)pt; }
                serial_puts("tilehost="); serial_puts(tile_override_host); serial_puts("\n");
                break;
            }
        for (const char *pc = cl0; pc && *pc; pc++)
            if (pc[0]=='w' && pc[1]=='a' && pc[2]=='l' && pc[3]=='l' && pc[4]=='t' && pc[5]=='h' && pc[6]=='e' && pc[7]=='m' && pc[8]=='e' && pc[9]=='=') {
                pc += 10;
                if (pc[0]=='m' && pc[1]=='a' && pc[2]=='p') wall_theme_override = WALL_WARM;
                else if (pc[0]=='s' && pc[1]=='a' && pc[2]=='t') wall_theme_override = WALL_SAT;
                else if (pc[0]=='p' && pc[1]=='h' && pc[2]=='o') wall_theme_override = WALL_PHOTO;
                break;
            }
        /* 1.0.12: llmhost=HOST / llmport=PORT, the Chat equivalent of
           wxhost= above -- for tools/checks/chat-samantha-check.py to point
           chat_send at a local fake HTTP server instead of the real,
           internet-facing Turing default, headlessly and without touching
           SETTINGS.TXT. Unlike wxhost's dotted-quad-only host, this takes
           any hostname byte up to the next space or end of string, the
           same real hostnames (turing.heyitsmejosh.com) llm_host itself
           already accepts, not just an IP literal. Applied to llm_host/
           llm_port after settings_load() runs, not here -- see that call
           site for why. */
        for (const char *pc = cl0; pc && *pc; pc++)
            if (pc[0]=='l' && pc[1]=='l' && pc[2]=='m' && pc[3]=='h' && pc[4]=='o' && pc[5]=='s' && pc[6]=='t' && pc[7]=='=') {
                pc += 8; int hp = 0;
                while (*pc && *pc != ' ' && hp < LLM_HOST_MAX - 1) llm_host_override[hp++] = *pc++;
                llm_host_override[hp] = 0;
                serial_puts("llmhostcli="); serial_puts(llm_host_override); serial_puts("\n");
                break;
            }
        for (const char *pc = cl0; pc && *pc; pc++)
            if (pc[0]=='l' && pc[1]=='l' && pc[2]=='m' && pc[3]=='p' && pc[4]=='o' && pc[5]=='r' && pc[6]=='t' && pc[7]=='=') {
                pc += 8; unsigned int pt = 0;
                while (*pc >= '0' && *pc <= '9') pt = pt * 10 + (unsigned int)(*pc++ - '0');
                if (pt && pt < 65536) llm_port_override = (int)pt;
                break;
            }
        for (const char *pc = cl0; pc && *pc; pc++) {
            if (pc != cl0 && pc[-1] != ' ') continue; /* whole words only */
            int is_h = key_is(pc, 11, "claudehost="), is_t = key_is(pc, 12, "claudetoken="), is_p = key_is(pc, 11, "claudeport=");
            if (is_h || is_t) {
                char *dst = is_h ? claude_host_override : claude_token_override;
                int cap = is_h ? CLAUDE_HOST_MAX : CLAUDE_TOKEN_MAX, j = 0;
                const char *v = pc + (is_h ? 11 : 12);
                while (*v && *v != ' ' && j < cap - 1) dst[j++] = *v++;
                dst[j] = 0;
                if (is_h) { serial_puts("claudehostcli="); serial_puts(claude_host_override); serial_puts("\n"); }
                else serial_puts("claudetokencli=set\n");
            } else if (is_p) {
                unsigned int pt = 0; const char *v = pc + 11;
                while (*v >= '0' && *v <= '9' && pt < 100000) pt = pt * 10 + (unsigned int)(*v++ - '0');
                if (pt && pt < 65536) claude_port_override = (int)pt;
            }
        }
        stocks_cmdline(cl0); /* stkhost=HOST:PORT, kernel/stocks.h */
        jt_clip_cmdline(cl0); /* cliptrace, kernel/syscall.c: content hash on the CLIPCOPY/CLIPPASTE lines */
        jt_facehost_cmdline(cl0); /* facehost=HOST[:PORT], kernel/syscall.c: where ring-3 Samantha fetches her face and speech */
    }
    vga_text_mode_init(); /* real hardware/QEMU already boot into text mode via their own BIOS; a BIOS-less multiboot path (v86) never sets it at all, so make it explicit rather than inherited */
    klog("vga_text_mode_init: text mode 3 programmed");
    gdt_install();
    klog("gdt_install: GDT loaded");
    idt_install();
    klog("idt_install: IDT loaded");
    if (panic_test_at_boot) jt_panic_test_target(); /* never returns: ring-0 fault, isr_handler halts after printing the crash report */
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
    syscall_windows_init(); /* 1.9.23: compositor window rows start empty */
    klog("tasks_init: scheduler ready");
    klog(sb16_init() ? "sb16_init: Sound Blaster 16 found" : "sb16_init: no sound card");
    ata_blockdev_register(); /* v33 (0.33.0): register real backends before anything tries to mount a filesystem over one */
    ramdisk_init();
    trash_init();
    int fs_ok = fat_mount();
    fs_ok_global = fs_ok;
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
        vfs_write_file("NOTES.TXT", demo_notes, strlen(demo_notes)); ramfs_seed_demo_docs(); if (boot_to_samantha) { u8 hh, mi, wd, dom, mon; cmos_read_time_stable(&hh, &mi, &wd, &dom, &mon); ramfs_seed_demo_events(cmos(9), mon, dom); } /* sample itinerary dated from the RTC; a real folder, so the tour's Burrow scene has something to open */
        klog("vfs: no FAT disk (v86 has none to mount), switched default backend to ramfs with demo files");
    }
    settings_load(); /* v47: real settings, saved defaults if SETTINGS.TXT doesn't exist yet */
    /* 1.0.12: apply llmhost=/llmport= AFTER settings_load(), not before --
       a boot-time command-line override has to win over whatever a real
       persisted SETTINGS.TXT says, the same way wxhost= always wins for
       weather because it feeds a variable settings_load() never touches
       at all. llm_host/llm_port ARE settings_load()'s own variables, so
       applying this any earlier would just get silently overwritten by a
       real settings file the moment one exists. Deliberately never
       settings_save()'d: this is a one-boot test override, not a change
       to what Settings remembers. */
    if (llm_host_override[0]) { int p = 0; while (llm_host_override[p] && p < LLM_HOST_MAX - 1) { llm_host[p] = llm_host_override[p]; p++; } llm_host[p] = 0; }
    if (llm_port_override) llm_port = llm_port_override;
    if (claude_host_override[0]) { int p = 0; while (claude_host_override[p]) { claude_host[p] = claude_host_override[p]; p++; } claude_host[p] = 0; }
    if (claude_token_override[0]) { int p = 0; while (claude_token_override[p]) { claude_token[p] = claude_token_override[p]; p++; } claude_token[p] = 0; }
    if (claude_port_override) claude_port = claude_port_override;
    if (wall_theme_override >= 0) { wall_theme = wall_theme_override; serial_puts("wallthemeoverride="); { char d[2] = { (char)(48 + wall_theme), 0 }; serial_puts(d); } serial_puts("\n"); }
    clear();
    boot_chime();
    if (bench_at_boot) bench_run(fs_ok);
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
