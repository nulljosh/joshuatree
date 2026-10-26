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
#include "shell.h"   /* 4.20.21: the shell's dispatcher */
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
const unsigned char *wall_photo, *const wall_photo_z = wallpaper_z; const unsigned int wall_photo_z_len = WALLPAPER_Z_LEN; /* 4.23.0: packed; wallmap.c's wall_photo_get unpacks it on first use. Only this file includes drivers/wallpaper.h */
const unsigned char *wall_src = wall_solid_row;
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
const char SC[128] = {
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
u8 cmos(u8 reg){ outb(0x70, reg); return inb(0x71); }

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

void show_time(void){
    while (cmos(0x0A) & 0x80) {}            /* wait out an update */
    u8 h = cmos(4), m = cmos(2), s = cmos(0);
    print2(h); putc(':'); print2(m); putc(':'); print2(s); putc('\n');
}

/* ---- PC speaker via PIT channel 2. A square wave is all this hardware can
   produce, no envelope, no timbre, so "soft" here just means picking two
   consonant notes and a short duration rather than one harsh flat tone,
   about as warm as a beep from 1981 gets. ---- */
void beep(unsigned int freq_hz, unsigned int duration_ticks){
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

void reboot(void){
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
int boot_to_samantha;
/* "phone" (430x760 portrait, kmain's parse) and "res=WxH" (physical px from embed.js at dpr 1, gui_parse_res; opens W/2 x H/2 at scale 2) pick gui_run's mode. */
int boot_to_phone, boot_res_w, boot_res_h;
int jt_phone_mode(void){ return boot_to_phone; } /* ring3app.c: windowed ring-3 apps get argv[1]="phone" so they can show libjt/osk */
#include "hint.h"
#include "phone_home.h" /* both defined in kernel/phone_home.h, included near gui_run; forward-declared so gui_draw_app_titlebar/gui_app_mouse_tick (both defined above it) can call them */
static const int GUI_DOCK_PORTFOLIO[GUI_ICON_COUNT] = {GUI_APPS_FOLDER, 21, 20, 8, 10, 12, 14, 11, 9, 13, GUI_TRASH}; /* Portfolio, Epiphany, Curbfind, Bookrank, Tonchi, Hikko, Quotes, Keyrate, Toroid */
void gui_order_init(void){ for (int i = 0; i < GUI_ICON_COUNT; i++) gui_order[i] = portfolio_dock ? GUI_DOCK_PORTFOLIO[i] : GUI_DOCK_DEFAULT[i]; }
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

#include "wallpaint.h"   /* the wallpaper painters and the day/night tint, kernel/wallpaint.c */
void music_ring3_open(void); void keyrate_ring3_open(void); void toroid_ring3_open(void); void calculator_ring3_open(void); void quotestreak_ring3_open(void); void bookrank_ring3_open(void); void tonchi_ring3_open(void); void fieldbook_ring3_open(void); void clock_ring3_open(void); void portfolio_ring3_open(void); void activity_ring3_open(void); void contacts_ring3_open(void); void hikko_ring3_open(void); void reminders_ring3_open(void); void curbfind_ring3_open(void); void calendar_ring3_open(void); void search_ring3_open(void); void epiphany_ring3_open(void); void burrow_ring3_open(void); void mail_ring3_open(void); void notes_ring3_open(void); void terminal_ring3_open(void); void samantha_ring3_open(void); void ring3app_autoopen_arm(const char *cl); void r3stress_arm(const char *cl); void r3stress_desktop_round(void); void ring3app_autoopen_run(int mx, int my); void entropy_init(void); void entropy_bytes(void *buf, unsigned int n); void pdestress_desktop_round(void);
void movies_ring3_open(void); void hamurabi_ring3_open(void); void windgate_ring3_open(void); void panes_ring3_open(void); void claude_ring3_open(void); void mines_ring3_open(void); void books_ring3_open(void); void solitaire_ring3_open(void); void paddle_ring3_open(void); void bricks_ring3_open(void); void depths_ring3_open(void); void snake_ring3_open(void); void chess_ring3_open(void);

int gui_ring3_windowed(int icon);
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
#include "menubar.h"   /* the menu bar, kernel/menubar.c */
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
    gui_wind_base_build();
    gui_menubar_force_redraw(); gui_draw_menubar(); /* 2.6.34: every full repaint redraws the bar; the Launchpad clears it and the minute gate left it gone */
    gui_draw_dock(hover_slot, drag_slot, drag_mx, drag_my);
}

/* v75: see wall_apply. wind_base is rebuilt lazily by gui_draw_desktop,
   the dock band by gui_redraw_dock_band's own top-mismatch check. */
void wall_caches_drop(void){
    gui_wallpaint_caches_drop();
    dock_band_cache_top = -1;
}

/* The software cursor (save-under, the antialiased arrow) lives in gui_paint.c, shared with the ARM build. */

#include "appview.h"   /* an app's viewport, its title-bar drag and title strip, kernel/appview.c */
#include "auth.h"
#include "editor.h"

#include "uitext.h"   /* the antialiased UI text and the wx_* painters, kernel/uitext.c */

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
int fs_ok_global = 0;
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
const char *gui_notice_name = 0; static unsigned int gui_notice_until = 0;
void gui_refuse_open(int icon){
    if (icon < 0 || icon >= GUI_APP_COUNT) return;
    gui_notice_name = APPS[icon].name; gui_notice_until = ticks() + 300;
    serial_puts("winrefuse: "); serial_puts(APPS[icon].name); serial_puts("\n"); /* tools/checks/dockcap-fallback-check.sh */
}
void gui_notice_draw(void){
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
    /* 32 */ {"Solitaire",  0x00B5502C, gui_icon_apps,       solitaire_ring3_open,  0, 0}, /* 4.23: Klondike, ring 3 (user/solitaire.c), Apps folder only like Mines; authored art is art/icons/solitaire.svg */
    /* 33 */ {"Paddle",       0x001C1C1E, gui_icon_apps,       paddle_ring3_open,       0, 0}, /* 4.23: Paddle against the computer, ring 3 (user/paddle.c), Apps folder only; authored art is art/icons/paddle.svg */
    /* 34 */ {"Bricks",     0x00B5502C, gui_icon_apps,       bricks_ring3_open,     0, 0}, /* 4.23: the brick-breaking game, ring 3 (user/bricks.c), Apps folder only; authored art is art/icons/bricks.svg */
    /* 35 */ {"Depths",     0x002A2724, gui_icon_apps,       depths_ring3_open,     0, 0}, /* 4.23: a first-person maze shooter, ring 3 (user/depths.c), Apps folder only; authored art is art/icons/depths.svg */
    /* 36 */ {"Snake",      0x00375A4A, gui_icon_apps,       snake_ring3_open,      0, 0}, /* 4.23: Snake, ring 3 (user/snake.c), Apps folder only; authored art is art/icons/snake.svg */
    /* 37 */ {"Chess",      0x00302C29, gui_icon_apps,       chess_ring3_open,      0, 0}, /* 4.25: Chess against the computer, ring 3 (user/chess.c), Apps folder only; authored art is art/icons/chess.svg */
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

void putdec(int v){   /* 4.20.1: kernel/progtest.c prints with it too */
    char tmp[12]; int i = 0;
    unsigned u = v < 0 ? (unsigned)(-v) : (unsigned)v;
    if (v < 0) putc('-');
    do { tmp[i++] = (char)('0' + u % 10); u /= 10; } while (u);
    while (i) putc(tmp[--i]);
}

/* split_argv and run(), the shell's dispatcher, live in kernel/shell.c since 4.20.21 (shell_line there). */

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
        shell_line(line);
    }
}
