/* Fuzzes arch/arm64/mail.c's IMAP and MIME readers on the host under ASan and UBSan, run by
   tools/checks/mail-fuzz-check.sh. A mail server's replies (exists, next_fetch and the From and Subject fields the inbox
   list reads) and every message's body (pick_part's multipart walk, unqp, unb64) are bytes a stranger can choose, on a
   kernel with no memory protection. mail.c is included whole, so its static functions are reachable, with the kernel it
   calls stubbed out below. Every input sits in a buffer of exactly its size, so a read past the reply's end is a
   sanitizer report even where the kernel's reply buffer is bigger. A hang is caught by alarm(1). Seconds, no QEMU. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>

static void out_null(const char *s) { (void)s; }
void (*cmd_out)(const char *s) = out_null;
void cmd_dec(unsigned v) { (void)v; }
char current_user[40];
void accounts_wipe(void *p, unsigned n) { memset(p, 0, n); }
unsigned long heap_mark(void) { return 0; }
void heap_release(unsigned long m) { (void)m; }
void ask_mail_password(const char *user) { (void)user; }
void browser_render(const char *html, unsigned n, const char *name) { (void)html; (void)n; (void)name; }
int keychain_get(const char *item, char *out, unsigned max) { (void)item; (void)out; (void)max; return 0; }
int keychain_set(const char *item, const char *value) { (void)item; (void)value; return 1; }
void *kmalloc(unsigned int n) { return malloc(n ? n : 1); }
int http_resolve_host(const char *host, unsigned int *ip) { (void)host; *ip = 0; return 0; }
int tcp_open(unsigned int dest_ip, unsigned short dest_port) { (void)dest_ip; (void)dest_port; return 0; }
int tcp_write(const void *data, unsigned int len) { (void)data; (void)len; return 0; }
int tcp_read(void *buf, unsigned int max, unsigned int timeout_ticks) { (void)buf; (void)max; (void)timeout_ticks; return -1; }
void tcp_close(void) {}
int tls_connect(unsigned ip, unsigned short port, const char *host) { (void)ip; (void)port; (void)host; return -1; }
int tls_handshake(void) { return -1; }
int tls_last_error(void) { return 0; }
int tls_send(const void *buf, unsigned n) { (void)buf; (void)n; return -1; }
int tls_recv(char *buf, unsigned max, unsigned ticks) { (void)buf; (void)max; (void)ticks; return -1; }
void tls_close(void) {}
int tls_upgrade(const char *host) { (void)host; return -1; }

#include "../../arch/arm64/mail.c"

static const char LIST[] =
    "* 3 EXISTS\r\n* 0 RECENT\r\n* 2 FETCH (BODY[HEADER.FIELDS (FROM SUBJECT)] {58}\r\nFrom: \"Sam Lee\" <sam@example.com>\r\n"
    "Subject: Lunch?\r\n\r\n)\r\n* 3 FETCH (BODY[HEADER.FIELDS (FROM SUBJECT)] {40}\r\nFrom: bea@example.com\r\nSubject: Notes\r\n\r\n)\r\n"
    "a2 OK FETCH done\r\n";
static const char MSG[] =
    "From: Sam <sam@example.com>\r\nContent-Type: multipart/alternative; boundary=\"XyZ\"\r\nContent-Transfer-Encoding: 7bit\r\n\r\n"
    "--XyZ\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Transfer-Encoding: quoted-printable\r\n\r\nLunch at noon=3F=\r\n by =E2=80=94 the lake\r\n"
    "--XyZ\r\nContent-Type: text/html\r\nContent-Transfer-Encoding: base64\r\n\r\nPGI+THVuY2g8L2I+IGF0IG5vb24/\r\n--XyZ--\r\n";

static sigjmp_buf g_jmp;
static long g_cases, g_hangs;
static void on_alarm(int sig) { (void)sig; siglongjmp(g_jmp, 1); }

static void run_list(const char *in, unsigned n) {   /* what fetch_inbox does with a reply, minus the network */
    rb = malloc(n ? n : 1); memcpy(rb, in, n); rlen = n;
    exists();
    unsigned at = 0, seq, ln, rows = 0; const char *lit;
    while (rows++ < 20 && next_fetch(&at, &seq, &lit, &ln)) {
        char f[96], s[72]; field(lit, ln, "From:", f, sizeof f); field(lit, ln, "Subject:", s, sizeof s);
    }
    free(rb); rb = 0; rlen = 0;
}
static void run_msg(const char *in, unsigned n) {   /* headers, the blank line, then pick_part on the body */
    char *b = malloc(n ? n : 1); memcpy(b, in, n);
    unsigned h = 0; while (h + 3 < n && !(b[h] == '\r' && b[h + 1] == '\n' && b[h + 2] == '\r' && b[h + 3] == '\n')) h++;
    unsigned body = h + 4 <= n ? h + 4 : n, bn = n - body; int html;
    pick_part(b, h, b + body, &bn, &html);
    free(b);
    b = malloc(n ? n : 1); memcpy(b, in, n); unqp(b, n); free(b);
    b = malloc(n ? n : 1); memcpy(b, in, n); unb64(b, n); free(b);
}
static void one(void (*f)(const char *, unsigned), const char *in, unsigned n) {
    g_cases++;
    if (sigsetjmp(g_jmp, 1)) { g_hangs++; fprintf(stderr, "HANG at a %u byte input\n", n); return; }
    alarm(1); f(in, n); alarm(0);
}
static unsigned rnd_state = 777;
static unsigned rnd(void) { rnd_state = rnd_state * 1103515245u + 12345u; return rnd_state >> 8; }
static void fuzz(const char *name, void (*f)(const char *, unsigned), const char *s, unsigned n, int rounds) {
    long before = g_cases;
    for (unsigned k = 0; k <= n; k++) one(f, s, k);
    static const char nasty[] = "{}*-=\r\n \"<>:;0123456789";
    char *m = malloc(n + 64);
    for (int t = 0; t < rounds; t++) {
        unsigned len = n; memcpy(m, s, n);
        int edits = 1 + (int)(rnd() % 6);
        for (int e = 0; e < edits; e++) {
            unsigned p = len ? rnd() % len : 0, op = rnd() % 4;
            if (op == 0 && len) m[p] = (char)rnd();
            else if (op == 1 && len) m[p] = nasty[rnd() % (sizeof nasty - 1)];
            else if (op == 2 && len + 1 < n + 64) { memmove(m + p + 1, m + p, len - p); m[p] = nasty[rnd() % (sizeof nasty - 1)]; len++; }
            else if (len) { memmove(m + p, m + p + 1, len - p - 1); len--; }
        }
        one(f, m, len);
    }
    free(m);
    fprintf(stderr, "mail-fuzz: %s, %ld cases\n", name, g_cases - before);
}
int main(void) {
    signal(SIGALRM, on_alarm);
    fuzz("imap list", run_list, LIST, sizeof LIST - 1, 4000);
    fuzz("message", run_msg, MSG, sizeof MSG - 1, 4000);
    puts(g_hangs ? "MAIL FUZZ: FAIL (hang)" : "MAIL FUZZ: PASS");
    return g_hangs != 0;
}
