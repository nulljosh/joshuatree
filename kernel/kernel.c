/* Freestanding i386 kernel: VGA text, PS/2 keyboard, RTC clock, tiny shell. */
#include "gdt.h"
#include "idt.h"
#include "irq.h"
#include "pmm.h"
#include "paging.h"
#include "kheap.h"
#include "task.h"
#include "ring3.h"
#include "ata.h"
#include "fat.h"
#include "vfs.h"
#include "blockdev.h"
#include "ramdisk.h"
#include "ramfs.h"
#include "exec.h"
#include "libc.h"
#include "pci.h"
#include "vbe.h"
#include "mouse.h"
#include "window.h"
#include "font.h"
#include "rtl8139.h"
#include "net.h"
#include "http.h"
#include "wallpaper.h"
#include "serial.h"
#include "app_weather.h"
#include "app_curbfind.h"
#include "app_keyrate.h"
#include "app_bookrank.h"
#include "app_quotestreak.h"
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

static char getch(void){
    for (;;) {
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
static int gui_getch_or_click(void){
    mouse_click_edge_sync(); /* a button already held (e.g. the click that opened this app) is the baseline, not a fresh click */
    for (;;) {
        int sc = kbd_pop();
        if (sc >= 0) {
            if (sc & 0x80) continue;
            char c = SC[sc & 0x7F];
            if (c) return (int)(unsigned char)c;
            continue;
        }
        if (mouse_click_edge()) return -1;
        __asm__ volatile ("hlt");
    }
}

/* ---- extended keys (arrows) for the file browser. 0xE0 is the make-code
   prefix for the "extended" keyboard block; 0x48/0x50 are up/down within it. ---- */
#define KEY_UP    256
#define KEY_DOWN  257
#define KEY_ENTER 258
#define KEY_ESC   259

static int get_key(void){
    for (;;) {
        int sc = kbd_pop();
        if (sc < 0) { __asm__ volatile ("hlt"); continue; }
        if (sc == 0xE0) {
            int sc2;
            do { sc2 = kbd_pop(); if (sc2 < 0) __asm__ volatile ("hlt"); } while (sc2 < 0);
            if (sc2 == 0x48) return KEY_UP;
            if (sc2 == 0x50) return KEY_DOWN;
            continue; /* other extended keys: ignore */
        }
        if (sc & 0x80) continue;
        char c = SC[sc & 0x7F];
        if (c == '\n') return KEY_ENTER;
        if (c == 27)   return KEY_ESC;
        if (c) return c;
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
#define GUI_ICON_COUNT 8
static const char *GUI_LABELS[GUI_ICON_COUNT] = {"Weather", "Curbfind", "Chat", "Files", "Keyrate", "Bookrank", "Quotes", "Notes"};
static const unsigned int GUI_COLORS[GUI_ICON_COUNT] = {
    0x0085144B, 0x007A2048, 0x00365E8C, 0x00707070, 0x00B08900, 0x002F7B4F, 0x008B4A9C, 0x006B4423
};

/* gui_order is a permutation of icon indices by dock slot: dragging an icon
   and dropping it on another slot swaps the two, so the arrangement is
   real and sticks for the rest of this GUI session (reset to launch order
   next time `gui` runs; nothing about layout is saved to disk, matching
   this whole desktop's one-screen, nothing-persisted scope). */
static int gui_order[GUI_ICON_COUNT];
static void gui_order_init(void){ for (int i = 0; i < GUI_ICON_COUNT; i++) gui_order[i] = i; }

#define GUI_BG          0x00FAF8F6
#define GUI_MENUBAR_H   30
#define DOCK_ICON       56
#define DOCK_GAP        16
#define DOCK_PAD        12
#define DOCK_MARGIN_BOT 24
#define DOCK_MAGNIFY    12
#define DOCK_LIFT       10

static int gui_dock_w(void){ return GUI_ICON_COUNT * DOCK_ICON + (GUI_ICON_COUNT - 1) * DOCK_GAP + 2 * DOCK_PAD; }
static int gui_dock_x0(void){ return ((int)window_width() - gui_dock_w()) / 2; }
static int gui_dock_y0(void){ return (int)window_height() - DOCK_ICON - 2 * DOCK_PAD - DOCK_MARGIN_BOT; }
static int gui_slot_x(int slot){ return gui_dock_x0() + DOCK_PAD + slot * (DOCK_ICON + DOCK_GAP); }

/* Which dock slot a point falls in, clamped to the nearest end rather than
   returning "none": once a drag has started, the icon should track the
   cursor even past the dock's own edge, the same way a real dock does. */
static int gui_slot_at(int mx){
    int rel = mx - (gui_dock_x0() + DOCK_PAD) - DOCK_ICON / 2;
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
   things blending toward "whatever's roughly there"). */
static unsigned int gui_wallpaper_color(int row){
    int area_h = (int)window_height() - GUI_MENUBAR_H;
    int r = row - GUI_MENUBAR_H;
    if (r < 0) r = 0;
    if (area_h < 1) area_h = 1;
    if (r >= area_h) r = area_h - 1;
    int sy = r * WALLPAPER_H / area_h;
    if (sy >= WALLPAPER_H) sy = WALLPAPER_H - 1;
    const unsigned char *p = &wallpaper_rgb[(sy * WALLPAPER_W + WALLPAPER_W / 2) * 3];
    return ((unsigned int)p[0] << 16) | ((unsigned int)p[1] << 8) | p[2];
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
#define AA_BAND 5

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
static void gui_rounded_rect_on_wallpaper(int x, int y, int w, int h, unsigned int color, int r){
    window_rect(x, y, w, h, color);
    int outer2 = (r + AA_BAND) * (r + AA_BAND);
    for (int dy = 0; dy <= r + AA_BAND; dy++){
        unsigned int bg_top = gui_wallpaper_color(y + dy);
        unsigned int bg_bot = gui_wallpaper_color(y + h - 1 - dy);
        for (int dx = 0; dx <= r + AA_BAND; dx++){
            int d2 = dx * dx + dy * dy;
            if (d2 <= r * r) continue;
            if (d2 > outer2) {
                window_pixel(x + dx,         y + dy,         bg_top);
                window_pixel(x + w - 1 - dx, y + dy,         bg_top);
                window_pixel(x + dx,         y + h - 1 - dy, bg_bot);
                window_pixel(x + w - 1 - dx, y + h - 1 - dy, bg_bot);
                continue;
            }
            int t = gui_isqrt(d2) - r;
            window_pixel(x + dx,         y + dy,         gui_lerp(color, bg_top, t, AA_BAND));
            window_pixel(x + w - 1 - dx, y + dy,         gui_lerp(color, bg_top, t, AA_BAND));
            window_pixel(x + dx,         y + h - 1 - dy, gui_lerp(color, bg_bot, t, AA_BAND));
            window_pixel(x + w - 1 - dx, y + h - 1 - dy, gui_lerp(color, bg_bot, t, AA_BAND));
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
    int outer2 = (r + AA_BAND) * (r + AA_BAND);
    for (int dy = 0; dy <= r + AA_BAND; dy++){
        for (int dx = 0; dx <= r + AA_BAND; dx++){
            int d2 = dx * dx + dy * dy;
            if (d2 <= r * r) continue;
            unsigned int top_local = gui_lerp(color_top, color_bottom, dy, h);
            unsigned int bot_local = gui_lerp(color_top, color_bottom, h - 1 - dy, h);
            if (d2 > outer2) {
                window_pixel(x + dx,         y + dy,         bg);
                window_pixel(x + w - 1 - dx, y + dy,         bg);
                window_pixel(x + dx,         y + h - 1 - dy, bg);
                window_pixel(x + w - 1 - dx, y + h - 1 - dy, bg);
                continue;
            }
            int t = gui_isqrt(d2) - r;
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


static void gui_draw_wallpaper(void){
    int w = (int)window_width(), h = (int)window_height();
    int area_h = h - GUI_MENUBAR_H;
    /* A real 2D blit of the embedded photo (nearest-neighbor, this
       framebuffer has no scaling hardware and this kernel has no
       resampling filter to spare), not the single-column sample
       gui_wallpaper_color above uses for blend targets: this is what
       actually shows on screen, that one only needs to be close. */
    for (int row = 0; row < area_h; row++){
        int sy = row * WALLPAPER_H / (area_h > 0 ? area_h : 1);
        if (sy >= WALLPAPER_H) sy = WALLPAPER_H - 1;
        const unsigned char *src_row = &wallpaper_rgb[sy * WALLPAPER_W * 3];
        for (int col = 0; col < w; col++){
            int sx = col * WALLPAPER_W / w;
            if (sx >= WALLPAPER_W) sx = WALLPAPER_W - 1;
            const unsigned char *p = &src_row[sx * 3];
            window_pixel(col, GUI_MENUBAR_H + row, ((unsigned int)p[0] << 16) | ((unsigned int)p[1] << 8) | p[2]);
        }
    }

    /* Direct feedback after living with both a while: the 8-bit pixel
       trees and the desktop's own echo of "hello" are gone now, real
       photo speaks for itself, and "hello" stays a one-time boot moment
       (gui_draw_boot_screen) rather than repeating on every desktop
       frame. Removed rather than left disabled behind a flag, nothing
       here needs to come back on short notice. */
}

/* Fills a downward-pointing triangle: flat top of half-width `half_w` at
   (cx, y0), narrowing to a point over `h` rows. Used for the map pin's tip
   and the quote marks' tails. */
static void gui_fill_triangle_down(int cx, int y0, int half_w, int h, unsigned int color){
    for (int row = 0; row < h; row++){
        int w = half_w - (half_w * row) / h;
        if (w < 0) w = 0;
        window_rect(cx - w, y0 + row, 2 * w + 1, 1, color);
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
static void gui_draw_menubar(void){
    u8 h = cmos(4), m = cmos(2), wd = cmos(6), dom = cmos(7), mon = cmos(8);
    u8 hv = (h & 0x0F) + ((h >> 4) * 10), mv = (m & 0x0F) + ((m >> 4) * 10);
    u8 wdv = (wd & 0x0F) + ((wd >> 4) * 10);
    u8 domv = (dom & 0x0F) + ((dom >> 4) * 10);
    u8 monv = (mon & 0x0F) + ((mon >> 4) * 10);
    if (mv == gui_menubar_last_min) return;
    gui_menubar_last_min = mv;

    window_rect(0, 0, (int)window_width(), GUI_MENUBAR_H, 0x00FFFFFF);
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
    unsigned int sun_top = 0x00FFE380, sun_bot = 0x00FFA716; /* warm gold, a real color, not flat white */
    gui_fill_circle_gradient(cx, cy, r, sun_top, sun_bot, bg);
    gui_draw_capsule(cx, cy - gap,         cx, cy - gap - ray,         2, ICON_FG, bg);
    gui_draw_capsule(cx, cy + gap,         cx, cy + gap + ray,         2, ICON_FG, bg);
    gui_draw_capsule(cx - gap,       cy,   cx - gap - ray,       cy,   2, ICON_FG, bg);
    gui_draw_capsule(cx + gap,       cy,   cx + gap + ray,       cy,   2, ICON_FG, bg);
    gui_draw_capsule(cx - gap, cy - gap,   cx - gap - diag, cy - gap - diag, 2, ICON_FG, bg);
    gui_draw_capsule(cx + gap, cy - gap,   cx + gap + diag, cy - gap - diag, 2, ICON_FG, bg);
    gui_draw_capsule(cx - gap, cy + gap,   cx - gap - diag, cy + gap + diag, 2, ICON_FG, bg);
    gui_draw_capsule(cx + gap, cy + gap,   cx + gap + diag, cy + gap + diag, 2, ICON_FG, bg);
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
    unsigned int light = gui_blend(bg, 0x00FFFFFF);
    int gloss_h = h * 2 / 5;
    for (int row = corner_r; row < gloss_h; row++)
        window_rect(x + corner_r, y + row, w - 2 * corner_r, 1, gui_lerp(light, bg, row, gloss_h));
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
static void gui_fill_ellipse(int cx, int cy, int rx, int ry, unsigned int color, unsigned int into){
    if (rx <= 0 || ry <= 0) return;
    int outer2 = (ry + AA_BAND) * (ry + AA_BAND);
    for (int dy = -ry - AA_BAND; dy <= ry + AA_BAND; dy++){
        for (int dx = -rx - AA_BAND; dx <= rx + AA_BAND; dx++){
            int sdx = dx * ry / rx; /* scale x into the same units as y, so it's a circle in transformed space */
            int d2 = sdx * sdx + dy * dy;
            if (d2 > outer2) continue;
            if (d2 <= ry * ry) { window_pixel(cx + dx, cy + dy, color); continue; }
            int t = gui_isqrt(d2) - ry;
            window_pixel(cx + dx, cy + dy, gui_lerp(color, into, t, AA_BAND));
        }
    }
}

/* A soft contact shadow centered right at the icon's own bottom edge:
   the icon (drawn after this) covers the top half of the ellipse, only
   the bottom crescent peeks out, exactly the soft "floating above the
   tray" cue a flat icon can't give on its own. `into` is the dock
   tray's own flat color, icons sit on the tray, not the gradient
   wallpaper behind it. */
static void gui_draw_icon_shadow(int cx_center, int cy_bottom, int size){
    unsigned int dock_bg = 0x00EFEBE4;
    unsigned int shadow = gui_blend(dock_bg, 0x00000000);
    gui_fill_ellipse(cx_center, cy_bottom - 1, size / 2 - 2, size / 10, shadow, dock_bg);
}

static void gui_draw_icon_glyph(int icon, int cx_center, int cy, int size, unsigned int bg){
    switch (icon) {
        case 0: gui_icon_weather(cx_center, cy, size, bg); break;
        case 1: gui_icon_pin(cx_center, cy, size, bg); break;
        case 2: gui_icon_chat(cx_center, cy, size, bg); break;
        case 3: gui_icon_folder(cx_center, cy, size, bg); break;
        case 4: gui_icon_keyrate(cx_center, cy, size, bg); break;
        case 5: gui_icon_book(cx_center, cy, size, bg); break;
        case 6: gui_icon_quotes(cx_center, cy, size, bg); break;
        case 7: gui_icon_notes(cx_center, cy, size, bg); break;
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
#define ICON_SS_SCALE 3
static void gui_draw_one_icon(int icon, int cx_center, int cy_bottom, int size){
    int x = cx_center - size / 2, y = cy_bottom - size;
    unsigned int bg = GUI_COLORS[icon];
    unsigned int bg_light = gui_blend(bg, 0x00FFFFFF), bg_dark = gui_blend(bg, 0x00000000);

    unsigned int ssz = (unsigned int)size * ICON_SS_SCALE;
    unsigned int *ssbuf = (unsigned int *)kmalloc(ssz * ssz * sizeof(unsigned int));
    if (ssbuf) {
        window_push_target(ssbuf, ssz, ssz);
        gui_rounded_rect_gradient(0, 0, (int)ssz, (int)ssz, bg_light, bg_dark, GUI_BG, 12 * ICON_SS_SCALE);
        gui_draw_gloss(0, 0, (int)ssz, (int)ssz, bg, 13 * ICON_SS_SCALE);
        int scy = (int)ssz / 2;
        unsigned int real_fg = ICON_FG;
        ICON_FG = gui_blend(bg, 0x00000000);
        gui_draw_icon_glyph(icon, (int)ssz / 2 + ICON_SS_SCALE, scy + 2 * ICON_SS_SCALE, (int)ssz, bg);
        ICON_FG = real_fg;
        gui_draw_icon_glyph(icon, (int)ssz / 2, scy, (int)ssz, bg);
        window_pop_target();

        unsigned int samples = ICON_SS_SCALE * ICON_SS_SCALE;
        for (int oy = 0; oy < size; oy++){
            for (int ox = 0; ox < size; ox++){
                unsigned int rs = 0, gs = 0, bs = 0;
                for (int sy = 0; sy < ICON_SS_SCALE; sy++){
                    unsigned int *row = &ssbuf[(unsigned int)(oy * ICON_SS_SCALE + sy) * ssz + (unsigned int)ox * ICON_SS_SCALE];
                    for (int sx = 0; sx < ICON_SS_SCALE; sx++){
                        unsigned int c = row[sx];
                        rs += (c >> 16) & 0xFF; gs += (c >> 8) & 0xFF; bs += c & 0xFF;
                    }
                }
                window_pixel(x + ox, y + oy, ((rs / samples) << 16) | ((gs / samples) << 8) | (bs / samples));
            }
        }
        kfree(ssbuf);
        return;
    }

    gui_rounded_rect_gradient(x, y, size, size, bg_light, bg_dark, GUI_BG, 12);
    gui_draw_gloss(x, y, size, size, bg, 13);
    int cy = y + size / 2;
    unsigned int real_fg = ICON_FG;
    ICON_FG = gui_blend(bg, 0x00000000);
    gui_draw_icon_glyph(icon, cx_center + 1, cy + 2, size, bg);
    ICON_FG = real_fg;
    gui_draw_icon_glyph(icon, cx_center, cy, size, bg);
}

/* hover_slot: which slot shows the magnify+label (-1 none). drag_slot: the
   slot currently being dragged, drawn separately so it can float free of
   the row under the cursor instead of at its slot position. */
static void gui_draw_desktop(int hover_slot, int drag_slot, int drag_mx, int drag_my){
    gui_draw_wallpaper();
    gui_draw_menubar();

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
    gui_rounded_rect_on_wallpaper(dock_x, y0, dock_w, dock_h, 0x00EFEBE4, 20);

    for (int slot = 0; slot < GUI_ICON_COUNT; slot++) {
        if (slot == drag_slot) continue; /* drawn last, floating at the cursor */
        int icon = gui_order[slot];
        int magnified = (slot == hover_slot);
        int size = magnified ? DOCK_ICON + DOCK_MAGNIFY : DOCK_ICON;
        int cx_center = gui_slot_x(slot) + DOCK_ICON / 2;
        int cy_bottom = y0 + DOCK_PAD + DOCK_ICON - (magnified ? DOCK_LIFT : 0);
        gui_draw_icon_shadow(cx_center, cy_bottom, size);
        gui_draw_one_icon(icon, cx_center, cy_bottom, size);
        if (magnified) {
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

static void gui_draw_cursor(int x, int y){
    window_rect(x, y, 3, 13, 0x001C1C1E);
    window_rect(x, y, 13, 3, 0x001C1C1E);
    window_rect(x + 1, y + 1, 1, 11, 0x00FFFFFF);
    window_rect(x + 1, y + 1, 11, 1, 0x00FFFFFF);
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
        int sc = kbd_pop();
        /* Real, reported bug: "any key" closed every read-only viewer,
           including Keyrate once it became a real typing test, the first
           keystroke anyone typed closed the app instead of registering.
           Esc (or a click, unchanged) closes now; every other key is
           just consumed and ignored, harmless on a page with nothing
           else to do with a keypress, and no longer surprising on one
           that does. */
        if (sc >= 0 && !(sc & 0x80) && SC[sc & 0x7F] == 27) return;
        if (mouse_click_edge()) return;
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
    gui_fill_circle(26, 20, 6, 0x00FF5F57, 0x00FAF8F6);
    gui_fill_circle(46, 20, 6, 0x00D8D4CE, 0x00FAF8F6);
    gui_fill_circle(66, 20, 6, 0x00D8D4CE, 0x00FAF8F6);
    font_draw_string(title, 84, 12, 0x0085144B, -1);
}

static void gui_launch_html(const char *label, const unsigned char *data, unsigned int data_len){
    window_clear(0x00FAF8F6);
    gui_draw_app_titlebar(label);

    /* app_weather_html/app_curbfind_html are raw byte arrays generated by
       gen_app.sh, not null-terminated C strings; html_to_text expects one,
       so copy with an explicit terminator rather than let it scan past the
       real buffer into whatever memory follows. */
    static char html[20480]; /* comfortably covers every embedded app (quotestreak is the largest at 18947 bytes) */
    unsigned int copy_len = data_len < sizeof(html) - 1 ? data_len : sizeof(html) - 1;
    for (unsigned int i = 0; i < copy_len; i++) html[i] = (char)data[i];
    html[copy_len] = 0;

    static char text[6144];
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
    font_draw_string("type a message, enter to send, esc to cancel:", 20, 44, 0x0075726E, -1);

    static char msg[200];
    unsigned int n = 0;
    for (;;) {
        int k = get_key();
        if (k == KEY_ESC) return;
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
   (irq.c's ticks()) stands in — good enough for word order, not for
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

static void gui_launch(int icon){
    if (icon == 0)      gui_launch_html("Weather", app_weather_html, app_weather_len);
    else if (icon == 1) gui_launch_html("Curbfind", app_curbfind_html, app_curbfind_len);
    else if (icon == 2) gui_launch_chat();
    else if (icon == 3) gui_launch_files();
    else if (icon == 4) gui_launch_keyrate();
    else if (icon == 5) gui_launch_html("Bookrank", app_bookrank_html, app_bookrank_len);
    else if (icon == 6) gui_launch_html("Quotestreak", app_quotestreak_html, app_quotestreak_len);
    else if (icon == 7) gui_launch_editor();
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
    gui_draw_logo(400, 260, 5, bg);
    gui_draw_hello_script(400, 320, 6, 0x00F5EFE8, bg);

    unsigned int start = ticks();
    unsigned int logo_only = 60; /* 0.6s: just the logo and wordmark, no bar yet */
    unsigned int bar_span  = 40; /* 0.4s: bar fills once shown, ~1s total */
    int bar_x = 320, bar_y = 340, bar_w = 160, bar_h = 6;
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
#define GUI_MENU_ITEM_COUNT 6
static const char *GUI_MENU_LABELS[GUI_MENU_ITEM_COUNT] = {
    "About Joshua Tree", "Files", "Notes", "-", "Restart", "Shut Down"
};
#define GUI_MENU_ROW_H  22
#define GUI_MENU_SEP_H  9
#define GUI_MENU_X0     4
#define GUI_MENU_W      180

static int gui_menu_row_h(int i){ return GUI_MENU_LABELS[i][0] == '-' ? GUI_MENU_SEP_H : GUI_MENU_ROW_H; }
static int gui_menu_total_h(void){ int h = 0; for (int i = 0; i < GUI_MENU_ITEM_COUNT; i++) h += gui_menu_row_h(i); return h; }

/* Returns the item index under (mx,my), -2 for a separator row (a real
   hit, but not an actionable one), or -1 if outside the menu entirely. */
static int gui_menu_hit_test(int mx, int my){
    int y = GUI_MENUBAR_H;
    if (mx < GUI_MENU_X0 || mx >= GUI_MENU_X0 + GUI_MENU_W || my < y || my >= y + gui_menu_total_h()) return -1;
    for (int i = 0; i < GUI_MENU_ITEM_COUNT; i++){
        int rh = gui_menu_row_h(i);
        if (my < y + rh) return GUI_MENU_LABELS[i][0] == '-' ? -2 : i;
        y += rh;
    }
    return -1;
}

static void gui_draw_apple_menu(int hover_item){
    int y0 = GUI_MENUBAR_H, total_h = gui_menu_total_h();
    unsigned int bg = 0x002C2C2E, border = 0x001C1C1E, text = 0x00F5F5F7;
    window_rect(GUI_MENU_X0, y0, GUI_MENU_W, total_h, bg);
    window_rect(GUI_MENU_X0, y0, GUI_MENU_W, 1, border);
    window_rect(GUI_MENU_X0, y0 + total_h - 1, GUI_MENU_W, 1, border);
    window_rect(GUI_MENU_X0, y0, 1, total_h, border);
    window_rect(GUI_MENU_X0 + GUI_MENU_W - 1, y0, 1, total_h, border);
    int ry = y0;
    for (int i = 0; i < GUI_MENU_ITEM_COUNT; i++){
        int rh = gui_menu_row_h(i);
        if (GUI_MENU_LABELS[i][0] == '-') { window_rect(GUI_MENU_X0 + 8, ry + rh / 2, GUI_MENU_W - 16, 1, 0x00545458); ry += rh; continue; }
        if (i == hover_item) window_rect(GUI_MENU_X0 + 2, ry, GUI_MENU_W - 4, rh, 0x0085144B);
        font_draw_string(GUI_MENU_LABELS[i], GUI_MENU_X0 + 12, ry + 5, text, -1);
        ry += rh;
    }
}

static void gui_menu_run_item(int item){
    if (item == 0) gui_launch_about();
    else if (item == 1) gui_launch_files();
    else if (item == 2) gui_launch_editor();
    else if (item == 4) reboot();
    else if (item == 5) {
        window_clear(0x00111111);
        font_draw_string("It's now safe to turn off this computer.", 20, (int)window_height() / 2, 0x00F5F5F7, -1);
        __asm__ volatile ("cli");
        for (;;) __asm__ volatile ("hlt");
    }
}

static void gui_run(void){
    if (!window_open(800, 600, 32)) { puts("no VGA device found or out of page tables\n"); return; }
    gui_draw_boot_screen();
    gui_order_init();
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

    int last_mx = mx, last_my = my, last_hover = -1, last_drag = -1, last_menu_open = 0, last_menu_hover = -2;
    gui_menubar_force_redraw(); /* this GUI session's first frame, the minute-change gate must not skip it */
    gui_draw_desktop(-1, -1, 0, 0);
    gui_draw_cursor(mx, my);
    for (;;) {
        __asm__ volatile ("hlt");
        int sc = kbd_pop();
        if (sc >= 0 && !(sc & 0x80) && SC[sc & 0x7F] == 27) break; /* esc, non-blocking */
        int dx = 0, dy = 0;
        int moved_mouse = mouse_get_delta(&dx, &dy, &buttons);
        if (moved_mouse) {
            mx += dx; my += dy;
            if (mx < 0) mx = 0; if ((unsigned)mx >= window_width())  mx = (int)window_width() - 1;
            if (my < 0) my = 0; if ((unsigned)my >= window_height()) my = (int)window_height() - 1;
        }
        int held = buttons & 1;
        int just_pressed = held && !(prev_buttons & 1);
        int just_released = !held && (prev_buttons & 1);
        int logo_here = !menu_open && mx >= 4 && mx <= 28 && my < GUI_MENUBAR_H;
        int slot_here = menu_open ? -1 : gui_dock_hit_test(mx, my); /* the dock is inert while the menu covers it */

        if (just_pressed) {
            if (logo_here) { menu_open = 1; menu_opening = 1; }
            else if (slot_here >= 0) { press_slot = slot_here; press_x = mx; press_y = my; drag_slot = -1; }
        }

        if (held && press_slot >= 0 && drag_slot < 0) {
            int moved = (mx > press_x ? mx - press_x : press_x - mx) + (my > press_y ? my - press_y : press_y - my);
            if (moved > 8) drag_slot = press_slot; /* threshold crossed: this is a drag, not a click */
        }

        int launched = 0;
        if (just_released) {
            if (menu_open) {
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
                gui_launch(gui_order[press_slot]);
                launched = 1; /* the app view just took over the whole screen; force a redraw below even if the cursor never moved */
            }
            press_slot = -1; drag_slot = -1;
        }
        prev_buttons = buttons;

        int hover_slot = (drag_slot < 0) ? slot_here : -1;
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
        if (launched || mx != last_mx || my != last_my || hover_slot != last_hover || drag_slot != last_drag || menu_open != last_menu_open || menu_hover != last_menu_hover) {
            /* Real, user-reported bug, reproduced live: hovering the cursor
               over the menu bar left a jagged trail of ghost cursors there.
               Root cause: the menu bar's own minute-change gate (above)
               means it only repaints when the clock ticks over, but the
               cursor is drawn directly on top of it every frame regardless.
               Everywhere else on screen gui_draw_wallpaper repaints every
               row every frame, which erases the previous cursor draw for
               free; the menu bar's rows are the one band nothing repaints
               on a normal frame, so old cursor pixels never get cleared
               while the mouse is up there. Forcing a real menu bar redraw
               whenever the cursor is entering, moving within, or leaving
               that band (not just on the minute) fixes it at the source
               instead of special-casing the cursor draw itself. */
            if (my < GUI_MENUBAR_H || last_my < GUI_MENUBAR_H) gui_menubar_force_redraw();
            gui_draw_desktop(hover_slot, drag_slot, mx, my);
            if (menu_open) gui_draw_apple_menu(menu_hover);
            if (drag_slot < 0) gui_draw_cursor(mx, my);
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
    if (!strcmp(line, "help"))       puts("help clear echo time uptime dmesg mem reboot crash pagefault heaptest heapgrow tasktest preempttest isotest reaptest ring3test ps kill killtest sleep disktest diskuse fsuse ls cat exec rm cd mkdir write browse lspci gfxtest fonttest mousetest nettest ifconfig netscan web serve serveapp chat build gui testapps\n");
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
        ring3_test();
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
        else { puts(vfs_delete(arg) ? "deleted\n" : "not found\n"); }
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
        if (!*arg) { puts("usage: serveapp weather|curbfind|keyrate|bookrank|quotestreak\n"); }
        else if (!rtl8139_init()) { puts("no RTL8139 found or reset failed\n"); }
        else {
            net_init(0x0A00020F);
            if (!strcmp(arg, "weather"))          serve_app("weather", app_weather_html, app_weather_len);
            else if (!strcmp(arg, "curbfind"))    serve_app("curbfind", app_curbfind_html, app_curbfind_len);
            else if (!strcmp(arg, "keyrate"))     serve_app("keyrate", app_keyrate_html, app_keyrate_len);
            else if (!strcmp(arg, "bookrank"))    serve_app("bookrank", app_bookrank_html, app_bookrank_len);
            else if (!strcmp(arg, "quotestreak")) serve_app("quotestreak", app_quotestreak_html, app_quotestreak_len);
            else puts("unknown app, try weather, curbfind, keyrate, bookrank, or quotestreak\n");
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
            for (int i = 0; i < GUI_ICON_COUNT; i++) gui_launch(i);
            window_close();
            clear();
            puts("testapps done\n");
        }
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
    irq_install();
    klog("irq_install: PIC remapped, PIT/keyboard IRQs live");
    mouse_init();
    klog("mouse_init: PS/2 mouse enabled");
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
    int fs_ok = fat_mount();
    klog(fs_ok ? "fat_mount: FAT16 filesystem mounted" : "fat_mount: no filesystem found");
    fat_vfs_register(); /* registered regardless of fs_ok: an unmounted fat backend just returns real failures, same as before v29 */
    ramfs_init();
    klog("vfs: fat + ramfs backends registered, fat active");
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
