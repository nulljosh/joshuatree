#ifndef GUI_PRIMS_H
#define GUI_PRIMS_H

/* Small, pure GUI math primitives with no state of their own, split out of
   kernel.c so the color/geometry helpers every icon and line drawer leans
   on aren't buried in the one file. */

/* Channel-wise average of two 0x00RRGGBB colors. */
unsigned int gui_blend(unsigned int a, unsigned int b);

/* sqrt() via the x87 fsqrt instruction, used by the line/circle primitives. */
double gui_line_sqrt(double x);

#endif
