/* shellcore: the shell engine the Terminal and Panes share (2.11.0), so the two stay in sync.
 *
 * One command line goes through three steps: sh_echo prints "~<cwd>> <line>" into the scrollback, sh_dispatch
 * runs it (clear and cd are local; anything else is "<cwd>\n<line>" sent through SYS_SHELL_RUN, whose answer is
 * printed, with the `terminal: ran=N` serial line), and the caller owns everything around it (input buffer,
 * scrollback storage, history, drawing). Output goes through a put-one-char callback, so each app keeps its own
 * scrollback layout. The cwd is a relative path from the root ("" is the root) validated with jt_readdir; the
 * kernel never learns it except per call.
 *
 * Static and header-only on purpose: ring-3 images have no .bss and one flat binary each, so there is no shared
 * library to link; the cost of "shared" here is one include.
 */
#ifndef JT_SHELLCORE_H
#define JT_SHELLCORE_H
#include "jtsys.h"

#define SH_CWD_MAX 63
#define SH_LINE_MAX 95
#define SH_OUT_MAX 2048
#define SH_REQ_MAX (SH_CWD_MAX + SH_LINE_MAX + 3)
#define SH_EXEC  0
#define SH_CLEAR 1
#define SH_CD    2

typedef void (*sh_put_fn)(char);

static int sh_seq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void sh_puts(sh_put_fn put, const char *s) { while (*s) put(*s++); }

/* cd: validate with jt_readdir (relative to the root, where cwd lives), then move the caller's cwd. */
static void sh_cd(char *cwd, const char *arg, sh_put_fn put) {
    char t[SH_CWD_MAX + 1];
    unsigned n = 0;
    if (!*arg || (arg[0] == '/' && !arg[1])) { cwd[0] = 0; return; }
    if (arg[0] == '.' && arg[1] == '.' && !arg[2]) {
        while (cwd[n]) n++;
        while (n && cwd[n - 1] != '/') n--;
        if (n) n--;
        cwd[n] = 0;
        return;
    }
    for (const char *c = cwd; *c; c++) { if (n >= SH_CWD_MAX) goto toolong; t[n++] = *c; }
    if (n) { if (n >= SH_CWD_MAX) goto toolong; t[n++] = '/'; }
    for (; *arg; arg++) { if (n >= SH_CWD_MAX) goto toolong; t[n++] = *arg; }
    t[n] = 0;
    struct jt_dirent d;
    if (jt_readdir(t, &d, 1) < 0) { sh_puts(put, "cd: no such folder\n"); return; }
    for (unsigned i = 0; i <= n; i++) cwd[i] = t[i];
    return;
toolong:
    sh_puts(put, "cd: path too long\n");
}

/* The echoed prompt line. `in` is NUL terminated. */
static void sh_echo(sh_put_fn put, const char *cwd, const char *in) {
    sh_puts(put, "~"); sh_puts(put, cwd); sh_puts(put, "> "); sh_puts(put, in); put('\n');
}

/* Runs one non-empty line. Returns SH_CLEAR (the caller empties its scrollback), SH_CD or SH_EXEC. */
static int sh_dispatch(char *cwd, const char *in, unsigned inlen, char *req, char *out, sh_put_fn put) {
    const char *a = in;
    while (*a == ' ') a++;
    if (sh_seq(in, "clear")) return SH_CLEAR;
    if (a[0] == 'c' && a[1] == 'd' && (!a[2] || a[2] == ' ')) {
        a += 2;
        while (*a == ' ') a++;
        sh_cd(cwd, a, put);
        return SH_CD;
    }
    unsigned q = 0;
    for (const char *c = cwd; *c; c++) req[q++] = *c;
    req[q++] = '\n';
    for (unsigned i = 0; i < inlen; i++) req[q++] = in[i];
    req[q] = 0;
    int n = jt_shell_run(req, out, SH_OUT_MAX);
    if (n < 0) sh_puts(put, "shell: refused\n");
    else for (int i = 0; i < n; i++) put(out[i]);
    char m[32] = "terminal: ran=";
    int l = 14, d[8], dn = 0;
    unsigned v = n < 0 ? 0 : (unsigned)n;
    if (!v) d[dn++] = 0;
    while (v && dn < 8) { d[dn++] = (int)(v % 10); v /= 10; }
    while (dn) m[l++] = (char)('0' + d[--dn]);
    m[l++] = '\n';
    jt_write(1, m, (unsigned)l);
    return SH_EXEC;
}
#endif
