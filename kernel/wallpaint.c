/* The i386 desktop wallpaper, out of kernel.c since 4.23.6: the day/night tint by the real hour, the wallpaper
   colour per theme, the wind sway over the photo, its full-screen cache, and the row and rect painters every
   repaint funnels through. i386 only; the ARM build has its own (arch/arm64/wall.c). */
#include "libc.h"
#include "window.h"
#include "gui_prims.h"
#include "gui_paint.h"
#include "desktop.h"
#include "wallmap.h"
#include "weather.h"
#include "walltint.h"
#include "irq.h"
#include "kheap.h"
#include "settings.h"
#include "wallpaint.h"

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
    unsigned char h = cmos(4);
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
unsigned int gui_wallpaper_color(int row){
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
int wind_phase = 0;     /* -256..256, current displacement scale */

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
    if (!wall_src || wall_src != wall_photo) return 0; /* v75: streets don't sway; the tick still runs (zero shift) so rain/snow keep compositing */
    if (row >= WIND_HORIZON_ROW) return 0;
    int h = WIND_HORIZON_ROW - row;                     /* 0..365 */
    int amp = (h * h) / (365 * 365 / 14);               /* up to ~14 logical px at the very top, ~6 at the crown */
    amp = amp * wind_weather_pct / 100;                 /* v60: real-weather scale, see wind_weather_pct above */
    return amp * wind_phase;                            /* * (-256..256) */
}

void gui_draw_wallpaper_rows_sway(int y_from, int y_to, int sway);
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
static int wind_base_width = 0;
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
void gui_draw_wallpaper_rows_sway_ex(int y_from, int y_to, int sway, int ex, int ey, int ew, int eh){
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
void gui_draw_wallpaper_rows_sway(int y_from, int y_to, int sway){ gui_draw_wallpaper_rows_sway_ex(y_from, y_to, sway, 0, 0, 0, 0); }

void gui_draw_wallpaper(void){
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
void gui_daynight_wallpaper_rect(int x, int y, int w, int h){
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

/* The swaying crown's raw cache, built on the first full desktop draw (gui_draw_desktop) at the current size. */
void gui_wind_base_build(void){
    if (!wind_enabled || wind_base) return;
    {
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
}

/* wall_caches_drop (kernel.c): the wind cache and the full-screen wallpaper cache are rebuilt lazily after a theme switch. */
void gui_wallpaint_caches_drop(void){
    if (wind_base) { kfree(wind_base); wind_base = 0; }
    gui_wall_full_cache_drop();
}
