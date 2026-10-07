/* Claude in the Console. The bottom row of the Console window is a one-line editor, `ask> `: typed keys land there,
   Backspace takes one back, Enter sends the line to the Claude relay on the Mac (tools/claude-relay/relay.py) as one
   HTTP POST through the shared IP stack, and the answer is printed into the Console, wrapped to 53 columns (the house
   rule, so a photo of the Pi screen and the UART log read the same). No app, no ring 3: the kernel asks.

   Where the relay is and the token it wants come from claude_cfg.h, generated at build time by claude_cfg.sh from the
   environment and a file outside the repo; the token is sent only in the Authorization header of this one request and
   is never printed. Every outcome is one short honest line: `claude: thinking`, `claude: no token`, `claude: no
   network` (no DHCP lease: on a Pi that means Wi-Fi has not joined yet), `claude: error -401` for a relay refusal,
   `claude: timeout`. The relay keeps a session per conversation, so a follow-up question knows what came before. */
#include "../../drivers/net.h"
#include "../../drivers/http.h"
#include "claude_cfg.h"

void kputs(const char *s);                     /* main.c */
void kdec(unsigned v);
int con_columns(void);
void con_prompt(const char *s, unsigned n);
unsigned long heap_mark(void);
void heap_release(unsigned long m);
void http_post_set_bearer(const char *token);

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

static int is_session(const char *s, unsigned n) {
    if (n != 36) return 0;
    for (unsigned i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == '-')) return 0;
    }
    return 1;
}

static void ask(const char *q, unsigned n) {
    static char body[2 * ASK_MAX + 96], reply[REPLY_MAX + 1];
    say_wrapped("ask> ", q, n);
    if (!CLAUDE_TOKEN_LEN) { kputs("claude: no token\n"); return; }
    if (!net_get_gateway()) {                  /* no DHCP lease, or no card at all */
#ifdef PI_BUILD
        kputs("claude: no network, Wi-Fi has not joined yet\n");
#else
        kputs("claude: no network\n");
#endif
        return;
    }
    unsigned b = 0;                            /* {"prompt":"...","session":"..."}: only " and \ need escaping in ASCII */
    for (const char *p = "{\"prompt\":\""; *p; p++) body[b++] = *p;
    for (unsigned i = 0; i < n; i++) { if (q[i] == '"' || q[i] == '\\') body[b++] = '\\'; body[b++] = q[i]; }
    for (const char *p = "\",\"session\":\""; *p; p++) body[b++] = *p;
    for (const char *p = session; *p; p++) body[b++] = *p;
    body[b++] = '"'; body[b++] = '}';
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
    say_wrapped("", reply + start, (unsigned)got - start);
}

/* Called from the main loop: runs a question Enter queued, so the keyboard handler never blocks on the network. */
void ask_poll(void) {
    if (!pending) return;
    ask(question, qlen);
    pending = 0;
    con_prompt(line, len);
}
