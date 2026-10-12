/* The one command line. cmd_run takes a typed line and a sink for its output; everything the Terminal's prompt
   understands (help, the /model and /effort commands, the browser, the local model, Claude) is dispatched here and nowhere else.
   Another window can run the same commands by calling cmd_run with its own sink. browser.c and ask.c print through cmd_out.
   The prompt is zsh style (`joshua@tree % `): a word that is no command prints `zsh: command not found: WORD`, and Samantha
   is the command `sam QUESTION`, which returns at once and prints her answer when it lands (ask.c).
   ls and cat read the SD card's FAT partition (emmc.c, drivers/fat.c), 8.3 names, the root or one folder down (ls JT,
   cat JT/NOTE.TXT). write, rm and mkdir change only the JT folder, through emmc.c's sd_write, sd_remove and sd_mkdir. */
#include "bt.h"
#include "emmc.h"
#include "../../drivers/fat.h"
void kputs(const char *s);   /* main.c: the UART and the log of the window in front */
void say_wrapped(const char *prefix, const char *s, unsigned n);   /* ask.c */
int browse_command(const char *q, unsigned n);                     /* browser.c */
int nimble_command(const char *q, unsigned n);                     /* nimble.c: `nimble QUESTION` */
int ask_command(const char *q, unsigned n);                        /* ask.c: "stop" and the /model, /effort, /status, /clear, /help commands */
void ask_claude(const char *q, unsigned n);                        /* ask.c: `llm PROMPT` */
void sam_command(const char *q, unsigned n);                       /* ask.c: `sam QUESTION`, queued and answered without blocking the prompt */
int ask_busy(void);                                                /* ask.c: a `sam` request is waiting on the relay */
int userrun(const char *name, unsigned n, unsigned long *status, int *faulted);   /* main.c: load an app from the table and run it at EL0 */

int audio_tone(int freq_hz, int ms);                               /* audio.c: the 3.5 mm jack, polled PWM */
int audio_volume(void);                                            /* audio.c: the master volume, 0 to 100 */
void audio_set_volume(int percent);
int audio_mute(void);
void menubar_notify(const char *message);                          /* main.c: the menu bar's short notice */

#define VOL_STEP 10          /* `vol up`, `vol down` and the volume keys move this much */
#define TICK_HZ 1000         /* the click a volume key plays so the new level can be heard: calibrate on the real speaker */
#define TICK_MS 40

void (*cmd_out)(const char *s) = kputs;
void cmd_dec(unsigned v) { char t[11]; unsigned n = 10; t[n] = 0; do t[--n] = (char)('0' + v % 10); while (v /= 10); cmd_out(t + n); }

void power_run(int off);                                   /* main.c: the confirmed power action, refused while Samantha answers */
static const char help[] = "browse URL (a bare host is https), open N, back, forward, reload, links, more, up, down, "
                           "top, bottom, find WORD, nimble QUESTION (a web answer with sources), beep [FREQ MS], vol [0-100|up|down], llm PROMPT, bt, bt scan, bt pair, userrun NAME, "
                           "ls [DIR], cat NAME, write NAME.EXT TEXT, rm NAME, mkdir NAME (the last three in JT/), sam QUESTION (ask Samantha), reboot, shutdown, help.";
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
static unsigned word(const char *q, unsigned n, char *out, unsigned size) {   /* the first word of q into out; returns what it used */
    unsigned k = 0, used = 0;
    while (used < n && q[used] == ' ') used++;
    while (used < n && q[used] != ' ') { if (k < size - 1) out[k++] = q[used]; used++; }
    out[k] = 0;
    return used;
}
static char *down(char *path) {   /* DIR/NAME: into DIR, then NAME; a bare NAME stays in the root. 0 when DIR is not there */
    for (char *p = path; *p; p++)
        if (*p == '/') { *p = 0; int in = fat_chdir(path); *p = '/'; return in ? p + 1 : 0; }
    return path;
}
static void cat_file(const char *q, unsigned n) {   /* the first 4 KB of a file, as text */
    static char text[4097];
    char path[64];
    word(q, n, path, sizeof path);
    if (!sd_ready()) { cmd_out("cat: no SD card\n"); return; }
    char *name = down(path);
    int got = name ? fat_read_file(name, text, sizeof text) : -1;
    if (name && name != path) fat_chdir("..");   /* back to the root */
    if (got < 0) { cmd_out("cat: no file "); cmd_out(path); cmd_out("\n"); return; }
    unsigned shown = got > 4096 ? 4096 : (unsigned)got, m = 0;
    for (unsigned i = 0; i < shown; i++) if (text[i] != '\r') text[m++] = text[i];   /* DOS line ends print as one */
    if (m) say_wrapped("", text, m);
    if (got > 4096) cmd_out("cat: the first 4096 bytes\n");
}
static int num(const char *s, unsigned n, int *v) {   /* a plain decimal of 1 to 6 digits */
    if (!n || n > 6) return 0;
    *v = 0;
    for (unsigned i = 0; i < n; i++) { if (s[i] < '0' || s[i] > '9') return 0; *v = *v * 10 + (s[i] - '0'); }
    return 1;
}
static void beep(int freq_hz, int ms) { audio_tone(freq_hz, ms); }   /* the one place cmd.c plays a sound */
static int beep_command(const char *q, unsigned n) {   /* `beep` is 440 Hz for 200 ms; `beep FREQ MS` picks both */
    int f = 440, ms = 200;
    if (n < 4 || q[0] != 'b' || q[1] != 'e' || q[2] != 'e' || q[3] != 'p' || (n > 4 && q[4] != ' ')) return 0;
    if (n > 4) {
        const char *a = q + 5; unsigned an = n - 5, i = 0;
        while (i < an && a[i] != ' ') i++;
        if (i == an || !num(a, i, &f) || !num(a + i + 1, an - i - 1, &ms)) { cmd_out("beep: use beep, or beep FREQ MS\n"); return 1; }
    }
    beep(f, ms);
    return 1;
}
static void vol_text(char *m) {   /* "Volume 70" */
    const char *w = "Volume "; int v = audio_volume(), i = 0;
    while (*w) m[i++] = *w++;
    if (v >= 100) m[i++] = '1';
    if (v >= 10) m[i++] = (char)('0' + v / 10 % 10);
    m[i++] = (char)('0' + v % 10); m[i] = 0;
}
static int vol_command(const char *q, unsigned n) {   /* `vol` shows the level; `vol N` (0 to 100), `vol up`, `vol down` set it */
    int v;
    if (n < 3 || q[0] != 'v' || q[1] != 'o' || q[2] != 'l' || (n > 3 && q[3] != ' ')) return 0;
    if (n == 3) { cmd_out("volume "); cmd_dec((unsigned)audio_volume()); cmd_out("\n"); return 1; }
    if (n == 6 && q[4] == 'u' && q[5] == 'p') audio_set_volume(audio_volume() + VOL_STEP);
    else if (n == 8 && q[4] == 'd' && q[5] == 'o' && q[6] == 'w' && q[7] == 'n') audio_set_volume(audio_volume() - VOL_STEP);
    else if (num(q + 4, n - 4, &v) && v <= 100) audio_set_volume(v);
    else { cmd_out("vol: use vol, vol 0 to 100, vol up or vol down\n"); return 1; }
    cmd_out("volume "); cmd_dec((unsigned)audio_volume()); cmd_out("\n");
    return 1;
}
/* A volume key from main.c's ui_key, before any pane: F11 or Volume Down steps down, F12 or Volume Up steps up, Mute toggles
   (Linux key codes 87, 88, 114, 115, 113; xhci.c's hid_media turns the Mac media keys into the last three). Shows "Volume N"
   in the menu bar and plays the tick. Returns 1 if the key was one of these. */
int volume_key(unsigned code) {
    char m[16];
    if (code == 87 || code == 114) audio_set_volume(audio_volume() - VOL_STEP);
    else if (code == 88 || code == 115) audio_set_volume(audio_volume() + VOL_STEP);
    else if (code == 113) audio_mute();
    else return 0;
    vol_text(m); menubar_notify(m);
    beep(TICK_HZ, TICK_MS);
    return 1;
}

/* userrun NAME: load the user program NAME (hello, calendar, notes, music, loop) from the image table and run it at EL0, its first writes
   on the screen, then say how it ended, or that it is still running: an app that yields (one with a window does) carries on
   from the desktop's loop (M3c part 2), and its red dot or its own Esc ends it. One app at a time. */
static void userrun_command(const char *name, unsigned n) {
    unsigned long status = 0; int faulted = 0;
    int r = userrun(name, n, &status, &faulted);
    if (r == -1) { cmd_out("userrun: no program called that, try hello, calendar, notes, music or loop\n"); return; }
    if (r == -3) { cmd_out("userrun: an app is already running, close it first\n"); return; }
    cmd_out("userrun ");
    for (unsigned i = 0; i < n; i++) { char c[2] = { name[i], 0 }; cmd_out(c); }
    if (r == 2) cmd_out(": running in its window\n");
    else if (r == 1) cmd_out(": running, it yields to the desktop\n");
    else if (r) cmd_out(": would not load\n");
    else if (faulted) cmd_out(": stopped by a fault, the kernel carried on\n");
    else { cmd_out(": exit "); cmd_dec((unsigned)status); cmd_out("\n"); }
}
static int word_is(const char *q, unsigned n, const char *w) {   /* the whole line is the word w */
    unsigned k = 0;
    for (; w[k]; k++) if (k >= n || q[k] != w[k]) return 0;
    return k == n;
}
static int is(const char *q, unsigned n, const char *w) {   /* the line starts with the word w and a space */
    unsigned k = 0;
    for (; w[k]; k++) if (k >= n || q[k] != w[k]) return 0;
    return k < n && q[k] == ' ';
}
static void jt_command(const char *q, unsigned n, int op) {   /* 0 write NAME TEXT, 1 rm NAME, 2 mkdir NAME */
    static char text[1024];
    char name[64];
    unsigned used = word(q, n, name, sizeof name), len = 0;
    if (!sd_ready()) { cmd_out(op == 0 ? "write" : op == 1 ? "rm" : "mkdir"); cmd_out(": no SD card\n"); return; }
    if (op == 0) {                                 /* the rest of the line, then a newline: a text file */
        if (used < n) used++;
        while (used < n && len < sizeof text - 1) text[len++] = q[used++];
        text[len++] = '\n';
    }
    int ok = op == 0 ? sd_write(name, text, len) : op == 1 ? sd_remove(name) : sd_mkdir(name);
    if (ok < 0) return;                            /* refused: emmc.c has said why */
    if (op == 0) cmd_out(ok ? "wrote JT/" : "write: JT/");
    else if (op == 1) cmd_out(ok ? "removed JT/" : "rm: no file JT/");
    else cmd_out(ok ? "made JT/" : "mkdir: no folder made, JT/");
    cmd_out(name);
    if (op == 0 && ok) { cmd_out(", "); cmd_dec(len); cmd_out(" bytes"); }
    if (op == 0 && !ok) cmd_out(" not written");
    if (op == 2 && ok) cmd_out("/");
    cmd_out("\n");
}

static int blocked_while_sam(const char *q, unsigned n) {   /* the one TCP connection and the heap are in use: these would collide */
    static const char *const names[] = {"browse", "open", "back", "forward", "reload", "nimble", "llm", "bt", "userrun"};
    for (unsigned a = 0; a < sizeof names / sizeof *names; a++) {
        unsigned k = 0;
        while (names[a][k] && k < n && q[k] == names[a][k]) k++;
        if (!names[a][k] && (k == n || q[k] == ' ')) return 1;
    }
    return 0;
}
static void not_found(const char *q, unsigned n) {   /* zsh: command not found: WORD */
    char w[40];
    word(q, n, w, sizeof w);
    cmd_out("zsh: command not found: "); cmd_out(w); cmd_out("\n");
}
void cmd_run(const char *q, unsigned n, void (*out)(const char *)) {
    cmd_out = out ? out : kputs;
    while (n && q[0] == ' ') { q++; n--; }
    while (n && q[n - 1] == ' ') n--;
    if (!n) return;
    if (ask_busy() && blocked_while_sam(q, n)) { cmd_out("zsh: sam is using the network, try again when she answers\n"); return; }
    if (n == 3 && q[0] == 's' && q[1] == 'a' && q[2] == 'm') { sam_command(q + 3, 0); return; }
    if (is(q, n, "sam")) { sam_command(q + 4, n - 4); return; }
    if (n == 4 && q[0] == 'h' && q[1] == 'e' && q[2] == 'l' && q[3] == 'p') { say_wrapped("", help, sizeof help - 1); return; }
    if ((n == 2 && q[0] == 'l' && q[1] == 's') || is(q, n, "ls")) {
        if (!sd_ready()) { cmd_out("ls: no SD card\n"); return; }
        char dir[64] = "";
        if (n > 3) word(q + 3, n - 3, dir, sizeof dir);
        if (*dir && !fat_chdir(dir)) { cmd_out("ls: no folder "); cmd_out(dir); cmd_out("\n"); return; }
        ls_count = 0; fat_list(ls_one);
        if (*dir) fat_chdir("..");
        if (!ls_count) cmd_out("ls: no files\n");
        return;
    }
    if (is(q, n, "cat")) { cat_file(q + 4, n - 4); return; }
    if (is(q, n, "write")) {   /* NAME needs a dot (NOTE.TXT) */
        unsigned k = 6;
        while (k < n && q[k] != ' ' && q[k] != '.') k++;
        if (k < n && q[k] == '.') { jt_command(q + 6, n - 6, 0); return; }
        cmd_out("write: use write NAME.EXT TEXT\n"); return;
    }
    if (is(q, n, "rm")) { jt_command(q + 3, n - 3, 1); return; }
    if (is(q, n, "mkdir")) { jt_command(q + 6, n - 6, 2); return; }
    if (word_is(q, n, "reboot") || word_is(q, n, "restart")) { power_run(0); return; }     /* typing it is the confirmation the chord asks for */
    if (word_is(q, n, "shutdown") || word_is(q, n, "poweroff")) { power_run(1); return; }
    if (bt_command(q, n)) return;
    if (beep_command(q, n)) return;
    if (vol_command(q, n)) return;
    if (n > 8 && q[0] == 'u' && q[1] == 's' && q[2] == 'e' && q[3] == 'r' && q[4] == 'r' && q[5] == 'u' && q[6] == 'n' && q[7] == ' ') {
        const char *name = q + 8; unsigned m = n - 8;
        while (m && name[0] == ' ') { name++; m--; }
        if (m) { userrun_command(name, m); return; }
    }
    if (ask_command(q, n)) return;
    if (browse_command(q, n)) return;
    if (nimble_command(q, n)) return;
    if (is(q, n, "llm")) { ask_claude(q, n); return; }
    not_found(q, n);
}
