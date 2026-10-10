/* Claude in the Terminal. The bottom row of the Terminal window (its dock tile, or F1) is a one-line editor behind a prompt naming the model, `Claude Haiku 5.5 $ `:
   typed keys land there, Backspace takes one back, Enter runs the line. `browse`, `open N` and `llm` are commands;
   anything else goes to the Claude relay on the Mac (tools/claude-relay/relay.py) as one POST through the shared
   IP stack, HTTPS on the Pi and loopback HTTP by default in QEMU. Everything a command prints goes into the Terminal, above the prompt, wrapped to 53 columns (the house
   rule, so a photo of the Pi screen and the UART log read the same); the Console only keeps logs (docs/TERMINAL.md).
   No app, no ring 3: the kernel asks.

   Where the relay is and the token it wants come from claude_cfg.h, generated at build time by claude_cfg.sh from the
   environment and a file outside the repo; the token is sent only in the Authorization header of this one request and
   is never printed. Enter hands the line to cmd.c's cmd_run; this file answers what is not a browser command. Every outcome is one short honest line: `claude: thinking`, `claude: no token`, `claude: no
   network` (no DHCP lease: on a Pi that means Wi-Fi has not joined yet), `claude: error -401` for a relay refusal,
   `claude: timeout`. The relay keeps a session per conversation, so a follow-up question knows what came before.
   An answer may end with actions for the Pi ([[note TEXT]], [[browse URL]] and the rest: see take_actions); the Pi
   runs them and sends what they printed back as the next turn, up to AGENT_STEPS times (docs/AGENT.md). */
#include "../../drivers/net.h"
#include "../../drivers/http.h"
#include "tls.h"
#include "claude_cfg.h"
#ifdef CLAUDE_RELEASE        /* a Pi release image: no token, so the Console says `claude: no token` (the Makefile sets it) */
#undef CLAUDE_TOKEN_LEN
#define CLAUDE_TOKEN_LEN 0
#endif

void kputs(const char *s);                     /* main.c */
extern void (*cmd_out)(const char *s);         /* cmd.c: where this line's output goes */
void cmd_dec(unsigned v);
void cmd_run(const char *q, unsigned n, void (*out)(const char *));
#define kputs cmd_out
#define kdec cmd_dec
int con_columns(void);
void con_prompt(const char *s, unsigned n);
void term_output(int on);   /* main.c: what a command prints goes to the Terminal */
void *kmalloc(unsigned n);
unsigned long heap_mark(void);
unsigned long net_clock_utc(void);   /* ip.c: seconds since 1970, 0 until the network set it */
int wifi_signal_level(void);         /* wifi.c: 1 to 3 */
void heap_release(unsigned long m);
void http_post_set_bearer(const char *token);
void led_blink(unsigned times);      /* main.c, Pi only: the green light, through the firmware mailbox */

#define ASK_MAX 200          /* one question; the relay takes up to 4 KB, this keeps the JSON body small */
#define ASK_WIDTH 53         /* the Pi console rule: every printed line fits a 53-column row */
#define ASK_TICKS 18000      /* 180 s at 100 Hz: past the relay's own 150 s limit, so its 504 lands first */
#define REPLY_MAX 8192       /* the relay caps its answer at 8191 bytes */

static char line[ASK_MAX + 1], question[ASK_MAX + 1];
static unsigned len, qlen;
static int shift, pending;
static char session[37];     /* the relay's session uuid, "" before the first answer */
/* Runtime model and effort (docs/AGENT.md): /model and /effort change these, post() sends them when not default. */
static int model_i, effort_i = 1;                 /* index into models[] (0 auto) and efforts[] (1 medium, the default) */
static const char *const models[] = {"auto", "haiku", "sonnet", "opus"};
static const char *const efforts[] = {"low", "medium", "high"};
#define MODEL_MAX 32
static char last_name[MODEL_MAX + 1];             /* the model that answered last, as the relay names it ("M Claude Opus 5.5"); "" before any answer */
static unsigned steps_total;                      /* relay calls since /clear */
static int relay_state;                           /* 0 not tried, 1 answered, 2 failed */

/* The prompt names the model that last answered: "Claude Sonnet 5.5 $ ". Before any answer, and whenever the relay
   cannot be reached, it names the relay's default model (--api-model, Haiku); a build with no relay token says "Claude".
   tools/checks/relay-api-check.py keeps ASK_DEFAULT_MODEL in step with relay.py's default. */
#define ASK_DEFAULT_MODEL "Claude Haiku 5.5"
static char prompt[MODEL_MAX + 16];
/* "Claude Opus 5.5 high $ ": the model that last answered, else the one /model picked, else the default; the effort
   shows only when it is not the default (/effort, docs/AGENT.md). */
const char *ask_prompt(void) {   /* main.c draws it on the bottom row */
    unsigned k = 0;
    const char *m = last_name[0] ? last_name : model_i ? "Claude" : CLAUDE_TOKEN_LEN ? ASK_DEFAULT_MODEL : "Claude";
    for (; *m && k < MODEL_MAX; m++) prompt[k++] = *m;
    if (!last_name[0] && model_i) { prompt[k++] = ' '; for (m = models[model_i]; *m; m++) prompt[k++] = *m; }
    if (effort_i != 1) { prompt[k++] = ' '; for (m = efforts[effort_i]; *m; m++) prompt[k++] = *m; }
    prompt[k++] = ' '; prompt[k++] = '$'; prompt[k++] = ' '; prompt[k] = 0;
    return prompt;
}
/* The relay's "M <name>" line is network data: only a name that starts "Claude", short, printable ASCII, is used. */
static void take_model(const char *s, unsigned n) {
    if (n < 6 || n > MODEL_MAX || !(s[0] == 'C' && s[1] == 'l' && s[2] == 'a' && s[3] == 'u' && s[4] == 'd' && s[5] == 'e')) return;
    for (unsigned i = 0; i < n; i++) if (s[i] < 32 || s[i] > 126) return;
    for (unsigned i = 0; i < n; i++) last_name[i] = s[i];
    last_name[n] = 0;
}

/* Linux key codes 0..57 to characters, plain and with Shift. 0 is a key the editor does not type. */
static const char plain[58] = {
    0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 0, 0,
    'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 0, 0,
    'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
    'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, 0, 0, ' ' };
static const char shifted[58] = {
    0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', 0, 0,
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', 0, 0,
    'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
    'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, 0, 0, ' ' };
#define KEY_BACKSPACE 14
#define KEY_ENTER 28
#define KEY_KPENTER 96
#define KEY_LSHIFT 42
#define KEY_RSHIFT 54
#define KEY_ESC 1

int ask_char(unsigned code) { return code < 58 ? (shift ? shifted[code] : plain[code]) : 0; }   /* main.c's Calculator: same map, same Shift */
void ask_redraw(void) { con_prompt(line, len); }   /* main.c: the Terminal was reopened, put the line being typed back */

/* One key event from any keyboard. 1 when it is the editor's (the caller then keeps its echo line off the screen). */
/* One key event, while the Terminal is in front (Shift always). 1 when it is the editor's. */
static int stop_flag;        /* the kill switch: Esc, or "stop" and Enter, while the agent loop runs */
static int is_stop(const char *s, unsigned n) { return n == 4 && s[0] == 's' && s[1] == 't' && s[2] == 'o' && s[3] == 'p'; }
int ask_pending(void) { return pending; }   /* main.c: a question is running, so Esc is the kill switch and not "back to the desktop" */
int ask_key(unsigned code, unsigned value) {
    if (code == KEY_LSHIFT || code == KEY_RSHIFT) { shift = value != 0; return 1; }
    if (code == KEY_ESC) { if (value && pending) stop_flag = 1; return 0; }
    int typed = code < 58 && plain[code];
    if (!typed && code != KEY_BACKSPACE && code != KEY_ENTER && code != KEY_KPENTER) return 0;
    if (!value) return 1;                      /* key up: nothing to do, but it is still ours */
    if (typed) { if (len < ASK_MAX) line[len++] = shift ? shifted[code] : plain[code]; }
    else if (code == KEY_BACKSPACE) { if (len) len--; }
    else if (len && pending) { if (is_stop(line, len)) { stop_flag = 1; len = 0; } }   /* a question is running: only "stop" counts */
    else if (len) {                            /* Enter: hand the line to ask_poll, outside the input handler */
        for (unsigned i = 0; i < len; i++) question[i] = line[i];
        qlen = len; len = 0; pending = 1;
    }
    con_prompt(line, len);
    return 1;
}

/* The agent loop's result buffer: what an action printed, sent back to the relay as the next turn (docs/AGENT.md).
   Only filled while `capturing` is set, by say_wrapped and the action runner; cut at RESULT_MAX, never more. */
#define RESULT_MAX 1200      /* escaped twice over it still fits the relay's 4 KB body */
#define AGENT_STEPS 5
static char result[RESULT_MAX + 1];
static unsigned rlen, capturing;
static void result_add(const char *s, unsigned n) { if (capturing) for (unsigned i = 0; i < n && rlen < RESULT_MAX; i++) result[rlen++] = s[i]; }
static void result_puts(const char *s) { unsigned n = 0; while (s[n]) n++; result_add(s, n); }

/* Prints text as lines of at most `width` columns, breaking at a space where it can, and makes it plain ASCII. */
void say_wrapped(const char *prefix, const char *s, unsigned n) {   /* browser.c prints its pages through it too */
    int cols = con_columns();
    unsigned width = cols >= 20 && cols < ASK_WIDTH ? (unsigned)cols : ASK_WIDTH;
    char out[ASK_WIDTH + 2];
    unsigned o = 0, i = 0;
    for (const char *p = prefix; *p && o < width; p++) out[o++] = *p;
    while (i < n) {
        unsigned end = i, room = width - o;    /* take what fits, up to a newline */
        while (end < n && end - i < room && s[end] != '\n') end++;
        if (end < n && end - i == room && s[end] != ' ' && s[end] != '\n') {   /* the cut is inside a word: back up to it */
            unsigned b = end;
            while (b > i && s[b - 1] != ' ') b--;
            if (b > i) end = b;
        }
        for (unsigned k = i; k < end; k++) { char c = s[k]; out[o++] = c >= 32 && c < 127 ? c : '?'; }
        while (o && out[o - 1] == ' ') o--;
        out[o++] = '\n'; out[o] = 0; kputs(out); result_add(out, o); o = 0;
        i = end;
        if (i < n && s[i] == '\n') i++;        /* the newline that ended this line */
        else while (i < n && s[i] == ' ') i++; /* a wrapped line never starts with a space */
    }
    if (o) { out[o++] = '\n'; out[o] = 0; kputs(out); result_add(out, o); }
}

/* pi-actions begin: pure, no kernel calls, so tools/checks/pi-actions-check.py compiles this block on the host.
   Samantha may end an answer with actions, each alone on its line, 80 characters at most (docs/AGENT.md):
   [[note TEXT]] [[say TEXT]] [[led blink]] [[open APP]] [[browse URL]] (http or https only) [[calc EXPR]] [[status]].
   take_actions removes those lines from s in place, records at most ACT_MAX of them, and returns the new length.
   A [[ ]] line it does not know is recorded as ACT_UNKNOWN (the kernel logs and ignores it); a long one, a fifth one,
   or one inside a sentence stays as plain text. */
#define ACT_MAX 4
#define ACT_LEN 80
#define ACT_UNKNOWN 0
#define ACT_NOTE 1
#define ACT_LED 2
#define ACT_SAY 3
#define ACT_OPEN 4
#define ACT_BROWSE 5
#define ACT_CALC 6
#define ACT_STATUS 7
static int act_kind[ACT_MAX];
static char act_text[ACT_MAX][ACT_LEN + 1];
static unsigned act_n;
static int starts(const char *s, unsigned n, const char *w) {
    unsigned i = 0;
    for (; w[i]; i++) if (i >= n || s[i] != w[i]) return 0;
    return 1;
}
static int act_word(const char *p, unsigned m, const char *w, int kind, unsigned *skip) {   /* "w TEXT": kind, TEXT at *skip */
    unsigned l = 0; while (w[l]) l++;
    if (m > l + 1 && starts(p, m, w) && p[l] == ' ') { *skip = l + 1; return kind; }
    return 0;
}
static unsigned take_actions(char *s, unsigned n) {
    unsigned o = 0, i = 0;
    act_n = 0;
    while (i < n) {
        unsigned e = i, z;
        while (e < n && s[e] != '\n') e++;
        z = e;
        while (z > i && (s[z - 1] == ' ' || s[z - 1] == '\r')) z--;
        int taken = 0;
        if (z - i >= 4 && z - i <= ACT_LEN && act_n < ACT_MAX && starts(s + i, z - i, "[[") && s[z - 2] == ']' && s[z - 1] == ']') {
            const char *p = s + i + 2; unsigned m = z - i - 4, skip = 0; int kind = ACT_UNKNOWN;
            if (m == 9 && starts(p, m, "led blink")) kind = ACT_LED;
            else if (m == 6 && starts(p, m, "status")) kind = ACT_STATUS;
            else if (!(kind = act_word(p, m, "note", ACT_NOTE, &skip)) && !(kind = act_word(p, m, "say", ACT_SAY, &skip))
                     && !(kind = act_word(p, m, "open", ACT_OPEN, &skip)) && !(kind = act_word(p, m, "calc", ACT_CALC, &skip))
                     && (kind = act_word(p, m, "browse", ACT_BROWSE, &skip))
                     && !starts(p + skip, m - skip, "http://") && !starts(p + skip, m - skip, "https://")) kind = ACT_UNKNOWN;
            if (kind == ACT_LED || kind == ACT_STATUS) skip = m;   /* no text */
            if (kind == ACT_UNKNOWN) skip = 0;           /* keep the whole marker, so the log says what was ignored */
            unsigned k = 0;
            for (; skip + k < m; k++) act_text[act_n][k] = p[skip + k];
            act_text[act_n][k] = 0;
            act_kind[act_n++] = kind;
            taken = 1;
        }
        if (!taken) { for (unsigned k = i; k < e; k++) s[o++] = s[k]; if (e < n) s[o++] = '\n'; }
        i = e < n ? e + 1 : e;
    }
    while (o && (s[o - 1] == '\n' || s[o - 1] == ' ' || s[o - 1] == '\r')) o--;   /* no blank tail where markers were */
    return o;
}
/* pi-actions end */

static int is_session(const char *s, unsigned n) {
    if (n != 36) return 0;
    for (unsigned i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == '-')) return 0;
    }
    return 1;
}

static unsigned put_dec(char *o, unsigned long v) {   /* decimal digits of v at o, returns how many */
    char t[20]; unsigned n = 0, k = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) o[k++] = t[--n];
    return k;
}
/* The local model (llm.c): `llm PROMPT` at the Terminal's prompt, and Samantha's answer when the relay cannot be reached. */
int llm_generate(const char *prompt, unsigned n, char *out, unsigned cap, unsigned *npos, unsigned *tps10);
static void local(const char *q, unsigned n) {
    static char out[2048];
    unsigned npos, tps10;
    kputs("llm: thinking\n");
    int got = llm_generate(q, n, out, sizeof out, &npos, &tps10);
    if (got == -1) { kputs("llm: no model in this build\n"); return; }
    if (got == -2) { kputs("llm: out of memory\n"); return; }
    if (got == -3) { kputs("llm: the model files are bad\n"); return; }
    if (got == -4) { kputs("llm: prompt too long\n"); return; }
    say_wrapped("", out, (unsigned)got);
    kputs("llm: "); kdec(npos); kputs(" tokens, "); kdec(tps10 / 10); kputs("."); kdec(tps10 % 10); kputs(" tok/s\n");
}
int llm_present(void);
static void fallback(const char *q, unsigned n) { if (llm_present()) local(q, n); }   /* silent in a build with no model */

static unsigned pi_status(char *st) {   /* "ip 10.0.0.189, utc 1791..., wifi 3/3" into st (80 bytes), returns its length */
    unsigned k = 0, ip = net_get_ip(); unsigned long u = net_clock_utc();
    for (const char *p = "ip "; *p; p++) st[k++] = *p;
    for (int sh = 24; sh >= 0; sh -= 8) { k += put_dec(st + k, (ip >> sh) & 255); if (sh) st[k++] = '.'; }
    for (const char *p = ", utc "; *p; p++) st[k++] = *p;
    k += put_dec(st + k, u);
    for (const char *p = ", wifi "; *p; p++) st[k++] = *p;
    st[k++] = (char)('0' + wifi_signal_level()); st[k++] = '/'; st[k++] = '3';
    st[k] = 0;
    return k;
}

/* Slash commands run on the Pi and are never sent (docs/AGENT.md). Returns 1 when q was one. */
static int word_is(const char *s, unsigned n, const char *w) { unsigned i = 0; for (; w[i]; i++) if (i >= n || s[i] != w[i]) return 0; return i == n; }
static void choices(const char *label, const char *const *list, unsigned count, unsigned cur) {
    kputs(label); kputs(":");
    for (unsigned i = 0; i < count; i++) { kputs(i == cur ? " [" : " "); kputs(list[i]); if (i == cur) kputs("]"); }
    kputs("\n");
}
void con_clear(void);   /* main.c: empties the Console window, keeps the log */
static int slash(const char *q, unsigned n) {
    if (!n || q[0] != '/') return 0;
    unsigned c = 1; while (c < n && q[c] != ' ') c++;      /* the command word is q[1..c) */
    const char *a = q + c; unsigned an = n - c;            /* its argument, trimmed */
    while (an && *a == ' ') { a++; an--; }
    while (an && a[an - 1] == ' ') an--;
    const char *w = q + 1; unsigned wn = c - 1;
    if (word_is(w, wn, "model") || word_is(w, wn, "effort")) {
        int is_model = w[0] == 'm';
        const char *const *list = is_model ? models : efforts; unsigned count = is_model ? 4 : 3, i;
        if (!an) { choices(is_model ? "model" : "effort", list, count, is_model ? (unsigned)model_i : (unsigned)effort_i); return 1; }
        for (i = 0; i < count; i++) if (word_is(a, an, list[i])) break;
        if (i == count) { kputs(is_model ? "model: not one of auto, haiku, sonnet, opus\n" : "effort: not one of low, medium, high\n"); return 1; }
        if (is_model) { model_i = (int)i; last_name[0] = 0; } else effort_i = (int)i;
        kputs(is_model ? "model: " : "effort: "); kputs(list[i]); kputs("\n");
        ask_redraw();                                      /* the prompt shows it */
    } else if (word_is(w, wn, "status")) {
        kputs("status: model "); kputs(models[model_i]); kputs(", effort "); kputs(efforts[effort_i]); kputs("\n");
        kputs("status: prompt "); kputs(ask_prompt()); kputs("\n");
        kputs("status: relay ");
        kputs(!CLAUDE_TOKEN_LEN ? "no token" : !net_get_gateway() ? "no network" : relay_state == 1 ? "answered last time"
              : relay_state == 2 ? "did not answer last time" : "not tried yet");
        kputs("\nstatus: steps used "); kdec(steps_total); kputs("\n");
    } else if (word_is(w, wn, "clear")) {
        session[0] = 0; last_name[0] = 0; steps_total = 0; relay_state = 0;
        con_clear(); kputs("clear: new conversation\n"); ask_redraw();
    } else if (word_is(w, wn, "help")) {
        kputs("/model [auto|haiku|sonnet|opus]  pick the model\n/effort [low|medium|high]  how hard it thinks\n"
              "/status  model, effort, relay, steps\n/clear  new conversation\n");
    } else kputs("unknown command, try /help\n");
    return 1;
}

#if defined(PI_BUILD) || CLAUDE_TLS
/* The Pi never sends its bearer over HTTP. QEMU retains its existing HTTP path unless TLS is requested. */
static int relay_post_tls(const char *body, unsigned len, const char *token, char *reply, int *status) {
    static char req[2 * RESULT_MAX + 512 + sizeof(CLAUDE_HOST)];
    char digits[10]; unsigned n = 0, dn = 0, v = len;
    do { digits[dn++] = (char)('0' + v % 10); v /= 10; } while (v);
    const char *parts[] = {"POST /api/claude HTTP/1.0\r\nHost: ", CLAUDE_HOST, "\r\nAuthorization: Bearer ", token,
                          "\r\nContent-Type: application/json\r\nContent-Length: "};
    for (unsigned i = 0; i < 5; i++) for (const char *p = parts[i]; *p; p++) req[n++] = *p;
    while (dn) req[n++] = digits[--dn];
    for (const char *p = "\r\nConnection: close\r\n\r\n"; *p; p++) req[n++] = *p;
    for (unsigned i = 0; i < len; i++) req[n++] = body[i];
    char *raw = kmalloc(REPLY_MAX + 2048);
    int got = raw ? https_fetch_timeout(CLAUDE_HOST, CLAUDE_PORT, req, n, raw, REPLY_MAX + 2048, ASK_TICKS) : -1;
    for (unsigned i = 0; i < n; i++) ((volatile char *)req)[i] = 0;
    *status = got > 0 ? http_status_of(raw, (unsigned)got) : 0;
    if (got <= 0) return -1;
    int start = http_body_start(raw, (unsigned)got);
    if (start < 0 || (unsigned)(got - start) > REPLY_MAX) return -1;
    got -= start;
    for (int i = 0; i < got; i++) reply[i] = raw[start + i];
    return got;
}
#endif

/* One POST to the relay. Returns the answer's length with *ans pointing into reply, -1 when the relay never answered
   (the caller may fall back to the local model) and -2 for a refusal it already printed. */
static int post(const char *q, unsigned n, char *reply, char **ans) {
    static char body[2 * RESULT_MAX + 256];
    unsigned b = 0;                            /* {"prompt":"...","session":"...","pi":"..."}: " and \ escaped, newlines as \n, other controls as spaces */
    for (const char *p = "{\"prompt\":\""; *p; p++) body[b++] = *p;
    for (unsigned i = 0; i < n; i++) {
        char c = q[i];
        if (c == '"' || c == '\\') { body[b++] = '\\'; body[b++] = c; }
        else if (c == '\n') { body[b++] = '\\'; body[b++] = 'n'; }
        else body[b++] = (unsigned char)c < 32 ? ' ' : c;
    }
    for (const char *p = "\",\"session\":\""; *p; p++) body[b++] = *p;
    for (const char *p = session; *p; p++) body[b++] = *p;
    if (model_i) { for (const char *p = "\",\"model\":\""; *p; p++) body[b++] = *p; for (const char *p = models[model_i]; *p; p++) body[b++] = *p; }
    if (effort_i != 1) { for (const char *p = "\",\"effort\":\""; *p; p++) body[b++] = *p; for (const char *p = efforts[effort_i]; *p; p++) body[b++] = *p; }
    for (const char *p = "\",\"pi\":\""; *p; p++) body[b++] = *p;
    b += pi_status(body + b);                  /* the Pi's live status, so the answer can be about this machine */
    body[b++] = '"'; body[b++] = '}';
    kputs("claude: thinking\n");
    static char token[64];
    for (int i = 0; i < CLAUDE_TOKEN_LEN && i < 63; i++) token[i] = (char)claude_token[i];
    token[CLAUDE_TOKEN_LEN < 63 ? CLAUDE_TOKEN_LEN : 63] = 0;
    unsigned long mark = heap_mark();          /* http.c borrows two buffers from the bump heap; give them back */
    int status;
#if defined(PI_BUILD) || CLAUDE_TLS
    int got = relay_post_tls(body, b, token, reply, &status);
#else
    http_post_set_bearer(token);
    int got = http_post_timeout(CLAUDE_HOST, "/api/claude", CLAUDE_PORT, body, b, reply, REPLY_MAX, ASK_TICKS);
    http_post_set_bearer(0);
    status = http_last_status();
#endif
    heap_release(mark);
    for (int i = 0; i < 64; i++) token[i] = 0;
    steps_total++;
    if (got < 0) {
        relay_state = 2; last_name[0] = 0;     /* out of reach: the prompt goes back to the default model */
        int e = net_last_error();
#if defined(PI_BUILD) || CLAUDE_TLS
        kputs("claude: TLS failed, no HTTP fallback\n");
#endif
        if (e == NET_ERR_CONNECT_TIMEOUT || e == NET_ERR_REPLY_TIMEOUT) kputs("claude: timeout\n");
        else { kputs("claude: error "); kputs(e == NET_ERR_NONE ? "busy" : net_error_name(e)); kputs("\n"); }
        return -1;
    }
    reply[got] = 0;
    relay_state = status == 200 ? 1 : 2;
    if (status != 200) {                       /* the relay's own one-line reason follows its code */
        if (!status) { kputs("claude: error bad reply\n"); return -2; }
        kputs("claude: error -"); kdec((unsigned)status); kputs("\n");
        say_wrapped("", reply, (unsigned)got);
        return -2;
    }
    unsigned start = 0;                        /* "S <session uuid>\nM <model>\n<answer>" */
    if (got >= 2 && reply[0] == 'S' && reply[1] == ' ') {
        unsigned e = 2; while (e < (unsigned)got && reply[e] != '\n') e++;
        if (e - 2 >= 36 && is_session(reply + 2, 36)) {
            for (unsigned i = 0; i < 36; i++) session[i] = reply[2 + i];
            session[36] = 0;
        }
        start = e < (unsigned)got ? e + 1 : e;
    }
    if (start + 2 <= (unsigned)got && reply[start] == 'M' && reply[start + 1] == ' ') {   /* "M <model>": the one that answered, for the prompt */
        unsigned e = start + 2; while (e < (unsigned)got && reply[e] != '\n') e++;
        take_model(reply + start + 2, e - start - 2);
        start = e < (unsigned)got ? e + 1 : e;
    }
    *ans = reply + start;
    return got - (int)start;
}

int app_open_name(const char *name);         /* main.c */
int browse_command(const char *q, unsigned n);   /* browser.c: the agent's [[browse URL]] runs it directly */
int calc_eval_str(const char *src, int deg, char *out);   /* calc.c: 0 ok, out holds 32 bytes */
void input_drain(void);                      /* main.c: the keys that queued during a request */

/* Runs the recorded actions, printing each outcome, and writes the same outcomes into `result` for the next turn. */
static void run_actions(void) {
    static const char *const names[] = {"", "note", "led blink", "say", "open", "browse", "calc", "status"};
    rlen = 0; capturing = 1;
    result_puts("The Pi ran your actions. Results:\n");
    for (unsigned a = 0; a < act_n; a++) {
        const char *t = act_text[a]; unsigned k = 0; while (t[k]) k++;
        capturing = 0;
        if (act_kind[a] == ACT_UNKNOWN) { kputs("agent: ignored [["); kputs(t); kputs("]]\n"); continue; }
        capturing = 1;
        result_puts(names[act_kind[a]]); if (k) { result_puts(" "); result_puts(t); } result_puts(": ");
        switch (act_kind[a]) {
        case ACT_NOTE: say_wrapped("pi: ", t, k); break;
        case ACT_SAY: say_wrapped("say: ", t, k); break;   /* no voice on ARM yet: a caption */
        case ACT_LED:
#ifdef PI_BUILD
            led_blink(4); result_puts("blinked\n");
#else
            kputs("pi: no green light on QEMU\n"); result_puts("no light on QEMU\n");
#endif
            break;
        case ACT_OPEN: {
            int ok = app_open_name(t);
            kputs(ok ? "pi: opened " : "pi: no app named "); kputs(t); kputs("\n");
            result_puts(ok ? "opened\n" : "no such app\n");
            break; }
        case ACT_CALC: {
            char out[32]; int err = calc_eval_str(t, 0, out);
            kputs("calc: "); kputs(t); kputs(" = "); kputs(err ? "error" : out); kputs("\n");
            result_puts(err ? "error\n" : out); if (!err) result_puts("\n");
            break; }
        case ACT_STATUS: {
            char st[80]; pi_status(st);
            kputs("pi: "); kputs(st); kputs("\n"); result_puts(st); result_puts("\n");
            break; }
        case ACT_BROWSE: {                     /* browser.c prints the page through say_wrapped, so it lands in result */
            char cmd[ACT_LEN + 8]; unsigned o = 0;
            for (const char *p = "browse "; *p; p++) cmd[o++] = *p;
            for (unsigned i = 0; i < k; i++) cmd[o++] = t[i];
            browse_command(cmd, o);
            break; }
        }
        capturing = 0;
    }
}

int ask_command(const char *q, unsigned n) {   /* cmd.c, before the browser: "stop" with nothing running, and the slash commands */
    if (is_stop(q, n)) { kputs("agent: nothing running\n"); return 1; }
    return slash(q, n);
}
void ask_claude(const char *q, unsigned n) {   /* cmd.c sends here what is not a command or a browser line */
    static char reply[REPLY_MAX + 1];
    if (n > 4 && q[0] == 'l' && q[1] == 'l' && q[2] == 'm' && q[3] == ' ') { local(q + 4, n - 4); return; }
    if (!CLAUDE_TOKEN_LEN) { kputs("claude: no token\n"); fallback(q, n); return; }
    if (!net_get_gateway()) {                  /* no DHCP lease, or no card at all */
#ifdef PI_BUILD
        kputs("claude: no network, Wi-Fi has not joined yet\n");
#else
        kputs("claude: no network\n");
#endif
        last_name[0] = 0;                      /* out of reach: the prompt goes back to the default model */
        fallback(q, n);                        /* the relay is out of reach: the local model answers */
        return;
    }
    /* The agent loop (docs/AGENT.md): an answer that ends with actions gets them run, and what they printed goes back
       to the relay as the next turn, up to AGENT_STEPS times. Esc or "stop" between steps ends it. */
    stop_flag = 0;
    const char *prompt = q; unsigned plen = n;
    for (unsigned step = 1; step <= AGENT_STEPS; step++) {
        char *ans;
        kputs("agent: step "); kdec(step); kputs("\n");
        int got = post(prompt, plen, reply, &ans);
        if (got == -1 && step == 1) fallback(q, n);   /* the relay never answered the question: the local model does */
        if (got < 0) return;
        say_wrapped("", ans, take_actions(ans, (unsigned)got));   /* the answer first, then what she asked the Pi to do */
        if (!act_n) return;
        run_actions();
        if (step == AGENT_STEPS) { kputs("agent: step limit\n"); return; }
        input_drain();
        if (stop_flag) { kputs("agent: stopped\n"); return; }
        prompt = result; plen = rlen;
    }
}

/* Called from the main loop: runs a question Enter queued, so the keyboard handler never blocks on the network. */
void ask_poll(void) {
    if (!pending) return;
    term_output(1);
    say_wrapped(ask_prompt(), question, qlen);
    cmd_run(question, qlen, 0);                /* cmd.c: the one command line (help, /model, browse, llm, Claude), into the Terminal */
    term_output(0);
    pending = 0;
    con_prompt(line, len);
}
