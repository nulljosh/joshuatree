#ifndef CLOCKICON_H
#define CLOCKICON_H
/* The Clock dock/Apps tile as a live analog face. Included by kernel.c, after
   gui_aa_line_phys and cmos_read_time_stable, before gui_draw_one_icon_on. */

/* Clock icon: the tile itself is plain (cached, no glyph). The whole face,
   ticks and hands are laid on live in physical pixels by
   gui_clock_draw_hands at every site that draws the icon, so every edge is
   exact coverage AA (the supersampled glyph path blurred small dots) and the
   hands follow the time. No text anywhere. */
static const short GUI_SIN6[17] = {0,105,208,309,407,500,588,669,743,809,866,914,951,978,995,1000,1000};
/* sin * 1000 for an angle in half degrees (0..719 is one turn) */
static int gui_sin_hd(int u){
    u %= 720; if (u < 0) u += 720;
    int q = u / 180, r = u % 180;
    if (q & 1) r = 180 - r;
    int i = r / 12, f = r % 12;
    int v = GUI_SIN6[i] + (GUI_SIN6[i + 1] - GUI_SIN6[i]) * f / 12;
    return q >= 2 ? -v : v;
}
static void gui_icon_clock(int cx, int cy, int s, unsigned int bg){ (void)cx; (void)cy; (void)s; (void)bg; }
static void gui_clock_draw_hands(int cx_center, int cy_bottom, int size){
    u8 h, m, wd, dom, mon;
    cmos_read_time_stable(&h, &m, &wd, &dom, &mon);
    int hv = ((h & 0x0F) + ((h >> 4) * 10)) % 12, mv = ((m & 0x0F) + ((m >> 4) * 10)) % 60;
    int sc = (int)window_scale(); if (sc < 1) sc = 1;
    int pw = size * sc;
    double cxp = (cx_center - size / 2) * sc + pw / 2.0, cyp = (cy_bottom - size) * sc + pw / 2.0;
    double R = pw * 38 / 100.0;
    int uh = hv * 60 + mv / 2; /* half degrees: 60 per hour, plus 1 per two minutes */
    int um = mv * 12;
    unsigned int ink = 0x001F1F22, tick = 0x006A6E86;
    gui_aa_line_phys(cxp, cyp, cxp, cyp, 0x00B8BBCB, R * 2.0);   /* rim */
    gui_aa_line_phys(cxp, cyp, cxp, cyp, 0x00FFFFFF, R * 2.0 - pw * 0.035);
    for (int t = 0; t < 12; t++) {
        int u = t * 60, big = (t % 3) == 0;
        double r0 = R * (big ? 0.72 : 0.80), r1 = R * 0.90;
        double sx = gui_sin_hd(u) / 1000.0, sy = -gui_sin_hd(u + 180) / 1000.0;
        gui_aa_line_phys(cxp + sx * r0, cyp + sy * r0, cxp + sx * r1, cyp + sy * r1, big ? ink : tick, pw * (big ? 0.03 : 0.02));
    }
    double hl = R * 0.50, ml = R * 0.72;
    gui_aa_line_phys(cxp, cyp, cxp + gui_sin_hd(uh) * hl / 1000.0, cyp - gui_sin_hd(uh + 180) * hl / 1000.0, ink, pw * 0.055);
    gui_aa_line_phys(cxp, cyp, cxp + gui_sin_hd(um) * ml / 1000.0, cyp - gui_sin_hd(um + 180) * ml / 1000.0, ink, pw * 0.04);
    gui_aa_line_phys(cxp, cyp, cxp, cyp, 0x00FF3B30, pw * 0.07);
}

/* 1 when the minute changed since *seen (the first call only seeds it). */
static int gui_clock_tick(int *seen){
    u8 h, m, wd, dom, mon;
    cmos_read_time_stable(&h, &m, &wd, &dom, &mon);
    int cm = (m & 0x0F) + ((m >> 4) * 10), changed = *seen >= 0 && cm != *seen;
    *seen = cm;
    return changed;
}
/* Live overlays drawn over a cached tile: Calendar's date, Clock's hands. */
static void gui_icon_overlay(int icon, int cx_center, int cy_bottom, int size){
    if (icon == 2) gui_calendar_draw_date(cx_center, cy_bottom, size);
    else if (icon == 23) gui_clock_draw_hands(cx_center, cy_bottom, size);
}

#endif
