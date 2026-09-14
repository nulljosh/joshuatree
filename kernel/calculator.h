/* v70 (0.64.0): Calculator. Simplified recursive-descent parser for +, -, *,
   /, (), and numbers. Pure arithmetic, no functions or constants, to keep the
   parser portable and avoid linking external dependencies. Expression text is
   captured from the user via one-line input, evaluated on enter, and the
   result is shown. */

#define CALC_INPUT_MAX 80
#define CALC_OUTPUT_MAX 32

typedef enum {
    TOK_NUM, TOK_SYM, TOK_END
} calc_tok_kind;

typedef struct {
    calc_tok_kind kind;
    double num_val;
    char sym_val;
} calc_token;

static int calc_is_digit(char c) {
    return c >= '0' && c <= '9';
}

typedef struct {
    const char *src;
    int pos;
} calc_lexer;

static calc_token calc_next(calc_lexer *lex) {
    const char *s = lex->src;
    int i = lex->pos;
    calc_token tok;
    tok.kind = TOK_END;
    tok.num_val = 0;
    tok.sym_val = 0;

    while (s[i] == ' ' || s[i] == '\t') i++;

    if (s[i] == 0) { lex->pos = i; return tok; }

    if (calc_is_digit(s[i]) || (s[i] == '.' && calc_is_digit(s[i+1]))) {
        int j = i;
        double whole = 0, frac = 0, frac_scale = 0.1;
        while (calc_is_digit(s[j])) {
            whole = whole * 10 + (s[j] - '0');
            j++;
        }
        if (s[j] == '.' && calc_is_digit(s[j+1])) {
            j++;
            while (calc_is_digit(s[j])) {
                frac = frac + (s[j] - '0') * frac_scale;
                frac_scale = frac_scale * 0.1;
                j++;
            }
        }
        tok.kind = TOK_NUM;
        tok.num_val = whole + frac;
        lex->pos = j;
        return tok;
    }

    if (s[i] == '+' || s[i] == '-' || s[i] == '*' || s[i] == '/' ||
        s[i] == '(' || s[i] == ')') {
        tok.kind = TOK_SYM;
        tok.sym_val = s[i];
        lex->pos = i + 1;
        return tok;
    }

    lex->pos = i + 1;
    tok.kind = TOK_END;
    return tok;
}

typedef enum {
    EXPR_NUM, EXPR_NEG, EXPR_OP
} expr_kind;

typedef struct expr_node {
    expr_kind kind;
    double num_val;
    char op;
    struct expr_node *left, *right;
} expr_node;

static expr_node* calc_expr(calc_lexer *lex);
static double calc_eval(expr_node *e);

static expr_node* calc_primary(calc_lexer *lex) {
    calc_token tok = calc_next(lex);
    expr_node *n = (expr_node*)kmalloc(sizeof(expr_node));
    if (!n) return 0;

    if (tok.kind == TOK_NUM) {
        n->kind = EXPR_NUM;
        n->num_val = tok.num_val;
        return n;
    }

    if (tok.kind == TOK_SYM && tok.sym_val == '(') {
        expr_node *e = calc_expr(lex);
        calc_token close = calc_next(lex);
        if (close.kind != TOK_SYM || close.sym_val != ')') {
            kfree(e);
            n->kind = EXPR_NUM;
            n->num_val = 0;
            return n;
        }
        kfree(n);
        return e;
    }

    n->kind = EXPR_NUM;
    n->num_val = 0;
    return n;
}

static expr_node* calc_unary(calc_lexer *lex) {
    calc_token tok = calc_next(lex);
    if (tok.kind == TOK_SYM && tok.sym_val == '-') {
        expr_node *e = calc_unary(lex);
        expr_node *n = (expr_node*)kmalloc(sizeof(expr_node));
        if (!n) { kfree(e); return 0; }
        n->kind = EXPR_NEG;
        n->left = e;
        n->right = 0;
        return n;
    }
    lex->pos--;
    return calc_primary(lex);
}

static expr_node* calc_term(calc_lexer *lex) {
    expr_node *left = calc_unary(lex);

    for (;;) {
        calc_token tok = calc_next(lex);
        if (tok.kind == TOK_SYM && (tok.sym_val == '*' || tok.sym_val == '/')) {
            expr_node *right = calc_unary(lex);
            expr_node *n = (expr_node*)kmalloc(sizeof(expr_node));
            if (!n) { kfree(left); kfree(right); return 0; }
            n->kind = EXPR_OP;
            n->op = tok.sym_val;
            n->left = left;
            n->right = right;
            left = n;
        } else {
            lex->pos--;
            break;
        }
    }

    return left;
}

static expr_node* calc_expr(calc_lexer *lex) {
    expr_node *left = calc_term(lex);

    for (;;) {
        calc_token tok = calc_next(lex);
        if (tok.kind == TOK_SYM && (tok.sym_val == '+' || tok.sym_val == '-')) {
            expr_node *right = calc_term(lex);
            expr_node *n = (expr_node*)kmalloc(sizeof(expr_node));
            if (!n) { kfree(left); kfree(right); return 0; }
            n->kind = EXPR_OP;
            n->op = tok.sym_val;
            n->left = left;
            n->right = right;
            left = n;
        } else {
            lex->pos--;
            break;
        }
    }

    return left;
}

static expr_node *calc_parse(const char *src) {
    calc_lexer lex;
    lex.src = src;
    lex.pos = 0;
    return calc_expr(&lex);
}

static double calc_eval(expr_node *e) {
    if (!e) return 0;

    switch (e->kind) {
    case EXPR_NUM:
        return e->num_val;
    case EXPR_NEG:
        return -calc_eval(e->left);
    case EXPR_OP: {
        double a = calc_eval(e->left);
        double b = calc_eval(e->right);
        switch (e->op) {
        case '+': return a + b;
        case '-': return a - b;
        case '*': return a * b;
        case '/': return b != 0 ? a / b : 0;
        default: return 0;
        }
    }
    }
    return 0;
}

static void calc_free(expr_node *e) {
    if (!e) return;
    switch (e->kind) {
    case EXPR_NUM:
        break;
    case EXPR_NEG:
        calc_free(e->left);
        break;
    case EXPR_OP:
        calc_free(e->left);
        calc_free(e->right);
        break;
    }
    kfree(e);
}

static void calc_format_result(double result, char *buf, int max) {
    int i = 0;
    if (result < 0) { buf[i++] = '-'; result = -result; }

    int integer = (int)result;
    double frac = result - integer;

    if (integer == 0) {
        buf[i++] = '0';
    } else {
        int digits[12], digit_count = 0, val = integer;
        while (val > 0 && digit_count < 12) {
            digits[digit_count++] = val % 10;
            val /= 10;
        }
        for (int j = digit_count - 1; j >= 0 && i < max - 1; j--) {
            buf[i++] = '0' + digits[j];
        }
    }

    if (frac > 0.0001 && i < max - 5) {
        buf[i++] = '.';
        for (int j = 0; j < 4 && i < max - 1; j++) {
            frac *= 10;
            int digit = (int)frac;
            buf[i++] = '0' + digit;
            frac -= digit;
        }
    }

    buf[i] = 0;
}

static void gui_launch_calculator(void) {
    static char input[CALC_INPUT_MAX];
    static char output[CALC_OUTPUT_MAX];
    int input_len = 0;
    input[0] = 0;
    output[0] = 0;

    for (;;) {
        window_clear(GUI_BG);
        gui_draw_app_titlebar("Calculator");
        font_draw_string("expr: + - * / ( ) enter evaluate  esc closes", 20, 52, 0x00807468, -1);
        window_rect(20, 76, (int)window_width() - 40, 20, 0x00FFFFFF);
        input[input_len] = 0;
        font_draw_string(input, 24, 78, 0x001C1C1E, -1);

        if (output[0]) {
            window_rect(20, 120, (int)window_width() - 40, 1, 0x00E0D8CE);
            font_draw_string("= ", 20, 136, 0x00807468, -1);
            font_draw_string(output, 40, 136, 0x001C1C1E, -1);
        }

        sleep_ticks(5);
        mouse_click_edge_sync();
        int k = get_key_or_click();
        if (k == KEY_ESC || k == KEY_CLICK) return;

        if (k == KEY_ENTER) {
            input[input_len] = 0;
            expr_node *ast = calc_parse(input);
            double result = calc_eval(ast);
            calc_free(ast);
            calc_format_result(result, output, CALC_OUTPUT_MAX);
        } else if (k == '\b') {
            if (input_len > 0) input_len--;
        } else if (input_len < CALC_INPUT_MAX - 1 && k >= 32 && k < 127) {
            input[input_len++] = (char)k;
        }
    }
}
