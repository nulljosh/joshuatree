/* music: a song player, as a real ring-3 program with its own window.
 *
 * The library is every .WAV (and .MP3) in the Files root and in a MUSIC
 * folder, found with SYS_READDIR. One track at a time is read whole into the SYS_BRK heap with
 * SYS_READFILE (open() stops at 8KB) and refused with a visible message above MAX_BYTES.
 * user/libjt/wav.c turns it into 8-bit unsigned mono PCM, which this program volume-scales and
 * feeds to SYS_AUDIO in chunks as the ring has room.
 *
 * Time is never a guess: the elapsed position is the frame the current stream started at plus
 * jt_audio_status().played. Pause, seek and track changes stop the queue, wait until it has
 * drained (playing == 0, so the in-flight DMA chunk is counted), then restart from the new frame.
 * While paused the position is frozen at the number the drain produced.
 *
 * Keys: space play/pause, left/right seek 5s, up/down volume, n/p next/previous, s shuffle,
 * r repeat (off, all, one), Esc closes. Mouse: click a song, a button, the seek bar or the
 * volume bar, or drag either knob (the seek lands on release); the wheel scrolls the list. Shuffle plays every song once a round: repeat off
 * stops when the round is done, repeat all starts another. When one song ends into the next, a
 * song that will not play is skipped; n and p stop on it and say why. The backquote key is the
 * deliberate crash tools/checks/ring3crash-all-check.py presses. tools/checks/music-check.py and
 * music-qa-check.py read the `music:` lines this program writes to serial.
 */
#include "jtsys.h"
#include "libjt/text.h"
#include "libjt/wav.h"
#include "libjt/mp3.h"
#include "libjt/string.h"
#include "libjt/stdlib.h"

/* drivers/jpeg.c, compiled into libjt: album art is the JPEG inside the MP3's ID3 tag */
int jpeg_decode_scaled32(const unsigned char *data, unsigned int len, unsigned int *dst, unsigned int dw, unsigned int dh,
                         unsigned int *w, unsigned int *h);

#define BG     0x00FAF8F6
#define INK    0x001C1C1E
#define HINT   0x0075726E
#define ACCENT 0x00B5502C
#define TINT   0x00F2E4DC
#define RULE   0x00E4DED6
#define CREAM  0x00FAF8F4

#define MAX_TRACKS 64
#define MAX_BYTES  (3u << 20)   /* biggest song read whole into the heap */
#define CHUNK      4096         /* bytes fed to SYS_AUDIO per call */
#define LEAD_MIN   3072         /* keep about a third of a second queued (never under this, never over LEAD_MAX): */
#define LEAD_MAX   8192         /* the volume is applied before a sample is queued, so what is queued is what is late */
#define SEEK_MS    5000
#define EAGAIN_    11
#define ENOSYS_    38

enum { ST_STOPPED, ST_PLAYING, ST_PAUSED };
enum { K_PAUSE, K_SEEK, K_START };

struct track { char file[JT_DIRENT_NAME]; unsigned char dir; unsigned size; char title[40]; char artist[28]; unsigned ms; };   /* title and artist: the MP3's ID3 tag, when it has one */

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
static struct jt_dirent ents[JT_READDIR_MAX] JT_DATA;
static struct track tracks[MAX_TRACKS] JT_DATA;
static int ntracks JT_DATA = 0;
static int sel JT_DATA = 0;            /* highlighted row */
static int cur JT_DATA = -1;           /* loaded track, -1 none */
static int top JT_DATA = 0;            /* first visible row */
static int state JT_DATA = ST_STOPPED;
static int shuffle JT_DATA = 0;
static unsigned char heard[MAX_TRACKS] JT_DATA;   /* shuffle: songs that have had their turn this round */
static int tried JT_DATA = 0;          /* a song has been started since the window opened (so sel is where n and p move from) */
static int repeat JT_DATA = 0;         /* 0 off, 1 all, 2 one */
static int volume JT_DATA = 100;       /* 0..100, applied to samples before they are queued */
static char msg[64] JT_DATA = {0};     /* a visible message: too big, not a WAV, no card */
static unsigned rng JT_DATA = 2463534242u;
static int drag_zone JT_DATA = 0;        /* what a held press grabbed: 1 the seek bar, 2 the volume bar, 3 nothing that drags */
static int scrubbing JT_DATA = 0;        /* the seek knob follows the pointer; the release seeks there */
static unsigned scrub_ms JT_DATA = 0;

static unsigned char *file JT_DATA = 0;   /* the whole song, on the heap */
static struct wav w JT_DATA;
static struct mp3 mp JT_DATA;
#define ART 96
static unsigned *art JT_DATA = 0;        /* the playing song's cover, decoded once at load, on the heap (36 KB is too much for the image) */
static int have_art JT_DATA = 0;
static int is_mp3 JT_DATA; /* the open song is an MP3: w.rate and w.frames mirror it so the time math is one path */
static unsigned char chunk[CHUNK] JT_DATA;
static unsigned base JT_DATA = 0;         /* frame the current stream started at */
static unsigned fed JT_DATA = 0;          /* frames queued since then */
static int stream_live JT_DATA = 0;       /* the queue has taken a byte of this stream, so played is ours */
static int sent_all JT_DATA = 0;          /* the last frame (with END) is queued */
static unsigned played JT_DATA = 0;       /* jt_audio_status().played, last read */
static unsigned queued_b JT_DATA = 0;     /* bytes waiting in the driver's ring after the last feed (the check reads it as `ahead`) */
static int pend JT_DATA = 0;              /* a stop is draining; finish it when the queue is idle */
static int pend_state JT_DATA = 0, pend_kind JT_DATA = 0, pend_final JT_DATA = 0;
static unsigned pend_pos JT_DATA = 0;
static unsigned last_log JT_DATA = 0;

/* layout, set by layout() from the window size */
static int lw JT_DATA, rx JT_DATA, rx1 JT_DATA, bar_y JT_DATA, ctl_y JT_DATA, vol_y JT_DATA;
static int play_cx JT_DATA, prev_cx JT_DATA, next_cx JT_DATA, shuf_x JT_DATA, rep_x JT_DATA, pill_y JT_DATA, vol_x0 JT_DATA, vol_x1 JT_DATA;
#define ROW_H  26
#define LIST_Y 46
#define FOOT_H 30   /* the key hint line at the bottom: no row may reach into it */
#define PILL_W_SHUF 84
#define PILL_W_REP  104

/* ---- small helpers ------------------------------------------------------------------------ */
static int utoa10(unsigned v, char *buf) {
    char tmp[12]; int tn = 0, n = 0;
    do { tmp[tn++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (tn) buf[n++] = tmp[--tn];
    buf[n] = 0;
    return n;
}
static void say(const char *a, const char *b, unsigned v, int with_num) { /* one line, one write: what the check reads */
    char line[96]; int l = 0;
    while (*a && l < 60) line[l++] = *a++;
    while (b && *b && l < 80) line[l++] = *b++;
    if (with_num) l += utoa10(v, line + l);
    line[l++] = '\n';
    jt_write(1, line, (unsigned)l);
}
static void say2(const char *a, unsigned v, const char *b, unsigned u) { /* "<a><v><b><u>" in one write */
    char line[96]; int l = 0;
    while (*a && l < 50) line[l++] = *a++;
    l += utoa10(v, line + l);
    while (*b && l < 70) line[l++] = *b++;
    l += utoa10(u, line + l);
    line[l++] = '\n';
    jt_write(1, line, (unsigned)l);
}
static void set_msg(const char *s) { int i = 0; while (s[i] && i < 63) { msg[i] = s[i]; i++; } msg[i] = 0; }
static unsigned xrand(void) { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; }
static void rect(int x, int y, int w_, int h_, unsigned c) {
    if (x < 0) { w_ += x; x = 0; }
    if (y < 0) { h_ += y; y = 0; }
    if (x + w_ > (int)win.width)  w_ = (int)win.width - x;
    if (y + h_ > (int)win.height) h_ = (int)win.height - y;
    for (int yy = 0; yy < h_; yy++) {
        unsigned *row = win.pixels + (unsigned)(y + yy) * win.width + (unsigned)x;
        for (int xx = 0; xx < w_; xx++) row[xx] = c;
    }
}
static unsigned mix(unsigned a, unsigned b) { /* half and half */
    return ((a >> 1) & 0x007F7F7F) + ((b >> 1) & 0x007F7F7F);
}
static void disc(int cx, int cy, int r, unsigned c) {
    for (int dy = -r; dy <= r; dy++) {
        int y = cy + dy;
        if (y < 0 || y >= (int)win.height) continue;
        for (int dx = -r; dx <= r; dx++) {
            int x = cx + dx, d2 = dx * dx + dy * dy;
            if (x < 0 || x >= (int)win.width || d2 > r * r + r) continue;
            unsigned *p = win.pixels + (unsigned)y * win.width + (unsigned)x;
            *p = d2 <= (r - 1) * (r - 1) ? c : mix(*p, c);
        }
    }
}
static void tri(int x, int cy, int tw, int th, int right, unsigned c) { /* flat triangle, tip on the right or left */
    for (int i = 0; i < tw; i++) {
        int half = th * (right ? tw - i : i + 1) / (2 * tw);
        rect(x + i, cy - half, 1, half * 2 + 1, c);
    }
}
static int text(const char *s, int x, int y, unsigned fg) { return jt_text_draw(&win, JT_FACE_BODY, x, y, fg, s); }
static int btext(const char *s, int x, int y, unsigned fg) { return jt_text_draw(&win, JT_FACE_BOLD, x, y, fg, s); }
static void ellipsize(char *s, int maxw) { /* in place, s is a local buffer of at least 56 bytes */
    int n = 0;
    while (s[n]) n++;
    if (jt_text_width(JT_FACE_BODY, s) <= maxw) return;
    while (n > 1) {
        s[--n] = 0;
        char t[60]; int k = 0;
        while (s[k] && k < 52) { t[k] = s[k]; k++; }
        t[k++] = '.'; t[k++] = '.'; t[k++] = '.'; t[k] = 0;
        if (jt_text_width(JT_FACE_BODY, t) <= maxw) { for (int i = 0; i <= k; i++) s[i] = t[i]; return; }
    }
}
static void fmt_time(unsigned ms, char *out) { /* m:ss */
    unsigned s = ms / 1000, n = 0;
    n += (unsigned)utoa10(s / 60, out);
    out[n++] = ':';
    out[n++] = (char)('0' + (s % 60) / 10);
    out[n++] = (char)('0' + s % 10);
    out[n] = 0;
}
static void title_of(const struct track *t, char *out) { /* an ID3 title as it is, else ALPHA.WAV -> Alpha */
    int i = 0;
    if (t->title[0]) { while (t->title[i] && i < 52) { out[i] = t->title[i]; i++; } out[i] = 0; return; }
    while (t->file[i] && t->file[i] != '.' && i < 24) {
        char c = t->file[i];
        if (i > 0 && c >= 'A' && c <= 'Z') c = (char)(c + 32);
        out[i] = c; i++;
    }
    out[i] = 0;
}
static int ext_is(const char *f, const char *e) { /* case-blind test: ext_is("A.WAV", "WAV") */
    int n = 0, m = 0;
    while (f[n]) n++;
    while (e[m]) m++;
    if (n < m + 1) return 0;
    for (int i = 0; i < m; i++) {
        char a = f[n - m + i]; if (a >= 'a' && a <= 'z') a = (char)(a - 32);
        if (a != e[i]) return 0;
    }
    return f[n - m - 1] == '.';
}

/* ---- the library ------------------------------------------------------------------------- */
static void scan_dir(const char *dir, int dirid) {
    int n = jt_readdir(dir, ents, JT_READDIR_MAX);
    if (n < 0) return;
    if (n > JT_READDIR_MAX) n = JT_READDIR_MAX;
    for (int i = 0; i < n && ntracks < MAX_TRACKS; i++) {
        if (ents[i].is_dir) continue;
        if (!ext_is(ents[i].name, "WAV") && !ext_is(ents[i].name, "MP3")) continue;
        struct track *t = &tracks[ntracks++];
        int k = 0;
        while (ents[i].name[k] && k < JT_DIRENT_NAME - 1) { t->file[k] = ents[i].name[k]; k++; }
        t->file[k] = 0; t->dir = (unsigned char)dirid; t->size = ents[i].size; t->title[0] = t->artist[0] = 0; t->ms = 0;
    }
}
static void probe(struct track *t);
static int title_cmp(const struct track *a, const struct track *b) { /* case-blind, by the shown title */
    char x[56], y[56]; title_of(a, x); title_of(b, y);
    for (int i = 0;; i++) {
        int c = x[i], d = y[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if (d >= 'A' && d <= 'Z') d += 32;
        if (c != d || !c) return c - d;
    }
}
static void scan_library(void) {
    ntracks = 0;
    scan_dir("", 0);
    scan_dir("MUSIC", 1);
    for (int i = 0; i < ntracks; i++) probe(&tracks[i]);
    for (int i = 1; i < ntracks; i++) { /* insertion sort by title, as iTunes lists them: a stable order for the check and the eye */
        struct track t = tracks[i]; int j = i - 1;
        while (j >= 0 && title_cmp(&tracks[j], &t) > 0) { tracks[j + 1] = tracks[j]; j--; }
        tracks[j + 1] = t;
    }
    say("music: library n=", 0, (unsigned)ntracks, 1);
}

/* ---- decoders ---------------------------------------------------------------------------
   WAV through libjt/wav, MP3 through libjt/mp3 (minimp3 on a 64KB heap stack of its own, since a ring-3
   program has a 4KB one). Either way the rest of the player sees w.rate and w.frames and src_read8. */
static void close_song(void) { if (is_mp3) mp3_close(&mp); is_mp3 = 0; }
static int open_mp3(const unsigned char *buf, unsigned len) {
    if (mp3_open(&mp, buf, len) != MP3_OK) return -1;
    w.rate = mp.rate; w.frames = mp.frames; w.channels = 1; w.bits = 8; w.pcm = 0;
    is_mp3 = 1;
    return 0;
}
static unsigned src_read8(unsigned from, unsigned char *out, unsigned n) {
    if (!is_mp3) return wav_read8(&w, from, out, n);
    if (from != mp.next && mp3_seek(&mp, from) != MP3_OK) return 0;   /* only a seek or a restart lands here */
    return mp3_read8(&mp, out, n);
}

static void track_path(const struct track *t, char *path) {
    int l = 0;
    if (t->dir) { const char *d = "MUSIC/"; while (*d) path[l++] = *d++; }
    for (int i = 0; t->file[i] && l < JT_PATH_MAX; i++) path[l++] = t->file[i];
    path[l] = 0;
}
static void probe(struct track *t) { /* the start of a song is enough for its title, artist and length: 8.3 names are all FAT shows, the tag has the real ones */
    static unsigned char head[2048] JT_DATA;
    char path[JT_PATH_MAX + 1];
    track_path(t, path);
    int got = jt_readfile(path, head, sizeof head);   /* a short read on purpose: open() refuses anything over 8KB, which is every song */
    if (got <= 0) return;
    if (ext_is(t->file, "MP3")) {
        if (mp3_id3(head, (unsigned)got, t->title, sizeof t->title, t->artist, sizeof t->artist)) { say("music: tag ", t->title, 0, 0); if (t->artist[0]) say("music: artist ", t->artist, 0, 0); }
        t->ms = mp3_probe_ms(head, (unsigned)got, t->size);
    } else t->ms = wav_probe_ms(head, (unsigned)got);
    {   char b[48]; int k = 0; while (t->file[k] && k < 40) { b[k] = t->file[k]; k++; } b[k++] = '='; b[k] = 0; say("music: length ", b, t->ms, 1); }
}
static int load_file(const struct track *t, unsigned char *buf, unsigned size) {
    char path[JT_PATH_MAX + 1];
    track_path(t, path);
    int r = jt_readfile(path, buf, size);
    if (r == -ENOSYS_) { /* a kernel without SYS_READFILE can still load a song open() fits */
        int fd = jt_open(path, 0);
        if (fd < 0) return fd;
        unsigned got = 0; int n;
        while (got < size && (n = jt_read(fd, buf + got, size - got > 255 ? 255 : size - got)) > 0) got += (unsigned)n;
        jt_close(fd);
        r = (int)got;
    }
    return r;
}

/* ---- playback ---------------------------------------------------------------------------- */
/* 32-bit math only (no libgcc here). Frames * 1000 does not fit in 32 bits for a song over 97 seconds at 44.1 kHz,
   and an MP3 of that length is well under MAX_BYTES, so split it into whole seconds and the remainder. */
static unsigned ms_of(unsigned fr) { return w.rate ? fr / w.rate * 1000u + fr % w.rate * 1000u / w.rate : 0; }
static unsigned frames_of(unsigned ms) { return ms / 1000u * w.rate + ms % 1000u * w.rate / 1000u; }
static unsigned total_ms(void) { return file ? ms_of(w.frames) : 0; }
static unsigned pos_frames(void) {
    unsigned p;
    if (!file) return 0;
    if (pend && !pend_final) p = pend_pos;
    else p = base + (state == ST_PLAYING && stream_live ? played : 0);
    return p > w.frames ? w.frames : p;
}
/* Stop the queue and, once it has drained, land in next_state at pos (or, final, where it stopped). */
static void drain(int next_state, int kind, unsigned pos, int final) {
    pend = 1; pend_state = next_state; pend_kind = kind; pend_pos = pos; pend_final = final;
    jt_audio_stop();
}
static void finalize(void) {
    unsigned pos = pend_final ? base + (stream_live ? played : 0) : pend_pos;
    if (pos > w.frames) pos = w.frames;
    base = pos; fed = 0; stream_live = 0; sent_all = 0; pend = 0;
    state = pend_state;
    if (pend_kind == K_PAUSE) say("music: paused pos=", 0, ms_of(pos), 1);
    else if (pend_kind == K_SEEK) say("music: seek pos=", 0, ms_of(pos), 1);
}
static void stop_all(void) {
    jt_audio_stop();
    state = ST_STOPPED; pend = 0; stream_live = 0; sent_all = 0; fed = 0; base = 0; played = 0;
}
/* ---- the list: which rows show, and following the selection ------------------------------- */
static int list_rows(void) { int r = ((int)win.height - LIST_Y - FOOT_H) / ROW_H; return r < 1 ? 1 : r; }
static void clamp_top(void) { int m = ntracks - list_rows(); if (m < 0) m = 0; if (top > m) top = m; if (top < 0) top = 0; }
static void follow(void) { /* scroll just enough that the selected row shows: only when the selection moves, never on a repaint, or the wheel could not leave it */
    int rows = list_rows();
    if (sel < top) top = sel;
    if (sel >= top + rows) top = sel - rows + 1;
    clamp_top();
}
static void heard_reset(void) { for (int i = 0; i < MAX_TRACKS; i++) heard[i] = 0; if (cur >= 0) heard[cur] = 1; }

/* Loads track i whole and starts it from the top. Returns 0, or leaves a message and stops. */
static int start_track(int i) {
    struct track *t = &tracks[i];
    stop_all();
    close_song();
    if (file) { free(file); file = 0; }
    cur = -1; sel = i; msg[0] = 0; tried = 1; heard[i] = 1; follow();
    if (t->size > MAX_BYTES || t->size == 0) {
        set_msg(t->size ? "Too big to play (3 MB is the most)" : "That file is empty");
        say("music: refused ", t->file, 0, 0);
        return -1;
    }
    file = (unsigned char *)malloc(t->size);
    if (!file) { set_msg("Not enough memory for that song"); say("music: refused ", t->file, 0, 0); return -1; }
    int n = load_file(t, file, t->size);
    if (n != (int)t->size) { set_msg("Could not read that file"); free(file); file = 0; say("music: unreadable ", t->file, 0, 0); return -1; }
    int r = ext_is(t->file, "MP3") ? open_mp3(file, t->size) : wav_open(&w, file, t->size);
    if (r != 0) {
        set_msg(ext_is(t->file, "MP3") ? "Not an MP3 this player can read" : "Not a WAV this player can read");
        free(file); file = 0; say("music: unsupported ", t->file, 0, 0);
        return -1;
    }
    cur = i;
    have_art = 0;
    {   unsigned off, len, aw, ah;
        if (!art) art = (unsigned *)malloc(ART * ART * 4);
        if (art && is_mp3 && mp3_id3_pic(file, t->size, &off, &len) && jpeg_decode_scaled32(file + off, len, art, ART, ART, &aw, &ah) == 0) {
            have_art = 1; say("music: art ", t->file, 0, 0);
        }
    }
    state = ST_PLAYING;
    say("music: playing ", t->file, 0, 0);
    say2("music: loaded total=", total_ms(), " rate=", w.rate);
    drain(ST_PLAYING, K_START, 0, 0); /* the queue may still be finishing the last song's chunk: wait for idle so played restarts at 0 */
    return 0;
}
static int pick_next(int forward, int auto_advance) { /* -1 when the list ends and repeat is off */
    if (ntracks == 0) return -1;
    if (shuffle && ntracks > 1) { /* a song at random from those not yet heard; when none are left the round is over */
        int left = 0;
        for (int i = 0; i < ntracks; i++) if (!heard[i]) left++;
        if (left == 0) {
            if (auto_advance && repeat == 0) return -1;
            heard_reset();                                  /* repeat all, or n pressed: a new round, not starting with this song */
            for (int i = 0; i < ntracks; i++) if (!heard[i]) left++;
        }
        int r = (int)(xrand() % (unsigned)left);
        for (int i = 0; i < ntracks; i++) if (!heard[i] && r-- == 0) return i;
    }
    /* cur is -1 before anything has played (n starts the highlighted song) and after a song that would not play (n moves on from it) */
    int k = cur < 0 && !tried ? sel : (cur < 0 ? sel : cur) + (forward ? 1 : -1);
    if (k >= ntracks) { if (auto_advance && repeat == 0) return -1; k = 0; }
    if (k < 0) k = ntracks - 1;
    return k;
}
static void seek_ms(int target) {
    if (!file) return;
    if (target < 0) target = 0;
    unsigned f = frames_of((unsigned)target);
    if (f >= w.frames) f = w.frames ? w.frames - 1 : 0;
    if (state == ST_PAUSED && !pend) { base = f; fed = 0; stream_live = 0; say("music: seek pos=", 0, ms_of(f), 1); return; }
    drain(state == ST_STOPPED ? ST_PAUSED : (pend ? pend_state : state), K_SEEK, f, 0);
}
static void toggle_play(void) {
    if (!file || state == ST_STOPPED) { if (ntracks) start_track(sel); return; }
    if (pend) { pend_state = pend_state == ST_PLAYING ? ST_PAUSED : ST_PLAYING; return; }
    if (state == ST_PLAYING) drain(ST_PAUSED, K_PAUSE, 0, 1);
    else { state = ST_PLAYING; fed = 0; stream_live = 0; sent_all = 0; say("music: resumed pos=", 0, ms_of(base), 1); }
}
static void go_next(int forward, int auto_advance) {
    if (ntracks == 0) return;
    if (!forward && file && ms_of(pos_frames()) > 3000) { seek_ms(0); return; } /* previous restarts a song that is well along */
    for (int tries = 0; tries < ntracks; tries++) {
        int k = pick_next(forward, auto_advance);
        if (k < 0) break;
        /* one song ending into the next skips a song that will not play (its reason stays on screen if none after it does); n and p stop on it */
        if (start_track(k) == 0 || !auto_advance) return;
    }
    stop_all(); say("music: ended list", 0, 0, 0);
}

/* One pass of the player: read the queue, finish a drain, feed more, notice the end. */
static void tick(void) {
    struct jt_audio_status as;
    queued_b = 0;
    if (jt_audio_status(&as) <= 0) { as.playing = 0; as.queued = 0; as.space = 0; as.played = played; }
    played = as.played;
    if (pend) { if (as.playing) return; finalize(); }
    if (state != ST_PLAYING || !file) return;
    if (stream_live && !as.playing && !sent_all) { base += fed; fed = 0; stream_live = 0; } /* underrun: fold what played into the base */
    unsigned lead = w.rate / 3;
    if (lead < LEAD_MIN) lead = LEAD_MIN;
    if (lead > LEAD_MAX) lead = LEAD_MAX;
    for (int rounds = 0; rounds < 2 && !sent_all; rounds++) {
        if (as.queued + 512 > lead) break;
        unsigned from = base + fed;
        if (from >= w.frames) { sent_all = 1; break; }
        unsigned n = w.frames - from < CHUNK ? w.frames - from : CHUNK;
        if (n > lead - as.queued) n = lead - as.queued;   /* top the queue up to the lead, not past it */
        if (n > as.space) n = as.space;
        if (n == 0) break;
        unsigned got = src_read8(from, chunk, n);
        if (got == 0) { sent_all = 1; break; }
        if (volume < 100) for (unsigned i = 0; i < got; i++) chunk[i] = (unsigned char)(128 + ((int)chunk[i] - 128) * volume / 100);
        int r = jt_audio_play(chunk, got, w.rate, from + got >= w.frames ? JT_AUDIO_END : 0);
        if (r < 0) { set_msg("No sound card found"); stop_all(); return; }
        if (r == 0) break;
        if (!stream_live) played = 0;
        fed += (unsigned)r; stream_live = 1; as.queued += (unsigned)r; as.space -= (unsigned)r;
        if (from + (unsigned)r >= w.frames) sent_all = 1;
    }
    queued_b = as.queued;
    if (sent_all && stream_live && !as.playing) { /* the whole song has been heard */
        say("music: ended ", tracks[cur].file, 0, 0);
        if (repeat == 2) start_track(cur);
        else go_next(1, 1);
    }
}

/* ---- drawing ----------------------------------------------------------------------------- */
static void layout(void) {
    int W = (int)win.width, H = (int)win.height;
    lw = W / 3; if (lw > 270) lw = 270; if (lw < 150) lw = 150;
    rx = lw + 28; rx1 = W - 28;
    bar_y = H - 140; ctl_y = H - 84; vol_y = H - 46;   /* the volume row clears the key hint under it */
    prev_cx = rx + 18; play_cx = rx + 18 + 62; next_cx = rx + 18 + 124;
    if (rx1 - rx >= 188 + PILL_W_SHUF + 8 + PILL_W_REP) { shuf_x = rx + 188; pill_y = ctl_y; }   /* beside the transport */
    else { shuf_x = rx; pill_y = bar_y - 44; }                                                    /* a half-screen window: their own row above the seek bar */
    rep_x = shuf_x + PILL_W_SHUF + 8;
    vol_x0 = rx + 76; vol_x1 = vol_x0 + 150;
    if (vol_x1 > rx1 - 50) vol_x1 = rx1 - 50;
}
static void pill(int x, int y, int pw, const char *label, int on) {
    rect(x, y, pw, 28, on ? ACCENT : TINT);
    rect(x, y, 2, 2, BG); rect(x + pw - 2, y, 2, 2, BG); rect(x, y + 26, 2, 2, BG); rect(x + pw - 2, y + 26, 2, 2, BG);
    int tw = jt_text_width(JT_FACE_BODY, label);
    text(label, x + (pw - tw) / 2, y + 5, on ? CREAM : INK);
}
static void draw(void) {
    int W = (int)win.width, H = (int)win.height;
    layout();
    rect(0, 0, W, H, BG);
    rect(lw + 12, 16, 1, H - 16 - FOOT_H - 2, RULE);   /* stops above the key hint, which runs under both panes */

    /* library */
    {   char hd[24] = "Library "; utoa10((unsigned)ntracks, hd + 8);
        btext(hd, 20, 14, INK);
        int rows = list_rows();
        clamp_top();
        if (ntracks == 0) {
            text("No songs yet", 20, LIST_Y + 6, INK);
            text("Put .WAV or .MP3", 20, LIST_Y + 34, HINT);
            text("songs in a MUSIC", 20, LIST_Y + 54, HINT);
            text("folder in Files.", 20, LIST_Y + 74, HINT);
        }
        for (int i = top; i < ntracks && i < top + rows; i++) {
            int y = LIST_Y + (i - top) * ROW_H;
            if (i == sel) rect(10, y, lw - 4, ROW_H - 2, TINT);
            char t[56]; title_of(&tracks[i], t);
            int dw = 0;
            if (tracks[i].ms) { char d[12]; fmt_time(tracks[i].ms, d); dw = jt_text_width(JT_FACE_BODY, d) + 8; text(d, lw + 6 - dw, y + 3, HINT); }
            ellipsize(t, lw - 56 - dw);
            if (i == cur && state == ST_PLAYING) tri(18, y + ROW_H / 2 - 1, 7, 10, 1, ACCENT);
            else if (i == cur && state == ST_PAUSED) { rect(18, y + 6, 2, 11, ACCENT); rect(22, y + 6, 2, 11, ACCENT); }   /* as wide as the triangle; stopped shows no mark, since bars would say paused */
            if (i == cur) btext(t, 30, y + 3, ACCENT); else text(t, 30, y + 3, INK);
        }
    }

    /* now playing: the cover on the left, the title, the artist and the state beside it, the way Apple Music's player reads */
    {   int ty = 52, tx = rx + ART + 22, tw = rx1 - tx;
        char t[56] = "Nothing playing";
        if (cur >= 0) title_of(&tracks[cur], t);
        else if (ntracks && sel < ntracks) title_of(&tracks[sel], t);
        ellipsize(t, tw);
        btext(t, tx, ty + 12, cur >= 0 ? INK : HINT);
        int ti = cur >= 0 ? cur : ntracks && sel < ntracks ? sel : -1;
        /* under the title: a message (accent), else the artist (with paused or stopped after it), else the state in words */
        char s[64]; int k = 0;
        const char *state_word = state == ST_PAUSED ? "Paused" : state == ST_STOPPED && cur >= 0 ? "Stopped" : 0;
        if (msg[0]) { while (msg[k] && k < 62) { s[k] = msg[k]; k++; } }
        else if (ti >= 0 && tracks[ti].artist[0]) {
            while (tracks[ti].artist[k] && k < 28) { s[k] = tracks[ti].artist[k]; k++; }
            if (state_word) { s[k++] = ','; s[k++] = ' '; for (int q = 0; state_word[q] && k < 62; q++) s[k++] = (char)(state_word[q] >= 'A' && state_word[q] <= 'Z' && q == 0 ? state_word[q] + 32 : state_word[q]); }
        } else {
            const char *w = state == ST_PLAYING ? "Playing" : state_word ? state_word : !ntracks ? "Add songs to begin" : "Press space to play";
            while (w[k] && k < 62) { s[k] = w[k]; k++; }
        }
        s[k] = 0;
        ellipsize(s, tw);
        text(s, tx, ty + 42, msg[0] ? ACCENT : HINT);
        if (have_art && cur >= 0) {   /* the cover */
            for (int yy = 0; yy < ART; yy++) for (int xx = 0; xx < ART; xx++) {
                int X = rx + xx, Y = ty + yy;
                if (X < (int)win.width && Y < (int)win.height) win.pixels[(unsigned)Y * win.width + (unsigned)X] = art[yy * ART + xx];
            }
        } else {
        /* a flat tile with two notes, no text: what shows when the song has no cover */
        rect(rx, ty, 96, 96, ACCENT);
        disc(rx + 30, ty + 66, 11, CREAM); disc(rx + 64, ty + 59, 11, CREAM);
        rect(rx + 38, ty + 22, 5, 44, CREAM); rect(rx + 72, ty + 16, 5, 43, CREAM);
        for (int i = 0; i < 39; i++) rect(rx + 38 + i, ty + 22 - i * 6 / 39, 1, 14, CREAM);
        }
    }

    /* seek bar and time */
    {   unsigned tot = total_ms(), pos = scrubbing ? scrub_ms : ms_of(pos_frames());
        char a[12], b[12]; fmt_time(pos, a); fmt_time(tot, b);
        rect(rx, bar_y, rx1 - rx, 6, RULE);
        int fill = tot ? (int)(pos * (unsigned)(rx1 - rx) / tot) : 0;
        if (fill > rx1 - rx) fill = rx1 - rx;
        rect(rx, bar_y, fill, 6, ACCENT);
        disc(rx + fill, bar_y + 3, 7, ACCENT);
        text(a, rx, bar_y + 14, HINT);
        text(b, rx1 - jt_text_width(JT_FACE_BODY, b), bar_y + 14, HINT);
    }

    /* transport */
    {   int cy = ctl_y + 14;
        rect(prev_cx - 10, cy - 8, 3, 16, INK); tri(prev_cx - 7, cy, 13, 16, 0, INK);
        rect(next_cx + 8, cy - 8, 3, 16, INK); tri(next_cx - 6, cy, 13, 16, 1, INK);
        disc(play_cx, cy, 24, ACCENT);
        if (state == ST_PLAYING) { rect(play_cx - 8, cy - 10, 6, 20, CREAM); rect(play_cx + 3, cy - 10, 6, 20, CREAM); }
        else tri(play_cx - 6, cy, 18, 22, 1, CREAM);
        pill(shuf_x, pill_y, PILL_W_SHUF, "Shuffle", shuffle);
        pill(rep_x, pill_y, PILL_W_REP, repeat == 0 ? "Repeat off" : repeat == 1 ? "Repeat all" : "Repeat one", repeat != 0);
    }

    /* volume */
    {   text("Volume", rx, vol_y, HINT);
        rect(vol_x0, vol_y + 8, vol_x1 - vol_x0, 5, RULE);
        int f = (vol_x1 - vol_x0) * volume / 100;
        rect(vol_x0, vol_y + 8, f, 5, ACCENT);
        disc(vol_x0 + f, vol_y + 10, 6, ACCENT);
        char v[8]; int n = utoa10((unsigned)volume, v); v[n++] = '%'; v[n] = 0;
        text(v, vol_x1 + 14, vol_y, HINT);
    }
    {   char h[64] = "space play  arrows seek and volume  n p song  s r modes";
        ellipsize(h, W - 40);
        text(h, 20, H - 24, HINT);
    }
}

/* ---- input ------------------------------------------------------------------------------- */
static void set_volume(int v) { v = v < 0 ? 0 : v > 100 ? 100 : v; if (v == volume) return; volume = v; say("music: vol=", 0, (unsigned)volume, 1); }
static void toggle_shuffle(void) { /* turning it on starts a round: the song playing now counts as heard */
    shuffle = !shuffle;
    if (shuffle) heard_reset();
    say("music: shuffle=", 0, (unsigned)shuffle, 1);
}
static int inside(int x, int y, int x0, int y0, int rw, int rh) { return x >= x0 && x < x0 + rw && y >= y0 && y < y0 + rh; }
static int on_seek_bar(int x, int y) { return inside(x, y, rx - 6, bar_y - 10, rx1 - rx + 12, 26); }
static int on_vol_bar(int x, int y) { return inside(x, y, vol_x0 - 8, vol_y - 4, vol_x1 - vol_x0 + 16, 24); }
static unsigned bar_ms(int x) { unsigned tot = total_ms(); int fx = x < rx ? 0 : x > rx1 ? rx1 - rx : x - rx; return (unsigned)fx * tot / (unsigned)(rx1 - rx); }
static int vol_of(int x) { int fx = x < vol_x0 ? 0 : x > vol_x1 ? vol_x1 - vol_x0 : x - vol_x0; return fx * 100 / (vol_x1 - vol_x0); }
static void drag(int x, int y) { /* the first step is where the press landed, which picks the bar; the later ones move its knob */
    layout();
    if (!drag_zone) drag_zone = on_seek_bar(x, y) ? 1 : on_vol_bar(x, y) ? 2 : 3;
    if (drag_zone == 1) { if (total_ms()) { scrubbing = 1; scrub_ms = bar_ms(x); } }
    else if (drag_zone == 2) set_volume(vol_of(x));
}
static void click(int x, int y) {
    layout();
    int cy = ctl_y + 14;
    int zone = drag_zone; drag_zone = 0;
    if (zone == 1) { scrubbing = 0; if (total_ms()) seek_ms((int)bar_ms(x)); return; }   /* the release ends a scrub at the knob, wherever the pointer went */
    if (zone == 2) { set_volume(vol_of(x)); return; }
    if (zone == 3) return;   /* a drag that began elsewhere is not a click on where it ended */
    if (x < lw + 12) { /* the library */
        int i = top + (y - LIST_Y) / ROW_H;
        if (y >= LIST_Y && (y - LIST_Y) / ROW_H < list_rows() && i >= 0 && i < ntracks) start_track(i);
        return;
    }
    if (on_seek_bar(x, y)) { if (total_ms()) seek_ms((int)bar_ms(x)); return; }
    if (inside(x, y, prev_cx - 14, cy - 18, 30, 36)) { go_next(0, 0); return; }
    if (inside(x, y, next_cx - 14, cy - 18, 30, 36)) { go_next(1, 0); return; }
    if (inside(x, y, play_cx - 26, cy - 26, 52, 52)) { toggle_play(); return; }
    if (inside(x, y, shuf_x, pill_y, PILL_W_SHUF, 28)) { toggle_shuffle(); return; }
    if (inside(x, y, rep_x, pill_y, PILL_W_REP, 28)) { repeat = (repeat + 1) % 3; say("music: repeat=", 0, (unsigned)repeat, 1); return; }
    if (on_vol_bar(x, y)) set_volume(vol_of(x));
}
/* Returns 1 to quit. */
static int key(int k) {
    if (k == '`') { jt_write(1, "music: crashing on purpose\n", 27); *(volatile int *)0 = 1; }
    if (k == JT_KEY_ESC) return 1;
    if (k == ' ' || k == JT_KEY_ENTER) toggle_play();
    else if (k == JT_KEY_RIGHT) seek_ms((int)ms_of(pos_frames()) + SEEK_MS);
    else if (k == JT_KEY_LEFT) seek_ms((int)ms_of(pos_frames()) - SEEK_MS);
    else if (k == JT_KEY_UP) set_volume(volume + 10);
    else if (k == JT_KEY_DOWN) set_volume(volume - 10);
    else if (k == 'n') go_next(1, 0);
    else if (k == 'p') go_next(0, 0);
    else if (k == 's') toggle_shuffle();
    else if (k == 'r') { repeat = (repeat + 1) % 3; say("music: repeat=", 0, (unsigned)repeat, 1); }
    return 0;
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open(&win) != 0 || !win.pixels) {
        jt_write(2, "music: no window\n", 17);
        jt_exit(1);
    }
    rng ^= (unsigned)jt_time(0);
    scan_library();
    layout();
    {   char line[48]; int l = 0; const char *p = "music: ui bar=";
        while (*p) line[l++] = *p++;
        l += utoa10((unsigned)rx, line + l); line[l++] = ',';
        l += utoa10((unsigned)rx1, line + l); line[l++] = ',';
        l += utoa10((unsigned)(bar_y + 3), line + l); line[l++] = '\n';
        jt_write(1, line, (unsigned)l);
    }
    draw();
    unsigned flags = JT_POLL_PRESENT, shown = 0xFFFFFFFFu;
    int dirty = 0;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        if (r == 1 && jt_window_resized(&ev, &win)) { layout(); follow(); say2("music: ui size=", win.width, "x", win.height); draw(); flags = JT_POLL_PRESENT; continue; }
        flags = 0;
        int st0 = state, cur0 = cur;
        tick();
        if (state != st0 || cur != cur0) dirty = 1;
        {   unsigned pm = ms_of(pos_frames()) / 250;   /* repaint the bar four times a second */
            if (pm != shown) { shown = pm; dirty = 1; }
            unsigned t = (unsigned)jt_time(0);          /* the serial line is paced by the wall clock; the position in it is not */
            if (t != last_log && file) {
                last_log = t;
                say2(state == ST_PLAYING ? "music: at playing pos=" : state == ST_PAUSED ? "music: at paused pos=" : "music: at stopped pos=",
                     ms_of(pos_frames()), " ahead=", w.rate ? queued_b * 1000u / w.rate : 0);
            }
        }
        if (r == -EAGAIN_ || r == 0) {
            if (dirty) { draw(); flags = JT_POLL_PRESENT; dirty = 0; }
            else jt_sched_yield();
            continue;
        }
        if (r != 1) break;
        if (ev.kind == JT_EV_KEY) { if (key(ev.a)) break; }
        else if (ev.kind == JT_EV_CLICK) click(ev.a, ev.b);
        else if (ev.kind == JT_EV_DRAG) drag(ev.a, ev.b);
        else if (ev.kind == JT_EV_WHEEL) { int t0 = top; top -= ev.a; clamp_top(); if (top != t0) say("music: list top=", 0, (unsigned)top, 1); }
        draw(); flags = JT_POLL_PRESENT; dirty = 0;
    }
    jt_audio_stop();
    jt_write(1, "music: closed\n", 14);
    jt_exit(0);
}
