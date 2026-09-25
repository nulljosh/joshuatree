#include "gui_prims.h"

/* Channel-wise average of two 0x00RRGGBB colors. No alpha channel in this
   framebuffer to composite with, so a real anti-aliased edge (a soft
   transition band instead of one hard cutoff) has to be a genuine, solid,
   precomputed color, not a blend against whatever's already drawn. */
unsigned int gui_blend(unsigned int a, unsigned int b){
    unsigned int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    unsigned int br = (b >> 16) & 0xFF, bg2 = (b >> 8) & 0xFF, bb = b & 0xFF;
    return ((ar + br) / 2 << 16) | ((ag + bg2) / 2 << 8) | ((ab + bb) / 2);
}

double gui_line_sqrt(double x){ double r; __asm__ volatile ("fsqrt" : "=t"(r) : "0"(x)); return r; }
