#include "avi.h"

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static int is(const uint8_t *p, const char *s) { return p[0]==(uint8_t)s[0] && p[1]==(uint8_t)s[1] && p[2]==(uint8_t)s[2] && p[3]==(uint8_t)s[3]; }

/* Walks the chunks in [pos, end) inside buf. Returns 1 and fills id/body/size (size clamped to what
   is really there), 0 when none are left. */
static int chunk_at(const uint8_t *buf, uint32_t pos, uint32_t end, const uint8_t **id, uint32_t *body, uint32_t *size) {
    if (end < pos || end - pos < 8) return 0;
    *id = buf + pos; *body = pos + 8;
    uint32_t s = le32(buf + pos + 4), room = end - *body;
    *size = s > room ? room : s;
    return 1;
}
static uint32_t next_pos(uint32_t body, uint32_t size) { return body + size + (size & 1); }

static int parse_strl(struct avi *a, uint32_t pos, uint32_t end, uint32_t index, int *have_v) {
    const uint8_t *id; uint32_t body, size, strh = 0, strf = 0, strf_len = 0;
    while (chunk_at(a->buf, pos, end, &id, &body, &size)) {
        if (is(id, "strh") && size >= 20) strh = body;
        else if (is(id, "strf")) { strf = body; strf_len = size; }
        pos = next_pos(body, size);
    }
    if (!strh || !strf) return AVI_OK;                       /* an odd stream we do not play: skip it */
    const uint8_t *h = a->buf + strh, *f = a->buf + strf;
    if (is(h, "vids")) {
        if (*have_v) return AVI_OK;
        if (!(is(h + 4, "MJPG") || is(h + 4, "mjpg")) || strf_len < 20) return AVI_E_UNSUP;
        a->width = le32(f + 4); a->height = le32(f + 8);
        if (!a->width || !a->height || a->width > 4096 || a->height > 4096) return AVI_E_UNSUP;
        a->vstream = (uint8_t)index; *have_v = 1;
    } else if (is(h, "auds") && !a->arate) {
        if (strf_len < 16) return AVI_E_UNSUP;
        uint32_t tag = le16(f), ch = le16(f + 2), rate = le32(f + 4), bits = le16(f + 14);
        if (tag != 1 || (bits != 8 && bits != 16) || ch < 1 || ch > 2 || rate < 4000 || rate > 44100) return AVI_E_UNSUP;
        a->achannels = (uint16_t)ch; a->abits = (uint16_t)bits; a->arate = rate; a->astream = (uint8_t)index;
    }
    return AVI_OK;
}

int avi_open(struct avi *a, const uint8_t *buf, uint32_t len) {
    if (len < 12 || !is(buf, "RIFF") || !is(buf + 8, "AVI ")) return AVI_E_FORMAT;
    a->buf = buf; a->len = len; a->movi = a->movi_end = 0;
    a->width = a->height = a->us_per_frame = a->arate = 0; a->achannels = a->abits = 0;
    int have_v = 0; uint32_t index = 0, pos = 12;
    const uint8_t *id; uint32_t body, size;
    while (chunk_at(buf, pos, len, &id, &body, &size)) {
        if (is(id, "LIST") && size >= 4) {
            if (is(buf + body, "hdrl")) {
                uint32_t p = body + 4, end = body + size;
                const uint8_t *id2; uint32_t b2, s2;
                while (chunk_at(buf, p, end, &id2, &b2, &s2)) {
                    if (is(id2, "avih") && s2 >= 4) a->us_per_frame = le32(buf + b2);
                    else if (is(id2, "LIST") && s2 >= 4 && is(buf + b2, "strl")) {
                        int r = parse_strl(a, b2 + 4, b2 + s2, index++, &have_v);
                        if (r) return r;
                    }
                    p = next_pos(b2, s2);
                }
            } else if (is(buf + body, "movi") && !a->movi) {
                a->movi = body + 4; a->movi_end = body + size;
            }
        }
        pos = next_pos(body, size);
    }
    if (!have_v || !a->movi) return AVI_E_EMPTY;
    if (!a->us_per_frame || a->us_per_frame > 1000000) a->us_per_frame = 100000;   /* missing or silly: 10 fps */
    a->pos = a->movi;
    return AVI_OK;
}

void avi_rewind(struct avi *a) { a->pos = a->movi; }

int avi_next(struct avi *a, struct avi_chunk *c) {
    const uint8_t *id; uint32_t body, size;
    for (;;) {
        if (!chunk_at(a->buf, a->pos, a->movi_end, &id, &body, &size)) return 0;
        uint32_t raw = le32(id + 4);
        if (is(id, "LIST")) { a->pos = body + 4; continue; }          /* 'rec ' groups: step inside */
        if (raw > a->movi_end - body) { a->pos = a->movi_end; return 0; }   /* cut-off chunk: that is the end */
        a->pos = next_pos(body, size);
        if (id[0] >= '0' && id[0] <= '9' && id[1] >= '0' && id[1] <= '9') {
            uint8_t st = (uint8_t)((id[0] - '0') * 10 + (id[1] - '0'));
            if (st == a->vstream && id[2] == 'd' && (id[3] == 'c' || id[3] == 'b')) { c->kind = AVI_VIDEO; }
            else if (a->arate && st == a->astream && id[2] == 'w' && id[3] == 'b') { c->kind = AVI_AUDIO; }
            else continue;
            c->data = a->buf + body; c->len = size;
            return 1;
        }
    }
}

uint32_t avi_audio8(const struct avi *a, const uint8_t *in, uint32_t inlen, uint8_t *out, uint32_t outmax) {
    uint32_t step = a->achannels * (a->abits / 8u), n = step ? inlen / step : 0;
    if (n > outmax) n = outmax;
    for (uint32_t i = 0; i < n; i++, in += step) {
        int v;
        if (a->abits == 8) v = a->achannels == 2 ? (in[0] + in[1]) / 2 : in[0];
        else {
            int l = (int16_t)le16(in), r = a->achannels == 2 ? (int16_t)le16(in + 2) : l;
            v = ((l + r) / 2 >> 8) + 128;
        }
        out[i] = (uint8_t)v;
    }
    return n;
}
