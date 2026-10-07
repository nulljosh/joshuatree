/* claude: Claude Code from inside Joshua Tree, as a ring-3 program (2.14.0).
 *
 * Joshua Tree has no TLS and no Node, so Claude Code cannot run here. It runs on
 * the relay machine (tools/claude-relay/relay.py, usually the Mac running QEMU),
 * and this window is the front end: you type a question, the kernel POSTs it to
 * the relay's /api/claude with SYS_HTTP_POST and JT_POST_CLAUDE, the relay runs
 * `claude -p` with read-only tools, and the answer comes back as plain text.
 *
 * The program never sees the relay's address or token. Settings owns both
 * (Assistant > Claude relay, Claude token) and the kernel adds the bearer header
 * itself, only for /api/claude on that host. With either one missing the kernel
 * refuses at once (-EACCES), so the browser demo shows a clear line instead of
 * waiting on a relay it can never reach.
 *
 * Wire: body {"prompt":"...","session":"<uuid or empty>"}; reply "S <uuid>\n<text>".
 * The session uuid is kept and sent back, so a conversation carries on. "/new"
 * starts a fresh one.
 *
 * Keys: typing, Backspace, Enter sends, Up/Down and the wheel scroll the
 * transcript, paste inserts the clipboard, Esc closes. Backquote on an empty
 * input line is the deliberate crash every ring-3 app has (a backquote inside a
 * prompt is just a character). Serial markers: claude: ring-3 window, claude:
 * sent N, claude: reply N, claude: error N, claude: closed.
 */
#include "jtsys.h"
#include "libjt/text.h"
#include "libjt/stdlib.h"

#define BG      0x00F5F0EB /* WINDOW_BODY */
#define INK     0x001C1C1E /* WINDOW_INK */
#define MUTED   0x00807468
#define ACCENT  0x00B5502C /* the house terracotta */
#define ERR     0x00A33B3B
#define FIELD   0x00FFFFFF
#define RULE    0x00D9D3CB
#define MARGIN  20
#define LOG_MAX 24576       /* the transcript: role byte, text, NUL, oldest dropped first */
#define LINES_MAX 1600
#define IN_MAX  1000        /* typed prompt, printable ASCII */
#define REQ_MAX 6144        /* JT_HTTP_POST_BODY_MAX */
#define RESP_MAX 8192       /* JT_HTTP_POST_REPLY_MAX */
#define SID_MAX 40

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
struct line { unsigned start, len; char kind; }; /* kind: role byte of a label, 't' text, ' ' gap */
struct arena {
    char log[LOG_MAX]; char in[IN_MAX + 1]; char req[REQ_MAX]; char resp[RESP_MAX + 1];
    char sid[SID_MAX + 1]; char tmp[256]; char clip[IN_MAX + 1]; struct line lines[LINES_MAX]; int cw[128];
};
static struct arena *ar JT_DATA = 0;
static unsigned loglen JT_DATA = 0, inlen JT_DATA = 0;
static int nlines JT_DATA = 0, scroll JT_DATA = 0, layout_w JT_DATA = -1;
static const char *status JT_DATA = 0;
static unsigned status_rgb JT_DATA = MUTED;

static void serial_n(const char *tag, int n) {
    char b[48]; int k = 0;
    while (*tag) b[k++] = *tag++;
    if (n < 0) { b[k++] = '-'; n = -n; }
    char d[12]; int nd = 0;
    do { d[nd++] = (char)('0' + n % 10); n /= 10; } while (n);
    while (nd) b[k++] = d[--nd];
    b[k++] = '\n';
    jt_write(1, b, (unsigned)k);
}
static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }

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

/* Transcript. Each turn is a role byte ('Y' you, 'C' Claude, 'E' an error line), its text, a NUL. */
static void log_add(char role, const char *text, unsigned n) {
    if (n > LOG_MAX / 2) n = LOG_MAX / 2;
    while (loglen + n + 2 > LOG_MAX && loglen) {          /* drop whole turns from the front */
        unsigned first = 0;
        while (first < loglen && ar->log[first]) first++;
        first++;
        for (unsigned i = first; i < loglen; i++) ar->log[i - first] = ar->log[i];
        loglen -= first;
    }
    ar->log[loglen++] = role;
    for (unsigned i = 0; i < n; i++) {
        char c = text[i];
        ar->log[loglen++] = (c == '\n' || (c >= 32 && c < 127) || (unsigned char)c == 0xB0) ? c : '?';
    }
    ar->log[loglen++] = 0;
    layout_w = -1; scroll = 0;
}

static int cwid(char c) { unsigned char u = (unsigned char)c; return u < 128 ? ar->cw[u] : ar->cw['?']; }
static void push_line(unsigned start, unsigned len, char kind) {
    if (nlines >= LINES_MAX) {                            /* keep the newest: shift the oldest half out */
        for (int i = LINES_MAX / 2; i < LINES_MAX; i++) ar->lines[i - LINES_MAX / 2] = ar->lines[i];
        nlines = LINES_MAX / 2;
    }
    ar->lines[nlines].start = start; ar->lines[nlines].len = len; ar->lines[nlines].kind = kind; nlines++;
}
/* Greedy word wrap of every turn to the text width, once per width or transcript change. */
static void layout(int width) {
    nlines = 0;
    unsigned p = 0;
    while (p < loglen) {
        char role = ar->log[p++];
        push_line(p, 0, role);                            /* the "You" / "Claude" label line */
        while (p < loglen && ar->log[p]) {
            unsigned s = p, last_space = 0; int w = 0, have_space = 0;
            while (p < loglen && ar->log[p] && ar->log[p] != '\n') {
                int cw = cwid(ar->log[p]);
                if (w + cw > width && p > s) break;
                if (ar->log[p] == ' ') { last_space = p; have_space = 1; }
                w += cw; p++;
            }
            if (p < loglen && ar->log[p] && ar->log[p] != '\n' && have_space && last_space > s) {
                push_line(s, last_space - s, 't'); p = last_space + 1;   /* break at the last space */
            } else {
                push_line(s, p - s, 't');
                if (p < loglen && ar->log[p] == '\n') p++;
            }
        }
        p++;                                              /* the NUL */
        push_line(p, 0, ' ');
    }
    layout_w = width;
}

static void draw(void) {
    int W = (int)win.width, H = (int)win.height;
    int lh = jt_text_height(JT_FACE_BODY) + 3;
    int text_w = W - 2 * MARGIN;
    if (text_w < 40) text_w = 40;
    if (layout_w != text_w) layout(text_w);
    rect(0, 0, W, H, BG);

    /* Header: who answers and where. */
    int x = jt_text_draw(&win, JT_FACE_BOLD, MARGIN, 12, INK, "Claude Code");
    jt_text_draw(&win, JT_FACE_BODY, x + 10, 12, MUTED, "runs on the relay machine, read-only");
    rect(MARGIN, 36, W - 2 * MARGIN, 1, RULE);

    /* Input field and the status line above it. */
    int field_y = H - 42, field_h = 30;
    int status_y = field_y - lh - 6;
    int top = 46, bottom = status_y - 6;
    rect(MARGIN, field_y, W - 2 * MARGIN, field_h, FIELD);
    rect(MARGIN, field_y, W - 2 * MARGIN, 1, RULE);
    rect(MARGIN, field_y + field_h - 1, W - 2 * MARGIN, 1, RULE);
    {
        int room = W - 2 * MARGIN - 24, w = 0; unsigned from = inlen;
        while (from > 0 && w + cwid(ar->in[from - 1]) <= room) { from--; w += cwid(ar->in[from]); }
        int ty = field_y + (field_h - jt_text_height(JT_FACE_BODY)) / 2;
        int cx = MARGIN + 10;
        if (inlen) {
            unsigned k = 0;
            for (unsigned i = from; i < inlen && k < sizeof ar->tmp - 1; i++) ar->tmp[k++] = ar->in[i];
            ar->tmp[k] = 0;
            /* jt_text_draw takes at most a line's worth; draw in 90-byte pieces */
            unsigned off = 0;
            while (off < k) {
                char save = 0; unsigned end = off + 90 < k ? off + 90 : k;
                save = ar->tmp[end]; ar->tmp[end] = 0;
                cx = jt_text_draw(&win, JT_FACE_BODY, cx, ty, INK, ar->tmp + off);
                ar->tmp[end] = save; off = end;
            }
        } else {
            jt_text_draw(&win, JT_FACE_BODY, cx, ty, MUTED, "Ask Claude about this code");
        }
        rect(inlen ? cx + 1 : MARGIN + 9, ty + 1, 2, jt_text_height(JT_FACE_BODY) - 2, ACCENT); /* caret */
    }
    jt_text_draw(&win, JT_FACE_BODY, MARGIN, status_y, status_rgb,
                 status ? status : "Enter sends.  Up and Down scroll.  /new starts over.  Esc closes.");

    /* Transcript: the newest lines that fit, `scroll` lines up from the bottom. */
    int rows = (bottom - top) / lh;
    if (rows < 1) rows = 1;
    if (!nlines) {
        jt_text_draw(&win, JT_FACE_BODY, MARGIN, top + 8, MUTED, "Claude Code answers from the relay machine, with read-only tools.");
        jt_text_draw(&win, JT_FACE_BODY, MARGIN, top + 8 + lh, MUTED, "Set it up: docs/CLAUDE-APP.md. Then Settings > Assistant > Claude relay and Claude token.");
        return;
    }
    int max_scroll = nlines > rows ? nlines - rows : 0;
    if (scroll > max_scroll) scroll = max_scroll;
    if (scroll < 0) scroll = 0;
    int first = nlines - rows - scroll;
    if (first < 0) first = 0;
    int y = top;
    for (int i = first; i < nlines && y + lh <= bottom + 2; i++, y += lh) {
        struct line *ln = &ar->lines[i];
        if (ln->kind == 'Y') jt_text_draw(&win, JT_FACE_BOLD, MARGIN, y, MUTED, "You");
        else if (ln->kind == 'C') jt_text_draw(&win, JT_FACE_BOLD, MARGIN, y, ACCENT, "Claude");
        else if (ln->kind == 'E') jt_text_draw(&win, JT_FACE_BOLD, MARGIN, y, ERR, "Not sent");
        else if (ln->kind == 't' && ln->len) {
            char role = 'C';                              /* colour by the turn this line belongs to */
            for (int j = i; j >= 0; j--) if (ar->lines[j].kind != 't') { role = ar->lines[j].kind; break; }
            unsigned n = ln->len < 200 ? ln->len : 200, off = 0;
            int lx = MARGIN;
            while (off < n) {                             /* jt_text_draw pieces of at most 90 bytes */
                unsigned k = 0;
                while (k < 90 && off + k < n) { ar->tmp[k] = ar->log[ln->start + off + k]; k++; }
                ar->tmp[k] = 0;
                lx = jt_text_draw(&win, JT_FACE_BODY, lx, y, role == 'E' ? ERR : INK, ar->tmp);
                off += k;
            }
        }
    }
    if (scroll) jt_text_draw(&win, JT_FACE_BODY, W - MARGIN - jt_text_width(JT_FACE_BODY, "more below"), bottom - lh, MUTED, "more below");
}

static void present(void) { struct jt_event dummy; draw(); jt_window_poll(&dummy, JT_POLL_PRESENT); }

static int json_str(char *out, int n, int cap, const char *s, unsigned len) {
    for (unsigned i = 0; i < len && n < cap - 8; i++) {
        char c = s[i];
        if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = c; }
        else if (c == '\n') { out[n++] = '\\'; out[n++] = 'n'; }
        else if ((unsigned char)c < 32 || (unsigned char)c > 126) continue;
        else out[n++] = c;
    }
    return n;
}
static int cat(char *out, int n, int cap, const char *s) { while (*s && n < cap - 1) out[n++] = *s++; return n; }

/* What a failed post means, in words. The kernel returns -errno, or -status for a non-200. */
static const char *why(int r) {
    switch (r) {
    case -13:  return "No Claude relay is set up. Start relay.py on your Mac, then fill in Settings > Assistant > Claude relay and Claude token. The browser demo cannot reach one.";
    case -19:  return "No network card. Claude needs the network to reach the relay.";
    case -5:   return "The relay did not answer. Is relay.py running, and is Settings > Assistant > Claude relay right?";
    case -16:  return "The network is busy with another fetch. Try again in a moment.";
    case -401: return "The relay refused the token. Check Settings > Assistant > Claude token.";
    case -413: return "That question is too long for the relay.";
    case -429: return "Claude is still answering another question.";
    case -504: return "Claude took too long and the relay stopped it. Try a smaller question.";
    case -502: return "Claude Code failed on the relay machine. Is it installed and logged in there?";
    default:   return r <= -100 ? "The relay answered with an HTTP error." : "The request failed.";
    }
}

static void send(void) {
    ar->in[inlen] = 0;
    if (!inlen) return;
    if (inlen == 4 && ar->in[0] == '/' && ar->in[1] == 'n' && ar->in[2] == 'e' && ar->in[3] == 'w') {
        loglen = 0; nlines = 0; layout_w = -1; ar->sid[0] = 0; inlen = 0;
        status = "New conversation."; status_rgb = MUTED;
        jt_write(1, "claude: new session\n", 20);
        return;
    }
    log_add('Y', ar->in, inlen);
    int n = cat(ar->req, 0, REQ_MAX, "{\"prompt\":\"");
    n = json_str(ar->req, n, REQ_MAX, ar->in, inlen);
    n = cat(ar->req, n, REQ_MAX, "\",\"session\":\"");
    n = cat(ar->req, n, REQ_MAX, ar->sid);
    n = cat(ar->req, n, REQ_MAX, "\"}");
    inlen = 0;
    status = "Claude is thinking ..."; status_rgb = ACCENT;
    present();                                            /* the desktop shows "thinking" before the wait */
    serial_n("claude: sent ", n);
    struct jt_http_post a = { "/api/claude", ar->req, (unsigned)n, ar->resp, RESP_MAX, JT_HTTP_POST_TICKS_CLAUDE };
    int r = jt_http_post_ex(&a, JT_POST_CLAUDE);
    if (r > 0) {
        ar->resp[r] = 0;
        const char *body = ar->resp;
        if (r > 2 && ar->resp[0] == 'S' && ar->resp[1] == ' ') {     /* "S <uuid>\n" */
            int i = 2, k = 0;
            while (i < r && ar->resp[i] != '\n' && k < SID_MAX) ar->sid[k++] = ar->resp[i++];
            ar->sid[k] = 0;
            if (k == 1 && ar->sid[0] == '-') ar->sid[0] = 0;
            body = ar->resp + (i < r ? i + 1 : i);
        }
        log_add('C', body, slen(body));
        status = 0; status_rgb = MUTED;
        serial_n("claude: reply ", r);
    } else {
        if (r == 0) r = -502;
        const char *msg = why(r);
        log_add('E', msg, slen(msg));
        status = "Not sent. The reason is above."; status_rgb = ERR;
        serial_n("claude: error ", r);
    }
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open(&win) != 0 || !win.pixels) { jt_write(2, "claude: no window\n", 18); jt_exit(1); }
    ar = (struct arena *)malloc(sizeof *ar);
    if (!ar) { jt_write(2, "claude: no heap\n", 16); jt_exit(1); }
    for (unsigned k = 0; k < sizeof *ar; k++) ((unsigned char *)ar)[k] = 0;
    for (int c = 32; c < 127; c++) { char g[2] = { (char)c, 0 }; ar->cw[c] = jt_text_width(JT_FACE_BODY, g); }
    for (int c = 0; c < 32; c++) ar->cw[c] = ar->cw['?'];
    ar->cw[127] = ar->cw['?'];
    draw();
    jt_write(1, "claude: ring-3 window\n", 22);
    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        if (r == 1 && jt_window_resized(&ev, &win)) { layout_w = -1; draw(); flags = JT_POLL_PRESENT; continue; }
        flags = 0;
        if (r == -11) { jt_sched_yield(); continue; }
        if (r != 1) break;
        if (ev.kind == JT_EV_WHEEL) { scroll += ev.a > 0 ? 3 : -3; draw(); flags = JT_POLL_PRESENT; continue; }
        if (ev.kind != JT_EV_KEY) continue;
        int k = ev.a;
        if (k == '`' && !inlen) { jt_write(1, "claude: crashing on purpose\n", 28); *(volatile int *)0 = 1; }
        if (k == JT_KEY_ESC) break;
        if (k == JT_KEY_ENTER) send();
        else if (k == 8) { if (inlen) inlen--; }
        else if (k == JT_KEY_UP) scroll++;
        else if (k == JT_KEY_DOWN) { if (scroll) scroll--; }
        else if (k == JT_KEY_PASTE) {
            int n = IN_MAX > (int)inlen ? jt_clip_get(ar->clip, IN_MAX - inlen) : 0;
            for (int i = 0; i < n; i++) { char pc = ar->clip[i]; ar->in[inlen++] = (pc >= 32 && pc < 127) ? pc : ' '; }
        }
        else if (k >= 32 && k < 127 && inlen < IN_MAX) { ar->in[inlen++] = (char)k; if (status && status_rgb != ACCENT) status = 0; }
        else continue;
        draw();
        flags = JT_POLL_PRESENT;
    }
    jt_write(1, "claude: closed\n", 15);
    jt_exit(0);
}
