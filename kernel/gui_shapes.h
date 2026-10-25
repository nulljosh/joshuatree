#ifndef GUI_SHAPES_H
#define GUI_SHAPES_H
/* kernel/gui_shapes.c: i386 shape painters split out of kernel.c. */
void gui_aa_line_phys(double fx0, double fy0, double fx1, double fy1, unsigned int color, double width);   /* logical coordinates, drawn per physical pixel */
void gui_rounded_rect_gradient(int x, int y, int w, int h, unsigned int color_top, unsigned int color_bottom, unsigned int bg, int r);
void gui_fill_triangle_down(int cx, int y0, int half_w, int h, unsigned int color);   /* antialiased, point down: the icons' pins and tips */
#endif
