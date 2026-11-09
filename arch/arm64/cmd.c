/* The one command line. cmd_run takes a typed line and a sink for its output; everything the Terminal's prompt
   understands (help, the /model and /effort commands, the browser, the local model, Claude) is dispatched here and nowhere else.
   Another window can run the same commands by calling cmd_run with its own sink. browser.c and ask.c print through cmd_out.
   The prompt is zsh style (`joshua@tree % `): a word that is no command prints `zsh: command not found: WORD`, and Samantha
   is the command `sam QUESTION`, which returns at once and prints her answer when it lands (ask.c).
   ls and cat read the SD card's FAT partition (emmc.c, drivers/fat.c), 8.3 names, the root or one folder down (ls JT,
   cat JT/NOTE.TXT). write, rm and mkdir change only the JT folder, through emmc.c's sd_write, sd_remove and sd_mkdir. */
#include "bt.h"
#include "emmc.h"
#include "accounts.h"
#include "sudo.h"
#include "limits.h"
#include "login.h"
#include "home.h"
#include "snap.h"
#include "../../drivers/fat.h"
void kputs(const char *s);   /* main.c: the UART and the log of the window in front */
void say_wrapped(const char *prefix, const char *s, unsigned n);   /* ask.c */
int browse_command(const char *q, unsigned n);                     /* browser.c */
int nimble_command(const char *q, unsigned n);                     /* nimble.c: `nimble QUESTION` */
void backup_command(void);                                         /* ask.c: `backup`, the home folder to the relay on the Mac */
void restore_mac_command(void);                                    /* ask.c: `restore mac yes`, the newest backup back from the Mac */
int ask_command(const char *q, unsigned n);                        /* ask.c: "stop" and the /model, /effort, /status, /clear, /help commands */
void ask_claude(const char *q, unsigned n);                        /* ask.c: `llm PROMPT` */
void sam_command(const char *q, unsigned n);                       /* ask.c: `sam QUESTION`, queued and answered without blocking the prompt */
int ask_busy(void);                                                /* ask.c: a `sam` request is waiting on the relay */
int userrun(const char *name, unsigned n, int allow_unsigned, unsigned long *status, int *faulted);
void ufile_args_next(const char *s, unsigned n);   /* ufile.c, 6.69.0: the words after userrun NAME */
void store_install(const char *name, unsigned n);   /* store.c, 6.55.0 */
extern unsigned dbg_want;                           /* main.c, 7.2.0: `debug NAME`, the steps app_start gives the next app */   /* main.c: load an app from the table and run it at EL0 */

int audio_tone(int freq_hz, int ms);                               /* audio.c: the 3.5 mm jack, polled PWM */
int audio_volume(void);                                            /* audio.c: the master volume, 0 to 100 */
void audio_set_volume(int percent);
int audio_mute(void);
void menubar_notify(const char *message);                          /* main.c: the menu bar's short notice */

#define VOL_STEP 10          /* `vol up`, `vol down` and the volume keys move this much */
#define TICK_HZ 1000         /* the click a volume key plays so the new level can be heard: calibrate on the real speaker */
#define TICK_MS 40

void (*cmd_out)(const char *s) = kputs;
static int priv(const char *what);   /* below: 1 if a privileged command may go on */
void cmd_dec(unsigned v) { char t[11]; unsigned n = 10; t[n] = 0; do t[--n] = (char)('0' + v % 10); while (v /= 10); cmd_out(t + n); }

void power_run(int off);                                   /* main.c: the confirmed power action, refused while Samantha answers */
static const char help[] = "browse URL (a bare host is https), open N, back, forward, reload, links, more, up, down, "
                           "top, bottom, find WORD, fill N TEXT (a form field), fetch URL (save it), tabs, mail (login HOST USER, N, send TO SUBJECT | BODY, reply N BODY, logout), calendar subscribe URL, calendar caldav URL USER, calendar sync, cookies [clear], history [WORDS|top|clear], wifi (join NAME), nimble QUESTION (a web answer with sources), beep [FREQ MS], mic [SECONDS], double-tap and hold Space (speak to Samantha), vol [0-100|up|down], llm PROMPT, bt, bt scan, bt pair, userrun NAME [WORDS] (the words go to the app), userrun A | userrun B (A's output is B's input), cores, sched, disk, stick [ls|cat NAME|copy NAME] (a USB stick), debug NAME [STEPS] (run an app one instruction at a time: where it is, the instruction, the registers that changed, then its registers and stack), install NAME (an app from the store), "
                           "ls [DIR], cat NAME, write NAME.EXT TEXT, rm NAME, mkdir NAME (the last three in JT/, admins only; ~ is your own home folder, JT/HOME/YOU, open to you alone: write ~/NOTE.TXT TEXT, cat ~/NOTE.TXT, ls ~), users, admins, adduser NAME PASSWORD, deluser NAME PASSWORD (the password is never shown), "
                           "sudo COMMAND (an admin's password for five minutes, sudo -k forgets it), passwd [NAME], makeadmin NAME, unadmin NAME, logout, snapshot (copy your home folder, the last three are kept), snapshots, restore N yes (put it back as that snapshot was), backup (your home folder to the Mac, through the relay), restore mac yes (the newest backup back from the Mac), boottime (when the first key came and what held the keys up), sam QUESTION (ask Samantha), shortcut [NAME] (NAME = A; B saves one, rm NAME drops it), reboot, shutdown, man NAME (how a command works), help.";
#include "man.h"
static void man_show(const char *q, unsigned n) {   /* 13.6.0: q is the one word after man */
    for (unsigned i = 0; i < sizeof man_pages / sizeof man_pages[0]; i++) {
        const char *w = man_pages[i][0], *p = man_pages[i][1];
        unsigned k = 0;
        while (k < n && w[k] && q[k] == w[k]) k++;
        if (k == n && !w[k]) { cmd_out(p); cmd_out("\n"); return; }
    }
    cmd_out("man: no page for "); for (unsigned k = 0; k < n; k++) { char c[2] = { q[k], 0 }; cmd_out(c); } cmd_out(", try help\n");
}
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
static int walk_path(const char *what, char *path, unsigned cap, int keep, char **leaf) {   /* 4.11.0: `~` and any depth; 1 with the cursor moved (home_back puts it back), else the reason is printed */
    char full[96];
    int e = home_expand(path, full, sizeof full);
    if (e == 0) { cmd_out(what); cmd_out(": no home folder, nobody is signed in\n"); return 0; }
    if (e < 0) { cmd_out(what); cmd_out(": that path is too long\n"); return 0; }
    unsigned k = 0;
    for (; full[k] && k + 1 < cap; k++) path[k] = full[k];
    path[k] = 0;
    int r = home_walk(path, keep, leaf);
    if (r < 0) { cmd_out(what); cmd_out(": that is another person's home folder, an admin can open it with sudo\n"); }
    return r > 0 ? 1 : r < 0 ? 0 : -1;
}
static void cat_file(const char *q, unsigned n) {   /* the first 4 KB of a file, as text */
    static char text[4097];
    char path[64], shown_path[64], *name;
    word(q, n, path, sizeof path);
    for (unsigned i = 0; i < sizeof path; i++) shown_path[i] = path[i];
    if (!sd_ready()) { cmd_out("cat: no SD card\n"); return; }
    int w = walk_path("cat", path, sizeof path, 1, &name);
    if (w == 0) return;
    if (w < 0) { cmd_out("cat: no file "); cmd_out(shown_path); cmd_out("\n"); return; }
    if (accounts_count() && accounts_protected(name) && !priv("cat")) { home_back(); return; }
    int got = fat_read_file(name, text, sizeof text);
    home_back();   /* back to the root */
    if (got < 0) { cmd_out("cat: no file "); cmd_out(shown_path); cmd_out("\n"); return; }
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
int usb_mic_ok(void);   /* xhci.c */
unsigned usb_mic_record(short *dst, unsigned max, unsigned ms);
void usb_mic_stat(unsigned st[6]);
static short mic_buf[16000 * 5];
static int mic_command(const char *q, unsigned n) {   /* `mic` listens for 3 seconds; `mic N` for N (1 to 5). It reports what the USB microphone heard; nothing is kept or sent. */
    int secs = 3;
    if (n < 3 || q[0] != 'm' || q[1] != 'i' || q[2] != 'c' || (n > 3 && q[3] != ' ')) return 0;
    if (n > 3 && (!num(q + 4, n - 4, &secs) || secs < 1 || secs > 5)) { cmd_out("mic: use mic, or mic 1 to 5 (seconds)\n"); return 1; }
    if (!usb_mic_ok()) { cmd_out("mic: no USB microphone found (plug it into a Pi port, then wait a second)\n"); return 1; }
    cmd_out("mic: listening for "); cmd_dec((unsigned)secs); cmd_out(" s, say something\n");
    unsigned got = usb_mic_record(mic_buf, (unsigned)secs * 16000, (unsigned)secs * 1000 + 300), peak = 0;
    unsigned long long sq = 0;
    for (unsigned i = 0; i < got; i++) { int v = mic_buf[i]; unsigned a = (unsigned)(v < 0 ? -v : v); if (a > peak) peak = a; sq += (unsigned long long)(a * a); }
    unsigned rms = 0;
    if (got) { unsigned long long m = sq / got, r = 0; while ((r + 1) * (r + 1) <= m) r++; rms = (unsigned)r; }
    unsigned st[6]; usb_mic_stat(st);
    cmd_out("mic: "); cmd_dec(got); cmd_out(" samples at 16 kHz, loudest "); cmd_dec(peak); cmd_out(" of 32767, average "); cmd_dec(rms); cmd_out("\n");
    char bar[33]; unsigned k = peak * 32 / 32768; if (peak && !k) k = 1;
    for (unsigned i = 0; i < 32; i++) bar[i] = i < k ? '#' : '.';
    bar[32] = 0; cmd_out("mic: ["); cmd_out(bar); cmd_out("]\n");
    cmd_out("mic: device "); cmd_dec(st[0]); cmd_out(" Hz, "); cmd_dec(st[1]); cmd_out(" ch, "); cmd_dec(st[2]); cmd_out(" transfers, "); cmd_dec(st[3]); cmd_out(" errors, last code "); cmd_dec(st[4]); cmd_out("\n");
    if (got < (unsigned)secs * 16000 * 9 / 10) cmd_out("mic: fewer samples than the time asked for, the USB stream is dropping frames\n");
    else if (peak < 100) cmd_out("mic: only silence. Check the mic's mute button and gain knob\n");
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
static int sudo_run;   /* (below) */
static void userrun_command(const char *name, unsigned n) {
    unsigned long status = 0; int faulted = 0;
    int r = userrun(name, n, !accounts_count() || sudo_run, &status, &faulted);   /* 6.54.0: an unsigned app on the card is an admin's, like any write to JT/ */
    if (r == -1) { cmd_out("userrun: no program called that, try hello, calendar, notes, music or loop, or put NAME.APP in JT/APPS\n"); return; }
    if (r == -3) { cmd_out("userrun: an app is already running, close it first\n"); return; }
    if (r == -4 || r == -5) {
        cmd_out("userrun ");
        for (unsigned i = 0; i < n; i++) { char c[2] = { name[i], 0 }; cmd_out(c); }
        cmd_out(r == -4 ? ": not signed, so only an admin runs it: sudo userrun NAME\n" : ": its signature does not match it, so it does not run (tools/signapp.sh signs it again)\n");
        return;
    }
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
void logout_request(void);   /* main.c: the login screen comes back on the next pass of the main loop */
void cmd_run(const char *q, unsigned n, void (*out)(const char *));
/* 11.4.0, Shortcuts: a few Terminal commands under one name, as macOS's Shortcuts chains actions. JT/SHORTCUT.TXT holds one
   a line, `NAME = COMMAND; COMMAND`. `shortcut` lists them, `shortcut NAME` runs each command as if it were typed (shown
   first, with the same sudo rules), `shortcut NAME = ...` saves one and `shortcut rm NAME` drops it. Samantha writes them
   through her `shortcut` plugin (ask.c), only under a name the person typed. A shortcut never runs another one.
   ponytail: one 2 KB file read whole each time; a folder of them when someone has more than a screenful. */
#define SC_FILE "SHORTCUT.TXT"
#define SC_MAX 2048
static char sc_buf[SC_MAX + 1];
static int sc_running;
static unsigned sc_load(void) {
    int got = 0;
    if (sd_ready() && sd_enter_jt(0)) { got = fat_read_file(SC_FILE, sc_buf, SC_MAX); fat_chdir(".."); }
    if (got < 0) got = 0;
    sc_buf[got] = 0;
    return (unsigned)got;
}
static void sc_trim(const char **s, unsigned *n) { while (*n && **s == ' ') { (*s)++; (*n)--; } while (*n && (*s)[*n - 1] == ' ') (*n)--; }
static int sc_name_ok(const char *s, unsigned n) {
    if (!n || n > 12) return 0;
    for (unsigned i = 0; i < n; i++) { char c = s[i]; if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return 0; }
    return 1;
}
static int sc_same(const char *line, unsigned ln, const char *name, unsigned nn) {   /* the line is NAME's: "NAME =" */
    unsigned k = 0; while (k < ln && line[k] != '=') k++;
    const char *a = line; unsigned an = k; sc_trim(&a, &an);
    if (an != nn) return 0;
    for (unsigned i = 0; i < nn; i++) if (a[i] != name[i]) return 0;
    return k < ln;
}
/* Save (or with no body, drop) NAME. 1 saved, 0 refused (it says why). Shared with Samantha's plugin. */
int shortcut_save(const char *name, unsigned nn, const char *body, unsigned bn) {
    sc_trim(&name, &nn); sc_trim(&body, &bn);
    if (!sc_name_ok(name, nn)) { cmd_out("shortcut: a name is 1 to 12 of a-z, 0-9 and -\n"); return 0; }
    if (bn > 200) { cmd_out("shortcut: 200 characters at most\n"); return 0; }
    for (unsigned i = 0; i < bn; i++) if (body[i] == '\n' || body[i] == '\r' || (unsigned char)body[i] < 32) { cmd_out("shortcut: one line only\n"); return 0; }
    for (unsigned i = 0; i + 8 <= bn; i++)
        if ((i == 0 || body[i - 1] == ';' || body[i - 1] == ' ') && word_is(body + i, 8, "shortcut") && (i + 8 == bn || body[i + 8] == ' ' || body[i + 8] == ';')) {
            cmd_out("shortcut: a shortcut cannot run another\n"); return 0;
        }
    if (!sd_ready()) { cmd_out("shortcut: no SD card\n"); return 0; }
    if (!sudo_may_write()) { cmd_out("shortcut: only an admin saves shortcuts\n"); return 0; }
    static char out[SC_MAX + 1];
    unsigned n = sc_load(), o = 0, found = 0;
    for (unsigned i = 0; i < n;) {   /* every other line as it was */
        unsigned e = i; while (e < n && sc_buf[e] != '\n') e++;
        if (sc_same(sc_buf + i, e - i, name, nn)) found = 1;
        else if (e > i) { for (unsigned k = i; k < e; k++) out[o++] = sc_buf[k]; out[o++] = '\n'; }
        i = e + 1;
    }
    if (bn) {
        if (o + nn + bn + 4 > SC_MAX) { cmd_out("shortcut: the file is full, drop one first\n"); return 0; }
        for (unsigned k = 0; k < nn; k++) out[o++] = name[k];
        out[o++] = ' '; out[o++] = '='; out[o++] = ' ';
        for (unsigned k = 0; k < bn; k++) out[o++] = body[k];
        out[o++] = '\n';
    } else if (!found) { cmd_out("shortcut: none named that\n"); return 0; }
    if (sd_write(SC_FILE, out, o) != 1) { cmd_out("shortcut: the card refused it\n"); return 0; }
    char nm[13]; for (unsigned k = 0; k < nn; k++) nm[k] = name[k]; nm[nn] = 0;
    cmd_out(bn ? "shortcut: saved " : "shortcut: dropped "); cmd_out(nm); cmd_out("\n");
    return 1;
}
static void shortcut_command(const char *q, unsigned n) {   /* the words after `shortcut` (n may be 0) */
    sc_trim(&q, &n);
    unsigned eq = 0; while (eq < n && q[eq] != '=') eq++;
    if (eq < n) { shortcut_save(q, eq, q + eq + 1, n - eq - 1); return; }
    if (is(q, n, "rm")) { shortcut_save(q + 3, n - 3, "", 0); return; }
    unsigned len = sc_load();
    if (!n) {
        if (!len) { cmd_out("shortcut: none yet. shortcut NAME = COMMAND; COMMAND saves one\n"); return; }
        say_wrapped("", sc_buf, len);
        return;
    }
    if (sc_running) { cmd_out("shortcut: a shortcut cannot run another\n"); return; }
    for (unsigned i = 0; i < len;) {
        unsigned e = i; while (e < len && sc_buf[e] != '\n') e++;
        if (sc_same(sc_buf + i, e - i, q, n)) {
            unsigned b = i; while (sc_buf[b] != '=') b++;
            static char body[SC_MAX + 1]; unsigned bn = 0;   /* a copy: a command may read the file again */
            for (unsigned k = b + 1; k < e; k++) body[bn++] = sc_buf[k];
            void (*out)(const char *) = cmd_out;
            sc_running = 1;
            for (unsigned s = 0; s < bn;) {
                unsigned t = s; while (t < bn && body[t] != ';') t++;
                const char *c = body + s; unsigned cn = t - s; sc_trim(&c, &cn);
                if (cn) {
                    char shown[204]; unsigned k = 0; for (; k < cn && k < 200; k++) shown[k] = c[k]; shown[k] = 0;
                    out("shortcut> "); out(shown); out("\n");
                    cmd_run(c, cn, out);
                }
                s = t + 1;
            }
            sc_running = 0; cmd_out = out;
            return;
        }
        i = e + 1;
    }
    cmd_out("shortcut: none named that. shortcut lists them\n");
}
static int eq_(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int sudo_run;   /* above 0 while a command runs under sudo: the password was just checked, or is still remembered */
/* A privileged command (adduser, deluser of someone else, any write to JT/, passwd for someone else, makeadmin, unadmin). With
   no accounts everything is open, as before. Under sudo it goes. An admin who forgot the word is told to use it; anyone else
   is told they are not an admin. 1 if the command may go on. */
static int priv(const char *what) {
    if (!accounts_count() || sudo_run) return 1;
    cmd_out(what);
    cmd_out(sudo_is_admin(current_user) ? ": needs sudo, try sudo " : ": you are not an admin; try sudo ");
    cmd_out(what); cmd_out("\n");
    return 0;
}
static unsigned slen_(const char *s);
static void home_file_command(const char *verb, int op, char *name, const char *text, unsigned len) {   /* write, rm or mkdir on a path with a slash or a ~: 4.11.0 */
    char full[96], rel[96];
    unsigned k = 0;
    if (name[0] == '~') { for (; name[k]; k++) rel[k] = name[k]; rel[k] = 0; }
    else { rel[0] = 'J'; rel[1] = 'T'; rel[2] = '/'; for (; name[k] && k < sizeof rel - 4; k++) rel[3 + k] = name[k]; rel[3 + k] = 0; }   /* a plain path is under JT/, as write NAME always was */
    int e = home_expand(rel, full, sizeof full);
    if (e == 0) { cmd_out(verb); cmd_out(": no home folder, nobody is signed in\n"); return; }
    if (e < 0) { cmd_out(verb); cmd_out(": that path is too long\n"); return; }
    int own = home_owns(full);   /* the signed-in person's own folder needs no sudo; the rest of JT/ is shared and does */
    if (!own && !priv(verb)) return;
    if (!own && !sd_ready()) { cmd_out(verb); cmd_out(": no SD card\n"); return; }
    int ok = home_op(op, full, text, len);
    if (ok == -1) { cmd_out(verb); cmd_out(": that is another person's home folder, an admin can open it with sudo\n"); return; }
    if (ok == -2) { cmd_out(verb); cmd_out(": use an 8.3 name such as NOTE.TXT\n"); return; }
    if (op == 0) cmd_out(ok ? "wrote " : "write: ");
    else if (op == 1) cmd_out(ok ? "removed " : "rm: no file ");
    else cmd_out(ok ? "made " : "mkdir: no folder made, ");
    cmd_out(full);
    if (op == 0 && ok) { cmd_out(", "); cmd_dec(len); cmd_out(" bytes"); }
    if (op == 0 && !ok) cmd_out(" not written");
    if (op == 2 && ok) cmd_out("/");
    cmd_out("\n");
}
static void jt_command(const char *q, unsigned n, int op) {   /* 0 write NAME TEXT, 1 rm NAME, 2 mkdir NAME */
    static char text[1024];
    char name[64];
    unsigned used = word(q, n, name, sizeof name), len = 0;
    const char *verb = op == 0 ? "write" : op == 1 ? "rm" : "mkdir";
    int homed = name[0] == '~' || ((name[0] | 32) == 'h' && (name[1] | 32) == 'o' && (name[2] | 32) == 'm' && (name[3] | 32) == 'e' && name[4] == '/');   /* ~/... or HOME/...: every other name is a bare JT/ file, as before */
    if (!homed && !priv(verb)) return;
    if (accounts_protected(name)) { cmd_out(verb); cmd_out(": "); cmd_out(name); cmd_out(" is kept by adduser, passwd and makeadmin, not by hand\n"); return; }
    if (!sd_ready()) { cmd_out(verb); cmd_out(": no SD card\n"); return; }
    if (op == 0) {                                 /* the rest of the line, then a newline: a text file */
        if (used < n) used++;
        while (used < n && len < sizeof text - 1) text[len++] = q[used++];
        text[len++] = '\n';
    }
    if (homed) { home_file_command(verb, op, name, text, len); return; }
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

static unsigned slen_(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
/* users, adduser NAME PASSWORD, deluser NAME PASSWORD (accounts.c). The password is the last word, copied to a local buffer that is
   wiped before returning; no message here ever repeats it, and ask.c shows the typed line with it starred out. */
static void users_command(void) {
    if (!accounts_count()) { cmd_out("users: none yet, try adduser NAME PASSWORD\n"); return; }
    for (unsigned i = 0; i < accounts_count(); i++) { cmd_out(accounts_name(i)); cmd_out("\n"); }
}
static void home_note(const char *name) {   /* a made account gets its folder; a card that will not take it is said, the account stays */
    char dir[HOME_DIR_MAX + 1]; home_dirname(name, dir);
    if (home_make(name)) { cmd_out("adduser: home folder JT/HOME/"); cmd_out(dir); cmd_out("\n"); }
    else cmd_out("adduser: the home folder could not be made, it is made again the first time a file is saved\n");
}
static int account_apply(const char *name_in, const char *pass_in, int remove, int forced, int first, int self);
static void account_command(const char *q, unsigned n, int remove) {   /* q is the line after the command word */
    char name[ACC_NAME_MAX + 2], pass[ACC_PASS_MAX + 2];
    unsigned used = word(q, n, name, sizeof name);
    while (used < n && q[used] == ' ') used++;
    unsigned used2 = used + word(q + used, n - used, pass, sizeof pass);
    int forced = remove && sudo_run && name[0] && !pass[0];   /* an admin's deluser NAME needs no password of the account's own */
    if (!name[0] || (!pass[0] && !forced) || used2 != n || slen_(name) > ACC_NAME_MAX || slen_(pass) > ACC_PASS_MAX) {
        cmd_out(remove ? "deluser: use deluser NAME PASSWORD (no spaces in the password)\n" : "adduser: use adduser NAME PASSWORD (no spaces in the password)\n");
        accounts_wipe(pass, sizeof pass);
        return;
    }
    int first = !remove && !accounts_count();      /* no accounts at all: the first one may be made by anyone, and is an admin */
    int self = remove && eq_(name, current_user);
    if (!first && !(self && !forced) && !priv(remove ? "deluser" : "adduser")) { accounts_wipe(pass, sizeof pass); return; }
    account_apply(name, pass, remove, forced, first, self);
    accounts_wipe(pass, sizeof pass);
}
/* The work of adduser and deluser once the line is read and the privilege is settled; the Settings Users pane (users_add and
   users_delete below) runs this same code. 0 when the account was added or removed, 1 when it was refused (the reason is said). */
static int account_apply(const char *name_in, const char *pass_in, int remove, int forced, int first, int self) {
    char name[ACC_NAME_MAX + 2], pass[ACC_PASS_MAX + 2];
    unsigned k = 0;
    for (; name_in[k] && k < ACC_NAME_MAX + 1; k++) name[k] = name_in[k];
    name[k] = 0;
    for (k = 0; pass_in[k] && k < ACC_PASS_MAX + 1; k++) pass[k] = pass_in[k];
    pass[k] = 0;
    if (!remove && home_collides(name)) {   /* two long names that fold to one folder name would share their files */
        cmd_out("adduser "); cmd_out(name); cmd_out(": its home folder name is taken by another account, pick a different name\n");
        accounts_wipe(pass, sizeof pass);
        return 1;
    }
    if (remove && sudo_is_admin(name) && sudo_admin_count() == 1 && accounts_count() > 1) {
        cmd_out("deluser "); cmd_out(name); cmd_out(": the last admin, make another admin first\n");
        accounts_wipe(pass, sizeof pass);
        return 1;
    }
    cmd_out(remove ? "deluser: working\n" : "adduser: working, the password is being stretched\n");
    int r = remove ? (forced ? accounts_delete(name) : accounts_remove(name, pass)) : accounts_add(name, pass);
    accounts_wipe(pass, sizeof pass);
    cmd_out(remove ? "deluser " : "adduser ");
    cmd_out(name);
    if (r) { cmd_out(": "); cmd_out(accounts_reason(r)); }
    else cmd_out(remove ? ": removed" : ": added");
    cmd_out("\n");
    if (r) return 1;
    if (remove) {
        char dir[HOME_DIR_MAX + 1]; home_dirname(name, dir);   /* 4.11.0: nothing is deleted with the account */
        cmd_out("deluser: the files in JT/HOME/"); cmd_out(dir); cmd_out(" stay on the card; a new account with that name would see them\n");
        sudo_scrub(name);                           /* the name must not come back as an admin */
        if (self) {
            sudo_drop(); accounts_wipe(current_user, ACC_NAME_MAX + 1);
            if (accounts_count()) logout_request();
        }
    } else if (first) {
        sudo_first(name);
        for (unsigned i = 0; name[i] && i < ACC_NAME_MAX; i++) { current_user[i] = name[i]; current_user[i + 1] = 0; }
        sudo_arm(name);
        cmd_out(name); cmd_out(" is the first account, so an admin, and signed in\n");
        home_note(name);
    } else { sudo_scrub(name); home_note(name); }
    return 0;
}

static void admins_command(void) {
    if (!sudo_admin_count()) { cmd_out("admins: none, the first adduser makes one\n"); return; }
    for (unsigned i = 0; i < sudo_admin_count(); i++) { cmd_out(sudo_admin_name(i)); cmd_out("\n"); }
}
static void admin_command(const char *q, unsigned n, int make) {   /* makeadmin NAME, unadmin NAME */
    const char *cmd = make ? "makeadmin" : "unadmin";
    char name[ACC_NAME_MAX + 2];
    unsigned used = word(q, n, name, sizeof name);
    while (used < n && q[used] == ' ') used++;
    if (!name[0] || used != n || slen_(name) > ACC_NAME_MAX) { cmd_out(cmd); cmd_out(": use "); cmd_out(cmd); cmd_out(" NAME\n"); return; }
    if (!priv(cmd)) return;
    int r = make ? sudo_make(name) : sudo_unmake(name);
    cmd_out(cmd); cmd_out(" "); cmd_out(name); cmd_out(": ");
    cmd_out(r ? sudo_reason(r) : make ? "now an admin" : "no longer an admin");
    cmd_out("\n");
}

/* sudo and passwd need a password, typed on the line after the command with the stars drawn by ask.c (ask_password): the next
   line Enter sends comes to cmd_password instead of cmd_run, whatever it holds. That keeps the Terminal one line at a time,
   shows nothing of the password on screen or UART (not even its length: the echo is the prompt alone), and lets Esc cancel.
   Every buffer that held a password, or a command with one in it, is wiped before the step returns. */
#define PW_NONE 0
#define PW_SUDO 1     /* sudo CMD: waiting for the signed-in user's password; pw_cmd holds CMD */
#define PW_OLD 2      /* passwd: the current password of pw_target */
#define PW_NEW1 3     /* passwd: the new password */
#define PW_NEW2 4     /* passwd: the new password again */
#define PW_MAIL 5     /* mail login: the app password, for mail.c */
#define PW_WIFI 6     /* wifi join SSID: the network's passphrase; pw_cmd holds the SSID */
#define PW_CALDAV 7   /* calendar caldav URL USER: the app password, for ics.c */
void ask_password(const char *label);   /* ask.c: the next line typed is a password: stars, no echo */
static int pw_state;
static char pw_cmd[256], pw_target[ACC_NAME_MAX + 2], pw_new[ACC_PASS_MAX + 2], pw_label[56];
static void pw_clear(void) {
    accounts_wipe(pw_cmd, sizeof pw_cmd); accounts_wipe(pw_new, sizeof pw_new); accounts_wipe(pw_target, sizeof pw_target);
    pw_state = PW_NONE;
}
static void pw_ask(int state, const char *a, const char *b) {   /* the label is a + b + ": " */
    unsigned k = 0;
    for (; a[k] && k < sizeof pw_label - 3; k++) pw_label[k] = a[k];
    for (unsigned i = 0; b[i] && k < sizeof pw_label - 3; i++) pw_label[k++] = b[i];
    pw_label[k++] = ':'; pw_label[k++] = ' '; pw_label[k] = 0;
    pw_state = state;
    ask_password(pw_label);
}
void cmd_password_cancel(void) { pw_clear(); }
void ask_mail_password(const char *user) { pw_clear(); pw_ask(PW_MAIL, "App password for ", user); }   /* mail.c */
void ask_caldav_password(const char *user) { pw_clear(); pw_ask(PW_CALDAV, "Calendar app password for ", user); }   /* ics.c */
void ics_password(const char *pass);
void mail_password(const char *pass);   /* mail.c */
int mail_command(const char *q, unsigned n);
int ics_command(const char *q, unsigned n);   /* ics.c */
unsigned wifi_networks(const char **name, int *rssi, unsigned max); const char *wifi_network(void); int wifi_nic_up(void);   /* wifi.c */
int wifi_save(const char *ssid, unsigned sl, const char *pass, unsigned pl); int wifi_rejoin(void);   /* ask.c: Esc at the prompt, or a sign-out */
static void sudo_command(const char *q, unsigned n) {   /* q is the line after "sudo " */
    while (n && q[0] == ' ') { q++; n--; }
    if (!n) { cmd_out("sudo: use sudo COMMAND, or sudo -k to forget the password\n"); return; }
    if (word_is(q, n, "-k")) { sudo_drop(); cmd_out("sudo: password forgotten\n"); return; }
    if (word_is(q, n, "sudo") || is(q, n, "sudo")) { cmd_out("sudo: not inside sudo\n"); return; }
    if (!accounts_count()) { cmd_run(q, n, cmd_out); return; }   /* no accounts: everything is open already */
    if (!current_user[0]) { cmd_out("sudo: nobody is signed in\n"); return; }
    if (sudo_active()) { sudo_run++; cmd_run(q, n, cmd_out); sudo_run--; return; }
    if (n >= sizeof pw_cmd) { cmd_out("sudo: that line is too long\n"); return; }
    pw_clear();
    for (unsigned i = 0; i < n; i++) pw_cmd[i] = q[i];
    pw_ask(PW_SUDO, "[sudo] password for ", current_user);
}
/* 12.0, parental controls: `limits` lists them; `limits NAME apps A,B` (or all), `limits NAME hours F-T` (or any), `limits NAME
   web off` (or on) and `limits NAME clear` set them, under sudo. limits.c keeps JT/LIMITS.TXT and does the refusing. */
static void limits_command(const char *q, unsigned n) {
    char name[ACC_NAME_MAX + 2], field[8], value[LIM_APPS_MAX + 2], line[ACC_NAME_MAX + LIM_APPS_MAX + 40];
    unsigned u = word(q, n, name, sizeof name); while (u < n && q[u] == ' ') u++;
    unsigned v = u + word(q + u, n - u, field, sizeof field); while (v < n && q[v] == ' ') v++;
    unsigned w = v + word(q + v, n - v, value, sizeof value); while (w < n && q[w] == ' ') w++;
    if (!name[0]) {
        unsigned i = 0;
        for (; limits_line(i, line, sizeof line); i++) { cmd_out("limits: "); cmd_out(line); cmd_out("\n"); }
        if (!i) cmd_out("limits: nobody is limited\n");
        return;
    }
    int clear = eq_(field, "clear");
    if (w != n || !field[0] || (!clear && !value[0]) || (clear && value[0])) { cmd_out("limits: use limits NAME apps A,B | hours F-T | web on|off | clear\n"); return; }
    if (!priv("limits")) return;
    int r = limits_set(name, field, value);
    if (r == ACC_E_NOUSER) { cmd_out("limits: no such account\n"); return; }
    if (r == LIM_E_ADMIN) { cmd_out("limits: "); cmd_out(name); cmd_out(" is an admin, and admins are never limited\n"); return; }
    if (r == LIM_E_VALUE) { cmd_out("limits: apps takes names with commas or all, hours takes F-T or any, web takes on or off\n"); return; }
    if (r) { cmd_out("limits: not saved, the card refused it\n"); return; }
    for (unsigned i = 0; limits_line(i, line, sizeof line); i++) {
        unsigned k = 0; while (name[k] && line[k] == name[k]) k++;
        if (!name[k] && (line[k] == ' ' || !line[k])) { cmd_out("limits: "); cmd_out(line); cmd_out("\n"); return; }
    }
    cmd_out("limits: "); cmd_out(name); cmd_out(" is not limited\n");
}
static void passwd_command(const char *q, unsigned n) {   /* passwd, passwd NAME; under sudo passwd NAME resets without the old one */
    char name[ACC_NAME_MAX + 2];
    unsigned used = word(q, n, name, sizeof name);
    while (used < n && q[used] == ' ') used++;
    if (used != n || slen_(name) > ACC_NAME_MAX) { cmd_out("passwd: use passwd, or sudo passwd NAME\n"); return; }
    if (!accounts_count()) { cmd_out("passwd: no accounts yet, adduser NAME PASSWORD makes one\n"); return; }
    if (!name[0]) for (unsigned i = 0; current_user[i]; i++) { name[i] = current_user[i]; name[i + 1] = 0; }
    if (!name[0]) { cmd_out("passwd: nobody is signed in\n"); return; }
    int self = eq_(name, current_user);
    if (!self && !priv("passwd")) return;
    if (!accounts_has(name)) { cmd_out("passwd: no such account\n"); return; }
    pw_clear();
    for (unsigned i = 0; name[i]; i++) { pw_target[i] = name[i]; pw_target[i + 1] = 0; }
    if (self && !sudo_run) pw_ask(PW_OLD, "Current password", "");
    else pw_ask(PW_NEW1, "New password", "");
}
/* The work of passwd once both new passwords agree; the Settings Users pane (users_passwd below) runs this same code. */
void keychain_rekey(const char *name, const char *pass);   /* keychain.c */
static int passwd_apply(const char *target, const char *pass) {
    cmd_out("passwd: working, the password is being stretched\n");
    int r = accounts_passwd(target, pass);
    if (!r) keychain_rekey(target, pass);   /* 6.21.0: the signed-in person's own keychain follows the new password */
    cmd_out("passwd "); cmd_out(target); cmd_out(r ? ": " : ": password changed");
    if (r) cmd_out(accounts_reason(r));
    cmd_out("\n");
    return r != 0;
}
/* ask.c, for the line typed while a password is wanted. */
void cmd_password(const char *q, unsigned n, void (*out)(const char *)) {
    cmd_out = out ? out : kputs;
    char pass[ACC_PASS_MAX + 2];
    int st = pw_state, big = n > ACC_PASS_MAX;
    pw_state = PW_NONE;
    for (unsigned i = 0; i < n && i < ACC_PASS_MAX + 1; i++) pass[i] = q[i];
    pass[big ? ACC_PASS_MAX + 1 : n] = 0;
    if (st == PW_SUDO) {
        int r = sudo_check(current_user, pass);   /* waits a second when it is wrong */
        accounts_wipe(pass, sizeof pass);
        if (r == SUDO_E_NOTADMIN) { cmd_out("sudo: "); cmd_out(current_user); cmd_out(" is not an admin\n"); pw_clear(); return; }
        if (r) { cmd_out("sudo: wrong password\n"); pw_clear(); return; }
        char line[sizeof pw_cmd]; unsigned len = slen_(pw_cmd);
        for (unsigned i = 0; i <= len; i++) line[i] = pw_cmd[i];
        pw_clear();
        sudo_run++; cmd_run(line, len, cmd_out); sudo_run--;
        accounts_wipe(line, sizeof line);
    } else if (st == PW_OLD) {
        int r = big ? ACC_E_WRONG : accounts_check(pw_target, pass);
        accounts_wipe(pass, sizeof pass);
        if (r) { login_wait_ms(LOGIN_DELAY_MS); cmd_out("passwd: wrong password\n"); pw_clear(); return; }
        pw_ask(PW_NEW1, "New password", "");
    } else if (st == PW_NEW1) {
        if (big || slen_(pass) < ACC_PASS_MIN) { accounts_wipe(pass, sizeof pass); cmd_out("passwd: "); cmd_out(accounts_reason(ACC_E_PASS)); cmd_out(", nothing changed\n"); pw_clear(); return; }
        for (unsigned i = 0; i <= slen_(pass); i++) pw_new[i] = pass[i];
        accounts_wipe(pass, sizeof pass);
        pw_ask(PW_NEW2, "Retype new password", "");
    } else if (st == PW_NEW2) {
        int ok = !big && eq_(pass, pw_new);
        accounts_wipe(pass, sizeof pass);
        if (!ok) { cmd_out("passwd: the two passwords differ, nothing changed\n"); pw_clear(); return; }
        passwd_apply(pw_target, pw_new);
        pw_clear();
    } else if (st == PW_CALDAV) {   /* 6.24.0 */
        if (big || !pass[0]) cmd_out("calendar: no app password, nothing changed\n"); else ics_password(pass);
        accounts_wipe(pass, sizeof pass);
    } else if (st == PW_WIFI) {   /* 6.22.0 */
        int r = big ? -1 : wifi_save(pw_cmd, slen_(pw_cmd), pass, slen_(pass));
        accounts_wipe(pass, sizeof pass);
        if (r < 0) cmd_out("wifi: a passphrase is 8 to 63 plain characters, nothing saved\n");
        else if (!r) cmd_out("wifi: no card to save it on\n");
        else {   /* 6.88.0: joined now, where the chip is running */
            cmd_out("wifi: saved "); cmd_out(pw_cmd); cmd_out(", joining\n");
            int j = wifi_rejoin();
            cmd_out(j > 0 ? "wifi: joined\n" : j < 0 ? "wifi: it did not join; check the passphrase, wifi join tries again\n" : "wifi: the Pi joins it at its next start\n");
        }
        pw_clear();
    } else if (st == PW_MAIL) {
        if (big || !pass[0]) cmd_out("mail: no app password, nothing changed\n"); else mail_password(pass);
        accounts_wipe(pass, sizeof pass);
    } else accounts_wipe(pass, sizeof pass);
}
/* The Settings Users pane (settings_ui.h, 4.14.0) calls these. The pane has already settled who may do what with the same
   sudo_check and remembered window the Terminal uses; the work and its messages are the commands' own, said through out. */
int users_add(const char *name, const char *pass, void (*out)(const char *)) {
    void (*saved)(const char *) = cmd_out; cmd_out = out ? out : kputs;
    int r = account_apply(name, pass, 0, 0, !accounts_count(), 0);
    cmd_out = saved; return r;
}
int users_delete(const char *name, void (*out)(const char *)) {
    void (*saved)(const char *) = cmd_out; cmd_out = out ? out : kputs;
    int r = account_apply(name, "", 1, 1, 0, eq_(name, current_user));
    cmd_out = saved; return r;
}
int users_passwd(const char *name, const char *pass, void (*out)(const char *)) {
    void (*saved)(const char *) = cmd_out; cmd_out = out ? out : kputs;
    int r = passwd_apply(name, pass);
    cmd_out = saved; return r;
}
static int blocked_while_sam(const char *q, unsigned n) {   /* the one TCP connection and the heap are in use: these would collide */
    static const char *const names[] = {"browse", "open", "back", "forward", "reload", "nimble", "llm", "bt", "userrun", "debug", "install"};
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
static void snap_one(unsigned id, const char *info) { cmd_dec(id); cmd_out("  "); cmd_out(info); cmd_out("\n"); }
static int snap_refused(const char *verb, int r) {   /* the reason a snapshot call said no, printed; 1 when it did */
    if (r >= 0 && r != 0) return 0;
    cmd_out(verb);
    cmd_out(r == -1 ? ": no SD card\n" : r == -2 ? ": nobody is signed in, so there is no home folder\n" : r == -3 ? ": no such snapshot, see snapshots\n" : ": the card would not take it\n");
    return 1;
}
static void snap_command(const char *q, unsigned n) {   /* snapshot | snapshots | restore N yes */
    unsigned id, files, skipped;
    if (word_is(q, n, "snapshots")) {
        int r = snap_list(snap_one);
        if (r == 0) cmd_out("snapshots: none yet, try snapshot\n");
        else if (r < 0) snap_refused("snapshots", r);
        return;
    }
    if (word_is(q, n, "snapshot")) {
        int r = snap_take(&id, &files, &skipped);
        if (snap_refused("snapshot", r)) return;
        cmd_out("snapshot "); cmd_dec(id); cmd_out(": "); cmd_dec(files); cmd_out(" files");
        if (skipped) { cmd_out(", "); cmd_dec(skipped); cmd_out(" skipped (over 32 KB or past 24 in a folder)"); }
        cmd_out("\n"); return;
    }
    char w1[16], w2[16];
    unsigned used = word(q + 8, n - 8, w1, sizeof w1);
    word(q + 8 + used, n - 8 - used, w2, sizeof w2);
    int v, yes = w2[0] == 'y' && w2[1] == 'e' && w2[2] == 's' && !w2[3];
    if (w1[0] == 'm' && w1[1] == 'a' && w1[2] == 'c' && !w1[3]) {
        if (yes) { restore_mac_command(); return; }
        cmd_out("restore mac: this puts your newest backup on the Mac into your home folder, over any file of the same name. "
                "A snapshot is taken first, so restore N yes undoes it (with three kept, the oldest goes). Type restore mac yes to go on\n");
        return;
    }
    if (!num(w1, slen_(w1), &v)) { cmd_out("restore: use restore N yes (N from snapshots), or restore mac yes\n"); return; }
    if (!yes) {
        cmd_out("restore "); cmd_dec((unsigned)v); cmd_out(": this makes your home folder exactly as that snapshot was, files made since are removed. Type restore ");
        cmd_dec((unsigned)v); cmd_out(" yes to go on\n"); return;
    }
    int r = snap_restore((unsigned)v, &files, &skipped);
    if (snap_refused("restore", r)) return;
    cmd_out("restore "); cmd_dec((unsigned)v); cmd_out(": "); cmd_dec(files); cmd_out(" files back");
    if (skipped) { cmd_out(", "); cmd_dec(skipped); cmd_out(" skipped"); }
    cmd_out("\n");
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
    if (word_is(q, n, "man")) { cmd_out("man: which command? try man ls\n"); return; }
    if (is(q, n, "man")) { man_show(q + 4, n - 4); return; }
    if (word_is(q, n, "limits")) { limits_command(q, 0); return; }
    if (is(q, n, "limits")) { limits_command(q + 7, n - 7); return; }
    if (word_is(q, n, "shortcut")) { shortcut_command(q, 0); return; }
    if (is(q, n, "shortcut")) { shortcut_command(q + 9, n - 9); return; }
    if ((n == 2 && q[0] == 'l' && q[1] == 's') || is(q, n, "ls")) {
        if (!sd_ready()) { cmd_out("ls: no SD card\n"); return; }
        char dir[64] = "";
        if (n > 3) word(q + 3, n - 3, dir, sizeof dir);
        char shown_dir[64]; for (unsigned i = 0; i < sizeof dir; i++) shown_dir[i] = dir[i];
        char *leaf; int w = 1;
        if (*dir) w = walk_path("ls", dir, sizeof dir, 0, &leaf);
        if (w == 0) return;
        if (w < 0) { cmd_out("ls: no folder "); cmd_out(shown_dir); cmd_out("\n"); return; }
        ls_count = 0; fat_list(ls_one);
        if (*dir) home_back();
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
    if (word_is(q, n, "stick") || is(q, n, "stick")) {   /* 8.9.0: a USB stick: stick ls, stick cat NAME, stick copy NAME (into JT/) */
        int stick_begin(void); void stick_end(void); unsigned long usb_disk_blocks(void); void *kmalloc(unsigned int n);
        char verb[8], name[16]; unsigned u = word(q + 5, n - 5, verb, sizeof verb), vn = 0;   /* word skips the spaces before each */
        word(q + 5 + u, n - 5 - u, name, sizeof name);
        while (verb[vn]) vn++;
        int ls = !vn || word_is(verb, vn, "ls"), cat = word_is(verb, vn, "cat"), copy = word_is(verb, vn, "copy");
        if (!ls && !cat && !copy) { cmd_out("stick ls, stick cat NAME, stick copy NAME (into JT/ on the card)\n"); return; }
        if ((cat || copy) && !name[0]) { cmd_out("stick: which file? stick ls lists them\n"); return; }
        if (copy && accounts_count() && !priv("stick copy")) return;   /* a write to JT/, an admin's, like write */
        if (!usb_disk_blocks()) { cmd_out("stick: no USB stick\n"); return; }
        static unsigned char *sbuf; const unsigned SMAX = 256 * 1024;
        if (!sbuf) sbuf = kmalloc(SMAX);
        if (!sbuf) { cmd_out("stick: out of memory\n"); return; }
        if (!stick_begin()) { cmd_out("stick: no FAT partition on it\n"); return; }
        int got = 0;
        if (ls) { ls_count = 0; fat_list(ls_one); if (!ls_count) cmd_out("stick: empty\n"); }
        else got = fat_read_file(name, sbuf, SMAX);
        stick_end();
        if (ls) return;
        if (got < 0) { cmd_out("stick: no file "); cmd_out(name); cmd_out(" on the stick\n"); return; }
        if (cat) {
            static char t[4097]; unsigned k = (unsigned)got < 4096 ? (unsigned)got : 4096;
            for (unsigned i = 0; i < k; i++) t[i] = (char)sbuf[i];
            t[k] = 0; cmd_out(t); if (k && t[k - 1] != '\n') cmd_out("\n");
            return;
        }
        if ((unsigned)got >= SMAX) { cmd_out("stick: that file is over 256 KB, too big to copy yet\n"); return; }
        int w = sd_write(name, sbuf, (unsigned)got);
        if (w == 1) { cmd_out("stick: copied "); cmd_out(name); cmd_out(" to JT/, "); cmd_dec((unsigned)got); cmd_out(" bytes\n"); }
        else cmd_out(w < 0 ? "stick: that name is not allowed in JT/\n" : "stick: the card would not take it\n");
        return;
    }
    if (word_is(q, n, "disk")) {   /* 8.8.0: the block cache's counts since boot */
        void disk_cache_stats(unsigned *, unsigned *, unsigned *); unsigned h, r, w; disk_cache_stats(&h, &r, &w);
        cmd_out("disk: "); cmd_dec(h); cmd_out(" sectors from the cache, "); cmd_dec(r); cmd_out(" read from the card, "); cmd_dec(w); cmd_out(" written through\n");
        return;
    }
    if (word_is(q, n, "sched")) {   /* 8.6.0: the longest one app held the core in a turn since the last time this was asked */
        extern unsigned long sched_worst; unsigned long cnt_hz(void);
        cmd_out("sched: the longest turn an app took was "); cmd_dec((unsigned)(sched_worst * 1000 / cnt_hz())); cmd_out(" ms; every app runs in 10 ms slices, the desktop between them\n");
        sched_worst = 0; return;
    }
    if (word_is(q, n, "reader on") || word_is(q, n, "reader off")) { extern int reader_on; void reader_set(int on); reader_set(q[8] == 'n'); cmd_out(reader_on ? "reader: on; the arrows read the dock, Cmd+F5 turns it off\n" : "reader: off\n"); return; }   /* 10.0.0 */
    if (word_is(q, n, "mem")) {   /* 8.12.0: the heap's frame pool and the swap file */
        extern unsigned pool_in; extern unsigned long swap_outs, swap_ins; unsigned swap_pool_pages(void);
        cmd_out("mem: "); cmd_dec(pool_in); cmd_out(" of "); cmd_dec(swap_pool_pages()); cmd_out(" heap pages in RAM, ");
        cmd_dec((unsigned)swap_outs); cmd_out(" sent to the card, "); cmd_dec((unsigned)swap_ins); cmd_out(" read back\n");
        return;
    }
    if (word_is(q, n, "cores")) {   /* 8.2.0: which of the four cores are up, and how often each has woken */
        extern volatile unsigned long core_runs[4 * 8], core_jobs[4 * 8], core_mpidr[4]; extern volatile int core_up[4]; unsigned cores_count(void);
        cmd_out("cores: "); cmd_dec(cores_count()); cmd_out(" of 4 running\n");
        for (int c = 0; c < 4; c++) {
            cmd_out("core "); cmd_dec((unsigned)c);
            if (!core_up[c]) { cmd_out(": asleep\n"); continue; }
            cmd_out(c ? ": up, woken " : ": up, runs the kernel"); if (c) cmd_dec((unsigned)core_runs[c * 8]);
            if (c) { cmd_out(" times, ran "); cmd_dec((unsigned)core_jobs[c * 8]); cmd_out(" app slices"); }   /* 8.11.0 */
            cmd_out("\n");
        }
        return;
    }
    if (word_is(q, n, "users")) { users_command(); return; }
    if (word_is(q, n, "admins")) { admins_command(); return; }
    if (word_is(q, n, "sudo")) { sudo_command(q, 0); return; }
    if (is(q, n, "sudo")) { sudo_command(q + 5, n - 5); return; }
    if (word_is(q, n, "passwd") || is(q, n, "passwd")) { passwd_command(q + 6 + (n > 6), n > 6 ? n - 7 : 0); return; }
    if (word_is(q, n, "makeadmin") || is(q, n, "makeadmin")) { admin_command(q + 9 + (n > 9), n > 9 ? n - 10 : 0, 1); return; }
    if (word_is(q, n, "unadmin") || is(q, n, "unadmin")) { admin_command(q + 7 + (n > 7), n > 7 ? n - 8 : 0, 0); return; }
    if (word_is(q, n, "backup")) { backup_command(); return; }
    if (word_is(q, n, "boottime")) { const char *boot_stamps(void); const char *t = boot_stamps(); cmd_out(*t ? t : "boottime: no key yet and no slow step\n"); return; }   /* boot.c: the dead first seconds, timed */
    if (word_is(q, n, "logout")) { if (accounts_count()) { cmd_out("logout: signing out\n"); logout_request(); } else cmd_out("logout: no accounts, nobody to sign out\n"); return; }
    if (word_is(q, n, "adduser") || is(q, n, "adduser")) { account_command(q + 7 + (n > 7), n > 7 ? n - 8 : 0, 0); return; }
    if (word_is(q, n, "deluser") || is(q, n, "deluser")) { account_command(q + 7 + (n > 7), n > 7 ? n - 8 : 0, 1); return; }
    if (word_is(q, n, "snapshot") || word_is(q, n, "snapshots") || word_is(q, n, "restore") || is(q, n, "restore")) { snap_command(q, n); return; }
    if (is(q, n, "rm")) { jt_command(q + 3, n - 3, 1); return; }
    if (is(q, n, "mkdir")) { jt_command(q + 6, n - 6, 2); return; }
    if (word_is(q, n, "reboot") || word_is(q, n, "restart")) { power_run(0); return; }     /* typing it is the confirmation the chord asks for */
    if (word_is(q, n, "shutdown") || word_is(q, n, "poweroff")) { power_run(1); return; }
    if (bt_command(q, n)) return;
    { int gpu_command(const char *q, unsigned n); if (gpu_command(q, n)) return; }
    { int heat_command(const char *q, unsigned n); if (heat_command(q, n)) return; }   /* 13.3.0: heat.c, how hot the chip is */   /* 13.2.0: gpu.c, the V3D brought up by hand */
    if (beep_command(q, n)) return;
    if (mic_command(q, n)) return;
    if (vol_command(q, n)) return;
    if (word_is(q, n, "debug") || is(q, n, "debug")) {   /* 7.2.0: debug NAME [STEPS], userrun with its first steps shown (main.c dbg_step) */
        const char *name = q + 5; unsigned m = n > 5 ? n - 5 : 0, nm = 0, steps = 0;
        while (m && name[0] == ' ') { name++; m--; }
        while (nm < m && name[nm] != ' ') nm++;
        for (unsigned i = nm; i < m; i++) if (name[i] >= '0' && name[i] <= '9' && steps < 1000) steps = steps * 10 + (unsigned)(name[i] - '0');
        if (!nm) { cmd_out("debug NAME [STEPS]: run an app one instruction at a time, 12 steps unless you say, 64 at most\n"); return; }
        dbg_want = !steps ? 12 : steps > 64 ? 64 : steps;
        userrun_command(name, nm);
        dbg_want = 0;   /* taken by app_start, or never: a refused run leaves nothing for the next app */
        return;
    }
    if (n > 8 && is(q, n, "userrun")) {   /* 8.3.0: userrun A [WORDS] | userrun B [WORDS], A's output is B's input */
        unsigned bar = 0;
        while (bar < n && q[bar] != '|') bar++;
        if (bar < n) {
            const char *a = q + 8, *b = q + bar + 1; unsigned am = bar - 8, bm = n - bar - 1;
            while (bm && b[0] == ' ') { b++; bm--; }
            if (bm > 8 && b[0] == 'u' && b[1] == 's' && b[2] == 'e' && b[3] == 'r' && b[4] == 'r' && b[5] == 'u' && b[6] == 'n' && b[7] == ' ') { b += 8; bm -= 8; }
            while (am && a[0] == ' ') { a++; am--; }
            while (am && a[am - 1] == ' ') am--;
            while (bm && b[0] == ' ') { b++; bm--; }
            unsigned an = 0, bn = 0, aw, bw;
            while (an < am && a[an] != ' ') an++;
            while (bn < bm && b[bn] != ' ') bn++;
            for (aw = an; aw < am && a[aw] == ' '; aw++) { }
            for (bw = bn; bw < bm && b[bw] == ' '; bw++) { }
            if (!an || !bn) { cmd_out("userrun A | userrun B: A's output becomes B's input\n"); return; }
            int userrun_pipe(const char *, unsigned, const char *, unsigned, const char *, unsigned, const char *, unsigned, int);
            int r = userrun_pipe(a, an, a + aw, am - aw, b, bn, b + bw, bm - bw, !accounts_count() || sudo_run);
            if (r == -3) cmd_out("userrun: a pipe or every app slot is busy, wait for it to end\n");
            else if (r == -1 || r == -11) cmd_out("userrun: no program called that on one side of the pipe\n");
            else if (r == -4 || r == -5 || r == -14 || r == -15) cmd_out("userrun: one side of the pipe is not signed, or its signature does not match: sudo userrun\n");
            else if (r) cmd_out("userrun: one side of the pipe would not load\n");
            else cmd_out("userrun: both running, the first one's output goes to the second\n");
            return;
        }
    }
    if (n > 8 && q[0] == 'u' && q[1] == 's' && q[2] == 'e' && q[3] == 'r' && q[4] == 'r' && q[5] == 'u' && q[6] == 'n' && q[7] == ' ') {
        const char *name = q + 8; unsigned m = n - 8;
        while (m && name[0] == ' ') { name++; m--; }
        unsigned nm = 0;   /* 6.69.0: userrun NAME WORDS, the words for the app (JT_SYS_ARGS) */
        while (nm < m && name[nm] != ' ') nm++;
        unsigned at = nm;
        while (at < m && name[at] == ' ') at++;
        ufile_args_next(name + at, m - at);
        if (nm) userrun_command(name, nm);
        ufile_args_next("", 0);   /* taken by app_start, or never: a refused run leaves nothing for the next app */
        if (nm) return;
    }
    if (word_is(q, n, "update") || word_is(q, n, "update install")) {   /* 9.3.0: self-update (update.c); installing is an admin's */
        void update_check(void), update_install(void);
        if (n == 6) update_check(); else if (priv("update install")) update_install();
        return;
    }
    if (n > 8 && is(q, n, "install")) {   /* 6.55.0: an app from the store into JT/APPS, signed or not at all (store.c) */
        const char *name = q + 8; unsigned m = n - 8;
        while (m && name[0] == ' ') { name++; m--; }
        while (m && name[m - 1] == ' ') m--;
        if (priv("install")) store_install(name, m);
        return;
    }
    if (ask_command(q, n)) return;
    if (browse_command(q, n)) return;
    if (mail_command(q, n)) return;   /* 6.18.0 */
    if (n == 4 && q[0] == 'w' && q[1] == 'i' && q[2] == 'f' && q[3] == 'i') {   /* 6.22.0: wifi, what the Pi joins and hears */
        const char *name[16]; int rssi[16]; unsigned k = wifi_networks(name, rssi, 16);
        const char *net = wifi_network();
        cmd_out("wifi: "); cmd_out(net[0] ? "joins " : "no network set"); if (net[0]) cmd_out(net); cmd_out(wifi_nic_up() ? ", connected\n" : "\n");
        if (!k) cmd_out("wifi: no networks heard (none in range, or no Wi-Fi chip on this machine)\n");
        for (unsigned i = 0; i < k; i++) { cmd_out("  "); cmd_out(name[i]); cmd_out(rssi[i] >= -60 ? "  strong\n" : rssi[i] >= -72 ? "  okay\n" : "  weak\n"); }
        cmd_out("wifi join NAME picks one\n");
        return;
    }
    if (n > 10 && q[0] == 'w' && q[1] == 'i' && q[2] == 'f' && q[3] == 'i' && q[4] == ' ' && q[5] == 'j' && q[6] == 'o' && q[7] == 'i' && q[8] == 'n' && q[9] == ' ') {
        unsigned s = 10; while (s < n && q[s] == ' ') s++;
        if (n - s > 32 || s == n) { cmd_out("wifi: a network name is 1 to 32 characters\n"); return; }
        pw_clear();
        for (unsigned i = s; i < n; i++) pw_cmd[i - s] = q[i];
        pw_cmd[n - s] = 0;
        pw_ask(PW_WIFI, "Wi-Fi password for ", pw_cmd);
        return;
    }
    if (ics_command(q, n)) return;    /* 6.19.0 */
    if (nimble_command(q, n)) return;
    if (is(q, n, "llm")) { ask_claude(q, n); return; }
    not_found(q, n);
}
