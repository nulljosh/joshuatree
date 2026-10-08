/* The Calculator on ARM: the evaluator and the keypad's state. Built with the FPU allowed (like text.c); main.c is not,
   so everything crossing the boundary is a string or an int. The parser is user/calculator.c's recursive descent,
   with two changes: dividing (or taking % of) zero is an error, not 0, and a number can carry a `deg` suffix
   (sin(30deg)). The math is short series on the hardware FPU, no libm. Plain C, so
   tools/checks/arm64-calc-check.py compiles this same file on the host and runs it over a table of sums. */

#define NAN_ (0.0 / 0.0)
#define PI 3.14159265358979323846
#define LN2 0.69314718055994530942
#define INPUT_MAX 80
#define OUTPUT_MAX 32

static double ans, mem;
static int deg_mode, sci_mode, has_mem;
static char input[INPUT_MAX], output[OUTPUT_MAX];
static int input_len;

/* ---- math ---- */
static double m_abs(double x) { return x < 0 ? -x : x; }
static double m_sqrt(double x) { return x < 0 ? NAN_ : __builtin_sqrt(x); }   /* one fsqrt instruction */
static double m_sin(double x) {   /* reduce to [-pi, pi], then the Taylor series */
    if (x != x || m_abs(x) > 1e9) return NAN_;
    x -= 2 * PI * (double)(long long)(x / (2 * PI));
    if (x > PI) x -= 2 * PI; else if (x < -PI) x += 2 * PI;
    double term = x, sum = x;
    for (int n = 1; n < 30; n++) { term *= -x * x / ((2 * n) * (2 * n + 1)); sum += term; }
    return sum;
}
static double m_cos(double x) { return m_sin(x + PI / 2); }
static double m_atan(double x) {   /* |x| > 1 folds to 1/x, then a half-angle step, then the series */
    if (x != x) return x;
    if (x < 0) return -m_atan(-x);
    if (x > 1) return PI / 2 - m_atan(1 / x);
    if (x > 0.4142135623730950) return PI / 4 + m_atan((x - 1) / (x + 1));
    double term = x, sum = x;
    for (int n = 1; n < 60; n++) { term *= -x * x; sum += term / (2 * n + 1); }
    return sum;
}
static double m_atan2(double y, double x) {
    if (x > 0) return m_atan(y / x);
    if (x < 0) return y < 0 ? m_atan(y / x) - PI : m_atan(y / x) + PI;
    return y > 0 ? PI / 2 : y < 0 ? -PI / 2 : 0;
}
static double m_ln(double x) {   /* x = m * 2^k with m near 1, then 2 * atanh((m - 1) / (m + 1)) */
    if (!(x > 0) || x > 1.7e308) return NAN_;
    int k = 0;
    while (x > 1.41421356237309505) { x /= 2; k++; }
    while (x < 0.70710678118654752) { x *= 2; k--; }
    double t = (x - 1) / (x + 1), t2 = t * t, term = t, sum = 0;
    for (int n = 0; n < 40; n++) { sum += term / (2 * n + 1); term *= t2; }
    return 2 * sum + k * LN2;
}
static double m_exp(double x) {   /* x = k ln2 + r with |r| <= ln2 / 2, e^r by its series, then times 2^k */
    if (x != x) return x;
    if (x > 709.7) return NAN_;   /* past the largest double: an error, not infinity */
    if (x < -745) return 0;
    long long k = (long long)(x / LN2 + (x < 0 ? -0.5 : 0.5));
    double r = x - (double)k * LN2, term = 1, sum = 1;
    for (int n = 1; n < 25; n++) { term *= r / n; sum += term; }
    for (; k > 0; k--) sum *= 2;
    for (; k < 0; k++) sum /= 2;
    return sum;
}
static double m_pow(double a, double b) {
    if (b == (double)(long long)b && m_abs(b) <= 4096) {   /* a whole exponent: exact by repeated squaring (2^10 is 1024) */
        long long n = (long long)b; double r = 1, p = a;
        for (long long e = n < 0 ? -n : n; e; e >>= 1) { if (e & 1) r *= p; p *= p; }
        return n < 0 ? (r == 0 ? NAN_ : 1 / r) : r;
    }
    if (a == 0) return b > 0 ? 0 : NAN_;
    if (a < 0) return NAN_;   /* a negative base needs a whole exponent */
    return m_exp(b * m_ln(a));
}
static double m_fact(double n) {
    if (n < 0 || n != (double)(int)n || n > 170) return NAN_;
    double r = 1; for (int i = 2; i <= (int)n; i++) r *= i; return r;
}
static double to_rad(double x) { return deg_mode ? x * PI / 180 : x; }
static double from_rad(double x) { return deg_mode ? x * 180 / PI : x; }

/* ---- the parser: user/calculator.c's grammar, one double per rule ---- */
typedef enum { TOK_NUM, TOK_SYM, TOK_ID, TOK_END, TOK_BAD } tok_kind;
typedef struct { tok_kind kind; double num; char sym; char id[6]; } token;
typedef struct { const char *src; int pos; } lexer;

static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int id_is(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

static token next(lexer *lx) {
    const char *s = lx->src; int i = lx->pos;
    token t; t.kind = TOK_END; t.num = 0; t.sym = 0; t.id[0] = 0;
    while (s[i] == ' ') i++;
    if (!s[i]) { lx->pos = i; return t; }
    if (is_digit(s[i]) || (s[i] == '.' && is_digit(s[i + 1]))) {
        double whole = 0, frac = 0, scale = 0.1;
        while (is_digit(s[i])) whole = whole * 10 + (s[i++] - '0');
        if (s[i] == '.') { i++; while (is_digit(s[i])) { frac += (s[i++] - '0') * scale; scale *= 0.1; } }
        t.kind = TOK_NUM; t.num = whole + frac; lx->pos = i; return t;
    }
    if (is_alpha(s[i])) {
        int n = 0;
        while (is_alpha(s[i])) { if (n < 5) t.id[n++] = (char)(s[i] | 0x20); i++; }
        t.id[n] = 0; t.kind = TOK_ID; lx->pos = i; return t;
    }
    const char *syms = "+-*/()^!%";
    for (const char *p = syms; *p; p++) if (s[i] == *p) { t.kind = TOK_SYM; t.sym = s[i]; lx->pos = i + 1; return t; }
    t.kind = TOK_BAD; lx->pos = i + 1; return t;
}

static double expr(lexer *lx);
static double unary(lexer *lx);
static double paren(lexer *lx) {
    token t = next(lx);
    if (t.kind != TOK_SYM || t.sym != '(') return NAN_;
    double v = expr(lx);
    t = next(lx);
    return t.kind == TOK_SYM && t.sym == ')' ? v : NAN_;
}
static double primary(lexer *lx) {
    int save = lx->pos;
    token t = next(lx);
    if (t.kind == TOK_NUM) return t.num;
    if (t.kind == TOK_SYM && t.sym == '(') { lx->pos = save; return paren(lx); }
    if (t.kind != TOK_ID) return NAN_;
    const char *f = t.id;
    if (id_is(f, "pi")) return PI;
    if (id_is(f, "e")) return 2.71828182845904523536;
    if (id_is(f, "ans")) return ans;
    if (id_is(f, "mem")) return mem;
    double x = paren(lx), c;
    if (id_is(f, "sin")) return m_sin(to_rad(x));
    if (id_is(f, "cos")) return m_cos(to_rad(x));
    if (id_is(f, "tan")) { c = m_cos(to_rad(x)); return m_abs(c) < 1e-15 ? NAN_ : m_sin(to_rad(x)) / c; }
    if (id_is(f, "asin")) return m_abs(x) > 1 ? NAN_ : from_rad(m_atan2(x, m_sqrt(1 - x * x)));
    if (id_is(f, "acos")) return m_abs(x) > 1 ? NAN_ : from_rad(m_atan2(m_sqrt(1 - x * x), x));
    if (id_is(f, "atan")) return from_rad(m_atan(x));
    if (id_is(f, "sqrt")) return m_sqrt(x);
    if (id_is(f, "ln")) return m_ln(x);
    if (id_is(f, "log")) return m_ln(x) / 2.30258509299404568402;
    if (id_is(f, "exp")) return m_exp(x);
    if (id_is(f, "abs")) return m_abs(x);
    return NAN_;
}
static double postfix(lexer *lx) {   /* 5!, and 30deg: an angle in degrees, whatever the mode */
    double v = primary(lx);
    for (;;) {
        int save = lx->pos;
        token t = next(lx);
        if (t.kind == TOK_SYM && t.sym == '!') v = m_fact(v);
        else if (t.kind == TOK_ID && id_is(t.id, "deg")) v = deg_mode ? v : v * PI / 180;
        else { lx->pos = save; return v; }
    }
}
static double power(lexer *lx) {   /* right-associative: 2^3^2 = 2^9 */
    double base = postfix(lx);
    int save = lx->pos;
    token t = next(lx);
    if (t.kind == TOK_SYM && t.sym == '^') return m_pow(base, unary(lx));
    lx->pos = save;
    return base;
}
static double unary(lexer *lx) {   /* -2^2 = -4 */
    int save = lx->pos;
    token t = next(lx);
    if (t.kind == TOK_SYM && t.sym == '-') return -unary(lx);
    lx->pos = save;
    return power(lx);
}
static double term(lexer *lx) {
    double left = unary(lx);
    for (;;) {
        int save = lx->pos;
        token t = next(lx);
        if (t.kind != TOK_SYM || (t.sym != '*' && t.sym != '/' && t.sym != '%')) { lx->pos = save; return left; }
        double right = unary(lx);
        if (t.sym == '*') left *= right;
        else if (right == 0) left = NAN_;   /* divide by zero is an error, not 0 */
        else if (t.sym == '/') left /= right;
        else left -= right * (double)(long long)(left / right);
    }
}
static double expr(lexer *lx) {
    double left = term(lx);
    for (;;) {
        int save = lx->pos;
        token t = next(lx);
        if (t.kind == TOK_SYM && (t.sym == '+' || t.sym == '-')) { double r = term(lx); left = t.sym == '+' ? left + r : left - r; }
        else { lx->pos = save; return left; }
    }
}
static double eval(const char *src) {
    lexer lx = { src, 0 };
    double v = expr(&lx);
    return next(&lx).kind == TOK_END ? v : NAN_;   /* anything left over (a stray ')', a bad character) is an error */
}

/* "Error" for NaN or out of range, else up to 8 decimals with trailing zeros dropped, e-notation outside 1e-6..1e15. */
static int utoa(unsigned long long v, char *b) {
    char t[24]; int n = 0, k = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) b[k++] = t[--n];
    return k;
}
static void format(double r, char *b) {
    int i = 0, ex = 0;
    if (r != r || r > 1e300 || r < -1e300) { const char *e = "Error"; while (*e) b[i++] = *e++; b[i] = 0; return; }
    if (m_abs(r) < 1e-12) r = 0;   /* sin(pi) is 1.2e-16 in doubles: show it as the 0 it means */
    if (r < 0) { b[i++] = '-'; r = -r; }
    if (r >= 1e15) while (r >= 10) { r /= 10; ex++; }
    else if (r > 0 && r < 1e-6) while (r < 1) { r *= 10; ex--; }
    int places = ex ? 6 : 8;
    double half = 0.5; for (int k = 0; k < places; k++) half /= 10;
    r += half;
    if (ex && r >= 10) { r /= 10; ex++; }
    unsigned long long w = (unsigned long long)r;
    i += utoa(w, b + i);
    double frac = r - (double)w;
    b[i++] = '.';
    for (int k = 0; k < places; k++) { frac *= 10; int d = (int)frac; b[i++] = (char)('0' + d); frac -= d; }
    while (b[i - 1] == '0') i--;
    if (b[i - 1] == '.') i--;
    if (ex) { b[i++] = 'e'; if (ex < 0) { b[i++] = '-'; ex = -ex; } i += utoa((unsigned)ex, b + i); }
    b[i] = 0;
}

/* ---- the interface main.c and the check use: strings and ints only ---- */
int calc_eval_str(const char *src, int deg, char *out) {   /* 0 ok, -1 error; out holds OUTPUT_MAX bytes */
    int keep = deg_mode; deg_mode = deg;
    double v = eval(src);
    deg_mode = keep;
    format(v, out);
    if (v == v && out[0] != 'E') { ans = v; return 0; }
    return -1;
}
const char *calc_input(void) { return input; }
const char *calc_output(void) { return output; }
int calc_flags(void) { return sci_mode | deg_mode << 1 | has_mem << 2; }

static void append(const char *s) { while (*s && input_len < INPUT_MAX - 1) input[input_len++] = *s++; input[input_len] = 0; }
static void memory(int sign) {   /* M+ and M-: the value on screen, worked out first if it is still a sum */
    double v = input_len ? eval(input) : ans;
    if (v != v) { format(v, output); return; }
    mem += sign * v; has_mem = 1;
}

/* The keypads. "" is a gap. The toggle keys print what they switch to. */
static const char *const STD_KEYS[] = {
    "C", "Del", "(", ")",
    "7", "8", "9", "/",
    "4", "5", "6", "*",
    "1", "2", "3", "-",
    "0", ".", "ans", "+",
    "Sci", "%", "^", "=",
};
static const char *const SCI_KEYS[] = {
    "sin", "cos", "tan", "asin", "acos", "atan",
    "ln", "log", "exp", "sqrt", "^", "!",
    "MC", "MR", "M+", "M-", "Deg", "Std",
    "7", "8", "9", "/", "(", ")",
    "4", "5", "6", "*", "C", "Del",
    "1", "2", "3", "-", "pi", "e",
    "0", ".", "%", "+", "ans", "=",
};
int calc_keys(int *cols) {   /* how many keys the current pad has, and its columns */
    *cols = sci_mode ? 6 : 4;
    return sci_mode ? (int)(sizeof SCI_KEYS / sizeof *SCI_KEYS) : (int)(sizeof STD_KEYS / sizeof *STD_KEYS);
}
const char *calc_key(int i) {
    const char *k = sci_mode ? SCI_KEYS[i] : STD_KEYS[i];
    return id_is(k, "Deg") && deg_mode ? "Rad" : k;
}

/* One key, clicked or typed. Returns 1 when "=" just ran (main.c logs the line). */
int calc_press(const char *k) {
    if (id_is(k, "C")) { input_len = 0; input[0] = 0; output[0] = 0; return 0; }
    if (id_is(k, "Del")) { if (input_len) input[--input_len] = 0; return 0; }
    if (id_is(k, "Sci") || id_is(k, "Std")) { sci_mode = !sci_mode; return 0; }
    if (id_is(k, "Deg") || id_is(k, "Rad")) { deg_mode = !deg_mode; return 0; }
    if (id_is(k, "MC")) { mem = 0; has_mem = 0; return 0; }
    if (id_is(k, "MR")) { append("mem"); return 0; }
    if (id_is(k, "M+")) { memory(1); return 0; }
    if (id_is(k, "M-")) { memory(-1); return 0; }
    if (id_is(k, "=")) { if (!input_len) return 0; calc_eval_str(input, deg_mode, output); return 1; }
    append(k);
    if (is_alpha(k[0]) && !id_is(k, "pi") && !id_is(k, "e") && !id_is(k, "ans")) append("(");   /* sin opens its bracket */
    return 0;
}
int calc_type(int ch) {   /* a typed character: Enter is =, backspace Del, Tab the mode */
    char s[2] = { (char)ch, 0 };
    if (ch == '\n' || ch == '=') return calc_press("=");
    if (ch == '\b') return calc_press("Del");
    if (ch == '\t') return calc_press("Sci");
    if (ch >= 32 && ch < 127) append(s);
    return 0;
}
