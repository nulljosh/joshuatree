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

/* ---- task demo: two tasks that each print a letter and yield, round-robin,
   to prove context switching actually swaps stacks correctly. Bounded, then
   hlt forever: a task can't safely return (see task.c), and now that irq0
   itself can round-robin into these on every tick regardless of who calls
   yield(), letting them print forever would spam the shell's own output for
   the rest of the boot session the first time anyone ran tasktest. ---- */
static void task_a(void){ for (int i = 0; i < 10; i++) { puts("A"); yield(); } for (;;) __asm__ volatile ("hlt"); }
static void task_b(void){ for (int i = 0; i < 10; i++) { puts("B"); yield(); } for (;;) __asm__ volatile ("hlt"); }

/* ---- preemption demo: two tasks that never call yield() or hlt, proving
   the timer itself forces a switch. The shell's own wait loop below also
   never yields/hlts on purpose, so if preemption weren't real this whole
   command would just spin, and neither counter would ever move. ---- */
static volatile int preempt_a_count = 0;
static volatile int preempt_b_count = 0;
static void preempt_task_a(void){ for (;;) preempt_a_count++; }
static void preempt_task_b(void){ for (;;) preempt_b_count++; }

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
    fat_list(browse_collect_cb);
    int sel = 0;
    browse_draw(sel);
    for (;;) {
        int k = get_key();
        if (k == KEY_ESC || k == 'q') { clear(); return; }
        if (k == KEY_UP)   { if (sel > 0) sel--; browse_draw(sel); }
        if (k == KEY_DOWN) { if (sel < browse_count - 1) sel++; browse_draw(sel); }
        if (k == KEY_ENTER && browse_count > 0 && browse_is_dir[sel]) {
            fat_chdir(browse_names[sel]);
            browse_count = 0;
            fat_list(browse_collect_cb);
            sel = 0;
            browse_draw(sel);
        }
        else if (k == KEY_ENTER && browse_count > 0) {
            clear();
            puts(browse_names[sel]); puts(":\n\n");
            char buf[2048];
            int n = fat_read_file(browse_names[sel], buf, sizeof(buf) - 1);
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
#define GUI_ICON_COUNT 7
static const char *GUI_LABELS[GUI_ICON_COUNT] = {"Weather", "Curbfind", "Chat", "Files", "Keyrate", "Bookrank", "Quotes"};
static const unsigned int GUI_COLORS[GUI_ICON_COUNT] = {
    0x00C1502F, 0x007A2048, 0x00365E8C, 0x00707070, 0x00B08900, 0x002F7B4F, 0x008B4A9C
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

static void gui_rounded_rect(int x, int y, int w, int h, unsigned int color, unsigned int bg, int r){
    window_rect(x, y, w, h, color);
    unsigned int edge = gui_blend(color, bg);
    int inner = r * r, outer = (r + 1) * (r + 1);
    for (int dy = 0; dy <= r; dy++){
        for (int dx = 0; dx <= r; dx++){
            int d2 = dx * dx + dy * dy;
            if (d2 <= inner) continue; /* the corner's own quarter circle, already the right color */
            unsigned int c = (d2 <= outer) ? edge : bg; /* one soft blended ring, then full background */
            window_pixel(x + dx,         y + dy,         c);
            window_pixel(x + w - 1 - dx, y + dy,         c);
            window_pixel(x + dx,         y + h - 1 - dy, c);
            window_pixel(x + w - 1 - dx, y + h - 1 - dy, c);
        }
    }
}

static void gui_draw_menubar(void){
    window_rect(0, 0, (int)window_width(), GUI_MENUBAR_H, 0x00FFFFFF);
    window_rect(0, GUI_MENUBAR_H - 1, (int)window_width(), 1, 0x00DDD9D3);
    font_draw_string("Joshua Tree", 14, 7, 0x001C1C1E, -1);

    u8 h = cmos(4), m = cmos(2);
    char clock[6];
    u8 hv = (h & 0x0F) + ((h >> 4) * 10), mv = (m & 0x0F) + ((m >> 4) * 10);
    clock[0] = '0' + hv / 10; clock[1] = '0' + hv % 10; clock[2] = ':';
    clock[3] = '0' + mv / 10; clock[4] = '0' + mv % 10; clock[5] = 0;
    font_draw_string(clock, (int)window_width() - 60, 7, 0x001C1C1E, -1);
}

/* Real pictograms, not letters: there's no image decoder or asset pipeline
   in this kernel (deliberately, see roadmap.md's font/asset scope notes),
   so each icon is drawn from the same primitives gui_rounded_rect already
   uses (window_pixel/window_rect plus a circle-distance test), geometric
   but genuinely representative of what each app actually is, the same way
   a real dock icon reads as its app at a glance without needing a label. */
#define ICON_FG 0x00FFFFFF

/* `into` is whatever color surrounds this circle, so the one-pixel edge
   band can blend toward it: the icon's own colored background for a solid
   fill, or the fill color itself when punching a hole (the pin's eyelet)
   into a shape that was drawn in that fill color. */
static void gui_fill_circle(int cx, int cy, int r, unsigned int color, unsigned int into){
    unsigned int edge = gui_blend(color, into);
    int inner = r * r, outer = (r + 1) * (r + 1);
    for (int dy = -r - 1; dy <= r + 1; dy++){
        for (int dx = -r - 1; dx <= r + 1; dx++){
            int d2 = dx * dx + dy * dy;
            if (d2 <= inner) window_pixel(cx + dx, cy + dy, color);
            else if (d2 <= outer) window_pixel(cx + dx, cy + dy, edge);
        }
    }
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

static void gui_icon_weather(int cx, int cy, int s, unsigned int bg){
    int r = s / 6, ray = s / 8, gap = r + 2;
    gui_fill_circle(cx, cy, r, ICON_FG, bg);
    window_rect(cx - 1, cy - gap - ray, 2, ray, ICON_FG);
    window_rect(cx - 1, cy + gap,       2, ray, ICON_FG);
    window_rect(cx - gap - ray, cy - 1, ray, 2, ICON_FG);
    window_rect(cx + gap,       cy - 1, ray, 2, ICON_FG);
    for (int t = 0; t < ray; t++){
        int d = ((gap + t) * 7) / 10; /* ~cos(45deg), diagonal ray projection */
        window_pixel(cx - d, cy - d, ICON_FG);
        window_pixel(cx + d, cy - d, ICON_FG);
        window_pixel(cx - d, cy + d, ICON_FG);
        window_pixel(cx + d, cy + d, ICON_FG);
    }
}

static void gui_icon_pin(int cx, int cy, int s, unsigned int bg){
    int r = s / 6, head_cy = cy - s / 10;
    gui_fill_circle(cx, head_cy, r, ICON_FG, bg);
    gui_fill_circle(cx, head_cy, r / 3, bg, ICON_FG); /* punch the pinhole */
    gui_fill_triangle_down(cx, head_cy + r - 1, r, s / 4, ICON_FG);
}

static void gui_icon_chat(int cx, int cy, int s, unsigned int bg){
    int w = (s * 7) / 10, h = (s * 5) / 10;
    int x = cx - w / 2, y = cy - h / 2 - s / 12;
    gui_rounded_rect(x, y, w, h, ICON_FG, bg, 5);
    gui_fill_triangle_down(x + w / 5, y + h - 1, s / 10, s / 8, ICON_FG);
}

static void gui_icon_folder(int cx, int cy, int s){
    int w = (s * 7) / 10, h = (s * 5) / 10;
    int x = cx - w / 2, y = cy - h / 2 + s / 12;
    window_rect(x, y, w / 3, s / 12, ICON_FG);
    window_rect(x, y + s / 12, w, h, ICON_FG);
}

static void gui_icon_keyrate(int cx, int cy, int s){
    int key = s / 6, gap = s / 14;
    int total_w = 3 * key + 2 * gap, total_h = 2 * key + gap;
    int x0 = cx - total_w / 2, y0 = cy - total_h / 2;
    for (int row = 0; row < 2; row++)
        for (int col = 0; col < 3; col++)
            window_rect(x0 + col * (key + gap), y0 + row * (key + gap), key, key, ICON_FG);
}

static void gui_icon_book(int cx, int cy, int s, unsigned int bg){
    int w = (s * 7) / 10, h = (s * 5) / 10;
    int x = cx - w / 2, y = cy - h / 2;
    window_rect(x, y, w, h, ICON_FG);
    window_rect(cx - 1, y, 2, h, bg); /* spine split between the two pages */
}

static void gui_icon_quotes(int cx, int cy, int s, unsigned int bg){
    int r = s / 9, off = s / 6, base_cy = cy - s / 10;
    gui_fill_circle(cx - off, base_cy, r, ICON_FG, bg);
    gui_fill_triangle_down(cx - off, base_cy + r - 1, r, s / 6, ICON_FG);
    gui_fill_circle(cx + off, base_cy, r, ICON_FG, bg);
    gui_fill_triangle_down(cx + off, base_cy + r - 1, r, s / 6, ICON_FG);
}

static void gui_draw_one_icon(int icon, int cx_center, int cy_bottom, int size){
    int x = cx_center - size / 2, y = cy_bottom - size;
    unsigned int bg = GUI_COLORS[icon];
    gui_rounded_rect(x, y, size, size, bg, GUI_BG, 12);
    int cy = y + size / 2;
    switch (icon) {
        case 0: gui_icon_weather(cx_center, cy, size, bg); break;
        case 1: gui_icon_pin(cx_center, cy, size, bg); break;
        case 2: gui_icon_chat(cx_center, cy, size, bg); break;
        case 3: gui_icon_folder(cx_center, cy, size); break;
        case 4: gui_icon_keyrate(cx_center, cy, size); break;
        case 5: gui_icon_book(cx_center, cy, size, bg); break;
        case 6: gui_icon_quotes(cx_center, cy, size, bg); break;
    }
}

/* hover_slot: which slot shows the magnify+label (-1 none). drag_slot: the
   slot currently being dragged, drawn separately so it can float free of
   the row under the cursor instead of at its slot position. */
static void gui_draw_desktop(int hover_slot, int drag_slot, int drag_mx, int drag_my){
    window_clear(GUI_BG);
    gui_draw_menubar();
    font_draw_string("drag to rearrange -- click to open -- esc to quit", gui_dock_x0(), gui_dock_y0() - 44, 0x0075726E, -1);

    int y0 = gui_dock_y0();
    gui_rounded_rect(gui_dock_x0(), y0, gui_dock_w(), DOCK_ICON + 2 * DOCK_PAD, 0x00EFEBE4, GUI_BG, 20);

    for (int slot = 0; slot < GUI_ICON_COUNT; slot++) {
        if (slot == drag_slot) continue; /* drawn last, floating at the cursor */
        int icon = gui_order[slot];
        int magnified = (slot == hover_slot);
        int size = magnified ? DOCK_ICON + DOCK_MAGNIFY : DOCK_ICON;
        int cx_center = gui_slot_x(slot) + DOCK_ICON / 2;
        int cy_bottom = y0 + DOCK_PAD + DOCK_ICON - (magnified ? DOCK_LIFT : 0);
        gui_draw_one_icon(icon, cx_center, cy_bottom, size);
        if (magnified) {
            int label_w = (int)strlen(GUI_LABELS[icon]) * 8;
            font_draw_string(GUI_LABELS[icon], cx_center - label_w / 2, cy_bottom - size - 18, 0x001C1C1E, -1);
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

static void gui_wait_close(void){
    font_draw_string("any key to go back", 20, (int)window_height() - 30, 0x0075726E, -1);
    get_key();
}

static void gui_launch_html(const char *label, const unsigned char *data, unsigned int data_len){
    window_clear(0x00FAF8F6);
    font_draw_string(label, 20, 16, 0x00C1502F, -1);

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
    font_draw_string("Files", 20, 16, 0x00C1502F, -1);
    gui_fat_count = 0;
    fat_list(gui_fat_collect);
    if (gui_fat_count == 0) font_draw_string("(no files, or no FAT filesystem)", 20, 50, 0x001C1C1E, -1);
    for (int i = 0; i < gui_fat_count; i++) font_draw_string(gui_fat_names[i], 20, 50 + i * 18, 0x001C1C1E, -1);
    gui_wait_close();
}

static void gui_launch_chat(void){
    window_clear(0x00FAF8F6);
    font_draw_string("Chat", 20, 16, 0x00C1502F, -1);
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
    font_draw_string("Chat", 20, 16, 0x00C1502F, -1);
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

static void gui_launch(int icon){
    if (icon == 0)      gui_launch_html("Weather", app_weather_html, app_weather_len);
    else if (icon == 1) gui_launch_html("Curbfind", app_curbfind_html, app_curbfind_len);
    else if (icon == 2) gui_launch_chat();
    else if (icon == 3) gui_launch_files();
    else if (icon == 4) gui_launch_html("Keyrate", app_keyrate_html, app_keyrate_len);
    else if (icon == 5) gui_launch_html("Bookrank", app_bookrank_html, app_bookrank_len);
    else if (icon == 6) gui_launch_html("Quotestreak", app_quotestreak_html, app_quotestreak_len);
}

static void gui_run(void){
    if (!window_open(800, 600, 32)) { puts("no VGA device found or out of page tables\n"); return; }
    gui_order_init();
    int mx = 400, my = 300, buttons = 0, prev_buttons = 0;
    /* press_slot: the slot the mouse went down on, latched until release.
       drag_slot: only set once the mouse has actually moved past a small
       threshold while held, so a plain click (down, no movement, up)
       never gets mistaken for a drag onto its own slot. */
    int press_slot = -1, press_x = 0, press_y = 0, drag_slot = -1;

    int last_mx = mx, last_my = my, last_hover = -1, last_drag = -1;
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
        int slot_here = gui_dock_hit_test(mx, my);

        if (just_pressed && slot_here >= 0) { press_slot = slot_here; press_x = mx; press_y = my; drag_slot = -1; }

        if (held && press_slot >= 0 && drag_slot < 0) {
            int moved = (mx > press_x ? mx - press_x : press_x - mx) + (my > press_y ? my - press_y : press_y - my);
            if (moved > 8) drag_slot = press_slot; /* threshold crossed: this is a drag, not a click */
        }

        int launched = 0;
        if (just_released) {
            if (drag_slot >= 0) {
                int target = gui_slot_at(mx);
                int tmp = gui_order[drag_slot];
                gui_order[drag_slot] = gui_order[target];
                gui_order[target] = tmp;
            } else if (press_slot >= 0 && press_slot == slot_here) {
                gui_launch(gui_order[press_slot]);
                launched = 1; /* the app view just took over the whole screen; force a redraw below even if the cursor never moved */
            }
            press_slot = -1; drag_slot = -1;
        }
        prev_buttons = buttons;

        int hover_slot = (drag_slot < 0) ? slot_here : -1;
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
        if (launched || mx != last_mx || my != last_my || hover_slot != last_hover || drag_slot != last_drag) {
            gui_draw_desktop(hover_slot, drag_slot, mx, my);
            if (drag_slot < 0) gui_draw_cursor(mx, my);
            last_mx = mx; last_my = my; last_hover = hover_slot; last_drag = drag_slot;
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
    if (!strcmp(line, "help"))       puts("help clear echo time uptime mem reboot crash pagefault heaptest tasktest preempttest ring3test sleep disktest ls cat exec rm cd mkdir write browse lspci gfxtest fonttest mousetest nettest web serve serveapp chat build gui\n");
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
    else if (!strcmp(line, "uptime")){ putn(ticks() / 100); puts("s\n"); }
    else if (!strcmp(line, "mem")) {
        putn(pmm_free_frames() * 4); puts("K free / ");
        putn(pmm_total_frames() * 4); puts("K total (4K frames)\n");
    }
    else if (!strcmp(line, "sleep")) { puts("sleeping 1s...\n"); sleep_ticks(100); puts("awake\n"); }
    else if (!strcmp(line, "disktest")) {
        char wbuf[512], rbuf[512];
        for (int i = 0; i < 512; i++) wbuf[i] = (char)i;
        if (!ata_write_sector(100, wbuf)) { puts("disk write failed (no drive?)\n"); }
        else if (!ata_read_sector(100, rbuf)) { puts("disk read failed\n"); }
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
        preempt_a_count = 0; preempt_b_count = 0;
        int ida = task_create(preempt_task_a);
        int idb = task_create(preempt_task_b);
        if (ida < 0 || idb < 0) { puts("no free task slots (run fewer other task tests first)\n"); }
        else {
            unsigned int deadline = ticks() + 20; /* ~200ms real wall clock */
            while (ticks() < deadline) { } /* deliberately no yield()/hlt here */
            puts((preempt_a_count > 0 && preempt_b_count > 0) ? "preempted without yield: ok\n" : "no preemption (still cooperative-only)\n");
        }
    }
    else if (!strcmp(line, "ls"))    fat_list(ls_cb);
    else if (!strcmp(line, "browse")) browse();
    else if (!strcmp(line, "cat")) {
        if (!*arg) { puts("usage: cat <file>\n"); }
        else {
            char buf[4096];
            int n = fat_read_file(arg, buf, sizeof(buf) - 1);
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
        else { puts(fat_delete(arg) ? "deleted\n" : "not found\n"); }
    }
    else if (!strcmp(line, "cd")) {
        if (!*arg) { puts("usage: cd <dir> (or ..)\n"); }
        else { puts(fat_chdir(arg) ? "ok\n" : "not found or not a directory\n"); }
    }
    else if (!strcmp(line, "mkdir")) {
        if (!*arg) { puts("usage: mkdir <name>\n"); }
        else { puts(fat_mkdir(arg) ? "created\n" : "failed (name taken, disk full, or directory full)\n"); }
    }
    else if (!strcmp(line, "write")) {
        if (!*arg) { puts("usage: write <file> <content>\n"); }
        else {
            char *content = arg;
            while (*content && *content != ' ') content++;
            if (*content) *content++ = 0;
            puts(fat_write_file(arg, content, strlen(content)) ? "written\n" : "failed (name taken or disk full)\n");
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
    else if (!strcmp(line, "gfxtest")) {
        if (!window_open(800, 600, 32)) { puts("no VGA device found or out of page tables\n"); }
        else {
            window_rect(0, 0, 800, 200, 0x00C1502F);
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
            font_draw_string("Joshua Tree", 20, 20, 0x00C1502F, -1);
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
    else if (!strcmp(line, "mousetest")) {
        if (!window_open(800, 600, 32)) { puts("no VGA device found or out of page tables\n"); }
        else {
            int cx_pos = 400, cy_pos = 300;
            int buttons = 0;
            do {
                window_clear(0x00FAF8F6);
                window_rect(cx_pos - 5, cy_pos - 5, 10, 10, 0x00C1502F);
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
    vga_text_mode_init(); /* real hardware/QEMU already boot into text mode via their own BIOS; a BIOS-less multiboot path (v86) never sets it at all, so make it explicit rather than inherited */
    gdt_install();
    idt_install();
    irq_install();
    mouse_init();
    font_init(); /* must run while still in plain VGA text mode, before any window_open */
    pmm_init(multiboot_info_addr);
    paging_install();
    tasks_init();
    int fs_ok = fat_mount();
    clear();
    boot_chime();
    puts("joshuatree v0 -- type help\n");
    if (!fs_ok) puts("(no FAT filesystem found -- ls/cat unavailable)\n");
    kbd_drain(); /* discard any stray byte queued during boot (keyboard_enable_scanning, mouse_init) before real input starts */
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
