/* apps_geom.h: where the Apps folder (the Launchpad) puts its glass panel,
   icons and labels. Pure integer math in logical pixels, so every position
   and size is a whole pixel (no half-pixel blur at 2x). Included by kernel.c.

   The panel is centered across the screen and in the strip between the menu
   bar and the dock. Padding is even: APPS_PAD on the left, right and bottom
   of the content, and the same above the hint line. Icons are 56px (the old
   74px, 24 percent smaller) in 120 by 88 cells, so a label never touches the
   next one. When the strip is too short for three rows the folder shows two
   or one and scrolls the rest. */
#ifndef JT_APPS_GEOM_H
#define JT_APPS_GEOM_H
#define APPS_COLS 5
#define APPS_TILE 56
#define APPS_CELL_W 120
#define APPS_CELL_H 94
#define APPS_PAD 24      /* panel edge to the hint line and to the last label */
#define APPS_PADX 28     /* panel edge to the first and last cell */
#define APPS_GRID_TOP 54 /* panel top to the first icon row */
#define APPS_LABEL_H 80  /* icon top to the bottom of its label */
#define APPS_MARGIN 8    /* window content edge to the panel */
#define APPS_FRAME_H 40  /* title bar 32 plus bottom border 8 */
#define APPS_FRAME_W 16
struct apps_geom {
    int vis, cell_w, cell_h, tile;
    int px, py, pw, ph; /* glass panel, viewport coordinates */
    int x0, y0;         /* first cell's left edge, first icon row's top */
};
static inline int apps_panel_h(int rows){ return APPS_GRID_TOP + (rows - 1) * APPS_CELL_H + APPS_LABEL_H + APPS_PAD; }
/* Panel top, centered in the strip from top (menu bar bottom) to bot (dock top). */
static inline int apps_panel_y(int top, int bot, int rows){ return (top + bot - apps_panel_h(rows)) / 2; }
/* Most rows (3 down to 1) whose window clears the menu bar by 2px (the lowest a window may sit) and the dock by 8px. */
static inline int apps_rows_fit(int top, int bot){
    int r = 3;
    while (r > 1) {
        int py = apps_panel_y(top, bot, r);
        if (py - APPS_MARGIN - 32 >= top + 2 && py + apps_panel_h(r) + APPS_MARGIN + 8 <= bot - 8) break;
        r--;
    }
    return r;
}
/* vw: viewport width; vis: rows shown; py: panel top in the viewport. */
static inline struct apps_geom apps_geom_make(int vw, int vis, int py){
    struct apps_geom g;
    g.vis = vis; g.tile = APPS_TILE; g.cell_h = APPS_CELL_H;
    g.cell_w = APPS_CELL_W;
    if (vw < APPS_COLS * g.cell_w + 2 * APPS_PADX) { g.cell_w = (vw - 2 * APPS_PADX) / APPS_COLS; if (g.cell_w < APPS_TILE + 8) g.cell_w = APPS_TILE + 8; }
    g.pw = APPS_COLS * g.cell_w + 2 * APPS_PADX; g.ph = apps_panel_h(vis);
    g.px = (vw - g.pw) / 2; g.py = py;
    g.x0 = g.px + APPS_PADX; g.y0 = py + APPS_GRID_TOP;
    return g;
}
#endif
