/* Freestanding i386 kernel: VGA text, PS/2 keyboard, RTC clock, tiny shell. */
#include "gdt.h"
#include "idt.h"
#include "symtab.h"
#include "irq.h"
#include "pic.h"
#include "pmm.h"
#include "paging.h"
#include "kheap.h"
#include "picker.h"
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
#include "user_wc.h"
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
#include "wallmap.h"   /* 4.18.3: the map and satellite wallpapers live in kernel/wallmap.c (and wall_sat.h with them) */
#include "gui_shapes.h"  /* 4.20.5: gui_aa_line_phys and gui_rounded_rect_gradient */
#include "selftest.h"  /* 4.18.5: pngtest, jpegtest, heaptest, nettest and mailtest live in kernel/selftest.c */
#include "desktop.h"   /* 4.19.1: the menu bar geometry and day/night grade the split-out units share */
#include "multiwin.h"  /* 4.19.1: the window manager */
#include "walltint.h"  /* 4.19.1: the per-pixel wallpaper grades */
#include "settings.h"  /* 4.18.4: SETTINGS.TXT load and save, and the LLM, Mail and Claude relay settings, live in kernel/settings.c */
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
   during satellite fetch.

   4.11.4: the default is now a plain grey (WALL_GREY), wall_src points at
   wall_solid_row for the four plain walls, and nothing is fetched. */
const unsigned char *const wall_photo = wallpaper_rgb;   /* wallmap.c reaches the photo through this; only this file includes drivers/wallpaper.h */
const unsigned char *wall_src = wallpaper_rgb;
/* v0.83.x: a REAL, once-captured satellite photograph (kernel/wall_sat.h,
   tools/gen/gen_wall_sat.py -- the same real mt0.google.com tiles
   wall_fetch() itself pulls for WALL_SAT, baked in at build time), decoded
   lazily on first need and kept for the kernel's lifetime. Direct owner
   request: with no live map (yet, or ever, see wall_apply's own comment)
   the desktop shows a satellite look, not the baked tree photo -- the
   tree stays reachable from Settings (WALL_PHOTO, an explicit user pick),
   it is only the no-network AUTOMATIC fallback that changes. */
unsigned char *wall_sat_rgb = 0;
/* 4.11.4: the plain wallpapers (Grey, Terracotta, Clay, Ink, the same four
   the Pi offers) are two flat rows of the chosen colour, not a 1.5 MB image.
   wall_src points here when a plain wall is up; every reader that walks
   wall_src row by row (gui_wallpaper_row, gui_wallpaper_color) pins its row
   to 0, so row 0 and the row after it are all they ever touch. */
unsigned char wall_solid_row[WALLPAPER_W * 3 * 2];
unsigned char *wall_map = 0;        /* the fetched mosaic, kmalloc'd, kept while the session lives so Photo->Map needs no refetch */
int wall_map_tx = 0, wall_map_ty = 0, wall_map_cx = 0, wall_map_cy = 0; /* tile x/y of the mosaic's top-left tile, crop offset inside it */
int wall_map_is_sat = 0; /* v0.73: which real source wall_map's pixels actually came from (OpenTopoMap PNG vs Google satellite JPEG). Warm/Cool/Raw all share ONE fetch, since they're just different grades of the same topo pixels -- Satellite is a genuinely different image, not a grade, so switching across this boundary must drop wall_map and refetch instead of reusing stale pixels from the other source. */
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
#include "serial.h"
#include "sb16.h"
#include "speak.h"
#include "app.h"
#include "png.h"
#include "jpeg.h"
#include "version.h"
#include "ring3app.h"
#include "weather.h"   /* 4.12.3: the menu-bar weather lives in kernel/weather.c */
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

void clear(void){   /* console.h */
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

int get_key(void){   /* console.h */
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
void cmos_read_time_stable(u8 *h, u8 *m, u8 *wd, u8 *dom, u8 *mon){
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

void putn(unsigned int v){   /* console.h */
    char buf[12]; int n = 0;
    if (v == 0) buf[n++] = '0';
    while (v) { buf[n++] = '0' + v % 10; v /= 10; }
    while (n) putc(buf[--n]);
}

#include "klog.h"   /* 4.20.6: the dmesg ring buffer and the notification panel live in kernel/klog.c */

/* ---- shell ---- */

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
void render_wrapped_text(const char *text, int x0, int y0, int max_w_px, int max_h_px, unsigned int fg) {
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

int web_starts_with(const char *p, const char *needle) {
    while (*needle) { if (*p != *needle) return 0; p++; needle++; }
    return 1;
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
   live outside this kernel (jaybulb.com), same launch shape as
   Search/Contacts/Calculator/Stocks. Inserted before GUI_APPS_FOLDER, so
   it grew GUI_APP_COUNT from 25 to 26 and pushed GUI_APPS_FOLDER/GUI_TRASH
   up by one each, same shift the v0.86.0 comment above describes for
   Search. tools/gen/gen_icon_art.py's ART/VARIANT index maps moved with
   it (24: apps, 25: trash); Portfolio itself has no authored art yet, so
   it keeps the primitive glyph path like every other unart'd icon. */
/* GUI_APP_COUNT: kernel/desktop.h */
/* GUI_APP_PANES: kernel/desktop.h */
/* GUI_APP_PORTFOLIO: kernel/desktop.h */
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
/* the GUI_ICON_ART_SIZE == ICON_ART_SIZE assert moved with icon_art.h to kernel/icons.c in 4.20.7 */
static const int GUI_DOCK_DEFAULT[GUI_ICON_COUNT] = GUI_DOCK_DEFAULT_ORDER; /* gui_paint.h: the ARM dock draws the same order */

/* gui_order is a permutation of icon indices by dock slot: dragging an icon
   and dropping it on another slot swaps the two, so the arrangement is
   real and sticks for the rest of this GUI session (reset to launch order
   next time `gui` runs; nothing about layout is saved to disk, matching
   this whole desktop's one-screen, nothing-persisted scope). */
int gui_order[GUI_ICON_COUNT];
/* Portfolio mode ("portfolio" on the multiboot command line, sent by the
   landing's embed.js when jaybulb.com/os.html frames it): the dock is
   Joshua's own apps instead of the system set. Same slot count, Apps folder
   and Trash stay at the ends; everything left out is still in the Apps folder. */
int portfolio_dock;   /* kernel/phone_home.c reads it too */
int gui_apps_n(void){ return portfolio_dock ? GUI_APPS_FOLDER : GUI_APPS_FOLDER - 1; } /* apps the folder and phone home list */
int gui_app_at(int k){ return (!portfolio_dock && k >= GUI_APP_PORTFOLIO) ? k + 1 : k; } /* grid position to APPS[] index */
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
#include "phone_home.h" /* both defined in kernel/phone_home.h, included near gui_run; forward-declared so gui_draw_app_titlebar/gui_app_mouse_tick (both defined above it) can call them */
static const int GUI_DOCK_PORTFOLIO[GUI_ICON_COUNT] = {GUI_APPS_FOLDER, 21, 20, 8, 10, 12, 14, 11, 9, 13, GUI_TRASH}; /* Portfolio, Epiphany, Curbfind, Bookrank, Tonchi, Hikko, Quotes, Keyrate, Toroid */
static void gui_order_init(void){ for (int i = 0; i < GUI_ICON_COUNT; i++) gui_order[i] = portfolio_dock ? GUI_DOCK_PORTFOLIO[i] : GUI_DOCK_DEFAULT[i]; }
int dock_hover = -1; /* slot whose label is showing */

/* GUI_BG lives in app.h now: a moved-out app's own unit clears to it too. */
/* GUI_MENUBAR_H: kernel/desktop.h */
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
         ROADMAP.md's satellite entry): the fetched map put through
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
   contract v75 established). settings_load clamps to 0..8 (4.11.4: 5..8 are
   the plain walls) so a hand-
   edited or stale SETTINGS.TXT can't select a theme that doesn't exist. */
const unsigned int wall_solid_color[4] = { 0x006E6E6Eu, 0x00B5502Cu, 0x00923F20u, 0x00141311u };
const char *const wall_solid_name[4] = { "Grey", "Terracotta", "Clay", "Ink" };
/* Settings and `wallpaper` step through the themes in this order: the plain
   walls first, then the photo and the map themes. */
const unsigned char wall_cycle_order[WALL_COUNT] = { WALL_GREY, WALL_TERRA, WALL_CLAY, WALL_INK, WALL_PHOTO, WALL_WARM, WALL_COOL, WALL_RAW, WALL_SAT };
/* True for the four themes that need tiles from the network. */
/* v0.76.7: default flipped from WALL_WARM to WALL_SAT per Joshua's own
   direct, previously-recorded request ("the real satellite-town wallpaper
   should become the default once buildable", ROADMAP.md's "Later idea:
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
/* 4.11.4: default flipped again, WALL_SAT -> WALL_GREY. Direct report: "still
   showing the stale wallpaper, the satellite; it should just be a plain
   wallpaper". The satellite and map themes stay selectable in Settings, but
   a fresh boot (the ISO, QEMU, and the browser demo's every idle-tour lap)
   is a flat grey and fetches no tile. */
int wall_theme = WALL_GREY;
int wind_enabled = 1; /* settings.c reads and writes it */

/* SETTINGS.TXT and the LLM, Mail and Claude relay settings it carries: kernel/settings.c (settings.h). */

/* gui_dock_icon/gui_dock_w/gui_dock_x0/gui_dock_y0/gui_slot_x/gui_slot_at/
   gui_dock_hit_test: dock geometry and hit-testing, moved to
   dock_geom.c/.h. DOCK_ICON/DOCK_MARGIN_BOT/DOCK_TRAY_COLOR now live in
   dock_geom.h too. */

/* Paints a rect, then overwrites each corner's pixels outside a quarter
   circle of radius r with bg, faking a rounded rect with no alpha. */
/* gui_blend, gui_line_sqrt: moved to gui_prims.c/.h (pure color/math
   primitives, no state of their own). */


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
void daynight_calc(int hour, int *night_out, int *day_out){
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
unsigned int gui_daynight_tint_pct(unsigned int rgb, int night, int day){
    if (night > 0) return gui_lerp(rgb, 0x00201009, night, 100); /* toward the wallpaper's own espresso-brown floor, never blue */
    if (day > 0)   return gui_lerp(rgb, 0x00DDDDDD, day, 100);   /* toward this file's own documented Silver, a small real brighten */
    return rgb;
}
unsigned int gui_daynight_tint(unsigned int rgb){
    if (wall_src == wall_solid_row) return rgb; /* 4.11.4: a plain wall is exactly the colour picked, day and night */
    return gui_daynight_tint_pct(rgb, daynight_night_pct, daynight_day_pct);
}

/* gui_map_tint, gui_map_tint_cool, gui_sat_color, gui_wall_tint: the per-pixel wallpaper grades, in kernel/walltint.h */

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
    if (wall_src == wall_solid_row) sy = 0; /* 4.11.4: the plain wall is two rows long */
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
int wind_weather_pct = 100;

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
void movies_ring3_open(void); void hamurabi_ring3_open(void); void windgate_ring3_open(void); void panes_ring3_open(void); void claude_ring3_open(void); void mines_ring3_open(void); void books_ring3_open(void);

int gui_ring3_windowed(int icon);
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
    if (wall_src == wall_solid_row) { sy = 0; c.wy = 0; } /* 4.11.4: the plain wall is two rows long, both the same */
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
void gui_draw_wallpaper_rect(int x, int y, int w, int h){
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
void gui_fill_triangle_down(int cx, int y0, int half_w, int h, unsigned int color){
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
void gui_menubar_force_redraw(void){ gui_menubar_last_min = -1; } int gui_bleed_open(void); /* a full-bleed window covers the menu bar */

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
volatile unsigned jt_data_stamp = 0; /* bumped whenever WEATHER.TXT or STOCKS.TXT is rewritten; windows poll it through SYS_SYSINFO */
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
/* Hit box for the menu-bar weather text, recomputed by gui_draw_menubar
   every time it actually redraws that text (same cadence the clock hit
   test already tolerates: coarse, minute-granularity, matching how often
   the underlying layout can shift). -1/-1 means "nothing drawn, not
   clickable" rather than a stale box from a previous boot. */
int weather_hit_x0 = -1, weather_hit_x1 = -1;
void gui_draw_mark_sized(int cx, int cy, int size, unsigned int ink);   /* kernel/bootscreen.c */
void gui_draw_menubar(void){
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

#include "icons.h"   /* 4.20.7: the app icon painters and their cache live in kernel/icons.c */
#include "clockicon.h"

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
void wall_caches_drop(void){
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
   ROADMAP.md's later product ideas, not a shortcut bolted on here. */
void gui_draw_app_titlebar(const char *title){
    if (gui_app_windowed) return;
    if (boot_to_phone) { phone_app_titlebar_draw(title); return; } /* kernel/phone_home.h: back chevron, no Esc key on a phone */
    gui_fill_circle(26, 20, 6, 0x00FF5F57, 0x00FAF8F6);
    gui_fill_circle(46, 20, 6, 0x00FFD64A, 0x00FAF8F6);
    gui_fill_circle(66, 20, 6, 0x00D8D4CE, 0x00FAF8F6);
    gui_close_glyph(26, 20, 0x00FF5F57);
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
#include "../lib/text_ink.h"

#include "ttf_render.h"
#include "auth.h"
#include "editor.h"

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
#define GUI_AA_GLYPH(c) (&editor_glyphs[((0 * 2 + 0) * 4 + 2) * 95 + ((c) - 32)]) /* DejaVu Sans, regular, 24px */
/* v77: the real per-glyph advance in physical px, the metric the fixed
   16px cell ignored (see font_draw_string's own note). Degree ring and
   anything outside 32..126 keep the old cell so nothing else moves. */
int gui_aa_advance(unsigned char c){
    if (c < 32 || c > 126) return 16;
    return GUI_AA_GLYPH(c)->advance;
}
void gui_aa_char(unsigned char c, int px, int py, unsigned int fg, int bg, int cell){
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

void gui_launch(int icon); /* mutually recursive with the folder: the folder launches apps, and the dock launches the folder */
#include "apps_geom.h"
#include "appsfolder.h"   /* 4.20.8: the Apps folder lives in kernel/appsfolder.c */

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
void gui_launch(int icon){
    if (icon >= 0 && icon < GUI_APP_COUNT && APPS[icon].open) APPS[icon].open();
}

int gui_multiwin_open(int icon);
/* 2.0 gate 5: a full window table or a failed window launch is an honest refusal now, never a
   blocking takeover of the screen. Serial marker plus a notice the next repaint draws. */
static const char *gui_notice_name = 0; static unsigned int gui_notice_until = 0;
void gui_refuse_open(int icon){
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

/* The window manager (open, close, focus, snap, the switcher, screenshots): kernel/multiwin.c, multiwin.h */
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
    /* 12 */ {"Patois",     0x00376E5E, gui_icon_tonchi,     tonchi_ring3_open,     0, 0}, /* 1.9.1: ring 3 too (user/tonchi.c) */
    /* 13 */ {"Toroid",     0x00234A78, gui_icon_toroid,     toroid_ring3_open,     0, 0}, /* 1.7.11: ring 3 too (user/toroid.c) */
    /* 14 */ {"Thinkpond",0x00A6741E, gui_icon_hikko,   hikko_ring3_open,   0, 0}, /* 1.9.8: ring 3 (user/hikko.c) */
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
    /* 27 */ {"Airwell",    0x000B1420, gui_icon_chat,       windgate_ring3_open,   0, 0}, /* 2.8: guided breathing, ring 3 (user/windgate.c), Apps folder only; authored art (art/icons/windgate.svg) covers the icon */
    /* 28 */ {"Panes",      0x00F5F5F8, gui_icon_chat,       panes_ring3_open,      0, 0}, /* 2.11: cmux-style tabs and split panes sharing the Terminal's shell engine, ring 3 (user/panes.c), Apps folder only; authored art (art/icons/panes.svg) covers the icon */
    /* 29 */ {"Claude",     0x00B5502C, gui_icon_chat,       claude_ring3_open,     0, 0}, /* 2.14: Claude Code through the relay (user/claude.c, tools/claude-relay/relay.py), ring 3, Apps folder only; authored art (art/icons/claude.svg) covers the icon */
    /* 30 */ {"Mines",      0x00556B85, gui_icon_apps,       mines_ring3_open,      0, 0}, /* Minesweeper, ring 3 (user/mines.c), Apps folder only; authored art is art/icons/mines.svg, a cream mine on the accent */
    /* 31 */ {"Books",      0x008E3A1E, gui_icon_book,       books_ring3_open,      0, 0}, /* 2.66: plain-text reader, ring 3 (user/books.c), Apps folder only like Mines; authored art is art/icons/books.svg */
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
#include "bootscreen.h"   /* 4.20.9: the brand mark, the boot splash and About live in kernel/bootscreen.c */

#include "menu.h"   /* 4.20.10: the Apple menu's drawing and hit test, and the weather dropdown, live in kernel/menu.c */

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
/* phone_home.h (included above): phone mode's real home screen, kernel/phone_home.c since 4.20.18 */
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
    int r3drag_on = 0, r3drag_x = 0, r3drag_y = 0;   /* a press held and moved inside a ring-3 window's content: JT_EV_DRAG steps */
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
    (void)mouse_take_wheel(); /* a wheel step banked while booting (the PS/2 wake-up packet) is not input: a window under the pointer must not open already scrolled. Not mouse_get_wheel: that pumps the vmmouse queue and can eat the first pointer packet on a slow host */
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
            if (WALL_THEME_FETCHES() && !wall_map && geo_have && wall_fetch()) { wall_apply(1); gui_draw_desktop(-1, -1, 0, 0); gui_cursor_save(mx, my); gui_draw_cursor(mx, my); }
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
        {   /* The wheel goes to the ring-3 window under the pointer (JT_EV_WHEEL, +1 up, -1 down), the way a click does. Only the blocking launcher ever read it before, so no window app saw a wheel step. */
            int wz = mouse_take_wheel();   /* not mouse_get_wheel: that pumps the vmmouse queue again and swallows a click whose press and release land in one frame (v86) */
            if (wz && win_hit_here >= 0 && gui_windows[win_hit_here].task >= 0)
                syscall_window_push_event(gui_windows[win_hit_here].task, JT_EV_WHEEL, wz > 0 ? 1 : -1, 0);
        }

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
                press_x = mx; press_y = my;   /* a content press remembers where, for the drag steps below */
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

        if (held && press_window >= 0 && !win_drag_armed && gui_windows[press_window].task >= 0 && press_y >= gui_windows[press_window].y + 30
            && (r3drag_on || mx != press_x || my != press_y)) {
            /* The pointer moving with the button held inside a ring-3 window's content: JT_EV_DRAG with the viewport position, the press's
               own first so the program knows what it grabbed, then one per new pixel, so a seek or volume knob follows the hand. A plain
               click sends nothing new, and the release still sends its click. */
            const gui_window_t *pw = &gui_windows[press_window];
            if (!r3drag_on) { r3drag_on = 1; r3drag_x = press_x; r3drag_y = press_y; syscall_window_push_drag(pw->task, press_x - (pw->x + 8), press_y - (pw->y + 32)); }
            if (mx != r3drag_x || my != r3drag_y) { r3drag_x = mx; r3drag_y = my; syscall_window_push_drag(pw->task, mx - (pw->x + 8), my - (pw->y + 32)); }
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
                   test rather than kept as false evidence; see ROADMAP.md
                   for the honest state of the underlying report, this fix
                   narrows a real gap but is not confirmed to be the full
                   explanation for a hang lasting more than one frame. */
                slot_here = (menu_open || notif_open || weather_open) ? -1 : gui_dock_hit_test(mx, my);
                launched = 1; /* the app view just took over the whole screen; force a redraw below even if the cursor never moved */
            }
            press_slot = -1; drag_slot = -1; press_window = -1; r3drag_on = 0;
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

void putdec(int v){   /* 4.20.1: kernel/progtest.c prints with it too */
    char tmp[12]; int i = 0;
    unsigned u = v < 0 ? (unsigned)(-v) : (unsigned)v;
    if (v < 0) putc('-');
    do { tmp[i++] = (char)('0' + u % 10); u /= 10; } while (u);
    while (i) putc(tmp[--i]);
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
    if (selftest_run(line, arg)) return;   /* kernel/selftest.c: the built-in self-test commands */
    if (taskdemo_run(line)) return;        /* kernel/taskdemo.c: the scheduler demos */
    if (shellfs_run(line, arg)) return;    /* kernel/shellfs.c: ls, cat, rm, cd, mkdir, write, trash, browse */
    if (shellnet_run(line, arg)) return;
    if (wxtest_run(line, arg)) return;
    if (!strcmp(line, "help"))       { puts("help clear echo time uptime dmesg mem reboot crash pagefault bench heaptest heapgrow tasktest fputest preempttest weathertest daynighttest maptinttest walltest weatherfxtest weatherfxcliptest geotest weatherpaneltest windweathertest cursortest texttest wraptest mailtest dockstyletest wind isotest reaptest ring3test usertest notetest filetest ps kill killtest sleep disktest diskuse fsuse ls cat exec rm cd mkdir write browse lspci gfxtest fonttest mousetest nettest ifconfig netscan web serve serveapp chat build gui testapps calctest pngtest jpegtest chattest beep say listen pick pickcheck\n");
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
    else if (!strcmp(line, "pick")) {
        if (!*arg) puts("usage: pick <sentence>\n");
        else { puts("pick: "); puts(picker_pick(arg)); puts("\n"); }
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
    else if (guitest_run(line, arg)) {}   /* 4.20.2: cursortest, texttest, wraptest and dockstyletest live in kernel/guitest.c */
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
        /* v75/v81: grey|terracotta|clay|ink (4.11.4, the plain walls)|photo|warm|cool|raw picks the theme (persisted like
           wind/dockscale); "map" stays a working alias for "warm" (v75's
           original two-state name, now one of four themes) so nothing
           that already typed `wallpaper map` breaks. fetch forces the map
           download right now (its own NIC/net bring-up, same as
           weather_fetch), no argument reports state. */
        /* 4.11.4: the four plain walls (the Pi's names and colours). */
        if (!strcmp(arg, "grey") || !strcmp(arg, "gray")) { wall_switch_theme(WALL_GREY); wall_apply(0); puts("wallpaper: grey\n"); }
        else if (!strcmp(arg, "terracotta")) { wall_switch_theme(WALL_TERRA); wall_apply(0); puts("wallpaper: terracotta\n"); }
        else if (!strcmp(arg, "clay")) { wall_switch_theme(WALL_CLAY); wall_apply(0); puts("wallpaper: clay\n"); }
        else if (!strcmp(arg, "ink")) { wall_switch_theme(WALL_INK); wall_apply(0); puts("wallpaper: ink\n"); }
        else if (!strcmp(arg, "photo")) { wall_switch_theme(WALL_PHOTO); wall_apply(0); puts("wallpaper: photo\n"); }
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
            const char *tn = wall_theme >= WALL_GREY ? wall_solid_name[wall_theme - WALL_GREY] : wall_theme == WALL_PHOTO ? "photo" : wall_theme == WALL_COOL ? "map (cool)" : wall_theme == WALL_RAW ? "map (raw)" : wall_theme == WALL_SAT ? "satellite" : "map (warm)";
            puts("wallpaper: "); puts(tn);
            puts(wall_src == wallpaper_rgb ? " (showing photo)\n" : wall_src == wall_solid_row ? " (plain)\n" : " (showing map)\n");
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
    else if (!strcmp(line, "chat") || !strcmp(line, "samantha")) {
        /* 1.9.26: Samantha is a ring-3 program (user/samantha.c); the text shell opens her window like `notes`. */
        puts("chat: Samantha is a desktop window now, run gui and click her in the dock\n"); /* 2.0 gate 5: no blocking launch from the text shell */
    }
    else if (!strcmp(line, "notes")) {
        puts("notes: Notes is a desktop window now, run gui and click it in the dock\n"); /* 2.0 gate 5: no blocking launch from the text shell */
    }
    else if (!strcmp(line, "gui")) {
        gui_run();
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
                else if (pc[0]=='g' && pc[1]=='r' && pc[2]=='e') wall_theme_override = WALL_GREY;
                break;
            }
        /* 1.0.12: llmhost=HOST / llmport=PORT, the Chat equivalent of
           wxhost= above -- for tools/checks/chat-samantha-check.py to point
           chat_send at a local fake HTTP server instead of the real,
           internet-facing Turing default, headlessly and without touching
           SETTINGS.TXT. Unlike wxhost's dotted-quad-only host, this takes
           any hostname byte up to the next space or end of string, the
           same real hostnames (turing.jaybulb.com) llm_host itself
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
    /* Install the shell utility on either the mounted disk or the RAM fallback. */
    { unsigned char probe;
      if (vfs_read_file("WC.BIN", &probe, 1) < 0 &&
          ring3app_write_packed("WC.BIN", user_wc, USER_WC_CLEN, USER_WC_LEN, USER_WC_SUM, 0) < 0)
          klog("wc: could not install WC.BIN");
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
void shell_line(char *line) { run(line); }   /* kernel/progtest.c's notetest drives the real dispatcher */
