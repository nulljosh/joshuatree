/* 13.5.0, browser.c's own video: an .mp4 address plays in the page with no other computer. The file comes into vbuf (as
   much as fits: a longer one plays until it runs out), user/libjt's mp4.c finds the samples, h264.c decodes the pictures
   into image slot 0 and aac.c the sound into audio.c's app stream, which is the clock: the picture shown is the latest one
   due at the samples played, every picture is decoded (later ones need it) but only the due one is drawn. The decoders'
   memory is a 3 MiB pool taken once beside vbuf, so a page's heap release never frees it. Space pauses, another page
   stops it. Log lines: "browser: mp4 playing WxH, N pictures, sound R Hz", "browser: mp4 frame K", "browser: mp4 done". */
#include "../../user/libjt/mp4.h"
#include "../../user/libjt/h264.h"
#include "../../user/libjt/aac.h"
#define MP4_POOL (3u << 20)
static unsigned char *mpool; static unsigned mpool_used;
static void *mp_alloc(unsigned long n) {
    n = (n + 15) & ~15UL;
    if (!mpool || mpool_used + n > MP4_POOL) return 0;
    void *p = mpool + mpool_used; mpool_used += (unsigned)n;
    for (unsigned long i = 0; i < n; i++) ((unsigned char *)p)[i] = 0;
    return p;
}
static struct mp4 m4; static struct h264 *m4d; static struct aac m4a;
static struct mp4_iter m4vi, m4ai;
static const unsigned char *m4buf; static unsigned m4len;
static int m4on, m4paused, m4sound;
static unsigned m4pics, m4drawn, m4skip;
static unsigned long m4base, m4t0;          /* samples played before this run of sound; ticks at a silent clip's start */
static unsigned char m4pcm[1024]; static unsigned m4pcm_n, m4pcm_at;   /* a decoded sound frame not yet all queued */
static unsigned long long m4next_dts; static int m4have_next; static unsigned long long m4next_off; static unsigned m4next_sz;

static void m4_stop(void) { if (m4on) { audio_app_stop(); m4on = 0; } }
static unsigned long m4_played(void) { unsigned st[7]; audio_app_status(st); return m4base + st[6]; }
static int m4_peek(void) {   /* the next picture's sample, if it is in the bytes we hold */
    if (m4have_next) return 1;
    unsigned long long off, dts; unsigned sz;
    if (!mp4_iter_next(&m4vi, &off, &sz, &dts) || off + sz > m4len) return 0;
    m4next_off = off; m4next_sz = sz; m4next_dts = dts; m4have_next = 1;
    return 1;
}
static void m4_feed(void) {   /* top up the sound ring from the AAC frames, as far as it takes */
    if (!m4sound) return;
    for (;;) {
        if (m4pcm_at < m4pcm_n) {
            int took = audio_app_queue(m4pcm + m4pcm_at, m4pcm_n - m4pcm_at, (unsigned)m4a.rate, 0);
            m4pcm_at += (unsigned)took;
            if (m4pcm_at < m4pcm_n) return;   /* the ring is full: the rest next pass */
        }
        unsigned long long off, dts; unsigned sz;
        if (!mp4_iter_next(&m4ai, &off, &sz, &dts) || off + sz > m4len) { m4sound = 2; return; }   /* 2: the sound has all gone in */
        static short s[2048];
        if (aac_frame(&m4a, m4buf + off, sz, s) < 0) continue;   /* a damaged frame is a moment of silence */
        unsigned from = m4skip > 1024 ? 1024 : m4skip; m4skip -= from;
        m4pcm_n = 0; m4pcm_at = 0;
        for (unsigned i = from; i < 1024; i++) {   /* to 8-bit mono, what the app stream takes */
            int v = m4a.channels == 2 ? (s[2 * i] + s[2 * i + 1]) / 2 : s[i];
            m4pcm[m4pcm_n++] = (unsigned char)((v >> 8) + 128);
        }
    }
}
static void m4_draw(const struct h264_pic *p) {   /* the picture, scaled to fit, into image slot 0 (BT.601, studio range) */
    unsigned dw, dh; fit(p->w, p->h, &dw, &dh);
    for (unsigned y = 0; y < dh; y++) {
        unsigned sy = p->y0 + y * p->h / dh;
        for (unsigned x = 0; x < dw; x++) {
            unsigned sx = p->x + x * p->w / dw;
            int c = p->y[sy * p->stride + sx] - 16, d = p->u[sy / 2 * p->cstride + sx / 2] - 128, e = p->v[sy / 2 * p->cstride + sx / 2] - 128;
            int r = (298 * c + 409 * e + 128) >> 8, g = (298 * c - 100 * d - 208 * e + 128) >> 8, b = (298 * c + 516 * d + 128) >> 8;
            r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
            img_px[0][y * dw + x] = (unsigned)r << 16 | (unsigned)g << 8 | (unsigned)b;
        }
    }
    img_w[0] = dw; img_h[0] = dh;
}
static int m4_decode_next(struct h264_pic *p) {   /* one picture; 1 decoded, 0 none left, -1 refused */
    if (!m4_peek()) return 0;
    m4have_next = 0;
    const unsigned char *q = m4buf + m4next_off, *e = q + m4next_sz;
    while (e - q >= 4) {
        unsigned l = (unsigned)q[0] << 24 | q[1] << 16 | q[2] << 8 | q[3]; q += 4;
        if (l > (unsigned)(e - q)) break;
        if (h264_nal(m4d, q, l) < 0) { kputs("browser: mp4 picture refused: "); kputs(h264_error(m4d)); kputs("\n"); return -1; }
        q += l;
    }
    m4pics++;
    return h264_end(m4d, p);
}

static int url_is_mp4(const char *u) {   /* the path ends in .mp4, before any query */
    unsigned n = 0; while (u[n] && u[n] != '?' && u[n] != '#') n++;
    return n > 4 && u[n - 4] == '.' && (u[n - 3] | 32) == 'm' && (u[n - 2] | 32) == 'p' && u[n - 1] == '4';
}
static void mp4_play(const char *url) {
    vid_stop(); m4_stop();
    if (!vbuf || !mpool) { kputs("browser: out of memory for the video\n"); return; }
    kputs("browser: fetching the video\n");
    int got = fetch(url, (char *)vbuf, vcap);
    if (got <= 0) return;
    int st = http_status_of((char *)vbuf, (unsigned)got), hb = http_body_start((char *)vbuf, (unsigned)got);
    if (st != 200 || hb <= 0) { kputs("browser: the video was not there\n"); return; }
    m4buf = vbuf + hb; m4len = (unsigned)got - (unsigned)hb;
    if (mp4_open(&m4, m4buf, m4len) != MP4_OK || !m4.video.kind) { kputs("browser: not an H.264 MP4, or its index is not at the front\n"); return; }
    mpool_used = 0;
    if (!(m4d = h264_new(mp_alloc))) { kputs("browser: out of memory for the video\n"); return; }
    const unsigned char *a = m4.video.cfg, *p = a + 6; unsigned ns = a[5] & 31;
    for (unsigned i = 0; i < ns && p + 2 <= a + m4.video.cfglen; i++) { unsigned l = (unsigned)p[0] << 8 | p[1]; h264_nal(m4d, p + 2, l); p += 2 + l; }
    unsigned np = p < a + m4.video.cfglen ? *p++ : 0;
    for (unsigned i = 0; i < np && p + 2 <= a + m4.video.cfglen; i++) { unsigned l = (unsigned)p[0] << 8 | p[1]; h264_nal(m4d, p + 2, l); p += 2 + l; }
    mp4_iter_init(&m4vi, &m4.video); m4have_next = 0; m4pics = m4drawn = 0;
    m4sound = m4.audio.kind && aac_init(&m4a, m4.audio.cfg, m4.audio.cfglen) == 0;
    if (m4sound) { mp4_iter_init(&m4ai, &m4.audio); m4skip = m4.audio.skip; m4pcm_n = m4pcm_at = 0; }
    static char page[256]; unsigned n = 0;
    const char *t = title[0] ? title : "Video";
    VP("<html><head><title>"); VP(t); VP("</title></head><body><h1>"); VP(t); VP("</h1><img src=\"video.jpg\"><p>Space pauses, Space plays on.</p></body></html>");
    page[n] = 0;
    char u[URL_MAX]; scopy(u, url, slen(url), URL_MAX);
    browser_render(page, n, u);
    nimg = 1; img_w[0] = img_h[0] = 0;
    m4on = 1; m4paused = 0; m4base = 0; m4t0 = ticks();
    if (m4sound) { audio_app_stop(); m4_feed(); }
    kputs("browser: mp4 playing "); kdec(m4.video.width); kputs("x"); kdec(m4.video.height); kputs(", "); kdec(m4.video.nsamples); kputs(" pictures, ");
    if (m4sound) { kputs("sound "); kdec((unsigned)m4a.rate); kputs(" Hz\n"); } else kputs("no sound\n");
}
static int m4_tick(void) {   /* 1 when a new picture is in slot 0 */
    if (!m4on || m4paused) return 0;
    m4_feed();
    unsigned long long now = m4sound ? (unsigned long long)m4_played() * m4.video.timescale / (unsigned)m4a.rate
                                     : (unsigned long long)(ticks() - m4t0) * m4.video.timescale / 100;
    struct h264_pic pic; int got = 0, ended = 0;
    for (int k = 0; k < 4; k++) {   /* at most four pictures a pass, so the window stays live while it catches up */
        if (!m4_peek()) { ended = 1; break; }
        if (m4next_dts > now) break;
        int r = m4_decode_next(&pic);
        if (r < 0) { ended = 1; break; }
        if (r > 0) got = 1;
    }
    if (got) {
        m4_draw(&pic); m4drawn++;
        if (m4pics % 24 == 1) { kputs("browser: mp4 frame "); kdec(m4pics); kputs("\n"); }
    }
    if (ended && (!m4sound || (m4sound == 2 && m4pcm_at >= m4pcm_n && !audio_busy()))) {
        m4on = 0; kputs("browser: mp4 done pictures="); kdec(m4pics); kputs(" drawn="); kdec(m4drawn); kputs("\n");
    }
    return got;
}
static void m4_pause(void) {
    if (!m4paused) { m4base = m4_played(); audio_app_stop(); m4paused = 1; kputs("browser: video paused\n"); return; }
    m4paused = 0; m4t0 = ticks() - (unsigned long)(m4next_dts * 100 / (m4.video.timescale ? m4.video.timescale : 1));
    m4pcm_at = 0;   /* the frame being queued is played again from its start: a few ms twice, no gap */
    if (m4sound) m4_feed();
    kputs("browser: video playing on\n");
}
