/* Claude in the Terminal. The bottom row of the Terminal window (its dock tile, or F1) is a one-line editor behind a zsh style prompt, `joshua@tree % `:
   typed keys land there, Backspace takes one back, Enter runs the line. `browse`, `open N` and `llm` are commands, and so is
   `sam QUESTION`: it goes to the Claude relay on the Mac (tools/claude-relay/relay.py) as one POST through the shared
   IP stack, HTTPS on the Pi and loopback HTTP by default in QEMU. Everything a command prints goes into the Terminal, above the prompt, wrapped to 53 columns (the house
   rule, so a photo of the Pi screen and the UART log read the same); the Console only keeps logs (docs/GUIDE.md).
   No app, no ring 3: the kernel asks.

   Where the relay is and the token it wants come from claude_cfg.h, generated at build time by claude_cfg.sh from the
   environment and a file outside the repo; the token is sent only in the Authorization header of this one request and
   is never printed. Enter hands the line to cmd.c's cmd_run; this file answers what is not a browser command. Every outcome is one short honest line: `claude: thinking`, `claude: no token`, `claude: no
   network` (no DHCP lease: on a Pi that means Wi-Fi has not joined yet), `claude: error -401` for a relay refusal,
   `claude: timeout`. The relay keeps a session per conversation, so a follow-up question knows what came before.
   An answer may end with actions for the Pi ([[note TEXT]], [[browse URL]] and the rest: see take_actions); the Pi
   runs them and sends what they printed back as the next turn, up to AGENT_STEPS times (docs/GUIDE.md).

   `sam` never blocks the prompt. The command only queues the question; ask_poll starts it on the next pass of the main
   loop, and while it waits on the network (drivers/net.c calls net_idle once a pass in every client wait loop) ask_service
   polls the keyboard and runs any line typed meanwhile at once, so the answer lands on a fresh line above the prompt
   whenever it arrives. Only one request flies at a time. Commands that need the one TCP connection or the heap (browse,
   open, llm, ...) are refused with a short line until she answers (cmd.c).

   `sam` with no question opens the chat view (docs/GUIDE.md): the Terminal window becomes a header, a scrolling history
   of your lines and hers, and an input row. Each line goes through the same sam_command as `sam QUESTION`, so sums, the
   slash commands and the relay behave alike; her output is captured into the history instead of the Terminal's log. Esc or
   /quit goes back to the shell (after her answer, when one is in flight). Every drawn line is also written to the UART as
   `tui| ...` so a headless check reads the screen. */
#include "../../drivers/net.h"
#include "../../drivers/http.h"
#include "tls.h"
#include "claude_cfg.h"
#include "emmc.h"
#include "../../drivers/fat.h"
#ifdef CLAUDE_RELEASE        /* a Pi release image: no token, so the Console says `claude: no token` (the Makefile sets it) */
#undef CLAUDE_TOKEN_LEN
#define CLAUDE_TOKEN_LEN 0
#endif

void kputs(const char *s);                     /* main.c */
static void (*const real_puts)(const char *) = kputs;   /* the Terminal's own printer, before kputs means cmd_out below */
extern void (*cmd_out)(const char *s);         /* cmd.c: where this line's output goes */
void cmd_dec(unsigned v);
void cmd_run(const char *q, unsigned n, void (*out)(const char *));
#define kputs cmd_out
#define kdec cmd_dec
int con_columns(void);
void con_prompt(const char *s, unsigned n);
void term_output(int on);   /* main.c: what a command prints goes to the Terminal */
void *kmalloc(unsigned n);
unsigned int ticks(void);            /* ip.c: 100 a second */
unsigned long heap_mark(void);
unsigned long net_clock_utc(void);   /* ip.c: seconds since 1970, 0 until the network set it */
int wifi_signal_level(void);         /* wifi.c: 1 to 3 */
void heap_release(unsigned long m);
void http_post_set_bearer(const char *token);
void led_blink(unsigned times);      /* main.c, Pi only: the green light, through the firmware mailbox */

static void ask_service(void);
extern void (*net_idle)(void);   /* drivers/net.c */
#define ASK_MAX 200          /* one question; the relay takes up to 4 KB, this keeps the JSON body small */
#define ASK_WIDTH 53         /* the Pi console rule: every printed line fits a 53-column row */
#define ASK_TICKS 3000       /* 30 s at 100 Hz: no answer by then and `sam` stops waiting (the board once took 2 to 3 minutes) */
#define ASK_SECS 30
#define REPLY_MAX 8192       /* the relay caps its answer at 8191 bytes */

#define MODEL_MAX 32
struct ask_context {
    char line[ASK_MAX + 1], question[ASK_MAX + 1], session[37], last_name[MODEL_MAX + 1];
    unsigned len, qlen, steps_total;
    int pending, model_i, effort_i, relay_state;
};
static struct ask_context contexts[2] = {{.effort_i = 1}, {.effort_i = 1}};
static struct ask_context *ac = &contexts[0];
static int shift;
static const char *const models[] = {"auto", "haiku", "sonnet", "opus"};
static const char *const efforts[] = {"low", "medium", "high"};
/* A request keeps its session and output target until it finishes or is stopped. */
static int sam_state;        /* 0 idle, 1 queued by `sam`, 2 waiting on the relay */
static int tui_on, tui_quit;         /* the chat view has the window; leave it when the question in flight ends */
static unsigned tui_n, tui_up;       /* history lines held; how many lines the view is scrolled back from the newest */
void sam_command(const char *q, unsigned n);
static void tui_enter(void);
static void tui_paint(int full);
static void tui_escape(void);
static void tui_submit(void);
static void tui_move(int up);
static void prompt_row(const char *s, unsigned n);   /* the bottom row: the shell prompt, or the chat view's */
void tui_log(const char *s);       /* main.c: a line to the UART only */
int con_rows_total(void);          /* main.c: text rows in the Terminal window, 0 when there is no window to draw in */
void con_tui_begin(void);
void con_tui_rows(const char *const *rows, unsigned n);
void con_tui_input(const char *p, const char *s, unsigned n);
void con_tui_end(void);
static char sam_q[ASK_MAX + 1];
static unsigned sam_qn;
int ask_select(unsigned pane) {
    if (pane >= 2 || ac->pending || sam_state || tui_on) return 0;
    ac = &contexts[pane];
    return 1;
}

/* The prompt is zsh style and never changes: `joshua@tree % `. The model that last answered shows in /status, not on the
   prompt (Samantha is a command now, not the shell). tools/checks/relay-api-check.py keeps ASK_DEFAULT_MODEL in step
   with relay.py's TURING_NAME. */
#define ASK_DEFAULT_MODEL "Samantha"
const char *ask_prompt(void) { return "joshua@tree % "; }   /* main.c draws it on the bottom row */
/* The relay's "M <name>" line is network data: only "Samantha" (Turing) or a short printable name that starts
   "Claude" is used. */
static void take_model(const char *s, unsigned n) {
    static const char sam[] = "Samantha";
    int is_sam = n == sizeof sam - 1;
    for (unsigned i = 0; is_sam && i < n; i++) is_sam = s[i] == sam[i];
    if (!is_sam && (n < 6 || n > MODEL_MAX || !(s[0] == 'C' && s[1] == 'l' && s[2] == 'a' && s[3] == 'u' && s[4] == 'd' && s[5] == 'e'))) return;
    for (unsigned i = 0; i < n; i++) if (s[i] < 32 || s[i] > 126) return;
    for (unsigned i = 0; i < n; i++) ac->last_name[i] = s[i];
    ac->last_name[n] = 0;
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

#define KEY_CAPSLOCK 58
static int caps;   /* Caps Lock: letters only, and Shift inverts it. No keyboard LED yet. */
int ask_char(unsigned code) {   /* main.c's Calculator and Spotlight: same map, same Shift, same Caps */
    if (code >= 58) return 0;
    int up = plain[code] >= 'a' && plain[code] <= 'z' ? shift != caps : shift;
    return up ? shifted[code] : plain[code];
}
void ask_prefill(const char *s) {   /* main.c: the Samantha tile types `sam ` on an empty line, so the next word is the question */
    if (ac->len || ac->pending || sam_state) return;
    while (*s && ac->len < ASK_MAX) ac->line[ac->len++] = *s++;
    ac->line[ac->len] = 0;
}
void ask_redraw(void) { if (tui_on) tui_paint(1); else con_prompt(ac->line, ac->len); }   /* main.c: the Terminal was reopened, put the line being typed back */

/* One key event from any keyboard. 1 when it is the editor's (the caller then keeps its echo line off the screen). */
/* One key event, while the Terminal is in front (Shift always). 1 when it is the editor's. */
static int stop_flag;        /* the kill switch: Esc, or "stop" and Enter, while the agent loop runs */
static int is_stop(const char *s, unsigned n) { return n == 4 && s[0] == 's' && s[1] == 't' && s[2] == 'o' && s[3] == 'p'; }
int ask_pending(void) { return ac->pending || sam_state || tui_on; }   /* main.c: a question is running, so Esc is the kill switch and not "back to the desktop" */
int ask_key(unsigned code, unsigned value) {
    if (code == KEY_LSHIFT || code == KEY_RSHIFT) { shift = value != 0; return 1; }
    if (code == KEY_CAPSLOCK) { if (value) caps = !caps; return 1; }   /* one flip per press */
    if (code == KEY_ESC && tui_on) { if (value) tui_escape(); return 1; }
    if (tui_on && (code == 103 || code == 108)) { if (value) tui_move(code == 103); return 1; }   /* Up and Down scroll the history */
    if (code == KEY_ESC) { if (value && (ac->pending || sam_state)) stop_flag = 1; return 0; }
    int typed = code < 58 && plain[code];
    if (!typed && code != KEY_BACKSPACE && code != KEY_ENTER && code != KEY_KPENTER) return 0;
    if (!value) return 1;                      /* key up: nothing to do, but it is still ours */
    if (typed) { if (ac->len < ASK_MAX) ac->line[ac->len++] = (char)ask_char(code); }
    else if (code == KEY_BACKSPACE) { if (ac->len) ac->len--; }
    else if (ac->len && tui_on) { tui_submit(); return 1; }
    else if (ac->len && ac->pending) { if (is_stop(ac->line, ac->len)) { stop_flag = 1; ac->len = 0; } }   /* a question is running: only "stop" counts */
    else if (ac->len) {                            /* Enter: hand the line to ask_poll, outside the input handler */
        for (unsigned i = 0; i < ac->len; i++) ac->question[i] = ac->line[i];
        ac->qlen = ac->len; ac->len = 0; ac->pending = 1;
    }
    prompt_row(ac->line, ac->len);
    return 1;
}

void clip_put(const char *s, unsigned n);       /* main.c, 3.10.0: the system clipboard */
unsigned clip_take(char *o, unsigned room);
/* Cmd or Ctrl with a letter, while the Terminal is in front (main.c checked the modifier). C copies the whole input line, X
   cuts it, V adds the clipboard at the end of the line (a line break or tab is a space, other control bytes are dropped; a
   running question allows only C), A is taken but does nothing: the whole line is always what C and X use. 1 if it was one
   of the four, so the letter is not typed. */
int ask_edit(unsigned code) {
    if (code != 46 && code != 45 && code != 47 && code != 30) return 0;
    if (code == 46 || code == 45) { clip_put(ac->line, ac->len); if (code == 45 && !ac->pending) ac->len = 0; }
    else if (code == 47 && !ac->pending) {
        char b[ASK_MAX + 1]; unsigned n = clip_take(b, ASK_MAX - ac->len);
        for (unsigned i = 0; i < n; i++) { char c = b[i] == '\n' || b[i] == '\t' ? ' ' : b[i]; if (c >= ' ' && c < 127) ac->line[ac->len++] = c; }
    }
    prompt_row(ac->line, ac->len);
    return 1;
}

/* The agent loop's result buffer: what an action printed, sent back to the relay as the next turn (docs/GUIDE.md).
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
   Samantha may end an answer with actions, each alone on its line, 80 characters at most (docs/GUIDE.md):
   [[note TEXT]] [[say TEXT]] [[led blink]] [[open APP]] [[browse URL]] (http or https only) [[calc EXPR]] [[status]]
   [[save NAME]] (the answer's first ``` block into JT/NAME, only when the question itself named NAME), [[list]] (the
   files in JT/) and [[read NAME]] (JT/NAME, the first 4 KB): both read only.
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
#define ACT_SAVE 8
#define ACT_LIST 9
#define ACT_READ 10
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
            else if (m == 4 && starts(p, m, "list")) kind = ACT_LIST;
            else if (!(kind = act_word(p, m, "note", ACT_NOTE, &skip)) && !(kind = act_word(p, m, "say", ACT_SAY, &skip))
                     && !(kind = act_word(p, m, "open", ACT_OPEN, &skip)) && !(kind = act_word(p, m, "calc", ACT_CALC, &skip))
                     && !(kind = act_word(p, m, "save", ACT_SAVE, &skip)) && !(kind = act_word(p, m, "read", ACT_READ, &skip))
                     && (kind = act_word(p, m, "browse", ACT_BROWSE, &skip))
                     && !starts(p + skip, m - skip, "http://") && !starts(p + skip, m - skip, "https://")) kind = ACT_UNKNOWN;
            if (kind == ACT_LED || kind == ACT_STATUS || kind == ACT_LIST) skip = m;   /* no text */
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
/* The body of the first ``` fenced block in s (the line after the opening fence, up to the closing one): its length,
   with *at its start. 0 when there is no complete block. What [[save NAME]] writes. */
static unsigned code_block(const char *s, unsigned n, unsigned *at) {
    unsigned i = 0;
    while (i + 3 <= n && !(s[i] == '`' && s[i + 1] == '`' && s[i + 2] == '`')) i++;
    if (i + 3 > n) return 0;
    while (i < n && s[i] != '\n') i++;          /* past the fence and its language word */
    if (i >= n) return 0;
    unsigned b = ++i;
    for (; i + 3 <= n; i++)
        if (s[i] == '`' && s[i + 1] == '`' && s[i + 2] == '`' && (i == b || s[i - 1] == '\n')) { *at = b; return i - b; }
    return 0;
}
/* 1 when `name` appears in q, any case: a file is only ever saved under a name the person typed (Turing's law 12). */
static int named_in(const char *q, unsigned qn, const char *name) {
    unsigned l = 0; while (name[l]) l++;
    for (unsigned i = 0; l && i + l <= qn; i++) {
        unsigned k = 0;
        for (; k < l; k++) { char a = q[i + k], b = name[k]; if (a >= 'A' && a <= 'Z') a += 32; if (b >= 'A' && b <= 'Z') b += 32; if (a != b) break; }
        if (k == l) return 1;
    }
    return 0;
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

/* Slash commands run on the Pi and are never sent (docs/GUIDE.md). Returns 1 when q was one. */
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
        if (!an) { choices(is_model ? "model" : "effort", list, count, is_model ? (unsigned)ac->model_i : (unsigned)ac->effort_i); return 1; }
        for (i = 0; i < count; i++) if (word_is(a, an, list[i])) break;
        if (i == count) { kputs(is_model ? "model: not one of auto, haiku, sonnet, opus\n" : "effort: not one of low, medium, high\n"); return 1; }
        if (is_model) { ac->model_i = (int)i; ac->last_name[0] = 0; } else ac->effort_i = (int)i;
        kputs(is_model ? "model: " : "effort: "); kputs(list[i]); kputs("\n");
        ask_redraw();                                      /* the prompt shows it */
    } else if (word_is(w, wn, "status")) {
        kputs("status: model "); kputs(models[ac->model_i]); kputs(", effort "); kputs(efforts[ac->effort_i]); kputs("\n");
        kputs("status: last answer "); kputs(ac->last_name[0] ? ac->last_name : ASK_DEFAULT_MODEL); kputs("\n");
        kputs("status: relay ");
        kputs(!CLAUDE_TOKEN_LEN ? "no token" : !net_get_gateway() ? "no network" : ac->relay_state == 1 ? "answered last time"
              : ac->relay_state == 2 ? "did not answer last time" : "not tried yet");
        kputs("\nstatus: steps used "); kdec(ac->steps_total); kputs("\n");
    } else if (word_is(w, wn, "clear")) {
        ac->session[0] = 0; ac->last_name[0] = 0; ac->steps_total = 0; ac->relay_state = 0;
        if (tui_on) { tui_n = 0; tui_up = 0; }
        con_clear(); kputs("clear: new conversation\n"); ask_redraw();
    } else if (word_is(w, wn, "help")) {
        kputs("/model [auto|haiku|sonnet|opus]  pick the model\n/effort [low|medium|high]  how hard it thinks\n"
              "/status  model, effort, relay, steps\n/clear  new conversation\n");
    } else kputs("unknown command, try /help\n");
    return 1;
}

/* Where one question's time went, in ticks (100 a second): finding the relay (name lookup, the LAN probe), the TLS connect
   and handshake, and the wait from the request going out to the whole answer back. Summed over the agent steps and
   printed as one `sam: N.Ns (find a, tls b, wait c)` line when the question ends. */
static unsigned t_find, t_tls, t_wait, timed;
static unsigned t_mark;          /* sam_run: when the question started */
static unsigned request_sent;    /* the request is out: a timeout after this is the 30 s answer wait, not the connect */
static void put_secs(unsigned t) { kdec(t / 100); kputs("."); kdec((t % 100) / 10); }
static void timing_line(void) {
    if (!timed) return;
    kputs("sam: "); put_secs(ticks() - t_mark); kputs("s (find "); put_secs(t_find); kputs(", tls "); put_secs(t_tls);
    kputs(", wait "); put_secs(t_wait); kputs(")\n");
}

#if defined(PI_BUILD) || CLAUDE_TLS
/* The Pi never sends its bearer over HTTP. QEMU retains its existing HTTP path unless TLS is requested.
   Finding the relay (2.46): the Mac's LAN address can change under the card, so the address does not matter. The relay's
   certificate names RELAY_NAME and chains to our private CA (TLS_TA), and that is what is checked, whatever the IP.
   The Pi tries the address that answered last, else the baked CLAUDE_HOST; if that will not connect or will not prove
   itself, it broadcasts one probe on the LAN (relay.py answers on its own port) and tries the address the reply came
   from. The reply is untrusted and carries nothing secret: it only says where to try. The token goes out only after
   a verified handshake, and nothing is retried once the request has been sent, so Claude never runs twice. */
#define RELAY_NAME "jt-relay"
#define RELAY_PROBE_SRC 53101        /* our side of the probe */
#define RELAY_PROBE_TICKS 150        /* 1.5 s for an answer from the LAN */
#define RELAY_CONNECT_TICKS 300      /* 3 s for a SYN-ACK: the relay is on the LAN, a dead address should fail fast */
static unsigned relay_ip;            /* the relay that last proved itself, 0 until one has */
static unsigned short relay_port;

/* The card remembers it too: JT/RELAY.TXT holds "a.b.c.d:port" and nothing else (no token), so the first question after
   a reboot goes straight to the relay. It is only a hint; the TLS handshake still has to prove the name jt-relay. */
#define RELAY_FILE "RELAY.TXT"
#define RELAY_CACHED_TICKS 200       /* 2 s for a SYN-ACK from the relay that worked last time; failing it falls back to the probe */
static unsigned card_ip; static unsigned short card_port; static int card_loaded;
static void card_load(void) {
    char b[32]; unsigned got, ip = 0, i = 0, port = 0;
    card_loaded = 1;
    if (!sd_ready() || !sd_enter_jt(0)) return;
    int r = fat_read_file(RELAY_FILE, b, sizeof b - 1);
    fat_chdir("..");
    if (r < 7) return;
    got = (unsigned)r;
    for (int o = 0; o < 4; o++) {
        unsigned v = 0, d = 0;
        while (i < got && b[i] >= '0' && b[i] <= '9' && v < 1000) { v = v * 10 + (unsigned)(b[i++] - '0'); d++; }
        if (!d || v > 255 || i >= got || b[i] != (o < 3 ? '.' : ':')) return;
        ip = ip << 8 | v; i++;
    }
    while (i < got && b[i] >= '0' && b[i] <= '9' && port <= 65535) port = port * 10 + (unsigned)(b[i++] - '0');
    if (port < 1 || port > 65535) return;
    card_ip = ip; card_port = (unsigned short)port;
}
static void card_save(unsigned ip, unsigned short port) {   /* only when the address changed: a write a question would be wasteful */
    char b[24]; unsigned n = 0;
    if (card_loaded && ip == card_ip && port == card_port) return;
    if (!sd_ready()) return;
    for (int s = 24; s >= 0; s -= 8) { n += put_dec(b + n, (ip >> s) & 255); b[n++] = s ? '.' : ':'; }
    n += put_dec(b + n, port); b[n++] = '\n';
    if (sd_write(RELAY_FILE, b, n) == 1) { card_ip = ip; card_port = port; card_loaded = 1; }
}

static void put_ip(unsigned ip) {
    for (int s = 24; s >= 0; s -= 8) { kdec((ip >> s) & 255); if (s) kputs("."); }
}

/* "JT-RELAY? <8 hex>" padded to 32 bytes; the answer "JT-RELAY <port> <same 8 hex>" is never longer than the probe. */
static int relay_discover(unsigned *ip, unsigned short *port) {
    static unsigned salt;
    char probe[32], nonce[8], rep[48];
    unsigned v = ticks() ^ (++salt * 2654435761u) ^ (unsigned)net_get_ip();
    for (int i = 0; i < 8; i++) nonce[i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15];
    unsigned n = 0;
    for (const char *p = "JT-RELAY? "; *p; p++) probe[n++] = *p;
    for (int i = 0; i < 8; i++) probe[n++] = nonce[i];
    while (n < sizeof probe) probe[n++] = ' ';
    for (int attempt = 0; attempt < 2; attempt++) {
        unsigned from = 0;
        int got = udp_broadcast_ask(CLAUDE_PORT, RELAY_PROBE_SRC, probe, n, rep, sizeof rep, &from, RELAY_PROBE_TICKS);
        if (got < 0) continue;
        unsigned i = 0, p = 0;
        for (const char *m = "JT-RELAY "; *m; m++, i++) if (i >= (unsigned)got || rep[i] != *m) goto bad;
        while (i < (unsigned)got && rep[i] >= '0' && rep[i] <= '9' && p <= 65535) p = p * 10 + (unsigned)(rep[i++] - '0');
        if (p < 1 || p > 65535 || i >= (unsigned)got || rep[i++] != ' ' || (unsigned)got - i < 8) goto bad;
        for (int k = 0; k < 8; k++) if (rep[i + k] != nonce[k]) goto bad;
        unsigned mask = net_get_netmask() ? net_get_netmask() : 0xFFFFFF00u, me = net_get_ip();
        if (((from ^ me) & mask) || from == me || !(from & ~mask) || (from & ~mask) == ~mask) goto bad;   /* a LAN host, not us or a broadcast */
        *ip = from; *port = (unsigned short)p;
        return 1;
    bad:;
    }
    return 0;
}

/* A verified TLS connection to the relay, ready for the request: 0, or -1 with tls_last_error set. `wait` is the SYN-ACK wait. */
static int relay_try(unsigned ip, unsigned short port, unsigned wait) {
    unsigned saved = net_connect_wait_ticks, t0 = ticks();
    net_connect_wait_ticks = wait;
    int r = tls_connect_private(ip, port, RELAY_NAME) < 0 || tls_handshake() < 0 ? -1 : 0;
    net_connect_wait_ticks = saved;
    t_tls += ticks() - t0;
    if (!r) { relay_ip = ip; relay_port = port; card_save(ip, port); }
    return r;
}
static int relay_open(void) {
    unsigned f0 = ticks(), tls0 = t_tls, ret;
    unsigned ip = relay_ip, wait = RELAY_CACHED_TICKS; unsigned short port = relay_port;
    if (!ip && !card_loaded) card_load();
    if (!ip && card_ip) { ip = card_ip; port = card_port; }      /* the card's last good relay */
    if (!ip) {                                                    /* never found one: the baked address, at the full wait */
        port = CLAUDE_PORT; wait = RELAY_CONNECT_TICKS;
        if (CLAUDE_HOST[0] && !http_resolve_host(CLAUDE_HOST, &ip)) ip = 0;
    }
    if (ip && relay_try(ip, port, wait) == 0) { ret = 0; goto done; }
    relay_ip = 0; ret = (unsigned)-1;
    if (ip && tls_last_error() == TLS_ERR_ENTROPY) goto done;     /* no randomness: another address would not help */
    unsigned found; unsigned short fport;
    if (!relay_discover(&found, &fport)) { kputs("samantha: no relay answered on the LAN\n"); goto done; }
    if ((found == ip && fport == port) || relay_try(found, fport, RELAY_CONNECT_TICKS) < 0) goto done;
    kputs("samantha: relay found at "); put_ip(found); kputs("\n");
    ret = 0;
done:
    t_find += (ticks() - f0) - (t_tls - tls0);                    /* everything but the TLS tries is finding */
    return (int)ret;
}

static int relay_post_tls(const char *body, unsigned body_len, const char *token, char *reply, int *status) {
    static char req[2 * RESULT_MAX + 512 + sizeof(RELAY_NAME)];
    char digits[10]; unsigned n = 0, dn = 0, v = body_len;
    *status = 0;
    if (relay_open() < 0) return -1;          /* nothing sent: the token stays home */
    do { digits[dn++] = (char)('0' + v % 10); v /= 10; } while (v);
    const char *parts[] = {"POST /api/claude HTTP/1.0\r\nHost: ", RELAY_NAME, "\r\nAuthorization: Bearer ", token,
                          "\r\nContent-Type: application/json\r\nContent-Length: "};
    for (unsigned i = 0; i < 5; i++) for (const char *p = parts[i]; *p; p++) req[n++] = *p;
    while (dn) req[n++] = digits[--dn];
    for (const char *p = "\r\nConnection: close\r\n\r\n"; *p; p++) req[n++] = *p;
    for (unsigned i = 0; i < body_len; i++) req[n++] = body[i];
    char *raw = kmalloc(REPLY_MAX + 2048);
    request_sent = 1;
    int got = raw ? tls_exchange_timeout(req, n, raw, REPLY_MAX + 2048, ASK_TICKS) : -1;
    if (!raw) tcp_close();
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

static void no_answer(void) {   /* "sam: no answer in 30s (relay at a.b.c.d)": the prompt comes back, the busy flag clears in sam_run */
    kputs("sam: no answer in "); kdec(ASK_SECS); kputs("s (relay at ");
#if defined(PI_BUILD) || CLAUDE_TLS
    if (relay_ip) put_ip(relay_ip); else
#endif
    kputs(CLAUDE_HOST);
    kputs(")\n");
}

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
    for (const char *p = ac->session; *p; p++) body[b++] = *p;
    if (ac->model_i) { for (const char *p = "\",\"model\":\""; *p; p++) body[b++] = *p; for (const char *p = models[ac->model_i]; *p; p++) body[b++] = *p; }
    if (ac->effort_i != 1) { for (const char *p = "\",\"effort\":\""; *p; p++) body[b++] = *p; for (const char *p = efforts[ac->effort_i]; *p; p++) body[b++] = *p; }
    for (const char *p = "\",\"pi\":\""; *p; p++) body[b++] = *p;
    b += pi_status(body + b);                  /* the Pi's live status, so the answer can be about this machine */
    /* The relay puts this field in Samantha's system text (never changes the question), 120 characters at most. It is the one
       place the Pi can ask for a short answer without touching relay.py, and the persona lines stay the relay's own. */
    for (const char *p = ". Reply in one or two sentences unless asked for detail."; *p; p++) body[b++] = *p;
    body[b++] = '"'; body[b++] = '}';
    kputs("samantha: thinking\n");
    timed = 1; request_sent = 0;
    unsigned p0 = ticks(), find0 = t_find, tls0 = t_tls;
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
    t_wait += (ticks() - p0) - (t_find - find0) - (t_tls - tls0);   /* the rest of this call: send, the relay thinking, the reply */
    ac->steps_total++;
    if (got < 0) {
        ac->relay_state = 2; ac->last_name[0] = 0;     /* out of reach: the prompt goes back to the default model */
        int e = net_last_error();
#if defined(PI_BUILD) || CLAUDE_TLS
        if (request_sent && (tls_last_error() == TLS_ERR_TIMEOUT || e == NET_ERR_REPLY_TIMEOUT)) { no_answer(); return -2; }
        if (tls_last_error() == TLS_ERR_ENTROPY) { kputs("samantha: hardware randomness unavailable\n"); return -1; }
        kputs("samantha: TLS failed, no HTTP fallback\n");
#else
        if (e == NET_ERR_REPLY_TIMEOUT) { no_answer(); return -2; }
#endif
        if (e == NET_ERR_CONNECT_TIMEOUT || e == NET_ERR_REPLY_TIMEOUT) kputs("samantha: timeout\n");
        else { kputs("samantha: error "); kputs(e == NET_ERR_NONE ? "busy" : net_error_name(e)); kputs("\n"); }
        return -1;
    }
    reply[got] = 0;
    ac->relay_state = status == 200 ? 1 : 2;
    if (status != 200) {                       /* the relay's own one-line reason follows its code */
        if (!status) { kputs("samantha: error bad reply\n"); return -2; }
        kputs("samantha: error -"); kdec((unsigned)status); kputs("\n");
        say_wrapped("", reply, (unsigned)got);
        return -2;
    }
    unsigned start = 0;                        /* "S <session uuid>\nM <model>\n<answer>" */
    if (got >= 2 && reply[0] == 'S' && reply[1] == ' ') {
        unsigned e = 2; while (e < (unsigned)got && reply[e] != '\n') e++;
        if (e - 2 >= 36 && is_session(reply + 2, 36)) {
            for (unsigned i = 0; i < 36; i++) ac->session[i] = reply[2 + i];
            ac->session[36] = 0;
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
static const char *said; static unsigned said_n; static unsigned rlen_before; static void (*saved_out)(const char *);
static void tee(const char *s) { real_puts(s); result_puts(s); }   /* a command's line to the Terminal and into the result */   /* the answer whose actions run, for [[save]] */
static void run_actions(void) {
    static const char *const names[] = {"", "note", "led blink", "say", "open", "browse", "calc", "status", "save", "list", "read"};
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
        case ACT_SAVE: {                       /* asked-for files only, into JT/ (emmc.c refuses boot files and non 8.3 names) */
            unsigned at = 0, len = code_block(said, said_n, &at); int ok = 0;
            if (!named_in(sam_q, sam_qn, t)) { kputs("save: "); kputs(t); kputs(" was not in your question, not saved\n"); result_puts("refused, not named by the person\n"); break; }
            if (!len) { kputs("save: no code block to save\n"); result_puts("no code block\n"); break; }
            if (!sd_ready()) { kputs("save: no SD card\n"); result_puts("no SD card\n"); break; }
            ok = sd_write(t, said + at, len) == 1;
            kputs(ok ? "save: wrote JT/" : "save: could not write "); kputs(t); kputs(ok ? ", cat JT/" : "\n"); if (ok) { kputs(t); kputs(" to read it\n"); }
            result_puts(ok ? "saved\n" : "not saved\n");
            break; }
        case ACT_LIST: {                       /* what is in JT/: the same lines `ls JT` prints, sent back to her too */
            rlen_before = rlen; saved_out = cmd_out;
            cmd_run("ls JT", 5, tee);
            cmd_out = saved_out;           /* cmd_run left its own printer set; the next action prints through the old one */
            if (rlen == rlen_before) result_puts("nothing in JT/\n");
            break; }
        case ACT_READ: {                       /* JT/NAME as text, through cat; an 8.3 name only, so no path can be built */
            char cmd[ACT_LEN + 8]; unsigned o = 0, okname = k > 0 && k <= 12;
            for (unsigned i = 0; i < k; i++) okname = okname && ((t[i] >= 'a' && t[i] <= 'z') || (t[i] >= 'A' && t[i] <= 'Z') || (t[i] >= '0' && t[i] <= '9') || t[i] == '_' || t[i] == '-' || t[i] == '.');
            if (!okname) { kputs("read: not a file name\n"); result_puts("not a file name\n"); break; }
            for (const char *p = "cat JT/"; *p; p++) cmd[o++] = *p;
            for (unsigned i = 0; i < k; i++) cmd[o++] = t[i];
            rlen_before = rlen; saved_out = cmd_out;
            cmd_run(cmd, o, real_puts);   /* cat prints through say_wrapped, which the result buffer already sees */
            cmd_out = saved_out;
            if (rlen == rlen_before) result_puts("no such file\n");
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
    if (is_stop(q, n)) {
        if (sam_state) { stop_flag = 1; kputs("sam: stopping after this step\n"); }
        else kputs("agent: nothing running\n");
        return 1;
    }
    return slash(q, n);
}
void ask_claude(const char *q, unsigned n) {   /* cmd.c: `llm PROMPT` */
    if (n > 4 && q[0] == 'l' && q[1] == 'l' && q[2] == 'm' && q[3] == ' ') local(q + 4, n - 4);
}
int ask_busy(void) { return sam_state != 0; }   /* cmd.c: `sam` is waiting on the relay, so the network and the heap are taken */
/* Plain arithmetic is answered on the Pi at once and never sent: digits, spaces, . + - * / ^ and parentheses, with an
   optional "what is" in front, quotes around it and a ? or = behind. The evaluator is the Calculator's (calc.c), so there
   is one parser. A sum it cannot do (1/0, an unbalanced bracket) goes to the relay like any question. */
static int has_word(const char *q, unsigned n, const char *w) { unsigned i = 0; for (; w[i]; i++) if (i >= n || q[i] != w[i]) return 0; return 1; }
static int local_math(const char *q, unsigned n) {
    static const char *const lead[] = {"what is ", "what's ", "whats ", "calculate ", "calc "};
    char e[ASK_MAX + 1]; unsigned m = 0, digit = 0, op = 0;
    while (n && (q[0] == ' ' || q[0] == '"' || q[0] == '\'')) { q++; n--; }
    while (n && (q[n - 1] == ' ' || q[n - 1] == '"' || q[n - 1] == '\'' || q[n - 1] == '?' || q[n - 1] == '=')) n--;
    for (unsigned k = 0; k < sizeof lead / sizeof *lead; k++) {
        unsigned w = 0; while (lead[k][w]) w++;
        if (has_word(q, n, lead[k])) { q += w; n -= w; break; }
    }
    for (unsigned i = 0; i < n; i++) {
        char c = q[i];
        if (c >= '0' && c <= '9') digit = 1;
        else if (c == '+' || c == '-' || c == '*' || c == '/' || c == '^') op = 1;
        else if (c != ' ' && c != '.' && c != '(' && c != ')') return 0;
        e[m++] = c;
    }
    if (!digit || !op) return 0;
    e[m] = 0;
    char out[32];
    if (calc_eval_str(e, 0, out)) return 0;
    kputs("sam: "); kputs(out); kputs("\n");
    return 1;
}
/* ---- The chat view. Lines are kept as plain text, already wrapped; the window shows the newest ones above the input row. */
#define TUI_LINES 160
#define TUI_W 64
static char tui_h[TUI_LINES][TUI_W];
static char tui_cur[TUI_W]; static unsigned tui_cn;           /* the line her output is still writing */
static char tui_head_last[TUI_W + 40];
static unsigned tui_width(void) { int c = con_columns(); return c >= 20 && c < ASK_WIDTH ? (unsigned)c : ASK_WIDTH; }
static void tlog(const char *a, const char *b) {              /* "tui| a b" to the UART only */
    char o[TUI_W + 96]; unsigned n = 0;
    for (const char *p = "tui| "; *p; p++) o[n++] = *p;
    for (; *a && n < sizeof o - 2; a++) o[n++] = *a;
    for (; *b && n < sizeof o - 2; b++) o[n++] = *b;
    o[n++] = '\n'; o[n] = 0;
    tui_log(o);
}
static unsigned tui_relay(void) {   /* the relay the header names: the one in memory, else the card's, else none (local only) */
#if defined(PI_BUILD) || CLAUDE_TLS
    if (!relay_ip && !card_loaded) card_load();
    return relay_ip ? relay_ip : card_ip;
#else
    return 0;
#endif
}
static unsigned tui_body(void) { unsigned r = (unsigned)con_rows_total(); return r > 2 ? r - 2 : r ? 1 : 20; }   /* history rows: the window less the header and its rule */
static unsigned tui_total(void) { return tui_n + (sam_state ? 1 : 0); }                                           /* the thinking marker is a row too */
static void tui_paint(int full) {
    static char head[TUI_W + 8], rule[TUI_W + 8], think[TUI_W + 8];
    static const char *rows[96];
    unsigned R = (unsigned)con_rows_total(), w = tui_width(), body = tui_body(), total = tui_total(), ip = tui_relay();
    char right[24]; unsigned rn = 0;
    if (ip) {
        for (const char *p = "relay "; *p; p++) right[rn++] = *p;
        for (int sh = 24; sh >= 0; sh -= 8) { rn += put_dec(right + rn, (ip >> sh) & 255); if (sh) right[rn++] = '.'; }
    } else for (const char *p = "local only"; *p; p++) right[rn++] = *p;
    right[rn] = 0;
    for (unsigned i = 0; i < w; i++) { head[i] = ' '; rule[i] = '-'; }
    head[w] = rule[w] = 0;
    { const char *t = "Samantha"; for (unsigned i = 0; t[i]; i++) head[i] = t[i]; }
    for (unsigned i = 0; i < rn && rn < w; i++) head[w - rn + i] = right[i];
    { const char *t = tui_quit ? "thinking (leaving when she answers)" : "thinking...";
      unsigned i = 0; for (; t[i]; i++) think[i] = t[i]; think[i] = 0; }
    unsigned max_up = total > body ? total - body : 0;
    if (tui_up > max_up) tui_up = max_up;
    unsigned end = total - tui_up, start = end > body ? end - body : 0;
    rows[0] = head; rows[1] = rule;
    for (unsigned r = 2; r < R && r < 96; r++) rows[r] = 0;
    for (unsigned i = start; i < end && 2 + (i - start) < 96; i++) rows[2 + (i - start)] = i < tui_n ? tui_h[i] : think;
    if (full) con_tui_begin();
    con_tui_rows(rows, R < 96 ? R : 96);
    { unsigned same = 1, i = 0; for (; head[i] || tui_head_last[i]; i++) if (head[i] != tui_head_last[i]) { same = 0; break; }
      if (!same || full) { for (i = 0; head[i]; i++) tui_head_last[i] = head[i]; tui_head_last[i] = 0; tlog("head ", head); } }
    { char v[40]; unsigned n = 0;
      n += put_dec(v + n, end > start ? start + 1 : 0); v[n++] = '-'; n += put_dec(v + n, end); v[n++] = '/'; n += put_dec(v + n, total);
      v[n++] = ' '; v[n++] = 'u'; v[n++] = 'p'; v[n++] = ' '; n += put_dec(v + n, tui_up); v[n] = 0; tlog("view ", v); }
    if (sam_state) tlog("marker ", think);
    prompt_row(ac->line, ac->len);
}
static void prompt_row(const char *s, unsigned n) {
    if (!tui_on) { con_prompt(s, n); return; }
    char line[ASK_MAX + 8]; unsigned k = 0;
    for (; k < n; k++) line[k] = s[k];
    line[k] = 0;
    con_tui_input("you: ", s, n);
    tlog("input you: ", line);
}
static void tui_commit(const char *l) {
    if (tui_n == TUI_LINES) { for (unsigned i = 1; i < TUI_LINES; i++) for (unsigned k = 0; k < TUI_W; k++) tui_h[i - 1][k] = tui_h[i][k]; tui_n--; }
    unsigned k = 0; for (; l[k] && k < TUI_W - 1; k++) tui_h[tui_n][k] = l[k];
    tui_h[tui_n++][k] = 0;
    tlog("line ", l);
}
static int tui_chatter(const char *l) {   /* progress lines that belong to the UART log, not the conversation */
    unsigned n = 0; while (l[n]) n++;
    if (starts(l, n, "agent: step ") || starts(l, n, "samantha: thinking") || starts(l, n, "samantha: relay found")) return 1;
    if (!starts(l, n, "sam: ")) return 0;                         /* "sam: 3.2s (find 0.1, tls 0.2, wait 2.9)" */
    unsigned i = 5; while (i < n && l[i] >= '0' && l[i] <= '9') i++;
    return i > 5 && i + 4 < n && l[i] == '.' && l[i + 2] == 's' && l[i + 3] == ' ' && l[i + 4] == '(';
}
static void tui_puts(const char *s) {   /* cmd_out while the chat view is up: whole lines into the history */
    int dirty = 0;
    if (!tui_on) { real_puts(s); return; }
    for (; *s; s++) {
        if (*s == '\n') {
            tui_cur[tui_cn] = 0;
            if (tui_chatter(tui_cur)) { real_puts(tui_cur); real_puts("\n"); } else { tui_commit(tui_cur); dirty = 1; }
            tui_cn = 0;
        } else if (tui_cn < TUI_W - 1) tui_cur[tui_cn++] = *s >= 32 && *s < 127 ? *s : '?';
    }
    if (dirty) { tui_up = 0; tui_paint(0); }
}
static void tui_enter(void) {
    if (sam_state) { kputs("sam: still thinking\n"); return; }
    tui_on = 1; tui_quit = 0; tui_up = 0; tui_cn = 0; ac->len = 0;
    tui_head_last[0] = 0;
    tui_relay();                                  /* reads the card's hint now, from the main loop */
    tui_log("tui| open\n");
    if (!tui_n) tui_commit("Type a question and press Enter. Esc leaves.");
    tui_paint(1);
}
static void tui_leave(void) {
    tui_on = 0; tui_quit = 0; ac->len = 0;
    tui_log("tui| closed\n");
    con_tui_end();                                 /* the Terminal's text and the shell prompt */
}
static void tui_escape(void) {
    if (sam_state == 2) { stop_flag = 1; tui_quit = 1; tui_paint(0); }   /* in flight: leave once she has answered */
    else { sam_state = 0; tui_leave(); }                                 /* idle, or only queued: nothing to wait for */
}
static void tui_move(int up) { if (up) tui_up++; else if (tui_up) tui_up--; tui_paint(0); }
void ask_tui_scroll(unsigned code) {   /* main.c's Page Up 104, Page Down 109, Home 102, End 107 */
    unsigned b = tui_body(), half = b / 2 ? b / 2 : 1, total = tui_total(), max_up = total > b ? total - b : 0;
    if (code == 104) tui_up += half;
    else if (code == 109) tui_up = tui_up > half ? tui_up - half : 0;
    else if (code == 102) tui_up = max_up;
    else tui_up = 0;
    tui_paint(0);
}
int ask_tui(void) { return tui_on; }
static void tui_submit(void) {   /* Enter in the chat view: your line, then the same sam path as `sam QUESTION` */
    char q[ASK_MAX + 1]; unsigned n = ac->len;
    for (unsigned i = 0; i < n; i++) q[i] = ac->line[i];
    ac->len = 0;
    unsigned a = 0, b = n; while (a < b && q[a] == ' ') a++; while (b > a && q[b - 1] == ' ') b--;
    if (a == b) { prompt_row(ac->line, ac->len); return; }
    if (word_is(q + a, b - a, "/quit")) {
        if (sam_state == 2) { stop_flag = 1; tui_quit = 1; tui_paint(0); } else { sam_state = 0; tui_leave(); }
        return;
    }
    void (*saved)(const char *) = cmd_out;
    cmd_out = tui_puts;
    tui_cn = 0; tui_up = 0;
    say_wrapped("you: ", q + a, b - a);
    if (!ask_command(q + a, b - a)) sam_command(q + a, b - a);   /* "stop", the slash commands, else a question */
    cmd_out = saved;
    tui_paint(0);
}
void sam_command(const char *q, unsigned n) {   /* cmd.c: `sam QUESTION` queues the question and returns; ask_poll sends it */
    while (n && q[0] == ' ') { q++; n--; }
    if (!n) { if (!tui_on) tui_enter(); return; }   /* `sam` alone opens the chat view */
    if (sam_state) { kputs("sam: still thinking\n"); return; }
    if (local_math(q, n)) return;              /* 20 for 10+10, on the Pi, no relay and no busy flag */
    if (n > ASK_MAX) n = ASK_MAX;
    for (unsigned i = 0; i < n; i++) sam_q[i] = q[i];
    sam_qn = n; sam_state = 1;
}
static void ask_relay_steps(const char *q, unsigned n) {   /* the whole exchange for one question, run from ask_relay */
    static char reply[REPLY_MAX + 1];
    if (!CLAUDE_TOKEN_LEN) { kputs("samantha: no token\n"); fallback(q, n); return; }
    if (!net_get_gateway()) {                  /* no DHCP lease, or no card at all */
#ifdef PI_BUILD
        kputs("samantha: no network, Wi-Fi has not joined yet\n");
#else
        kputs("samantha: no network\n");
#endif
        ac->last_name[0] = 0;                      /* out of reach: the prompt goes back to the default model */
        fallback(q, n);                        /* the relay is out of reach: the local model answers */
        return;
    }
    /* The agent loop (docs/GUIDE.md): an answer that ends with actions gets them run, and what they printed goes back
       to the relay as the next turn, up to AGENT_STEPS times. Esc or "stop" between steps ends it. */
    stop_flag = 0;
    const char *prompt = q; unsigned plen = n;
    for (unsigned step = 1; step <= AGENT_STEPS; step++) {
        char *ans;
        kputs("agent: step "); kdec(step); kputs("\n");
        int got = post(prompt, plen, reply, &ans);
        if (got == -1 && step == 1) fallback(q, n);   /* the relay never answered the question: the local model does */
        if (got < 0) return;
        said_n = take_actions(ans, (unsigned)got); said = ans;
        say_wrapped("sam: ", ans, said_n);   /* the answer first, then what she asked the Pi to do */
        if (!act_n) return;
        run_actions();
        if (step == AGENT_STEPS) { kputs("agent: step limit\n"); return; }
        input_drain();
        if (stop_flag) { kputs("agent: stopped\n"); return; }
        prompt = result; plen = rlen;
    }
}

static void ask_relay(const char *q, unsigned n) {   /* one question, then the line that says where the time went */
    t_find = t_tls = t_wait = timed = 0; t_mark = ticks();
    ask_relay_steps(q, n);
    timing_line();
}

/* Called from the main loop: runs a line Enter queued, so the keyboard handler never blocks. Also called from ask_service
   while a `sam` request waits on the network, which is why term_output nests and nothing here may touch the network. */
static unsigned poll_depth;
static void run_typed(void) {
    poll_depth++; term_output(1);
    say_wrapped(ask_prompt(), ac->question, ac->qlen);
    void (*saved)(const char *) = cmd_out;
    cmd_run(ac->question, ac->qlen, 0);                /* cmd.c: the one command line (help, /model, browse, llm, sam), into the Terminal */
    cmd_out = saved;
    poll_depth--; term_output(poll_depth ? 1 : 0);
    ac->pending = 0;
    prompt_row(ac->line, ac->len);
}
static void sam_run(void) {   /* the queued question, on a main loop pass of its own */
    poll_depth++; if (!tui_on) term_output(1);   /* in the chat view her lines go to its history, not the Terminal's log */
    cmd_out = tui_on ? tui_puts : real_puts;
    sam_state = 2; stop_flag = 0;
    net_idle = ask_service;
    ask_relay(sam_q, sam_qn);
    net_idle = 0;
    sam_state = 0;
    poll_depth--; term_output(poll_depth ? 1 : 0);
    cmd_out = real_puts;                          /* the shell's next echo must not land in a chat view that is gone */
    if (tui_on && tui_quit) tui_leave();          /* Esc or /quit came while she was thinking */
    else if (tui_on) tui_paint(0);                /* the thinking marker goes */
    prompt_row(ac->line, ac->len);
}
void ask_poll(void) {
    if (ac->pending) run_typed();
    else if (sam_state == 1) sam_run();
}
/* net_idle, while `sam` waits: the keys that arrived, and a line Enter completed runs now. Not while an action is
   capturing its output for the next turn (that output must be Samantha's alone): the line waits for the next pass. */
static void ask_service(void) {
    input_drain();
    if (ac->pending && !capturing) run_typed();
}
