/* movie: a motion-JPEG AVI player, as a ring-3 program with its own window (2.2, Movies).
 *
 * The file picker lists folders and .AVI files from the Files area (SYS_READDIR on the VFS root).
 * Opening a clip reads the whole file into the SYS_BRK heap (SYS_READFILE, because open() holds
 * 8KB), walks it once with user/libjt/avi.c to index every video and audio chunk, then plays.
 *
 * Sync rule: the audio is the clock. The frame on screen is
 *     floor(played_samples / audio_rate / (us_per_frame / 1e6))
 * with played_samples from SYS_AUDIO status. Only the frame that is due is decoded, a late frame is
 * dropped and never waited for. The audio ring is topped up only as space opens. A clip without
 * sound (or a machine without a card) runs from the 100Hz tick counter instead.
 *
 * Seek and pause both stop the audio, wait for the queue to go idle (the kernel resets `played` then),
 * and requeue from the chunk and sample that match the target time.
 *
 * Draw: the frame is decoded with jpeg_decode_scaled (shrink only, at most 256 wide), then nearest
 * scaled into a letterboxed rectangle. `f` hides the controls and letterboxes into the whole window
 * (there is no syscall to resize the window itself).
 *
 * Serial lines (what tools/checks/movie-check.py reads): movie=play, movie=frame N ms=M played=P,
 * movie=pause, movie=resume, movie=seek, movie=done, movie=error REASON. Backquote crashes on purpose
 * so tools/checks/ring3crash-all-check.py can prove the desktop survives it.
 */
#include "jtsys.h"
#include "libjt/text.h"
#include "libjt/stdlib.h"
#include "libjt/string.h"
#include "libjt/avi.h"

/* decoder from drivers/jpeg.c, compiled into libjt (user/libjt/jpeg.c) */
int jpeg_decode_scaled(const unsigned char *data, unsigned int len, unsigned short *dst,
                       unsigned int dw, unsigned int dh, unsigned int *w, unsigned int *h);

#define BG     0x00FAF8F6
#define INK    0x001C1C1E
#define DIM    0x0075726E
#define RULE   0x00E0D8CE
#define ACCENT 0x00B5502C
#define STAGE  0x00111111
#define PALE   0x00F5F0EB

#define MAX_FILE   (6u * 1024u * 1024u) /* the heap is 8MB; leave room for the index and the decoder */
#define MAX_CHUNKS 40000u
#define HEAD_H     28
#define BAR_H      44
#define ROW_H      26
#define AUDIO_STAGE 16384u

enum { M_PICK, M_PLAY, M_ERR };

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
static int mode JT_DATA = M_PICK;
static int dirty JT_DATA = 1;

/* picker */
static char cwd[JT_PATH_MAX + 1] JT_DATA = {0};
static struct jt_dirent raw[JT_READDIR_MAX] JT_DATA;
static int order[JT_READDIR_MAX] JT_DATA;
static int count JT_DATA = 0, sel JT_DATA = 0, top JT_DATA = 0;
static char errmsg[72] JT_DATA = {0};
static char errname[JT_DIRENT_NAME] JT_DATA = {0};

/* the open clip */
static unsigned char *fbuf JT_DATA = 0;
static struct avi av JT_DATA;
static unsigned *voff JT_DATA = 0, *vlen JT_DATA = 0;     /* video chunks: offset into fbuf, length */
static unsigned *aoff JT_DATA = 0, *alen JT_DATA = 0, *acum JT_DATA = 0; /* audio chunks; acum[i] = output samples before chunk i */
static unsigned nv JT_DATA = 0, na JT_DATA = 0, total_samples JT_DATA = 0;
static unsigned short *vbuf JT_DATA = 0;                    /* decoded frame, RGB565 */
static unsigned dw JT_DATA = 0, dh JT_DATA = 0;             /* its size */
static unsigned char *stage JT_DATA = 0;                    /* converted audio waiting for the ring */
static unsigned us JT_DATA = 100000, dur_ms JT_DATA = 0;
static char fname[JT_DIRENT_NAME] JT_DATA = {0};

/* playback */
static int paused JT_DATA = 0, ended JT_DATA = 0, fs JT_DATA = 0;
static int have_audio JT_DATA = 0;     /* the clip has sound and the card took it */
static int audio_clock JT_DATA = 0;    /* 1: the clock is base_samples + played, 0: ticks */
static unsigned a_rate JT_DATA = 0;
static unsigned hold_ms JT_DATA = 0;   /* the position while the clock is not running */
static int shown JT_DATA = -1;         /* frame on screen, -1 none yet */
static unsigned dropped JT_DATA = 0, shown_n JT_DATA = 0, bad_run JT_DATA = 0;
static int waiting_idle JT_DATA = 0, run_started JT_DATA = 0, feed_done JT_DATA = 0;
static unsigned base_samples JT_DATA = 0, last_played JT_DATA = 0;
static unsigned feed_chunk JT_DATA = 0, feed_off JT_DATA = 0;  /* next audio chunk and output samples of it already taken */
static unsigned stage_len JT_DATA = 0, stage_off JT_DATA = 0;
static unsigned t0_ticks JT_DATA = 0, base_ms JT_DATA = 0;     /* tick clock: position = base_ms + (ticks - t0) * 10 */
static unsigned last_sec JT_DATA = 0;

/* layout, set by layout() */
static int px JT_DATA, py JT_DATA, pw JT_DATA, ph JT_DATA;     /* the picture rectangle */
static int vx JT_DATA, vy JT_DATA, vw JT_DATA, vh JT_DATA;     /* the video stage around it */
static int bar_x0 JT_DATA, bar_x1 JT_DATA, bar_y JT_DATA;
static unsigned short *colmap JT_DATA = 0;
static int colmap_n JT_DATA = 0;

/* ---- small helpers ---------------------------------------------------- */
static void rect(int x, int y, int w, int h, unsigned c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)win.width)  w = (int)win.width - x;
    if (y + h > (int)win.height) h = (int)win.height - y;
    for (int yy = 0; yy < h; yy++) {
        unsigned *row = win.pixels + (unsigned)(y + yy) * win.width + (unsigned)x;
        for (int xx = 0; xx < w; xx++) row[xx] = c;
    }
}
static int text(int face, const char *s, int x, int y, unsigned fg) { return jt_text_draw(&win, face, x, y, fg, s); }
static int utoa10(unsigned v, char *buf) {
    char tmp[12]; int tn = 0, n = 0;
    do { tmp[tn++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (tn) buf[n++] = tmp[--tn];
    buf[n] = 0;
    return n;
}
/* one serial line, one write: "movie=" + words and numbers. Pass strings and unsigned values alternately via the helpers. */
struct ln { char b[120]; int n; };
static void ln_s(struct ln *l, const char *s) { while (*s && l->n < 112) l->b[l->n++] = *s++; }
static void ln_u(struct ln *l, unsigned v) { char t[12]; utoa10(v, t); ln_s(l, t); }
static void ln_out(struct ln *l) { l->b[l->n++] = '\n'; jt_write(1, l->b, (unsigned)l->n); }
static void say(const char *s) { struct ln l = {{0}, 0}; ln_s(&l, s); ln_out(&l); }
static unsigned ticks(void) { struct jt_tasks t; jt_tasks(&t, -1); return t.ticks; }

static unsigned s_to_ms(unsigned s, unsigned rate) { return s / rate * 1000u + (s % rate) * 1000u / rate; }
static unsigned ms_to_s(unsigned ms, unsigned rate) { return ms / 1000u * rate + (ms % 1000u) * rate / 1000u; }
static void mmss(unsigned ms, char *out) {
    unsigned s = ms / 1000u; int n = utoa10(s / 60u, out);
    out[n++] = ':'; out[n++] = (char)('0' + s % 60u / 10u); out[n++] = (char)('0' + s % 10u); out[n] = 0;
}
static int is_avi(const char *n) {
    int l = 0; while (n[l]) l++;
    if (l < 5) return 0;
    const char *e = n + l - 4;
    return e[0] == '.' && (e[1] | 32) == 'a' && (e[2] | 32) == 'v' && (e[3] | 32) == 'i';
}

/* ---- picker ----------------------------------------------------------- */
static void load_dir(void) {
    int r = jt_readdir(cwd, raw, JT_READDIR_MAX);
    int n = r < 0 ? 0 : (r > JT_READDIR_MAX ? JT_READDIR_MAX : r);
    count = 0;
    for (int pass = 0; pass < 2; pass++)           /* folders first, then clips, each sorted by name */
        for (int i = 0; i < n; i++) {
            if ((pass == 0) != (raw[i].is_dir != 0)) continue;
            if (pass == 1 && !is_avi(raw[i].name)) continue;
            int k = count++;
            while (k > 0 && ((pass == 0) == (raw[order[k - 1]].is_dir != 0)) && strcmp(raw[order[k - 1]].name, raw[i].name) > 0) { order[k] = order[k - 1]; k--; }
            order[k] = i;
        }
    if (sel >= count) sel = count ? count - 1 : 0;
    top = 0;
    struct ln l = {{0}, 0}; ln_s(&l, "movie=list "); ln_s(&l, cwd[0] ? cwd : "/"); ln_s(&l, " n="); ln_u(&l, (unsigned)count); ln_out(&l);
}
static void announce_sel(void) {
    if (!count) return;
    struct ln l = {{0}, 0}; ln_s(&l, "movie=select "); ln_s(&l, raw[order[sel]].name); ln_out(&l);
}

/* ---- clip lifetime ---------------------------------------------------- */
static void close_clip(void) {
    jt_audio_stop();
    if (fbuf) free(fbuf);
    if (voff) free(voff); if (vlen) free(vlen);
    if (aoff) free(aoff); if (alen) free(alen); if (acum) free(acum);
    if (vbuf) free(vbuf);
    if (stage) free(stage);
    fbuf = 0; voff = vlen = aoff = alen = acum = 0; vbuf = 0; stage = 0;
    nv = na = 0; have_audio = 0; audio_clock = 0; waiting_idle = run_started = feed_done = 0;
}
static void fail(const char *why, const char *code) {
    close_clip();
    int i = 0; while (why[i] && i < (int)sizeof errmsg - 1) { errmsg[i] = why[i]; i++; } errmsg[i] = 0;
    mode = M_ERR; dirty = 1;
    struct ln l = {{0}, 0}; ln_s(&l, "movie=error "); ln_s(&l, code); ln_out(&l);
}

static int decode_frame(unsigned f) {
    unsigned w = 0, h = 0;
    return jpeg_decode_scaled(fbuf + voff[f], vlen[f], vbuf, dw, dh, &w, &h);
}

/* Queue state after stop: wait until idle, then feed from the chunk holding sample s. */
static void audio_restart(unsigned ms) {
    unsigned s = ms_to_s(ms, a_rate);
    jt_audio_stop();
    waiting_idle = 1; run_started = 0; audio_clock = 1; last_played = 0;
    stage_len = stage_off = 0; feed_done = 0;
    base_samples = s;
    if (s >= total_samples) { feed_done = 1; feed_chunk = na; feed_off = 0; return; }
    unsigned c = 0;
    while (c + 1 < na && acum[c + 1] <= s) c++;
    feed_chunk = c; feed_off = s - acum[c];
}
static void tick_clock_from(unsigned ms) { audio_clock = 0; base_ms = ms; t0_ticks = ticks(); }

static unsigned clock_ms(void) {
    if (paused || ended) return hold_ms;
    if (audio_clock) {
        if (!run_started) return hold_ms;
        struct jt_audio_status st;
        if (jt_audio_status(&st) < 0) return hold_ms;
        if (st.played < last_played) base_samples += last_played;   /* the queue went idle and restarted */
        last_played = st.played;
        return s_to_ms(base_samples + st.played, a_rate);
    }
    return base_ms + (ticks() - t0_ticks) * 10u;
}

static void start_at(unsigned ms) {
    hold_ms = ms; shown = -1; paused = 0; ended = 0;
    if (have_audio) audio_restart(ms); else tick_clock_from(ms);
}

static void open_clip(int idx) {
    const struct jt_dirent *e = &raw[idx];
    int i = 0; while (e->name[i] && i < JT_DIRENT_NAME - 1) { fname[i] = e->name[i]; errname[i] = e->name[i]; i++; } fname[i] = errname[i] = 0;
    char path[JT_PATH_MAX + 1]; int l = 0;
    for (int k = 0; cwd[k] && l < JT_PATH_MAX; k++) path[l++] = cwd[k];
    if (l && l < JT_PATH_MAX) path[l++] = '/';
    for (int k = 0; fname[k] && l < JT_PATH_MAX; k++) path[l++] = fname[k];
    path[l] = 0;

    if (e->size > MAX_FILE) {
        char m[72] = "Too big to play here: "; int n = 22;
        n += utoa10((e->size + 1048575u) / 1048576u, m + n);
        const char *t = " MB, the limit is 6 MB"; while (*t) m[n++] = *t++; m[n] = 0;
        fail(m, "too-big"); return;
    }
    if (e->size < 12) { fail("This file is empty or not a movie", "format"); return; }
    fbuf = (unsigned char *)malloc(e->size);
    if (!fbuf) { fail("Not enough memory to load this file", "nomem"); return; }
    int n = jt_readfile(path, fbuf, e->size);
    if (n != (int)e->size) { fail(n == -27 ? "Too big to play here" : "Could not read the file", "read"); return; }
    int r = avi_open(&av, fbuf, e->size);
    if (r == AVI_E_UNSUP) { fail("Only motion-JPEG video with PCM sound plays here", "unsupported"); return; }
    if (r != AVI_OK) { fail("This is not a playable AVI file", "format"); return; }

    /* index every chunk once: the player then seeks by number, never by scanning */
    struct avi_chunk c;
    avi_rewind(&av);
    while (avi_next(&av, &c)) { if (c.kind == AVI_VIDEO) nv++; else na++; }
    if (!nv) { fail("The movie has no video frames", "empty"); return; }
    if (nv > MAX_CHUNKS || na > MAX_CHUNKS) { fail("The movie has too many chunks", "toomany"); return; }
    voff = (unsigned *)malloc(nv * 4u); vlen = (unsigned *)malloc(nv * 4u);
    aoff = (unsigned *)malloc((na + 1) * 4u); alen = (unsigned *)malloc((na + 1) * 4u); acum = (unsigned *)malloc((na + 1) * 4u);
    if (!voff || !vlen || !aoff || !alen || !acum) { fail("Not enough memory to index the movie", "nomem"); return; }
    unsigned step = av.achannels * (av.abits / 8u), vi = 0, ai = 0, acc = 0;
    avi_rewind(&av);
    while (avi_next(&av, &c)) {
        unsigned off = (unsigned)(c.data - fbuf);
        if (c.kind == AVI_VIDEO) { voff[vi] = off; vlen[vi] = c.len; vi++; }
        else { aoff[ai] = off; alen[ai] = c.len; acum[ai] = acc; acc += step ? c.len / step : 0; ai++; }
    }
    acum[na] = total_samples = acc;
    us = av.us_per_frame;
    dur_ms = nv * (us / 1000u) + nv * (us % 1000u) / 1000u;

    /* decode size: never above the source, never above what the decoder's row buffer holds */
    dw = av.width < 256 ? av.width : 256;
    dh = av.height * dw / av.width;
    if (dh > 256) { dh = 256; dw = av.width * dh / av.height; }
    if (!dw) dw = 1;
    if (!dh) dh = 1;
    vbuf = (unsigned short *)malloc(dw * dh * 2u);
    stage = (unsigned char *)malloc(AUDIO_STAGE);
    if (!vbuf || !stage) { fail("Not enough memory to play", "nomem"); return; }
    for (unsigned k = 0; k < dw * dh; k++) vbuf[k] = 0;
    if (decode_frame(0) != 0) { fail("The first frame will not decode: the file is damaged", "corrupt"); return; }

    a_rate = av.arate;
    have_audio = av.arate >= 4000 && av.arate <= 44100 && na > 0;
    bad_run = 0; dropped = 0; shown_n = 0; last_sec = 0xFFFFFFFFu;
    mode = M_PLAY; dirty = 1;
    {   struct ln lg = {{0}, 0};
        ln_s(&lg, "movie=play "); ln_s(&lg, fname); ln_s(&lg, " frames="); ln_u(&lg, nv); ln_s(&lg, " us="); ln_u(&lg, us);
        ln_s(&lg, " rate="); ln_u(&lg, have_audio ? a_rate : 0); ln_s(&lg, " size="); ln_u(&lg, e->size); ln_out(&lg); }
    start_at(0);
}

/* ---- audio feeding ---------------------------------------------------- */
/* Top the ring up; only as much as the ring has room for. */
static void feed_audio(void) {
    if (!have_audio || !audio_clock || paused || ended) return;
    struct jt_audio_status st;
    if (jt_audio_status(&st) < 0) { have_audio = 0; tick_clock_from(hold_ms); return; }
    if (waiting_idle) {
        if (st.playing) return;
        waiting_idle = 0; last_played = 0;
    }
    if (feed_done) {
        if (run_started && !st.playing) {              /* the sound ended before the picture: carry on by the tick clock */
            unsigned at = clock_ms();
            tick_clock_from(at);
        }
        return;
    }
    for (int guard = 0; guard < 8; guard++) {
        if (stage_off >= stage_len) {
            if (feed_chunk >= na) { feed_done = 1; break; }
            unsigned step = av.achannels * (av.abits / 8u), have = alen[feed_chunk] / step;
            if (feed_off >= have) { feed_chunk++; feed_off = 0; continue; }
            unsigned take = have - feed_off;
            if (take > AUDIO_STAGE) take = AUDIO_STAGE;
            stage_len = avi_audio8(&av, fbuf + aoff[feed_chunk] + feed_off * step, take * step, stage, AUDIO_STAGE);
            stage_off = 0; feed_off += take;
            if (feed_off >= have) { feed_chunk++; feed_off = 0; }
            if (!stage_len) continue;
        }
        if (jt_audio_status(&st) < 0) break;
        unsigned room = st.space, n = stage_len - stage_off;
        if (n > room) n = room;
        if (n > JT_AUDIO_CHUNK_MAX) n = JT_AUDIO_CHUNK_MAX;
        if (!n) break;
        int last = (feed_chunk >= na) && (stage_off + n == stage_len);
        int r = jt_audio_play(stage + stage_off, n, a_rate, last ? JT_AUDIO_END : 0);
        if (r < 0) { have_audio = 0; tick_clock_from(hold_ms); return; }   /* no card: the picture still plays */
        if (r == 0) break;
        stage_off += (unsigned)r;
        run_started = 1;
        if (last && stage_off >= stage_len) { feed_done = 1; break; }
    }
}

/* ---- drawing ---------------------------------------------------------- */
static void layout(void) {
    int W = (int)win.width, H = (int)win.height;
    if (fs) { vx = 0; vy = 0; vw = W; vh = H; }
    else { vx = 0; vy = HEAD_H; vw = W; vh = H - HEAD_H - BAR_H; if (vh < 8) vh = 8; }
    if (mode == M_PLAY && av.width && av.height) {
        if ((long)vw * (long)av.height <= (long)vh * (long)av.width) { pw = vw; ph = (int)((unsigned)vw * av.height / av.width); }
        else { ph = vh; pw = (int)((unsigned)vh * av.width / av.height); }
        if (pw < 1) pw = 1;
        if (ph < 1) ph = 1;
        px = vx + (vw - pw) / 2; py = vy + (vh - ph) / 2;
    }
    bar_x0 = 52; bar_x1 = W - 110; bar_y = H - BAR_H + 19;
    if (bar_x1 < bar_x0 + 20) bar_x1 = bar_x0 + 20;
    if (colmap_n < W) {
        if (colmap) free(colmap);
        colmap = (unsigned short *)malloc((unsigned long)W * 2u);
        colmap_n = colmap ? W : 0;
    }
}

static void draw_picture(void) {
    if (!vbuf || !colmap || pw < 1 || ph < 1 || colmap_n < pw) return;
    for (int x = 0; x < pw; x++) colmap[x] = (unsigned short)((unsigned)x * dw / (unsigned)pw);
    int prev = -1; unsigned *prow = 0;
    for (int y = 0; y < ph; y++) {
        int sy = (int)((unsigned)y * dh / (unsigned)ph);
        int wy = py + y;
        if (wy < 0 || wy >= (int)win.height) continue;
        unsigned *row = win.pixels + (unsigned)wy * win.width;
        if (sy == prev && prow) { memcpy(row + px, prow + px, (unsigned long)pw * 4u); prow = row; continue; }
        const unsigned short *src = vbuf + (unsigned)sy * dw;
        for (int x = 0; x < pw; x++) {
            unsigned p = src[colmap[x]];
            unsigned r = ((p >> 8) & 0xF8u) | (p >> 13), g = ((p >> 3) & 0xFCu) | ((p >> 9) & 3u), b = ((p << 3) & 0xF8u) | ((p >> 2) & 7u);
            row[px + x] = (r << 16) | (g << 8) | b;
        }
        prev = sy; prow = row;
    }
}

static void draw_bar(unsigned pos_ms) {
    int W = (int)win.width, H = (int)win.height;
    rect(0, H - BAR_H, W, BAR_H, BG);
    rect(0, H - BAR_H, W, 1, RULE);
    int cy = H - BAR_H + 22;
    if (paused || ended) {                                   /* play triangle */
        for (int i = 0; i < 12; i++) rect(18 + i, cy - 7 + i / 2, 1, 14 - i, ACCENT);
    } else {                                                 /* pause bars */
        rect(18, cy - 7, 4, 14, ACCENT); rect(26, cy - 7, 4, 14, ACCENT);
    }
    rect(bar_x0, bar_y, bar_x1 - bar_x0, 6, RULE);
    int fw = 0;
    if (dur_ms) { unsigned w = (unsigned)(bar_x1 - bar_x0); if (pos_ms > dur_ms) pos_ms = dur_ms; fw = (int)((pos_ms / 16u) * w / (dur_ms / 16u + 1u)); }
    if (fw > bar_x1 - bar_x0) fw = bar_x1 - bar_x0;
    rect(bar_x0, bar_y, fw, 6, ACCENT);
    rect(bar_x0 + fw - 1, bar_y - 3, 3, 12, ACCENT);
    char a[12], b[12], t[28]; mmss(pos_ms, a); mmss(dur_ms, b);
    int n = 0; for (int i = 0; a[i]; i++) t[n++] = a[i];
    t[n++] = ' '; t[n++] = '/'; t[n++] = ' '; for (int i = 0; b[i]; i++) t[n++] = b[i]; t[n] = 0;
    text(JT_FACE_BODY, t, bar_x1 + 12, bar_y - 8, INK);
}

static void draw_head(void) {
    int W = (int)win.width;
    rect(0, 0, W, HEAD_H, BG);
    text(JT_FACE_BOLD, fname, 20, 4, INK);
    const char *h = "space  left right 5s  f  esc";
    text(JT_FACE_BODY, h, W - jt_text_width(JT_FACE_BODY, h) - 16, 5, DIM);
}

static void draw_list(void) {
    int W = (int)win.width, H = (int)win.height;
    rect(0, 0, W, H, BG);
    text(JT_FACE_BOLD, "Movies", 20, 14, INK);
    {   char p[JT_PATH_MAX + 4]; int n = 0; p[n++] = '/';
        for (int i = 0; cwd[i] && n < JT_PATH_MAX; i++) p[n++] = cwd[i];
        p[n] = 0; text(JT_FACE_BODY, p, 100, 14, DIM); }
    const char *h = "up down  enter plays  backspace folder up  esc closes";
    text(JT_FACE_BODY, h, W - jt_text_width(JT_FACE_BODY, h) - 16, 14, DIM);
    rect(20, 42, W - 40, 1, RULE);
    int rows = (H - 56) / ROW_H; if (rows < 1) rows = 1;
    if (sel < top) top = sel;
    if (sel >= top + rows) top = sel - rows + 1;
    if (!count) { text(JT_FACE_BODY, "No .AVI files here. Put a motion-JPEG AVI in Files and open Movies again.", 20, 60, DIM); return; }
    for (int i = 0; i < rows && top + i < count; i++) {
        const struct jt_dirent *e = &raw[order[top + i]];
        int y = 50 + i * ROW_H;
        if (top + i == sel) rect(14, y, W - 28, ROW_H - 2, PALE);
        if (e->is_dir) rect(22, y + 7, 12, 10, DIM); else { rect(22, y + 5, 12, 14, ACCENT); rect(27, y + 9, 3, 6, PALE); }
        char nm[JT_DIRENT_NAME + 2]; int n = 0;
        for (int k = 0; e->name[k] && n < JT_DIRENT_NAME - 1; k++) nm[n++] = e->name[k];
        if (e->is_dir) nm[n++] = '/';
        nm[n] = 0;
        text(JT_FACE_BODY, nm, 44, y + 3, INK);
        if (!e->is_dir) {
            char sz[20]; int m = utoa10((e->size + 1023u) / 1024u, sz); sz[m++] = ' '; sz[m++] = 'K'; sz[m++] = 'B'; sz[m] = 0;
            text(JT_FACE_BODY, sz, W - 40 - jt_text_width(JT_FACE_BODY, sz), y + 3, DIM);
        }
    }
}

static void draw_err(void) {
    int W = (int)win.width, H = (int)win.height;
    rect(0, 0, W, H, BG);
    char t[JT_DIRENT_NAME + 24]; int n = 0; const char *p = "Can't play "; while (*p) t[n++] = *p++;
    for (int i = 0; errname[i] && n < (int)sizeof t - 2; i++) t[n++] = errname[i]; t[n] = 0;
    text(JT_FACE_BOLD, t, 24, 40, INK);
    text(JT_FACE_BODY, errmsg, 24, 72, ACCENT);
    text(JT_FACE_BODY, "enter or esc goes back to the list", 24, 110, DIM);
}

static void draw_all(unsigned pos_ms) {
    layout();
    if (mode == M_PICK) { draw_list(); return; }
    if (mode == M_ERR) { draw_err(); return; }
    {   static int lp[4] JT_DATA = {-1, -1, -1, -1};   /* the picture rectangle, once per change: what the check reads pixels by */
        if (lp[0] != px || lp[1] != py || lp[2] != pw || lp[3] != ph) {
            lp[0] = px; lp[1] = py; lp[2] = pw; lp[3] = ph;
            struct ln l = {{0}, 0}; ln_s(&l, "movie=layout "); ln_u(&l, (unsigned)px); ln_s(&l, " "); ln_u(&l, (unsigned)py);
            ln_s(&l, " "); ln_u(&l, (unsigned)pw); ln_s(&l, " "); ln_u(&l, (unsigned)ph); ln_out(&l);
        }
    }
    rect(0, 0, (int)win.width, (int)win.height, BG);
    rect(vx, vy, vw, vh, STAGE);
    if (!fs) { draw_head(); draw_bar(pos_ms); }
    if (shown >= 0) draw_picture();
}

/* ---- the player's heartbeat -------------------------------------------- */
static void finish(void) {
    ended = 1; hold_ms = dur_ms; jt_audio_stop(); audio_clock = 0;
    struct ln l = {{0}, 0};
    ln_s(&l, "movie=done frames="); ln_u(&l, nv); ln_s(&l, " shown="); ln_u(&l, shown_n); ln_s(&l, " dropped="); ln_u(&l, dropped);
    ln_out(&l); dirty = 1;
}

static void tick(void) {
    if (ended) return;
    if (!paused) feed_audio();
    unsigned ms = clock_ms();
    if (!paused) {
        unsigned f = ms * 1000u / us;
        if (f >= nv) { finish(); return; }
        if ((int)f != shown) {
            if (shown >= 0 && (int)f > shown + 1) dropped += f - (unsigned)shown - 1u;
            if (decode_frame(f) == 0) {
                bad_run = 0;
                struct ln l = {{0}, 0};
                struct jt_audio_status st; st.played = 0;
                if (audio_clock) jt_audio_status(&st);
                ln_s(&l, "movie=frame "); ln_u(&l, f); ln_s(&l, " ms="); ln_u(&l, ms); ln_s(&l, " played="); ln_u(&l, st.played);
                ln_out(&l);
                shown = (int)f; shown_n++;
                if (!fs) { rect(vx, vy, vw, vh, STAGE); }
                draw_picture();
                dirty = 1;
            } else if (++bad_run >= 5) { fail("The movie is damaged and stopped playing", "corrupt"); return; }
            else shown = (int)f;      /* keep the last good picture, move on */
        }
    }
    unsigned sec = ms / 1000u;
    if (sec != last_sec) { last_sec = sec; dirty = 1; }
}

static void seek_to(int delta_ms, int absolute, unsigned abs_ms) {
    unsigned cur = clock_ms(), t;
    if (absolute) t = abs_ms;
    else if (delta_ms < 0) t = cur > (unsigned)(-delta_ms) ? cur - (unsigned)(-delta_ms) : 0;
    else t = cur + (unsigned)delta_ms;
    if (t >= dur_ms) t = dur_ms > 1 ? dur_ms - 1 : 0;
    struct ln l = {{0}, 0}; ln_s(&l, "movie=seek ms="); ln_u(&l, t); ln_out(&l);
    int was_paused = paused;
    ended = 0; shown = -1; hold_ms = t; run_started = 0;
    if (was_paused) { jt_audio_stop(); decode_frame(t * 1000u / us < nv ? t * 1000u / us : nv - 1); shown = (int)(t * 1000u / us); }
    else { paused = 0; if (have_audio) audio_restart(t); else tick_clock_from(t); }
    dirty = 1;
}

static void toggle_pause(void) {
    if (ended) { say("movie=restart"); start_at(0); dirty = 1; return; }
    if (!paused) {
        hold_ms = clock_ms(); paused = 1; jt_audio_stop(); run_started = 0; waiting_idle = 0;
        struct ln l = {{0}, 0}; ln_s(&l, "movie=pause ms="); ln_u(&l, hold_ms); ln_s(&l, " frame="); ln_u(&l, (unsigned)(shown < 0 ? 0 : shown)); ln_out(&l);
    } else {
        paused = 0;
        struct ln l = {{0}, 0}; ln_s(&l, "movie=resume ms="); ln_u(&l, hold_ms); ln_out(&l);
        if (have_audio) audio_restart(hold_ms); else tick_clock_from(hold_ms);
    }
    dirty = 1;
}

static void back_to_list(void) {
    close_clip(); mode = M_PICK; fs = 0; paused = 0; ended = 0; dirty = 1;
    say("movie=stop");
}

static void enter_dir(const char *name) {
    char saved[JT_PATH_MAX + 1]; memcpy(saved, cwd, sizeof saved);
    int l = 0; while (cwd[l]) l++;
    if (l && l < JT_PATH_MAX) cwd[l++] = '/';
    for (const char *s = name; *s && l < JT_PATH_MAX; s++) cwd[l++] = *s;
    cwd[l] = 0;
    if (jt_readdir(cwd, raw, JT_READDIR_MAX) < 0) { memcpy(cwd, saved, sizeof saved); return; }
    sel = 0; load_dir(); announce_sel();
}
static void up_dir(void) {
    int l = 0; while (cwd[l]) l++;
    while (l > 0 && cwd[l - 1] != '/') l--;
    if (l > 0) l--;
    cwd[l] = 0; sel = 0; load_dir(); announce_sel();
}

static void activate(void) {
    if (!count) return;
    const struct jt_dirent *e = &raw[order[sel]];
    if (e->is_dir) enter_dir(e->name); else open_clip(order[sel]);
    dirty = 1;
}

static void on_key(int k) {
    if (k == '`') { say("movies: crashing on purpose"); *(volatile int *)0 = 1; }
    if (mode == M_ERR) { if (k == JT_KEY_ENTER || k == JT_KEY_ESC || k == ' ') { mode = M_PICK; dirty = 1; load_dir(); } return; }
    if (mode == M_PICK) {
        if (k == JT_KEY_ESC) { say("movies: closed"); jt_exit(0); }
        else if (k == JT_KEY_DOWN && sel + 1 < count) { sel++; announce_sel(); dirty = 1; }
        else if (k == JT_KEY_UP && sel > 0) { sel--; announce_sel(); dirty = 1; }
        else if (k == JT_KEY_ENTER) activate();
        else if (k == 8) { up_dir(); dirty = 1; }
        return;
    }
    if (k == JT_KEY_ESC) { if (fs) { fs = 0; say("movie=fullscreen 0"); dirty = 1; } else back_to_list(); }
    else if (k == ' ') toggle_pause();
    else if (k == JT_KEY_LEFT) seek_to(-5000, 0, 0);
    else if (k == JT_KEY_RIGHT) seek_to(5000, 0, 0);
    else if (k == 'f') { fs = !fs; say(fs ? "movie=fullscreen 1" : "movie=fullscreen 0"); dirty = 1; }
}

static void on_click(int x, int y) {
    if (mode == M_ERR) { mode = M_PICK; dirty = 1; load_dir(); return; }
    if (mode == M_PICK) {
        int row = (y - 50) / ROW_H;
        if (y >= 50 && top + row < count) {
            if (top + row == sel) activate(); else { sel = top + row; announce_sel(); dirty = 1; }
        }
        return;
    }
    if (fs) { toggle_pause(); return; }
    int H = (int)win.height;
    if (y >= H - BAR_H) {
        if (x < bar_x0 - 8) toggle_pause();
        else if (x >= bar_x0 - 4 && x <= bar_x1 + 4 && dur_ms) {
            unsigned w = (unsigned)(bar_x1 - bar_x0), dx = x <= bar_x0 ? 0u : (unsigned)(x - bar_x0);
            if (dx > w) dx = w;
            seek_to(0, 1, dx * (dur_ms / w) + dx * (dur_ms % w) / w);
        }
    } else if (y >= vy) toggle_pause();
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open(&win) != 0 || !win.pixels) {
        jt_write(2, "movies: no window\n", 18);
        jt_exit(1);
    }
    {   struct ln l = {{0}, 0}; ln_s(&l, "movies: ring-3 window "); ln_u(&l, win.width); ln_s(&l, "x"); ln_u(&l, win.height); ln_out(&l); }
    load_dir();
    announce_sel();
    draw_all(0);
    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        if (r == 1 && jt_window_resized(&ev, &win)) { draw_all(clock_ms()); flags = JT_POLL_PRESENT; continue; }
        flags = 0;
        if (mode == M_PLAY) tick();
        if (r == -11 /* -EAGAIN */) {
            if (dirty) {
                if (mode == M_PLAY && !fs) draw_bar(clock_ms()); else if (mode != M_PLAY) draw_all(0);
                dirty = 0; flags = JT_POLL_PRESENT;
            } else jt_sched_yield();
            continue;
        }
        if (r != 1) break;
        if (ev.kind == JT_EV_KEY) on_key(ev.a);
        else if (ev.kind == JT_EV_CLICK) on_click(ev.a, ev.b);
        if (dirty) { draw_all(clock_ms()); dirty = 0; }
        flags = JT_POLL_PRESENT;
    }
    say("movies: closed");
    jt_exit(0);
}
