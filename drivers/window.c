#include "window.h"
#include "vbe.h"
#include "paging.h"
#include "kheap.h"
#include "serial.h"

typedef unsigned int u32;

static u32 *fb = 0;
static u32 win_w = 0, win_h = 0;   /* logical size, what every caller lays out against */
static u32 phys_w = 0, scale = 1;  /* v41: physical framebuffer may be an integer multiple */
static int view_x = 0, view_y = 0;
static u32 view_w = 0, view_h = 0;
static u32 *screen_band = 0;
static int screen_band_top = 0;
static u32 screen_band_h = 0;

/* v0.77.x: the real back buffer the roadmap's "a compositor in gui_run"
   item has been waiting on, and the systemic fix for the whole "every
   click or keystroke redraws the page" class of report. Every drawing
   call in this kernel already funnels through exactly three writers
   (window_pixel_phys, window_clear, window_phys_row), so pointing those
   three at an offscreen buffer of the same physical geometry makes every
   repaint invisible: the viewer only ever sees window_present() copy the
   damaged rectangle across in one pass, never the erase-then-redraw in
   between. Apps that still repaint their whole window stay just as
   wasteful as they were, but they stop flashing, which is the part the
   owner actually sees.

   Cost at the real mode this runs at (960x540 logical, scale 2, so
   1920x1080 physical): 1920*1080*4 = 8,294,400 bytes. QEMU's default is
   128MB of RAM and paging.c's MAX_EXTRA_TABLES (16) gives 64MB of
   mapping headroom, against a GUI working set that was ~19MB before
   this, so no ceiling needs raising. On a machine where the kmalloc
   fails (v86's 32MB browser demo is the real one) back stays 0 and every
   path below falls straight through to the framebuffer, i.e. exactly the
   behaviour that shipped before this change, no new failure mode. */
static u32 *back = 0;

static int present_logged = 0;
/* Not debug state: a real frame counter the host-side checks read by symbol.
   With drawing offscreen, a kernel variable changing no longer means the
   screen has changed, so a check that samples the framebuffer has to wait
   for a real present first. editor_qa.py hit exactly that race and failed
   in CI, sampling a frame the kernel had drawn but not yet shown.
   Initialized non-zero deliberately, so it lands in .data next to the other
   symbols those checks read rather than out in .bss past the 4MB line,
   where their "physical = virtual - 0xC0000000" address math stops holding.
   Callers compare it for change, never for an absolute value. */
volatile unsigned int window_present_count = 1;

/* v0.78.x: per-row damage spans, not one screen-wide bounding box.
   The first cut tracked a single union bbox, and a real measurement of a
   dock hover showed why that was wrong: 91 of 112 presents covered rows
   60..1080, 1020 of the 1080 rows on screen. Nothing had drawn most of
   that. The wind sway band repaints near the top and the dock band
   repaints at the bottom, and a single bbox of two disjoint regions
   swallows everything between them, so a hover frame pushed 1.96M pixels
   (7.8MB) to the framebuffer to show maybe 86k pixels of real change.

   One span per row fixes it exactly, with no heuristics and no rect-list
   merging to get wrong: two compares per pixel on the write path (down
   from four), and a present that copies precisely the columns each row
   actually touched. Disjoint regions stay disjoint both vertically (rows
   nothing drew into are skipped) and horizontally (a row only copies its
   own dirty span). Costs 2 ints per physical row, 8.6KB at 1080p. */
static int *row_x0 = 0, *row_x1 = 0;
static int dmg_y0 = 0, dmg_y1 = 0;   /* row range to scan; empty when y1 <= y0 */
#define DMG_EMPTY 0x7FFFFFFF

static void damage_reset(void) {
    dmg_y0 = DMG_EMPTY; dmg_y1 = 0;
    if (row_x0) for (u32 y = 0; y < win_h * scale; y++) { row_x0[y] = DMG_EMPTY; row_x1[y] = 0; }
}
static void damage_all(void) {
    if (!row_x0) return;
    dmg_y0 = 0; dmg_y1 = (int)(win_h * scale);
    for (int y = 0; y < dmg_y1; y++) { row_x0[y] = 0; row_x1[y] = (int)phys_w; }
}
static void damage_add(int x, int y) {
    if (x < row_x0[y]) row_x0[y] = x;
    if (x + 1 > row_x1[y]) row_x1[y] = x + 1;
    if (y < dmg_y0) dmg_y0 = y;
    if (y + 1 > dmg_y1) dmg_y1 = y + 1;
}

/* Precise damage for a caller that writes a known rectangle directly,
   instead of letting the per-pixel path infer it. window_phys_row's one
   real caller needs this: it hands out a raw row pointer, so without it
   the only safe assumption is the whole row width. */
void window_damage(int x, int y, int w, int h) {
    if (!back || !row_x0) return;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w, y1 = y + h;
    if (x1 > (int)phys_w) x1 = (int)phys_w;
    if (y1 > (int)(win_h * scale)) y1 = (int)(win_h * scale);
    if (x1 <= x0 || y1 <= y0) return;
    for (int yy = y0; yy < y1; yy++) {
        if (x0 < row_x0[yy]) row_x0[yy] = x0;
        if (x1 > row_x1[yy]) row_x1[yy] = x1;
    }
    if (y0 < dmg_y0) dmg_y0 = y0;
    if (y1 > dmg_y1) dmg_y1 = y1;
}

void window_set_viewport(int x, int y, u32 w, u32 h) { view_x = x; view_y = y; view_w = w; view_h = h; }
void window_clear_viewport(void) { view_w = view_h = 0; view_x = view_y = 0; }
void window_push_screen_band(u32 *buf, int top, u32 h) { screen_band = buf; screen_band_top = top; screen_band_h = h; }
void window_pop_screen_band(void) { screen_band = 0; screen_band_top = 0; screen_band_h = 0; }

static u32 *screen_pixel_ex(int px, int py, int write) {
    if (view_w) {
        if (px < 0 || py < 0 || (u32)px >= view_w * scale || (u32)py >= view_h * scale) return 0;
        px += view_x * (int)scale; py += view_y * (int)scale;
    }
    if (px < 0 || py < 0 || (u32)px >= phys_w || (u32)py >= win_h * scale) return 0;
    if (screen_band) {
        /* an offscreen band compose: this write never touches the screen
           or the back buffer, so it damages neither */
        if (py < screen_band_top || (u32)(py - screen_band_top) >= screen_band_h) return 0;
        return &screen_band[(u32)(py - screen_band_top) * phys_w + (u32)px];
    }
    if (back) {
        if (write) damage_add(px, py);
        return &back[(u32)py * phys_w + (u32)px];
    }
    return &fb[(u32)py * phys_w + (u32)px];
}
static u32 *screen_pixel(int px, int py) { return screen_pixel_ex(px, py, 0); }

/* A single redirectable render target, real supersampling for anything
   that wants it (icons, first user: gui_draw_one_icon renders each one
   at a few times its real size into a heap buffer, then box-downsamples
   it back to screen size, real anti-aliasing from oversampling instead
   of AA_BAND's discrete color-stepping, which visibly bands under a
   close zoom no matter how wide the band gets). Not a stack: nothing in
   this kernel needs to nest offscreen targets, one level covers every
   real caller, and a stack would be complexity nothing asks for. */
static u32 *target_fb = 0;
static u32 target_w = 0, target_h = 0;

int window_has_target(void) { return target_fb != 0; }
void window_push_target(u32 *buf, u32 w, u32 h) { target_fb = buf; target_w = w; target_h = h; }
void window_pop_target(void) { target_fb = 0; target_w = 0; target_h = 0; }

/* v41: open a physical mode `s` times larger than the logical one. Every
   existing caller keeps drawing in logical coordinates and window_pixel
   fills an s x s block, so nothing above this file has to know; the only
   thing that changes is that code which *wants* real physical pixels
   (icons) can write them through window_pixel_phys and come out s times
   sharper. This is the honest fix for "big pixels": vector polish had
   hit the 800x600 ceiling, the pixels themselves had to get smaller. */
int window_open_scaled(u32 width, u32 height, u32 bpp, u32 s) {
    u32 addr;
    if (s < 1) s = 1;
    if (!vbe_set_mode(width * s, height * s, bpp, &addr)) return 0;
    if (!paging_map_region(addr, width * s * height * s * (bpp / 8))) { vbe_disable(); return 0; }
    fb = (u32 *)addr;
    win_w = width;
    win_h = height;
    phys_w = width * s;
    scale = s;
    back = (u32 *)kmalloc(phys_w * height * s * 4);
    row_x0 = (int *)kmalloc(height * s * sizeof(int));
    row_x1 = (int *)kmalloc(height * s * sizeof(int));
    if (!back || !row_x0 || !row_x1) {
        /* all or nothing: a back buffer with no damage tracking would be a
           screen nothing ever updates, which is worse than no back buffer */
        if (back) { kfree(back); back = 0; }
        if (row_x0) { kfree(row_x0); row_x0 = 0; }
        if (row_x1) { kfree(row_x1); row_x1 = 0; }
    }
    if (back) for (u32 i = 0; i < phys_w * height * s; i++) back[i] = 0;
    damage_reset();
    if (back) damage_all();
    return 1;
}

int window_open(u32 width, u32 height, u32 bpp) {
    return window_open_scaled(width, height, bpp, 1);
}

u32 window_scale(void) { return scale; }

u32 window_get_pixel_phys(int px, int py) {
    u32 *p = screen_pixel(px, py);
    return p ? *p : 0;
}

u32 *window_phys_row(int py) {
    if (view_w || screen_band || py < 0 || (u32)py >= win_h * scale) return 0;
    if (back) {
        /* No damage is recorded here: a raw row pointer says nothing about
           which columns the caller will touch, and assuming the whole row
           was the measured cost of the dock hover path (1920 columns
           damaged to change ~174). Callers that take a row pointer declare
           what they wrote with window_damage. */
        return back + (u32)py * phys_w;
    }
    return fb + (u32)py * phys_w;
}

void window_pixel_phys(int px, int py, u32 color) {
    u32 *p = screen_pixel_ex(px, py, 1);
    if (p) *p = color;
}

/* Copies the damaged rectangle of the back buffer onto the real
   framebuffer in one pass, then clears the damage. This is the only
   moment anything a caller drew becomes visible, so it belongs at a real
   frame boundary (every place a loop has finished drawing and is about
   to wait for input), never in the middle of one. A no-op when there is
   no back buffer, and a no-op when nothing was drawn since the last
   call, which is what makes the ~100Hz idle loop free. */
void window_present(void) {
    if (!back || !fb || !row_x0) return;
    if (dmg_y1 <= dmg_y0) return;
    int y0 = dmg_y0 < 0 ? 0 : dmg_y0;
    int y1 = dmg_y1 > (int)(win_h * scale) ? (int)(win_h * scale) : dmg_y1;
    for (int y = y0; y < y1; y++) {
        int x0 = row_x0[y], x1 = row_x1[y];
        row_x0[y] = DMG_EMPTY; row_x1[y] = 0;
        if (x1 <= x0) continue;                      /* nothing drew into this row */
        if (x0 < 0) x0 = 0;
        if (x1 > (int)phys_w) x1 = (int)phys_w;
        /* a tight run copy over the row's own dirty span, the shape a real
           memcpy compiles to, rather than an indexed per-pixel loop */
        const u32 *src = back + (u32)y * phys_w + (u32)x0;
        u32 *dst = fb + (u32)y * phys_w + (u32)x0;
        for (int n = x1 - x0; n > 0; n--) *dst++ = *src++;
    }
    dmg_y0 = DMG_EMPTY; dmg_y1 = 0;
    window_present_count++;
    /* Discriminating marker for tools/checks/backbuffer-check.sh. Capped:
       this fires at every frame boundary, so left uncapped it writes eight
       bytes out the serial port forever and buries every other check's own
       markers under megabytes of log. The cap is far above what any check
       needs to tell "presenting" from "not presenting". */
    if (present_logged < 256) { present_logged++; serial_puts("present\n"); }
}

int window_has_back_buffer(void) { return back != 0; }

/* A real, self-contained proof that drawing is genuinely offscreen, not a
   claim about it: write a known value through the normal drawing path,
   read the VISIBLE framebuffer back directly (not window_get_pixel_phys,
   which now reads the back buffer), and require it to still hold the old
   value. Then present, and require it to have changed. Restores the pixel
   it borrowed either way. Returns 1 only when the whole sequence holds.
   Checked once from gui_run and reported over serial; see
   tools/checks/backbuffer-check.sh. */
int window_backbuffer_selftest(void) {
    if (!back || !fb) return 0;
    int px = (int)phys_w - 1, py = (int)(win_h * scale) - 1;
    u32 was_fb = fb[(u32)py * phys_w + (u32)px];
    u32 was_back = back[(u32)py * phys_w + (u32)px];
    u32 probe = was_fb ^ 0x00FFFFFF;
    damage_reset();
    window_pixel_phys(px, py, probe);
    int offscreen = (fb[(u32)py * phys_w + (u32)px] == was_fb); /* the screen must NOT have moved yet */
    window_present();
    int presented = (fb[(u32)py * phys_w + (u32)px] == probe);
    back[(u32)py * phys_w + (u32)px] = was_back;
    fb[(u32)py * phys_w + (u32)px] = was_fb;
    damage_all(); /* the caller's own first real frame repaints everything anyway */
    return offscreen && presented;
}

void window_close(void) {
    /* v77: unmap the framebuffer region, freeing its page tables for reuse.
       This is critical on the browser demo where the GUI runs and exits
       multiple times per session, or on systems where the heap needs to grow
       for other work after the GUI closes. Without this, every window_open
       consumes MAX_EXTRA_TABLES until all 16 are exhausted and nothing else
       can map new regions. fb is a physical address (valid as virtual due to
       identity mapping), so pass it directly to paging_unmap_region. */
    if (fb) {
        u32 size = phys_w * win_h * scale * 4; /* bytes per pixel = 4 */
        paging_unmap_region((u32)fb, size);
    }
    vbe_disable();
    if (back) { kfree(back); back = 0; }
    if (row_x0) { kfree(row_x0); row_x0 = 0; }
    if (row_x1) { kfree(row_x1); row_x1 = 0; }
    dmg_y0 = DMG_EMPTY; dmg_y1 = 0;
    fb = 0;
    win_w = win_h = 0;
    phys_w = 0; scale = 1;
    window_clear_viewport(); window_pop_screen_band();
}

void window_clear(u32 color) {
    if (view_w) {
        for (u32 y = 0; y < view_h * scale; y++)
            for (u32 x = 0; x < view_w * scale; x++) window_pixel_phys((int)x, (int)y, color);
    } else {
        u32 *dst = back ? back : fb;
        for (u32 i = 0; i < phys_w * win_h * scale; i++) dst[i] = color;
        if (back) damage_all();
    }
}

void window_pixel(int x, int y, u32 color) {
    if (target_fb) {
        if (x < 0 || y < 0 || (u32)x >= target_w || (u32)y >= target_h) return;
        target_fb[(u32)y * target_w + (u32)x] = color;
        return;
    }
    if (x < 0 || y < 0 || (u32)x >= (view_w ? view_w : win_w) || (u32)y >= (view_h ? view_h : win_h)) return;
    for (u32 j = 0; j < scale; j++)
        for (u32 i = 0; i < scale; i++) window_pixel_phys(x * (int)scale + (int)i, y * (int)scale + (int)j, color);
}

void window_rect(int x, int y, int w, int h, u32 color) {
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            window_pixel(xx, yy, color);
}

/* v40: read-back, so a software cursor can save what it's about to cover
   and put it back exactly, whatever it was (wallpaper, dock, an icon). */
u32 window_get_pixel(int x, int y) {
    if (target_fb) {
        if (x < 0 || y < 0 || (u32)x >= target_w || (u32)y >= target_h) return 0;
        return target_fb[(u32)y * target_w + (u32)x];
    }
    return window_get_pixel_phys(x * (int)scale, y * (int)scale);
}

u32 window_width(void)  { return target_fb ? target_w : (view_w ? view_w : win_w); }
u32 window_height(void) { return target_fb ? target_h : (view_h ? view_h : win_h); }
