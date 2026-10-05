/* samantha: Samantha's chat window as a real ring-3 program (slice 2 of
 * "Samantha at ring 3" in docs/ARCHITECTURE.md).
 *
 * Full screen (2.9.0): her portrait (/face/hd.jpg, 736 px) fills the window, with room around her in a wide one (2.12.1);
 * the input line is a blurred glass panel; what you say and what she answers appear as captions over the picture that fade out after her voice ends;
 * Tab opens the whole conversation on glass; a red dot or Esc closes her. Her mouth is drawn by code from the
 * loudness of the PCM she is playing (vm_mouth). Enter asks /api/pick first and then /api/chat, both through
 * SYS_HTTP_POST, with the request and reply shapes kernel/chat.h used. Serial markers: chatpick=, chatreply=,
 * chatfail=, samtyped= (what the keyboard handed her) and samface: (what the checks read).
 *
 * Face: a host without the portrait leaves the old frame set, the real 320x320 JPEG frames
 * (/face/idle-N.jpg, /face/talk-N.jpg) fetched with SYS_HTTP_GET one per idle poll and scaled up; every JPEG
 * lives in the SYS_BRK heap and the frame on screen is decoded on demand by drivers/jpeg.c (built into libjt).
 * Tools: a pick runs here (slices 3 and 4): reminders, calendar, mail and notes through the
 * file syscalls, weather through SYS_SYSINFO, "open <app>" through SYS_LAUNCH_REQUEST.
 * Type is the antialiased libjt face.
 */
#include "jtsys.h"
#include "libjt/text.h"
#include "libjt/stdlib.h"
/* decoder from drivers/jpeg.c, compiled into libjt (user/libjt/jpeg.c) */
int jpeg_decode_scaled32(const unsigned char *data, unsigned int len, unsigned int *dst,
                         unsigned int dw, unsigned int dh, unsigned int *w, unsigned int *h);
int jpeg_decode_scaled(const unsigned char *data, unsigned int len, unsigned short *dst,
                       unsigned int dw, unsigned int dh, unsigned int *w, unsigned int *h);
#define UFACE_MAX 320     /* a whole source frame: portfolio mode draws it full screen */
static int uface_n JT_DATA = 60;  /* decode side: the small 60 px band, or 320 in portfolio mode */
#define UFACE_SIDE uface_n
#define UFACE_IDLE_N 24   /* every idle frame the kernel keeps */
#define UFACE_TALK_N 48   /* every talk frame */
#define UFACE_STRIDE 1
#define UFACE_FILE 65536  /* one JPEG fetch: a frame is ~21KB, the portrait ~59KB, SYS_HTTP_GET carries 64KB */

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
#define LINE 16

struct turn { int mine; char text[TXT]; };
#define CAP_HOLD_USER 220      /* ticks (10 ms) your own caption holds after you send */
#define CAP_HOLD_AFTER 180     /* ticks her caption holds after her voice ends */
#define CAP_IN 12              /* fade in */
#define CAP_OUT 60             /* fade out: 0.6 s */
#define CAP_ALPHA 150          /* backdrop black at 150/256 */
#define NCAP 2          /* captions over the picture: yours, then hers */
struct cap { int mine, live; unsigned born, ends; char text[TXT]; };
struct mmsg { char from[MAIL_FROM], subj[MAIL_SUBJ], body[MAIL_BODY]; int read; };
struct arena {
    struct turn t[NMSG]; struct cap cap[NCAP]; char in[INMAX + 1]; char req[REQ]; char resp[RESP];
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
static int wrap_ell JT_DATA = 0; static char ell_buf[264] JT_DATA;   /* wrap_ell with wrap_cap: the last row drawn ends in "..." (the text goes on) */
static const char *ellipsize(const char *ln, int maxw) {   /* ln then "...", cut back (to a word if it can) until the row fits maxw */
    int len = slen(ln), n = len > 258 ? 258 : len, k;
    for (;; n--) {
        for (int i = 0; i < n; i++) ell_buf[i] = ln[i];
        while (n > 0 && ell_buf[n - 1] == ' ') n--;
        ell_buf[n] = ell_buf[n + 1] = ell_buf[n + 2] = '.'; ell_buf[n + 3] = 0;
        if (n == 0 || tw(ell_buf) <= maxw) break;
    }
    if (n < len && ln[n] != ' ') {   /* it cut a word: step back to the space before it, unless that is most of the row */
        for (k = n; k > 0 && ln[k - 1] != ' '; k--) ;
        while (k > 0 && ln[k - 1] == ' ') k--;
        if (k > n / 2) { ell_buf[k] = ell_buf[k + 1] = ell_buf[k + 2] = '.'; ell_buf[k + 3] = 0; }
    }
    return ell_buf;
}
static int wrap_y0 JT_DATA = 0, wrap_y1 JT_DATA = 0;   /* when set, rows outside [y0, y1) are counted but not drawn (the scrollback's clip) */
static int wrap(const char *s, int maxw, int x, int y, unsigned fg, int draw) {
    char ln[260], w[82], t[264];
    int n = 0, rows = 0;
    ln[0] = 0;
#define FLUSH() do { if (draw && (!wrap_cap || rows < wrap_cap) && (!wrap_y1 || (y + rows * LINE >= wrap_y0 && y + rows * LINE + LINE <= wrap_y1))) text(wrap_ell && wrap_cap && rows == wrap_cap - 1 ? ellipsize(ln, maxw) : ln, x, y + rows * LINE, fg); rows++; n = 0; ln[0] = 0; } while (0)
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

static unsigned now_ticks(void) { struct jt_tasks t; jt_tasks(&t, 0); return t.ticks; }

static void push(int mine, const char *s) {
    if (nturn == NMSG) { for (int i = 1; i < NMSG; i++) ar->t[i - 1] = ar->t[i]; nturn--; }
    struct turn *t = &ar->t[nturn++];
    t->mine = mine;
    int n = 0; while (s[n] && n < TXT - 1) { t->text[n] = s[n]; n++; }
    t->text[n] = 0;
    struct cap *c = &ar->cap[mine ? 0 : 1];   /* the same words go up over the picture */
    for (int i = 0; i <= n; i++) c->text[i] = t->text[i];
    c->mine = mine; c->live = 1; c->born = now_ticks(); c->ends = mine ? c->born + CAP_HOLD_USER : 0;
}


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
static int uface_video JT_DATA = 0;               /* the desktop Samantha: full-window face under a liquid-glass panel */
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
static int vm_hd JT_DATA = 0, hd_state JT_DATA = 0;
static void hd_load(void);
static int hd_animating(unsigned now);
static int blink_prog(unsigned now);
static int uface_noblink JT_DATA = 0;
static unsigned blink_said JT_DATA = 0;
static int blink_shown JT_DATA = 0;   /* the lid position last painted: one more repaint is owed after it opens again */
static int face_step(unsigned now) {
    if (!face_inited) return 0;
    if (uface_video && hd_state == 0) { if (inlen == 0 && !spk_active) { hd_load(); if (vm_hd) { draw(); return 1; } } return 0; }   /* the portrait first: one sharp picture, no 72 frame downloads */
    if (uface_video && vm_hd) {   /* a still: redraw only while the mouth or a caption is moving */
        if ((int)(now - face_next) < 0) return 0;
        if (!hd_animating(now)) return 0;
        face_next = now + (blink_shown > 0 ? 2 : 8); draw(); return 1;
    }
    if (!uface_done) {
        if (inlen > 0) return 0;
        if (!spk_active) {   /* never download while she talks: a blocked app starves her voice */
            face_load_step(); if (uface_done) return 1;
            if (!uface_idle_n) return 0;
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
        draw(); return 1;
    } else {
        face_at = (face_at + 1) % uface_idle_n;
        draw(); return 1;
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
                else if (*p == 'u') {   /* \uXXXX: ring-3 text is Latin-1, so the degree sign (b0) and friends keep their glyph */
                    unsigned cp = 0; int k = 0;
                    while (k < 4 && p[1 + k]) { char h = p[1 + k]; cp = cp * 16 + (unsigned)(h <= '9' ? h - '0' : (h | 32) - 'a' + 10); k++; }
                    p += k;
                    out[n++] = cp < 0x100 ? (char)cp : (cp == 0x2018 || cp == 0x2019) ? '\'' : (cp == 0x201C || cp == 0x201D) ? '"' : (cp == 0x2013 || cp == 0x2014) ? '-' : '?';
                }
                else out[n++] = *p;
                p++;
            } else if ((unsigned char)*p == 0xC2 && ((unsigned char)p[1] & 0xC0) == 0x80) { out[n++] = p[1]; p += 2; }   /* raw UTF-8 for U+0080..U+00BF */
            else if ((unsigned char)*p == 0xC3 && ((unsigned char)p[1] & 0xC0) == 0x80) { out[n++] = (char)((unsigned char)p[1] + 0x40); p += 2; }   /* U+00C0..U+00FF */
            else if ((unsigned char)*p >= 0x80) { unsigned char c = (unsigned char)*p++; out[n++] = '?'; while (c >= 0xC0 && ((unsigned char)*p & 0xC0) == 0x80) p++; }   /* anything else is one placeholder */
            else out[n++] = *p++;
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
    "Stocks", "Search", "Epiphany", "Portfolio", "Activity", "Clock", "Music", "Movies", "Hamurapi", "Windgate", "Panes",
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
static unsigned spk_queued JT_DATA = 0, talk_t0 JT_DATA = 0;   /* bytes still queued at the card; tick her reply began */

/* ---- Video mode: Samantha is the whole screen. Her portrait is bilinear-scaled from the 736 px portrait (/face/hd.jpg) or, with no
   portrait on the host, from the 320 px frame set. A tall window (the phone) is filled by her, the overflow cropped. A wide one
   (2.12.1) gives her room: the portrait is 86 percent of the width, centred, her eyes about 40 percent down, the bands either side
   her own wall colour (sampled from the portrait's top corners, blended across) with the outer tenth of the portrait faded into it.
   Nothing frames her: the words are glass laid over the picture.
     - the input line sits in a blurred, tinted glass panel at the bottom;
     - what you said and what she answers appear as captions over the picture on a soft dark backdrop, fade in,
       hold while she speaks and a moment after, then fade out over about 0.6 s;
     - Tab opens a glass scrollback with the whole conversation (Up and Down scroll, Tab or Esc closes it);
     - a red dot at the top left closes her, and so does Esc.
   The face alone is composed once per size (vm_base) and never drawn on. Each frame copies it into the back buffer,
   lets the mouth follow her voice, lays the glass, captions and text over it, and copies the buffer to the window in
   one pass so the compositor never catches half a picture. ---- */
#define VM_MARGIN 16
#define VM_PH 46               /* input panel height */
#define VM_WASH 150            /* tint: white at 150/256 over the blurred face */
#define VM_PANEL_MAX 640
#define HD_N 736               /* the portrait's side, /face/hd.jpg */
static unsigned *vm_src JT_DATA = 0, *vm_base JT_DATA = 0, *vm_panel JT_DATA = 0, *vm_col JT_DATA = 0, *vm_small JT_DATA = 0, *vm_small2 JT_DATA = 0, *vm_save JT_DATA = 0;
static unsigned *vm_bg JT_DATA = 0;   /* her wall across the window, one colour per column (the second half of vm_col's block) */
static int vm_framed JT_DATA = 0;   /* 1 when a wide window gives her room: bands of wall either side, the portrait's edges faded into them */
#define VM_OUT 0xFFFFu   /* vm_col's source column for a window column outside the portrait: only the wall is there */
static unsigned vm_rowmix[HD_N + 2] JT_DATA;
static int vm_n JT_DATA = 320;                      /* source side: 736 for the portrait, 320 for a frame */
static int hd_tries JT_DATA = 0;   /* hd_state 0 untried, 1 loaded, 2 not available */
static int vm_key JT_DATA = -2, vm_cap JT_DATA = 0, vm_small_n JT_DATA = 0;
static int vm_px JT_DATA = 0, vm_py JT_DATA = 0, vm_pw JT_DATA = 0, vm_ph JT_DATA = 0;
static int vm_inv JT_DATA = 0, vm_ox JT_DATA = 0, vm_oy JT_DATA = 0;   /* 16.16 source px per window px; crop origin in source px */
static int vm_mx JT_DATA = 0, vm_my JT_DATA = 0, vm_mhw JT_DATA = 0, vm_ml1 JT_DATA = 0, vm_ml2 JT_DATA = 0, vm_mlu JT_DATA = 0, vm_mdmax JT_DATA = 0;   /* her mouth, window px */
static int *vm_seam JT_DATA = 0;   /* seam row down each column, 24.8 fixed point; the second half of the array is scratch */
static int vm_ex[2] JT_DATA, vm_ey[2] JT_DATA, vm_ew[2] JT_DATA, vm_eh JT_DATA = 0;   /* eye centres, half widths and the half height, window px */
static int mouth_s JT_DATA = 0, mouth_last JT_DATA = -1;   /* smoothed opening 0..255, last value logged */
static int uface_phone JT_DATA = 0;
static int hist_open JT_DATA = 0, hist_off JT_DATA = 0;
static unsigned open_t JT_DATA = 0;

static unsigned lerp8(unsigned a, unsigned b, unsigned f) {   /* packed 0RGB, f 0..255 */
    unsigned rb = (((a & 0xFF00FF) * (256 - f) + (b & 0xFF00FF) * f) >> 8) & 0xFF00FF;
    unsigned g = (((a & 0x00FF00) * (256 - f) + (b & 0x00FF00) * f) >> 8) & 0x00FF00;
    return rb | g;
}
static int isqrt_u(unsigned v) {
    unsigned r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    while (b) { if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; }
    return (int)r;
}
/* Half-width of the rounded rect (radius r, height h) cut off at row dy: how far in the corner pulls the edge. */
static int round_inset(int dy, int h, int r) {
    int d = dy < r ? r - dy : (dy >= h - r ? dy - (h - 1 - r) : 0), inset = 0;
    if (d) { while (inset < r && (r - inset) * (r - inset) + d * d > r * r) inset++; }
    return inset;
}

/* The portrait: one fetch, decoded to full colour. Tried first; a host without it (404, empty 200) leaves the frame set. */
static void put_num(char *o, int *n, int v);
static unsigned char *hd_jpg JT_DATA = 0;
static unsigned hd_len JT_DATA = 0;
/* Decode the kept portrait into a fresh vm_n x vm_n buffer (the caller frees it); 0 when it will not decode. */
static unsigned *hd_decode(void) {
    unsigned w = 0, h = 0;
    unsigned *px = (unsigned *)malloc((unsigned)(HD_N * HD_N) * 4);
    if (px && jpeg_decode_scaled32(hd_jpg, hd_len, px, HD_N, HD_N, &w, &h) == 0 && w == HD_N && h == HD_N) return px;
    if (px) free(px);
    return 0;
}
/* The portrait: one fetch, kept compressed (59 KB) and decoded to full colour only while a size is composed. Tried first;
   a host without it (404, empty 200) leaves the frame set. */
static void hd_load(void) {
    int got = jt_http_get("/face/hd.jpg", uface_file, UFACE_FILE);
    if (got == -16) return;   /* -EBUSY: ask again next poll, no try spent */
    if (got > 0) {
        hd_jpg = (unsigned char *)malloc((unsigned)got); hd_len = (unsigned)got;
        if (hd_jpg) {
            for (int k = 0; k < got; k++) hd_jpg[k] = uface_file[k];
            unsigned *px = hd_decode();   /* proves it decodes before we commit to it */
            if (px) { free(px); vm_n = HD_N; vm_hd = 1; hd_state = 1; vm_key = -2; uface_done = 1; jt_write(1, "face: hd=736\n", 13); return; }
            free(hd_jpg); hd_jpg = 0;
        }
    }
    { char d[40] = "samface: hd failed "; int n = 19; put_num(d, &n, got); d[n++] = '\n'; jt_write(1, d, (unsigned)n); }
    if (++hd_tries >= 2) {
        hd_state = 2;
        uface_cur = (unsigned short *)malloc((unsigned)(UFACE_SIDE * UFACE_SIDE * 2));   /* now the frame set is the face */
        if (!uface_cur) face_inited = 0;
    }
}

/* Window size to portrait mapping, the panel's place and where her mouth lands. A tall window shows all of her, edge to edge
   (the phone). A wide one (2.12.1) gives her room: the portrait is 86 percent of the width (never shorter than the window), centred,
   her eye row (373 thousandths down) about 40 percent down the window, kept inside the picture; ox is then negative, -ox columns of
   wall either side. Each window column gets the portrait column it reads and how much wall to mix in (the outer tenth fades in as
   (i/f)^1.5, i pixels from the edge, f a tenth of the portrait) in vm_col's top byte, or VM_OUT where only the wall shows. */
static void vm_geom(int W, int H) {
    vm_framed = hd_state != 2 && W > H;   /* the frame set (a host with no portrait) keeps the old fill; until we know, the portrait's layout, so her mouth does not move when it loads */
    int S = W > H ? W : H, oy;
    if (vm_framed) {
        S = W * 86 / 100; if (S < H) S = H;
        oy = S * 373 / 1000 - H * 40 / 100;
    } else oy = S * 235 / 1000;
    vm_inv = (int)(((unsigned)vm_n << 16) / (unsigned)S);
    vm_pw = W - 2 * VM_MARGIN; if (vm_pw > VM_PANEL_MAX) vm_pw = VM_PANEL_MAX;
    vm_px = (W - vm_pw) / 2; vm_ph = VM_PH;
    vm_py = H - VM_MARGIN - vm_ph; if (vm_py < 0) vm_py = 0;
    if (vm_framed) {   /* a short, wide window (an ordinary window on a big screen): her eyes ride higher so her lips stay clear of the panel */
        int low = S * 581 / 1000 + S * 55 / 1000 + 4 - vm_py;
        if (oy < low) oy = low;
    }
    int maxy = S - H;
    if (oy > maxy) oy = maxy;
    if (oy < 0) oy = 0;
    int ox = (S - W) / 2;
    vm_ox = ox; vm_oy = oy;
    int f = S / 10; if (f < 1) f = 1;
    for (int x = 0; x < W; x++) {
        int px = ox + x;   /* the portrait column under window column x */
        unsigned wt = 0;   /* how much wall is mixed in: 0 is the portrait alone */
        if (vm_framed) {
            if (px < 0 || px >= S) { vm_col[x] = VM_OUT; continue; }
            int i = px < S - 1 - px ? px : S - 1 - px;
            if (i < f) {
                int t8 = i * 256 / f, w8 = t8 * isqrt_u((unsigned)(t8 * 256)) >> 8;   /* (i/f)^1.5, 0..255 */
                if (w8 <= 0) { vm_col[x] = VM_OUT; continue; }
                wt = (unsigned)(256 - w8);
            }
        }
        int sx = (int)(((unsigned)px * 2 + 1) * (unsigned)vm_inv >> 1) - 32768;
        if (sx < 0) sx = 0;
        if (sx > ((vm_n - 2) << 16) + 65535) sx = ((vm_n - 2) << 16) + 65535;
        vm_col[x] = (unsigned)(sx >> 16) | ((unsigned)((sx >> 8) & 255) << 16) | (wt << 24);
    }
    /* her mouth, in thousandths of the portrait's side: centre across 518, lip seam down 581, half width 87 */
    vm_mx = S * 518 / 1000 - ox; vm_my = S * 581 / 1000 - oy; vm_mhw = S * 87 / 1000;
    vm_ml1 = S * 55 / 1000; vm_ml2 = S * 130 / 1000; vm_mlu = S * 70 / 1000; vm_mdmax = S * 21 / 1000;
    { /* her chin slides down with an open mouth only as far as the glass panel (it is hidden past that), never less than 1.5 lip depths */
        int lim = vm_py - 6 - vm_my;
        if (vm_ml2 > lim) vm_ml2 = lim;
        if (vm_ml2 < vm_ml1 * 3 / 2) vm_ml2 = vm_ml1 * 3 / 2;
    }
    /* her eyes, in thousandths of the portrait's side: centres across 427 and 636, down 367 and 379, half widths 37 and 43, half height 16 */
    vm_ex[0] = S * 427 / 1000 - ox; vm_ey[0] = S * 367 / 1000 - oy; vm_ex[1] = S * 636 / 1000 - ox; vm_ey[1] = S * 379 / 1000 - oy;
    vm_ew[0] = S * 37 / 1000; vm_ew[1] = S * 43 / 1000; vm_eh = S * 16 / 1000;
}

/* The average colour of a block of the portrait: the wall in her top corners. */
static unsigned wall_avg(int x0, int x1, int y0, int y1) {
    unsigned sr = 0, sg = 0, sb = 0, n = 0;
    for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) {
        unsigned c = vm_src[y * vm_n + x]; sr += (c >> 16) & 255; sg += (c >> 8) & 255; sb += c & 255; n++;
    }
    if (!n) return BG;
    return ((sr / n) << 16) | ((sg / n) << 8) | (sb / n);
}
/* Her wall across the whole window: the colour of the portrait's top left corner (the first 4 percent across, 2 to 25 percent
   down) at the left edge, the top right corner's at the right edge, blended straight across. Needs vm_src. */
static void vm_walls(int W) {
    int n = vm_n, xs = n * 4 / 100, ya = n * 2 / 100, yb = n * 25 / 100;
    unsigned l = wall_avg(0, xs, ya, yb), r = wall_avg(n - xs, n, ya, yb);
    for (int x = 0; x < W; x++) {
        unsigned c = 0;
        for (int sh = 16; sh >= 0; sh -= 8) {
            int a = (int)((l >> sh) & 255), b = (int)((r >> sh) & 255);
            c |= (unsigned)(a + (b - a) * x / (W - 1)) << sh;
        }
        vm_bg[x] = c;
    }
}

/* One window row y, columns xa..xb, bilinear from the vm_n x vm_n source; a column outside the portrait is her wall, and the
   portrait's faded edge columns are mixed into it. Every other column is the scaled portrait alone. */
static void vm_scale_row(unsigned *dst, int y, int xa, int xb) {
    int sy = (int)(((unsigned)(vm_oy + y) * 2 + 1) * (unsigned)vm_inv >> 1) - 32768;
    if (sy < 0) sy = 0;
    if (sy > ((vm_n - 2) << 16) + 65535) sy = ((vm_n - 2) << 16) + 65535;
    int iy = sy >> 16; unsigned fy = (unsigned)((sy >> 8) & 255);
    const unsigned *a = vm_src + iy * vm_n, *b = a + vm_n;
    for (int i = 0; i < vm_n; i++) vm_rowmix[i] = lerp8(a[i], b[i], fy);
    for (int x = xa; x < xb; x++) {
        unsigned c = vm_col[x], ix = c & 0xFFFF;
        if (ix == VM_OUT) { dst[x] = vm_bg[x]; continue; }
        unsigned px = lerp8(vm_rowmix[ix], vm_rowmix[ix + 1], (c >> 16) & 255), wt = c >> 24;
        dst[x] = wt ? lerp8(px, vm_bg[x], wt) : px;
    }
}

static void vm_box1d(unsigned *img, int n, int stride, int r, unsigned *line) {
    unsigned cnt = (unsigned)(2 * r + 1), recip = 65536u / cnt;
    unsigned sr = 0, sg = 0, sb = 0;
    for (int i = 0; i < n; i++) line[i] = img[i * stride];
    for (int k = -r; k <= r; k++) { unsigned c = line[k < 0 ? 0 : (k > n - 1 ? n - 1 : k)]; sr += (c >> 16) & 255; sg += (c >> 8) & 255; sb += c & 255; }
    for (int i = 0; i < n; i++) {
        img[i * stride] = (((sr * recip) >> 16) << 16) | (((sg * recip) >> 16) << 8) | ((sb * recip) >> 16);
        unsigned ca = line[i + r + 1 > n - 1 ? n - 1 : i + r + 1], cb = line[i - r < 0 ? 0 : i - r];
        sr += ((ca >> 16) & 255) - ((cb >> 16) & 255); sg += ((ca >> 8) & 255) - ((cb >> 8) & 255); sb += (ca & 255) - (cb & 255);
    }
}

/* Liquid glass over the rect x,y,w,h of the face (vm_base): a real blur of the pixels behind it (averaged down 4x,
   box-blurred, scaled back up bilinear), a white tint, a bright rim, rounded corners. Written to out[w*h]; pixels
   outside the rounded corners are the face's own, so out can be laid straight over the rect. */
static void vm_glass(unsigned *out, int stride, int x, int y, int w, int h, int rad, int wash) {
    const int F = 4;
    int W = back_w, H = back_h;
    int sw = (w + F - 1) / F + 2, sh = (h + F - 1) / F + 2;
    if (sw * sh > vm_small_n || w <= 0 || h <= 0) return;
    for (int j = 0; j < sh; j++) for (int i = 0; i < sw; i++) {
        unsigned sr = 0, sg = 0, sb = 0;
        for (int b = 0; b < F; b += 2) for (int a = 0; a < F; a += 2) {
            int px = x - F + i * F + a, py = y - F + j * F + b;
            if (px < 0) px = 0; if (px >= W) px = W - 1; if (py < 0) py = 0; if (py >= H) py = H - 1;
            unsigned c = vm_base[py * W + px]; sr += (c >> 16) & 255; sg += (c >> 8) & 255; sb += c & 255;
        }
        vm_small[j * sw + i] = ((sr >> 2) << 16) | ((sg >> 2) << 8) | (sb >> 2);
    }
    for (int round = 0; round < 2; round++) {
        for (int j = 0; j < sh; j++) vm_box1d(vm_small + j * sw, sw, 1, 3, vm_small2);
        for (int i = 0; i < sw; i++) vm_box1d(vm_small + i, sh, sw, 3, vm_small2);
    }
    for (int dy = 0; dy < h; dy++) {
        int inset = round_inset(dy, h, rad);
        int fy = ((dy + F) * 256) / F - 128, j0 = fy >> 8; unsigned wy = (unsigned)(fy & 255);
        if (j0 > sh - 2) { j0 = sh - 2; wy = 255; }
        for (int dx = 0; dx < w; dx++) {
            unsigned face = vm_base[(y + dy) * W + (x + dx)];
            if (dx < inset || dx >= w - inset) { out[dy * stride + dx] = face; continue; }
            int fx = ((dx + F) * 256) / F - 128, i0 = fx >> 8; unsigned wx = (unsigned)(fx & 255);
            if (i0 > sw - 2) { i0 = sw - 2; wx = 255; }
            unsigned top = lerp8(vm_small[j0 * sw + i0], vm_small[j0 * sw + i0 + 1], wx);
            unsigned bot = lerp8(vm_small[(j0 + 1) * sw + i0], vm_small[(j0 + 1) * sw + i0 + 1], wx);
            unsigned c = lerp8(top, bot, wy);
            int rim = dy == 0 || dy == h - 1 || dx == inset || dx == w - inset - 1;
            out[dy * stride + dx] = lerp8(c, WHITE, rim ? 235 : (unsigned)wash);
        }
    }
}

static void vm_seam_find(void);
/* Compose the face alone into vm_base from vm_src, then the input panel's glass over a copy of it. */
static void vm_compose(void) {
    int W = back_w, H = back_h;
    unsigned *tmp = 0;
    if (vm_hd) { tmp = hd_decode(); if (!tmp) { for (int i = 0; i < W * H; i++) vm_base[i] = BG; return; } vm_src = tmp; }
    if (vm_framed) vm_walls(W);
    for (int y = 0; y < H; y++) vm_scale_row(vm_base + y * W, y, 0, W);
    if (tmp) { free(tmp); vm_src = 0; }
    vm_seam_find();
    vm_glass(vm_panel, vm_pw, vm_px, vm_py, vm_pw, vm_ph, 22, VM_WASH);
}

/* ---- the mouth and the blink, drawn by code ----
   The portrait's lips are shut. open (0..255) is how wide she is speaking. Her real lip seam is found column by column in
   the picture (vm_seam), because it is not a straight line: the upper lip lifts a quarter of the opening along that
   curve, the lower lip and chin slide down the rest, and the gap between is her mouth. The edges of the gap are soft
   (about a pixel of blend into the lips' own colour, so the lip shading carries on into the opening), the upper teeth
   are a short arch under the upper lip that fades out toward the corners and is shaded from the top, the inside is
   dark with a hint of tongue, and the corners draw in a little as she opens. Every other pixel is the portrait's own.
   Nothing is drawn at open == 0, so a closed or silent mouth is the portrait exactly. */

static int ss8(int v) { if (v <= 0) return 0; if (v >= 256) return 256; return ((v * v >> 8) * (768 - 2 * v)) >> 8; }   /* smoothstep, 0..256 */
static int lum_of(unsigned c) { return (int)(((c >> 16) & 255) * 3 + ((c >> 8) & 255) * 6 + (c & 255)); }
static unsigned samp(int x8, int y8) {   /* bilinear read of the composed portrait */
    int W = back_w;
    if (x8 < 0) x8 = 0;
    if (y8 < 0) y8 = 0;
    int x = x8 >> 8, y = y8 >> 8; unsigned fx = (unsigned)(x8 & 255), fy = (unsigned)(y8 & 255);
    if (x > W - 2) { x = W - 2; fx = 255; }
    if (y > back_h - 2) { y = back_h - 2; fy = 255; }
    const unsigned *p = vm_base + y * W + x;
    return lerp8(lerp8(p[0], p[1], fx), lerp8(p[W], p[W + 1], fx), fy);
}
/* Follow the seam between her lips: the darkest row near the nominal one in each column, then smoothed over a few
   columns so one noisy pixel cannot kink it. */
static void vm_seam_find(void) {
    if (!vm_seam || !vm_hd || vm_mhw < 8) return;
    int W = back_w, H = back_h, R = vm_mhw * 13 / 10;
    int ya = vm_my - vm_ml1 * 30 / 100, yb = vm_my + vm_ml1 * 25 / 100;
    if (ya < 1) ya = 1;
    if (yb > H - 3) yb = H - 3;
    int xa = vm_mx - R < 1 ? 1 : vm_mx - R, xb = vm_mx + R > W - 2 ? W - 2 : vm_mx + R;
    int *t = vm_seam + W;
    for (int x = xa; x <= xb; x++) {
        int best = 1 << 30, by = vm_my;
        for (int y = ya; y <= yb; y++) {
            int s = 0;
            for (int dy = -1; dy <= 1; dy++) { const unsigned *r = vm_base + (y + dy) * W + x; s += lum_of(r[-1]) + 2 * lum_of(r[0]) + lum_of(r[1]); }
            s += (y > vm_my ? y - vm_my : vm_my - y) * 3;
            if (s < best) { best = s; by = y; }
        }
        vm_seam[x] = by * 256;
    }
    int rr = vm_mhw / 8; if (rr < 2) rr = 2;
    for (int pass = 0; pass < 3; pass++) {
        for (int x = xa; x <= xb; x++) {
            int s = 0, n = 0;
            for (int d = -rr; d <= rr; d++) { int q = x + d; if (q < xa || q > xb) continue; s += vm_seam[q]; n++; }
            t[x] = s / n;
        }
        for (int x = xa; x <= xb; x++) vm_seam[x] = t[x];
    }
}
static void vm_mouth(unsigned *dst, int open) {
    if (!vm_hd || !vm_seam || open <= 0 || vm_mhw < 8) return;
    int W = back_w, H = back_h;
    int sq = vm_mhw * 5 / 100 * open / 255;                 /* how far the corners draw in */
    int hwe = vm_mhw - sq, R = vm_mhw * 13 / 10;
    int g8max = vm_mdmax * 256 * open / 255 * 14 / 10, E8 = 330;      /* E8: the soft edge, 1.3 px */
    int thmax = vm_ml1 * 256 / 9;                           /* the most tooth that shows */
    int ca = vm_mx - hwe * 8 / 10, cb = vm_mx + hwe * 8 / 10;   /* the chord between the corners: an open mouth is rounder than the seam it opens along */
    if (ca < 1) ca = 1;
    if (cb > W - 2) cb = W - 2;
    int chord8 = (vm_seam[ca] + vm_seam[cb]) / 2;
    for (int x = vm_mx - R; x <= vm_mx + R; x++) {
        if (x < 1 || x >= W - 1) continue;
        int dx = x - vm_mx, ad = dx < 0 ? -dx : dx, s8;
        if (ad <= hwe) s8 = ad * vm_mhw * 256 / hwe;
        else s8 = vm_mhw * 256 + (ad - hwe) * 256 * (R - vm_mhw) / (R - hwe);
        int u = ad * 256 / hwe; if (u > 256) u = 256;
        int ug = u * 256 / 215; if (ug > 256) ug = 256;   /* the opening stops short of the corners, which stay lips */
        int pq = (isqrt_u((unsigned)(65536 - ug * ug)) + (256 - (ug * ug >> 8))) / 2;   /* the opening's outline: halfway between an ellipse and a lens */
        int g8 = g8max * pq >> 8, gu = g8 / 5, gl = g8 - gu;
        int sy8 = vm_seam[x];
        gl += (chord8 - sy8) * 45 / 100 * pq / 256; if (gl < 0) gl = 0;   /* the lower edge sags less than the seam does */
        g8 = gu + gl;
        int y0 = (sy8 >> 8) - vm_mlu, y1 = (sy8 >> 8) + vm_ml2;
        if (y0 < 0) y0 = 0;
        if (y1 > H - 1) y1 = H - 1;
        for (int y = y0; y < y1; y++) {
            int t8 = y * 256 - sy8, at = t8 < 0 ? -t8 : t8, w, rel;
            if (t8 < 0) {
                int lo = vm_ml1 * 128, hi = vm_mlu * 256;
                w = at <= lo ? 256 : (at >= hi ? 0 : 256 * (hi - at) / (hi - lo));
                rel = t8 + gu * w / 256;
            } else {
                int lo = vm_ml1 * 256, hi = vm_ml2 * 256;
                w = at <= lo ? 256 : (at >= hi ? 0 : 256 * (hi - at) / (hi - lo));
                rel = t8 - gl * w / 256;
            }
            int sxo = dx < 0 ? -s8 : s8;
            int xs8 = (vm_mx << 8) + dx * 256 + (sxo - dx * 256) * w / 256;   /* the corners' draw-in fades with the lip zone */
            int cov = t8 < 0 ? ss8(rel * 256 / E8) : ss8(-rel * 256 / E8);
            int ys8 = sy8 + (t8 < 0 ? (rel < 0 ? rel : 0) : (rel > 0 ? rel : 0));
            unsigned px = samp(xs8, ys8);
            if (cov > 0 && g8 > 64) {
                int dtop = t8 + gu;                        /* how far below the upper lip's edge, 8.8 */
                int fr = dtop * 256 / g8; if (fr < 0) fr = 0; if (fr > 256) fr = 256;
                unsigned in = ((unsigned)(38 + fr * 34 / 256) << 16) | ((unsigned)(13 + fr * 14 / 256) << 8) | (unsigned)(17 + fr * 14 / 256);   /* the dark inside */
                if (g8 >= 5 * 256) {                       /* a hint of tongue low in a wide opening */
                    int qu = u * 256 / 120, qv = (fr > 205 ? fr - 205 : 205 - fr) * 256 / 70;
                    int q = (qu * qu + qv * qv) >> 8, tg = q >= 256 ? 0 : ss8(256 - q);
                    int big = g8 >= 9 * 256 ? 256 : (g8 - 5 * 256) * 256 / (4 * 256);
                    in = lerp8(in, 0x00864240, (unsigned)(tg * big / 256 * 150 / 256));
                }
                if (g8 >= 3 * 256) {                       /* the upper teeth: a short arch, shaded from the top, gone toward the corners */
                    int th = g8 * 55 / 100; if (th > thmax) th = thmax;
                    int arch = 200 + ((256 - (u * u >> 8)) * 56 >> 8);
                    th = th * arch >> 8;
                    int lw = u <= 80 ? 256 : (u >= 175 ? 0 : 256 * (175 - u) / 95);
                    int ta = ss8((th - dtop) * 256 / E8) * lw >> 8;
                    if (ta > 0) {
                        int gr = th > 0 ? dtop * 256 / th : 0; if (gr < 0) gr = 0; if (gr > 256) gr = 256;
                        unsigned tc = lerp8(0x00D2C2AC, 0x009C8470, (unsigned)(gr > 255 ? 255 : gr));
                        int side = 256 - ((u * u >> 8) * 150 >> 8);
                        int lip = 130 + (ss8(dtop * 256 / (3 * 256)) * 126 >> 8);   /* the upper lip's shadow on the teeth */
                        tc = lerp8(0x00000000, tc, (unsigned)((side * lip >> 8) > 255 ? 255 : (side * lip >> 8)));
                        in = lerp8(in, tc, (unsigned)(ta > 255 ? 255 : ta));
                    }
                }
                px = lerp8(px, in, (unsigned)(cov > 255 ? 255 : cov));
            } else if (cov > 0) px = lerp8(px, 0x00381418, (unsigned)(cov > 255 ? 255 : cov) * 3 / 4);
            dst[y * W + x] = px;
        }
    }
}

/* The blink: each eye gets a lid that comes down from the lash line to the lower lid and goes back up. The lid is the
   portrait's own skin above the eye, stretched down over it (so its shading, creases and colour are hers), with a dark
   lash line along its edge. prog is 0 (open, nothing drawn) to 256 (shut). */
static unsigned soft(int x, int y8) {   /* five reads along the row, 12 px wide */
    return lerp8(lerp8(samp((x - 6) * 256, y8), samp((x + 6) * 256, y8), 128), lerp8(lerp8(samp((x - 3) * 256, y8), samp((x + 3) * 256, y8), 128), samp(x * 256, y8), 128), 170);
}
static void vm_blink(unsigned *dst, int prog) {
    if (!vm_hd || prog <= 0 || vm_eh < 6) return;
    int W = back_w, H = back_h, eh = vm_eh;
    for (int e = 0; e < 2; e++) {
        int cx = vm_ex[e], cy = vm_ey[e], ew = vm_ew[e], tilt = e ? -22 : 46;   /* tilt: the inner corner sits lower, the slope across the eye /256 */
        for (int x = cx - ew - 2; x <= cx + ew + 2; x++) {
            if (x < 2 || x >= W - 2) continue;
            int u = (x - cx) * 256 / ew; if (u > 256) u = 256; if (u < -256) u = -256;
            int hh = eh * isqrt_u((unsigned)((65536 - u * u) * 64)) / 8;   /* half the lens height at this column, 8.8 */
            if (hh < 256) continue;
            int au = u < 0 ? -u : u, hx = ss8((256 - au) * 256 / 70);          /* the sides fade into the untouched portrait */
            int mid = (cy * 256 + tilt * (x - cx)) - 128;
            int top = mid - hh, bot = mid + hh + 256;
            int edge = top + (bot - top) * prog / 256;                          /* the lid's lower edge in this column */
            int y0 = top - eh * 150;                                            /* the skin that gets stretched starts here ... */
            int send = top - (prog * 4);                                        /* ... and its source ends a little above the lashes */
            int ya = y0 >> 8, yb = (edge >> 8) + 1;
            if (ya < 0) ya = 0;
            if (yb >= H) yb = H - 1;
            for (int y = ya; y <= yb; y++) {
                int y8 = y * 256 + 128;
                int under = ss8((edge - y8) * 256 / 330);                       /* the lid covers above its edge, soft by a pixel */
                unsigned px = dst[y * W + x];
                if (under > 0) {
                    int ys8 = y0 + (y8 - y0) * (send - y0) / (edge - y0 > 1 ? edge - y0 : 1);
                    int ramp = ss8((y8 - y0) * 256 / 2048);   /* sharp where the stretch begins, so there is no seam */
                    unsigned lid = lerp8(samp(x * 256, ys8), soft(x, ys8), (unsigned)(ramp > 255 ? 255 : ramp));   /* stretched skin read soft: no streaks */
                    lid = lerp8(lid, soft(x, bot + eh * 200), (unsigned)(ramp * 170 / 256));   /* and warmed with the lighter skin below the eye, so a shut lid is skin, not the lashes' shadow */
                    int dep = ss8((y8 - top) * 256 / (hh + 256));          /* a little shadow deepens toward the lash line */
                    lid = lerp8(lid, 0x005A3A30, (unsigned)(dep * 28 / 256));
                    unsigned hsh = ((unsigned)x * 73856093u) ^ ((unsigned)y * 19349663u); hsh ^= hsh >> 13; hsh *= 1274126177u; hsh ^= hsh >> 16;
                    int gr = (int)(hsh & 7) - 3;   /* the portrait's own grain, so the lid is not smoother than the skin around it */
                    { int r = (int)((lid >> 16) & 255) + gr, g = (int)((lid >> 8) & 255) + gr, b = (int)(lid & 255) + gr;
                      r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
                      lid = ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b; }
                    int cov = under * hx >> 8;
                    px = lerp8(px, lid, (unsigned)(cov > 255 ? 255 : cov));
                }
                if (prog > 24) {                                                /* the lash line along the lid's edge */
                    int d = y8 - edge; if (d < 0) d = -d;
                    int la = 256 - d * 256 / 560; if (la < 0) la = 0;
                    int fade = prog > 80 ? 256 : (prog - 24) * 256 / 56;
                    la = la * hx >> 8; la = la * fade >> 8;
                    if (la > 0) px = lerp8(px, 0x002E1E1A, (unsigned)(la * 150 / 256));
                }
                dst[y * W + x] = px;
            }
        }
    }
}

/* ---- her voice to an opening ---- */
static int mouth_target(unsigned now) {
    if (!face_talking(now)) return 0;
    if (spk_active && spk_card && spk_pcm && spk_len) {
        int pos = (int)spk_off - (int)spk_queued - 512;   /* about where the speaker is in this clip */
        if (pos < 0) pos = 0;
        if (pos + 320 > (int)spk_len) pos = (int)spk_len - 320;
        if (pos < 0 || pos > (int)spk_off) return 0;
        unsigned sum = 0;
        for (int i = 0; i < 320; i++) { int d = (int)spk_pcm[pos + i] - 128; sum += (unsigned)(d < 0 ? -d : d); }
        int m = (int)(sum / 320);   /* mean distance from silence, 0..127 */
        if (m < 3) return 0;
        int v = (m - 3) * 255 / 40;
        return v > 255 ? 255 : v;
    }
    /* No sound card: nothing to measure, so she shapes the sentence itself, one letter every 60 ms, wide on vowels
       and closed on the lips' own letters. */
    if (nturn > 0 && !ar->t[nturn - 1].mine) {
        const char *s = ar->t[nturn - 1].text; int n = slen(s), i = (int)((now - talk_t0) / 6u);
        if (i >= 0 && i < n) {
            char c = s[i] | 32;
            if (c == 'a' || c == 'o') return 255;
            if (c == 'e' || c == 'i' || c == 'u' || c == 'y') return 170;
            if (c == 'm' || c == 'b' || c == 'p' || c == ' ') return 0;
            return 90;
        }
    }
    return 0;
}

/* ---- captions ---- */
static int cap_rows(const char *s, int tw_, int maxrows) {
    int rows = wrap(s, tw_, 0, 0, 0, 0);
    return rows > maxrows ? maxrows : rows;
}
static int cap_alpha(const struct cap *c, unsigned now) {
    if (!c->live) return 0;
    int a = 255;
    unsigned age = now - c->born;
    if (age < CAP_IN) a = (int)(age * 255 / CAP_IN);
    if (c->ends) {
        int over = (int)(now - c->ends);
        if (over >= CAP_OUT) return 0;
        if (over > 0) { int o = 255 - over * 255 / CAP_OUT; if (o < a) a = o; }
    }
    return a;
}
static int caps_alive(void) { for (int k = 0; k < NCAP; k++) if (ar->cap[k].live) return 1; return 0; }
static int caps_prev JT_DATA = 0;
static void cap_update(unsigned now) {
    for (int k = 0; k < NCAP; k++) {
        struct cap *c = &ar->cap[k];
        if (!c->live) continue;
        if (!c->ends && !c->mine && !face_talking(now) && now - c->born > 30) c->ends = now + CAP_HOLD_AFTER;   /* her voice is over: hold a moment, then fade */
        if (c->ends && (int)(now - c->ends) >= CAP_OUT) c->live = 0;
    }
    if (caps_alive() != caps_prev) { caps_prev = caps_alive(); jt_write(1, caps_prev ? "samface: captions=1\n" : "samface: captions=0\n", 21); }   /* the check waits on these */
}

/* One caption over the picture, bottom edge at ybot: a soft dark backdrop (rounded, with a feathered edge), white text,
   all composed at full strength and then mixed over what was there by the fade alpha. Returns the box's top. */
static int cap_draw(struct cap *c, int ybot, int ytop_limit, unsigned now, int fixed_top, int fixed_rows, int amax) {
    int a = cap_alpha(c, now), W = back_w;
    if (a > amax) a = amax;   /* a caption giving way to a newer one in the same place */
    int tw_ = (W - 2 * VM_MARGIN - 40 > 600 ? 600 : W - 2 * VM_MARGIN - 40);
    int avail = fixed_top ? fixed_rows : (ybot - ytop_limit - 26) / LINE; if (avail < 1) avail = 1;
    int rows = cap_rows(c->text, tw_, avail > 7 ? 7 : avail);
    int bw = 0;
    { /* the widest row decides the box */
        int saved_sink = 0; (void)saved_sink;
        bw = rows > 1 ? tw_ : (tw(c->text) < tw_ ? tw(c->text) : tw_);
    }
    int bh = rows * LINE + 18, bx = (W - bw) / 2 - 16, by = fixed_top ? fixed_top : ybot - bh;
    int FE = 8;   /* feather */
    int rx = bx - FE, ry = by - FE, rw = bw + 32 + 2 * FE, rh = bh + 2 * FE;
    if (rx < 0) rx = 0; if (ry < 0) ry = 0;
    if (rx + rw > W) rw = W - rx; if (ry + rh > back_h) rh = back_h - ry;
    if (a <= 0 || rw <= 0 || rh <= 0 || rw * rh > vm_cap) return by;
    for (int i = 0; i < rh; i++) for (int j = 0; j < rw; j++) vm_save[i * rw + j] = backbuf[(ry + i) * W + rx + j];
    /* backdrop: signed distance to the rounded rect (radius 16) mapped to alpha across the feather */
    int cx2 = bx + (bw + 32) / 2, cy2 = by + bh / 2, hw = (bw + 32) / 2 - 16, hh = bh / 2 - 16;
    if (hh < 0) hh = 0;
    for (int i = 0; i < rh; i++) for (int j = 0; j < rw; j++) {
        int px = rx + j, py = ry + i;
        int dx = px - cx2; if (dx < 0) dx = -dx; dx -= hw; if (dx < 0) dx = 0;
        int dy = py - cy2; if (dy < 0) dy = -dy; dy -= hh; if (dy < 0) dy = 0;
        int dist = isqrt_u((unsigned)(dx * dx + dy * dy)) - 16;   /* < 0 inside */
        if (dist >= FE) continue;
        unsigned al = dist <= -FE ? CAP_ALPHA : (unsigned)(CAP_ALPHA * (FE - dist) / (2 * FE));
        backbuf[py * W + px] = lerp8(backbuf[py * W + px], 0x00101012, al);
    }
    unsigned *real = win.pixels; win.pixels = backbuf;
    wrap_cap = rows; wrap_ell = fixed_top && cap_rows(c->text, tw_, 99) > rows;   /* the shared top slot says so when a reply goes on past its rows */
    int tx = (W - tw_) / 2;
    if (rows == 1 && bw < tw_) tx = (W - bw) / 2;
    wrap(c->text, tw_, tx, by + 9, c->mine ? 0x00E8D8BC : WHITE, 1);
    wrap_cap = wrap_ell = 0;
    win.pixels = real;
    for (int i = 0; i < rh; i++) for (int j = 0; j < rw; j++) {
        unsigned *p = &backbuf[(ry + i) * W + rx + j];
        *p = lerp8(vm_save[i * rw + j], *p, (unsigned)a);
    }
    return by;
}

/* ---- the blink ----
   Every 3 to 6 seconds her lids close and open over 120 ms. The gap after each blink comes from a counter run through
   a small hash, never from the clock or a random source, so the same boot blinks at the same moments. A `noblink`
   boot flag (the checks that read exact pixels pass it) switches it off. */
#define BLINK_TICKS 12u   /* 100 Hz ticks: 120 ms */
static unsigned blink_n JT_DATA = 0, blink_at JT_DATA = 0;
static unsigned blink_gap(unsigned n) {   /* 300..599 ticks */
    unsigned h = (n + 1) * 2654435761u; h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
    return 300u + h % 300u;
}
/* 0 (open) .. 256 (shut) for this moment; also moves on to the next blink once one has finished */
static int blink_prog(unsigned now) {
    if (uface_noblink || !vm_hd || !open_t) return 0;
    if (!blink_at) blink_at = now + blink_gap(blink_n);   /* counted from the first moment the portrait is up */
    int d = (int)(now - blink_at);
    if (d < 0) return 0;
    if (blink_said != blink_n + 1) {   /* one serial line per blink, with the tick it began on: the check reads the schedule from these */
        blink_said = blink_n + 1;
        char l[40] = "samface: blink="; int k = 15; put_num(l, &k, (int)blink_at); l[k++] = '\n'; jt_write(1, l, (unsigned)k);
    }
    if (d >= (int)BLINK_TICKS) { blink_n++; blink_at += BLINK_TICKS + blink_gap(blink_n); return 0; }
    int ph = d * 256 / (int)BLINK_TICKS;   /* 0..255 through the blink: down for the first 40%, up for the rest */
    return ph < 102 ? ss8(ph * 256 / 102) : ss8((256 - ph) * 256 / 154);
}
static int hd_animating(unsigned now) {
    return face_talking(now) || mouth_s > 0 || caps_alive() || rec_on || blink_prog(now) > 0 || blink_shown > 0;
}

static void pill(const char *s, int cx, int y, unsigned fg) {
    int w = tw(s) + 28, h = 26, x = cx - w / 2, W = back_w;
    for (int dy = 0; dy < h; dy++) {
        int inset = round_inset(dy, h, 13);
        for (int dx = inset; dx < w - inset; dx++) {
            int px = x + dx, py = y + dy; if (px < 0 || px >= W || py < 0 || py >= back_h) continue;
            backbuf[py * W + px] = lerp8(backbuf[py * W + px], 0x00101012, 160);
        }
    }
    text(s, x + 14, y + 5, fg);
}

/* ---- scrollback: the whole conversation on a glass sheet ---- */
static void hist_draw(void) {
    int W = back_w, top = 56, bot = vm_py - 14;
    int h = bot - top;
    if (h < 80) return;
    int x = vm_px, w = vm_pw, tw_ = w - 60;
    vm_glass(backbuf + top * W + x, W, x, top, w, h, 22, 205);
    int total = 0;
    for (int i = 0; i < nturn; i++) total += wrap(ar->t[i].text, tw_ - (ar->t[i].mine ? 60 : 0), 0, 0, 0, 0) * LINE + 10;
    int view = h - 44;
    int maxoff = total - view; if (maxoff < 0) maxoff = 0;
    if (hist_off > maxoff) hist_off = maxoff;
    if (hist_off < 0) hist_off = 0;
    int y = top + 14 + (total < view ? 0 : -(maxoff - hist_off));   /* newest at the bottom unless scrolled */
    wrap_y0 = top + 12; wrap_y1 = top + h - 30;
    text("Conversation", x + 24, top + 10, ACCENT);
    wrap_y0 = top + 34;
    if (!nturn) text("Nothing yet.", x + 24, top + 40, DIM);
    y += 22;
    for (int i = 0; i < nturn; i++) {
        int mine = ar->t[i].mine, ind = mine ? 60 : 0;
        int rows = wrap(ar->t[i].text, tw_ - ind, 0, 0, 0, 0);
        wrap(ar->t[i].text, tw_ - ind, x + 24 + ind, y, mine ? DIM : INK, 1);
        y += rows * LINE + 10;
    }
    wrap_y0 = wrap_y1 = 0;
    text("Up and Down scroll, Tab closes", x + 24, top + h - 24, DIM);
}

static int vm_k JT_DATA = 1;   /* the picture is composed at 1/vm_k of the window and each pixel repeated vm_k times on the way out */
/* Tell the checks where her mouth and eyes are now. The geometry changes once the portrait has loaded (a wide window gives her
   room only then), so this runs after every vm_geom and prints only when a number moved. */
static int vm_rep[13] JT_DATA;
static void vm_report(void) {
    int v[13] = { vm_mx * vm_k, vm_my * vm_k, vm_mhw * vm_k, vm_ml1 * vm_k, vm_ex[0], vm_ey[0], vm_ew[0], vm_ex[1], vm_ey[1], vm_ew[1], vm_eh, back_w * vm_k, back_h * vm_k };
    for (int i = 4; i < 11; i++) v[i] *= vm_k;
    int same = 1; for (int i = 0; i < 13; i++) if (v[i] != vm_rep[i]) same = 0;
    if (same) return;
    for (int i = 0; i < 13; i++) vm_rep[i] = v[i];
    char d[72] = "samface: mouth="; int n = 15;
    for (int i = 0; i < 4; i++) { put_num(d, &n, v[i]); d[n++] = i < 3 ? ',' : '\n'; }
    jt_write(1, d, (unsigned)n);   /* centre x, seam y, half width, depth: where the check looks */
    char e[96] = "samface: eyes="; int k = 14;
    for (int i = 4; i < 11; i++) { put_num(e, &k, v[i]); e[k++] = i < 10 ? ',' : '\n'; }
    jt_write(1, e, (unsigned)k);   /* both eyes' centres and half widths, then the half height: where the blink check looks */
    char w[40] = "samface: win="; int j = 13; put_num(w, &j, v[11]); w[j++] = ','; put_num(w, &j, v[12]); w[j++] = '\n';
    jt_write(1, w, (unsigned)j);   /* her window's size (an ordinary window on a big screen is not the screen): where the glass panel is */
}
static void draw_video_k(void) {
    int W = (int)win.width, H = (int)win.height;
    if (W < 64 || H < 64) return;
    if (!backbuf || back_w != W || back_h != H) {
        unsigned *old[6] = { backbuf, vm_base, vm_panel, vm_col, vm_save, (unsigned *)vm_seam };
        for (int i = 0; i < 6; i++) if (old[i]) free(old[i]);
        if (vm_small) free(vm_small);
        if (vm_small2) free(vm_small2);
        backbuf = vm_base = vm_panel = vm_col = vm_bg = vm_save = vm_small = vm_small2 = 0; vm_seam = 0;
        back_w = W; back_h = H;
        backbuf = (unsigned *)malloc((unsigned)(W * H) * 4);
        vm_base = (unsigned *)malloc((unsigned)(W * H) * 4);
        vm_col = (unsigned *)malloc((unsigned)W * 8);   /* a source column and wall weight per window column, then the wall colour of each */
        vm_bg = vm_col ? vm_col + W : 0;
        vm_seam = (int *)malloc((unsigned)W * 8);
        vm_panel = (unsigned *)malloc((unsigned)(W * VM_PH) * 4);
        vm_cap = (VM_PANEL_MAX + 80) * 170; if (vm_cap > W * (H / 2)) vm_cap = W * (H / 2);
        vm_save = (unsigned *)malloc((unsigned)vm_cap * 4);
        vm_small_n = (W / 4 + 3) * (H / 4 + 3);
        vm_small = (unsigned *)malloc((unsigned)vm_small_n * 4);
        vm_small2 = (unsigned *)malloc((unsigned)(W > H ? W : H) * 4 + 64);
        vm_key = -2;
        if (!backbuf || !vm_base || !vm_col || !vm_seam || !vm_panel || !vm_save || !vm_small || !vm_small2) { rect(0, 0, W, H, BG); return; }
        vm_geom(W, H);
        vm_report();
    }
    unsigned now = now_ticks();
    if (!open_t) open_t = now;
    /* the source: the kept portrait, or the current frame of the frame set, rescaled whenever the frame changes */
    int kind = face_talking(now) && uface_talk_n ? 1 : 0, idx = kind ? face_talk_at : face_at;
    int key = vm_hd ? 0 : (uface_idle_n ? kind * 100 + idx : -1);
    if (key != vm_key) {
        if (!vm_hd) {
            vm_n = 320;
            if (!vm_src) vm_src = (unsigned *)malloc(320 * 320 * 4);
            if (!vm_src) { rect(0, 0, W, H, BG); return; }
            const unsigned short *f = key >= 0 ? face_frame(kind, idx) : 0;
            for (int i = 0; i < 320 * 320; i++) vm_src[i] = f ? px888(f[i]) : BG;
            vm_geom(W, H);
        }
        vm_geom(W, H);
        vm_report();
        vm_compose(); vm_key = key;
    }
    cap_update(now);
    unsigned *real = win.pixels, c = (unsigned)(W * H);
    { unsigned *d = backbuf, *s = vm_base, n = c; __asm__ volatile ("rep movsl" : "+D"(d), "+S"(s), "+c"(n) : : "memory"); }
    /* the mouth follows her voice: quick to open, a little slower to close */
    int target = mouth_target(now);
    mouth_s = target > mouth_s ? (mouth_s + target * 2) / 3 : (mouth_s * 2 + target) / 3;
    if (mouth_s < 6) mouth_s = 0;
    vm_mouth(backbuf, mouth_s);
    blink_shown = blink_prog(now);
    vm_blink(backbuf, blink_shown);
    { /* her opening for the check: 0 is exactly shut, 1 to 16 are the steps above it */
        int step = mouth_s ? 1 + mouth_s / 16 : 0;
        if (vm_hd && step != mouth_last) { mouth_last = step; char d[40] = "samface: open="; int n = 14; n = face_num(d, n, step); d[n++] = '\n'; jt_write(1, d, (unsigned)n); }
    }
    /* the input panel: its glass, then the line, the mic and the caret */
    for (int dy = 0; dy < vm_ph; dy++) for (int x = 0; x < vm_pw; x++) backbuf[(vm_py + dy) * W + vm_px + x] = vm_panel[dy * vm_pw + x];
    win.pixels = backbuf;
    {
        int lx = vm_px + 24, iy = vm_py + (vm_ph - LINE) / 2, mx = vm_px + vm_pw - 34;
        const char *shown = ar->in;
        while (*shown && tw(shown) > vm_pw - 110) shown++;
        if (!*shown) text(rec_on ? "Listening ..." : "Message Samantha", lx, iy, DIM);
        else text(shown, lx, iy, INK);
        if (!rec_on || (now / 40) % 2) rect(lx + (*shown ? tw(shown) + 1 : 0), iy - 2, 2, LINE + 4, ACCENT);
        /* the mic: click it to talk, click again to send (F2 holds to talk) */
        int cy = vm_py + vm_ph / 2;
        for (int dy = -11; dy <= 11; dy++) for (int dx = -11; dx <= 11; dx++) if (dx * dx + dy * dy <= 121) backbuf[(cy + dy) * W + mx + dx] = rec_on ? 0x00D9453A : lerp8(backbuf[(cy + dy) * W + mx + dx], INK, 36);
        rect(mx - 2, cy - 6, 5, 9, rec_on ? WHITE : INK); rect(mx - 4, cy + 1, 1, 3, rec_on ? WHITE : INK); rect(mx + 4, cy + 1, 1, 3, rec_on ? WHITE : INK);
        rect(mx - 3, cy + 4, 7, 1, rec_on ? WHITE : INK); rect(mx, cy + 5, 1, 3, rec_on ? WHITE : INK); rect(mx - 3, cy + 8, 7, 1, rec_on ? WHITE : INK);
    }
    /* captions. With room under her lips (the phone) hers is on the bottom and yours above it. A wide window puts her mouth low (2.12.1)
       and her chin travels down to the glass when she speaks, so nothing is drawn under her lips there: both captions share one slot at
       the top, as many rows as fit above her eyebrows (one at the very top of a window too short for that); the newest owns it and the
       other fades out as it fades in. */
    int ytop = vm_my + vm_ml1 + 8, ybot = vm_py - 12;   /* captions stay below her lower lip */
    if ((ybot - ytop - 26) / LINE >= 2) {
        for (int k = NCAP - 1; k >= 0; k--) {
            struct cap *cp = &ar->cap[k];
            if (!cp->live) continue;
            int top = cap_draw(cp, ybot, ytop, now, 0, 0, 255);
            if (cap_alpha(cp, now) > 0) ybot = top - 10;
        }
    } else {
        int brows = (vm_ey[0] < vm_ey[1] ? vm_ey[0] : vm_ey[1]) - vm_mhw * 8 / 10 - 6;   /* her eyebrows are about 0.07 of the portrait above her eyes */
        int top0 = 56, trows = (brows - top0 - 26) / LINE;   /* under the status pill; a box is its rows plus 18 and a feather of 8 either side */
        if (trows < 1) { top0 = 8; trows = (brows - top0 - 26) / LINE; }
        if (trows < 1) trows = 1; if (trows > 7) trows = 7;
        int own = -1;
        for (int k = 0; k < NCAP; k++) if (ar->cap[k].live && (own < 0 || ar->cap[k].born >= ar->cap[own].born)) own = k;
        for (int k = 0; own >= 0 && k < NCAP; k++) {   /* the one that is giving way first, the owner over it */
            struct cap *cp = &ar->cap[k];
            if (k != own && cp->live) cap_draw(cp, ybot, ytop, now, top0, trows, 255 - cap_alpha(&ar->cap[own], now));
        }
        if (own >= 0) cap_draw(&ar->cap[own], ybot, ytop, now, top0, trows, 255);
    }
    if (hist_open) hist_draw();
    /* status pill (not while the scrollback covers it) and the way out */
    if (status[0] != 'r' || status[1] != 'e') pill(status, W / 2, 16, WHITE);
    else if (!vm_hd && !uface_idle_n && !uface_done) pill("waking up ...", W / 2, 16, WHITE);
    if (!uface_phone) {
        for (int dy = -9; dy <= 9; dy++) for (int dx = -9; dx <= 9; dx++) {
            int d2 = dx * dx + dy * dy, px = 24 + dx, py = 24 + dy;
            if (d2 <= 81) backbuf[py * W + px] = d2 <= 49 ? 0x00FF5F57 : 0x00101012;
        }
        if (now - open_t < 700) pill("Esc closes", 24 + 62, 11, WHITE);
    }
    win.pixels = real;
    if (vm_k == 1) { unsigned *d = real, *s = backbuf, n = c; __asm__ volatile ("rep movsl" : "+D"(d), "+S"(s), "+c"(n) : : "memory"); }
}

/* The portrait, the base and the back buffer are 4 bytes a pixel each, and a ring-3 heap stops at 8 MB. A window bigger
   than about 560000 pixels (a 2560x1440 screen is 640 thousand logical pixels over 3.6 MB a buffer) is composed at half
   size or less, then every pixel is repeated on the way out: the same picture, chunkier type, still all of her. */
static void draw_video(void) {
    int WW = (int)win.width, WH = (int)win.height, k = 1;
    while ((WW / k) * (WH / k) > 560000) k++;
    if (k != vm_k) { vm_k = k; back_w = 0; }   /* a new size: buffers are rebuilt */
    if (k == 1) { draw_video_k(); return; }
    unsigned *real = win.pixels;
    win.width = (unsigned)(WW / k); win.height = (unsigned)(WH / k);
    draw_video_k();
    win.width = (unsigned)WW; win.height = (unsigned)WH; win.pixels = real;
    if (!backbuf) return;
    int IW = WW / k, IH = WH / k;
    for (int y = 0; y < WH; y++) {
        unsigned *d = real + y * WW; const unsigned *sr = backbuf + (y / k < IH ? y / k : IH - 1) * IW;
        if (y % k) { const unsigned *prev = real + (y - 1) * WW; for (int x = 0; x < WW; x++) d[x] = prev[x]; continue; }
        for (int x = 0; x < WW; x++) d[x] = sr[(x / k < IW ? x / k : IW - 1)];
    }
}

static void draw(void) {
    if (uface_video) { draw_video(); return; }
    draw_portfolio_buffered();
}

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
    if (jt_audio_status(&st) >= 0) { spk_played = st.played; spk_queued = st.queued; if (st.rate) spk_rate = st.rate; }
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
    serial("samtyped=", msg);   /* exactly what the keyboard handed her, for tools/checks/samantha-fullscreen-check.py */
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
        talk_until = now_ticks() + 100u * 2u + 6u * (unsigned)slen(ar->reply); talk_t0 = now_ticks();
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
        talk_until = now_ticks() + 100u * 2u + 6u * (unsigned)slen(ar->t[nturn - 1].text); talk_t0 = now_ticks();
        speak_begin(ar->t[nturn - 1].text);
    }
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    for (int i = 1; i < argc; i++) { const char *a = argv[i], *b = "portfolio"; while (*a && *a == *b) { a++; b++; } if (!*a && !*b) uface_portfolio = 1; }
    for (int i = 1; i < argc; i++) { const char *a = argv[i], *b = "phone"; while (*a && *a == *b) { a++; b++; } if (!*a && !*b) uface_phone = 1; }   /* the phone has its own back chevron: no red dot, no Esc hint */
    for (int i = 1; i < argc; i++) { const char *a = argv[i], *b = "noblink"; while (*a && *a == *b) { a++; b++; } if (!*a && !*b) uface_noblink = 1; }   /* checks that read exact pixels pass noblink */
    uface_full = 1; uface_n = UFACE_MAX;
    if (!uface_portfolio) uface_video = 1;   /* Samantha is full screen on the desktop and the phone alike; only Joshua's own face keeps its layout */
    if (jt_window_open(&win) != 0 || !win.pixels) { jt_write(2, "samantha: no window\n", 20); jt_exit(1); }
    ar = (struct arena *)malloc(sizeof *ar);              /* 1.9.27: the heap, not the image window */
    if (!ar) { jt_write(2, "samantha: no heap\n", 18); jt_exit(1); }
    for (unsigned k = 0; k < sizeof *ar; k++) ((unsigned char *)ar)[k] = 0;
    uface_file = (unsigned char *)malloc(UFACE_FILE);
    if (!uface_video) uface_cur = (unsigned short *)malloc((unsigned)(UFACE_SIDE * UFACE_SIDE * 2));   /* the frame set's decode buffer; the portrait path never needs it */
    face_inited = uface_file && (uface_video || uface_cur);
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
            if (spk_active && (k == 8 || (k >= 32 && k < 127))) speak_stop(); /* typing skips the rest of her reply, and the key still counts (it used to be eaten: "hat's on my calendar") */
            if (k == JT_KEY_ESC && hist_open) { hist_open = 0; draw(); jt_write(1, "samface: history=0\n", 19); }   /* Esc closes the scrollback first, then her */
            else if (k == JT_KEY_ESC) break;
            else if (k == 9) { hist_open = !hist_open; hist_off = 0; draw(); jt_write(1, hist_open ? "samface: history=1\n" : "samface: history=0\n", 19); }   /* the marker follows the paint, so a check that waits on it sees the panel */
            else if (k == JT_KEY_UP && hist_open) hist_off += 2 * LINE;
            else if (k == JT_KEY_DOWN && hist_open) hist_off -= 2 * LINE;
            else if (k == JT_KEY_F2) listen_start();
            else if (k == JT_KEY_F2_UP) listen_stop_and_send();
            else if (k == JT_KEY_COPY || k == JT_KEY_CUT) { jt_clip_set(ar->in, (unsigned)inlen); if (k == JT_KEY_CUT) { inlen = 0; ar->in[0] = 0; } } /* the system clipboard */
            else if (k == JT_KEY_PASTE) {
                char pb[INMAX + 1];
                int n = INMAX > inlen ? jt_clip_get(pb, (unsigned)(INMAX - inlen)) : 0;
                for (int i = 0; i < n; i++) if (pb[i] >= 32 && pb[i] < 127) ar->in[inlen++] = pb[i];
                ar->in[inlen] = 0;
            }
            else if (k == JT_KEY_ENTER) { speak_stop(); send(); }
            else if (k == 8) { if (inlen > 0) ar->in[--inlen] = 0; }
            else if (k >= 32 && k < 127 && inlen < INMAX) { ar->in[inlen++] = (char)k; ar->in[inlen] = 0; }
        } else if (ev.kind == JT_EV_CLICK && uface_video) {
            int cx = ev.a / vm_k, cy = ev.b / vm_k;
            { char d[40] = "samface: click="; int n = 15; put_num(d, &n, cx); d[n++] = ','; put_num(d, &n, cy); d[n++] = '\n'; jt_write(1, d, (unsigned)n); }
            if (!uface_phone && (cx - 24) * (cx - 24) + (cy - 24) * (cy - 24) <= 14 * 14) break;   /* the red dot: close */
            int mx = vm_px + vm_pw - 34, my = vm_py + vm_ph / 2;
            if ((cx - mx) * (cx - mx) + (cy - my) * (cy - my) <= 15 * 15) { if (rec_on) listen_stop_and_send(); else listen_start(); }
        } else if (ev.kind == JT_EV_WHEEL && hist_open) { hist_off += ev.a * LINE * 2; }
        else { flags = JT_POLL_PRESENT; continue; } /* other clicks and the wheel change nothing */
        draw();
        flags = JT_POLL_PRESENT;
    }
    jt_write(1, "samantha: closed\n", 17);
    jt_exit(0);
}
