#include "wav.h"

static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint32_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }

int wav_open(struct wav *w, const uint8_t *buf, uint32_t len) {
    if (len < 12 || buf[0]!='R' || buf[1]!='I' || buf[2]!='F' || buf[3]!='F' ||
        buf[8]!='W' || buf[9]!='A' || buf[10]!='V' || buf[11]!='E') return WAV_E_FORMAT;
    int have_fmt = 0, have_data = 0;
    uint32_t pos = 12;
    w->pcm = 0; w->frames = 0;
    while (len - pos >= 8) {                       /* pos <= len always, no overflow */
        uint32_t size = le32(buf + pos + 4), body = pos + 8, room = len - body;
        const uint8_t *id = buf + pos;
        if (id[0]=='f' && id[1]=='m' && id[2]=='t' && id[3]==' ') {
            if (size < 16 || room < 16) return WAV_E_FORMAT;
            uint32_t tag = le16(buf + body);
            w->channels = le16(buf + body + 2);
            w->rate     = le32(buf + body + 4);
            w->bits     = le16(buf + body + 14);
            if (tag != 1 && tag != 0xFFFE) return WAV_E_UNSUP;   /* 0xFFFE: extensible, PCM assumed */
            if ((w->bits != 8 && w->bits != 16) || w->channels < 1 || w->channels > 2 ||
                w->rate < 4000 || w->rate > 44100) return WAV_E_UNSUP;
            have_fmt = 1;
        } else if (id[0]=='d' && id[1]=='a' && id[2]=='t' && id[3]=='a') {
            if (!have_fmt) return WAV_E_FORMAT;       /* data before fmt: refuse rather than guess */
            if (size > room) size = room;             /* streamed files lie about size; clamp */
            w->pcm = buf + body;
            w->frames = size / (w->channels * (w->bits / 8u));
            have_data = 1;
            break;
        }
        if (size > room) return WAV_E_FORMAT;
        pos = body + size + (size & 1);               /* chunks are word aligned */
        if (pos > len) break;
    }
    return (have_fmt && have_data && w->frames) ? WAV_OK : WAV_E_EMPTY;
}

uint32_t wav_read8(const struct wav *w, uint32_t from, uint8_t *out, uint32_t n) {
    if (from >= w->frames) return 0;
    if (n > w->frames - from) n = w->frames - from;
    uint32_t step = w->channels * (w->bits / 8u);
    const uint8_t *p = w->pcm + (uint64_t)from * step;
    for (uint32_t i = 0; i < n; i++, p += step) {
        int v;
        if (w->bits == 8) v = w->channels == 2 ? (p[0] + p[1]) / 2 : p[0];
        else {
            int l = (int16_t)le16(p), r = w->channels == 2 ? (int16_t)le16(p + 2) : l;
            v = ((l + r) / 2 >> 8) + 128;             /* 16-bit signed -> 8-bit unsigned */
        }
        out[i] = (uint8_t)v;
    }
    return n;
}
