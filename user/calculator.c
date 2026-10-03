/* calculator: recursive-descent arithmetic (+ - * / ( )), as a real
 * ring-3 program.
 *
 * The third app to leave the kernel (roadmap 2.0), done exactly the way
 * user/keyrate.c and user/toroid.c were. Same parser drivers/
 * app_calculator.c runs in ring 0 (same grammar, same precedence, same
 * divide-by-zero-is-0 behavior), but evaluated directly while parsing
 * instead of building an expr_node tree on the heap: a flat binary has
 * no .bss and no kmalloc (user/note.ld), so where the in-kernel version
 * calls kmalloc/kfree per node, this one folds each grammar rule straight
 * into a double and returns it. The grammar and every edge case are
 * unchanged: '*'/'/' bind tighter than '+'/'-', parens override, unary
 * minus recurses, and dividing by zero yields 0 rather than a fault or a
 * NaN, exactly like calc_eval's `b != 0 ? a / b : 0` in
 * drivers/app_calculator.c. This file is compiled with no kernel include
 * path, linked flat, loaded off the VFS by exec_user, and reaches the
 * machine only through int 0x80: SYS_WINDOW_OPEN for a framebuffer,
 * SYS_WINDOW_POLL for input and the present, SYS_EXIT to leave.
 *
 * Scientific (1.9.29): ^ (right-assoc), postfix !, % (modulo), pi, e,
 * ans, and sin cos tan asin acos atan sqrt ln log exp abs, in radians.
 * The math is the x87's own instructions (fsin, fsqrt, fyl2x, f2xm1...),
 * since a flat user binary has no libm. Tab shows the scientific keys.
 *
 * Glyphs: antialiased DejaVu via libjt/text.h. The backquote key (`) is the deliberate crash, same as both:
 * a write through a null pointer, a page fault at ring 3, reaped by the
 * kernel. tools/checks/ring3calc-check.py presses it on purpose.
 */
#include "jtsys.h"
#include "libjt/text.h"

#define BG    0x00FAF8F6 /* GUI_BG */
#define BOX   0x00FFFFFF
#define DIV   0x00E0D8CE
#define INK   0x001C1C1E
#define HINT  0x0075726E
#define LABEL 0x00807468

#define CALC_INPUT_MAX  80
#define CALC_OUTPUT_MAX 32

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
static char input[CALC_INPUT_MAX] JT_DATA = {0};
static char output[CALC_OUTPUT_MAX] JT_DATA = {0};
static int input_len JT_DATA = 0;
static int has_output JT_DATA = 0;
static int sci JT_DATA = 0;          /* Tab: scientific keys on screen */
static double ans JT_DATA = 0;       /* the last result, as `ans` */

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
static int text(const char *s, int x, int y, unsigned fg) { return jt_text_draw(&win, JT_FACE_BODY, x, y, fg, s); }
static int utoa10(unsigned v, char *buf) {
    char tmp[12]; int tn = 0, n = 0;
    do { tmp[tn++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (tn) buf[n++] = tmp[--tn];
    buf[n] = 0;
    return n;
}

/* ---- parser: same grammar as drivers/app_calculator.c's calc_expr /
   calc_term / calc_unary / calc_primary, but each rule returns the
   evaluated double directly (no expr_node, no kmalloc: a flat user
   binary has no .bss and no heap). Divide by zero returns 0, the exact
   behavior calc_eval's EXPR_OP case gives the in-kernel version. */
typedef enum { TOK_NUM, TOK_SYM, TOK_ID, TOK_END } calc_tok_kind;
typedef struct { calc_tok_kind kind; double num_val; char sym_val; char id[6]; } calc_token;
typedef struct { const char *src; int pos; } calc_lexer;

static int calc_is_digit(char c) { return c >= '0' && c <= '9'; }
static int calc_is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

/* ---- x87 math: the FPU does the hard part, no libm needed ---- */
static double m_sqrt(double x) { if (x < 0) return 0.0 / 0.0; __asm__("fsqrt" : "+t"(x)); return x; }
static double m_sin(double x) { __asm__("fsin" : "+t"(x)); return x; }
static double m_cos(double x) { __asm__("fcos" : "+t"(x)); return x; }
static double m_tan(double x) { __asm__("fptan\n\tfstp %%st(0)" : "+t"(x)); return x; }
static double m_atan2(double y, double x) { double r; __asm__("fpatan" : "=t"(r) : "0"(x), "u"(y) : "st(1)"); return r; }
static double m_ln(double x) { double r; if (x <= 0) return 0.0 / 0.0; __asm__("fldln2\n\tfxch\n\tfyl2x" : "=t"(r) : "0"(x)); return r; }
static double m_exp(double x) {
    __asm__("fldl2e\n\tfmulp\n\tfld %%st(0)\n\tfrndint\n\tfxch\n\tfsub %%st(1), %%st\n\tf2xm1\n\tfld1\n\tfaddp\n\tfscale\n\tfstp %%st(1)" : "+t"(x));
    return x;
}
static double m_pow(double a, double b) {
    if (a == 0) return b > 0 ? 0 : 0.0 / 0.0;
    if (a > 0) return m_exp(b * m_ln(a));
    double bi = (double)(long long)b;
    if (bi != b) return 0.0 / 0.0;                 /* a negative base needs a whole exponent */
    double r = m_exp(b * m_ln(-a));
    return ((long long)b & 1) ? -r : r;
}
static double m_fact(double n) {
    if (n < 0 || n != (double)(int)n || n > 170) return 0.0 / 0.0;
    double r = 1; for (int i = 2; i <= (int)n; i++) r *= i; return r;
}
static int id_is(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

static calc_token calc_next(calc_lexer *lex) {
    const char *s = lex->src;
    int i = lex->pos;
    calc_token tok; tok.kind = TOK_END; tok.num_val = 0; tok.sym_val = 0; tok.id[0] = 0;

    while (s[i] == ' ' || s[i] == '\t') i++;
    if (s[i] == 0) { lex->pos = i; return tok; }

    if (calc_is_digit(s[i]) || (s[i] == '.' && calc_is_digit(s[i+1]))) {
        int j = i;
        double whole = 0, frac = 0, frac_scale = 0.1;
        while (calc_is_digit(s[j])) { whole = whole * 10 + (s[j] - '0'); j++; }
        if (s[j] == '.' && calc_is_digit(s[j+1])) {
            j++;
            while (calc_is_digit(s[j])) { frac = frac + (s[j] - '0') * frac_scale; frac_scale = frac_scale * 0.1; j++; }
        }
        tok.kind = TOK_NUM; tok.num_val = whole + frac; lex->pos = j;
        return tok;
    }
    if (calc_is_alpha(s[i])) {
        int n = 0;
        while (calc_is_alpha(s[i])) { if (n < 5) tok.id[n++] = (char)(s[i] | 0x20); i++; }
        tok.id[n] = 0; tok.kind = TOK_ID; lex->pos = i;
        return tok;
    }
    if (s[i] == '+' || s[i] == '-' || s[i] == '*' || s[i] == '/' || s[i] == '(' || s[i] == ')' ||
        s[i] == '^' || s[i] == '!' || s[i] == '%') {
        tok.kind = TOK_SYM; tok.sym_val = s[i]; lex->pos = i + 1;
        return tok;
    }
    lex->pos = i + 1;
    tok.kind = TOK_END;
    return tok;
}

static double calc_expr(calc_lexer *lex);

static double calc_paren(calc_lexer *lex) {
    calc_token open = calc_next(lex);
    if (open.kind != TOK_SYM || open.sym_val != '(') return 0.0 / 0.0;
    double v = calc_expr(lex);
    calc_token close = calc_next(lex);
    if (close.kind != TOK_SYM || close.sym_val != ')') return 0.0 / 0.0;
    return v;
}
static double calc_primary(calc_lexer *lex) {
    int save = lex->pos;
    calc_token tok = calc_next(lex);
    if (tok.kind == TOK_NUM) return tok.num_val;
    if (tok.kind == TOK_SYM && tok.sym_val == '(') { lex->pos = save; return calc_paren(lex); }
    if (tok.kind == TOK_ID) {
        const char *f = tok.id;
        if (id_is(f, "pi")) return 3.14159265358979323846;
        if (id_is(f, "e")) return 2.71828182845904523536;
        if (id_is(f, "ans")) return ans;
        double x = calc_paren(lex), q;
        if (id_is(f, "sin")) return m_sin(x);
        if (id_is(f, "cos")) return m_cos(x);
        if (id_is(f, "tan")) return m_tan(x);
        if (id_is(f, "asin")) { q = m_sqrt(1 - x * x); return m_atan2(x, q); }
        if (id_is(f, "acos")) { q = m_sqrt(1 - x * x); return m_atan2(q, x); }
        if (id_is(f, "atan")) return m_atan2(x, 1);
        if (id_is(f, "sqrt")) return m_sqrt(x);
        if (id_is(f, "ln")) return m_ln(x);
        if (id_is(f, "log")) return m_ln(x) / 2.30258509299404568402;
        if (id_is(f, "exp")) return m_exp(x);
        if (id_is(f, "abs")) return x < 0 ? -x : x;
        return 0.0 / 0.0;
    }
    return 0;
}
static double calc_postfix(calc_lexer *lex) {   /* 5! */
    double v = calc_primary(lex);
    for (;;) {
        int save = lex->pos;
        calc_token tok = calc_next(lex);
        if (tok.kind == TOK_SYM && tok.sym_val == '!') v = m_fact(v);
        else { lex->pos = save; return v; }
    }
}
/* Peek-then-rewind saves lex->pos before peeking and restores that exact
   position, not an assumed single-character step: the same fix
   drivers/app_calculator.c carries for the "10/2 got 0" bug (a `pos--`
   only undoes one character, which is wrong for a multi-digit NUM). */
static double calc_unary(calc_lexer *lex);
static double calc_power(calc_lexer *lex) {     /* 2^3^2 = 2^9: right-associative */
    double base = calc_postfix(lex);
    int save = lex->pos;
    calc_token tok = calc_next(lex);
    if (tok.kind == TOK_SYM && tok.sym_val == '^') return m_pow(base, calc_unary(lex));
    lex->pos = save;
    return base;
}
static double calc_unary(calc_lexer *lex) {     /* -2^2 = -4, like every calculator */
    int save = lex->pos;
    calc_token tok = calc_next(lex);
    if (tok.kind == TOK_SYM && tok.sym_val == '-') return -calc_unary(lex);
    lex->pos = save;
    return calc_power(lex);
}
static double calc_term(calc_lexer *lex) {
    double left = calc_unary(lex);
    for (;;) {
        int save = lex->pos;
        calc_token tok = calc_next(lex);
        if (tok.kind == TOK_SYM && (tok.sym_val == '*' || tok.sym_val == '/' || tok.sym_val == '%')) {
            double right = calc_unary(lex);
            if (tok.sym_val == '*') left = left * right;
            else if (tok.sym_val == '/') left = right != 0 ? left / right : 0;
            else left = right != 0 ? left - right * (double)(long long)(left / right) : 0;
        } else { lex->pos = save; break; }
    }
    return left;
}
static double calc_expr(calc_lexer *lex) {
    double left = calc_term(lex);
    for (;;) {
        int save = lex->pos;
        calc_token tok = calc_next(lex);
        if (tok.kind == TOK_SYM && (tok.sym_val == '+' || tok.sym_val == '-')) {
            double right = calc_term(lex);
            left = (tok.sym_val == '+') ? left + right : left - right;
        } else { lex->pos = save; break; }
    }
    return left;
}
static double calc_eval(const char *src) {
    calc_lexer lex; lex.src = src; lex.pos = 0;
    return calc_expr(&lex);
}

/* Same formatting as drivers/app_calculator.c's calc_format_result: an
   unsigned whole part via the shared utoa10 digit-reverse loop, then up
   to 4 fractional digits and a leading '-' for negatives. */
static void calc_format_result(double result, char *buf, int max) {
    int i = 0;
    if (result != result || result > 1e300 || result < -1e300) { const char *e = "Error"; while (*e) buf[i++] = *e++; buf[i] = 0; return; }
    if (result < 0) { buf[i++] = '-'; result = -result; }
    int ex = 0;                          /* past 4e9 or under 1e-4: 1.2345e12 */
    if (result >= 1e9) while (result >= 10) { result /= 10; ex++; }
    else if (result > 0 && result < 1e-4) while (result < 1) { result *= 10; ex--; }
    result += 0.00005;                   /* round to the 4 places shown */
    i += utoa10((unsigned)result, buf + i);
    double frac = result - (unsigned)result;
    if (frac > 0.0001 && i < max - 5) {
        buf[i++] = '.';
        for (int j = 0; j < 4 && i < max - 1; j++) {
            frac *= 10;
            int digit = (int)frac;
            buf[i++] = (char)('0' + digit);
            frac -= digit;
        }
        while (buf[i - 1] == '0') i--;
    }
    if (ex && i < max - 6) { buf[i++] = 'e'; if (ex < 0) { buf[i++] = '-'; ex = -ex; } i += utoa10((unsigned)ex, buf + i); }
    buf[i] = 0;
}

/* The scientific keys, shown on Tab. Each is what you type. */
static const char *const SCI_KEYS[] = {
    "sin(", "cos(", "tan(", "sqrt(", "^", "!",
    "asin(", "acos(", "atan(", "ln(", "log(", "exp(",
    "pi", "e", "ans", "abs(", "%", "( )",
};

static void calc_draw(void) {
    int W = (int)win.width, bw = W - 40;
    rect(0, 0, W, (int)win.height, BG);
    text(sci ? "Scientific   radians   Tab for basic   Enter evaluates   Esc closes"
             : "+ - * / ( )   Tab for scientific   Enter evaluates   Esc closes", 20, 36, HINT);
    rect(20, 60, bw, 28, BOX);
    if (input_len == 0) {
        text("Type a sum, like 12 * (3 + 4)", 28, 65, HINT);
        rect(26, 66, 1, 16, INK); /* caret */
    } else {
        /* keep the tail in view: drop leading characters until the text fits */
        const char *t = input;
        while (*t && jt_text_width(JT_FACE_BODY, t) > bw - 24) t++;
        int x = text(t, 28, 65, INK);
        rect(x + 1, 66, 1, 16, INK); /* caret */
    }
    rect(20, 104, bw, 56, BOX);
    text("Result", 28, 110, LABEL);
    if (has_output) {
        jt_text_draw(&win, JT_FACE_BOLD, 28, 130, INK, output);
    } else {
        text("Nothing yet. Press enter.", 28, 130, HINT);
    }
    if (sci) {
        int cols = 6, gap = 8, x0 = 20, y0 = 176, kh = 40;
        int kw = ((int)win.width - 40 - gap * (cols - 1)) / cols;
        for (int k = 0; k < (int)(sizeof SCI_KEYS / sizeof SCI_KEYS[0]); k++) {
            int x = x0 + (k % cols) * (kw + gap), y = y0 + (k / cols) * (kh + gap);
            rect(x, y, kw, kh, DIV);
            rect(x + 1, y + 1, kw - 2, kh - 2, BOX);
            int tw = jt_text_width(JT_FACE_BODY, SCI_KEYS[k]);
            text(SCI_KEYS[k], x + (kw - tw) / 2, y + 12, INK);
        }
    }
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open(&win) != 0 || !win.pixels) {
        jt_write(2, "calculator: no window\n", 22);
        jt_exit(1);
    }
    {   /* one line, one write: what tools/checks/ring3calc-check.py asserts on */
        char line[48]; int l = 0;
        const char *pfx = "calculator: ring-3 window ";
        while (*pfx) line[l++] = *pfx++;
        l += utoa10(win.width, line + l); line[l++] = 'x';
        l += utoa10(win.height, line + l); line[l++] = '\n';
        jt_write(1, line, (unsigned)l);
    }

    input[0] = 0; output[0] = 0; input_len = 0; has_output = 0;
    calc_draw();

    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        if (r == 1 && jt_window_resized(&ev, &win)) { calc_draw(); flags = JT_POLL_PRESENT; continue; } /* JT_EV_RESIZE: remapped, repaint at the new size */
        flags = 0;
        if (r == -11 /* -EAGAIN */) { jt_sched_yield(); continue; }
        if (r != 1) break;
        if (ev.kind != JT_EV_KEY) { flags = JT_POLL_PRESENT; continue; }

        if (ev.a == JT_KEY_ESC) break;
        if (ev.a == '`') {
            jt_write(1, "calculator: crashing on purpose\n", 33);
            *(volatile int *)0 = 1;
        }
        if (ev.a == '\t') {
            sci = !sci;
            jt_write(1, sci ? "calculator: scientific\n" : "calculator: basic\n", sci ? 23 : 18);
        } else if (ev.a == JT_KEY_ENTER) {
            input[input_len] = 0;
            double result = calc_eval(input);
            if (result == result) ans = result;
            calc_format_result(result, output, CALC_OUTPUT_MAX);
            has_output = 1;
            /* One line, one write: the discriminating marker
               tools/checks/ring3calc-check.py reads to confirm the
               ring-3 parser actually ran and what it computed, including
               the divide-by-zero-is-0 case. */
            char line[CALC_INPUT_MAX + CALC_OUTPUT_MAX + 32]; int l = 0;
            const char *pfx = "calculator: "; while (*pfx) line[l++] = *pfx++;
            for (const char *p = input; *p; p++) line[l++] = *p;
            const char *eq = " = "; while (*eq) line[l++] = *eq++;
            for (const char *p = output; *p; p++) line[l++] = *p;
            line[l++] = '\n';
            jt_write(1, line, (unsigned)l);
        } else if (ev.a == 8 /* backspace */) {
            if (input_len > 0) input_len--;
        } else if (input_len < CALC_INPUT_MAX - 1 && ev.a >= 32 && ev.a < 127) {
            input[input_len++] = (char)ev.a;
        } else {
            flags = JT_POLL_PRESENT;
            continue;
        }
        calc_draw();
        flags = JT_POLL_PRESENT;
    }
    jt_write(1, "calculator: closed\n", 20);
    jt_exit(0);
}
