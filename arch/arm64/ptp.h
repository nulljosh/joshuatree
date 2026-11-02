/* 11.0.0, USB precision touchpads (the Windows "PTP" layout most USB and laptop-kit pads speak): the report descriptor
   holds a Digitizer Touch Pad application with one Finger collection per contact (tip switch, contact ID, X, Y), a
   contact count and button 1, and a Feature, Input Mode (0x0D:0x52), that switches the pad from plain mouse to fingers
   when set to 3. ptp_layout reads that once; ptp_report turns each report into Linux's multitouch slot events (the
   protocol trackpad.h reads), so a USB pad gets the same gestures as QEMU's multitouch. The includer supplies kinput.
   ponytail: short items only, and the fingers of one report ID; a pad that splits fingers over hybrid reports still
   works one report at a time, since each finger says its own tip. */
#define PTP_FINGERS 5
struct ptp {
    int rid, n, cc, cb, btn, mode_rid, has_mode;   /* report ID, fingers found, contact count offset and width, button offset, Input Mode's report ID */
    struct { int tip, id, idb, x, xb, y, yb; } f[PTP_FINGERS];
    unsigned xmax, ymax, active, btn_down;
};
static int ptp_layout(struct ptp *p, const unsigned char *d, unsigned n) {   /* 1 when it is a precision touchpad */
    unsigned page = 0, size = 0, count = 0, rid = 0, lmax = 0, nu = 0, u[16], app = 0, depth = 0, fdepth = 0;
    int bit = 0, fi = -1, tp = 0;
    for (int i = 0; i < PTP_FINGERS; i++) p->f[i].tip = p->f[i].id = p->f[i].x = p->f[i].y = -1;
    p->rid = 0; p->n = 0; p->cc = p->btn = -1; p->cb = 0; p->has_mode = 0; p->mode_rid = 0; p->xmax = p->ymax = 0; p->active = p->btn_down = 0;
    for (unsigned i = 0; i < n; ) {
        unsigned b = d[i];
        if (b == 0xFE) { i += 3 + (i + 1 < n ? d[i + 1] : 0); continue; }
        unsigned sz = b & 3; if (sz == 3) sz = 4;
        unsigned v = 0;
        for (unsigned k = 0; k < sz && i + 1 + k < n; k++) v |= (unsigned)d[i + 1 + k] << (8 * k);
        i += 1 + sz;
        switch (b & 0xFC) {
        case 0x04: page = v; break;
        case 0x74: size = v; break;
        case 0x94: count = v; break;
        case 0x84: rid = v; bit = 0; break;
        case 0x24: lmax = v; break;
        case 0x08: if (nu < 16) u[nu++] = sz == 4 ? v : page << 16 | v; break;
        case 0xA0:                                                   /* collection */
            depth++;
            if (depth == 1) app = nu ? u[0] : 0;
            if (app == (0x0Du << 16 | 0x05)) tp = 1;
            if (nu && u[0] == (0x0Du << 16 | 0x22) && app == (0x0Du << 16 | 0x05) && p->n < PTP_FINGERS) { fi = p->n++; fdepth = depth; }
            nu = 0; break;
        case 0xC0: if (fi >= 0 && depth == fdepth) fi = -1; if (depth) depth--; nu = 0; break;
        case 0xB0:                                                   /* feature: only Input Mode matters */
            for (unsigned c = 0; c < nu; c++) if (u[c] == (0x0Du << 16 | 0x52)) { p->has_mode = 1; p->mode_rid = (int)rid; }
            nu = 0; break;
        case 0x80:                                                   /* input */
            for (unsigned c = 0; c < count && c < 64; c++) {
                unsigned us = c < nu ? u[c] : nu ? u[nu - 1] : 0;
                if (!(v & 1) && app == (0x0Du << 16 | 0x05)) {
                    if (fi >= 0) {
                        if (us == (0x0Du << 16 | 0x42)) p->f[fi].tip = bit;
                        else if (us == (0x0Du << 16 | 0x51)) { p->f[fi].id = bit; p->f[fi].idb = (int)size; }
                        else if (us == (1u << 16 | 0x30)) { p->f[fi].x = bit; p->f[fi].xb = (int)size; p->xmax = lmax; p->rid = (int)rid; }
                        else if (us == (1u << 16 | 0x31)) { p->f[fi].y = bit; p->f[fi].yb = (int)size; p->ymax = lmax; }
                    } else if (us == (0x0Du << 16 | 0x54)) { p->cc = bit; p->cb = (int)size; }
                    else if (us == (9u << 16 | 1)) p->btn = bit;
                }
                bit += (int)size;
            }
            nu = 0; break;
        case 0x90: nu = 0; break;   /* output */
        }
    }
    if (!tp || !p->n || !p->xmax || !p->ymax) return 0;
    for (int i = 0; i < p->n; i++) if (p->f[i].tip < 0 || p->f[i].x < 0 || p->f[i].y < 0) return 0;
    return 1;
}
static unsigned ptp_bits(const unsigned char *r, unsigned len, int off, int n) {
    unsigned v = 0;
    for (int i = 0; i < n && i < 32; i++) { unsigned b = (unsigned)(off + i); if (b / 8 < len && (r[b / 8] >> (b % 8)) & 1) v |= 1u << i; }
    return v;
}
static void ptp_report(struct ptp *p, const unsigned char *r, unsigned len) {
    if (p->rid) { if (!len || r[0] != p->rid) return; r++; len--; }
    unsigned cnt = p->cc >= 0 ? ptp_bits(r, len, p->cc, p->cb) : 0, seen = 0;
    int nf = cnt && (int)cnt < p->n ? (int)cnt : p->n;
    for (int i = 0; i < nf; i++) {
        unsigned id = p->f[i].id >= 0 ? ptp_bits(r, len, p->f[i].id, p->f[i].idb) : (unsigned)i, s = id % PTP_FINGERS;
        kinput(3, 0x2f, (int)s);
        if (ptp_bits(r, len, p->f[i].tip, 1)) {
            unsigned x = ptp_bits(r, len, p->f[i].x, p->f[i].xb), y = ptp_bits(r, len, p->f[i].y, p->f[i].yb);
            if (x > p->xmax) x = p->xmax;
            if (y > p->ymax) y = p->ymax;
            if (!(p->active >> s & 1)) kinput(3, 0x39, (int)id);
            kinput(3, 0x35, (int)(x * 32767u / p->xmax)); kinput(3, 0x36, (int)(y * 32767u / p->ymax));
            p->active |= 1u << s; seen |= 1u << s;
        } else if (p->active >> s & 1) { kinput(3, 0x39, -1); p->active &= ~(1u << s); }
    }
    if (cnt) for (unsigned s = 0; s < PTP_FINGERS; s++) if ((p->active & ~seen) >> s & 1) { kinput(3, 0x2f, (int)s); kinput(3, 0x39, -1); p->active &= ~(1u << s); }   /* a finger the count no longer covers lifted */
    if (p->btn >= 0) { unsigned b = ptp_bits(r, len, p->btn, 1); if (b != p->btn_down) { kinput(1, 272, (int)b); p->btn_down = b; } }   /* the pad's own click */
    kinput(0, 0, 0);
}
