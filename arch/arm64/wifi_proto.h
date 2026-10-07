/* Pure-logic half of the Pi 4 Wi-Fi driver (CYW43455 over SDIO): SDPCM/BCDC frame packing and parsing, the escan
   result walk and the NVRAM "key=value\0" packing. No hardware here, so tools/checks/wifi-host-check.sh compiles it
   with the host clang and feeds it frames copied from Linux brcmfmac traces. Layout per brcmfmac's sdio.c/bcdc.c. */
#ifndef WIFI_PROTO_H
#define WIFI_PROTO_H
#define SDPCM_HDRLEN 12
#define BCDC_HDRLEN 4
#define SDPCM_CONTROL 0
#define SDPCM_EVENT 1
#define SDPCM_DATA 2
#define BCDC_GET_VAR 262
#define BCDC_SET_VAR 263
#define WLC_E_ESCAN_RESULT 69
static inline unsigned rd16(const unsigned char *p) { return p[0] | p[1] << 8; }
static inline unsigned rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }
static inline void wr16(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; }
static inline void wr32(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
/* SDPCM header: len, ~len, seq, channel|flags, next-len, data offset, flow control, credit, 2 reserved; then the BCDC
   header: cmd, len, flags(id<<16 | set<<1), status, then the payload. Returns the whole frame length. */
static inline unsigned sdpcm_pack(unsigned char *f, unsigned seq, unsigned chan, unsigned cmd, unsigned reqid, int set,
                                  const void *payload, unsigned len) {
    unsigned n = SDPCM_HDRLEN + BCDC_HDRLEN * 4 + len;
    wr16(f, n); wr16(f + 2, ~n & 0xffff); f[4] = seq; f[5] = chan; f[6] = 0; f[7] = SDPCM_HDRLEN;
    f[8] = 0; f[9] = 0; f[10] = 0; f[11] = 0;
    unsigned char *b = f + SDPCM_HDRLEN;
    wr32(b, cmd); wr32(b + 4, len); wr32(b + 8, reqid << 16 | (set ? 2 : 0)); wr32(b + 12, 0);
    const unsigned char *s = payload;
    for (unsigned i = 0; i < len; i++) b[16 + i] = s[i];
    return n;
}
/* Checks a received frame; returns the channel, or -1 when the length or its complement is wrong. */
static inline int sdpcm_parse(const unsigned char *f, unsigned got, unsigned *dataoff, unsigned *len) {
    unsigned n = rd16(f), c = rd16(f + 2);
    if (got < SDPCM_HDRLEN || (n ^ c) != 0xffff || n > got || f[7] < SDPCM_HDRLEN || f[7] > n) return -1;
    *dataoff = f[7]; *len = n - f[7];
    return f[5] & 15;
}
/* A control reply: the BCDC status (0 = ok) and where the payload starts; -1 when the request id does not match. */
static inline int bcdc_reply(const unsigned char *b, unsigned len, unsigned reqid, unsigned *off) {
    if (len < 16 || (rd32(b + 8) >> 16) != reqid) return -1;
    *off = 16;
    return (int)rd32(b + 12);
}
/* An escan result event payload, after the 48-byte brcmf_event header: wl_escan_result (buflen, version, sync_id,
   bss_count) then wl_bss_info entries. The callback gets rssi, channel, ssid; returns the count. */
static inline unsigned escan_walk(const unsigned char *e, unsigned len,
                                  void (*cb)(int rssi, unsigned chan, const char *ssid, unsigned slen)) {
    if (len < 12) return 0;
    unsigned count = rd16(e + 10), p = 12, n = 0;
    for (unsigned i = 0; i < count && p + 32 <= len; i++) {
        unsigned bl = rd32(e + p); if (bl < 32 || p + bl > len) break;
        const unsigned char *b = e + p;
        unsigned slen = b[18] > 32 ? 32 : b[18];
        int rssi = (short)rd16(b + 78);
        unsigned chan = rd16(b + 72) & 0xff;          /* chanspec: low byte is the channel on the 43455 */
        cb(rssi, chan, (const char *)b + 19, slen);
        p += bl; n++;
    }
    return n;
}
/* NVRAM text to the chip's image: one "key=value\0" per non-comment line, padded to 4 bytes, then a 4-byte trailer
   (words = len/4 in the low 16 bits, its complement above). Returns the image length, or 0 when `out` is too small. */
static inline unsigned nvram_pack(const char *txt, unsigned tlen, unsigned char *out, unsigned cap) {
    unsigned o = 0, i = 0;
    while (i < tlen) {
        unsigned s = i; while (i < tlen && txt[i] != '\n' && txt[i] != '\r') i++;
        unsigned e = i; while (i < tlen && (txt[i] == '\n' || txt[i] == '\r')) i++;
        while (s < e && (txt[s] == ' ' || txt[s] == '\t')) s++;
        if (s == e || txt[s] == '#') continue;
        if (o + (e - s) + 1 > cap) return 0;
        for (unsigned k = s; k < e; k++) out[o++] = txt[k];
        out[o++] = 0;
    }
    while (o & 3) { if (o >= cap) return 0; out[o++] = 0; }
    if (o + 4 > cap) return 0;
    unsigned w = o / 4; wr32(out + o, (w & 0xffff) | (~w & 0xffff) << 16);
    return o + 4;
}
/* Firmware goes to the chip in 64-byte backplane windows; the image is padded up to a 4-byte multiple. */
static inline unsigned fw_padded(unsigned n) { return (n + 3) & ~3u; }
#endif
