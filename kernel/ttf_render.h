/* Shared runtime-TTF rendering helpers, factored out of editor.h's Notes
   path so the Terminal's fixed-width grid can draw through the exact same
   pipeline instead of a second copy: per-face font cache, a small glyph
   cache, monospace cell-width rounding, and the coverage-based ink/AA
   blend (text_ink, defined just above this include in kernel.c). Anything
   that draws real DejaVu glyphs at physical resolution belongs here now;
   editor.h and the terminal's mono renderer are both thin callers. */
#ifndef TTF_RENDER_H
#define TTF_RENDER_H

/* One cached ttf_font_t per face, lifetime = the kernel's whole run.
   ttf_load_face() reparses the embedded blob on every call (cheap, no file
   I/O), but there is no reason to pay that per glyph. */
static ttf_font_t *ttfr_fonts[TTF_FACE_COUNT];

static ttf_font_t *ttfr_face(ttf_face_t face) {
    if (!ttfr_fonts[face]) ttfr_fonts[face] = ttf_load_face(face);
    return ttfr_fonts[face];
}

/* Glyph cache, keyed by codepoint + physical px size (quantized to
   quarters of a px) + face. Direct-mapped, oldest entry per bucket
   evicted on a collision -- same shape editor.h used before this file
   existed, just shared now so the terminal's glyphs get the same cheap
   redraw. */
#define TTFR_CACHE_N 256
typedef struct { int used; unsigned int cp; unsigned int pxkey; unsigned int face; ttf_glyph_t g; } ttfr_cache_t;
static ttfr_cache_t ttfr_cache[TTFR_CACHE_N];

static unsigned int ttfr_pxkey(float px) { return (unsigned int)(px * 4.0f + 0.5f); }

static ttf_glyph_t *ttfr_glyph(ttf_face_t face, unsigned int cp, float px) {
    unsigned int pxkey = ttfr_pxkey(px);
    unsigned int slot = (cp * 2654435761u + pxkey * 40503u + (unsigned int)face * 2246822519u) % TTFR_CACHE_N;
    ttfr_cache_t *e = &ttfr_cache[slot];
    if (e->used && e->cp == cp && e->pxkey == pxkey && e->face == (unsigned int)face) return &e->g;
    if (e->used) ttf_free_glyph(&e->g);
    if (ttf_glyph(ttfr_face(face), cp, px, &e->g) != 0) { e->used = 0; return 0; }
    e->used = 1; e->cp = cp; e->pxkey = pxkey; e->face = (unsigned int)face;
    return &e->g;
}

/* Monospace cell width: ttf_advance() already rounds to an int, so the
   only real work here is picking the px size whose rounded advance lands
   exactly on the caller's target physical cell (the terminal and
   Keyrate's typed line both draw into a fixed cell already sized by the
   old bitmap font, so this walks sizes near that target rather than
   trusting a single guessed em size). Returns the px size to rasterize
   at; the caller already knows the cell width it asked for. Search is
   small (a few dozen half-px steps) and the result is cached by the
   caller, so this never runs per glyph. */
static float ttfr_mono_px_for_cell(int target_cell_px) {
    ttf_font_t *f = ttfr_face(TTF_FACE_MONO);
    float best_px = (float)target_cell_px * 1.6f; /* DejaVu Sans Mono's advance is ~0.6em; a reasonable starting guess */
    int best_diff = 1 << 30;
    for (float px = (float)target_cell_px * 1.0f; px <= (float)target_cell_px * 2.2f; px += 0.25f) {
        int adv = ttf_advance(f, ' ', px);
        int diff = adv - target_cell_px;
        if (diff < 0) diff = -diff;
        if (diff < best_diff) { best_diff = diff; best_px = px; }
        if (diff == 0) break;
    }
    return best_px;
}

/* Blends one already-rasterized glyph's coverage into the framebuffer at
   physical (base_x, base_y), through the same pre-boost + text_ink curve
   editor.h's Notes path uses (stb_truetype's raw coverage reads a little
   lighter than the ink curve was tuned against; see editor_draw_glyph's
   own note). clip_lo/clip_hi are physical x bounds (a fixed cell), so
   neighbouring monospace glyphs never bleed into each other -- pass
   INT_MIN/INT_MAX from a caller that does not need clipping (Notes,
   which wraps by measured advance rather than a fixed cell). */
static void ttfr_blend_glyph(ttf_glyph_t *g, int base_x, int base_y, unsigned int fg, int clip_lo, int clip_hi) {
    if (!g || !g->coverage) return;
    for (int row = 0; row < g->height; row++) {
        for (int col = 0; col < g->width; col++) {
            int a = g->coverage[row * g->width + col];
            if (!a) continue;
            a = a > 224 ? 255 : a * 8 / 7;
            if (a > 255) a = 255;
            int x = base_x + g->xoff + col, y = base_y + g->yoff + row;
            if (x < clip_lo || x >= clip_hi) continue;
            unsigned int d = window_get_pixel_phys(x, y);
            a = text_ink(a, fg, d);
            if (!a) continue;
            unsigned int r = (((fg >> 16) & 0xFF) * a + (int)((d >> 16) & 0xFF) * (255 - a)) / 255;
            unsigned int gg = (((fg >> 8) & 0xFF) * a + (int)((d >> 8) & 0xFF) * (255 - a)) / 255;
            unsigned int b = ((fg & 0xFF) * a + (int)(d & 0xFF) * (255 - a)) / 255;
            window_pixel_phys(x, y, (r << 16) | (gg << 8) | b);
        }
    }
}

#endif
