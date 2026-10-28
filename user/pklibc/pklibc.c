/* pklibc (6.57.0): the C library Plank's runtime and programs need, for a Plank program built as a Joshua Tree app
   (tools/plankapp.sh). libjt gives malloc, the string functions, atoi, strtol and exit; this file gives the rest: printf with
   long long and floats, strtod, qsort, setjmp's partner longjmp (setjmp.S), time, files through jtsys.h, and _start, which
   hands main an argv in its own frame because Plank's collector scans the stack up to argv. What the box cannot do yet
   (regular expressions, running a shell command, the environment) fails the way the C library says it may. */
#include "include/stdio.h"
#include "include/stdlib.h"
#include "include/string.h"
#include "include/ctype.h"
#include "include/time.h"
#include "include/unistd.h"
#include "include/regex.h"
#include "include/dirent.h"
#include "include/sys/stat.h"
#include "include/math.h"
#include "jtsys.h"

FILE pk_stdin JT_DATA = { 0, 0, 0 }, pk_stdout JT_DATA = { 1, 0, 0 }, pk_stderr JT_DATA = { 2, 0, 0 };

int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isalpha(int c) { return isupper(c) || islower(c); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int toupper(int c) { return islower(c) ? c - 32 : c; }
int tolower(int c) { return isupper(c) ? c + 32 : c; }

/* ---- printf ---- */
struct out { char *b; size_t n, at; int fd; char buf[256]; unsigned bn; };
static void put(struct out *o, char c) {
    if (o->fd >= 0) { o->buf[o->bn++] = c; if (o->bn == sizeof o->buf) { jt_write(o->fd, o->buf, o->bn); o->bn = 0; } }
    else if (o->at + 1 < o->n) o->b[o->at] = c;
    o->at++;
}
static void puts_n(struct out *o, const char *s, size_t n) { while (n--) put(o, *s++); }
static void pad(struct out *o, int k, char c) { while (k-- > 0) put(o, c); }
static const double P10[] = { 1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22 };
static double p10(int e) {   /* 10^e, exact up to 22 */
    double r = 1;
    if (e < 0) { while (e < -22) { r /= 1e22; e += 22; } return r / P10[-e]; }
    while (e > 22) { r *= 1e22; e -= 22; }
    return r * P10[e];
}
/* v > 0, finite: its first `sig` significant digits (rounded) into d, and the power of ten of the first one. */
static int digits(double v, int sig, char *d) {
    int e = 0;
    while (v >= 1e16) { v /= 1e16; e += 16; }
    while (v < 1) { v *= 1e16; e -= 16; }
    while (v >= 10) { v /= 10; e++; }
    double m = e >= 0 ? v : v;   /* m in [1, 10) */
    unsigned long long n = (unsigned long long)(m * p10(sig - 1) + 0.5), top = (unsigned long long)p10(sig);
    if (n >= top) { n /= 10; e++; }
    for (int i = sig - 1; i >= 0; i--) { d[i] = (char)('0' + n % 10); n /= 10; }
    return e;
}
static void fmt_double(struct out *o, double v, char conv, int prec, int width, int left, int plus) {
    char s[400]; int k = 0;
    if (v != v) { puts_n(o, "nan", 3); return; }
    int neg = v < 0 || (v == 0 && 1 / v < 0);
    if (neg) v = -v;
    if (neg) s[k++] = '-'; else if (plus) s[k++] = '+';
    if (v > 1.7976931348623157e308) { s[k++] = 'i'; s[k++] = 'n'; s[k++] = 'f'; }
    else if (conv == 'f') {
        if (prec > 17) prec = 17;
        if (v >= 1e18) { conv = 'e'; goto exp_form; }
        double sc = v * p10(prec) + 0.5;
        unsigned long long n = sc >= 1.8e19 ? 0 : (unsigned long long)sc, ip = n, fp = 0, f10 = (unsigned long long)p10(prec);
        if (prec) { ip = n / f10; fp = n % f10; }
        char t[24]; int tn = 0; do { t[tn++] = (char)('0' + ip % 10); ip /= 10; } while (ip);
        while (tn) s[k++] = t[--tn];
        if (prec) { s[k++] = '.'; for (int i = prec - 1; i >= 0; i--) { s[k + i] = (char)('0' + fp % 10); fp /= 10; } k += prec; }
    } else {
exp_form:;
        int g = conv == 'g', sig = g ? (prec ? prec : 1) : prec + 1;
        if (sig > 17) sig = 17;
        char d[20]; int e = v == 0 ? 0 : digits(v, sig, d);
        if (v == 0) for (int i = 0; i < sig; i++) d[i] = '0';
        int fixed = g && e >= -4 && e < sig, last = sig;
        if (g) while (last > 1 && d[last - 1] == '0') last--;   /* %g drops trailing zeros */
        if (fixed) {
            if (e < 0) { s[k++] = '0'; s[k++] = '.'; for (int i = -1; i > e; i--) s[k++] = '0'; for (int i = 0; i < last; i++) s[k++] = d[i]; }
            else { for (int i = 0; i <= e; i++) s[k++] = d[i]; if (last > e + 1) { s[k++] = '.'; for (int i = e + 1; i < last; i++) s[k++] = d[i]; } }
        } else {
            s[k++] = d[0];
            if (last > 1) { s[k++] = '.'; for (int i = 1; i < last; i++) s[k++] = d[i]; }
            s[k++] = 'e'; s[k++] = e < 0 ? '-' : '+'; if (e < 0) e = -e;
            if (e >= 100) s[k++] = (char)('0' + e / 100);
            s[k++] = (char)('0' + e / 10 % 10); s[k++] = (char)('0' + e % 10);
        }
    }
    if (!left) pad(o, width - k, ' ');
    puts_n(o, s, (size_t)k);
    if (left) pad(o, width - k, ' ');
}
static int format(struct out *o, const char *f, va_list ap) {
    for (; *f; f++) {
        if (*f != '%') { put(o, *f); continue; }
        int left = 0, zero = 0, plus = 0, width = 0, prec = -1, lng = 0;
        for (f++; *f == '-' || *f == '0' || *f == '+' || *f == ' ' || *f == '#'; f++) { if (*f == '-') left = 1; else if (*f == '0') zero = 1; else if (*f == '+') plus = 1; }
        if (*f == '*') { width = va_arg(ap, int); f++; } else while (isdigit(*f)) width = width * 10 + *f++ - '0';
        if (*f == '.') { f++; prec = 0; if (*f == '*') { prec = va_arg(ap, int); f++; } else while (isdigit(*f)) prec = prec * 10 + *f++ - '0'; }
        while (*f == 'l' || *f == 'z' || *f == 'h' || *f == 'j' || *f == 't') { if (*f != 'h') lng = 1; f++; }
        char c = *f, t[24]; int tn = 0;
        if (!c) break;
        if (c == '%') { put(o, '%'); continue; }
        if (c == 'f' || c == 'g' || c == 'e' || c == 'F' || c == 'G' || c == 'E') { fmt_double(o, va_arg(ap, double), (char)(c | 32), prec < 0 ? 6 : prec, width, left, plus); continue; }
        if (c == 's') {
            const char *s = va_arg(ap, const char *); if (!s) s = "(null)";
            size_t n = strlen(s); if (prec >= 0 && (size_t)prec < n) n = (size_t)prec;
            if (!left) pad(o, width - (int)n, ' '); puts_n(o, s, n); if (left) pad(o, width - (int)n, ' ');
            continue;
        }
        if (c == 'c') { if (!left) pad(o, width - 1, ' '); put(o, (char)va_arg(ap, int)); if (left) pad(o, width - 1, ' '); continue; }
        unsigned long long u; int neg = 0, base = c == 'x' || c == 'X' || c == 'p' ? 16 : c == 'o' ? 8 : 10;
        if (c == 'd' || c == 'i') { long long v = lng ? va_arg(ap, long long) : va_arg(ap, int); neg = v < 0; u = neg ? 0ULL - (unsigned long long)v : (unsigned long long)v; }
        else if (c == 'p') u = (unsigned long long)va_arg(ap, void *);
        else u = lng ? va_arg(ap, unsigned long long) : va_arg(ap, unsigned);
        const char *dig = c == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
        do { t[tn++] = dig[u % (unsigned)base]; u /= (unsigned)base; } while (u);
        while (tn < prec) t[tn++] = '0';
        int len = tn + (neg || plus ? 1 : 0) + (c == 'p' ? 2 : 0);
        if (!left && !zero) pad(o, width - len, ' ');
        if (neg) put(o, '-'); else if (plus) put(o, '+');
        if (c == 'p') { put(o, '0'); put(o, 'x'); }
        if (!left && zero) pad(o, width - len, '0');
        while (tn) put(o, t[--tn]);
        if (left) pad(o, width - len, ' ');
    }
    if (o->fd >= 0 && o->bn) jt_write(o->fd, o->buf, o->bn);
    else if (o->fd < 0 && o->n) o->b[o->at < o->n ? o->at : o->n - 1] = 0;
    return (int)o->at;
}
int vsnprintf(char *b, size_t n, const char *f, va_list ap) { struct out o; o.b = b; o.n = n; o.at = 0; o.fd = -1; o.bn = 0; return format(&o, f, ap); }
int vfprintf(FILE *s, const char *f, va_list ap) { struct out o; o.b = 0; o.n = 0; o.at = 0; o.fd = s->fd; o.bn = 0; return format(&o, f, ap); }
int snprintf(char *b, size_t n, const char *f, ...) { va_list ap; va_start(ap, f); int r = vsnprintf(b, n, f, ap); va_end(ap); return r; }
int sprintf(char *b, const char *f, ...) { va_list ap; va_start(ap, f); int r = vsnprintf(b, (size_t)-1 >> 1, f, ap); va_end(ap); return r; }
int printf(const char *f, ...) { va_list ap; va_start(ap, f); int r = vfprintf(stdout, f, ap); va_end(ap); return r; }
int fprintf(FILE *s, const char *f, ...) { va_list ap; va_start(ap, f); int r = vfprintf(s, f, ap); va_end(ap); return r; }
int fputs(const char *s, FILE *f) { return jt_write(f->fd, s, (unsigned)strlen(s)) < 0 ? EOF : 0; }
int fputc(int c, FILE *f) { char ch = (char)c; return jt_write(f->fd, &ch, 1) == 1 ? c : EOF; }
int putchar(int c) { return fputc(c, stdout); }
int puts(const char *s) { fputs(s, stdout); return fputc('\n', stdout); }
int fflush(FILE *f) { (void)f; return 0; }   /* nothing is held back: format writes as it goes */

/* ---- files ---- */
FILE *fopen(const char *p, const char *m) {
    static FILE files[8] JT_DATA;
    int flags = m[0] == 'w' ? JT_O_WRONLY | JT_O_CREAT | JT_O_TRUNC : m[0] == 'a' ? JT_O_WRONLY | JT_O_CREAT | JT_O_APPEND : JT_O_RDONLY;
    for (int i = 0; i < 8; i++) if (!files[i].fd) {
        int fd = jt_open(p, flags);
        if (fd < 0) return 0;
        files[i].fd = fd; files[i].eof = files[i].err = 0;
        return &files[i];
    }
    return 0;
}
size_t fread(void *b, size_t s, size_t n, FILE *f) {
    size_t want = s * n, got = 0;
    while (got < want) { int r = jt_read(f->fd, (char *)b + got, (unsigned)(want - got > 4096 ? 4096 : want - got)); if (r <= 0) { f->eof = 1; break; } got += (size_t)r; }
    return s ? got / s : 0;
}
size_t fwrite(const void *b, size_t s, size_t n, FILE *f) {
    size_t want = s * n, put_ = 0;
    while (put_ < want) { int r = jt_write(f->fd, (const char *)b + put_, (unsigned)(want - put_ > 4096 ? 4096 : want - put_)); if (r <= 0) { f->err = 1; break; } put_ += (size_t)r; }
    return s ? put_ / s : 0;
}
int fclose(FILE *f) { int r = jt_close(f->fd); f->fd = 0; return r; }
ssize_t getline(char **line, size_t *cap, FILE *f) {   /* an app has no keyboard on stdin: only a file gives lines */
    if (f->fd == 0) return -1;
    size_t n = 0; char c;
    for (;;) {
        if (n + 2 > *cap) { size_t nc = *cap ? *cap * 2 : 128; char *b = realloc(*line, nc); if (!b) return -1; *line = b; *cap = nc; }
        int r = jt_read(f->fd, &c, 1);
        if (r <= 0) break;
        (*line)[n++] = c;
        if (c == '\n') break;
    }
    if (!n) return -1;
    (*line)[n] = 0;
    return (ssize_t)n;
}
FILE *popen(const char *c, const char *m) { (void)c; (void)m; return 0; }   /* no shell to run a command in */
int pclose(FILE *f) { (void)f; return -1; }
int remove(const char *p) { return jt_unlink(p) < 0 ? -1 : 0; }
int mkdir(const char *p, unsigned m) { (void)m; return jt_mkdir(p) < 0 ? -1 : 0; }
int stat(const char *p, struct stat *s) {
    struct jt_dirent d[1];
    if (jt_readdir(p, d, 1) >= 0) { s->st_mode = S_IFDIR; s->st_size = 0; return 0; }   /* a folder lists */
    int fd = jt_open(p, JT_O_RDONLY);
    if (fd < 0) return -1;
    jt_close(fd); s->st_mode = 0100000; s->st_size = 0;
    return 0;
}
static struct jt_dirent dir_ents[64] JT_DATA; static int dir_n JT_DATA, dir_at JT_DATA; static DIR the_dir JT_DATA; static struct dirent dir_out JT_DATA;
DIR *opendir(const char *p) { int r = jt_readdir(p, dir_ents, 64); if (r < 0) return 0; dir_n = r; dir_at = 0; return &the_dir; }
struct dirent *readdir(DIR *d) {
    (void)d;
    if (dir_at >= dir_n) return 0;
    unsigned i = 0; for (; dir_ents[dir_at].name[i] && i < sizeof dir_out.d_name - 1; i++) dir_out.d_name[i] = dir_ents[dir_at].name[i];
    dir_out.d_name[i] = 0; dir_at++;
    return &dir_out;
}
int closedir(DIR *d) { (void)d; return 0; }
char *getcwd(char *b, size_t n) { if (n < 2) return 0; b[0] = '/'; b[1] = 0; return b; }   /* an app's files start at its own folder */
int getpid(void) { return jt_getpid(); }
char *getenv(const char *n) { (void)n; return 0; }
int regcomp(regex_t *r, const char *p, int f) { (void)r; (void)p; (void)f; return 1; }
int regexec(const regex_t *r, const char *s, size_t n, regmatch_t *m, int f) { (void)r; (void)s; (void)n; (void)m; (void)f; return 1; }
void regfree(regex_t *r) { (void)r; }
size_t regerror(int e, const regex_t *r, char *b, size_t n) { (void)e; (void)r; return (size_t)snprintf(b, n, "no regular expressions on Joshua Tree yet"); }
void abort(void) { jt_exit(134); __builtin_unreachable(); }

/* ---- numbers ---- */
long long strtoll(const char *s, char **e, int base) {
    const char *p = s; int neg = 0; unsigned long long v = 0; int any = 0;
    while (isspace(*p)) p++;
    if (*p == '-' || *p == '+') neg = *p++ == '-';
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] | 32) == 'x') { p += 2; base = 16; } else if (!base) base = 10;
    for (;; p++) {
        int d = isdigit(*p) ? *p - '0' : isalpha(*p) ? (*p | 32) - 'a' + 10 : 99;
        if (d >= base) break;
        v = v * (unsigned)base + (unsigned)d; any = 1;
    }
    if (e) *e = (char *)(any ? p : s);
    return neg ? -(long long)v : (long long)v;
}
long long atoll(const char *s) { return strtoll(s, 0, 10); }
unsigned long strtoul(const char *s, char **e, int b) { return (unsigned long)strtoll(s, e, b); }
double strtod(const char *s, char **e) {
    const char *p = s; int neg = 0, ex = 0, any = 0, nd = 0; unsigned long long m = 0;
    while (isspace(*p)) p++;
    if (*p == '-' || *p == '+') neg = *p++ == '-';
    if ((p[0] | 32) == 'i' && (p[1] | 32) == 'n' && (p[2] | 32) == 'f') { if (e) *e = (char *)p + 3; return neg ? -__builtin_inf() : __builtin_inf(); }
    if ((p[0] | 32) == 'n' && (p[1] | 32) == 'a' && (p[2] | 32) == 'n') { if (e) *e = (char *)p + 3; return __builtin_nan(""); }
    for (; isdigit(*p); p++, any = 1) { if (nd < 19) { m = m * 10 + (unsigned)(*p - '0'); if (m) nd++; } else ex++; }
    if (*p == '.') for (p++; isdigit(*p); p++, any = 1) if (nd < 19) { m = m * 10 + (unsigned)(*p - '0'); ex--; if (m) nd++; }
    if (any && (*p | 32) == 'e') {
        const char *q = p + 1; int en = 0, eneg = 0;
        if (*q == '-' || *q == '+') eneg = *q++ == '-';
        if (isdigit(*q)) { while (isdigit(*q)) { if (en < 10000) en = en * 10 + *q - '0'; q++; } ex += eneg ? -en : en; p = q; }
    }
    if (e) *e = (char *)(any ? p : s);
    double v = (double)m;
    if (ex < -340) v = 0; else if (ex > 310) v = m ? __builtin_inf() : 0; else if (ex < 0) v /= p10(-ex); else v *= p10(ex);
    return neg ? -v : v;
}
static void swap(char *a, char *b, size_t s) { while (s--) { char t = *a; *a++ = *b; *b++ = t; } }
void qsort(void *base, size_t n, size_t s, int (*cmp)(const void *, const void *)) {   /* heapsort: n log n always, no extra memory */
    char *b = base;
    for (size_t start = n / 2; start-- > 0;) for (size_t r = start;;) { size_t c = 2 * r + 1; if (c >= n) break; if (c + 1 < n && cmp(b + c * s, b + (c + 1) * s) < 0) c++; if (cmp(b + r * s, b + c * s) >= 0) break; swap(b + r * s, b + c * s, s); r = c; }
    for (size_t end = n; end-- > 1;) {
        swap(b, b + end * s, s);
        for (size_t r = 0;;) { size_t c = 2 * r + 1; if (c >= end) break; if (c + 1 < end && cmp(b + c * s, b + (c + 1) * s) < 0) c++; if (cmp(b + r * s, b + c * s) >= 0) break; swap(b + r * s, b + c * s, s); r = c; }
    }
}

/* ---- math: what Plank's builtins call (sqrt, floor, ceil, round are single instructions) ---- */
double fabs(double x) { return __builtin_fabs(x); }
double sqrt(double x) { return __builtin_sqrt(x); }
double floor(double x) { return __builtin_floor(x); }
double ceil(double x) { return __builtin_ceil(x); }
double round(double x) { return __builtin_round(x); }
double trunc(double x) { return __builtin_trunc(x); }
double fmod(double x, double y) { return y == 0 ? __builtin_nan("") : x - trunc(x / y) * y; }
double exp(double x) {   /* e^x = 2^k e^r, |r| <= ln2/2, r's series to 1e-17 */
    if (x != x) return x;
    if (x > 709.8) return __builtin_inf();
    if (x < -745.2) return 0;
    double k = round(x / 0.6931471805599453), r = x - k * 0.6931471805599453, t = 1, s = 1;
    for (int i = 1; i < 22; i++) { t *= r / i; s += t; }
    long long kk = (long long)k; double two = kk < 0 ? 0.5 : 2;
    for (long long i = kk < 0 ? -kk : kk; i; i--) s *= two;
    return s;
}
double log(double x) {   /* x = m 2^k, ln m from atanh's series, m in [sqrt(1/2), sqrt(2)) */
    if (x != x || x < 0) return __builtin_nan("");
    if (x == 0) return -__builtin_inf();
    if (x > 1.7976931348623157e308) return x;
    int k = 0;
    while (x >= 1.4142135623730951) { x /= 2; k++; }
    while (x < 0.7071067811865476) { x *= 2; k--; }
    double z = (x - 1) / (x + 1), z2 = z * z, t = z, s = 0;
    for (int i = 1; i < 60; i += 2) { s += t / i; t *= z2; }
    return 2 * s + k * 0.6931471805599453;
}
double pow(double x, double y) {
    if (y == 0) return 1;
    if (y == trunc(y) && fabs(y) < 1e18) {   /* a whole power: by squaring, exact where the answer is */
        long long n = (long long)y; double r = 1, b = x; unsigned long long u = n < 0 ? 0ULL - (unsigned long long)n : (unsigned long long)n;
        while (u) { if (u & 1) r *= b; b *= b; u >>= 1; }
        return n < 0 ? 1 / r : r;
    }
    if (x < 0) return __builtin_nan("");
    return exp(y * log(x));
}

/* ---- time: the kernel's clock, UTC ---- */
time_t time(time_t *t) { unsigned s = 0; jt_time(&s); if (t) *t = s; return s; }
int clock_gettime(int c, struct timespec *t) { (void)c; t->tv_sec = time(0); t->tv_nsec = 0; return 0; }
int nanosleep(const struct timespec *t, struct timespec *r) {   /* the app yields until the second turns; a shorter sleep is one yield */
    (void)r;
    time_t end = time(0) + t->tv_sec;
    do jt_sched_yield(); while (time(0) < end);
    return 0;
}
struct tm *localtime(const time_t *t) {   /* days to a civil date (Howard Hinnant's algorithm) */
    static struct tm tm JT_DATA;
    long long s = *t, d = s / 86400, r = s % 86400;
    if (r < 0) { r += 86400; d--; }
    tm.tm_hour = (int)(r / 3600); tm.tm_min = (int)(r / 60 % 60); tm.tm_sec = (int)(r % 60);
    tm.tm_wday = (int)((d % 7 + 11) % 7);
    long long z = d + 719468, era = (z >= 0 ? z : z - 146096) / 146097, doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153, day = doy - (153 * mp + 2) / 5 + 1, mon = mp < 10 ? mp + 3 : mp - 9;
    tm.tm_year = (int)(yoe + era * 400 + (mon <= 2) - 1900); tm.tm_mon = (int)mon - 1; tm.tm_mday = (int)day; tm.tm_yday = 0; tm.tm_isdst = 0;
    return &tm;
}
time_t mktime(struct tm *tm) {   /* a civil date and time, read as UTC, back to seconds (Hinnant's days_from_civil) */
    long long y = tm->tm_year + 1900LL + tm->tm_mon / 12, m = tm->tm_mon % 12 + 1;
    if (m <= 0) { m += 12; y--; }
    y -= m <= 2;
    long long era = (y >= 0 ? y : y - 399) / 400, yoe = y - era * 400, doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + tm->tm_mday - 1;
    long long days = era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
    return days * 86400 + tm->tm_hour * 3600LL + tm->tm_min * 60LL + tm->tm_sec;
}
char *strptime(const char *s, const char *f, struct tm *tm) {   /* %Y %m %d %H %M %S and literal characters, what Plank's dates use */
    for (; *f; f++) {
        if (*f != '%') { if (*s != *f) return 0; s++; continue; }
        f++;
        int v = 0, n = 0, most = *f == 'Y' ? 4 : 2;
        while (n < most && isdigit(*s)) { v = v * 10 + *s++ - '0'; n++; }
        if (!n) return 0;
        if (*f == 'Y') tm->tm_year = v - 1900; else if (*f == 'm') tm->tm_mon = v - 1; else if (*f == 'd') tm->tm_mday = v;
        else if (*f == 'H') tm->tm_hour = v; else if (*f == 'M') tm->tm_min = v; else if (*f == 'S') tm->tm_sec = v; else return 0;
    }
    return (char *)s;
}
size_t strftime(char *b, size_t n, const char *f, const struct tm *tm) {   /* %Y %m %d %H %M %S %%; anything else as written */
    size_t k = 0;
    for (; *f && k + 1 < n; f++) {
        if (*f != '%' || !f[1]) { b[k++] = *f; continue; }
        f++;
        int v = *f == 'Y' ? tm->tm_year + 1900 : *f == 'm' ? tm->tm_mon + 1 : *f == 'd' ? tm->tm_mday : *f == 'H' ? tm->tm_hour : *f == 'M' ? tm->tm_min : *f == 'S' ? tm->tm_sec : -1;
        if (v < 0) { b[k++] = *f; continue; }
        k += (size_t)snprintf(b + k, n - k, *f == 'Y' ? "%04d" : "%02d", v);
        if (k >= n) k = n - 1;
    }
    b[k] = 0;
    return k;
}

/* ---- the start: argv lives in this frame, the top of what the collector scans ---- */
int main(int argc, char **argv);
__attribute__((section(".text.start"), used))
void _start(void) {
    char *argv[2] = { "plank", 0 };
    jt_exit(main(1, argv));
}
