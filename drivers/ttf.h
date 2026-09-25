/* Runtime TrueType rasterizer, thin wrapper around drivers/stb_truetype.h.
   Renders glyphs at any pixel size (piece 1 of the fonts feature: lets
   text scale cleanly up to 200pt instead of the four fixed PIL bitmap
   sizes in drivers/editor_fonts.h). Not wired into Notes/UI yet. */
#ifndef TTF_H
#define TTF_H

typedef struct ttf_font ttf_font_t;

/* The six embedded faces (drivers font headers), one per family x weight the
   Notes editor exposes. Sans/Serif/Mono each carry a real regular and a
   real bold cut, no faked double-strike. */
typedef enum {
    TTF_FACE_SANS = 0,
    TTF_FACE_SANS_BOLD,
    TTF_FACE_SERIF,
    TTF_FACE_SERIF_BOLD,
    TTF_FACE_MONO,
    TTF_FACE_MONO_BOLD,
    TTF_FACE_COUNT
} ttf_face_t;

typedef struct {
    unsigned char *coverage; /* w*h bytes, 0-255 alpha, kmalloc'd; free with ttf_free_glyph */
    int width;
    int height;
    int xoff;                /* left bearing in px, add to pen x for bitmap origin */
    int yoff;                /* top bearing in px (baseline-relative), add to pen y */
    int advance;             /* horizontal advance in px, rounded */
} ttf_glyph_t;

/* Parses a TTF/OTF blob (must stay valid for the font's lifetime; not copied).
   Returns NULL on parse failure. */
ttf_font_t *ttf_load(const unsigned char *data);

/* Loads the kernel's default built-in embedded font (DejaVu Sans). */
ttf_font_t *ttf_load_default(void);

/* Loads one of the six embedded faces. Each call reparses the embedded
   blob (cheap: no file I/O, the caller is expected to cache the result). */
ttf_font_t *ttf_load_face(ttf_face_t face);

void ttf_free(ttf_font_t *font);

/* Rasterizes one glyph at px_size (pixels per em). Returns 0 on success.
   Caller must ttf_free_glyph(out) when done, even on an empty (whitespace)
   glyph, unless this returns nonzero. */
int ttf_glyph(ttf_font_t *font, unsigned int codepoint, float px_size, ttf_glyph_t *out);

void ttf_free_glyph(ttf_glyph_t *g);

/* Horizontal advance for codepoint at px_size, in pixels (rounded). */
int ttf_advance(ttf_font_t *font, unsigned int codepoint, float px_size);

/* Kerning adjustment between two consecutive codepoints at px_size, in
   pixels (rounded); 0 if the font has no kern table for the pair. */
int ttf_kerning(ttf_font_t *font, unsigned int cp1, unsigned int cp2, float px_size);

/* Ascent (baseline to line top) at px_size, in pixels (rounded); the pen's
   baseline y for a line whose top sits at some origin is origin + this. */
int ttf_ascent(ttf_font_t *font, float px_size);

#endif
