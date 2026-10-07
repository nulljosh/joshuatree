/* Runtime TrueType rasterizer. See ttf.h. Wraps stb_truetype.h (vendored
   verbatim, public domain / MIT, Sean Barrett) and satisfies its freestanding
   needs with the kernel heap and small local math/libc shims below, since
   the kernel has no libc or libm. */
#include "ttf.h"
#include "dejavu_font.h"
#include "dejavu_bold_font.h"
#include "dejavu_serif_font.h"
#include "dejavu_serif_bold_font.h"
#include "dejavu_mono_font.h"
#include "dejavu_mono_bold_font.h"
#include "lib_inter_font.h"
#include "lib_ebgaramond_font.h"
#include "lib_comicneue_font.h"
#include "lib_caveatbrush_font.h"
#include "lib_dancingscript_font.h"
#include "lib_jetbrainsmono_font.h"
#include "lib_sourceserif4_font.h"
#include "lib_sourcesans3_font.h"

/* The font registry: one row per face, in ttf_face_t order. */
static const struct {
    const char *name;
    const unsigned char *data;
    int mono;
} ttf_registry[TTF_FACE_COUNT] = {
    { "dejavu-sans",        dejavu_font_data,             0 },
    { "dejavu-sans-bold",   dejavu_bold_font_data,        0 },
    { "dejavu-serif",       dejavu_serif_font_data,       0 },
    { "dejavu-serif-bold",  dejavu_serif_bold_font_data,  0 },
    { "dejavu-mono",        dejavu_mono_font_data,        1 },
    { "dejavu-mono-bold",   dejavu_mono_bold_font_data,   1 },
    { "inter",              lib_inter_font_data,          0 },
    { "ebgaramond",         lib_ebgaramond_font_data,     0 },
    { "comicneue",          lib_comicneue_font_data,      0 },
    { "caveatbrush",        lib_caveatbrush_font_data,    0 },
    { "dancingscript",      lib_dancingscript_font_data,  0 },
    { "jetbrainsmono",      lib_jetbrainsmono_font_data,  1 },
    { "sourceserif",        lib_sourceserif4_font_data,   0 },
    { "sourcesans",         lib_sourcesans3_font_data,    0 },
};

const char *ttf_face_name(ttf_face_t face) {
    if ((int)face < 0 || face >= TTF_FACE_COUNT) return "";
    return ttf_registry[face].name;
}

int ttf_face_by_name(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < TTF_FACE_COUNT; i++) {
        const char *a = ttf_registry[i].name, *b = name;
        while (*a && *a == *b) { a++; b++; }
        if (*a == 0 && *b == 0) return i;
    }
    return -1;
}

int ttf_face_is_mono(ttf_face_t face) {
    if ((int)face < 0 || face >= TTF_FACE_COUNT) return 0;
    return ttf_registry[face].mono;
}

#ifdef TTF_HOST_BUILD
#include "kheap.h"
#include <string.h>
#include <stddef.h>
#define STBTT_strlen strlen
#define STBTT_memcpy memcpy
#define STBTT_memset memset
#else
#include "../kernel/kheap.h"
#include "../lib/libc.h"
#define STBTT_strlen strlen
#define STBTT_memcpy memcpy
#define STBTT_memset memset
#ifndef NULL
#define NULL ((void *)0)
#endif
typedef unsigned int ttf_size_t;
#define size_t ttf_size_t
#endif

/* ---- small freestanding float shims (no libm in the kernel) ----
   The host build (TTF_HOST_BUILD, run natively on whatever arch this Mac
   is, not i386) uses real libm so the host-check comparison against PIL
   isn't testing different math than the kernel intends; the kernel build
   uses x87 inline asm since clang -target i386 has no libm to link. */
#ifdef TTF_HOST_BUILD
#include <math.h>
static float ttf_sqrtf(float x) { return sqrtf(x); }
#elif defined(__aarch64__)
static float ttf_sqrtf(float x) { return x <= 0.0f ? 0.0f : __builtin_sqrtf(x); }   /* one fsqrt instruction */
#else
static float ttf_sqrtf(float x) {
    if (x <= 0.0f) return 0.0f;
    float r;
    __asm__ volatile ("fsqrt" : "=t"(r) : "0"(x));
    return r;
}
#endif
static float ttf_fabsf(float x) { return x < 0.0f ? -x : x; }
static int ttf_ifloor(float x) {
    int i = (int)x;
    if (x < 0.0f && (float)i != x) i -= 1;
    return i;
}
static int ttf_iceil(float x) {
    int i = (int)x;
    if (x > 0.0f && (float)i != x) i += 1;
    return i;
}
/* Only reachable from stbtt_GetGlyphSDF(), which this driver never calls;
   crude but present so the SDF code path still links. */
static float ttf_fmodf(float x, float y) {
    if (y == 0.0f) return 0.0f;
    int q = (int)(x / y);
    return x - (float)q * y;
}
#ifdef TTF_HOST_BUILD
static float ttf_powf(float x, float y) { return powf(x, y); }
static float ttf_cosf(float x) { return cosf(x); }
static float ttf_acosf(float x) { return acosf(x); }
#elif defined(__aarch64__)
/* The only callers are in the SDF path nothing here uses; a cube-root Newton step and no cos or acos keep it linking. */
static float ttf_powf(float x, float y) {
    if (x == 0.0f) return 0.0f;
    float ax = x < 0.0f ? -x : x, g = ax > 1.0f ? ax : 1.0f, n = 1.0f / y;
    for (int i = 0; i < 20; i++) {
        float gn = 1.0f, gnm1 = 1.0f;
        for (int k = 0; k < (int)n; k++) gnm1 = gn, gn *= g;
        if (gn == 0.0f) break;
        g = g - (gn - ax) / (n * gnm1);
        if (g <= 0.0f) g = 0.0001f;
    }
    return x < 0.0f ? -g : g;
}
static float ttf_cosf(float x) { (void)x; return 1.0f; }
static float ttf_acosf(float x) { (void)x; return 0.0f; }
#else
static float ttf_powf(float x, float y) {
    /* only used for cube roots (y == 1/3) in the unused SDF path; crude
       Newton's method on g^n = ax, n = 1/y, is plenty for that. */
    if (x == 0.0f) return 0.0f;
    float r, neg = x < 0.0f;
    float ax = neg ? -x : x;
    float n = 1.0f / y;
    float g = ax > 0.0f ? ax : 1.0f;
    for (int i = 0; i < 20; i++) {
        float gn = 1.0f, gnm1 = 1.0f;
        for (int k = 0; k < (int)n; k++) gnm1 = gn, gn *= g;
        if (gn == 0.0f) break;
        g = g - (gn - ax) / (n * gnm1);
        if (g <= 0.0f) g = 0.0001f;
    }
    r = g;
    return neg ? -r : r;
}
static float ttf_cosf(float x) { float r; __asm__ volatile ("fcos" : "=t"(r) : "0"(x)); return r; }
static float ttf_acosf(float x) {
    /* acos(x) = atan2(sqrt(1-x*x), x) via x87 fpatan; unused SDF path only */
    float s = ttf_sqrtf(1.0f - x * x);
    float r;
    __asm__ volatile ("fpatan" : "=t"(r) : "0"(x), "u"(s) : "st(1)");
    return r;
}
#endif

#define STBTT_ifloor(x)  ttf_ifloor(x)
#define STBTT_iceil(x)   ttf_iceil(x)
#define STBTT_sqrt(x)    ttf_sqrtf(x)
#define STBTT_pow(x, y)  ttf_powf(x, y)
#define STBTT_fmod(x, y) ttf_fmodf(x, y)
#define STBTT_cos(x)     ttf_cosf(x)
#define STBTT_acos(x)    ttf_acosf(x)
#define STBTT_fabs(x)    ttf_fabsf(x)
#define STBTT_malloc(x, u) ((void)(u), kmalloc((unsigned int)(x)))
#define STBTT_free(x, u)   ((void)(u), kfree(x))
#define STBTT_assert(x)  ((void)0)

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

struct ttf_font {
    stbtt_fontinfo info;
};

ttf_font_t *ttf_load(const unsigned char *data) {
    ttf_font_t *f = (ttf_font_t *)kmalloc(sizeof(ttf_font_t));
    if (!f) return 0;
    int off = stbtt_GetFontOffsetForIndex(data, 0);
    if (off < 0) { kfree(f); return 0; }
    if (!stbtt_InitFont(&f->info, data, off)) { kfree(f); return 0; }
    return f;
}

ttf_font_t *ttf_load_default(void) {
    return ttf_load(dejavu_font_data);
}

ttf_font_t *ttf_load_face(ttf_face_t face) {
    if ((int)face < 0 || face >= TTF_FACE_COUNT) return ttf_load(dejavu_font_data);
    return ttf_load(ttf_registry[face].data);
}

void ttf_free(ttf_font_t *font) {
    if (font) kfree(font);
}

int ttf_glyph(ttf_font_t *font, unsigned int codepoint, float px_size, ttf_glyph_t *out) {
    if (!font || !out) return -1;
    float scale = stbtt_ScaleForMappingEmToPixels(&font->info, px_size);
    int advance_i, lsb_i;
    stbtt_GetCodepointHMetrics(&font->info, (int)codepoint, &advance_i, &lsb_i);

    int x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBox(&font->info, (int)codepoint, scale, scale, &x0, &y0, &x1, &y1);
    int w = x1 - x0, h = y1 - y0;
    if (w < 0) w = 0;
    if (h < 0) h = 0;

    unsigned char *bmp = 0;
    if (w > 0 && h > 0) {
        bmp = (unsigned char *)kmalloc((unsigned int)(w * h));
        if (!bmp) return -1;
        stbtt_MakeCodepointBitmap(&font->info, bmp, w, h, w, scale, scale, (int)codepoint);
    }

    out->coverage = bmp;
    out->width = w;
    out->height = h;
    out->xoff = x0;
    out->yoff = y0;
    out->advance = ttf_ifloor(advance_i * scale + 0.5f);
    return 0;
}

void ttf_free_glyph(ttf_glyph_t *g) {
    if (g && g->coverage) { kfree(g->coverage); g->coverage = 0; }
}

int ttf_advance(ttf_font_t *font, unsigned int codepoint, float px_size) {
    if (!font) return 0;
    float scale = stbtt_ScaleForMappingEmToPixels(&font->info, px_size);
    int advance_i, lsb_i;
    stbtt_GetCodepointHMetrics(&font->info, (int)codepoint, &advance_i, &lsb_i);
    return ttf_ifloor(advance_i * scale + 0.5f);
}

int ttf_kerning(ttf_font_t *font, unsigned int cp1, unsigned int cp2, float px_size) {
    if (!font) return 0;
    float scale = stbtt_ScaleForMappingEmToPixels(&font->info, px_size);
    int k = stbtt_GetCodepointKernAdvance(&font->info, (int)cp1, (int)cp2);
    return ttf_ifloor(k * scale + 0.5f);
}

int ttf_ascent(ttf_font_t *font, float px_size) {
    if (!font) return 0;
    float scale = stbtt_ScaleForMappingEmToPixels(&font->info, px_size);
    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(&font->info, &ascent, &descent, &line_gap);
    return ttf_ifloor(ascent * scale + 0.5f);
}
