/* The one command line. cmd_run takes a typed line and a sink for its output; everything the Console's ask> row
   understands (the browser, the local model, Claude, help) is dispatched here and nowhere else, so a Terminal app
   can run the same commands later by calling cmd_run with its own sink. browser.c and ask.c print through cmd_out. */
void kputs(const char *s);   /* main.c: the UART and the Console log */
void say_wrapped(const char *prefix, const char *s, unsigned n);   /* ask.c */
int browse_command(const char *q, unsigned n);                     /* browser.c */
void ask_claude(const char *q, unsigned n);                        /* ask.c: `llm PROMPT` or the relay */

void (*cmd_out)(const char *s) = kputs;
void cmd_dec(unsigned v) { char t[11]; unsigned n = 10; t[n] = 0; do t[--n] = (char)('0' + v % 10); while (v /= 10); cmd_out(t + n); }

static const char help[] = "browse URL (a bare host is https), open N, back, forward, reload, links, more, up, down, "
                           "top, bottom, find WORD, llm PROMPT, help. Anything else asks Claude.";

void cmd_run(const char *q, unsigned n, void (*out)(const char *)) {
    cmd_out = out ? out : kputs;
    while (n && q[0] == ' ') { q++; n--; }
    while (n && q[n - 1] == ' ') n--;
    if (!n) return;
    if (n == 4 && q[0] == 'h' && q[1] == 'e' && q[2] == 'l' && q[3] == 'p') { say_wrapped("", help, sizeof help - 1); return; }
    if (browse_command(q, n)) return;
    ask_claude(q, n);
}
