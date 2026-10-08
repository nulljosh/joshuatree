/* Claude in the Console. The bottom row of the Console window is a one-line editor, `ask> `: typed keys land there,
   Backspace takes one back, Enter sends the line to the Claude relay on the Mac (tools/claude-relay/relay.py) as one
   HTTP POST through the shared IP stack, and the answer is printed into the Console, wrapped to 53 columns (the house
   rule, so a photo of the Pi screen and the UART log read the same). No app, no ring 3: the kernel asks.

   Where the relay is and the token it wants come from claude_cfg.h, generated at build time by claude_cfg.sh from the
   environment and a file outside the repo; the token is sent only in the Authorization header of this one request and
   is never printed. Every outcome is one short honest line: `claude: thinking`, `claude: no token`, `claude: no
   network` (no DHCP lease: on a Pi that means Wi-Fi has not joined yet), `claude: error -401` for a relay refusal,
   `claude: timeout`. The relay keeps a session per conversation, so a follow-up question knows what came before.
   An answer may end with actions for the Pi ([[note TEXT]], [[led blink]]): see take_actions. */
#include "../../drivers/net.h"
#include "../../drivers/http.h"
#include "claude_cfg.h"

void kputs(const char *s);                     /* main.c */
void kdec(unsigned v);
int con_columns(void);
void con_prompt(const char *s, unsigned n);
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
static int shift, pending, active;
static char session[37];     /* the relay's session uuid, "" before the first answer */

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

int ask_active(void) { return active; }
void ask_redraw(void) { con_prompt(line, len); }   /* main.c: the Console was reopened, put the line being typed back */

/* One key event from any keyboard. 1 when it is the editor's (the caller then keeps its echo line off the screen). */
int ask_key(unsigned code, unsigned value) {
    if (code == KEY_LSHIFT || code == KEY_RSHIFT) { shift = value != 0; return 1; }
    int typed = code < 58 && plain[code];
    if (!typed && code != KEY_BACKSPACE && code != KEY_ENTER && code != KEY_KPENTER) return 0;
    if (!value) return 1;                      /* key up: nothing to do, but it is still ours */
    active = 1;
    if (typed) { if (len < ASK_MAX) line[len++] = shift ? shifted[code] : plain[code]; }
    else if (code == KEY_BACKSPACE) { if (len) len--; }
    else if (len && !pending) {                /* Enter: hand the line to ask_poll, outside the input handler */
        for (unsigned i = 0; i < len; i++) question[i] = line[i];
        qlen = len; len = 0; pending = 1;
    }
    con_prompt(line, len);
    return 1;
}

/* Prints text as lines of at most `width` columns, breaking at a space where it can, and makes it plain ASCII. */
static void say_wrapped(const char *prefix, const char *s, unsigned n) {
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
        out[o++] = '\n'; out[o] = 0; kputs(out); o = 0;
        i = end;
        if (i < n && s[i] == '\n') i++;        /* the newline that ended this line */
        else while (i < n && s[i] == ' ') i++; /* a wrapped line never starts with a space */
    }
    if (o) { out[o++] = '\n'; out[o] = 0; kputs(out); }
}

/* pi-actions begin: pure, no kernel calls, so tools/checks/pi-actions-check.py compiles this block on the host.
   Samantha may end an answer with actions, each alone on its line: [[note TEXT]] or [[led blink]], 80 characters at
   most. take_actions removes those lines from s in place, records them, and returns the new length. Anything else in
   [[ ]] (an unknown action, a long one, one inside a sentence) stays as plain text. */
#define ACT_MAX 4
#define ACT_LEN 80
#define ACT_NOTE 1
#define ACT_LED 2
static int act_kind[ACT_MAX];
static char act_text[ACT_MAX][ACT_LEN + 1];
static unsigned act_n;
static int starts(const char *s, unsigned n, const char *w) {
    unsigned i = 0;
    for (; w[i]; i++) if (i >= n || s[i] != w[i]) return 0;
    return 1;
}
static unsigned take_actions(char *s, unsigned n) {
    unsigned o = 0, i = 0;
    act_n = 0;
    while (i < n) {
        unsigned e = i, z;
        while (e < n && s[e] != '\n') e++;
        z = e;
        while (z > i && (s[z - 1] == ' ' || s[z - 1] == '\r')) z--;
        int kind = 0;
        if (z - i >= 4 && z - i <= ACT_LEN && act_n < ACT_MAX && starts(s + i, z - i, "[[") && s[z - 2] == ']' && s[z - 1] == ']') {
            const char *p = s + i + 2; unsigned m = z - i - 4;
            if (m == 9 && starts(p, m, "led blink")) kind = ACT_LED;
            else if (m > 5 && starts(p, m, "note ")) kind = ACT_NOTE;
            if (kind) {
                unsigned k = 0;
                if (kind == ACT_NOTE) for (; k < m - 5; k++) act_text[act_n][k] = p[5 + k];
                act_text[act_n][k] = 0;
                act_kind[act_n++] = kind;
            }
        }
        if (!kind) { for (unsigned k = i; k < e; k++) s[o++] = s[k]; if (e < n) s[o++] = '\n'; }
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
/* The local model (llm.c): `llm PROMPT` at the ask> row, and Samantha's answer when the relay cannot be reached. */
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

static void ask(const char *q, unsigned n) {
    static char body[2 * ASK_MAX + 192], reply[REPLY_MAX + 1];
    say_wrapped("ask> ", q, n);
    if (n > 4 && q[0] == 'l' && q[1] == 'l' && q[2] == 'm' && q[3] == ' ') { local(q + 4, n - 4); return; }
    if (!CLAUDE_TOKEN_LEN) { kputs("claude: no token\n"); fallback(q, n); return; }
    if (!net_get_gateway()) {                  /* no DHCP lease, or no card at all */
#ifdef PI_BUILD
        kputs("claude: no network, Wi-Fi has not joined yet\n");
#else
        kputs("claude: no network\n");
#endif
        fallback(q, n);                        /* the relay is out of reach: the local model answers */
        return;
    }
    unsigned b = 0;                            /* {"prompt":"...","session":"..."}: only " and \ need escaping in ASCII */
    for (const char *p = "{\"prompt\":\""; *p; p++) body[b++] = *p;
    for (unsigned i = 0; i < n; i++) { if (q[i] == '"' || q[i] == '\\') body[b++] = '\\'; body[b++] = q[i]; }
    for (const char *p = "\",\"session\":\""; *p; p++) body[b++] = *p;
    for (const char *p = session; *p; p++) body[b++] = *p;
    body[b++] = '"';
    {   /* the Pi's live status, so the answer can be about this machine: "pi":"ip 10.0.0.189, utc 1791..., wifi 3/3" */
        char st[80]; unsigned k = 0, ip = net_get_ip(); unsigned long u = net_clock_utc();
        for (const char *p = "ip "; *p; p++) st[k++] = *p;
        for (int sh = 24; sh >= 0; sh -= 8) { k += put_dec(st + k, (ip >> sh) & 255); if (sh) st[k++] = '.'; }
        for (const char *p = ", utc "; *p; p++) st[k++] = *p;
        k += put_dec(st + k, u);
        for (const char *p = ", wifi "; *p; p++) st[k++] = *p;
        st[k++] = (char)('0' + wifi_signal_level()); st[k++] = '/'; st[k++] = '3';
        for (const char *p = ",\"pi\":\""; *p; p++) body[b++] = *p;
        for (unsigned i = 0; i < k; i++) body[b++] = st[i];
        body[b++] = '"';
    }
    body[b++] = '}';
    kputs("claude: thinking\n");
    static char token[64];
    for (int i = 0; i < CLAUDE_TOKEN_LEN && i < 63; i++) token[i] = (char)claude_token[i];
    token[CLAUDE_TOKEN_LEN < 63 ? CLAUDE_TOKEN_LEN : 63] = 0;
    unsigned long mark = heap_mark();          /* http.c borrows two buffers from the bump heap; give them back */
    http_post_set_bearer(token);
    int got = http_post_timeout(CLAUDE_HOST, "/api/claude", CLAUDE_PORT, body, b, reply, REPLY_MAX, ASK_TICKS);
    http_post_set_bearer(0);
    heap_release(mark);
    for (int i = 0; i < 64; i++) token[i] = 0;
    int status = http_last_status();
    if (got < 0) {
        int e = net_last_error();
        if (e == NET_ERR_CONNECT_TIMEOUT || e == NET_ERR_REPLY_TIMEOUT) kputs("claude: timeout\n");
        else { kputs("claude: error "); kputs(e == NET_ERR_NONE ? "busy" : net_error_name(e)); kputs("\n"); }
        fallback(q, n);                        /* the relay never answered: the local model does */
        return;
    }
    reply[got] = 0;
    if (status != 200) {                       /* the relay's own one-line reason follows its code */
        if (!status) { kputs("claude: error bad reply\n"); return; }
        kputs("claude: error -"); kdec((unsigned)status); kputs("\n");
        say_wrapped("", reply, (unsigned)got);
        return;
    }
    unsigned start = 0;                        /* "S <session uuid>\n<answer>" */
    if (got >= 2 && reply[0] == 'S' && reply[1] == ' ') {
        unsigned e = 2; while (e < (unsigned)got && reply[e] != '\n') e++;
        if (is_session(reply + 2, e - 2)) { for (unsigned i = 0; i < 36; i++) session[i] = reply[2 + i]; session[36] = 0; }
        start = e < (unsigned)got ? e + 1 : e;
    }
    say_wrapped("", reply + start, take_actions(reply + start, (unsigned)got - start));
    for (unsigned a = 0; a < act_n; a++) {     /* the answer first, then what she asked the Pi to do */
        if (act_kind[a] == ACT_NOTE) { char *t = act_text[a]; unsigned k = 0; while (t[k]) k++; say_wrapped("pi: ", t, k); }
#ifdef PI_BUILD
        else led_blink(4);
#else
        else kputs("pi: no green light on QEMU\n");
#endif
    }
}

/* Called from the main loop: runs a question Enter queued, so the keyboard handler never blocks on the network. */
void ask_poll(void) {
    if (!pending) return;
    ask(question, qlen);
    pending = 0;
    con_prompt(line, len);
}
