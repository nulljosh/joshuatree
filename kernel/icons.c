/* The i386 app icons, out of kernel.c since 4.20.7: one painter per app (gui_icon_*), the gloss and glyph passes, and
   the per-slot cache gui_render_icon_cached keeps so the dock never repaints an unchanged tile. */
#include "libc.h"
#include "window.h"
#include "font.h"
#include "gui_prims.h"
#include "gui_paint.h"
#include "gui_shapes.h"
#include "dock_geom.h"
#include "dock_draw.h"
#include "desktop.h"
#include "icons.h"
#include "kheap.h"
#include "trash.h"
#include "png.h"
#include "icon_art.h"

_Static_assert(GUI_ICON_ART_SIZE == ICON_ART_SIZE, "gui_paint.h and icon_art.h disagree on the artwork size");

#define aa_band gui_aa_band
#define AA_BAND aa_band
#define gui_icon_overlay gui_icon_overlay_draw

/* Real pictograms, not letters: there's no image decoder or asset pipeline
   in this kernel (deliberately, see ROADMAP.md's font/asset scope notes),
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

void gui_icon_weather(int cx, int cy, int s, unsigned int bg){
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

void gui_icon_pin(int cx, int cy, int s, unsigned int bg){
    int r = s / 4, head_cy = cy - s / 8;
    unsigned int pin_top = 0x00FF6B5B, pin_bot = 0x00E8291A; /* real map-pin red, a color, not white */
    gui_fill_circle_gradient(cx, head_cy, r, pin_top, pin_bot, bg);
    gui_fill_circle(cx, head_cy, r / 3, bg, pin_bot); /* punch the pinhole through to the icon's own background */
    gui_fill_triangle_down(cx, head_cy + r - 1, r, s / 3, pin_bot);
}

void gui_icon_chat(int cx, int cy, int s, unsigned int bg){
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
void gui_icon_folder(int cx, int cy, int s, unsigned int bg){
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
void gui_icon_keyrate(int cx, int cy, int s, unsigned int bg){
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
void gui_icon_book(int cx, int cy, int s, unsigned int bg){
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

void gui_icon_quotes(int cx, int cy, int s, unsigned int bg){
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
void gui_icon_notes(int cx, int cy, int s, unsigned int bg){
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
void gui_icon_reminders(int cx, int cy, int s, unsigned int bg){
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
void gui_icon_calendar(int cx, int cy, int s, unsigned int bg){
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
void gui_icon_mail(int cx, int cy, int s, unsigned int bg){
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
void gui_icon_tonchi(int cx, int cy, int s, unsigned int bg){
    int r = s * 3 / 10;
    gui_fill_circle(cx, cy, r, ICON_FG, bg);
    gui_draw_capsule(cx - r, cy - r / 3, cx + r, cy - r / 3, s / 24, bg, ICON_FG); /* "latitude" lines punched through in bg color */
    gui_draw_capsule(cx - r, cy + r / 3, cx + r, cy + r / 3, s / 24, bg, ICON_FG);
    gui_draw_capsule(cx, cy - r, cx, cy + r, s / 24, bg, ICON_FG); /* "meridian" */
}
void gui_icon_toroid(int cx, int cy, int s, unsigned int bg){
    int step = s / 4, r = s / 10;
    static const int alive[3][3] = {{0,1,0},{0,1,1},{1,1,0}}; /* a real small still-life pattern, not random noise */
    for (int row = 0; row < 3; row++)
        for (int col = 0; col < 3; col++)
            gui_fill_circle(cx + (col - 1) * step, cy + (row - 1) * step, r, alive[row][col] ? ICON_FG : gui_blend(ICON_FG, bg), bg);
}
void gui_icon_hikko(int cx, int cy, int s, unsigned int bg){
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
void gui_icon_trash(int cx, int cy, int s, unsigned int bg){
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

void gui_icon_apps(int cx, int cy, int s, unsigned int bg){
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
void gui_icon_terminal(int cx, int cy, int s, unsigned int bg){
    int half = s * 3 / 10, t = s / 16;
    gui_draw_capsule(cx - half, cy - half / 2, cx - half / 3, cy, t, ICON_FG, bg);
    gui_draw_capsule(cx - half / 3, cy, cx - half, cy + half / 2, t, ICON_FG, bg);
    window_rect(cx + 2, cy + half / 3, half - 2, t + 1, ICON_FG);
}
void gui_icon_fieldbook(int cx, int cy, int s, unsigned int bg){
    int half = s * 3 / 10;
    gui_draw_capsule(cx - half, cy - half / 3, cx - 2, cy + half, s / 18, ICON_FG, bg);
    gui_draw_capsule(cx + half, cy - half / 3, cx + 2, cy + half, s / 18, ICON_FG, bg);
    gui_draw_capsule(cx, cy - half / 3, cx, cy + half, s / 24, ICON_FG, bg); /* spine */
}
void gui_icon_contacts(int cx, int cy, int s, unsigned int bg){
    int r = s / 5;
    gui_fill_circle(cx - r / 2, cy - r / 2, r, ICON_FG, bg);
    gui_draw_capsule(cx - r, cy + r / 2, cx + r, cy + r, s / 20, ICON_FG, bg);
}
void gui_icon_calculator(int cx, int cy, int s, unsigned int bg){
    int w = s / 3, h = s / 2;
    window_rect(cx - w, cy - h / 2, w * 2, h, ICON_FG);
    gui_fill_circle(cx - w / 2, cy - h / 4, s / 24, ICON_FG, bg);
    gui_fill_circle(cx + w / 2, cy - h / 4, s / 24, ICON_FG, bg);
    gui_fill_circle(cx - w / 2, cy + h / 4, s / 24, ICON_FG, bg);
    gui_fill_circle(cx + w / 2, cy + h / 4, s / 24, ICON_FG, bg);
}
void gui_icon_stocks(int cx, int cy, int s, unsigned int bg){
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
void gui_icon_search(int cx, int cy, int s, unsigned int bg){
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
void gui_icon_activity(int cx, int cy, int s, unsigned int bg){
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
       kernel/memmap.h pins at JT_USER_BASE, and docs/ARCHITECTURE.md names that
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

/* clockicon.h stays with kernel.c (its time source lives there); gui_icon_overlay is reached through gui_icon_overlay_draw. */

void gui_draw_one_icon_on(int icon, int cx_center, int cy_bottom, int size, unsigned int under){
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
