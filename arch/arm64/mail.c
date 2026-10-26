/* Mail over IMAP and TLS (6.18.0): the inbox's newest headers and one message's text, read only, from the Terminal.
   `mail login HOST[:PORT] USER` asks for the app password with the stars sudo uses (cmd.c's PW_MAIL). The password lives in
   memory only, never on the card, the screen or the UART, and is wiped by `mail logout` and whenever the signed-in
   account is not the one that logged in to mail. It goes over TLS to port 993 alone, the server's certificate checked
   against the Mozilla roots like any https page; there is no plain-text IMAP (HOST:PORT moves the port, never the TLS). `mail` lists the newest ten, `mail N`
   shows message N through the browser's renderer (browser_render), plain text or HTML, its remote images never fetched.
   tools/checks/arm64-mail-check.py runs it against a fixture IMAP server. */
#include "tls.h"
#include "../../drivers/http.h"

extern void (*cmd_out)(const char *s);   /* cmd.c */
void cmd_dec(unsigned v);
extern char current_user[];              /* login.c: the signed-in account, "" when nobody is */
void accounts_wipe(void *p, unsigned n);
void *kmalloc(unsigned int n);
unsigned long heap_mark(void);
void heap_release(unsigned long m);
void ask_mail_password(const char *user);                            /* cmd.c: the stars prompt, then mail_password() */
void browser_render(const char *html, unsigned n, const char *name);  /* browser.c */

#define MAIL_PORT 993
#define MAIL_LIST 10
#define REPLY_MAX (2u << 20)   /* one command's reply; a message bigger than this is cut */
static char m_host[128], m_user[96], m_pass[128], m_owner[40];
static unsigned short m_port = MAIL_PORT;   /* HOST:PORT picks another, still over TLS */
static int m_ready;
static unsigned m_seq[MAIL_LIST], m_n;   /* the listed messages' sequence numbers, newest first */
static char *rb; static unsigned rlen;
static unsigned m_tag;

static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static int starts(const char *s, const char *w) { for (; *w; s++, w++) if ((*s | 32) != (*w | 32)) return 0; return 1; }
static int same(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void copy(char *d, const char *s, unsigned n, unsigned max) { if (n >= max) n = max - 1; for (unsigned i = 0; i < n; i++) d[i] = s[i]; d[n] = 0; }
static void forget(void) { accounts_wipe(m_pass, sizeof m_pass); m_ready = 0; m_n = 0; m_owner[0] = 0; }
static int mine(void) { if (m_ready && !same(m_owner, current_user)) forget(); return m_ready; }   /* another account, or a sign-out: wiped */

/* One tagged command, its whole reply into rb (literals {n} skipped as they come, so a message holding "a03 OK" cannot end
   it early). 1 on OK, 0 on NO or BAD, -1 when the connection failed or the reply would not fit. */
static int imap(const char *cmd, unsigned n) {
    char line[600], tag[4] = { 'a', (char)('0' + m_tag / 10 % 10), (char)('0' + m_tag % 10), 0 };
    m_tag++;
    if (n + 7 > sizeof line) return -1;
    unsigned o = 0; for (unsigned i = 0; i < 3; i++) line[o++] = tag[i];
    line[o++] = ' '; for (unsigned i = 0; i < n; i++) line[o++] = cmd[i];
    line[o++] = '\r'; line[o++] = '\n';
    int sent = tls_send(line, o);
    accounts_wipe(line, sizeof line);   /* LOGIN carries the password */
    if (sent < 0) return -1;
    rlen = 0; unsigned p = 0;
    for (;;) {
        for (;;) {   /* every whole line from p */
            unsigned e = p; while (e + 1 < rlen && !(rb[e] == '\r' && rb[e + 1] == '\n')) e++;
            if (e + 1 >= rlen) break;
            if (e > p && rb[e - 1] == '}') {   /* a literal follows: {n}, then n bytes, then the line goes on */
                unsigned b = e - 1, v = 0; while (b > p && rb[b - 1] != '{') b--;
                for (unsigned k = b; k < e - 1 && v < REPLY_MAX; k++) v = v * 10 + (unsigned)(rb[k] - '0');
                if (e + 2 + v > rlen) break;
                p = e + 2 + v; continue;
            }
            if (rb[p] == 'a' && rb[p + 1] == tag[1] && rb[p + 2] == tag[2] && rb[p + 3] == ' ') return starts(rb + p + 4, "OK") ? 1 : 0;
            p = e + 2;
        }
        if (rlen + 1 >= REPLY_MAX) return -1;
        int r = tls_recv(rb + rlen, REPLY_MAX - rlen - 1, 1000);
        if (r <= 0) return -1;
        rlen += (unsigned)r; rb[rlen] = 0;
    }
}
static unsigned quoted(char *d, unsigned o, const char *s) {   /* an IMAP quoted string */
    d[o++] = '"';
    for (; *s; s++) { if (*s == '"' || *s == '\\') d[o++] = '\\'; d[o++] = *s; }
    d[o++] = '"';
    return o;
}
/* Connected, greeted and logged in, with rb ready: 1. Otherwise 0 and one line saying why. The caller holds a heap mark. */
static int open_session(void) {
    unsigned ip;
    rb = kmalloc(REPLY_MAX);
    if (!rb) { cmd_out("mail: out of memory\n"); return 0; }
    if (!http_resolve_host(m_host, &ip)) { cmd_out("mail: no such host\n"); return 0; }
    if (tls_connect(ip, m_port, m_host) < 0 || tls_handshake() < 0) {
        int e = tls_last_error();
        cmd_out("mail: could not reach the server securely");
        if (e == 62) cmd_out(" (its certificate is not trusted)");
        else if (e == 56) cmd_out(" (its certificate is for another name)");
        cmd_out("\n"); return 0;
    }
    int r = tls_recv(rb, REPLY_MAX - 1, 1000);
    if (r < 4 || !starts(rb, "* OK")) { cmd_out("mail: the server did not greet\n"); tls_close(); return 0; }
    char login[400]; unsigned o = 0;
    for (const char *p = "LOGIN "; *p; p++) login[o++] = *p;
    o = quoted(login, o, m_user); login[o++] = ' '; o = quoted(login, o, m_pass);
    int ok = imap(login, o);
    accounts_wipe(login, sizeof login);
    if (ok != 1) { cmd_out(ok == 0 ? "mail: the server refused the user or the app password\n" : "mail: the connection dropped\n"); tls_close(); return 0; }
    return 1;
}
static void close_session(void) { imap("LOGOUT", 6); tls_close(); }
static unsigned exists(void) {   /* "* N EXISTS" off SELECT's reply */
    for (unsigned i = 0; i + 8 < rlen; i++) if ((i == 0 || rb[i - 1] == '\n') && rb[i] == '*') {
        unsigned k = i + 2, v = 0; while (rb[k] >= '0' && rb[k] <= '9') v = v * 10 + (unsigned)(rb[k++] - '0');
        if (starts(rb + k, " EXISTS")) return v;
    }
    return 0;
}
/* The value of a header field in h[0..n), up to its line end, into out ("" when it is not there). */
static void field(const char *h, unsigned n, const char *name, char *out, unsigned max) {
    out[0] = 0; unsigned k = slen(name);
    for (unsigned i = 0; i + k < n; i++) if ((i == 0 || h[i - 1] == '\n') && starts(h + i, name)) {
        unsigned a = i + k; while (a < n && h[a] == ' ') a++;
        unsigned e = a; while (e < n && h[e] != '\r' && h[e] != '\n') e++;
        copy(out, h + a, e - a, max); return;
    }
}
/* For each "* K FETCH (... {n}" in rb: K and the literal. 1 while there is another, from *at on. */
static int next_fetch(unsigned *at, unsigned *seq, const char **lit, unsigned *litn) {
    for (unsigned i = *at; i + 2 < rlen; i++) if ((i == 0 || rb[i - 1] == '\n') && rb[i] == '*' && rb[i + 1] == ' ') {
        unsigned k = i + 2, v = 0; while (rb[k] >= '0' && rb[k] <= '9') v = v * 10 + (unsigned)(rb[k++] - '0');
        if (!starts(rb + k, " FETCH")) continue;
        unsigned e = k; while (e + 1 < rlen && !(rb[e] == '\r' && rb[e + 1] == '\n')) e++;
        if (e == k || rb[e - 1] != '}') continue;
        unsigned b = e - 1, len = 0; while (b > k && rb[b - 1] != '{') b--;
        for (unsigned q = b; q < e - 1; q++) len = len * 10 + (unsigned)(rb[q] - '0');
        if (e + 2 + len > rlen) return 0;
        *seq = v; *lit = rb + e + 2; *litn = len; *at = e + 2 + len;
        return 1;
    }
    return 0;
}
static void list(void) {
    unsigned long mark = heap_mark();
    if (!open_session()) { heap_release(mark); return; }
    if (imap("SELECT INBOX", 12) != 1) { cmd_out("mail: no inbox\n"); close_session(); heap_release(mark); return; }
    unsigned total = exists();
    m_n = 0;
    if (!total) { cmd_out("mail: the inbox is empty\n"); close_session(); heap_release(mark); return; }
    unsigned lo = total > MAIL_LIST ? total - MAIL_LIST + 1 : 1;
    char cmd[96]; unsigned o = 0;
    for (const char *p = "FETCH "; *p; p++) cmd[o++] = *p;
    char d[12]; unsigned z = 0, v = lo; do d[z++] = (char)('0' + v % 10); while (v /= 10); while (z) cmd[o++] = d[--z];
    cmd[o++] = ':'; v = total; do d[z++] = (char)('0' + v % 10); while (v /= 10); while (z) cmd[o++] = d[--z];
    for (const char *p = " (BODY.PEEK[HEADER.FIELDS (FROM SUBJECT)])"; *p; p++) cmd[o++] = *p;
    if (imap(cmd, o) != 1) { cmd_out("mail: the server would not list the inbox\n"); close_session(); heap_release(mark); return; }
    struct { unsigned seq; char from[40], subj[72]; } row[MAIL_LIST]; unsigned nr = 0;
    unsigned at = 0, seq, ln; const char *lit;
    while (nr < MAIL_LIST && next_fetch(&at, &seq, &lit, &ln)) {
        row[nr].seq = seq;
        char f[96]; field(lit, ln, "From:", f, sizeof f);
        unsigned lt = 0; while (f[lt] && f[lt] != '<') lt++;   /* the name before <address>, else the address */
        if (lt) { while (lt && f[lt - 1] == ' ') lt--; copy(row[nr].from, f[0] == '"' ? f + 1 : f, f[0] == '"' ? lt - 2 : lt, sizeof row[nr].from); }
        else copy(row[nr].from, f, slen(f), sizeof row[nr].from);
        field(lit, ln, "Subject:", row[nr].subj, sizeof row[nr].subj);
        nr++;
    }
    close_session();
    cmd_out("mail: "); cmd_dec(total); cmd_out(total == 1 ? " message, the newest first\n" : " messages, the newest first\n");
    for (unsigned i = nr; i-- > 0;) {   /* newest first */
        m_seq[m_n++] = row[i].seq;
        cmd_dec(m_n); cmd_out("  "); cmd_out(row[i].from[0] ? row[i].from : "(no sender)"); cmd_out(": "); cmd_out(row[i].subj[0] ? row[i].subj : "(no subject)"); cmd_out("\n");
    }
    cmd_out("mail N opens one\n");
    heap_release(mark);
}

/* A part's body decoded: quoted-printable or base64 in place. The new length. */
static unsigned unqp(char *s, unsigned n) {
    unsigned w = 0;
    for (unsigned r = 0; r < n; r++) {
        if (s[r] != '=') { s[w++] = s[r]; continue; }
        if (r + 1 < n && s[r + 1] == '\n') { r++; continue; }                                   /* =\n, a soft break */
        if (r + 2 < n && s[r + 1] == '\r' && s[r + 2] == '\n') { r += 2; continue; }
        int hi = r + 2 < n ? (s[r + 1] >= 'A' ? (s[r + 1] | 32) - 'a' + 10 : s[r + 1] - '0') : -1;
        int lo = r + 2 < n ? (s[r + 2] >= 'A' ? (s[r + 2] | 32) - 'a' + 10 : s[r + 2] - '0') : -1;
        if (hi < 0 || hi > 15 || lo < 0 || lo > 15) { s[w++] = s[r]; continue; }
        s[w++] = (char)(hi << 4 | lo); r += 2;
    }
    return w;
}
static unsigned unb64(char *s, unsigned n) {
    unsigned w = 0, acc = 0, bits = 0;
    for (unsigned r = 0; r < n; r++) {
        char c = s[r]; int v = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
        if (v < 0) continue;
        acc = acc << 6 | (unsigned)v; bits += 6;
        if (bits >= 8) { bits -= 8; s[w++] = (char)(acc >> bits & 0xFF); }
    }
    return w;
}
/* The part to show out of a message (headers h, body b): with a multipart boundary, its text/html part, else its
   text/plain one; otherwise the body itself. Decoded; *html says which. */
static char *pick_part(const char *h, unsigned hn, char *b, unsigned *bn, int *html) {
    char ct[200], cte[40]; field(h, hn, "Content-Type:", ct, sizeof ct); field(h, hn, "Content-Transfer-Encoding:", cte, sizeof cte);
    char *bnd = 0; unsigned bl = 0;
    for (char *p = ct; *p; p++) if (starts(p, "boundary=")) { bnd = p + 9; if (*bnd == '"') bnd++; while (bnd[bl] && bnd[bl] != '"' && bnd[bl] != ';') bl++; break; }
    if (!bnd) {
        int has = 0; for (char *p = ct; *p; p++) if (starts(p, "text/html")) has = 1;
        *html = has;
        if (starts(cte, "quoted-printable")) *bn = unqp(b, *bn); else if (starts(cte, "base64")) *bn = unb64(b, *bn);
        return b;
    }
    char *best = 0; unsigned best_n = 0; int best_html = 0;
    for (unsigned i = 0; i + bl + 2 < *bn; i++) {
        if (b[i] != '-' || b[i + 1] != '-' || (i && b[i - 1] != '\n')) continue;
        unsigned k = 0; while (k < bl && b[i + 2 + k] == bnd[k]) k++;
        if (k < bl) continue;
        unsigned ph = i + 2 + bl; while (ph < *bn && b[ph] != '\n') ph++;
        ph++;
        unsigned pe = ph; while (pe + 3 < *bn && !(b[pe] == '\n' && (b[pe + 1] == '\n' || (b[pe + 1] == '\r' && b[pe + 2] == '\n')))) pe++;   /* the part's own headers */
        unsigned body = pe + (b[pe + 1] == '\r' ? 3 : 2), end = body;
        while (end + 2 < *bn && !(b[end] == '\n' && b[end + 1] == '-' && b[end + 2] == '-')) end++;
        char pct[120], pcte[40]; field(b + ph, pe - ph, "Content-Type:", pct, sizeof pct); field(b + ph, pe - ph, "Content-Transfer-Encoding:", pcte, sizeof pcte);
        int is_html = starts(pct, "text/html"), is_plain = starts(pct, "text/plain");
        if ((is_html && !best_html) || (is_plain && !best)) {
            best = b + body; best_n = end - body; best_html = is_html;
            if (starts(pcte, "quoted-printable")) best_n = unqp(best, best_n); else if (starts(pcte, "base64")) best_n = unb64(best, best_n);
        }
        i = end;
    }
    if (!best) { *html = 0; return b; }
    *bn = best_n; *html = best_html; return best;
}
static void open_message(unsigned k) {
    if (!k || k > m_n) { cmd_out(m_n ? "mail: no such message\n" : "mail: list the inbox first, with mail\n"); return; }
    unsigned long mark = heap_mark();
    if (!open_session()) { heap_release(mark); return; }
    char cmd[96]; unsigned o = 0;
    for (const char *p = "SELECT INBOX"; *p; p++) cmd[o++] = *p;
    if (imap(cmd, o) != 1) { cmd_out("mail: no inbox\n"); close_session(); heap_release(mark); return; }
    o = 0; for (const char *p = "FETCH "; *p; p++) cmd[o++] = *p;
    char d[12]; unsigned z = 0, v = m_seq[k - 1]; do d[z++] = (char)('0' + v % 10); while (v /= 10); while (z) cmd[o++] = d[--z];
    for (const char *p = " (BODY.PEEK[HEADER] BODY.PEEK[TEXT])"; *p; p++) cmd[o++] = *p;
    int got = imap(cmd, o);
    if (got != 1) { cmd_out("mail: the server would not send that message\n"); close_session(); heap_release(mark); return; }
    /* The reply holds two literals: the header, then the text. LOGOUT waits until they are read: its reply reuses rb. */
    const char *hdr = 0, *txt = 0; unsigned hn = 0, tn = 0;
    for (unsigned i = 0, seen = 0; i + 1 < rlen && seen < 2; i++) if (rb[i] == '}' && rb[i + 1] == '\r') {
        unsigned b = i, len = 0; while (b && rb[b - 1] >= '0' && rb[b - 1] <= '9') b--;
        if (!b || rb[b - 1] != '{') continue;
        for (unsigned q = b; q < i; q++) len = len * 10 + (unsigned)(rb[q] - '0');
        if (i + 3 + len > rlen) break;
        if (!seen++) { hdr = rb + i + 3; hn = len; } else { txt = rb + i + 3; tn = len; }
        i += 2 + len;
    }
    if (!hdr || !txt) { cmd_out("mail: that message did not come back whole\n"); close_session(); heap_release(mark); return; }
    int html; unsigned bn = tn;
    char *body = pick_part(hdr, hn, (char *)txt, &bn, &html);
    char subj[120]; field(hdr, hn, "Subject:", subj, sizeof subj);
    char from[120]; field(hdr, hn, "From:", from, sizeof from);
    char *page = kmalloc(bn * 5 + 512);   /* before LOGOUT reuses rb, which body points into */
    if (!page) { cmd_out("mail: out of memory\n"); close_session(); heap_release(mark); return; }
    /* Plain text goes in as escaped lines; the page's title is the subject, the first line who sent it. */
    unsigned w = 0;
    #define PUT(str) for (const char *p_ = (str); *p_; p_++) page[w++] = *p_
    PUT("<html><head><title>"); for (const char *p = subj[0] ? subj : "(no subject)"; *p; p++) page[w++] = *p == '<' ? ' ' : *p;
    PUT("</title></head><body><p>From "); for (const char *p = from; *p; p++) { if (*p == '<') PUT("&lt;"); else if (*p == '>') PUT("&gt;"); else if (*p == '&') PUT("&amp;"); else page[w++] = *p; }
    PUT("</p>");
    if (html) for (unsigned i = 0; i < bn; i++) page[w++] = body[i];
    else {
        PUT("<p>");
        for (unsigned i = 0; i < bn; i++) {
            char c = body[i];
            if (c == '\r') continue;
            if (c == '\n') PUT("<br>"); else if (c == '<') PUT("&lt;"); else if (c == '>') PUT("&gt;"); else if (c == '&') PUT("&amp;"); else page[w++] = c;
        }
        PUT("</p>");
    }
    PUT("</body></html>");
    #undef PUT
    close_session();
    char name[16] = "mail:"; o = 5; z = 0; v = k; do d[z++] = (char)('0' + v % 10); while (v /= 10); while (z) name[o++] = d[--z]; name[o] = 0;
    browser_render(page, w, name);
    heap_release(mark);
}

/* cmd.c, once the app password is typed. Tried at once, so a wrong one is said now, not at the next `mail`. */
void mail_password(const char *pass) {
    copy(m_pass, pass, slen(pass), sizeof m_pass);
    copy(m_owner, current_user, slen(current_user), sizeof m_owner);
    m_ready = 1;
    unsigned long mark = heap_mark();
    int ok = open_session();
    if (ok) { close_session(); cmd_out("mail: signed in as "); cmd_out(m_user); cmd_out(" at "); cmd_out(m_host); cmd_out("; mail lists the inbox\n"); }
    else forget();
    heap_release(mark);
}
int mail_command(const char *q, unsigned n) {   /* 1 when it was a mail command */
    if (!(n == 4 || (n > 4 && q[4] == ' ')) || !starts(q, "mail")) return 0;
    mine();
    if (n == 4) { if (!m_ready) cmd_out("mail: not signed in, use mail login HOST USER\n"); else list(); return 1; }
    const char *a = q + 5; unsigned an = n - 5;
    while (an && *a == ' ') { a++; an--; }
    if (an == 6 && starts(a, "logout")) { forget(); cmd_out("mail: signed out, the app password forgotten\n"); return 1; }
    if (an > 6 && starts(a, "login ")) {
        unsigned i = 6; while (i < an && a[i] == ' ') i++;
        unsigned h0 = i; while (i < an && a[i] != ' ') i++;
        unsigned h1 = i; while (i < an && a[i] == ' ') i++;
        unsigned u0 = i; while (i < an && a[i] != ' ') i++;
        if (h1 == h0 || i == u0 || i != an || h1 - h0 >= sizeof m_host || i - u0 >= sizeof m_user) { cmd_out("mail: use mail login HOST USER (the app password is asked next)\n"); return 1; }
        for (unsigned k = u0; k < i; k++) if ((unsigned char)a[k] < 0x21) { cmd_out("mail: that user name has a control character\n"); return 1; }
        forget();
        copy(m_host, a + h0, h1 - h0, sizeof m_host);
        m_port = MAIL_PORT;
        for (unsigned c = 0; m_host[c]; c++) if (m_host[c] == ':') {
            unsigned v = 0, d = c + 1; while (m_host[d] >= '0' && m_host[d] <= '9' && v < 70000) v = v * 10 + (unsigned)(m_host[d++] - '0');
            if (m_host[d] || !v || v > 65535) { cmd_out("mail: that port is not a number from 1 to 65535\n"); m_host[0] = 0; return 1; }
            m_port = (unsigned short)v; m_host[c] = 0; break;
        }
        copy(m_user, a + u0, i - u0, sizeof m_user);
        ask_mail_password(m_user);
        return 1;
    }
    unsigned k = 0, i = 0; while (i < an && a[i] >= '0' && a[i] <= '9') k = k * 10 + (unsigned)(a[i++] - '0');
    if (i && i == an) { if (!m_ready) cmd_out("mail: not signed in, use mail login HOST USER\n"); else open_message(k); return 1; }
    cmd_out("mail: use mail, mail N, mail login HOST USER or mail logout\n");
    return 1;
}
