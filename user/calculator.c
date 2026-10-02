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
typedef enum { TOK_NUM, TOK_SYM, TOK_END } calc_tok_kind;
typedef struct { calc_tok_kind kind; double num_val; char sym_val; } calc_token;
typedef struct { const char *src; int pos; } calc_lexer;

static int calc_is_digit(char c) { return c >= '0' && c <= '9'; }

static calc_token calc_next(calc_lexer *lex) {
    const char *s = lex->src;
    int i = lex->pos;
    calc_token tok; tok.kind = TOK_END; tok.num_val = 0; tok.sym_val = 0;

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
    if (s[i] == '+' || s[i] == '-' || s[i] == '*' || s[i] == '/' || s[i] == '(' || s[i] == ')') {
        tok.kind = TOK_SYM; tok.sym_val = s[i]; lex->pos = i + 1;
        return tok;
    }
    lex->pos = i + 1;
    tok.kind = TOK_END;
    return tok;
}

static double calc_expr(calc_lexer *lex);

static double calc_primary(calc_lexer *lex) {
    calc_token tok = calc_next(lex);
    if (tok.kind == TOK_NUM) return tok.num_val;
    if (tok.kind == TOK_SYM && tok.sym_val == '(') {
        double v = calc_expr(lex);
        calc_token close = calc_next(lex);
        if (close.kind != TOK_SYM || close.sym_val != ')') return 0;
        return v;
    }
    return 0;
}
/* Peek-then-rewind saves lex->pos before peeking and restores that exact
   position, not an assumed single-character step: the same fix
   drivers/app_calculator.c carries for the "10/2 got 0" bug (a `pos--`
   only undoes one character, which is wrong for a multi-digit NUM). */
static double calc_unary(calc_lexer *lex) {
    int save = lex->pos;
    calc_token tok = calc_next(lex);
    if (tok.kind == TOK_SYM && tok.sym_val == '-') return -calc_unary(lex);
    lex->pos = save;
    return calc_primary(lex);
}
static double calc_term(calc_lexer *lex) {
    double left = calc_unary(lex);
    for (;;) {
        int save = lex->pos;
        calc_token tok = calc_next(lex);
        if (tok.kind == TOK_SYM && (tok.sym_val == '*' || tok.sym_val == '/')) {
            double right = calc_unary(lex);
            left = (tok.sym_val == '*') ? left * right : (right != 0 ? left / right : 0);
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
    if (result < 0) { buf[i++] = '-'; result = -result; }
    i += utoa10((unsigned)result, buf + i);
    double frac = result - (int)result;
    if (frac > 0.0001 && i < max - 5) {
        buf[i++] = '.';
        for (int j = 0; j < 4 && i < max - 1; j++) {
            frac *= 10;
            int digit = (int)frac;
            buf[i++] = (char)('0' + digit);
            frac -= digit;
        }
    }
    buf[i] = 0;
}

static void calc_draw(void) {
    int W = (int)win.width, bw = W - 40;
    rect(0, 0, W, (int)win.height, BG);
    text("+ - * / ( )   enter evaluates   esc closes", 20, 36, HINT);
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
        flags = 0;
        if (r == -11 /* -EAGAIN */) { jt_sched_yield(); continue; }
        if (r != 1) break;
        if (ev.kind != JT_EV_KEY) { flags = JT_POLL_PRESENT; continue; }

        if (ev.a == JT_KEY_ESC) break;
        if (ev.a == '`') {
            jt_write(1, "calculator: crashing on purpose\n", 33);
            *(volatile int *)0 = 1;
        }
        if (ev.a == JT_KEY_ENTER) {
            input[input_len] = 0;
            double result = calc_eval(input);
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
