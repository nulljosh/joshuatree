/* The desktop painters both builds share: the i386 kernel and the ARM64 port (arch/arm64) link this same file.
   Integer only, so it builds under aarch64's -mgeneral-regs-only. Everything here draws through a handful of
   window.h calls (window_scale, window_pixel_phys, window_get_pixel_phys, window_fill_rect_phys) and the
   platform's gui_wallpaper_sample; drivers/window.c answers them on i386, arch/arm64/main.c on the Pi.
   Moved verbatim out of kernel.c and dock_draw.c, so the i386 pixels are the same as before. */
#include "gui_paint.h"
#include "gui_prims.h"
#include "dock_geom.h"
#include "window.h"

#define ICON_ART_SIZE GUI_ICON_ART_SIZE /* the generated header holds the artwork; only its size is needed here */

/* Channel-wise average of two 0x00RRGGBB colors. No alpha channel in this
   framebuffer to composite with, so a real anti-aliased edge (a soft
   transition band instead of one hard cutoff) has to be a genuine, solid,
   precomputed color, not a blend against whatever's already drawn. */
unsigned int gui_blend(unsigned int a, unsigned int b){
    unsigned int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    unsigned int br = (b >> 16) & 0xFF, bg2 = (b >> 8) & 0xFF, bb = b & 0xFF;
    return ((ar + br) / 2 << 16) | ((ag + bg2) / 2 << 8) | ((ab + bb) / 2);
}

/* Channel-wise linear interpolation between two 0x00RRGGBB colors, `t/max`
   of the way from `a` to `b`. */
unsigned int gui_lerp(unsigned int a, unsigned int b, int t, int max){
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
void gui_rounded_rect_on_wallpaper(int x, int y, int w, int h, unsigned int color, int r){
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
    /* Issue #14 ("everything super laggy"): every row outside the top
       band and outside a corner block (py in [pr, ph-pr)) always falls
       through the loop below to col=color with no wallpaper sample and
       no AA -- cy ends up -1 there so the `cx>=0&&cy>=0` corner branch
       never runs, and py>=pr>band so the top-edge branch never runs
       either, provably true for any r with a non-degenerate band. That
       is the whole middle of the shape, the vast majority of its area
       for any real window/panel/tray size. Bulk-filling it once with
       window_fill_rect_phys (same routing screen_pixel_ex does, paid
       once for the rect instead of once per pixel; see its own comment)
       and skipping those rows below leaves the loop doing exactly what
       it always did for the corner rows, just not for rows that were
       always going to end up flat `color` anyway. Measured on opening
       Files (~820x385 logical, tools/checks/frametime-check.py): this
       call's own share of a ~400ms open frame was ~120ms. */
    if (ph > 2 * pr) window_fill_rect_phys(px0, py0 + pr, pw, ph - 2 * pr, color);
    int outer_margin = 2; /* subsamples can land a touch past the whole-pixel test below */
    for (int py = 0; py < ph; py++){
        if (py >= pr && py < ph - pr) continue; /* the solid middle band, already filled above */
        int cy = py < pr ? pr : ph - 1 - pr;
        int oy = py - cy;
        /* v83: the straight top/bottom edge used to get its OWN linear
           band-width blend (weight purely a function of row distance from
           the edge, identical at every column) while the corner arc used
           a real per-pixel circle test -- two independently-computed AA
           models meeting at a hard x boundary (px==pr) with no shared
           math, so nothing guaranteed they agreed there. Confirmed the
           exact discontinuity with a real pmemsave capture (QA screenshot
           + tools/checks/windowedge-check.py): one column before the
           corner box the edge was ~100% background (the straight model's
           row-0 answer everywhere), one column into the corner box the
           circle test already read ~50% coverage, a hard jump that
           rendered as a visible step/notch right where the straight edge
           met the arc, plus a stair-stepped corner beyond it (the old
           circle test was fine in isolation, just never reconciled with
           its neighbour). Fix: treat the straight edge as the SAME circle
           test with the sample's x-offset pinned to 0 (ox=0 -- "directly
           below/above the arc's own centre column"), computed once per
           row since every straight column shares that offset, instead of
           a separately-tuned linear ramp. At the seam column itself
           (px==pr) the corner formula also evaluates to ox=0, so the two
           sides now literally compute the identical coverage there: one
           continuous shape, not two shapes glued together. */
        int straight_inside = -1; /* -1 = not yet computed for this row */
        for (int px = 0; px < pw; px++){
            int cx = px < pr ? pr : (px >= pw - pr ? pw - 1 - pr : px);
            int ox = px - cx;
            unsigned int col = color;
            int d2 = ox * ox + oy * oy;
            if (d2 > (pr - band - outer_margin) * (pr - band - outer_margin)){
                if (d2 > (pr + outer_margin) * (pr + outer_margin)) continue; /* comfortably outside: wallpaper untouched */
                int inside;
                if (ox == 0){
                    /* straight edge (or the corner's own seam column,
                       ox==0 there too): shared per-row coverage, computed
                       once and reused for every straight column so this
                       fix costs nothing extra across the wide flat run. */
                    if (straight_inside < 0){
                        int inside_row = 0;
                        for (int sy = 0; sy < SS; sy++){
                            int subdy = oy * SS + sy * 2 + 1 - SS;
                            for (int sx = 0; sx < SS; sx++){
                                int subdx = sx * 2 + 1 - SS;
                                long sd2 = (long)subdx * subdx + (long)subdy * subdy;
                                if (sd2 <= (long)(pr * SS) * (pr * SS)) inside_row++;
                            }
                        }
                        straight_inside = inside_row;
                    }
                    inside = straight_inside;
                } else {
                    /* v79: real coverage fraction, not a single threshold.
                       Sample SSxSS sub-points spread across this physical
                       pixel's own area and count how many fall inside
                       the true circle of radius pr; that fraction IS the
                       pixel's real AA coverage, the same quantity a 6x
                       supersample-then-box-downsample pass would produce,
                       computed directly instead of through a buffer. */
                    inside = 0;
                    for (int sy = 0; sy < SS; sy++){
                        int subdy = oy * SS + sy * 2 + 1 - SS; /* sample point offset, in 1/SS-pixel units, centred in each sub-cell */
                        for (int sx = 0; sx < SS; sx++){
                            int subdx = ox * SS + sx * 2 + 1 - SS;
                            long sd2 = (long)subdx * subdx + (long)subdy * subdy;
                            if (sd2 <= (long)(pr * SS) * (pr * SS)) inside++;
                        }
                    }
                }
                if (inside == 0) continue;               /* fully outside: leave the wallpaper alone */
                unsigned int edge_bg = gui_wallpaper_sample(px0 + px, py0 + py, 0);
                if (inside >= SS * SS) { col = color; }
                else col = gui_lerp(color, edge_bg, SS * SS - inside, SS * SS);
            }
            window_pixel_phys(px0 + px, py0 + py, col);
        }
    }
}



/* 2.0: a true 1px rule. window_rect doubles at scale 2, so the hairline is
   written per physical pixel on the logical row's last physical line. */
void gui_hairline_h(int x, int y, int w, unsigned int color){
    int sc = (int)window_scale(); if (sc < 1) sc = 1;
    for (int px = x * sc; px < (x + w) * sc; px++) window_pixel_phys(px, y * sc + sc - 1, color);
}

void gui_draw_dock_tray(void){
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
    gui_rounded_rect_on_wallpaper(dock_x, y0, dock_w, dock_h, DOCK_TRAY_COLOR, 18);
    gui_hairline_h(dock_x + 18, y0, dock_w - 36, 0x00D6D0C6); /* 2.0: same radius as windows, hairline top and bottom edge */
    gui_hairline_h(dock_x + 18, y0 + dock_h - 1, dock_w - 36, 0x00D6D0C6);
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
void gui_draw_icon_shadow(int cx_center, int cy_bottom, int size){
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
void gui_icon_art_bilinear(const unsigned char *art, unsigned int *out, int pw, unsigned int under){
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
void gui_icon_art_scale(const unsigned char *art, unsigned int *out, int pw, unsigned int under){
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

/* Copies a finished icon tile (pw x pw physical pixels, pw = size * window_scale()) onto the screen with its top-left
   at logical (x, y). Pixels equal to `under`, the tile's transparent corners, are skipped, so the contact shadow
   drawn first still shows through them. */
void gui_blit_tile(const unsigned int *tile, int x, int y, int size, unsigned int under){
    unsigned int sc = window_scale();
    int pw = size * (int)sc;
    for (int py = 0; py < pw; py++)
        for (int px = 0; px < pw; px++)
            if (tile[py * pw + px] != under)
                window_pixel_phys(x * (int)sc + px, y * (int)sc + py, tile[py * pw + px]);
}

/* The circle and capsule primitives, moved verbatim out of kernel.c for the dock's hover label and the window
   chrome. aa_band is the one AA width: kernel.c's icon renderer widens it for a supersampled render and puts it back. */
int gui_aa_band = 5;
#define AA_BAND gui_aa_band

/* No libm in this freestanding build, and these icons are small enough
   (radius well under 16px) that a plain increment-until-it-fits search is
   plenty fast for something drawn on hover, not every frame. */
int gui_isqrt(int n){
    if (n < 0) n = 0;
    int r = 0;
    while ((r + 1) * (r + 1) <= n) r++;
    return r;
}

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
void gui_fill_circle(int cx, int cy, int r, unsigned int color, unsigned int into){
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
void gui_capsule_phys(int pcx0, int pcy0, int pcx1, int pcy1, int pr, unsigned int color){
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

void gui_draw_capsule(int x0, int y0, int x1, int y1, int r, unsigned int color, unsigned int into){
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

/* The dock's hover label: the app's name in dark ink on a pale capsule with a hairline edge, the macOS dock tooltip in
   the tray's own cream. Bare light text read on dark wallpaper but vanished on bright map tiles and collided with an
   open window's bottom edge (QA tour, 2026-09-21); the hairline keeps the capsule distinct over a light window. The
   capsule spans ly-3 .. ly+19: clear of the tray's top edge and inside the band gui_dock_band_top() composes. */
void gui_draw_dock_label(int cx_center, int y0, const char *name){
    int label_w = gui_text_width(name);
    int ly = y0 - 21;
    int lx0 = cx_center - label_w / 2 - 2, lx1 = cx_center + label_w / 2 + 2;
    gui_draw_capsule(lx0, ly + 8, lx1, ly + 8, 11, DOCK_LABEL_EDGE, DOCK_LABEL_EDGE);
    gui_draw_capsule(lx0, ly + 8, lx1, ly + 8, 10, DOCK_LABEL_BG, DOCK_LABEL_BG);
    gui_text(name, cx_center - label_w / 2, ly, 0x001C1C1E);
}

/* One app window's frame: rounded body, content well, traffic lights, title. */
void gui_draw_window_frame(int x, int y, int w, int h, const char *name){
    gui_rounded_rect_on_wallpaper(x, y, w, h, 0x00F5F0EB, 18);
    window_rect(x + 8, y + 30, w - 16, h - 38, 0x00F5F0EB);
    gui_hairline_h(x + 8, y + 29, w - 16, 0x00D9D3CB); /* 2.0: one physical pixel rule under the title band */
    gui_fill_circle(x + 24, y + 16, 7, 0x00FF5F57, 0x00F5F0EB);
    gui_fill_circle(x + 46, y + 16, 7, 0x00FFD64A, 0x00F5F0EB);
    gui_fill_circle(x + 68, y + 16, 7, 0x00D8D4CE, 0x00F5F0EB);
    gui_text("x", x + 21, y + 8, 0x00602B28);
    gui_text("-", x + 43, y + 8, 0x00624A20);
    gui_text(name, x + (w - gui_text_width(name)) / 2, y + 8, 0x001C1C1E); /* 2.0: centered, full ink */
}

/* The Calendar tile shows the real date, macOS style: its art is a blank page and this writes the month and day on
   it, fresh at every draw (a date cannot be baked into the cached tile). Moved out of kernel.c so the Pi draws the
   same face. The month is a small red label whose caps are about 13% of the tile tall, the day numeral about 22% tall
   and centred under it. Dock: a 16px month and a 28px day. Bigger tiles (the Launchpad): a 24px month and a doubled
   20px day; scaling both with the tile let the day's caps reach the month's baseline. */
static const char *const GUI_CAL_MON3[12] = {"JAN","FEB","MAR","APR","MAY","JUN","JUL","AUG","SEP","OCT","NOV","DEC"};
#define GUI_CAL_RED 0x00FF3B30
#define GUI_CAL_INK 0x001F1F22
void gui_calendar_face(int cx_center, int cy_bottom, int size, int month, int day){
    int y = cy_bottom - size;
    if (month < 1 || month > 12) {
        /* No clock yet: a short red dash where the month goes and a thin ink dash where the day goes, at the text's
           own centres, so the tile reads as a calendar waiting for the time, never a made-up date. */
        int sc = (int)window_scale(), pw = size * sc, pcx = cx_center * sc, py = y * sc;
        int pr = pw * 3 / 100 < 2 ? 2 : pw * 3 / 100;
        int hm = pw * 12 / 100, hd = pw * 16 / 100;
        gui_capsule_phys(pcx - hm, py + pw * 20 / 100, pcx + hm, py + pw * 20 / 100, pr, GUI_CAL_RED);
        gui_capsule_phys(pcx - hd, py + pw * 63 / 100, pcx + hd, py + pw * 63 / 100, pr, GUI_CAL_INK);
        return;
    }
    if (day < 1 || day > 31) day = 1;
    char daybuf[3]; int n = 0;
    if (day >= 10) daybuf[n++] = (char)('0' + day / 10);
    daybuf[n++] = (char)('0' + day % 10);
    daybuf[n] = 0;
    int mul_m = 1, mul_d = 1, face_m = 0, face_d = 3;
    if (size > 40) { mul_d = 2; face_m = 2; face_d = 1; }
    const char *mon3 = GUI_CAL_MON3[month - 1];
    int ly_m = y + size * 13 / 100, ly_d = y + size * 51 / 100;
    int lwm = gui_icon_text_w(mon3, face_m, mul_m);
    gui_icon_text(mon3, cx_center - lwm / 2, ly_m, face_m, mul_m, GUI_CAL_RED);
    int lwd = gui_icon_text_w(daybuf, face_d, mul_d);
    gui_icon_text(daybuf, cx_center - lwd / 2, ly_d, face_d, mul_d, GUI_CAL_INK);
}

/* v40: a real software cursor. Save the 13x13 patch it's about to cover,
   draw, and later put that patch back exactly. Moving the cursor then
   costs ~340 pixel writes instead of repainting the desktop, which is the
   whole fix for "icons flash on hover": the flashing WAS the full
   repaint, visible because there's no double buffer, triggered by every
   single mouse packet. */
/* The backup is kept at PHYSICAL resolution (v56.1). It used to go through
   the logical layer: window_get_pixel reads only the top-left physical
   pixel of each scale x scale block and window_pixel writes the whole
   block back, so every cursor pass silently pixel-doubled whatever
   antialiased text it crossed. Surfaces that repaint on hover (menu bar,
   dock) hid it; the notification panel, never repainted while open, kept
   the damage and read as "still the old bitmap font" (roadmap, Sep 2026).
   Reproduced headlessly with a scripted sweep + pmemsave, fixed here. */
unsigned int cursor_backup[CURSOR_W * CURSOR_MAX_SCALE * CURSOR_H * CURSOR_MAX_SCALE];
int cursor_saved_x = -1, cursor_saved_y = -1;
int gui_cursor_scale(void){ int sc = (int)window_scale(); return sc > CURSOR_MAX_SCALE ? CURSOR_MAX_SCALE : sc; }
void gui_cursor_restore(void){
    if (cursor_saved_x < 0) return;
    int sc = gui_cursor_scale(), pw = CURSOR_W * sc, ph = CURSOR_H * sc;
    int px0 = cursor_saved_x * sc, py0 = cursor_saved_y * sc;
    for (int j = 0; j < ph; j++)
        for (int i = 0; i < pw; i++)
            window_pixel_phys(px0 + i, py0 + j, cursor_backup[j * pw + i]);
    cursor_saved_x = cursor_saved_y = -1;
}
void gui_cursor_save(int x, int y){
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
void gui_draw_cursor(int x, int y){
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
