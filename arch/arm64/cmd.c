/* The one command line. cmd_run takes a typed line and a sink for its output; everything the Terminal's prompt
   understands (help, the /model and /effort commands, the browser, the local model, Claude) is dispatched here and nowhere else.
   Another window can run the same commands by calling cmd_run with its own sink. browser.c and ask.c print through cmd_out.
   ls and cat read the SD card's FAT partition (emmc.c, drivers/fat.c): the root directory, 8.3 names. */
#include "emmc.h"
#include "../../drivers/fat.h"
void kputs(const char *s);   /* main.c: the UART and the log of the window in front */
void say_wrapped(const char *prefix, const char *s, unsigned n);   /* ask.c */
int browse_command(const char *q, unsigned n);                     /* browser.c */
int ask_command(const char *q, unsigned n);                        /* ask.c: "stop" and the /model, /effort, /status, /clear, /help commands */
void ask_claude(const char *q, unsigned n);                        /* ask.c: `llm PROMPT` or the relay */

void (*cmd_out)(const char *s) = kputs;
void cmd_dec(unsigned v) { char t[11]; unsigned n = 10; t[n] = 0; do t[--n] = (char)('0' + v % 10); while (v /= 10); cmd_out(t + n); }

static const char help[] = "browse URL (a bare host is https), open N, back, forward, reload, links, more, up, down, "
                           "top, bottom, find WORD, llm PROMPT, ls, cat NAME, help. Anything else asks Claude.";

static unsigned ls_count;
static void ls_one(const char *name, unsigned size, int is_dir) {   /* one line a file: the 8.3 name, then its size in bytes */
    unsigned k = 0;
    cmd_out(name);
    while (name[k]) k++;
    if (is_dir) cmd_out("/");
    else { while (k++ < 14) cmd_out(" "); cmd_dec(size); }
    cmd_out("\n");
    ls_count++;
}
static void cat_file(const char *q, unsigned n) {   /* the first 4 KB of a file in the root directory, as text */
    static char text[4097];
    char name[64];
    unsigned k = 0;
    while (n && *q == ' ') { q++; n--; }
    for (; k < n && k < sizeof name - 1; k++) name[k] = q[k];
    name[k] = 0;
    if (!sd_ready()) { cmd_out("cat: no SD card\n"); return; }
    int got = fat_read_file(name, text, sizeof text);
    if (got < 0) { cmd_out("cat: no file "); cmd_out(name); cmd_out("\n"); return; }
    unsigned shown = got > 4096 ? 4096 : (unsigned)got, m = 0;
    for (unsigned i = 0; i < shown; i++) if (text[i] != '\r') text[m++] = text[i];   /* DOS line ends print as one */
    if (m) say_wrapped("", text, m);
    if (got > 4096) cmd_out("cat: the first 4096 bytes\n");
}

void cmd_run(const char *q, unsigned n, void (*out)(const char *)) {
    cmd_out = out ? out : kputs;
    while (n && q[0] == ' ') { q++; n--; }
    while (n && q[n - 1] == ' ') n--;
    if (!n) return;
    if (n == 4 && q[0] == 'h' && q[1] == 'e' && q[2] == 'l' && q[3] == 'p') { say_wrapped("", help, sizeof help - 1); return; }
    if (n == 2 && q[0] == 'l' && q[1] == 's') {
        if (!sd_ready()) { cmd_out("ls: no SD card\n"); return; }
        ls_count = 0; fat_list(ls_one);
        if (!ls_count) cmd_out("ls: no files\n");
        return;
    }
    if (n > 4 && q[0] == 'c' && q[1] == 'a' && q[2] == 't' && q[3] == ' ') { cat_file(q + 4, n - 4); return; }
    if (ask_command(q, n)) return;
    if (browse_command(q, n)) return;
    ask_claude(q, n);
}
