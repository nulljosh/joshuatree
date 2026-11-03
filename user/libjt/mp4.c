/* MP4 reader: see mp4.h. Boxes are ISO/IEC 14496-12's; the AAC descriptor chain is 14496-1's esds. */
#include "mp4.h"

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }

/* The child box of type `t` inside [p, end): its payload and length, or 0. A box that runs past end is a format error. */
static const uint8_t *child(const uint8_t *p, const uint8_t *end, const char *t, uint32_t *len, int *bad) {
    while (end - p >= 8) {
        uint32_t n = be32(p), h = 8;
        if (n == 1) return *bad = 1, (const uint8_t *)0;   /* 64-bit sizes: never in a moov of this size */
        if (n == 0) n = (uint32_t)(end - p);
        if (n < h || n > (uint32_t)(end - p)) return *bad = 1, (const uint8_t *)0;
        if (p[4] == t[0] && p[5] == t[1] && p[6] == t[2] && p[7] == t[3]) { *len = n - h; return p + h; }
        p += n;
    }
    return 0;
}
static const uint8_t *path(const uint8_t *p, uint32_t n, const char *const *ts, uint32_t *len, int *bad) {
    for (; *ts && p; ts++) p = child(p, p + n, *ts, &n, bad);
    *len = n;
    return p;
}

static uint32_t desc_len(const uint8_t **p, const uint8_t *end) {   /* an esds descriptor's size: up to four 7-bit bytes */
    uint32_t n = 0;
    for (int i = 0; i < 4 && *p < end; i++) { uint8_t b = *(*p)++; n = n << 7 | (b & 0x7F); if (!(b & 0x80)) break; }
    return n;
}
static int esds_asc(const uint8_t *e, uint32_t n, struct mp4_track *t) {   /* the AudioSpecificConfig in an esds */
    const uint8_t *p = e + 4, *end = e + n;   /* past version and flags */
    if (p >= end || *p++ != 0x03) return 0;
    desc_len(&p, end);
    if (end - p < 3) return 0;
    uint8_t fl = p[2]; p += 3;
    if (fl & 0x80) p += 2;
    if (fl & 0x40) { if (p >= end) return 0; p += 1 + *p; }
    if (fl & 0x20) p += 2;
    if (p >= end || *p++ != 0x04) return 0;
    desc_len(&p, end);
    if (end - p < 13 || p[0] != 0x40) return 0;   /* 0x40: MPEG-4 audio */
    p += 13;
    if (p >= end || *p++ != 0x05) return 0;
    uint32_t l = desc_len(&p, end);
    if (l == 0 || l > (uint32_t)(end - p)) return 0;
    t->cfg = p; t->cfglen = l;
    return 1;
}

static int full(const uint8_t *b, uint32_t n, const uint8_t **out, uint32_t *len) {   /* past a full box's version and flags */
    if (!b || n < 4) return 0;
    *out = b + 4; *len = n - 4;
    return 1;
}

static void trak(const uint8_t *tk, uint32_t tn, struct mp4 *m, int *bad) {
    static const char *const mdhd_p[] = {"mdia", "mdhd", 0}, *const hdlr_p[] = {"mdia", "hdlr", 0}, *const stbl_p[] = {"mdia", "minf", "stbl", 0};
    uint32_t n, sn;
    const uint8_t *h = path(tk, tn, hdlr_p, &n, bad), *md = path(tk, tn, mdhd_p, &sn, bad);
    if (!h || n < 12 || !md || sn < 24) return;
    int vide = !__builtin_memcmp(h + 8, "vide", 4), soun = !__builtin_memcmp(h + 8, "soun", 4);
    struct mp4_track *t = vide ? &m->video : soun ? &m->audio : 0;
    if (!t || t->kind) return;   /* the first of each kind */
    uint32_t ts = md[0] == 1 ? (sn >= 32 ? be32(md + 20) : 0) : be32(md + 12);
    const uint8_t *st = path(tk, tn, stbl_p, &sn, bad);
    if (!st || !ts) return;
    struct mp4_track c = {0};
    c.timescale = ts;
    uint32_t l; const uint8_t *b;
    if (!full(child(st, st + sn, "stsz", &l, bad), l, &c.stsz, &c.stsz_len) || c.stsz_len < 8) return;
    if (!full(child(st, st + sn, "stsc", &l, bad), l, &c.stsc, &c.stsc_len) || c.stsc_len < 4) return;
    if (!full(child(st, st + sn, "stts", &l, bad), l, &c.stts, &c.stts_len) || c.stts_len < 4) return;
    if ((b = child(st, st + sn, "stco", &l, bad))) full(b, l, &c.stco, &c.stco_len);
    else if ((b = child(st, st + sn, "co64", &l, bad))) { full(b, l, &c.stco, &c.stco_len); c.co64 = 1; }
    if (!c.stco || c.stco_len < 4) return;
    c.nsamples = be32(c.stsz + 4);
    if (!be32(c.stsz) && c.stsz_len < 8 + 4ull * c.nsamples) return;
    if (c.stsc_len < 4 + 12ull * be32(c.stsc) || c.stts_len < 4 + 8ull * be32(c.stts) ||
        c.stco_len < 4 + (c.co64 ? 8ull : 4ull) * be32(c.stco)) return;
    const uint8_t *sd; uint32_t sdn;
    if (!full(child(st, st + sn, "stsd", &l, bad), l, &sd, &sdn) || sdn < 4 + 8) return;
    const uint8_t *e = sd + 4; uint32_t en = be32(e);   /* the first sample entry */
    if (en < 8 || en > sdn - 4) return;
    if (vide) {
        if (__builtin_memcmp(e + 4, "avc1", 4) || en < 8 + 78) return;
        c.width = be16(e + 8 + 24); c.height = be16(e + 8 + 26);
        const uint8_t *a = child(e + 8 + 78, e + en, "avcC", &l, bad);
        if (!a || l < 7) return;
        c.cfg = a; c.cfglen = l;
        c.kind = MP4_VIDEO;
    } else {
        if (__builtin_memcmp(e + 4, "mp4a", 4) || en < 8 + 28) return;
        c.channels = be16(e + 8 + 16); c.rate = be32(e + 8 + 24) >> 16;
        const uint8_t *es = child(e + 8 + 28, e + en, "esds", &l, bad);
        if (!es || !esds_asc(es, l, &c)) return;
        c.kind = MP4_AUDIO;
    }
    *t = c;
}

int mp4_open(struct mp4 *m, const uint8_t *buf, uint32_t len) {
    *m = (struct mp4){0};
    int bad = 0; uint32_t n;
    const uint8_t *mv = child(buf, buf + len, "moov", &n, &bad);
    if (!mv || bad) return MP4_E_FORMAT;
    const uint8_t *p = mv, *end = mv + n;
    while (end - p >= 8) {   /* every trak */
        uint32_t tn; const uint8_t *tk = child(p, end, "trak", &tn, &bad);
        if (!tk) break;
        trak(tk, tn, m, &bad);
        p = tk + tn;
    }
    if (bad) return MP4_E_FORMAT;
    return m->video.kind ? MP4_OK : MP4_E_EMPTY;
}

void mp4_iter_init(struct mp4_iter *it, const struct mp4_track *t) { *it = (struct mp4_iter){0}; it->t = t; it->chunk = (uint32_t)-1; }

int mp4_iter_next(struct mp4_iter *it, uint64_t *off, uint32_t *size, uint64_t *dts) {
    const struct mp4_track *t = it->t;
    if (!t || !t->kind || it->i >= t->nsamples) return 0;
    uint32_t fixed = be32(t->stsz), sz = fixed ? fixed : be32(t->stsz + 8 + 4 * it->i);
    if (!it->left_in_chunk) {   /* the next chunk: stsc says how many samples it holds */
        it->chunk++;
        if (it->chunk >= be32(t->stco)) return 0;
        uint32_t ne = be32(t->stsc);
        while (it->stsc_i + 1 < ne && be32(t->stsc + 4 + 12 * (it->stsc_i + 1)) <= it->chunk + 1) it->stsc_i++;
        if (!ne) return 0;
        it->left_in_chunk = be32(t->stsc + 4 + 12 * it->stsc_i + 4);
        if (!it->left_in_chunk) return 0;
        it->off = t->co64 ? (uint64_t)be32(t->stco + 4 + 8 * it->chunk) << 32 | be32(t->stco + 8 + 8 * it->chunk) : be32(t->stco + 4 + 4 * it->chunk);
    }
    while (!it->stts_left) {   /* the decode time: stts runs of equal deltas */
        if (it->stts_i >= be32(t->stts)) return 0;
        it->stts_left = be32(t->stts + 4 + 8 * it->stts_i); it->stts_delta = be32(t->stts + 8 + 8 * it->stts_i); it->stts_i++;
    }
    *off = it->off; *size = sz; *dts = it->dts;
    it->off += sz; it->dts += it->stts_delta; it->stts_left--; it->left_in_chunk--; it->i++;
    return 1;
}
