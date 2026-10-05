/* samcaps.h: the captions over Samantha's full-screen picture (2.12.1), #included into user/samantha.c where the pieces they
 * draw on (the back buffer, her mouth geometry, wrap, the arena's caption array) are already defined. Header-only so the
 * program stays under the 2000 line ceiling (tools/checks/godfile-check.sh).
 *
 * The stack: up to CAP_SHOW lines from just above the glass input, newest at the bottom, each older one a step dimmer; a new line
 * eases in over CAP_IN ticks rising from the input; the whole stack fades together once the talk has been quiet for CAP_QUIET ticks
 * (cap_update). Her own tick clock drives all of it, and she prints samface: line=, fade= and gone= with the tick for the check. */

/* ---- captions ---- */
static int cap_rows(const char *s, int tw_, int maxrows) {
    int rows = wrap(s, tw_, 0, 0, 0, 0);
    return rows > maxrows ? maxrows : rows;
}
static int cap_alpha(const struct cap *c, unsigned now) {
    if (!c->live) return 0;
    int a = 255;
    unsigned age = now - c->born;
    if (age < CAP_IN) a = (int)(age * 255 / CAP_IN);
    if (c->ends) {
        int over = (int)(now - c->ends);
        if (over >= CAP_OUT) return 0;
        if (over > 0) { int o = 255 - over * 255 / CAP_OUT; if (o < a) a = o; }
    }
    return a;
}
static int stk_alpha(unsigned now) {   /* the whole stack going together once the conversation is quiet */
    if (!stk_end) return 255;
    int over = (int)(now - stk_end);
    if (over <= 0) return 255;
    return over >= CAP_OUT ? 0 : 255 - over * 255 / CAP_OUT;
}
static int caps_alive(void) { for (int k = 0; k < NCAP; k++) if (ar->cap[k].live) return 1; return 0; }
static int caps_prev JT_DATA = 0;
static void cap_update(unsigned now) {
    if (caps_alive() && face_talking(now)) stk_mark = now;   /* while she is still speaking the exchange is active */
    if (!stk_end && caps_alive() && (int)(now - stk_mark) >= CAP_QUIET) {   /* quiet for 4 s: everything goes together */
        stk_end = stk_mark + CAP_QUIET;
        char l[40] = "samface: fade="; int k = 14; put_num(l, &k, (int)stk_end); l[k++] = '\n'; jt_write(1, l, (unsigned)k);
    }
    if (stk_end && (int)(now - stk_end) >= CAP_OUT) {
        for (int k = 0; k < NCAP; k++) ar->cap[k].live = 0;
        stk_end = 0;
        char l[40] = "samface: gone="; int k = 14; put_num(l, &k, (int)now); l[k++] = '\n'; jt_write(1, l, (unsigned)k);
    }
    for (int k = 0; k < NCAP; k++) { struct cap *c = &ar->cap[k]; if (c->live && c->ends && (int)(now - c->ends) >= CAP_OUT) c->live = 0; }
    int w = 0;   /* keep the live ones together at the front, oldest first */
    for (int k = 0; k < NCAP; k++) if (ar->cap[k].live) { if (k != w) ar->cap[w] = ar->cap[k]; w++; }
    for (int k = w; k < NCAP; k++) ar->cap[k].live = 0;
    if (caps_alive() != caps_prev) { caps_prev = caps_alive(); jt_write(1, caps_prev ? "samface: captions=1\n" : "samface: captions=0\n", 21); }   /* the check waits on these */
}

/* One caption over the picture, bottom edge at ybot: a soft dark backdrop (rounded, with a feathered edge), white text,
   all composed at full strength and then mixed over what was there by the fade alpha. Returns the box's top. */
static int cap_compact JT_DATA = 0;   /* 1: the one-row slot under her lips: a 26 px box with a 4 px feather */
static int cap_draw(struct cap *c, int ybot, int ytop_limit, unsigned now, int fixed_top, int fixed_rows, int amax, int yoff) {
    int a = cap_alpha(c, now), W = back_w;
    if (a > amax) a = amax;   /* a caption giving way to a newer one in the same place */
    int tw_ = (W - 2 * VM_MARGIN - 40 > 600 ? 600 : W - 2 * VM_MARGIN - 40);
    int avail = fixed_top ? fixed_rows : (ybot - ytop_limit - 26) / LINE; if (avail < 1) avail = 1;
    int rows = cap_rows(c->text, tw_, avail > 7 ? 7 : avail);
    int bw = 0;
    { /* the widest row decides the box */
        int saved_sink = 0; (void)saved_sink;
        bw = rows > 1 ? tw_ : (tw(c->text) < tw_ ? tw(c->text) : tw_);
    }
    int bh = rows * LINE + (cap_compact ? 10 : 18), bx = (W - bw) / 2 - 16, by = fixed_top ? fixed_top : ybot - bh;
    int ret = by; by += yoff;   /* yoff: still rising into place */
    int FE = cap_compact ? 4 : 8;   /* feather */
    int rx = bx - FE, ry = by - FE, rw = bw + 32 + 2 * FE, rh = bh + 2 * FE;
    if (rx < 0) rx = 0; if (ry < 0) ry = 0;
    if (rx + rw > W) rw = W - rx; if (ry + rh > back_h) rh = back_h - ry;
    if (a <= 0 || rw <= 0 || rh <= 0 || rw * rh > vm_cap) return ret;
    for (int i = 0; i < rh; i++) for (int j = 0; j < rw; j++) vm_save[i * rw + j] = backbuf[(ry + i) * W + rx + j];
    /* backdrop: signed distance to the rounded rect (radius 16) mapped to alpha across the feather */
    int cx2 = bx + (bw + 32) / 2, cy2 = by + bh / 2, hw = (bw + 32) / 2 - 16, hh = bh / 2 - 16;
    if (hh < 0) hh = 0;
    for (int i = 0; i < rh; i++) for (int j = 0; j < rw; j++) {
        int px = rx + j, py = ry + i;
        int dx = px - cx2; if (dx < 0) dx = -dx; dx -= hw; if (dx < 0) dx = 0;
        int dy = py - cy2; if (dy < 0) dy = -dy; dy -= hh; if (dy < 0) dy = 0;
        int dist = isqrt_u((unsigned)(dx * dx + dy * dy)) - 16;   /* < 0 inside */
        if (dist >= FE) continue;
        unsigned al = dist <= -FE ? CAP_ALPHA : (unsigned)(CAP_ALPHA * (FE - dist) / (2 * FE));
        backbuf[py * W + px] = lerp8(backbuf[py * W + px], 0x00101012, al);
    }
    unsigned *real = win.pixels; win.pixels = backbuf;
    wrap_cap = rows; wrap_ell = fixed_top && cap_rows(c->text, tw_, 99) > rows;   /* the shared top slot says so when a reply goes on past its rows */
    int tx = (W - tw_) / 2;
    if (rows == 1 && bw < tw_) tx = (W - bw) / 2;
    wrap(c->text, tw_, tx, by + (cap_compact ? 5 : 9), c->mine ? 0x00E8D8BC : WHITE, 1);
    wrap_cap = wrap_ell = 0;
    win.pixels = real;
    for (int i = 0; i < rh; i++) for (int j = 0; j < rw; j++) {
        unsigned *p = &backbuf[(ry + i) * W + rx + j];
        *p = lerp8(vm_save[i * rw + j], *p, (unsigned)a);
    }
    return ret;
}


/* Draw the stack for this moment over the back buffer. */
static void caps_draw(unsigned now) {
    /* captions, bottom up and conversation aware: up to CAP_SHOW lines stacked from just above the glass input, the newest at the bottom,
       each older one a step dimmer and pushed up as a new line eases in (CAP_IN ticks, rising from the input). The stack stays while
       the exchange goes on and fades all together once it has been quiet for CAP_QUIET ticks. A wide window puts her mouth low
       (2.12.1): one compact row fits under her lips and no more, so only the newest shows and the one before it fades out as it comes
       in. Where not even one row fits (a short ordinary window) none is drawn; Tab still shows the conversation. Nothing is ever
       drawn on her lips or her eyes. */
    int ytop = vm_my + vm_ml1 + 8, ybot = vm_py - 12;   /* captions stay below her lower lip */
    int ncap = 0; while (ncap < NCAP && ar->cap[ncap].live) ncap++;
    if (ncap) {
        struct cap *nw = &ar->cap[ncap - 1];
        int t = (int)(now - nw->born); if (t > CAP_IN) t = CAP_IN;
        int u = 256 - t * 256 / CAP_IN, rem = u * u / 256;   /* 256 at the start, 0 at the end: ease-out */
        int sa = stk_alpha(now);
        { static int rise_y JT_DATA = -1; static unsigned rise_born JT_DATA = 0;   /* the newest line's place while it rises, on her tick clock, for the check */
          int y = ((ybot - ytop - 26) / LINE >= 2 ? 12 : 10) * rem / 256;
          if ((t < CAP_IN || rise_born == nw->born) && (y != rise_y || rise_born != nw->born)) {
              char l[64] = "samface: rise="; int k = 14;
              put_num(l, &k, (int)nw->born); l[k++] = ','; put_num(l, &k, t); l[k++] = ','; put_num(l, &k, y); l[k++] = '\n'; jt_write(1, l, (unsigned)k);
              rise_y = y; rise_born = nw->born;
          } }
        if ((ybot - ytop - 26) / LINE >= 2) {
            int shift = 0;
            for (int r = 0; r < ncap; r++) {
                struct cap *cp = &ar->cap[ncap - 1 - r];
                if ((ybot - 34 < ytop && r) || r >= CAP_SHOW) break;   /* three boxes at most; the one beyond leaves as the newest rises */
                int dim = r == 0 ? 255 : r == 1 ? 170 : 105;
                int top = cap_draw(cp, ybot, ytop, now, 0, 0, dim * sa / 255, r == 0 ? 12 * rem / 256 : shift * rem / 256);
                if (r == 0) shift = ybot - top + 10;
                if (cap_alpha(cp, now) > 0) ybot = top - 10;
            }
        } else {
            int boxtop = vm_py - 4 - (LINE + 10);   /* a compact row: 26 high, its feather 4 */
            if (boxtop - 4 > vm_my + vm_ml1) {
                cap_compact = 1;
                if (ncap > 1) cap_draw(&ar->cap[ncap - 2], ybot, ytop, now, boxtop, 1, (255 - cap_alpha(nw, now)) * sa / 255, 0);
                cap_draw(nw, ybot, ytop, now, boxtop, 1, sa, 10 * rem / 256);
                cap_compact = 0;
            }
        }
    }
}
