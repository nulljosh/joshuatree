/* samantha: Samantha's chat window as a real ring-3 program (slice 2 of
 * "Samantha at ring 3" in docs/ARCHITECTURE.md).
 *
 * The look of kernel/chat.h: her name in mustard at the top, a conversation of
 * bubbles (you on the right, her on the left, newest at the bottom), and an
 * input line under it. Typing, Backspace, Enter sends, Esc closes, a click
 * never closes, and the backquote key is the deliberate crash. Enter asks
 * /api/pick first and then /api/chat, both through SYS_HTTP_POST, with the
 * request and reply shapes kernel/chat.h uses. Serial markers match the
 * kernel's: chatpick=, chatreply=, chatfail=.
 *
 * Face: the band under the title holds her animated face, the same real 320x320 JPEG frames the
 * kernel chat streams (/face/idle-N.jpg, /face/talk-N.jpg), fetched with SYS_HTTP_GET one frame
 * per idle poll so the window never blocks. 1.9.27: every JPEG (all 24 idle and 48 talk, about
 * 1.5MB) lives in the SYS_BRK heap through malloc, and the frame on screen is decoded on demand
 * by the kernel's own decoder (drivers/jpeg.c, built into libjt) straight to the 60x60 the
 * kernel's small face uses (FACE_SIDE in kernel/chat_face.h), RGB565. Idle loop while quiet,
 * talk frames while a reply shows.
 * Tools: a pick runs here (slices 3 and 4): reminders, calendar, mail and notes through the
 * file syscalls, weather through SYS_SYSINFO, "open <app>" through SYS_LAUNCH_REQUEST.
 * Type is the antialiased libjt face.
 */
#include "jtsys.h"
#include "libjt/text.h"
#include "libjt/stdlib.h"
/* decoder from drivers/jpeg.c, compiled into libjt (user/libjt/jpeg.c) */
int jpeg_decode_scaled(const unsigned char *data, unsigned int len, unsigned short *dst,
                       unsigned int dw, unsigned int dh, unsigned int *w, unsigned int *h);
#define UFACE_MAX 320     /* a whole source frame: portfolio mode draws it full screen */
static int uface_n JT_DATA = 60;  /* decode side: the small 60 px band, or 320 in portfolio mode */
#define UFACE_SIDE uface_n
#define UFACE_IDLE_N 24   /* every idle frame the kernel keeps */
#define UFACE_TALK_N 48   /* every talk frame */
#define UFACE_STRIDE 1
#define UFACE_FILE 32768  /* one JPEG fetch, real frames are ~21KB */

#define BG     0x00FAF8F6
#define INK    0x001C1C1E
#define DIM    0x0075726E
#define ACCENT 0x00B7862A
#define RULE   0x00E0D8CE
#define WHITE  0x00FFFFFF
#define YOUBG  0x00EDE6DC

#define NMSG 8          /* turns kept, oldest drop first (chat.h's CHAT_MAX) */
#define TXT 640         /* stored text per turn (chat.h's CHAT_CONTENT_MAX) */
#define INMAX 160
#define REQ 6144        /* SYS_HTTP_POST's 6 KB request cap, what chat.h used */
#define RESP 8192       /* and its 8 KB reply cap */
#define TREPLY 256      /* a tool's spoken reply, chat.h's tool_reply[256] */
#define REM_MAX 24
#define REM_TEXT 48
#define MAIL_MAX 24
#define MAIL_FROM 32
#define MAIL_SUBJ 48
#define MAIL_BODY 240
#define CAL_TEXT 40
#define FBUF 4096
#define FACE_H 64       /* band under the title the face lives in */
#define HEAD_H 36
#define INPUT_H 36
#define LINE 16

struct turn { int mine; char text[TXT]; };
struct mmsg { char from[MAIL_FROM], subj[MAIL_SUBJ], body[MAIL_BODY]; int read; };
struct arena {
    struct turn t[NMSG]; char in[INMAX + 1]; char req[REQ]; char resp[RESP];
    char fbuf[FBUF], one[1024], reply[TREPLY];
    char rtext[REM_MAX][REM_TEXT]; int rdone[REM_MAX]; int rcount;
    struct mmsg mail[MAIL_MAX]; int mcount;
    struct jt_dirent de[JT_READDIR_MAX];
};

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
extern char _user_end[];
static struct arena *ar JT_DATA = 0;
static int nturn JT_DATA = 0;
static int inlen JT_DATA = 0;
static const char *status JT_DATA = "ready";
static int face_at JT_DATA = 0;            /* idle loop position */
static int face_talk_at JT_DATA = 0;       /* talk frame on screen */
static unsigned face_next JT_DATA = 0;     /* tick of the next frame step */
static unsigned talk_until JT_DATA = 0;    /* tick the spoken reply is shown until */
static int face_inited JT_DATA = 0;
static unsigned char *uface_jpg[2][UFACE_TALK_N] JT_DATA;   /* [0]=idle [1]=talk, malloc'd JPEG bytes */
static unsigned uface_len[2][UFACE_TALK_N] JT_DATA;
static unsigned short *uface_cur JT_DATA = 0;                /* the one decoded frame, UFACE_SIDE^2 */
static int uface_cur_kind JT_DATA = -1, uface_cur_i JT_DATA = -1;
static unsigned char *uface_file JT_DATA = 0;
static int uface_idle_n JT_DATA = 0, uface_talk_n JT_DATA = 0, uface_done JT_DATA = 0;
static unsigned uface_open[UFACE_TALK_N] JT_DATA;
static unsigned last_talk JT_DATA = 0;   /* tick he last talked */
#define FACE_STEP 16                        /* ticks per cached frame (every 2nd source frame), ~12 fps of motion at 100 Hz */

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
static void text(const char *s, int x, int y, unsigned fg) { jt_text_draw(&win, JT_FACE_BODY, x, y, fg, s); }
static int tw(const char *s) { return jt_text_width(JT_FACE_BODY, s); }
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void serial(const char *tag, const char *s) {
    char b[320]; int l = 0;
    while (*tag && l < 40) b[l++] = *tag++;
    while (*s && l < 318) { char c = *s++; b[l++] = (c == '\n' || c == '\r') ? ' ' : c; }
    b[l++] = '\n';
    jt_write(1, b, (unsigned)l);
}

/* Greedy word wrap of s into lines of at most maxw pixels. Draws each line at
   (x, y + row * LINE) when draw is set; returns the line count. A word wider
   than a line is cut where it stops fitting. */
static int wrap_cap JT_DATA = 0;   /* when set, rows past this many are counted but not drawn */
static int wrap(const char *s, int maxw, int x, int y, unsigned fg, int draw) {
    char ln[260], w[82], t[264];
    int n = 0, rows = 0;
    ln[0] = 0;
#define FLUSH() do { if (draw && (!wrap_cap || rows < wrap_cap)) text(ln, x, y + rows * LINE, fg); rows++; n = 0; ln[0] = 0; } while (0)
    for (;;) {
        if (*s == '\n') { FLUSH(); s++; continue; }
        while (*s == ' ') s++;
        if (!*s) break;
        int wn = 0;
        while (s[wn] && s[wn] != ' ' && s[wn] != '\n' && wn < 80) { w[wn] = s[wn]; wn++; }
        w[wn] = 0;
        int tn = 0;
        for (int i = 0; i < n; i++) t[tn++] = ln[i];
        if (n) t[tn++] = ' ';
        for (int i = 0; i < wn; i++) t[tn++] = w[i];
        t[tn] = 0;
        if (n && tw(t) > maxw) { FLUSH(); continue; }
        if (!n && tw(w) > maxw) {
            int k = wn; while (k > 1) { w[k] = 0; if (tw(w) <= maxw) break; k--; }
            w[k] = 0; for (int i = 0; i <= k; i++) ln[i] = w[i];
            FLUSH(); s += k; continue;
        }
        for (int i = 0; i <= tn; i++) ln[i] = t[i];
        n = tn; s += wn;
    }
    if (n || !rows) FLUSH();
#undef FLUSH
    return rows;
}

static void push(int mine, const char *s) {
    if (nturn == NMSG) { for (int i = 1; i < NMSG; i++) ar->t[i - 1] = ar->t[i]; nturn--; }
    struct turn *t = &ar->t[nturn++];
    t->mine = mine;
    int n = 0; while (s[n] && n < TXT - 1) { t->text[n] = s[n]; n++; }
    t->text[n] = 0;
}

static unsigned now_ticks(void) { struct jt_tasks t; jt_tasks(&t, 0); return t.ticks; }

/* AUDIO SYNC hook (slice 6): true while her mouth should move. While a spoken
   reply is on the card, the mouth follows jt_audio_status (played samples);
   with no sound card (or no speech) a shown reply talks for a time
   proportional to its length, like before. */
static int spk_active JT_DATA = 0;           /* a reply is being fetched or played */
static int spk_card JT_DATA = 1;             /* 0 once the card said -ENODEV */
static unsigned spk_played JT_DATA = 0, spk_rate JT_DATA = 16000;
static int face_talking(unsigned now) {
    if (spk_active) return 1;
    return (int)(talk_until - now) > 0;
}

/* Draw one frame into the face band, centered under the title. */
static void face_blit(const unsigned short *f) {
    if (!f) return;
    int W = (int)win.width, x0 = (W - UFACE_SIDE) / 2, y0 = HEAD_H + 2;
    for (int y = 0; y < UFACE_SIDE; y++) {
        if (y0 + y >= (int)win.height) break;
        unsigned *row = win.pixels + (unsigned)(y0 + y) * win.width + (unsigned)x0;
        for (int x = 0; x < UFACE_SIDE && x0 + x < W; x++) {
            unsigned c = f[y * UFACE_SIDE + x];
            unsigned r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
            row[x] = (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
        }
    }
}

/* Portfolio: the 320 px frame fills the window height (square, centred; in a portrait window the
   sides crop), the rest is wall. Nearest source pixel with a half-step blend on each axis, so the
   1.7x stretch has no uneven doubled columns and costs a few ops a pixel. */
static unsigned short *fx_map JT_DATA = 0;   /* per dest column: source x in the low 15 bits, bit 15 = blend with x+1 */
static int fx_w JT_DATA = 0, fx_s JT_DATA = 0;
#define AVG565(a, b) ((unsigned short)((((a) & 0xF7DE) >> 1) + (((b) & 0xF7DE) >> 1)))
static unsigned px888(unsigned c) {
    unsigned r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
}
static void face_fill(const unsigned short *f) {
    int W = (int)win.width, H = (int)win.height, N = UFACE_SIDE, S = H, x0 = (W - S) / 2;
    if (!fx_map || fx_w != W || fx_s != S) {
        if (fx_map) free(fx_map);
        fx_map = (unsigned short *)malloc((unsigned)W * 2); fx_w = W; fx_s = S;
        if (!fx_map) return;
        for (int x = 0; x < W; x++) {
            int sx = (x - x0) * N * 2 / S;               /* half source pixels */
            if (sx < 0) sx = 0; if (sx > N * 2 - 2) sx = N * 2 - 2;
            fx_map[x] = (unsigned short)((sx >> 1) | ((sx & 1) ? 0x8000 : 0));
        }
    }
    for (int y = 0; y < H; y++) {
        int sy = y * N * 2 / S; if (sy > N * 2 - 2) sy = N * 2 - 2;
        const unsigned short *ra = f + (sy >> 1) * N, *rb = (sy & 1) ? ra + N : 0;
        unsigned *row = win.pixels + (unsigned)y * win.width, lc = px888(ra[1]), rc = px888(ra[N - 2]); /* the bars carry each row's own edge wall colour, so no seam */
        for (int x = 0; x < W; x++) {
            if (x < x0 || x >= x0 + S) { row[x] = x < x0 ? lc : rc; continue; }
            unsigned m = fx_map[x], i = m & 0x7FFF;
            unsigned a = ra[i];
            if (m & 0x8000) a = AVG565(a, ra[i + 1]);
            if (rb) { unsigned b = rb[i]; if (m & 0x8000) b = AVG565(b, rb[i + 1]); a = AVG565(a, b); }
            row[x] = px888(a);
        }
    }
}

/* Liquid glass: a rounded rect that lets the face show through, white wash at alpha/256, bright rim. */
static void glass(int x, int y, int w, int h, int r, int alpha, unsigned wash) {
    for (int dy = 0; dy < h; dy++) {
        int yy = y + dy; if (yy < 0 || yy >= (int)win.height) continue;
        int d = dy < r ? r - dy : (dy >= h - r ? dy - (h - 1 - r) : 0), inset = 0;
        if (d) { while (inset < r && (r - inset) * (r - inset) + d * d > r * r) inset++; }
        unsigned *row = win.pixels + (unsigned)yy * win.width;
        for (int xx = x + inset; xx < x + w - inset; xx++) {
            if (xx < 0 || xx >= (int)win.width) continue;
            unsigned c = row[xx], rim = (dy == 0 || dy == h - 1 || xx == x + inset || xx == x + w - inset - 1);
            unsigned a = rim ? alpha + 70 : (unsigned)alpha; if (a > 255) a = 255;
            unsigned rb = (((c & 0xFF00FF) * (256 - a) + (wash & 0xFF00FF) * a) >> 8) & 0xFF00FF;
            unsigned g = (((c & 0x00FF00) * (256 - a) + (wash & 0x00FF00) * a) >> 8) & 0x00FF00;
            row[xx] = rb | g;
        }
    }
}

/* Decode frame i of kind (0 idle, 1 talk) into uface_cur unless it is there already; returns it or 0. */
static const unsigned short *face_frame(int kind, int i) {
    if (!uface_cur || !uface_jpg[kind][i]) return 0;
    if (uface_cur_kind == kind && uface_cur_i == i) return uface_cur;
    unsigned w = 0, h = 0;
    if (jpeg_decode_scaled(uface_jpg[kind][i], uface_len[kind][i], uface_cur, UFACE_SIDE, UFACE_SIDE, &w, &h) != 0) return 0;
    uface_cur_kind = kind; uface_cur_i = i;
    return uface_cur;
}

/* Mouth contrast in the box where a portrait's mouth sits (centred, 58-74% down): the same measure
   the kernel face uses, here on the decoded 64x64 frame. Teeth by a dark gap = high variance. */
static unsigned face_openness(const unsigned short *f) {
    unsigned n = 0, sum = 0, sq = 0;
    for (int y = UFACE_SIDE * 58 / 100; y < UFACE_SIDE * 74 / 100; y++)
        for (int x = UFACE_SIDE * 40 / 100; x < UFACE_SIDE * 60 / 100; x++) {
            unsigned c = f[y * UFACE_SIDE + x];
            unsigned g = (((c >> 11) & 31) * 3 + ((c >> 5) & 63) * 2 + (c & 31) * 2) >> 3;
            sum += g; sq += g * g; n++;
        }
    unsigned m = sum / n;
    return sq / n - m * m;
}

/* Append the decimal of v (0 to 99) to d at n; returns the new length. */
static int face_num(char *d, int n, int v) {
    if (v >= 10) d[n++] = (char)('0' + v / 10);
    d[n++] = (char)('0' + v % 10);
    return n;
}

/* Fetch and decode ONE frame per call (idle first, then talk); runs from the idle poll. A frame that
   fails (404, dropped connection, bad JPEG) is tried once more on the next poll and then skipped: the
   clip goes on, so one flaky fetch never truncates her face. Loaded frames pack into the slots
   (uface_idle_n / uface_talk_n count them), uface_pos walks the clip itself. Only a dead host ends a
   clip early (FACE_DEAD_RUN skipped frames in a row, so no network costs 48 timeouts), and an idle clip
   with nothing in it means no face. The "face:" marker is written once when loading ends, as the
   kernel's chat_face_load does, and says how far each clip got and what was retried or skipped. */
#define FACE_DEAD_RUN 4
static void draw(void);
static int uface_full JT_DATA = 0;                /* a full-bleed face: portfolio mode and the phone's Samantha */
static int uface_portfolio JT_DATA = 0;         /* argv has "portfolio": Joshua's own face, landing/face-joshua/ */
static int uface_clip JT_DATA = 0;               /* 0 idle, then 1 talk */
static int uface_pos[2] JT_DATA;                 /* how far each clip got: frames tried */
static int uface_tries JT_DATA = 0;              /* failed attempts at the current frame */
static int uface_skipped JT_DATA = 0, uface_retried JT_DATA = 0, uface_run JT_DATA = 0;
static void face_load_finish(void) {
    uface_done = 1;
    char d[80]; int n = 0; const char *t = "face: idle=";
    while (*t) d[n++] = *t++;
    n = face_num(d, n, uface_idle_n); t = " talk="; while (*t) d[n++] = *t++;
    n = face_num(d, n, uface_talk_n); t = " end="; while (*t) d[n++] = *t++;
    n = face_num(d, n, uface_pos[0]); d[n++] = '/';
    n = face_num(d, n, uface_pos[1]); t = " skipped="; while (*t) d[n++] = *t++;
    n = face_num(d, n, uface_skipped); t = " retried="; while (*t) d[n++] = *t++;
    n = face_num(d, n, uface_retried); d[n++] = '\n'; jt_write(1, d, (unsigned)n);
    face_at = 0; face_talk_at = 0;
}
static void face_load_step(void) {
    if (uface_done) return;
    int clip = uface_clip, total = clip ? UFACE_TALK_N : UFACE_IDLE_N;
    int pos = uface_pos[clip], slot = clip ? uface_talk_n : uface_idle_n;
    char path[32]; int n = 0; const char *p = uface_portfolio ? "/face-joshua/" : "/face/";
    while (*p) path[n++] = *p++;
    p = clip ? "talk" : "idle"; while (*p) path[n++] = *p++;
    path[n++] = '-';
    n = face_num(path, n, pos * UFACE_STRIDE);
    p = ".jpg"; while (*p) path[n++] = *p++;
    path[n] = 0;
    int got = jt_http_get(path, uface_file, UFACE_FILE), ok = 0;
    unsigned w = 0, h = 0;
    if (got == -16) return;   /* -EBUSY: the desktop holds the one connection. Not this frame's fault, no retry spent, ask again next poll */
    uface_cur_kind = -1;   /* a bad JPEG can leave the decode buffer half written: it no longer holds any cached frame */
    if (got > 0 && uface_cur && jpeg_decode_scaled(uface_file, (unsigned)got, uface_cur, UFACE_SIDE, UFACE_SIDE, &w, &h) == 0 && w == 320 && h == 320) {
        unsigned char *keep = (unsigned char *)malloc((unsigned long)got);  /* heap, SYS_BRK */
        if (keep) {
            for (int k = 0; k < got; k++) keep[k] = uface_file[k];
            uface_jpg[clip][slot] = keep; uface_len[clip][slot] = (unsigned)got;
            uface_cur_kind = clip; uface_cur_i = slot;
            ok = 1;
        }
    }
    if (ok) {
        if (clip) { uface_open[slot] = face_openness(uface_cur); uface_talk_n++; } else uface_idle_n++;
        uface_tries = 0; uface_run = 0;
    } else if (uface_tries == 0) {
        uface_tries = 1; uface_retried++;   /* once more on the next poll */
        return;
    } else {
        uface_tries = 0; uface_skipped++; uface_run++;   /* give up on this frame, the clip goes on */
    }
    uface_pos[clip]++;
    if (uface_pos[clip] < total && uface_run < FACE_DEAD_RUN) return;   /* more of this clip to fetch */
    if (clip == 0 && uface_idle_n) { uface_clip = 1; uface_run = 0; if (uface_full) jt_write(1, "samface: idle ready\n", 20); return; }   /* idle in hand: on to the talk clip */
    face_load_finish();   /* talk clip done, or no idle frame at all (no face, talk is not tried) */
}

/* Advance the face one step when its tick comes due; returns 1 when it drew.
   Idle plays the loop forward; talking walks the talk clip forward and takes
   the next frame or the one after, whichever mouth is more open than the last
   (a stand-in for loudness until the AUDIO SYNC hook feeds a real level). */
static int face_step(unsigned now) {
    if (!face_inited) return 0;
    if (!uface_done) {
        if (inlen > 0) return 0;
        if (!spk_active) {   /* never download while she talks: a blocked app starves her voice */
            face_load_step(); if (uface_done) return 1;
            if (!uface_full || !uface_idle_n) return 0;
            face_at = 0; draw(); return 1;   /* a still, eyes-open portrait until every frame is in; a blink frozen by a download reads as a stall */
        }
    }
    if (!uface_idle_n) return 0;
    if ((int)(now - face_next) < 0) return 0;
    face_next = now + FACE_STEP;
    if (face_talking(now) && uface_talk_n) {
        if (spk_active && spk_card) {
            /* mouth time in ms = played * 1000 / rate; one cached frame per FACE_STEP*10 ms */
            unsigned ms = (spk_played / (spk_rate >= 1000 ? spk_rate / 1000 : 16u)); /* samples per ms */
            face_talk_at = (int)((ms / (FACE_STEP * 10u)) % (unsigned)uface_talk_n);
        } else {
            int a = (face_talk_at + 1) % uface_talk_n, b = (face_talk_at + 2) % uface_talk_n;
            face_talk_at = (uface_open[b] > uface_open[a] && (now / FACE_STEP) % 2) ? b : a;
        }
        if (uface_full) { draw(); return 1; }
        face_blit(face_frame(1, face_talk_at));
    } else {
        face_at = (face_at + 1) % uface_idle_n;
        if (uface_full) { draw(); return 1; }
        face_blit(face_frame(0, face_at));
    }
    return 1;
}

static unsigned fps_t0 JT_DATA = 0; static int fps_n JT_DATA = 0;
static unsigned *backbuf JT_DATA = 0; static int back_w JT_DATA = 0, back_h JT_DATA = 0;
static void draw_portfolio(void) {
    int W = (int)win.width, H = (int)win.height;
    { unsigned t = now_ticks(); fps_n++; if (!fps_t0) fps_t0 = t; if (t - fps_t0 >= 500) { char d[40] = "samface: draws="; int n = 15; n = face_num(d, n, fps_n); d[n++] = '\n'; jt_write(1, d, (unsigned)n); fps_t0 = t; fps_n = 0; } } /* serial: draws per 5 s */
    const unsigned short *f = uface_idle_n ? (face_talking(now_ticks()) && uface_talk_n ? face_frame(1, face_talk_at) : face_frame(0, face_at)) : 0;
    if (f) face_fill(f); else rect(0, 0, W, H, BG);
    int bw = W - 32 > 560 ? 560 : W - 32, bx = (W - bw) / 2, bh = 44, by = H - 24 - bh;
    if (f && (status[0] != 'r' || status[1] != 'e')) { int pw = tw(status) + 28; glass(16, 16, pw, 30, 15, 150, WHITE); text(status, 30, 23, INK); }
    /* His latest reply floats above the bar while he talks and for six seconds after, then the face is clear again.
       Your own message is not echoed: the bar holds it while you type, and the picture stays his. */
    unsigned now = now_ticks();
    if (face_talking(now)) last_talk = now;
    if (nturn > 0 && !ar->t[nturn - 1].mine && last_talk && (int)(now - last_talk) < 600) {
        int cw = bw - 40 > 480 ? 480 : bw - 40, rows = wrap(ar->t[nturn - 1].text, cw - 28, 0, 0, 0, 0);
        if (rows > 6) rows = 6;
        int h = rows * LINE + 22, y = by - 12 - h, cx = bx;
        glass(cx, y, cw, h, 16, 228, WHITE);
        wrap_cap = rows; wrap(ar->t[nturn - 1].text, cw - 28, cx + 14, y + 11, INK, 1); wrap_cap = 0;
    }
    glass(bx, by, bw, bh, 22, 228, WHITE);
    const char *shown = ar->in;
    while (*shown && tw(shown) > bw - 56) shown++;
    if (!*shown) text(uface_portfolio ? "Message Joshua" : "Message Samantha", bx + 24, by + 14, DIM);
    else text(shown, bx + 24, by + 14, INK);
    rect(bx + 24 + (*shown ? tw(shown) + 1 : 0), by + 12, 2, LINE + 4, ACCENT);
}

/* The picture is built off screen and copied to the window in one pass. Drawing straight into the window let the
   compositor grab a half-finished frame (face without the bar, bar without the bubble): torn, flickering bands. */
static void draw_portfolio_buffered(void) {
    unsigned *real = win.pixels; unsigned n = win.width * win.height;
    if (!backbuf || back_w != (int)win.width || back_h != (int)win.height) {
        if (backbuf) free(backbuf);
        backbuf = (unsigned *)malloc(n * 4); back_w = (int)win.width; back_h = (int)win.height;
    }
    if (!backbuf) { draw_portfolio(); return; }
    win.pixels = backbuf; draw_portfolio(); win.pixels = real;
    unsigned *d = real, *src = backbuf, c = n;
    __asm__ volatile ("rep movsl" : "+D"(d), "+S"(src), "+c"(c) : : "memory");
}

static void draw(void) {
    int W = (int)win.width, H = (int)win.height;
    if (uface_full) { draw_portfolio_buffered(); return; }
    rect(0, 0, W, H, BG);
    const char *who = uface_portfolio ? "Joshua" : "Samantha"; /* portfolio mode is his site: his name, his face */
    text(who, 20, 12, ACCENT);
    text(status, 20 + tw(who) + 16, 12, DIM);
    rect(20, HEAD_H, W - 40, 1, RULE);
    if (uface_idle_n) face_blit(face_talking(now_ticks()) && uface_talk_n ? face_frame(1, face_talk_at) : face_frame(0, face_at));
    int top = HEAD_H + FACE_H + 8, bottom = H - INPUT_H - 8;
    int bw = W - 40 - 80;  /* bubble text width: leaves a margin on the far side */
    if (bw > 360) bw = 360;
    int start = nturn, used = 0;
    for (int i = nturn - 1; i >= 0; i--) {
        int h = wrap(ar->t[i].text, bw - 20, 0, 0, 0, 0) * LINE + 14;
        if (used + h > bottom - top && i != nturn - 1) break;
        used += h + 6; start = i;
    }
    int y = top;
    if (!nturn) text(uface_portfolio ? "Say something to Joshua." : "Say something to Samantha.", 20, top + 4, DIM);
    for (int i = start; i < nturn; i++) {
        int rows = wrap(ar->t[i].text, bw - 20, 0, 0, 0, 0), h = rows * LINE + 14;
        int bx = ar->t[i].mine ? W - 20 - (bw + 0) : 20;
        if (y + h > bottom) h = bottom - y;
        rect(bx, y, bw, h, ar->t[i].mine ? YOUBG : WHITE);
        if (!ar->t[i].mine) { rect(bx, y, bw, 1, RULE); rect(bx, y + h - 1, bw, 1, RULE); rect(bx, y, 1, h, RULE); rect(bx + bw - 1, y, 1, h, RULE); }
        wrap(ar->t[i].text, bw - 20, bx + 10, y + 7, INK, 1);
        y += h + 6;
    }
    /* input line */
    int iy = H - INPUT_H;
    rect(20, iy, W - 40, INPUT_H - 8, WHITE);
    rect(20, iy, W - 40, 1, RULE); rect(20, iy + INPUT_H - 9, W - 40, 1, RULE);
    rect(20, iy, 1, INPUT_H - 8, RULE); rect(W - 21, iy, 1, INPUT_H - 8, RULE);
    const char *shown = ar->in;
    while (*shown && tw(shown) > W - 40 - 28) shown++; /* keep the tail visible */
    text(shown, 30, iy + 6, INK);
    rect(30 + tw(shown) + 1, iy + 6, 2, LINE, ACCENT);
}

/* The model reads a user turn as plain text, so only the JSON-significant bytes need escaping. */
static int esc(char *o, int n, int cap, const char *s) {
    for (; *s && n < cap - 6; s++) {
        char c = *s;
        if (c == '"' || c == '\\') { o[n++] = '\\'; o[n++] = c; }
        else if (c == '\n') { o[n++] = '\\'; o[n++] = 'n'; }
        else if ((unsigned char)c >= 32 && (unsigned char)c < 127) o[n++] = c;
    }
    return n;
}
static int cat(char *o, int n, int cap, const char *s) { while (*s && n < cap) o[n++] = *s++; return n; }

/* Value of the first "key":"..." string in a reply, unescaped; length, 0 when absent or not a string. */
static int extract(const char *j, const char *key, char *out, int cap) {
    int kl = slen(key);
    for (; *j; j++) {
        if (*j != '"') continue;
        int k = 0; while (k < kl && j[1 + k] == key[k]) k++;
        if (k != kl || j[1 + kl] != '"') continue;
        const char *p = j + 2 + kl;
        while (*p == ' ') p++;
        if (*p != ':') continue;
        p++; while (*p == ' ') p++;
        if (*p != '"') return 0;
        p++;
        int n = 0;
        while (*p && *p != '"' && n < cap - 1) {
            if (*p == '\\' && p[1]) {
                p++;
                if (*p == 'n') out[n++] = '\n';
                else if (*p == 'u') { p += 4; out[n++] = '?'; }
                else out[n++] = *p;
                p++;
            } else out[n++] = *p++;
        }
        out[n] = 0;
        return n;
    }
    return 0;
}

static int post(const char *path, int len, unsigned ticks) {
    struct jt_http_post a = { path, ar->req, (unsigned)len, ar->resp, RESP - 1, ticks };
    int r = jt_http_post(&a);
    if (r > 0) ar->resp[r] = 0;
    return r;
}

/* /api/pick: {"q":"...","sections":[],"os":"jt"} -> {"tool":..,"arg":..}. Returns 1 with tool set. */
static int pick(const char *msg, char *tool, int tsz, char *arg, int asz) {
    int n = cat(ar->req, 0, REQ, "{\"q\":\"");
    n = esc(ar->req, n, REQ, msg);
    n = cat(ar->req, n, REQ, "\",\"sections\":[],\"os\":\"jt\"}");
    tool[0] = arg[0] = 0;
    int r = post("/api/pick", n, 1000);
    if (r <= 0) { serial("chatpickfail=", r == -1 ? "connect" : "noreply"); return 0; }
    if (!extract(ar->resp, "tool", tool, tsz)) { serial("chatpick=", "none"); return 0; }
    extract(ar->resp, "arg", arg, asz);
    serial("chatpick=", tool);
    return 1;
}

/* /api/chat: the same body chat.h builds, the whole kept history. Returns 1 with the reply pushed. */
static int chat(void) {
    int n = 0, first = 0, size;
    for (;;) { /* drop the oldest turns until the escaped history fits the 6 KB-class budget this program keeps */
        size = 0;
        for (int i = first; i < nturn; i++) size += slen(ar->t[i].text) * 2 + 40;
        if (size < REQ - 120 || first == nturn - 1) break;
        first++;
    }
    n = cat(ar->req, n, REQ, "{\"model\":\"samantha\",");
    if (uface_portfolio) n = cat(ar->req, n, REQ, "\"persona\":\"joshua\","); /* answers as him: the worker's fixed lines and his doc pack */
    n = cat(ar->req, n, REQ, "\"stream\":false,\"think\":false,\"messages\":[");
    for (int i = first; i < nturn; i++) {
        if (i > first) ar->req[n++] = ',';
        n = cat(ar->req, n, REQ, "{\"role\":\"");
        n = cat(ar->req, n, REQ, ar->t[i].mine ? "user" : "assistant");
        n = cat(ar->req, n, REQ, "\",\"content\":\"");
        n = esc(ar->req, n, REQ, ar->t[i].text);
        n = cat(ar->req, n, REQ, "\"}");
    }
    n = cat(ar->req, n, REQ, "]}");
    int r = post("/api/chat", n, 4500);
    if (r == -1) { serial("chatfail=", "connect"); status = "error: couldn't reach the host"; return 0; }
    if (r <= 0) { if (r == -99 || r < -99) serial("chathttps=", "1"); serial("chatfail=", "noreply"); status = r < -99 ? "error: the host answered with an error" : "error: no reply"; return 0; }
    char ans[TXT];
    if (!extract(ar->resp, "content", ans, TXT)) { status = "error: no reply text"; return 0; }
    serial("chatreply=", ans);
    push(0, ans);
    return 1;
}

/* ---- TOOLS SLICE: chat_run_tool from kernel/chat.h, through the file syscalls ---- */

static void sertool(const char *tool, const char *res) {
    /* one write, one serial line: a split write lands as separate "syscall: write(1)" lines and the chattool= marker never reads whole */
    char b[320]; int l = 0; const char *t = "chattool=";
    while (*t) b[l++] = *t++;
    while (*tool && l < 60) b[l++] = *tool++;
    b[l++] = ':';
    while (*res && l < 318) { char c = *res++; b[l++] = (c == '\n' || c == '\r') ? ' ' : c; }
    b[l++] = '\n';
    jt_write(1, b, (unsigned)l);
}
static int streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int starts(const char *s, const char *p) { while (*p) if (*s++ != *p++) return 0; return 1; }
static void fmt(const char *prefix, const char *s) {
    int p = 0;
    for (; *prefix && p < TREPLY - 1; prefix++) ar->reply[p++] = *prefix;
    for (; *s && p < TREPLY - 1; s++) ar->reply[p++] = *s;
    ar->reply[p] = 0;
}
static void scopy(char *d, const char *s, int max) { int i = 0; while (s[i] && i < max - 1) { d[i] = s[i]; i++; } d[i] = 0; }

/* Whole file into buf (NUL terminated); length, or -1 when it does not exist. */
static int file_read(const char *path, char *buf, int cap) {
    int fd = jt_open(path, JT_O_RDONLY);
    if (fd < 0) return -1;
    int n = 0;
    while (n < cap - 1) { int r = jt_read(fd, buf + n, (unsigned)(cap - 1 - n)); if (r <= 0) break; n += r; }
    jt_close(fd);
    buf[n] = 0;
    return n;
}
static int file_write(const char *path, const char *buf, int n) {
    int fd = jt_open(path, JT_O_WRONLY | JT_O_CREAT | JT_O_TRUNC);
    if (fd < 0) return 0;
    int off = 0;
    while (off < n) { int r = jt_write(fd, buf + off, (unsigned)(n - off)); if (r <= 0) { jt_close(fd); return 0; } off += r; }
    jt_close(fd);
    return 1;
}

/* Reminders: REMINDERS.TXT, "<0/1> text" per line, read fresh and written whole. */
static void rem_load(void) {
    int n = file_read("REMINDERS.TXT", ar->fbuf, FBUF / 2);
    if (n < 0) n = 0;
    char *buf = ar->fbuf;
    int i = 0;
    ar->rcount = 0;
    while (i < n && ar->rcount < REM_MAX) {
        int ls = i;
        while (i < n && buf[i] != '\n') i++;
        int le = i;
        if (i < n) i++;
        if (le - ls < 3) continue;
        int j = 0;
        for (int k = ls + 2; k < le && j < REM_TEXT - 1; k++) ar->rtext[ar->rcount][j++] = buf[k];
        ar->rtext[ar->rcount][j] = 0;
        ar->rdone[ar->rcount] = (buf[ls] == '1');
        ar->rcount++;
    }
}
static void rem_save(void) {
    char *buf = ar->fbuf; int n = 0;
    for (int idx = 0; idx < ar->rcount; idx++) {
        buf[n++] = ar->rdone[idx] ? '1' : '0';
        buf[n++] = ' ';
        const char *s = ar->rtext[idx];
        while (*s && n < 2048 - 2) buf[n++] = *s++;
        buf[n++] = '\n';
    }
    file_write("REMINDERS.TXT", buf, n);
}

/* Mail: MAIL.TXT, "<r/u>|from|subject|body" per line; starter messages until the file exists. */
static void mail_load(void) {
    struct mmsg *m0 = &ar->mail[0], *m1 = &ar->mail[1];
    scopy(m0->from, "Joshua Tree", MAIL_FROM); scopy(m0->subj, "Welcome to Mail", MAIL_SUBJ);
    scopy(m0->body, "This is a real local inbox, no network behind it. Press enter to read a message, c to compose one, d to delete, esc to close.", MAIL_BODY); m0->read = 0;
    scopy(m1->from, "Joshua Tree", MAIL_FROM); scopy(m1->subj, "About this app", MAIL_SUBJ);
    scopy(m1->body, "Same shape as Notes and Reminders: everything here is written through to MAIL.TXT on the real FAT disk immediately, no Save button, no draft you can lose.", MAIL_BODY); m1->read = 0;
    ar->mcount = 2;
    char *buf = ar->fbuf;
    int n = file_read("MAIL.TXT", buf, FBUF);
    if (n < 0) return;
    ar->mcount = 0;
    int i = 0;
    while (i < n && ar->mcount < MAIL_MAX) {
        int is_read = (buf[i] == 'r');
        i += 2;
        struct mmsg *m = &ar->mail[ar->mcount];
        int j;
        j = 0; while (i < n && buf[i] != '|' && buf[i] != '\n' && j < MAIL_FROM - 1) m->from[j++] = buf[i++];
        m->from[j] = 0;
        while (i < n && buf[i] != '|' && buf[i] != '\n') i++;
        if (i < n && buf[i] == '|') i++;
        j = 0; while (i < n && buf[i] != '|' && buf[i] != '\n' && j < MAIL_SUBJ - 1) m->subj[j++] = buf[i++];
        m->subj[j] = 0;
        while (i < n && buf[i] != '|' && buf[i] != '\n') i++;
        if (i < n && buf[i] == '|') i++;
        j = 0; while (i < n && buf[i] != '\n' && j < MAIL_BODY - 1) m->body[j++] = buf[i++];
        m->body[j] = 0;
        while (i < n && buf[i] != '\n') i++;
        m->read = is_read;
        ar->mcount++;
        if (i < n && buf[i] == '\n') i++;
    }
}
static void mail_save(void) {
    char *buf = ar->fbuf; int n = 0;
    for (int idx = 0; idx < ar->mcount; idx++) {
        struct mmsg *m = &ar->mail[idx];
        buf[n++] = m->read ? 'r' : 'u';
        buf[n++] = '|';
        const char *s = m->from; while (*s && n < FBUF - 4) buf[n++] = *s++;
        buf[n++] = '|';
        s = m->subj;             while (*s && n < FBUF - 4) buf[n++] = *s++;
        buf[n++] = '|';
        s = m->body;             while (*s && n < FBUF - 2) buf[n++] = *s++;
        buf[n++] = '\n';
    }
    file_write("MAIL.TXT", buf, n);
}

/* Notes: NOTES/NOTES/N<7 digits>.TXT, flat NOTES.TXT where there are no folders (ramfs). */
static int notes_max_idx(int *ok) {
    int n = jt_readdir("NOTES/NOTES", ar->de, JT_READDIR_MAX);
    *ok = n >= 0;
    int mx = 0;
    for (int i = 0; i < n; i++) {
        const char *nm = ar->de[i].name;
        if (ar->de[i].is_dir || nm[0] == '.' || nm[0] != 'N') continue;
        int v = 0, k = 1; while (nm[k] >= '0' && nm[k] <= '9' && k < 9) v = v * 10 + (nm[k++] - '0');
        if (v > mx) mx = v;
    }
    return mx;
}
static void note_name(char *out, int v) {
    scopy(out, "NOTES/NOTES/N0000000.TXT", 32);
    for (int d = 18; d >= 12; d--) { out[d] = (char)('0' + v % 10); v /= 10; }
}

/* Days since 1970 to y/m/d (proleptic Gregorian). */
static void civil(unsigned days, int *y, int *m, int *d) {
    int z = (int)days + 719468;
    int era = z / 146097, doe = z - era * 146097;
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int yy = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = yy + (*m <= 2);
}

static const char *const APPNAME[] = {
    "Burrow", "Mail", "Calendar", "Notes", "Reminders", "Terminal", "Samantha", "Weather", "Curbfind", "Keyrate",
    "Bookrank", "Quotes", "Tonchi", "Toroid", "Hikko", "Fieldbook", "Contacts", "Calculator",
    "Stocks", "Search", "Epiphany", "Portfolio", "Activity", "Clock", "Music", "Movies", "Hamurapi", "Windgate",
};
#define NAPPS ((int)(sizeof APPNAME / sizeof APPNAME[0]))
#define HIKKO_SLOT 14 /* its index in APPNAME */
#define TONCHI_SLOT 12
#define HAMURAPI_SLOT 26

static int word_prefix_ci(const char *lbl, const char *s) {
    while (*lbl) {
        char a = *lbl, b = *s;
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
        lbl++; s++;
    }
    return *s == 0 || *s == ' ';
}
/* chat_match_app: whole-word, case-insensitive, "the " and " app" trimmed. Index into APPNAME or -1. */
static int match_app(const char *arg) {
    char buf[80]; int n = 0;
    for (const char *s = arg; *s && n < (int)sizeof(buf) - 1; s++) buf[n++] = *s;
    buf[n] = 0;
    char *b = buf;
    if ((b[0] | 32) == 't' && (b[1] | 32) == 'h' && (b[2] | 32) == 'e' && b[3] == ' ') b += 4;
    int bl = slen(b);
    if (bl > 4 && b[bl - 4] == ' ' && (b[bl - 3] | 32) == 'a' && (b[bl - 2] | 32) == 'p' && (b[bl - 1] | 32) == 'p') bl -= 4;
    b[bl] = 0;
    if ((b[0] | 32) == 'c' && (b[1] | 32) == 'h' && (b[2] | 32) == 'a' && (b[3] | 32) == 't' && !b[4]) b = "samantha";
    for (const char *w = b; ; w++) { /* Hikko was Sparkjar, then Hotaru: the old names still open it */
        if ((w == b || *(w - 1) == ' ') && (word_prefix_ci("sparkjar", w) || word_prefix_ci("hotaru", w))) return HIKKO_SLOT;
        if (!*w) break;
    }
    for (const char *w = b; ; w++) { /* Hamurapi was Hamurabi (the store name was taken): the old spelling still opens it */
        if ((w == b || *(w - 1) == ' ') && word_prefix_ci("hamurabi", w)) return HAMURAPI_SLOT;
        if (!*w) break;
    }
    for (const char *w = b; ; w++) { /* Tonchi was Lexly: the old name still opens it */
        if ((w == b || *(w - 1) == ' ') && word_prefix_ci("lexly", w)) return TONCHI_SLOT;
        if (!*w) break;
    }
    for (int i = 0; i < NAPPS; i++)
        for (const char *w = b; ; w++) {
            if ((w == b || *(w - 1) == ' ') && word_prefix_ci(APPNAME[i], w)) return i;
            if (!*w) break;
        }
    for (const char *w = b; ; w++) {
        if ((w == b || *(w - 1) == ' ') && (word_prefix_ci("files", w) || word_prefix_ci("file browser", w))) return 0;
        if (!*w) break;
    }
    return -1;
}

/* Returns 1 with ar->reply filled when the tool is handled here; 0 falls through to /api/chat.
   Same replies and chattool= markers as chat_run_tool in kernel/chat.h. */
static int run_tool(const char *tool, const char *arg, const char *said) {
    ar->reply[0] = 0;

    if (streq(tool, "new_reminder")) {
        rem_load();
        if (ar->rcount >= REM_MAX) { fmt("", "Reminders is full, nothing added."); sertool("new_reminder", "full"); return 1; }
        int idx = ar->rcount;
        scopy(ar->rtext[idx], arg, REM_TEXT);
        ar->rdone[idx] = 0;
        ar->rcount++;
        rem_save();
        fmt("Added reminder: ", arg);
        sertool("new_reminder", ar->rtext[idx]);
        return 1;
    }

    if (streq(tool, "list_reminders")) {
        rem_load();
        if (!ar->rcount) { fmt("", "No reminders."); sertool("list_reminders", "none"); return 1; }
        int p = 0, shown = 0;
        for (int i = 0; i < ar->rcount && p < TREPLY - 1; i++) {
            if (ar->rdone[i]) continue;
            if (shown) { ar->reply[p++] = ','; ar->reply[p++] = ' '; }
            for (const char *c = ar->rtext[i]; *c && p < TREPLY - 1; c++) ar->reply[p++] = *c;
            shown++;
        }
        ar->reply[p] = 0;
        if (!shown) fmt("", "No reminders.");
        sertool("list_reminders", shown ? ar->reply : "none");
        return 1;
    }

    if (streq(tool, "read_notes")) {
        char *buf = ar->fbuf; int n = 0, ok;
        int mx = notes_max_idx(&ok);
        if (ok) {
            for (int v = mx; v >= 1 && v > mx - 5 && n < FBUF - 2; v--) {
                char fn[32]; note_name(fn, v);
                int m = file_read(fn, ar->one, 1024);
                if (m <= 0) continue;
                while (m > 0 && (ar->one[m - 1] == '\n' || ar->one[m - 1] == ' ')) m--;
                if (n > 0) buf[n++] = ' ';
                for (int i = 0; i < m && n < FBUF - 2; i++) buf[n++] = ar->one[i] == '\n' ? ' ' : ar->one[i];
            }
        }
        if (n <= 0) n = file_read("NOTES.TXT", buf, FBUF);
        if (n <= 0) { fmt("", "No notes yet."); sertool("read_notes", "none"); return 1; }
        buf[n] = 0;
        fmt("", buf);
        sertool("read_notes", buf);
        return 1;
    }

    if (streq(tool, "new_note")) {
        if (starts(arg, "note:")) { arg += 5; while (*arg == ' ') arg++; }   /* "note: pick up dry cleaning" is the note "pick up dry cleaning", not "note: ..." twice */
        char *buf = ar->fbuf;
        int wrote = 0, len = 0;
        jt_mkdir("NOTES"); jt_mkdir("NOTES/NOTES"); /* harmless when they exist; fails on ramfs, which has no folders */
        int ok, mx = notes_max_idx(&ok);
        if (ok) {
            while (arg[len] && len < FBUF - 2) { buf[len] = arg[len]; len++; }
            buf[len++] = '\n';
            char fn[32]; note_name(fn, mx + 1);
            wrote = file_write(fn, buf, len);
        }
        if (!wrote) {
            int n = file_read("NOTES.TXT", buf, FBUF - 1);
            if (n < 0) n = 0;
            if (n > 0 && buf[n - 1] != '\n' && n < FBUF - 1) buf[n++] = '\n';
            for (const char *s2 = arg; *s2 && n < FBUF - 2; s2++) buf[n++] = *s2;
            buf[n++] = '\n';
            file_write("NOTES.TXT", buf, n);
        }
        fmt("Noted: ", arg);
        sertool("new_note", arg);
        return 1;
    }

    if (streq(tool, "weather")) {
        struct jt_sysinfo si; si.wx_have = 0; si.wx_text[0] = 0;
        if (jt_sysinfo(&si) < (int)sizeof si) { si.wx_have = 0; si.wx_text[0] = 0; }
        si.wx_text[JT_WX_TEXT_MAX - 1] = 0;
        if (si.wx_have && si.wx_text[0]) fmt("", si.wx_text);
        else fmt("", "No weather reading yet, open Weather first.");
        sertool("weather", si.wx_have ? si.wx_text : "none");
        return 1;
    }

    if (streq(tool, "calendar_today")) {
        struct jt_sysinfo si; si.epoch = 0;
        jt_sysinfo(&si);
        int y, m, d; civil(si.epoch / 86400u, &y, &m, &d);
        char ds[11] = { (char)('0' + (y / 1000) % 10), (char)('0' + (y / 100) % 10), (char)('0' + (y / 10) % 10), (char)('0' + y % 10), '-',
                        (char)('0' + (m / 10) % 10), (char)('0' + m % 10), '-', (char)('0' + (d / 10) % 10), (char)('0' + d % 10), 0 };
        char *buf = ar->fbuf;
        int n = file_read("EVENTS.TXT", buf, FBUF);
        if (n < 0) n = 0;
        int i = 0, found = 0, ts = 0, tl = 0;
        while (i < n && !found) {
            int ds0 = i;
            while (i < n && buf[i] != '|' && buf[i] != '\n' && i - ds0 < 10) i++;
            int dl = i - ds0, same = (dl == 10);
            for (int k = 0; same && k < 10; k++) if (buf[ds0 + k] != ds[k]) same = 0;
            if (i < n && buf[i] == '|') i++;
            int t0 = i;
            while (i < n && buf[i] != '\n' && i - t0 < CAL_TEXT - 1) i++;
            if (same) { found = 1; ts = t0; tl = i - t0; }
            while (i < n && buf[i] != '\n') i++;
            if (i < n) i++;
        }
        if (found) { buf[ts + tl] = 0; fmt("Today: ", buf + ts); sertool("calendar_today", buf + ts); }
        else { fmt("", "Nothing on today's calendar"); sertool("calendar_today", "none"); }
        return 1;
    }

    if (streq(tool, "say")) { fmt("", arg); sertool("say", arg); return 1; }

    if (streq(tool, "open_app")) {
        int icon = match_app(arg);
        if (icon < 0) return 0;
        if (jt_launch(APPNAME[icon]) != 0) return 0;
        fmt("Opening ", APPNAME[icon]);
        sertool("open_app", APPNAME[icon]);
        return 1;
    }

    if (streq(tool, "read_mail")) {
        mail_load();
        int idx = -1;
        for (int i = ar->mcount - 1; i >= 0 && idx < 0; i--) {
            if (!arg[0]) { idx = i; break; }
            for (int k = 0; ar->mail[i].from[k] && idx < 0; k++) {
                int m = 0;
                while (arg[m] && ar->mail[i].from[k + m] && ((arg[m] | 32) == (ar->mail[i].from[k + m] | 32))) m++;
                if (!arg[m]) idx = i;
            }
        }
        if (idx < 0) { fmt(arg[0] ? "No mail from " : "", arg[0] ? arg : "Your inbox is empty."); sertool("read_mail", "none"); return 1; }
        struct mmsg *m = &ar->mail[idx];
        int p = 0;
        const char *parts[] = { "From ", m->from, ": ", m->subj, ". ", m->body };
        for (int k = 0; k < 6; k++) for (const char *c = parts[k]; *c && p < TREPLY - 1; c++) ar->reply[p++] = *c;
        ar->reply[p] = 0;
        if (!m->read) { m->read = 1; mail_save(); }
        sertool("read_mail", m->subj);
        return 1;
    }

    if (streq(tool, "send_mail")) {
        const char *body = said;
        for (const char *c = said; *c; c++) {
            if (*c == ':') { body = c + 1; break; }
            if (starts(c, " that ")) { body = c + 6; break; }
            if (starts(c, " saying ")) { body = c + 8; break; }
        }
        while (*body == ' ') body++;
        mail_load();
        if (ar->mcount >= MAIL_MAX) { fmt("", "Mail is full, nothing sent."); sertool("send_mail", "full"); return 1; }
        struct mmsg *m = &ar->mail[ar->mcount];
        char to[MAIL_FROM]; int t = 0;
        for (const char *c = "To "; *c; c++) to[t++] = *c;
        for (const char *c = arg; *c && *c != '|' && t < MAIL_FROM - 1; c++) to[t++] = *c;
        to[t] = 0;
        scopy(m->from, to, MAIL_FROM);
        scopy(m->subj, "From Samantha", MAIL_SUBJ);
        int b = 0;
        for (const char *c = body; *c && b < MAIL_BODY - 1; c++) if (*c != '|') m->body[b++] = *c;
        m->body[b] = 0;
        m->read = 1;
        ar->mcount++;
        mail_save();
        fmt("Wrote it to ", arg);
        sertool("send_mail", arg);
        return 1;
    }

    return 0; /* open_url, web_search, current_tab, screenshot, clipboard, set_volume, battery, list_dir, read_file, make_logo, music, timer, set_heading, scroll_to, theme, reset_page: not handled (chat.h does not either) */
}

static int has_word(const char *hay, const char *needle) {
    for (; *hay; hay++) if (starts(hay, needle)) return 1;
    return 0;
}
/* chat_keyword_fallback: only after /api/pick named nothing. */
static int keyword_fallback(const char *msg, char *tool, int toolsz) {
    char lower[256]; int n = 0;
    for (const char *s = msg; *s && n < (int)sizeof(lower) - 1; s++) { char c = *s; if (c >= 'A' && c <= 'Z') c += 32; lower[n++] = c; }
    lower[n] = 0;
    int asking = has_word(lower, "what") || has_word(lower, "read") || has_word(lower, "list");
    const char *t = 0;
    if (has_word(lower, "itinerary") || has_word(lower, "agenda") || has_word(lower, "schedule") || has_word(lower, "calendar") || has_word(lower, "my plans") || has_word(lower, "my day")) t = "calendar_today";
    else if (asking && has_word(lower, "note")) t = "read_notes";
    else if (asking && has_word(lower, "reminders")) t = "list_reminders";
    if (!t) return 0;
    scopy(tool, t, toolsz);
    return 1;
}


/* ---- AUDIO SLICES 6 and 7: speak her reply, hold F2 to talk ---- */

#define JT_HTTP_PATH_MAX 128
#define SPK_PCM 65536u        /* one /api/speak clip: SYS_HTTP_GET's 64 KB cap, 4 s at 16 kHz */
#define SPK_TEXT 640
#define SPK_RATE 16000u
#define REC_MAX 65536u        /* JT_POST_BIG's body cap (JT_HTTP_BIG_MAX): ~4 s at 16 kHz, 8-bit */
static unsigned char *spk_pcm JT_DATA = 0;
static unsigned spk_len JT_DATA = 0, spk_off JT_DATA = 0;
static char *spk_text JT_DATA = 0;
static int spk_pos JT_DATA = 0;
static unsigned char *rec_buf JT_DATA = 0;
static unsigned rec_n JT_DATA = 0;
static int rec_on JT_DATA = 0;
static unsigned rec_peak JT_DATA = 0;

static void put_num(char *o, int *n, int v) {
    char d[12]; int k = 0;
    if (v < 0) { o[(*n)++] = '-'; v = -v; }
    if (!v) d[k++] = '0';
    while (v) { d[k++] = (char)('0' + v % 10); v /= 10; }
    while (k) o[(*n)++] = d[--k];
}

/* chat.h's speak_text prints "speak: status=N bytes=M"; the facespeak and speaks checks read it. */
static void speak_line(int status, int bytes) {
    char b[48]; int n = 0;
    const char *h = "speak: status="; while (*h) b[n++] = *h++;
    put_num(b, &n, status);
    h = " bytes="; while (*h) b[n++] = *h++;
    put_num(b, &n, bytes > 0 ? bytes : 0);
    b[n++] = '\n';
    jt_write(1, b, (unsigned)n);
}

/* One serial line per message: how long the tool pick, the chat reply and the first sound took, in ms from Enter.
   The lag gets numbers before anything changes. Ticks are 10 ms. */
static unsigned vt_start JT_DATA = 0, vt_pick JT_DATA = 0, vt_chat JT_DATA = 0, vt_open JT_DATA = 0;
static void voicetime(void) {
    char b[96]; int n = 0;
    const char *h = "voicetime: pick="; while (*h) b[n++] = *h++;
    put_num(b, &n, (int)(vt_pick * 10)); h = "ms chat="; while (*h) b[n++] = *h++;
    put_num(b, &n, (int)(vt_chat * 10)); h = "ms firstaudio="; while (*h) b[n++] = *h++;
    put_num(b, &n, (int)((now_ticks() - vt_start) * 10)); h = "ms\n"; while (*h) b[n++] = *h++;
    jt_write(1, b, (unsigned)n);
    vt_open = 0;
}

static void speak_stop(void) {
    if (spk_active) jt_audio_stop();
    spk_active = 0; spk_len = spk_off = 0; spk_pos = 0;
    if (spk_text) spk_text[0] = 0;
}

static void speak_begin(const char *reply) {
    if (!spk_card || !reply || !reply[0]) return;
    if (!spk_pcm) spk_pcm = (unsigned char *)malloc(SPK_PCM);
    if (!spk_text) spk_text = (char *)malloc(SPK_TEXT);
    if (!spk_pcm || !spk_text) return;
    scopy(spk_text, reply, SPK_TEXT);
    spk_pos = 0; spk_len = spk_off = 0; spk_played = 0;
    spk_active = 1;
}

/* Next /api/speak?t=<percent-encoded sentence piece>: the whole path is capped at 128 bytes, so a
   piece is at most ~100 encoded bytes, cut at the last sentence end or space that fits. */
static int speak_fetch(void) {
    static const char hex[] = "0123456789ABCDEF";
    char path[JT_HTTP_PATH_MAX + 1];
    int n = 0;
    const char *h = uface_portfolio ? "/api/speak?v=joshua&t=" : "/api/speak?t="; while (*h) path[n++] = *h++;
    while (spk_text[spk_pos] == ' ') spk_pos++;
    if (!spk_text[spk_pos]) return 0;
    int start = spk_pos, enc = n, i = start, cut = -1, cutenc = n;
    while (spk_text[i]) {
        unsigned char c = (unsigned char)spk_text[i];
        int plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == ',';
        int w = plain ? 1 : 3;
        if (enc + w > JT_HTTP_PATH_MAX - 1) break;
        if (plain) path[enc++] = (char)c;
        else if (c == ' ') { path[enc++] = '%'; path[enc++] = '2'; path[enc++] = '0'; }
        else if (c < 32 || c >= 127) { path[enc++] = '%'; path[enc++] = '2'; path[enc++] = '0'; } /* control and high bytes become a space */
        else { path[enc++] = '%'; path[enc++] = hex[c >> 4]; path[enc++] = hex[c & 15]; }
        i++;
        if (c == '.' || c == '!' || c == '?') { cut = i; cutenc = enc; }
    }
    if (spk_text[i]) { /* the piece didn't reach the end of the text: back up to a sentence end, else the last space */
        if (cut < 0) { int j = i; while (j > start && spk_text[j - 1] != ' ') j--; if (j > start) { cut = j; cutenc = -1; } }
        if (cut >= 0) {
            if (cutenc >= 0) enc = cutenc;
            else { /* re-encode up to cut */
                enc = n;
                for (int q = start; q < cut; q++) {
                    unsigned char c = (unsigned char)spk_text[q];
                    int plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == ',';
                    if (plain) path[enc++] = (char)c;
                    else if (c < 32 || c >= 127 || c == ' ') { path[enc++] = '%'; path[enc++] = '2'; path[enc++] = '0'; }
                    else { path[enc++] = '%'; path[enc++] = hex[c >> 4]; path[enc++] = hex[c & 15]; }
                }
            }
            i = cut;
        }
    }
    path[enc] = 0;
    spk_pos = i;
    int got = jt_http_get(path, spk_pcm, SPK_PCM);
    speak_line(got >= 0 ? 200 : (got <= -100 ? -got : 0), got);
    if (got >= 64) { spk_len = (unsigned)got; spk_off = 0; return 1; }
    return 0; /* a 404 page or an error body is not sound: skip this piece */
}

/* Called every pass of the poll loop: never blocks on the card, retries when the ring is full.
   Returns 1 when it changed what the face should show. */
static int speak_tick(void) {
    if (!spk_active) return 0;
    struct jt_audio_status st;
    if (jt_audio_status(&st) >= 0) { spk_played = st.played; if (st.rate) spk_rate = st.rate; }
    if (spk_off < spk_len) {
        unsigned left = spk_len - spk_off, n = left > JT_AUDIO_CHUNK_MAX ? JT_AUDIO_CHUNK_MAX : left;
        int r = jt_audio_play(spk_pcm + spk_off, n, SPK_RATE, n == left ? JT_AUDIO_END : 0);
        if (r == 0) return 0; /* ring full: retry next pass */
        if (r < 0) { if (r == -19) spk_card = 0; speak_stop(); return 1; } /* -ENODEV: honest silence, the timer talks */
        spk_off += (unsigned)r;
        if (vt_open) voicetime();   /* the first chunk of this reply just reached the card */
        return 1;
    }
    if (st.playing || st.queued) return 0; /* the last clip is still sounding */
    if (spk_text[spk_pos] && speak_fetch()) return 1;
    if (!spk_text[spk_pos] && spk_off >= spk_len) { spk_active = 0; spk_len = spk_off = 0; return 1; }
    return 1;
}

static void listen_stop_and_send(void);
static void send(void);

static void listen_start(void) {
    if (rec_on) return; /* key auto-repeat */
    if (!rec_buf) rec_buf = (unsigned char *)malloc(REC_MAX);
    if (!rec_buf) { status = "out of memory"; return; }
    speak_stop();
    int r = jt_rec_start(16000);
    if (r < 0) { status = r == -19 ? "no sound card" : "mic busy, try again"; return; }
    rec_on = 1; rec_n = 0; rec_peak = 0;
    status = "listening ... (release F2 to send)";
}

static void listen_tick(void) {
    if (!rec_on) return;
    for (;;) {
        if (rec_n >= REC_MAX) break;
        unsigned want = REC_MAX - rec_n; if (want > JT_REC_CHUNK_MAX) want = JT_REC_CHUNK_MAX;
        int got = jt_rec_read(rec_buf + rec_n, want);
        if (got <= 0) break;
        for (int i = 0; i < got; i++) {
            int d = (int)rec_buf[rec_n + (unsigned)i] - 128; if (d < 0) d = -d;
            if ((unsigned)d > rec_peak) rec_peak = (unsigned)d;
        }
        rec_n += (unsigned)got;
    }
    if (rec_n >= REC_MAX) listen_stop_and_send(); /* the clip cap reached: send what fits */
}

static void listen_stop_and_send(void) {
    if (!rec_on) return;
    for (int tries = 0; tries < 4 && rec_n < REC_MAX; tries++) { /* bank whatever was still landing */
        unsigned want = REC_MAX - rec_n; if (want > JT_REC_CHUNK_MAX) want = JT_REC_CHUNK_MAX;
        int got = jt_rec_read(rec_buf + rec_n, want);
        if (got <= 0) break;
        rec_n += (unsigned)got;
    }
    jt_rec_stop();
    rec_on = 0;
    if (!rec_n) { status = "heard nothing"; serial("listen: ", "nothing recorded"); return; } /* QEMU's SB16 records nothing: the honest path */
    status = "listening to you ...";
    draw();
    struct jt_event dummy; jt_window_poll(&dummy, JT_POLL_PRESENT);
    struct jt_http_post a = { "/api/listen", (const char *)rec_buf, rec_n, ar->resp, RESP - 1, 3000 };
    int r = jt_http_post_ex(&a, JT_POST_WORKER | JT_POST_BIG); /* /api/listen lives on the Worker, not Turing */
    if (r <= 0) { status = "couldn't hear that"; serial("listen: ", "post failed"); return; }
    ar->resp[r] = 0;
    char text[INMAX + 1];
    if (!extract(ar->resp, "text", text, sizeof text) || !text[0]) { status = "didn't catch that"; return; }
    serial("listen: ", text);
    scopy(ar->in, text, INMAX + 1); inlen = slen(ar->in);
    send();
}

static void send(void) {
    char msg[INMAX + 1], tool[24], arg[128];
    for (int i = 0; i <= inlen; i++) msg[i] = ar->in[i];
    if (!inlen) return;
    push(1, msg);
    inlen = 0; ar->in[0] = 0;
    status = "checking for a tool ...";
    vt_start = now_ticks(); vt_pick = vt_chat = 0; vt_open = 1;
    draw();
    struct jt_event dummy; jt_window_poll(&dummy, JT_POLL_PRESENT); /* mark dirty so the desktop paints before the wait */
    int picked = pick(msg, tool, sizeof tool, arg, sizeof arg);
    vt_pick = now_ticks() - vt_start;
    if (!picked) { arg[0] = 0; picked = keyword_fallback(msg, tool, sizeof tool); }
    if (picked && run_tool(tool, arg, msg)) {
        push(0, ar->reply);
        talk_until = now_ticks() + 100u * 2u + 6u * (unsigned)slen(ar->reply);
        status = "ready";
        speak_begin(ar->reply);
        return;
    }
    status = "generating ...";
    draw();
    jt_window_poll(&dummy, JT_POLL_PRESENT);
    int chatted = chat();
    vt_chat = now_ticks() - vt_start - vt_pick;
    if (chatted) {
        status = "ready";
        talk_until = now_ticks() + 100u * 2u + 6u * (unsigned)slen(ar->t[nturn - 1].text);
        speak_begin(ar->t[nturn - 1].text);
    }
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    for (int i = 1; i < argc; i++) { const char *a = argv[i], *b = "portfolio"; while (*a && *a == *b) { a++; b++; } if (!*a && !*b) uface_portfolio = 1; }
    for (int i = 1; i < argc; i++) { const char *a = argv[i], *b = "phone"; while (*a && *a == *b) { a++; b++; } if (!*a && !*b) uface_full = 1; }   /* the phone's Samantha is full bleed too */
    if (uface_portfolio) uface_full = 1;
    if (uface_full) uface_n = UFACE_MAX;
    if (jt_window_open(&win) != 0 || !win.pixels) { jt_write(2, "samantha: no window\n", 20); jt_exit(1); }
    ar = (struct arena *)malloc(sizeof *ar);              /* 1.9.27: the heap, not the image window */
    if (!ar) { jt_write(2, "samantha: no heap\n", 18); jt_exit(1); }
    for (unsigned k = 0; k < sizeof *ar; k++) ((unsigned char *)ar)[k] = 0;
    uface_file = (unsigned char *)malloc(UFACE_FILE);
    uface_cur = (unsigned short *)malloc((unsigned)(UFACE_SIDE * UFACE_SIDE * 2));
    face_inited = uface_file && uface_cur;
    draw();
    jt_write(1, "samantha: ring-3 window\n", 24);
    jt_write(1, "samopen\n", 8); jt_write(1, "samfocus\n", 9);  /* the markers samantha-boot-check reads: view open, input box drawn and focused */
    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        if (r == 1 && jt_window_resized(&ev, &win)) { draw(); flags = JT_POLL_PRESENT; continue; } /* JT_EV_RESIZE: remapped, repaint at the new size */
        flags = 0;
        if (r == -11) {
            /* idle: step the face on its tick, otherwise just yield (no spin on redraws) */
            listen_tick();
            if (speak_tick()) flags = JT_POLL_PRESENT;
            if (face_step(now_ticks())) flags = JT_POLL_PRESENT;
            if (rec_on) flags = JT_POLL_PRESENT;
            jt_sched_yield(); continue;
        }
        if (r != 1) break;
        if (ev.kind == JT_EV_KEY) {
            int k = ev.a;
            if (k == '`') { jt_write(1, "samantha: crashing on purpose\n", 30); *(volatile int *)0 = 1; }
            if (k == JT_KEY_ESC) break;
            else if (k == JT_KEY_F2) listen_start();
            else if (k == JT_KEY_F2_UP) listen_stop_and_send();
            else if (k == JT_KEY_COPY || k == JT_KEY_CUT) { jt_clip_set(ar->in, (unsigned)inlen); if (k == JT_KEY_CUT) { inlen = 0; ar->in[0] = 0; } } /* the system clipboard */
            else if (k == JT_KEY_PASTE) {
                char pb[INMAX + 1];
                int n = INMAX > inlen ? jt_clip_get(pb, (unsigned)(INMAX - inlen)) : 0;
                for (int i = 0; i < n; i++) if (pb[i] >= 32 && pb[i] < 127) ar->in[inlen++] = pb[i];
                ar->in[inlen] = 0;
            }
            else if (spk_active && k != JT_KEY_ENTER) speak_stop(); /* typing skips the rest of her reply */
            else if (k == JT_KEY_ENTER) { speak_stop(); send(); }
            else if (k == 8) { if (inlen > 0) ar->in[--inlen] = 0; }
            else if (k >= 32 && k < 127 && inlen < INMAX) { ar->in[inlen++] = (char)k; ar->in[inlen] = 0; }
        } else { flags = JT_POLL_PRESENT; continue; } /* clicks and wheel never close and change nothing */
        draw();
        flags = JT_POLL_PRESENT;
    }
    jt_write(1, "samantha: closed\n", 17);
    jt_exit(0);
}
