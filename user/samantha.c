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
 * FACE SLICE: the band under the title is reserved for the face and stays
 * empty until that slice. Tools: a pick runs here (slices 3 and 4): reminders, calendar, mail and notes through the
 * file syscalls, weather through SYS_SYSINFO, "open <app>" through SYS_LAUNCH_REQUEST.
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
#define FACE_H 64       /* FACE SLICE: reserved band under the title */
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
    char ln[260], w[82], t[264];
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

/* ---- TOOLS SLICE: chat_run_tool from kernel/chat.h, through the file syscalls ---- */

static void sertool(const char *tool, const char *res) {
    jt_write(1, "chattool=", 9);
    jt_write(1, tool, (unsigned)slen(tool));
    jt_write(1, ":", 1);
    jt_write(1, res, (unsigned)slen(res));
    jt_write(1, "\n", 1);
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
    "Bookrank", "Quotes", "Plan", "Lexly", "Toroid", "Sparkjar", "Homeqi", "Fieldbook", "Contacts", "Calculator",
    "Stocks", "Search", "Epiphany", "Portfolio", "Activity", "Clock",
};
#define NAPPS ((int)(sizeof APPNAME / sizeof APPNAME[0]))

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
    if (asking && has_word(lower, "note")) t = "read_notes";
    else if (asking && has_word(lower, "reminders")) t = "list_reminders";
    if (!t) return 0;
    scopy(tool, t, toolsz);
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
    int picked = pick(msg, tool, sizeof tool, arg, sizeof arg);
    if (!picked) { arg[0] = 0; picked = keyword_fallback(msg, tool, sizeof tool); }
    if (picked && run_tool(tool, arg, msg)) {
        push(0, ar->reply);
        status = "ready";
        return;
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
