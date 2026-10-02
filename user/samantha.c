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
 * Only the text path is here. FACE SLICE: the band under the title is
 * reserved for the face and stays empty until that slice. TOOLS SLICE: a pick
 * is shown as a text line; the tools do not run yet.
 * Type is the antialiased libjt face.
 */
#include "jtsys.h"
#include "libjt/text.h"

#define BG     0x00FAF8F6
#define INK    0x001C1C1E
#define DIM    0x0075726E
#define ACCENT 0x00B7862A
#define RULE   0x00E0D8CE
#define WHITE  0x00FFFFFF
#define YOUBG  0x00EDE6DC

#define NMSG 6          /* turns kept, oldest drop first (chat.h keeps 8) */
#define TXT 240         /* stored text per turn */
#define INMAX 160
#define REQ 2048
#define RESP 2048
#define FACE_H 64       /* FACE SLICE: reserved band under the title */
#define HEAD_H 36
#define INPUT_H 36
#define LINE 16

struct turn { int mine; char text[TXT]; };
struct arena { struct turn t[NMSG]; char in[INMAX + 1]; char req[REQ]; char resp[RESP]; };

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
extern char _user_end[];
static struct arena *ar JT_DATA = 0;
static int nturn JT_DATA = 0;
static int inlen JT_DATA = 0;
static const char *status JT_DATA = "ready";

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
static int wrap(const char *s, int maxw, int x, int y, unsigned fg, int draw) {
    char ln[TXT + 2], w[82], t[TXT + 4];
    int n = 0, rows = 0;
    ln[0] = 0;
#define FLUSH() do { if (draw) text(ln, x, y + rows * LINE, fg); rows++; n = 0; ln[0] = 0; } while (0)
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

static void draw(void) {
    int W = (int)win.width, H = (int)win.height;
    rect(0, 0, W, H, BG);
    text("Samantha", 20, 12, ACCENT);
    text(status, 20 + tw("Samantha") + 16, 12, DIM);
    rect(20, HEAD_H, W - 40, 1, RULE);
    /* FACE SLICE: FACE_H pixels of reserved band, x 0..W, y HEAD_H+1 .. HEAD_H+FACE_H. Draw the face here. */
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
    if (!nturn) text("Say something to Samantha.", 20, top + 4, DIM);
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
    n = cat(ar->req, n, REQ, "{\"model\":\"samantha\",\"stream\":false,\"think\":false,\"messages\":[");
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
    if (r <= 0) { serial("chatfail=", "noreply"); status = r < -99 ? "error: the host answered with an error" : "error: no reply"; return 0; }
    char ans[TXT];
    if (!extract(ar->resp, "content", ans, TXT)) { status = "error: no reply text"; return 0; }
    serial("chatreply=", ans);
    push(0, ans);
    return 1;
}

static void send(void) {
    char msg[INMAX + 1], tool[24], arg[128];
    for (int i = 0; i <= inlen; i++) msg[i] = ar->in[i];
    if (!inlen) return;
    push(1, msg);
    inlen = 0; ar->in[0] = 0;
    status = "checking for a tool ...";
    draw();
    struct jt_event dummy; jt_window_poll(&dummy, JT_POLL_PRESENT); /* mark dirty so the desktop paints before the wait */
    if (pick(msg, tool, sizeof tool, arg, sizeof arg)) {
        /* TOOLS SLICE: tools run here in a later slice (chat_run_tool); until then show the pick as text. */
        char line[TXT]; int n = cat(line, 0, TXT - 1, "[tool ");
        n = cat(line, n, TXT - 1, tool); n = cat(line, n, TXT - 1, ": "); n = cat(line, n, TXT - 1, arg);
        line[n++] = ']'; line[n] = 0;
        push(0, line);
    }
    status = "generating ...";
    draw();
    jt_window_poll(&dummy, JT_POLL_PRESENT);
    if (chat()) status = "ready";
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open(&win) != 0 || !win.pixels) { jt_write(2, "samantha: no window\n", 20); jt_exit(1); }
    ar = (struct arena *)(((unsigned)_user_end + 15u) & ~15u);
    draw();
    jt_write(1, "samantha: ring-3 window\n", 24);
    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        flags = 0;
        if (r == -11) { jt_sched_yield(); continue; }
        if (r != 1) break;
        if (ev.kind == JT_EV_KEY) {
            int k = ev.a;
            if (k == '`') { jt_write(1, "samantha: crashing on purpose\n", 30); *(volatile int *)0 = 1; }
            if (k == JT_KEY_ESC) break;
            else if (k == JT_KEY_ENTER) send();
            else if (k == 8) { if (inlen > 0) ar->in[--inlen] = 0; }
            else if (k >= 32 && k < 127 && inlen < INMAX) { ar->in[inlen++] = (char)k; ar->in[inlen] = 0; }
        } else { flags = JT_POLL_PRESENT; continue; } /* clicks and wheel never close and change nothing */
        draw();
        flags = JT_POLL_PRESENT;
    }
    jt_write(1, "samantha: closed\n", 17);
    jt_exit(0);
}
