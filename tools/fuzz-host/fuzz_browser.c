/* Fuzzes arch/arm64/browser.c's page and header parsers on the host under ASan and UBSan, run by
   tools/checks/browser-fuzz-check.sh. Every page a site sends goes through to_text: tags, entities, script and style
   skipping, the <style> rules (css_rules, hidden_tag, the descendant tracking, bold and alignment), links, images and
   forms with their fields (field_show's splice, fill and open on each). Every reply's Set-Cookie lines go through
   take_cookies. All of it runs on a kernel with no memory protection. browser.c is included whole, so its static
   functions are reachable, with the kernel it calls stubbed out below (no network, no card, no keychain). Each sample
   is truncated at every length, then mutated a few thousand times from a fixed seed; every input is copied into a
   buffer of exactly its size. A hang is caught by alarm(1). Seconds, no QEMU. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>

static void out_null(const char *s) { (void)s; }
void (*cmd_out)(const char *s) = out_null;
void cmd_dec(unsigned v) { (void)v; }
void say_wrapped(const char *prefix, const char *s, unsigned n) { (void)prefix; (void)s; (void)n; }
unsigned long heap_mark(void) { return 0; }
void heap_release(unsigned long m) { (void)m; }
void *kmalloc(unsigned int n) { return malloc(n ? n : 1); }
int yt_page(const char *vid, char *out, unsigned max, int *status) { (void)vid; (void)out; (void)max; *status = 0; return -1; }
int yt_avi(const char *vid, unsigned cap, char *out, int *status) { (void)vid; (void)cap; (void)out; *status = 0; return -1; }
int home_active(void) { return 0; }
int home_save(const char *name, const void *data, unsigned len) { (void)name; (void)data; (void)len; return 1; }
int sd_write(const char *name, const void *data, unsigned len) { (void)name; (void)data; (void)len; return 1; }
int app_dock(const char *name) { (void)name; return 0; }
int http_date_past(const char *d, unsigned n) { return n > 0 && d[0] == 'T'; }   /* clock.c's own fuzz is arm64-clock-check.py */
char current_user[40];
void accounts_wipe(void *p, unsigned n) { memset(p, 0, n); }
int keychain_get(const char *item, char *out, unsigned max) { (void)item; (void)out; (void)max; return 0; }
int keychain_set(const char *item, const char *value) { (void)item; (void)value; return 1; }
int mail_page(const char *url, char *out, unsigned max) { (void)url; (void)out; (void)max; return -1; }
int mail_form(const char *action, const char *to, const char *subj, const char *body) { (void)action; (void)to; (void)subj; (void)body; return 0; }
unsigned int ticks(void) { return 0; }
int home_expand(const char *in, char *out, unsigned cap) { (void)in; (void)out; (void)cap; return 0; }
int home_op(int op, const char *path, const void *data, unsigned len) { (void)op; (void)path; (void)data; (void)len; return 0; }
int home_walk(char *path, int keep, char **leaf) { (void)path; (void)keep; (void)leaf; return 0; }
void home_back(void) {}
int fat_read_at(const char *name, unsigned int off, void *buf, unsigned int len) { (void)name; (void)off; (void)buf; (void)len; return -1; }
int sd_ready(void) { return 0; }
int sd_enter_jt(int make) { (void)make; return 0; }
int fat_read_file(const char *name, void *buf, unsigned bufsize) { (void)name; (void)buf; (void)bufsize; return -1; }
void fat_cwd_set(unsigned int cluster) { (void)cluster; }
int fat_chdir(const char *name) { (void)name; return 0; }
int http_resolve_host(const char *host, unsigned int *ip) { (void)host; *ip = 0; return 0; }
int http_status_of(const char *raw, unsigned int total) { (void)raw; (void)total; return 0; }
int http_body_start(const char *raw, unsigned int total) {   /* the real rule: the blank line ends the headers */
    for (unsigned i = 0; i + 3 < total; i++) if (raw[i] == '\r' && raw[i + 1] == '\n' && raw[i + 2] == '\r' && raw[i + 3] == '\n') return (int)i + 4;
    return -1;
}
int inflate_raw(const unsigned char *src, unsigned int src_len, unsigned char *dst, unsigned int dst_len) { (void)src; (void)src_len; (void)dst; (void)dst_len; return -1; }
int png_decode(const unsigned char *data, unsigned int len, unsigned char **out, unsigned int *w, unsigned int *h, unsigned int *channels) { (void)data; (void)len; (void)out; (void)w; (void)h; (void)channels; return -1; }
int jpeg_decode_scaled32(const unsigned char *data, unsigned int len, unsigned int *dst, unsigned int dw, unsigned int dh, unsigned int *w, unsigned int *h) { (void)data; (void)len; (void)dst; (void)dw; (void)dh; (void)w; (void)h; return -1; }
unsigned int net_dns_wait_ticks, net_connect_wait_ticks;
unsigned int net_get_gateway(void) { return 0; }
int net_last_error(void) { return 0; }
int tcp_get_timeout(unsigned int dest_ip, unsigned short dest_port, const void *request, unsigned int request_len, void *response, unsigned int response_maxlen, unsigned int reply_timeout_ticks) {
    (void)dest_ip; (void)dest_port; (void)request; (void)request_len; (void)response; (void)response_maxlen; (void)reply_timeout_ticks; return -1;
}
int tls_connect(unsigned ip, unsigned short port, const char *host) { (void)ip; (void)port; (void)host; return -1; }
int tls_handshake(void) { return -1; }
int tls_exchange(const void *request, unsigned request_len, char *out, unsigned max) { (void)request; (void)request_len; (void)out; (void)max; return -1; }
int tls_last_error(void) { return 0; }

#include "../../arch/arm64/browser.c"

static const char PAGE[] =
    "<html><head><title>Fuzz &amp; page</title><style>/* c */ .menu, #ad { display : none } nav li { display:none }"
    " .lead{font-weight:700} .note span{font-weight:bold} .mid{text-align:center} .box div{text-align:right} @media x{.a{display:none}}"
    " div p .deep{display:none}</style><script>if (a<b) x='</div>';</script></head><body><!-- -- --><h1>Big</h1>"
    "<nav class=\"top menu\"><a href=/x>m</a></nav><ul class=nav><li>a<ul><li>b</li></ul></li></ul><p class=lead>Lead</p>"
    "<center>C</center><div align=right>R</div><div class=box><div>bx</div></div><p>t &#65; &lt;b&gt; &nbsp;<b>bold</b>"
    "<a href='/y'>link</a><img src=/i.png><img src=\"/j.jpg\" hidden></p><form action=/s method=post><input name=q value=\"v\">"
    "<input type=password name=pw><input type=hidden name=h value=1><input type=checkbox name=c checked><input type=radio name=r value=1>"
    "<input type=radio name=r value=2 checked><select name=s><option value=a>A</option><option selected>B</option></select>"
    "<textarea name=body>line one\nline two</textarea><textarea name=t2>x</textarea><input type=submit value=Go></form></body></html>";
static const char HEAD[] =
    "HTTP/1.1 200 OK\r\nSet-Cookie: sid=abc123; Path=/; Secure; HttpOnly\r\nset-cookie: keep=1; Max-Age=3600\r\n"
    "Set-Cookie: old=; Expires=Thu, 01 Jan 1970 00:00:00 GMT\r\nSet-Cookie: =bad\r\nSet-Cookie: gone=x; max-age=0\r\n\r\nbody";

static sigjmp_buf g_jmp;
static long g_cases, g_hangs;
static void on_alarm(int sig) { (void)sig; siglongjmp(g_jmp, 1); }

static void run_page(const char *in, unsigned n) {
    char *b = malloc(n ? n : 1); memcpy(b, in, n);
    scopy(cur, "https://fuzz.example/dir/page", 29, URL_MAX);
    to_text(b, n);
    for (unsigned k = 1; k <= nlinks && k < 40; k++) {   /* every field: open toggles or steps it, fill sets it, both through field_show */
        int kind = browser_field_kind(k);
        if (kind < 0) continue;
        char cmd[64]; int len;
        if (kind >= 3 && kind <= 5) { len = snprintf(cmd, sizeof cmd, "open %u", k); browse_command(cmd, (unsigned)len); }
        len = snprintf(cmd, sizeof cmd, "fill %u some text\\nmore", k); browse_command(cmd, (unsigned)len);
        len = snprintf(cmd, sizeof cmd, "fill %u ", k); browse_command(cmd, (unsigned)len);
    }
    free(b);
}
static void run_head(const char *in, unsigned n) {
    char *b = malloc(n ? n : 1); memcpy(b, in, n);
    struct url u; parse_url("https://fuzz.example/", &u);
    take_cookies(b, n, &u);
    free(b);
}
static void one(void (*f)(const char *, unsigned), const char *in, unsigned n) {
    g_cases++;
    if (sigsetjmp(g_jmp, 1)) { g_hangs++; fprintf(stderr, "HANG at a %u byte input\n", n); return; }
    alarm(1); f(in, n); alarm(0);
}
static unsigned rnd_state = 4242;
static unsigned rnd(void) { rnd_state = rnd_state * 1103515245u + 12345u; return rnd_state >> 8; }
static void fuzz(const char *name, void (*f)(const char *, unsigned), const char *s, unsigned n, int rounds) {
    long before = g_cases;
    for (unsigned k = 0; k <= n; k++) one(f, s, k);
    static const char nasty[] = "<>&;:/\\\r\n \"'=#!-.{}*,";
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
    fprintf(stderr, "browser-fuzz: %s, %ld cases\n", name, g_cases - before);
}
int main(void) {
    signal(SIGALRM, on_alarm);
    fuzz("page", run_page, PAGE, sizeof PAGE - 1, 4000);
    fuzz("cookies", run_head, HEAD, sizeof HEAD - 1, 4000);
    puts(g_hangs ? "BROWSER FUZZ: FAIL (hang)" : "BROWSER FUZZ: PASS");
    return g_hangs != 0;
}
