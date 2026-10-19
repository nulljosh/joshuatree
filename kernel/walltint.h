#ifndef WALLTINT_H
#define WALLTINT_H
/* The per-pixel wallpaper grades, always inlined into every reader (the desktop's wallpaper loop in kernel.c, and the
   tint self-tests in kernel/wxtest.c). Moved out of kernel.c in 4.19.1 so both units get the same inline code. */
#include "wallmap.h"

/* v79: the map wallpaper (wall_src pointed at wall_map instead of
   wall_photo, see v75's comment at wall_src's declaration) is real
   fetched OpenTopoMap pixel data, and OpenTopoMap ships its own neutral
   cartographer's palette (grays for streets/contours, cold greens for
   parks, flat blues for water) that never went through this file's own
   Mojave desert grade the baked photo did. Sitting next to the sand/
   granite/leather-brown dock and panels, it read as a foreign, cold
   rectangle dropped onto a warm desktop, direct request to fix.
   A grade toward this file's palette has to stay a MULTIPLY, not a lerp
   toward one flat target color the way gui_daynight_tint blends toward
   its espresso-brown/Silver floors: a lerp blend flattens every distinct
   map color toward the same target as the blend percentage climbs, which
   is exactly the "washes out detail" failure this was asked to avoid --
   street-name glyphs are 1-2px of near-black on near-white, road/water
   contrast is a gray line on a blue fill, and either one degrading toward
   a shared target erases the very contrast that makes the map legible.
   A per-channel multiply instead scales every pixel by the same warm
   ratio, so two pixels that started different stay different (their
   contrast ratio is preserved to within rounding), while the whole image
   shifts toward this palette's warmth: reds pushed up (272/256, +6.25%,
   toward Orange #FF851B's own red-forward hue), greens pulled down
   slightly (248/256, -3.1%, so parkland reads sand-toward-olive rather
   than postcard green), blues pulled down more (216/256, -15.6%, the
   real driver of the warm shift, matching wall_bot's own brown recipe a
   few hundred lines below: Orange blended with Black is red-heavy and
   blue-starved). Deliberately mild (a 15.6% max single-channel move) so
   near-white street-label backgrounds stay legibly near-white and near-
   black glyph strokes stay legibly near-black, both ends of the contrast
   range that has to survive; a heavier grade would gray-crush the whites
   the way a heavy sepia overlay does. Applied at the two real per-pixel
   choke points that read wall_src directly (gui_wallpaper_color below,
   for the icon-shadow/dock-tray blend targets, and gui_wallpaper_px
   further down, for the actual on-screen blit and the wind_base cache it
   feeds), both gated on `wall_src != wall_photo` so the baked photo's
   own already-correct palette is never touched, only the map is. Runs
   before gui_daynight_tint, not after: daynight's night/day blend is
   meant to read as "what hour it is" layered on top of whatever the base
   wallpaper actually looks like, the same order the photo already uses. */
static inline __attribute__((always_inline)) unsigned int gui_map_tint(unsigned int rgb){
    int r = (int)((rgb >> 16) & 0xFF), g = (int)((rgb >> 8) & 0xFF), b = (int)(rgb & 0xFF);
    r = (r * 272) >> 8; if (r > 255) r = 255;
    g = (g * 248) >> 8;
    b = (b * 216) >> 8;
    return ((unsigned int)r << 16) | ((unsigned int)g << 8) | (unsigned int)b;
}

/* v81: the Cool map theme, a real, distinct second grade for the direct
   request's "cooler/higher-contrast map variant for legibility", not a
   relabeled copy of Warm above. Two real moves, opposite of Warm's:
     1. channel balance pushed cool instead of warm -- blue up (300/256,
        +17.2%) and red down (208/256, -18.75%), green trimmed slightly
        (240/256, -6.25%), the mirror image of Warm's red-up/blue-down
        recipe, so the two themes are provably different curves, not the
        same curve with different constants that happen to look similar.
     2. a genuine contrast stretch on top (push every channel away from
        mid-gray 128 by ~15%, matching the 15% figure the direct request
        asked for under "higher-contrast"), which Warm deliberately does
        NOT do (Warm's own comment above stays a pure per-channel
        multiply, no contrast move, specifically to keep near-white/near-
        black street labels from crushing). Cool applies both: OpenTopoMap
        legibility is the point of this variant, so a real contrast boost
        is in scope here even though it wasn't for Warm.
   Clamped 0..255 at every step; run through the same maptinttest-shaped
   proof below (walltest) that a neutral gray goes cool (blue ends up
   strictly greater than red, the opposite assertion from Warm's), that
   distinct source colors stay distinct (multiply+affine contrast, never
   a lerp-to-one-target), and that near-white/near-black labels still
   read (contrast stretch pushes them further apart, not together). */
static inline __attribute__((always_inline)) unsigned int gui_map_tint_cool(unsigned int rgb){
    int r = (int)((rgb >> 16) & 0xFF), g = (int)((rgb >> 8) & 0xFF), b = (int)(rgb & 0xFF);
    r = (r * 208) >> 8;
    g = (g * 240) >> 8;
    b = (b * 300) >> 8; if (b > 255) b = 255;
    r = 128 + ((r - 128) * 147) / 128; if (r < 0) r = 0; if (r > 255) r = 255;
    g = 128 + ((g - 128) * 147) / 128; if (g < 0) g = 0; if (g > 255) g = 255;
    b = 128 + ((b - 128) * 147) / 128; if (b < 0) b = 0; if (b > 255) b = 255;
    return ((unsigned int)r << 16) | ((unsigned int)g << 8) | (unsigned int)b;
}

/* Keep the satellite photograph in color. A modest saturation lift makes
   trees, roofs, and roads readable without flattening their detail. */
static inline __attribute__((always_inline)) unsigned int gui_sat_color(unsigned int rgb){
    int r = (int)((rgb >> 16) & 0xFF), g = (int)((rgb >> 8) & 0xFF), b = (int)(rgb & 0xFF);
    int l = (r * 77 + g * 150 + b * 29) >> 8;
    r = l + (r - l) * 5 / 4; if (r < 0) r = 0; if (r > 255) r = 255;
    g = l + (g - l) * 5 / 4; if (g < 0) g = 0; if (g > 255) g = 255;
    b = l + (b - l) * 5 / 4; if (b < 0) b = 0; if (b > 255) b = 255;
    return ((unsigned int)r << 16) | ((unsigned int)g << 8) | (unsigned int)b;
}

/* v81: single choke point every wallpaper-theme reader goes through, so
   gui_wallpaper_color and gui_wallpaper_px (the two real per-pixel paths,
   see v79's comment above) can't drift out of sync on which theme applies
   which grade. wall_theme's own guard (never touch the baked photo) lives
   here once instead of being copy-pasted at each call site. */
static inline __attribute__((always_inline)) unsigned int gui_wall_tint(unsigned int rgb){
    if (wall_src == wall_photo) return rgb;      /* Photo: never graded */
    if (wall_src == wall_solid_row) return rgb;     /* 4.11.4: plain wall: never graded */
    if (wall_theme == WALL_COOL) return gui_map_tint_cool(rgb);
    if (wall_theme == WALL_RAW) return rgb;         /* Raw: OpenTopoMap's own palette, untouched */
    if (wall_theme == WALL_SAT) return gui_sat_color(rgb);
    return gui_map_tint(rgb);                       /* WALL_WARM, and the fallback while fetching */
}
#endif
