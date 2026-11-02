/* 10.2.0, the trackpad: fingers in, a mouse out. A multitouch device (QEMU's virtio-multitouch, Linux evdev's slot
   protocol: ABS_MT_SLOT, ABS_MT_TRACKING_ID, ABS_MT_POSITION_X and _Y, 0 to 32767) reports where each finger is; at
   each EV_SYN this turns the frame into what the rest of the kernel already reads from a mouse:
     one finger moving     the pointer moves by the same share of the screen (EV_REL x and y)
     two fingers moving    a scroll, natural as on a Mac: fingers down, the page comes down (REL_WHEEL, one notch per TP_NOTCH)
     a quick tap           a left click (BTN_LEFT down and up); two fingers tapped, a right click (BTN_RIGHT)
   A tap is a touch shorter than TP_TAP_MS that moved under TP_SLOP. The includer supplies tp_emit (boot.c: kinput) and
   the screen size. ponytail: no momentum, pinch or three-finger swipes; add them when a real pad is on the board. */
#define TP_SLOTS 5
#define TP_TAP_MS 250
#define TP_SLOP 400      /* of 32768: about 1 percent of the pad */
#define TP_NOTCH 1200    /* of 32768: two fingers this far is one wheel notch */
static struct { int on, need, x, y; } tp_f[TP_SLOTS];   /* need: 1 x, 2 y still to come for a finger that just landed */
static int tp_slot, tp_dirty, tp_prev_n, tp_px, tp_py, tp_max, tp_moved, tp_rx, tp_ry, tp_wheel;
static unsigned tp_t0;
static void tp_centroid(int *n, int *x, int *y) {
    int c = 0, sx = 0, sy = 0;
    for (int i = 0; i < TP_SLOTS; i++) if (tp_f[i].on && !tp_f[i].need) { c++; sx += tp_f[i].x; sy += tp_f[i].y; }
    *n = c; *x = c ? sx / c : 0; *y = c ? sy / c : 0;
}
static void tp_frame(unsigned now, unsigned w, unsigned h) {
    int n, x, y, rel = 0;
    tp_centroid(&n, &x, &y);
    if (n && !tp_prev_n) { tp_t0 = now; tp_max = 0; tp_moved = 0; tp_rx = tp_ry = tp_wheel = 0; }
    if (n > tp_max) tp_max = n;
    if (n && n == tp_prev_n) {   /* the same fingers moved: a finger lifting or landing shifts the centroid, so that frame is skipped */
        int dx = x - tp_px, dy = y - tp_py;
        tp_moved += (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
        if (n == 1) {   /* a share of the pad is the same share of the screen; the remainder carries to the next frame */
            tp_rx += dx * (int)w; tp_ry += dy * (int)h;
            int mx = tp_rx / 32768, my = tp_ry / 32768;
            tp_rx -= mx * 32768; tp_ry -= my * 32768;
            if (mx) { tp_emit(2, 0, mx); rel = 1; }
            if (my) { tp_emit(2, 1, my); rel = 1; }
        } else if (n == 2) {
            tp_wheel += dy;
            int notches = tp_wheel / TP_NOTCH;
            if (notches) { tp_wheel -= notches * TP_NOTCH; tp_emit(2, 8, notches); rel = 1; }
        }
    }
    if (!n && tp_prev_n && now - tp_t0 < TP_TAP_MS && tp_moved < TP_SLOP && tp_max <= 2) {
        unsigned b = tp_max == 2 ? 273 : 272;
        tp_emit(1, b, 1); tp_emit(0, 0, 0); tp_emit(1, b, 0); rel = 1;
    }
    tp_prev_n = n; tp_px = x; tp_py = y;
    if (rel) tp_emit(0, 0, 0);
}
/* 1 when the event was the trackpad's (the caller drops it); its EV_SYN runs the frame */
static int tp_event(unsigned type, unsigned code, int value, unsigned now, unsigned w, unsigned h) {
    if (type == 3 && code == 0x2f) { tp_slot = value >= 0 && value < TP_SLOTS ? value : 0; return 1; }   /* ABS_MT_SLOT */
    if (type == 3 && code == 0x39) {                                                                      /* ABS_MT_TRACKING_ID, -1 lifts */
        tp_f[tp_slot].on = value >= 0; tp_f[tp_slot].need = 3;   /* a landed finger counts once its x and y are both in: QEMU sends them in separate frames */
        tp_dirty = 1; return 1;
    }
    if (type == 3 && (code == 0x35 || code == 0x36)) {                                                    /* ABS_MT_POSITION_X, _Y */
        if (code == 0x35) { tp_f[tp_slot].x = value; tp_f[tp_slot].need &= ~1; } else { tp_f[tp_slot].y = value; tp_f[tp_slot].need &= ~2; }
        tp_dirty = 1; return 1;
    }
    if (type == 1 && code >= 320 && code <= 335) return 1;   /* BTN_TOUCH and the BTN_TOOL_* finger counts: the slots say it */
    if (type == 0 && tp_dirty) { tp_dirty = 0; tp_frame(now, w, h); return 1; }
    return 0;
}
