#ifndef JT_TEXT_INK_H
#define JT_TEXT_INK_H
/* Text ink curve, shared by every coverage-glyph path (gui_aa_char,
   wx_text, the Notes editor's editor_draw_glyph). Owner feedback on the
   AA text: "A-, sharpen them up a tad". Root cause of the softness: the
   DejaVu coverage bitmaps (FreeType via PIL, tools/gen/gen_editor_fonts.py)
   were blended as raw linear coverage in sRGB. A 24px vertical stem is
   ~2.2 physical px, e.g. 'l' rasterises as 188,255,108, so on a light
   surface only one column reaches full ink and the two flanking columns
   read as mid grey: the stem looks thin and fuzzy rather than inked.
   macOS gets its dense look from stem darkening plus a steep coverage
   curve; this does the same thing with a lookup, no layout change:
     dark on light:  a' = S(1 - (1-a)^1.3), S(x) = 128 + 1.2(x-128), clamped
     light on dark:  a' = S(a) only
   The first adds a little weight to thin dark stems so their cores hit
   full ink (188 -> 226, 108 -> 130); the second only steepens edges, so
   light-on-dark text (dock labels, dark chrome), which linear sRGB
   blending already makes look heavier, does not bloat. Both still pass
   through a smooth ramp of intermediate values: edges stay antialiased,
   just a shorter ramp. Faint fringes below ~8% coverage drop to zero,
   which is most of the visible "haze" around each glyph. */
static const unsigned char text_ink_dark[256] = {
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,2,4,5,7,8,10,11,13,14,16,17,19,20,22,
    23,25,26,28,29,31,32,34,35,37,38,40,41,43,44,46,47,49,50,51,53,54,56,57,59,60,62,63,64,66,67,69,
    70,72,73,75,76,77,79,80,82,83,84,86,87,89,90,91,93,94,96,97,98,100,101,103,104,105,107,108,109,111,112,113,
    115,116,118,119,120,122,123,124,126,127,128,130,131,132,134,135,136,137,139,140,141,143,144,145,147,148,149,150,152,153,154,155,
    157,158,159,161,162,163,164,166,167,168,169,170,172,173,174,175,177,178,179,180,181,183,184,185,186,187,189,190,191,192,193,194,
    196,197,198,199,200,201,203,204,205,206,207,208,209,210,211,213,214,215,216,217,218,219,220,221,222,223,224,226,227,228,229,230,
    231,232,233,234,235,236,237,238,239,240,241,242,243,244,245,245,246,247,248,249,250,251,252,253,254,255,255,255,255,255,255,255,
    255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,255,
};
static int text_luma(unsigned int c){ return (int)(((c >> 16) & 0xFF) * 77 + ((c >> 8) & 0xFF) * 150 + (c & 0xFF) * 29) >> 8; }
/* Coverage a (0..255) of a glyph pixel in colour fg over destination
   colour dst -> the alpha to actually blend with. */
static int text_ink(int a, unsigned int fg, unsigned int dst){
    if (a <= 0) return 0;
    if (a >= 255) return 255;
    if (text_luma(fg) <= text_luma(dst)) return text_ink_dark[a];
    a = 128 + (a - 128) * 6 / 5;
    return a < 0 ? 0 : a > 255 ? 255 : a;
}

#endif
