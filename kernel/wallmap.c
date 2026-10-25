/* The map and satellite wallpapers, out of kernel.c since 4.18.3: the tile fetch (OpenTopoMap PNGs or Google satellite
   JPEGs around the place geo_* names), the theme switch, the baked satellite fallback (kernel/wall_sat.h, decoded once)
   and wall_apply, which points wall_src at what the desktop draws. kernel/wallmap.h is the interface. The per-pixel
   readers (gui_wallpaper_row, gui_wallpaper_px) stay in kernel.c, inlined in the hot draw loops. */
#include "libc.h"
#include "net.h"
#include "http.h"
#include "png.h"
#include "jpeg.h"
#include "serial.h"
#include "kheap.h"
#include "irq.h"
#include "weather.h"
#include "wall_sat.h"
#include "wallmap.h"

/* 4.23.0: the tree photo ships packed (drivers/wallpaper.h: up-filtered, then deflated, about 520KB in place of
   1.5MB of raw RGB), which is what gave the kernel image room for more apps. The default wall is plain grey, so
   nothing pays to unpack it until something shows it: the Photo wall, a map wall before its tiles land, the
   satellite fallback, pngtest and wxtest. If the heap or the inflate fails, the caller gets the plain row back,
   which every wall reader already handles, never a short buffer. */
const unsigned char *wall_photo_get(void){
    static int failed;   /* one try: a heap too small now is too small on the next wall switch too */
    if (wall_photo) return wall_photo;
    if (failed) return wall_solid_row;
    unsigned int row = WALLPAPER_W * 3, n = row * WALLPAPER_H;
    unsigned char *p = (unsigned char *)kmalloc(n);
    if (!p || png_zlib_inflate(wall_photo_z, wall_photo_z_len, p, n) != 0) {
        serial_puts("wallpaper: photo unpack failed\n");
        if (p) kfree(p);
        failed = 1;
        return wall_solid_row;
    }
    for (unsigned int i = row; i < n; i++) p[i] = (unsigned char)(p[i] + p[i - row]);   /* undo the Up filter, top down */
    serial_puts("wallpaper: photo unpacked\n");
    return wall_photo = p;
}

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
char tile_override_host[20] = "";
unsigned short tile_override_port = 80;
/* 1.1.2: walltheme=map|sat|photo, a one-boot theme override applied AFTER
   settings_load() (same reasoning as llmhost=): the default has been
   WALL_SAT since v0.76.7, and tools/checks/wallpaper-check.py compares
   the first automatic fetch against OpenTopoMap PNGs, so it boots with
   walltheme=map instead of assuming the default. -1 = not given. */
int wall_theme_override = -1;

/* v75 (0.67.0): the real location-dynamic wallpaper, the item ROADMAP.md's
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
int wall_fetch(void){
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
void wall_switch_theme(int theme){
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
   points readers at, same layout wall_photo and wall_map already use.
   On any decode failure this leaves wall_sat_rgb null and the caller falls
   back to wall_photo, never a null wall_src. */
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
void wall_apply(int want_map){
    const unsigned char *next;
    if (wall_theme >= WALL_GREY) {
        /* 4.11.4: a plain wall. No tile, no photo decode: two rows of the
           colour (the buffer is tiny, so refilling on every call is free
           and a Grey -> Clay switch needs no other bookkeeping). */
        unsigned int c = wall_solid_color[wall_theme - WALL_GREY];
        for (int i = 0; i < WALLPAPER_W * 2; i++) {
            wall_solid_row[i * 3] = (unsigned char)(c >> 16);
            wall_solid_row[i * 3 + 1] = (unsigned char)(c >> 8);
            wall_solid_row[i * 3 + 2] = (unsigned char)c;
        }
        next = wall_solid_row;
    } else if (want_map && !wall_map){
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
        next = wall_sat_rgb ? wall_sat_rgb : wall_photo_get();
    } else {
        next = (want_map && wall_map) ? wall_map : wall_photo_get();
    }
    if (next == wall_src && wall_theme == wall_last_theme) return;
    /* Real-evidence marker for tools/checks/wallboot-check.sh, the same
       "log it at the one real choke point" pattern wall_switch_theme's
       own walltheme= line already uses. Fires every
       time wall_src actually changes buffer, which includes the very
       first call (wall_src's static initializer is wall_photo and
       wall_last_theme starts at -1, so the first real assignment always
       logs), letting a headless boot prove what buffer the first real
       desktop paint used without guessing from timing alone. "satfallback"
       is its own distinct value, not folded into "map": it is the baked
       v86 satellite capture, not a live wall_map fetch, and a check that
       cannot tell them apart could not prove the v86 fallback bake-in
       actually took effect. */
    serial_puts("wallsrc=");
    serial_puts(next == wall_photo ? "photo" : (next == wall_solid_row ? "solid" : (next == wall_sat_rgb ? "satfallback" : "map")));
    serial_puts("\n");
    wall_src = next;
    wall_last_theme = wall_theme;
    wall_caches_drop();
}
