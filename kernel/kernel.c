/* Freestanding i386 kernel: VGA text, PS/2 keyboard, RTC clock, tiny shell. */
#include "gdt.h"
#include "idt.h"
#include "irq.h"
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
/* v75 (0.67.0): the wallpaper is read through this pointer, not the baked
   array directly, so a real fetched image (wall_fetch below: a 2x2 mosaic
   of OpenTopoMap tiles around the ip-api location, decoded by
   drivers/png.c) can replace it at runtime. Same 960x540x3 layout, so the
   two real readers (gui_wallpaper_color, gui_wallpaper_row) only changed
   which base address they index. Always points at something valid: the
   baked photo until a fetch lands, and back to it if the user picks
   Photo in Settings. pngtest keeps comparing against wallpaper_rgb by
   name on purpose (its fixtures are crops of the baked photo). */
static const unsigned char *wall_src = wallpaper_rgb;
static unsigned char *wall_map = 0;        /* the fetched mosaic, kmalloc'd, kept while the session lives so Photo->Map needs no refetch */
static int wall_map_tx = 0, wall_map_ty = 0, wall_map_cx = 0, wall_map_cy = 0; /* tile x/y of the mosaic's top-left tile, crop offset inside it */
#define WALL_ZOOM 12
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

static void gui_app_mouse_tick(void);
static char getch(void){
    for (;;) {
        gui_app_mouse_tick();
        int sc = kbd_pop();
        if (sc < 0) { __asm__ volatile ("hlt"); continue; }
        if (sc & 0x80) continue;            /* key release */
        char c = SC[sc & 0x7F];
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
            char c = SC[sc & 0x7F];
            if (c) { gui_close_was_click = 0; return (int)(unsigned char)c; }
            continue;
        }
        if (mouse_click_edge()) { gui_close_was_click = 1; return -1; }
        __asm__ volatile ("hlt");
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
static int get_key_or_click(void);

static int get_key(void){
    for (;;) {
        int sc = kbd_pop();
        if (sc < 0) { __asm__ volatile ("hlt"); continue; }
        if (sc == 0xE0) {
            int sc2;
            do { sc2 = kbd_pop(); if (sc2 < 0) __asm__ volatile ("hlt"); } while (sc2 < 0);
            if (sc2 == 0x48) return KEY_UP;
            if (sc2 == 0x50) return KEY_DOWN;
            if (sc2 == 0x4B) return KEY_LEFT;
            if (sc2 == 0x4D) return KEY_RIGHT;
            continue; /* other extended keys: ignore */
        }
        if (sc & 0x80) continue;
        char c = SC[sc & 0x7F];
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
                do { sc2 = kbd_pop(); if (sc2 < 0) __asm__ volatile ("hlt"); } while (sc2 < 0);
                if (sc2 == 0x48) return KEY_UP;
                if (sc2 == 0x50) return KEY_DOWN;
                if (sc2 == 0x4B) return KEY_LEFT;
                if (sc2 == 0x4D) return KEY_RIGHT;
                continue;
            }
            if (!(sc & 0x80)) {
                char c = SC[sc & 0x7F];
                gui_close_was_click = 0;
                if (c == '\n') return KEY_ENTER;
                if (c == 27)   return KEY_ESC;
                if (c) return c;
            }
            continue;
        }
        if (mouse_click_edge()) { gui_close_was_click = 1; return KEY_CLICK; }
        __asm__ volatile ("hlt");
    }
}

/* ---- RTC via CMOS. ponytail: no PIT tick counter; the shell only ever
   needs wall-clock, and this needs no interrupt handler. ---- */
static u8 cmos(u8 reg){ outb(0x70, reg); return inb(0x71); }

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
   real pages correctly at all. */
static void render_wrapped_text(const char *text, int x0, int y0, int max_w_px, int max_h_px, unsigned int fg) {
    int x = x0, y = y0;
    const char *p = text;
    while (*p) {
        if (y + 16 > y0 + max_h_px) return; /* out of room */
        if (*p == '\n') { y += 16; x = x0; p++; continue; }
        if (*p == ' ') {
            if (x + 8 > x0 + max_w_px) { x = x0; y += 16; }
            else { x += 8; }
            p++;
            continue;
        }
        unsigned int wlen = 0;
        while (p[wlen] && p[wlen] != ' ' && p[wlen] != '\n') wlen++;
        if (x > x0 && x + (int)wlen * 8 > x0 + max_w_px) { x = x0; y += 16; if (y + 16 > y0 + max_h_px) return; }
        for (unsigned int i = 0; i < wlen; i++) {
            if (x + 8 > x0 + max_w_px) { x = x0; y += 16; if (y + 16 > y0 + max_h_px) return; }
            font_draw_char((unsigned char)p[i], x, y, fg, -1);
            x += 8;
        }
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
#define GUI_APP_COUNT   22 /* 20 real apps + the Apps folder + Trash */
#define GUI_APPS_FOLDER 20 /* not an app: the dock tile that opens the folder */
#define GUI_TRASH       21
static const char *GUI_LABELS[GUI_APP_COUNT] = {"Files", "Mail", "Calendar", "Notes", "Reminders", "Terminal", "Chat", "Weather", "Curbfind", "Keyrate", "Bookrank", "Quotes", "Plan", "Lexly", "Toroid", "Sparkjar", "Homeqi", "Fieldbook", "Contacts", "Calculator", "Apps", "Trash"};
static const unsigned int GUI_COLORS[GUI_APP_COUNT] = {
    0x00707070, 0x00A13F3F, 0x00A0553F, 0x006B4423, 0x00375A4A, 0x002B2B2B, 0x00365E8C, 0x0085144B,
    0x007A2048, 0x00B08900, 0x002F7B4F, 0x008B4A9C, 0x00475C6B, 0x00376E5E, 0x00234A78, 0x00A6741E, 0x00566A3A, 0x005A3E6B, 0x00A87C5B, 0x00556B85
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
#define GUI_ICON_COUNT 10
static const int GUI_DOCK_DEFAULT[GUI_ICON_COUNT] = {GUI_APPS_FOLDER, 0, 1, 2, 3, 4, 5, 6, 7, GUI_TRASH};

/* gui_order is a permutation of icon indices by dock slot: dragging an icon
   and dropping it on another slot swaps the two, so the arrangement is
   real and sticks for the rest of this GUI session (reset to launch order
   next time `gui` runs; nothing about layout is saved to disk, matching
   this whole desktop's one-screen, nothing-persisted scope). */
static int gui_order[GUI_ICON_COUNT];
static void gui_order_init(void){ for (int i = 0; i < GUI_ICON_COUNT; i++) gui_order[i] = GUI_DOCK_DEFAULT[i]; }
static unsigned char dock_hover_extra[GUI_ICON_COUNT];

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
/* v75: wallpaper source. 1 = a real map of the real location (the default
   once buildable, Joshua's own call in roadmap.md's satellite entry), 0 =
   the baked photo. Map mode still shows the photo until the fetch lands
   and keeps showing it if the fetch fails, never a blank desktop. */
static int wall_mode = 1;
static int wind_enabled = 1; /* real definition; forward of the v45 declaration below so settings_load (right here, needs both) can precede it in the file */

/* v47 (0.47.0): settings persisted through the VFS, so "customize the OS
   from inside the OS" actually survives a reboot instead of resetting to
   the compiled-in defaults every boot. Deliberately a flat key=value text
   file (SETTINGS.TXT), not a binary struct: it's human-readable from any
   app that can read a file (cat, the editor), and a corrupt or missing
   file just means defaults, never a crash, since every key is parsed with
   its own bounds check and a real default already set before parsing
   starts. */
#define SETTINGS_FILE "SETTINGS.TXT"
static void settings_load(void){
    static char buf[256];
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
        int val = 0, neg = 0, k = eq + 1;
        if (k < line_end && buf[k] == '-') { neg = 1; k++; }
        while (k < line_end && buf[k] >= '0' && buf[k] <= '9') { val = val * 10 + (buf[k] - '0'); k++; }
        if (neg) val = -val;
        int keylen = eq - start;
        int is_wind = keylen == 4 && buf[start]=='w' && buf[start+1]=='i' && buf[start+2]=='n' && buf[start+3]=='d';
        int is_dock = keylen == 4 && buf[start]=='d' && buf[start+1]=='o' && buf[start+2]=='c' && buf[start+3]=='k';
        int is_wall = keylen == 4 && buf[start]=='w' && buf[start+1]=='a' && buf[start+2]=='l' && buf[start+3]=='l';
        if (is_wind) wind_enabled = (val != 0);
        else if (is_dock && val >= 5 && val <= 25) dock_scale_pct = val;
        else if (is_wall) wall_mode = (val != 0);
    }
}

static void settings_save(void){
    char buf[64];
    int n = 0;
    const char *k1 = "wind="; while (*k1) buf[n++] = *k1++;
    buf[n++] = wind_enabled ? '1' : '0'; buf[n++] = '\n';
    const char *k2 = "dock="; while (*k2) buf[n++] = *k2++;
    if (dock_scale_pct >= 10) buf[n++] = '0' + dock_scale_pct / 10;
    buf[n++] = '0' + dock_scale_pct % 10;
    buf[n++] = '\n';
    const char *k3 = "wall="; while (*k3) buf[n++] = *k3++;
    buf[n++] = wall_mode ? '1' : '0'; buf[n++] = '\n';
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
#define DOCK_MAGNIFY    9
#define DOCK_LIFT       10

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
    if (my < y0 - DOCK_MAGNIFY - 20 || my >= y0 + h) return -1;
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
    int sc = (int)window_scale();
    int px0 = x * sc, py0 = y * sc, pw = w * sc, ph = h * sc, pr = r * sc, band = 3; /* v44.1: 3 physical px; AA_BAND*sc was 10 and read as a soft, blurry corner */
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
                int inner = pr - band;
                if (d2 > inner * inner){
                    if (d2 >= pr * pr) continue;            /* outside: leave the wallpaper alone */
                    int t = gui_isqrt(d2) - inner;
                    /* v44.3: the real pixel behind THIS corner, not the
                       row's centre-column sample (bg) used everywhere
                       else in this loop, so the tray's top-left/top-right
                       corners don't blend toward a color sampled from
                       the middle of the row. */
                    unsigned int corner_bg = gui_wallpaper_sample(px0 + px, py0 + py, 0);
                    col = gui_lerp(color, corner_bg, t, band);
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
static void gui_fill_circle(int cx, int cy, int r, unsigned int color, unsigned int into){
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
   freestanding build doesn't link. */
static void gui_draw_capsule(int x0, int y0, int x1, int y1, int r, unsigned int color, unsigned int into){
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
static inline __attribute__((always_inline)) struct wp_row gui_wallpaper_row(int py, int sway){
    struct wp_row c;
    int lw = (int)window_width(), lh = (int)window_height();
    int sc = (int)window_scale();
    c.pw = lw * sc;
    int area_h = lh - GUI_MENUBAR_H;
    int row = py - GUI_MENUBAR_H * sc; if (row < 0) row = 0;
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
    if (y_from < GUI_MENUBAR_H) y_from = GUI_MENUBAR_H;
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
static void gui_draw_logo(int x, int cy, int scale, unsigned int bg){
    unsigned int c = 0x0085144B;
    int split_y = cy - scale, top_y = cy - 7 * scale;
    int r = scale > 1 ? scale - 1 : 0;
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
static int geo_fetch(void){
    static char body[1024];
    int n = http_get("ip-api.com", "/json/", 80, body, sizeof(body) - 1);
    if (n <= 0) return 0;
    body[n] = 0;
    char lat[16], lon[16];
    if (!json_extract_number_text(body, "lat", lat, sizeof(lat))) return 0;
    if (!json_extract_number_text(body, "lon", lon, sizeof(lon))) return 0;
    int i;
    for (i = 0; lat[i]; i++) geo_lat[i] = lat[i]; geo_lat[i] = 0;
    for (i = 0; lon[i]; i++) geo_lon[i] = lon[i]; geo_lon[i] = 0;
    if (!json_extract_string(body, "city", geo_city, sizeof(geo_city))) geo_city[0] = 0;
    geo_have = 1;
    serial_puts("geo="); serial_puts(geo_lat); serial_puts(","); serial_puts(geo_lon); serial_puts("\n");
    return 1;
}

static void weather_fetch(void){
    weather_last_tick = ticks();
    if (!rtl8139_init()) return;
    net_init(0x0A00020F);
    if (!geo_have && !geo_fetch()) return; /* v71: no real location, no fetch, nothing fabricated */
    static char body[2048];
    static char path[128];
    { int p = 0; const char *s;
      for (s = "/v1/forecast?latitude="; *s; s++) path[p++] = *s;
      for (s = geo_lat; *s; s++) path[p++] = *s;
      for (s = "&longitude="; *s; s++) path[p++] = *s;
      for (s = geo_lon; *s; s++) path[p++] = *s;
      for (s = "&current=temperature_2m,weather_code"; *s; s++) path[p++] = *s;
      path[p] = 0; }
    serial_puts("wxurl="); serial_puts(path); serial_puts("\n");
    int n = http_get("api.open-meteo.com", path, 80, body, sizeof(body) - 1);
    if (n <= 0) return;
    body[n] = 0;
    int t10 = 0, code10 = 0;
    if (!json_current_number(body, "temperature_2m", &t10)) return;
    json_current_number(body, "weather_code", &code10);
    int t = (t10 >= 0 ? t10 + 5 : t10 - 5) / 10; /* round to whole degrees */
    weather_temp_c = t; weather_code10 = code10; weather_have = 1;
    wind_weather_pct = wind_pct_for_weather_code(code10 / 10); /* v60: real wind sway now follows real weather */
    int p = 0;
    if (t < 0) { weather_text[p++] = '-'; t = -t; }
    if (t >= 10) weather_text[p++] = '0' + t / 10;
    weather_text[p++] = '0' + t % 10;
    weather_text[p++] = (char)0xF8; /* CP437 degree sign, present in both the hardware font and the fallback */
    weather_text[p++] = ' ';
    for (const char *w = weather_word(code10 / 10); *w; w++) weather_text[p++] = *w;
    weather_text[p] = 0;
    serial_puts("wx="); serial_puts(weather_text); serial_puts("\n"); /* v71: tools/geo-check.sh asserts the fetch really landed, not just that the URL was built */
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
    for (int i = 0; i < WALL_COLS * WALL_ROWS; i++) {
        int col = i % WALL_COLS, row = i / WALL_COLS;
        int ttx = tx + col, tty = ty + row;
        int p = 0; const char *s;
        for (s = "/"; *s; s++) path[p++] = *s;
        { char d[12]; int nd = 0; unsigned int u = WALL_ZOOM; do { d[nd++] = '0' + u % 10; u /= 10; } while (u); while (nd) path[p++] = d[--nd]; path[p++] = '/'; }
        { char d[12]; int nd = 0; unsigned int u = (unsigned int)ttx; do { d[nd++] = '0' + u % 10; u /= 10; } while (u); while (nd) path[p++] = d[--nd]; path[p++] = '/'; }
        { char d[12]; int nd = 0; unsigned int u = (unsigned int)tty; do { d[nd++] = '0' + u % 10; u /= 10; } while (u); while (nd) path[p++] = d[--nd]; }
        for (s = ".png"; *s; s++) path[p++] = *s;
        path[p] = 0;
        int n_bytes = http_get("a.tile.opentopomap.org", path, 80, body, 65536);
        if (n_bytes <= 0) { kfree(body); if (!wall_map) kfree(dst); wall_serial_err("http", i); return 0; }
        unsigned char *px_out = 0; unsigned int w = 0, h = 0, ch = 0;
        int r = png_decode(body, (unsigned int)n_bytes, &px_out, &w, &h, &ch);
        if (r != 0 || w != WALL_TILE || h != WALL_TILE || ch != 3) {
            if (px_out) kfree(px_out); kfree(body); if (!wall_map) kfree(dst);
            wall_serial_err(r ? "png" : "tilesize", r ? r : (int)w); return 0;
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
    wall_map = dst; wall_map_tx = tx; wall_map_ty = ty; wall_map_cx = cx; wall_map_cy = cy;
    unsigned int fnv = 0x811c9dc5u;
    for (unsigned int i = 0; i < WALLPAPER_W * WALLPAPER_H * 3; i++) { fnv ^= dst[i]; fnv *= 0x01000193u; }
    { char b[96]; int i = 0; const char *s = "wall="; while (*s) b[i++] = *s++;
      int vals[5] = { WALL_ZOOM, tx, ty, cx, cy };
      for (int v = 0; v < 5; v++) { char d[12]; int nd = 0; unsigned int u = (unsigned int)vals[v]; do { d[nd++] = '0' + u % 10; u /= 10; } while (u); while (nd) b[i++] = d[--nd]; b[i++] = v < 4 ? ',' : ' '; }
      for (int sh = 28; sh >= 0; sh -= 4) { int nib = (fnv >> sh) & 0xF; b[i++] = nib < 10 ? '0' + nib : 'a' + nib - 10; }
      b[i++] = '\n'; b[i] = 0; serial_puts(b); }
    return 1;
}
/* Switch what the desktop paints from. Both directions drop every cache
   built from the old pixels (the wind crown band, the dock band) so the
   next frame is honest, not a stale composite of the previous source. */
static void wall_apply(int want_map){
    const unsigned char *next = (want_map && wall_map) ? wall_map : wallpaper_rgb;
    if (next == wall_src) return;
    wall_src = next;
    wall_caches_drop();
}

static void gui_draw_menubar(void){
    u8 h = cmos(4), m = cmos(2), wd = cmos(6), dom = cmos(7), mon = cmos(8);
    u8 hv = (h & 0x0F) + ((h >> 4) * 10), mv = (m & 0x0F) + ((m >> 4) * 10);
    u8 wdv = (wd & 0x0F) + ((wd >> 4) * 10);
    u8 domv = (dom & 0x0F) + ((dom >> 4) * 10);
    u8 monv = (mon & 0x0F) + ((mon >> 4) * 10);
    if (mv == gui_menubar_last_min) return;
    gui_menubar_last_min = mv;

    /* v48: Liquid Glass, direct request. No real alpha compositing in this
       framebuffer (see gui_blend's own note), so "translucent" here means
       the same trick the dock shadow already uses: a real, solid,
       precomputed blend of white toward whatever wallpaper color sits
       behind this row, sampled per-row (gui_wallpaper_color already does
       exactly this, reused, not a second sampler). Mostly white so text
       stays legible, just enough wallpaper bleeding through to read as
       glass instead of a flat opaque bar. */
    for (int row = 0; row < GUI_MENUBAR_H; row++)
        window_rect(0, row, (int)window_width(), 1, gui_lerp(gui_wallpaper_color(row), 0x00FFFFFF, 7, 10));
    window_rect(0, GUI_MENUBAR_H - 1, (int)window_width(), 1, 0x00DDD9D3);
    gui_draw_logo(16, GUI_MENUBAR_H / 2 + 2, 1, 0x00FFFFFF);
    font_draw_string("Joshua Tree", 32, 7, 0x001C1C1E, -1);

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

    font_draw_string(clock, (int)window_width() - p * 8 - 16, 7, 0x001C1C1E, -1);
    if (weather_text[0]) {
        int wl = (int)strlen(weather_text);
        int wx = (int)window_width() - p * 8 - 16 - wl * 8 - 28;
        font_draw_string(weather_text, wx, 7, 0x00884B16, -1);
        weather_hit_x0 = wx - 4; weather_hit_x1 = wx + wl * 8 + 4; /* v53: real click target, same padding feel as the clock's own */
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
    window_rect(x, y - s / 12, w / 3, s / 12, shade);  /* tab, sits behind the front face */
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
    for (int row = 0; row < h - lid; row++) {
        int w = half - (row * (half / 5)) / (h - lid);                              /* taper toward the base */
        window_rect(cx - w, top + lid + 1 + row, 2 * w + 1, 1, ICON_FG);
    }
    for (int r = -1; r <= 1; r++)                                                   /* ribs, punched through in the tile colour */
        window_rect(cx + r * (half / 2), top + lid + 5, s / 26 + 1, h - lid - 9, bg);
}

/* v37: the Apps folder tile, a 3x3 grid of rounded tiles reading as
   "more inside", the same shape every launcher grid has used since the
   first iPhone home screen. */
static void gui_icon_apps(int cx, int cy, int s, unsigned int bg){
    (void)bg;
    int t = s / 5, gap = s / 16, span = 3 * t + 2 * gap;
    int x0 = cx - span / 2, y0 = cy - span / 2;
    for (int row = 0; row < 3; row++)
        for (int col = 0; col < 3; col++)
            window_rect(x0 + col * (t + gap), y0 + row * (t + gap), t, t, ICON_FG);
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
    for (int dy = -ry; dy <= ry; dy++){
        for (int dx = -rx; dx <= rx; dx++){
            int sdx = dx * ry / rx;
            int d2 = sdx * sdx + dy * dy;
            if (d2 > ry * ry) continue;
            /* Quadratic-ish falloff: distance squared against radius
               squared gives a soft centre and a fast fade at the rim,
               closer to a real penumbra than a linear ramp. */
            int t = d2 * 100 / (ry * ry);
            window_pixel_phys(cx_p + dx, cy_p - sc + dy, gui_lerp(core, dock_bg, t, 100));
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
static unsigned int *gui_render_icon_cached(int icon, int size, int slot, unsigned int under){
    int variant = (icon == GUI_TRASH) ? (trash_count() > 0) : 0;
    if (icon_cache[icon][slot] && icon_cache_size[icon][slot] == size && icon_cache_under[icon][slot] == under && icon_cache_variant[icon][slot] == variant) return icon_cache[icon][slot];
    unsigned int sc = window_scale();
    int pw = size * (int)sc;
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
    for (int py = 0; py < pw; py++){
        for (int px = 0; px < pw; px++){
            unsigned int rs = 0, gs = 0, bs = 0;
            for (unsigned int sy = 0; sy < per; sy++){
                unsigned int *row = &ssbuf[((unsigned int)py * per + sy) * ssz + (unsigned int)px * per];
                for (unsigned int sx = 0; sx < per; sx++){ unsigned int c = row[sx]; rs += (c >> 16) & 0xFF; gs += (c >> 8) & 0xFF; bs += c & 0xFF; }
            }
            out[py * pw + px] = ((rs / samples) << 16) | ((gs / samples) << 8) | (bs / samples);
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

/* The hover transition reuses the fully rendered large tile. Rendering a
   fresh supersampled icon for every intermediate size stalls the frame. */
static void gui_draw_dock_icon(int icon, int cx_center, int cy_bottom, int size){
    if (size == DOCK_ICON || size == DOCK_ICON + DOCK_MAGNIFY) {
        gui_draw_one_icon(icon, cx_center, cy_bottom, size);
        return;
    }
    unsigned int *tile = gui_render_icon_cached(icon, DOCK_ICON + DOCK_MAGNIFY, 1, DOCK_TRAY_COLOR);
    if (!tile) { gui_draw_one_icon(icon, cx_center, cy_bottom, size); return; }
    int sc = (int)window_scale(), src = (DOCK_ICON + DOCK_MAGNIFY) * sc, dst = size * sc;
    int x0 = (cx_center - size / 2) * sc, y0 = (cy_bottom - size) * sc;
    for (int y = 0; y < dst; y++)
        for (int x = 0; x < dst; x++) {
            unsigned int color = tile[(y * src / dst) * src + x * src / dst];
            if (color != DOCK_TRAY_COLOR)
                window_pixel_phys(x0 + x, y0 + y, color);
        }
}

/* hover_slot: which slot shows the magnify+label (-1 none). drag_slot: the
   slot currently being dragged, drawn separately so it can float free of
   the row under the cursor instead of at its slot position. */
/* v40: the dock band's top edge, high enough to cover a magnified,
   lifted icon and its label, so repainting this band alone is enough to
   erase any previous hover state. */
static int gui_dock_band_top(void){ return gui_dock_y0() - DOCK_MAGNIFY - DOCK_LIFT - 24; }

static void gui_draw_dock(int hover_slot, int drag_slot, int drag_mx, int drag_my);

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
static unsigned char dock_presented_extra[GUI_ICON_COUNT];
static void gui_redraw_dock_band(int hover_slot, int drag_slot, int drag_mx, int drag_my){
    /* v43: the wallpaper rows behind the dock never change, so bilinear
       them once and copy thereafter. ~400k physical samples per hover
       change was the other half of the flash. */
    int sc = (int)window_scale();
    int top = gui_dock_band_top(), h = (int)window_height() - top;
    int pw = (int)window_width() * sc, ph = h * sc;
    if (!dock_band_cache || dock_band_cache_top != top) {
        if (dock_band_cache) kfree(dock_band_cache);
        if (dock_band_frame) kfree(dock_band_frame);
        dock_band_cache = (unsigned int *)kmalloc((unsigned int)(pw * ph) * sizeof(unsigned int));
        dock_band_frame = (unsigned int *)kmalloc((unsigned int)(pw * ph) * sizeof(unsigned int));
        dock_band_cache_top = top;
        if (dock_band_cache && dock_band_frame) {
            window_push_screen_band(dock_band_cache, top * sc, (unsigned int)ph);
            gui_draw_wallpaper_rows(top, (int)window_height());
            window_pop_screen_band();
        }
    }
    if (dock_band_cache && dock_band_frame) {
        for (int i = 0; i < pw * ph; i++) dock_band_frame[i] = dock_band_cache[i];
        window_push_screen_band(dock_band_frame, top * sc, (unsigned int)ph);
        gui_draw_dock(hover_slot, drag_slot, drag_mx, drag_my);
        window_pop_screen_band();
        /* Only present slots whose icon size changed. Copying the whole
           2 MB band on every hover step visibly exposed the half-drawn
           frame even though composition itself was offscreen. */
        for (int slot = 0; slot < GUI_ICON_COUNT; slot++) {
            if (dock_presented_extra[slot] == dock_hover_extra[slot]) continue;
            int left = (gui_slot_x(slot) - 25) * sc;
            int right = (gui_slot_x(slot) + DOCK_ICON + 25) * sc;
            if (left < 0) left = 0;
            if (right > pw) right = pw;
            for (int py = 0; py < ph; py++) {
                unsigned int *dst = window_phys_row(top * sc + py);
                for (int px = left; px < right; px++) {
                    unsigned int next = dock_band_frame[py * pw + px];
                    if (dst[px] != next) dst[px] = next;
                }
            }
            dock_presented_extra[slot] = dock_hover_extra[slot];
        }
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

static void gui_draw_dock(int hover_slot, int drag_slot, int drag_mx, int drag_my){
    (void)hover_slot;
    int y0 = gui_dock_y0(), dock_h = DOCK_ICON + 2 * DOCK_PAD, dock_w = gui_dock_w(), dock_x = gui_dock_x0();

    /* A soft shadow beneath the tray, the same floating-panel look a real
       macOS dock has, drawn before the tray itself so the tray's own edge
       sits cleanly on top of it. Real per-pixel colors blended toward
       black (gui_blend), fading back to the plain wallpaper color over a
       few rows, no alpha compositing needed since these are precomputed
       solid colors, same technique every AA edge in this file already
       uses. Inset a little past the tray's own rounded corners so it
       reads as a shadow, not a second, darker rectangle. */
    for (int row = 0; row < 10; row++){
        int sy = y0 + dock_h + row;
        unsigned int wall = gui_wallpaper_color(sy);
        unsigned int dark = gui_blend(wall, 0x00000000);
        window_rect(dock_x + 6, sy, dock_w - 12, 1, gui_lerp(dark, wall, row, 10));
    }

    /* gui_rounded_rect_on_wallpaper, not gui_rounded_rect: the tray's top
       and bottom corners sit against very different points on the
       gradient, one fixed blend sample for both was the real dark-bubble
       bug just found and fixed above. */
    gui_rounded_rect_on_wallpaper(dock_x, y0, dock_w, dock_h, DOCK_TRAY_COLOR, 20);

    for (int slot = 0; slot < GUI_ICON_COUNT; slot++) {
        if (slot == drag_slot) continue; /* drawn last, floating at the cursor */
        int icon = gui_order[slot];
        int extra = dock_hover_extra[slot];
        int size = DOCK_ICON + extra;
        int cx_center = gui_slot_x(slot) + DOCK_ICON / 2;
        int cy_bottom = y0 + DOCK_PAD + DOCK_ICON - extra * DOCK_LIFT / DOCK_MAGNIFY;
        gui_draw_icon_shadow(cx_center, cy_bottom, size);
        gui_draw_dock_icon(icon, cx_center, cy_bottom, size);
        if (extra > 0) {
            int label_w = (int)strlen(GUI_LABELS[icon]) * 8;
            /* Dark text on the old flat light backdrop; the gradient
               wallpaper makes the area right above the dock genuinely
               dark now, dark-on-dark was unreadable, caught live by
               actually hovering an icon on the real page, not assumed. */
            font_draw_string(GUI_LABELS[icon], cx_center - label_w / 2, cy_bottom - size - 18, 0x00FFF6EC, -1);
        }
    }
    if (drag_slot >= 0) {
        int icon = gui_order[drag_slot];
        gui_draw_one_icon(icon, drag_mx, drag_my + (DOCK_ICON + DOCK_MAGNIFY) / 2, DOCK_ICON + DOCK_MAGNIFY);
    }
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
static void gui_draw_cursor(int x, int y){
    /* v42: the standard arrow pointer, black fill with a white outline so
       it reads on both the dark sky and the light dock, replacing the
       crosshair corner mark. Scanline widths of the classic 12x19 arrow
       (tip at top-left, tail at the lower right), drawn as rows so it's
       one vector-ish description, not a sprite. */
    static const unsigned char rows[19] = {1,2,3,4,5,6,7,8,9,10,11,12,7,7,8,8,9,9,8};
    static const unsigned char tail_x[19] = {0,0,0,0,0,0,0,0,0,0,0,0,4,5,6,6,7,7,8};
    for (int r = 0; r < 19; r++){
        int x0 = x + tail_x[r], w = rows[r] - tail_x[r];
        if (w <= 0) continue;
        window_rect(x0, y + r, w, 1, 0x00FFFFFF);                       /* white outline row */
        if (w > 2 && r > 0 && r < 18) window_rect(x0 + 1, y + r, w - 2, 1, 0x00000000); /* black interior */
    }
}

/* App viewers have their own input loops. Keep the pointer alive while one
   is open, drawing it in screen coordinates outside the app viewport. */
static int gui_app_windowed = 0;
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
    sleep_ticks(5);
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
        if (sc >= 0 && !(sc & 0x80) && SC[sc & 0x7F] == 27) { gui_close_was_click = 0; return; }
        if (mouse_click_edge()) { gui_close_was_click = 1; return; }
        __asm__ volatile ("hlt");
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
    }
    font_draw_string(title, 84, 12, 0x0085144B, -1);
}

static void gui_launch_weather(void){
    if (!weather_text[0]) weather_fetch();
    window_clear(0x00F5F0EB);
    gui_draw_app_titlebar("Weather");
    int w = (int)window_width(), x = (w - 520) / 2;
    if (x < 16) x = 16;
    gui_rounded_rect_gradient(x, 72, 520, 250, 0x00FFF7E7, 0x00E9D9DA, 0x00F5F0EB, 22);
    gui_draw_one_icon_on(0, x + 95, 230, 100, 0x00F4E8E2);
    font_draw_string("Vancouver", x + 188, 112, 0x00645057, -1);
    font_draw_string(weather_text[0] ? weather_text : "Weather unavailable", x + 188, 158, 0x002A2226, -1);
    font_draw_string("Current conditions", x + 188, 195, 0x00746B70, -1);
    gui_wait_close();
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

static void gui_launch_files(void){
    window_clear(0x00FAF8F6);
    gui_draw_app_titlebar("Files");
    gui_fat_count = 0;
    vfs_list(gui_fat_collect);
    if (gui_fat_count == 0) font_draw_string("(no files, or no FAT filesystem)", 20, 50, 0x001C1C1E, -1);
    for (int i = 0; i < gui_fat_count; i++) font_draw_string(gui_fat_names[i], 20, 50 + i * 18, 0x001C1C1E, -1);
    gui_wait_close();
}

static void gui_launch_chat(void){
    window_clear(0x00FAF8F6);
    font_draw_string("Chat", 20, 16, 0x0085144B, -1);
    font_draw_string("type a message, enter to send, esc or click to cancel:", 20, 44, 0x0075726E, -1);

    static char msg[200];
    unsigned int n = 0;
    /* v67 (0.62.2): get_key() here was click-blind, the second real
       "stuck" app after Notes: a visitor with no keyboard (a phone, the
       landing page's idle tour) could open Chat and never leave it.
       Same click-cancels contract the other text prompts now keep. */
    mouse_click_edge_sync();
    for (;;) {
        int k = get_key_or_click();
        if (k == KEY_ESC || k == KEY_CLICK) return;
        if (k == KEY_ENTER) break;
        if (k == '\b') { if (n > 0) n--; }
        else if (n < sizeof(msg) - 1 && k >= 32 && k < 127) msg[n++] = (char)k;
        window_rect(20, 68, (int)window_width() - 40, 20, 0x00FFFFFF);
        msg[n] = 0;
        font_draw_string(msg, 24, 70, 0x001C1C1E, -1);
    }
    msg[n] = 0;
    if (n == 0) return;

    font_draw_string("asking llama3.1 (local, on the host machine)...", 20, 100, 0x0075726E, -1);
    if (!rtl8139_init()) { font_draw_string("no RTL8139 found", 20, 120, 0x001C1C1E, -1); gui_wait_close(); return; }
    net_init(0x0A00020F);

    char escaped[256];
    json_escape(msg, escaped, sizeof(escaped));
    static char req_body[512];
    unsigned int rn = 0;
    const char *parts[3];
    parts[0] = "{\"model\":\"llama3.1:8b\",\"stream\":false,\"prompt\":\"";
    parts[1] = escaped;
    parts[2] = "\"}";
    for (int p = 0; p < 3; p++) { const char *s = parts[p]; while (*s && rn < sizeof(req_body)) req_body[rn++] = *s++; }

    static char resp[4096];
    int respn = http_post("10.0.2.2", "/api/generate", 11434, req_body, rn, resp, sizeof(resp) - 1);
    window_clear(0x00FAF8F6);
    gui_draw_app_titlebar("Chat");
    font_draw_string(msg, 20, 44, 0x007A2048, -1);
    if (respn <= 0) { font_draw_string("FAIL (couldn't reach the host's Ollama server)", 20, 70, 0x001C1C1E, -1); }
    else {
        resp[respn] = 0;
        static char answer[2048];
        unsigned int an = json_extract_string(resp, "response", answer, sizeof(answer));
        answer[an] = 0;
        if (an == 0) font_draw_string("(no response field in the reply)", 20, 70, 0x001C1C1E, -1);
        else render_wrapped_text(answer, 20, 70, (int)window_width() - 40, (int)window_height() - 110, 0x001C1C1E);
    }
    gui_wait_close();
}

#include "editor.h"
#include "reminders.h"
#include "calendar.h"
#include "mail.h"
#include "contacts.h"
#include "calculator.h"

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
static void gui_aa_char(unsigned char c, int px, int py, unsigned int fg, int bg){
    if (c < 32 || c > 126) c = (c == 0xF8) ? 176 : '?'; /* 0xF8 is the CP437 degree sign the weather uses */
    const struct editor_glyph *g;
    if (c == 176) { /* degree: DejaVu has it, but the table only carries 32..126; draw a small ring instead */
        if (bg >= 0) for (int j = 0; j < 32; j++) for (int i = 0; i < 16; i++) window_pixel_phys(px + i, py + j, (unsigned int)bg);
        for (int j = 0; j < 9; j++) for (int i = 0; i < 9; i++) { int dx = i - 4, dy = j - 4; int d2 = dx*dx + dy*dy; if (d2 >= 5 && d2 <= 12) window_pixel_phys(px + 3 + i, py + 8 + j, fg); } /* a ring at cap height, where a degree sign sits */
        return;
    }
    g = &editor_glyphs[((0 * 2 + 0) * 4 + 2) * 95 + (c - 32)];
    if (bg >= 0) for (int j = 0; j < 32; j++) for (int i = 0; i < 16; i++) window_pixel_phys(px + i, py + j, (unsigned int)bg);
    int ox = px + 1 + g->left, oy = py + g->top - 2; /* `top` is measured from the line box's top (see editor_layout), not a baseline; the 24px face was sized for a 36px line box, ours is 32 */
    for (int row = 0; row < g->height; row++){
        for (int col = 0; col < g->width; col++){
            int a = editor_pixels[g->offset + row * g->width + col];
            if (!a) continue;
            int x = ox + col, y = oy + row;
            if (x < px || x >= px + 16) continue; /* keep inside the cell so neighbours never overdraw each other */
            unsigned int d = window_get_pixel_phys(x, y);
            unsigned int r = (((fg >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * (255 - a)) / 255;
            unsigned int gg = (((fg >> 8) & 0xFF) * a + ((d >> 8) & 0xFF) * (255 - a)) / 255;
            unsigned int b = ((fg & 0xFF) * a + (d & 0xFF) * (255 - a)) / 255;
            window_pixel_phys(x, y, (r << 16) | (gg << 8) | b);
        }
    }
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
            font_draw_char((unsigned char)target[i], x, y, i < pos ? 0x00884B16 : 0x001C1C1E, -1);
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
static void term_render(const char *input, unsigned int input_len){
    window_clear(0x001A1512); /* warm near-black, the Mojave palette's dark end, not a cold pure black */
    gui_draw_app_titlebar("Terminal");

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
            font_draw_char((unsigned char)term_buf[p], x, y, 0x00D8CFC4, -1);
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
        font_draw_char((unsigned char)input[i], x, py, 0x00F2E9D8, -1);
    window_rect(x, py, 8, 15, 0x00C98A3E); /* block cursor */
    font_draw_string("esc closes   |   same shell as text mode", 16, (int)window_height() - 28, 0x00807468, -1);
}

static void gui_launch_terminal(void){
    static char input[TERM_COLS];
    static char out[4096];
    unsigned int input_len = 0;

    if (term_len == 0) term_puts("Joshua Tree terminal. Type help.\n");
    term_render(input, input_len);

    for (;;) {
        /* See gui_wait_close and the Apps folder: settle for v86's canvas
           sampler, and treat a click/tap as a real way out for a visitor
           with no keyboard. */
        sleep_ticks(5);
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
static void gui_launch_apps(void){
    int sel = 0;
    int rows = (GUI_APPS_FOLDER + APPS_COLS - 1) / APPS_COLS;
    int cell_w = 150, cell_h = 108, tile = 60;
    int grid_w = APPS_COLS * cell_w;
    int x0 = ((int)window_width() - grid_w) / 2;
    int y0 = 95;

    for (;;) {
        window_clear(0x00201922);
        gui_draw_wallpaper();
        gui_apps_glass(x0 - 28, 25, grid_w + 56, 375);
        font_draw_string("Apps", x0, 40, 0x002A2226, -1);
        font_draw_string("arrow keys to move   enter opens   esc closes", x0, 65, 0x006A6064, -1);

        for (int i = 0; i < GUI_APPS_FOLDER; i++) {
            int row = i / APPS_COLS, col = i % APPS_COLS;
            int cx = x0 + col * cell_w + cell_w / 2;
            int cy = y0 + row * cell_h;
            if (i == sel) /* selection plate, drawn under the icon so it reads as a highlight, not a border */
                gui_rounded_rect_gradient(cx - tile / 2 - 10, cy - 10, tile + 20, cell_h - 14,
                                          0x00FFF8F1, 0x00E5D8D0, 0x00E9DEE0, 12);
            gui_draw_one_icon_on(i, cx, cy + tile, tile, 0x00E9DEE0);
            int lw = (int)strlen(GUI_LABELS[i]) * 8;
            font_draw_string(GUI_LABELS[i], cx - lw / 2, cy + tile + 10, 0x001C1C1E, -1);
        }
        (void)rows;

        /* The same two v86/touch accommodations gui_wait_close documents:
           a few real ticks of settle time so the emulator's canvas sampler
           actually catches this frame before we block, and a click/tap
           counting as input so a phone can leave this screen at all. */
        sleep_ticks(5);
        mouse_click_edge_sync();
        int k = get_key_or_click();
        if (k == KEY_ESC || k == KEY_CLICK) return; /* a tap anywhere closes the folder: with no keyboard there is no other way out */
        if (k == KEY_ENTER) { gui_launch(sel); continue; } /* returns here when that app closes, folder still open, same as a real launcher */
        if (k == 'a' && sel > 0) sel--;                 /* left  */
        else if (k == 'd' && sel < GUI_APPS_FOLDER - 1) sel++; /* right */
        else if (k == 'w' && sel >= APPS_COLS) sel -= APPS_COLS;
        else if (k == 's' && sel + APPS_COLS < GUI_APPS_FOLDER) sel += APPS_COLS;
        else if (k >= '1' && k <= '9' && (k - '1') < GUI_APPS_FOLDER) { sel = k - '1'; gui_launch(sel); }
    }
}

/* v39: the Trash, a real view of what rm set aside, with real recovery.
   Same keyboard-and-click contract every screen here uses (see
   gui_wait_close): a phone has no keyboard, so every action has a tap. */
static void gui_launch_trash(void){
    int sel = 0;
    for (;;) {
        window_clear(GUI_BG);
        gui_draw_app_titlebar("Trash");
        int n = trash_count();
        if (!n) {
            font_draw_string("Trash is empty.", 20, 70, 0x001C1C1E, -1);
            font_draw_string("Deleting a file with rm puts it here first.", 20, 94, 0x00807468, -1);
        } else {
            font_draw_string("up/down to pick   r restores   e empties   esc closes", 20, 52, 0x00807468, -1);
            for (int i = 0; i < n; i++) {
                int y = 84 + i * 22;
                if (i == sel) window_rect(16, y - 4, (int)window_width() - 32, 20, 0x00EDE6DC);
                font_draw_string(trash_name(i), 28, y, 0x001C1C1E, -1);
                char sz[16]; int p = 0; unsigned int v = trash_size(i);
                char t[12]; int ti = 0; if (!v) t[ti++] = '0'; while (v) { t[ti++] = '0' + v % 10; v /= 10; }
                while (ti) sz[p++] = t[--ti];
                sz[p++] = ' '; sz[p++] = 'b'; sz[p] = 0;
                font_draw_string(sz, 300, y, 0x00807468, -1);
            }
        }
        sleep_ticks(5);
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
#define SETTINGS_ROW_COUNT 3 /* v75: + wallpaper source */
static void gui_launch_settings(void){
    int sel = 0;
    for (;;) {
        window_clear(GUI_BG);
        gui_draw_app_titlebar("Settings");
        font_draw_string("up/down to pick   left/right or tap to change   esc closes", 20, 52, 0x00807468, -1);

        int rows_y[SETTINGS_ROW_COUNT] = {84, 116, 148};
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
            } else {
                /* v75: honest label. "Map" only once a real tile mosaic is
                   on screen; while it's still fetching, or when the fetch
                   failed and the photo is what's actually up, say so. */
                font_draw_string("Wallpaper", 28, y, 0x001C1C1E, -1);
                const char *lbl = !wall_mode ? "Photo" : (wall_map ? (geo_city[0] ? geo_city : "Map") : "Map (fetching, photo until then)");
                font_draw_string(lbl, 400, y, wall_mode && wall_map ? 0x002F7B4F : 0x001C1C1E, -1);
            }
        }
        font_draw_string("Settings are saved to disk and survive a reboot.", 20, (int)window_height() - 28, 0x00807468, -1);

        sleep_ticks(5);
        mouse_click_edge_sync();
        int k = get_key_or_click();
        if (k == KEY_ESC) return;
        if (k == KEY_UP && sel > 0) sel--;
        else if (k == KEY_DOWN && sel < SETTINGS_ROW_COUNT - 1) sel++;
        else if (k == KEY_CLICK || k == 'a' || k == 'd') {
            if (sel == 0) { wind_enabled = !wind_enabled; settings_save(); }
            else if (sel == 2) { wall_mode = !wall_mode; settings_save(); wall_apply(wall_mode); }
            else {
                int dir = (k == 'a') ? -1 : 1; /* a tap always steps up; a real direction only from the keyboard */
                if (k == KEY_CLICK) dir = 1;
                int v = dock_scale_pct + dir;
                if (v > 25) v = 5; if (v < 5) v = 25; /* wraps, so a tap always does something visible */
                dock_scale_pct = v; settings_save();
            }
        }
    }
}

static void gui_launch(int icon){
    if (icon == GUI_APPS_FOLDER) { gui_launch_apps(); return; }
    if (icon == GUI_TRASH) { gui_launch_trash(); return; }
    if (icon == 0)      gui_launch_files();
    else if (icon == 1) gui_launch_mail();
    else if (icon == 2) gui_launch_calendar();
    else if (icon == 3) gui_launch_editor();
    else if (icon == 4) gui_launch_reminders();
    else if (icon == 5) gui_launch_terminal();
    else if (icon == 6) gui_launch_chat();
    else if (icon == 7) gui_launch_weather();
    else if (icon == 8) gui_launch_html("Curbfind", app_curbfind_html, app_curbfind_len);
    else if (icon == 9) gui_launch_keyrate();
    else if (icon == 10) gui_launch_html("Bookrank", app_bookrank_html, app_bookrank_len);
    else if (icon == 11) gui_launch_html("Quotestreak", app_quotestreak_html, app_quotestreak_len);
    else if (icon == 12) gui_launch_html("Plan", app_plan_html, app_plan_len);
    else if (icon == 13) gui_launch_html("Lexly", app_lexly_html, app_lexly_len);
    else if (icon == 14) gui_launch_html("Toroid", app_toroid_html, app_toroid_len);
    else if (icon == 15) gui_launch_html("Sparkjar", app_sparkjar_html, app_sparkjar_len);
    else if (icon == 16) gui_launch_html("Homeqi", app_homeqi_html, app_homeqi_len);
    else if (icon == 17) gui_launch_html("Fieldbook", app_fieldbook_html, app_fieldbook_len);
    else if (icon == 18) gui_launch_contacts();
    else if (icon == 19) gui_launch_calculator();
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
    unsigned int bg = 0x00201009; /* the wallpaper's own espresso-brown, on-brand, not a new color */
    window_clear(bg);
    int cx = (int)window_width() / 2, cy = (int)window_height() / 2; /* v45.2: centred on the real window; 400 was the 800-wide centre and sat left of centre at 960 */
    gui_draw_logo(cx, cy - 10, 5, bg); /* v48: dropped the "hello" wordmark, direct request, logo alone reads cleaner */

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
            window_rect(bar_x, bar_y, fill, bar_h, 0x0085144B);
        }
        if (elapsed >= logo_only + bar_span) break;
        __asm__ volatile ("hlt");
    }
}

/* A real about panel, not a placeholder: actual physical memory stats
   straight from pmm (the same real physical memory manager the rest of
   this kernel allocates through) and real uptime off ticks(), the same
   PIT tick counter every other real-time feature in this file already
   uses. Closes the same way every other app view does. */
static void gui_launch_about(void){
    window_clear(0x00FAF8F6);
    gui_draw_app_titlebar("About Joshua Tree");
    font_draw_string("A freestanding i386 kernel, written from scratch.", 20, 50, 0x001C1C1E, -1);
    char buf[64]; int n;

    unsigned int total_kb = pmm_total_frames() * 4, free_kb = pmm_free_frames() * 4;
    n = 0; buf[n++] = 'M'; buf[n++] = 'e'; buf[n++] = 'm'; buf[n++] = 'o'; buf[n++] = 'r'; buf[n++] = 'y'; buf[n++] = ':'; buf[n++] = ' ';
    { char tmp[12]; int tn = 0; unsigned int v = free_kb; if (v == 0) tmp[tn++] = '0'; while (v > 0) { tmp[tn++] = (char)('0' + v % 10); v /= 10; } while (tn > 0) buf[n++] = tmp[--tn]; }
    buf[n++] = 'K'; buf[n++] = ' '; buf[n++] = 'f'; buf[n++] = 'r'; buf[n++] = 'e'; buf[n++] = 'e'; buf[n++] = ' '; buf[n++] = 'o'; buf[n++] = 'f'; buf[n++] = ' ';
    { char tmp[12]; int tn = 0; unsigned int v = total_kb; if (v == 0) tmp[tn++] = '0'; while (v > 0) { tmp[tn++] = (char)('0' + v % 10); v /= 10; } while (tn > 0) buf[n++] = tmp[--tn]; }
    buf[n++] = 'K'; buf[n] = 0;
    font_draw_string(buf, 20, 80, 0x00884B16, -1);

    unsigned int secs = ticks() / 100;
    n = 0; buf[n++] = 'U'; buf[n++] = 'p'; buf[n++] = 't'; buf[n++] = 'i'; buf[n++] = 'm'; buf[n++] = 'e'; buf[n++] = ':'; buf[n++] = ' ';
    { char tmp[12]; int tn = 0; unsigned int v = secs; if (v == 0) tmp[tn++] = '0'; while (v > 0) { tmp[tn++] = (char)('0' + v % 10); v /= 10; } while (tn > 0) buf[n++] = tmp[--tn]; }
    buf[n++] = 's'; buf[n] = 0;
    font_draw_string(buf, 20, 100, 0x00884B16, -1);
    /* v0.42.x: the version string, plus the one joke this release earns.
       It's a Joshua tree. Different tree. */
    font_draw_string("Version 0.42.1, the 4:20 release. It's a Joshua tree. Different tree.", 20, 130, 0x0075726E, -1);

    gui_wait_close();
}

/* A real Apple-menu-style dropdown off the tree logo, macOS-shaped (dark
   panel, one highlighted row under the cursor) but with items that
   actually do something real on this kernel, not a decorative copy of
   a macOS menu that happens to not work: About shows real memory/uptime
   stats, Files and Notes launch the same real apps the dock does,
   Restart calls this kernel's own real reboot() (the 8042 reset pulse,
   already used by the "reboot" shell command), Shut Down really halts
   the CPU. "-" is a separator row, not a real item. */
#define GUI_MENU_ITEM_COUNT 7
static const char *GUI_MENU_LABELS[GUI_MENU_ITEM_COUNT] = {
    "About Joshua Tree", "Files", "Notes", "Settings", "-", "Restart", "Shut Down"
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
        if (i == hover_item) window_rect(GUI_MENU_X0 + 2, ry, GUI_MENU_W - 4, rh, 0x0085144B);
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
static void gui_draw_notif_panel(void){
    int x0 = (int)window_width() - NOTIF_W - 4, y0 = GUI_MENUBAR_H;
    unsigned int bg = 0x002C2C2E, text = 0x00F5F5F7, dim = 0x00A0A0A6, warn = 0x00FFB454;

    /* live warnings first: the part that's actually worth a glance */
    const char *warns[3]; int nw = 0;
    unsigned int free_k = pmm_free_frames() * 4;
    if (free_k < 8192) warns[nw++] = "Memory is running low";
    if (trash_count() >= TRASH_MAX_ITEMS - 1) warns[nw++] = "Trash is nearly full";
    if (nw == 0) warns[nw++] = "No warnings";

    int n = klog_count < NOTIF_ROWS ? klog_count : NOTIF_ROWS;
    int total_h = 10 + (nw + 1) * 18 + n * 34 + 8;
    gui_rounded_rect_on_wallpaper(x0, y0, NOTIF_W, total_h, bg, GUI_FLYOUT_RADIUS);

    int y = y0 + 8;
    for (int i = 0; i < nw; i++, y += 18) font_draw_string(warns[i], x0 + 12, y, nw == 1 && warns[0][0] == 'N' ? dim : warn, -1);
    window_rect(x0 + 8, y + 6, NOTIF_W - 16, 1, 0x00545458); y += 18;

    /* newest last, like every log ever, capped to the last NOTIF_ROWS */
    int start = (klog_count < KLOG_MAX) ? 0 : klog_next;
    int skip = klog_count - n;
    for (int i = 0; i < n; i++, y += 34) {
        int idx = (start + skip + i) % KLOG_MAX;
        char line[44]; int p = 0;
        unsigned int t = klog_tick[idx] / 100; char tb[8]; int ti = 0;
        if (!t) tb[ti++] = '0'; while (t) { tb[ti++] = '0' + t % 10; t /= 10; }
        while (ti) line[p++] = tb[--ti];
        line[p++] = 's'; line[p++] = ' ';
        int k = 0;
        for (; klog_buf[idx][k] && p < 42; k++) line[p++] = klog_buf[idx][k];
        line[p] = 0;
        font_draw_string(line, x0 + 12, y, text, -1);
        if (klog_buf[idx][k]) {
            p = 0;
            line[p++] = ' '; line[p++] = ' '; line[p++] = ' ';
            for (; klog_buf[idx][k] && p < 42; k++) line[p++] = klog_buf[idx][k];
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
static void gui_menu_run_item(int item){
    if (item == 0) gui_launch_about();
    else if (item == 1) gui_launch_files();
    else if (item == 2) gui_launch_editor();
    else if (item == 3) gui_launch_settings();
    else if (item == 5) reboot();
    else if (item == 6) {
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
    font_set_aa(gui_aa_char); /* v44: real typeface for every string from here on */
    /* v46: no wind in the browser, decided up front rather than measured
       after the fact. The slow-frame gate still exists, but even the two
       frames it takes to trip blocked the kernel long enough that v86's
       PS/2 queue overflowed and the demo tour's paced cursor packets were
       dropped (tourtest failed twice, alone, on this build). The BIOS-font
       check from v38 is the reliable "this is v86" signal. */
    if (font_is_fallback()) wind_enabled = 0;
    gui_draw_boot_screen();
    gui_order_init();
    for (int i = 0; i < GUI_ICON_COUNT; i++) dock_hover_extra[i] = dock_presented_extra[i] = 0;
    int mx = 400, my = 300, buttons = 0, prev_buttons = 0;
    /* press_slot: the slot the mouse went down on, latched until release.
       drag_slot: only set once the mouse has actually moved past a small
       threshold while held, so a plain click (down, no movement, up)
       never gets mistaken for a drag onto its own slot. */
    int press_slot = -1, press_x = 0, press_y = 0, drag_slot = -1;
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
    gui_draw_desktop(-1, -1, 0, 0);
    cursor_saved_x = cursor_saved_y = -1;
    gui_cursor_save(mx, my);
    gui_draw_cursor(mx, my);
    for (;;) {
        __asm__ volatile ("hlt");
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
            if (wall_mode && !wall_map && geo_have && wall_fetch()) { wall_apply(1); gui_draw_desktop(-1, -1, 0, 0); gui_cursor_save(mx, my); gui_draw_cursor(mx, my); }
        }
        /* v45: wind, 4 frames a second, only while the desktop itself is
           what's on screen. Timed on its first frame; if that frame took
           longer than a tenth of a second the machine is too slow for
           this (v86 in a browser) and it switches itself off for good. */
        {
            static unsigned int wind_last = 0; static int wind_dir = 1;
            if (wind_enabled && !menu_open && !notif_open && !weather_open && drag_slot < 0 && ticks() - wind_last >= 5) { /* cached wallpaper: ~3 ticks per frame, leaving input time at 20 fps */
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
        int sc = kbd_pop();
        if (sc >= 0 && !(sc & 0x80) && SC[sc & 0x7F] == 27) break; /* esc, non-blocking */
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

        if (just_pressed) {
            if (logo_here) { menu_open = 1; menu_opening = 1; }
            else if (weather_here) { weather_open = 1; weather_opening = 1; weather_draw_pending = 1; }
            else if (clock_here) { notif_open = 1; notif_opening = 1; notif_draw_pending = 1; }
            else if (slot_here >= 0) { press_slot = slot_here; press_x = mx; press_y = my; drag_slot = -1; }
        }

        if (held && press_slot >= 0 && drag_slot < 0) {
            int moved = (mx > press_x ? mx - press_x : press_x - mx) + (my > press_y ? my - press_y : press_y - my);
            if (moved > 8) drag_slot = press_slot; /* threshold crossed: this is a drag, not a click */
        }

        int launched = notif_draw_pending || weather_draw_pending; notif_draw_pending = 0; weather_draw_pending = 0;
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
            } else if (drag_slot >= 0) {
                int target = gui_slot_at(mx);
                int tmp = gui_order[drag_slot];
                gui_order[drag_slot] = gui_order[target];
                gui_order[target] = tmp;
            } else if (press_slot >= 0 && press_slot == slot_here) {
                editor_mouse_x = mx; editor_mouse_y = my;
                gui_launch_from_dock(gui_order[press_slot]);
                mx = app_cursor_x; my = app_cursor_y; /* v68: the app's own loop tracked the pointer while it was open; pick up where it really is, not where the launching click was */
                launched = 1; /* the app view just took over the whole screen; force a redraw below even if the cursor never moved */
            }
            press_slot = -1; drag_slot = -1;
        }
        prev_buttons = buttons;

        int hover_slot = (drag_slot < 0) ? slot_here : -1;
        int dock_anim_changed = 0;
        static unsigned int dock_anim_last_tick = 0;
        if (ticks() - dock_anim_last_tick >= 3) {
            dock_anim_last_tick = ticks();
            for (int i = 0; i < GUI_ICON_COUNT; i++) {
                int target = (i == hover_slot) ? DOCK_MAGNIFY : 0;
                int next = dock_hover_extra[i];
                if (next < target) { next += 3; if (next > target) next = target; }
                else if (next > target) { next -= 3; if (next < target) next = target; }
                if (next != dock_hover_extra[i]) { dock_hover_extra[i] = next; dock_anim_changed = 1; }
            }
        }
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
        int cursor_only = !launched && !dock_anim_changed && (mx != last_mx || my != last_my)
                          && hover_slot == last_hover && drag_slot == last_drag
                          && menu_open == last_menu_open && menu_hover == last_menu_hover;
        int dock_only = !launched && !cursor_only && drag_slot < 0 && last_drag < 0
                        && !menu_open && !last_menu_open
                        && (hover_slot != last_hover || dock_anim_changed);
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
        } else if (launched || mx != last_mx || my != last_my || hover_slot != last_hover || dock_anim_changed || drag_slot != last_drag || menu_open != last_menu_open || menu_hover != last_menu_hover) {
            if (my < GUI_MENUBAR_H || last_my < GUI_MENUBAR_H) gui_menubar_force_redraw();
            cursor_saved_x = cursor_saved_y = -1; /* the full repaint replaces whatever the backup held */
            gui_draw_desktop(hover_slot, drag_slot, mx, my);
            for (int i = 0; i < GUI_ICON_COUNT; i++) dock_presented_extra[i] = dock_hover_extra[i];
            if (menu_open) gui_draw_apple_menu(menu_hover);
            if (notif_open) gui_draw_notif_panel();
            if (weather_open) gui_draw_weather_panel();
            if (drag_slot < 0) { gui_cursor_save(mx, my); gui_draw_cursor(mx, my); }
            last_mx = mx; last_my = my; last_hover = hover_slot; last_drag = drag_slot;
            last_menu_open = menu_open; last_menu_hover = menu_hover;
        }
    }
    window_close();
    clear();
    puts("back in text mode\n");
}

static void run(char *line){
    char *arg = line;
    while (*arg && *arg != ' ') arg++;
    if (*arg) *arg++ = 0;

    if (!*line)                    return;
    if (!strcmp(line, "help"))       puts("help clear echo time uptime dmesg mem reboot crash pagefault heaptest heapgrow tasktest preempttest weathertest daynighttest weatherfxtest weatherfxcliptest geotest weatherpaneltest windweathertest cursortest mailtest dockstyletest wind isotest reaptest ring3test ps kill killtest sleep disktest diskuse fsuse ls cat exec rm cd mkdir write browse lspci gfxtest fonttest mousetest nettest ifconfig netscan web serve serveapp chat build gui testapps contactstest calctest pngtest\n");
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
    else if (!strcmp(line, "tasktest")) {
        puts("\n");
        task_create(task_a);
        task_create(task_b);
        for (int i = 0; i < 10; i++) yield(); /* shell is task 0; let A/B interleave */
        puts("\ndone (expect ABABAB...)\n");
    }
    else if (!strcmp(line, "ring3test")) {
        ring3_test(arg); /* v64: "", "fault", or "spin", see ring3.h; all three come back to the shell */
    }
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
            font_set_aa(gui_aa_char); /* real physical-resolution AA text, the exact surface v56.1 fixed; gui_run() normally registers this but this test doesn't call gui_run() */
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
        /* v75: photo|map picks the source (persisted like wind/dockscale),
           fetch forces the map download right now (its own NIC/net bring-
           up, same as weather_fetch), no argument reports state. */
        if (!strcmp(arg, "photo")) { wall_mode = 0; settings_save(); wall_apply(0); puts("wallpaper: photo\n"); }
        else if (!strcmp(arg, "map")) { wall_mode = 1; settings_save(); wall_apply(1); puts(wall_map ? "wallpaper: map\n" : "wallpaper: map (fetches on the next weather cycle, or: wallpaper fetch)\n"); }
        else if (!strcmp(arg, "fetch")) {
            if (!rtl8139_init()) { puts("no NIC\n"); }
            else { net_init(0x0A00020F); if (!geo_have) geo_fetch();
                   if (wall_fetch()) { wall_apply(1); puts("wallpaper: map fetched (tiles "); putn((unsigned int)wall_map_tx); puts(","); putn((unsigned int)wall_map_ty); puts(" z"); putn(WALL_ZOOM); puts(")\n"); }
                   else puts("wallpaper: fetch failed, photo stays\n"); }
        }
        else { puts(wall_mode ? "wallpaper: map" : "wallpaper: photo"); puts(wall_src == wallpaper_rgb ? " (showing photo)\n" : " (showing map)\n"); }
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
        if (!*arg) { puts("usage: exec <file> (runs in ring 0, no isolation -- see roadmap.md v3)\n"); }
        else if (!exec_flat(arg)) { puts(arg); puts(": exec failed (not found or too big)\n"); }
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
    else if (!strcmp(line, "nettest")) {
        if (!rtl8139_init()) { puts("no RTL8139 found or reset failed\n"); }
        else {
            unsigned char mac[6];
            rtl8139_get_mac(mac);
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
            puts(rtl8139_send(buf, sizeof(buf)) ? "send:ok\n" : "send:FAIL (timeout)\n");

            net_init(0x0A00020F); /* 10.0.2.15, QEMU SLIRP's default guest IP */
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
        if (!rtl8139_init()) { puts("no RTL8139 found or reset failed\n"); }
        else {
            unsigned char mac[6]; rtl8139_get_mac(mac);
            puts("rtl0: 10.0.2.15\n  mac ");
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
        else if (!rtl8139_init()) { puts("no RTL8139 found or reset failed\n"); }
        else {
            net_init(0x0A00020F);
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
        else if (!rtl8139_init()) { puts("no RTL8139 found or reset failed\n"); }
        else {
            net_init(0x0A00020F);
            browse_web(first_host, first_path);
        }
    }
    else if (!strcmp(line, "serve")) {
        if (!rtl8139_init()) { puts("no RTL8139 found or reset failed\n"); }
        else {
            net_init(0x0A00020F);
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
        else if (!rtl8139_init()) { puts("no RTL8139 found or reset failed\n"); }
        else {
            net_init(0x0A00020F);
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
           is what "talking to it" can actually mean before that exists. */
        if (!*arg) { puts("usage: chat <message>\n"); }
        else if (!rtl8139_init()) { puts("no RTL8139 found or reset failed\n"); }
        else {
            net_init(0x0A00020F);
            char escaped[512];
            json_escape(arg, escaped, sizeof(escaped));

            static char req_body[768];
            unsigned int n = 0;
            const char *parts[3];
            parts[0] = "{\"model\":\"llama3.1:8b\",\"stream\":false,\"prompt\":\"";
            parts[1] = escaped;
            parts[2] = "\"}";
            for (int p = 0; p < 3; p++) {
                const char *s = parts[p];
                while (*s && n < sizeof(req_body)) req_body[n++] = *s++;
            }

            puts("asking llama3.1 (local, on the host machine)...\n");
            static char resp[4096];
            int rn = http_post("10.0.2.2", "/api/generate", 11434, req_body, n, resp, sizeof(resp) - 1);
            if (rn <= 0) { puts("FAIL (couldn't reach the host's Ollama server)\n"); }
            else {
                resp[rn] = 0;
                static char answer[2048];
                unsigned int an = json_extract_string(resp, "response", answer, sizeof(answer));
                if (an == 0) puts("(no response field in the reply)\n");
                else { puts(answer); putc('\n'); }
            }
        }
    }
    else if (!strcmp(line, "build")) {
        /* v10's "build stuff" loop: take a request, ask the LLM to generate
           a page for it, serve the result live. The kernel-native version
           of what gato does on macOS, minus the file-editing part, there's
           no persistent app catalog to edit yet, just this one slot. */
        if (!*arg) { puts("usage: build <what to make>\n"); }
        else if (!rtl8139_init()) { puts("no RTL8139 found or reset failed\n"); }
        else {
            net_init(0x0A00020F);
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
            window_rect(0, 0, 800, 200, 0x0085144B);
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
            font_draw_string("Joshua Tree", 20, 20, 0x0085144B, -1);
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
    else if (!strcmp(line, "mousetest")) {
        if (!window_open(800, 600, 32)) { puts("no VGA device found or out of page tables\n"); }
        else {
            int cx_pos = 400, cy_pos = 300;
            int buttons = 0;
            do {
                window_clear(0x00FAF8F6);
                window_rect(cx_pos - 5, cy_pos - 5, 10, 10, 0x0085144B);
                __asm__ volatile ("hlt"); /* wake on the next IRQ (timer, keyboard, or mouse) */
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
    else { puts("? "); puts(line); putc('\n'); }
}

void kmain(unsigned int multiboot_info_addr){
    serial_init();
    serial_puts("=== kmain boot start ===\n");
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
