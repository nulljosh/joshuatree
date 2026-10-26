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
#include "login.h"
#include "home.h"
#include "snap.h"
#include "../../drivers/fat.h"
void kputs(const char *s);   /* main.c: the UART and the log of the window in front */
void say_wrapped(const char *prefix, const char *s, unsigned n);   /* ask.c */
int browse_command(const char *q, unsigned n);                     /* browser.c */
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
static int priv(const char *what);   /* below: 1 if a privileged command may go on */
void cmd_dec(unsigned v) { char t[11]; unsigned n = 10; t[n] = 0; do t[--n] = (char)('0' + v % 10); while (v /= 10); cmd_out(t + n); }

void power_run(int off);                                   /* main.c: the confirmed power action, refused while Samantha answers */
static const char help[] = "browse URL (a bare host is https), open N, back, forward, reload, links, more, up, down, "
                           "top, bottom, find WORD, beep [FREQ MS], mic [SECONDS], double-tap and hold Space (speak to Samantha), vol [0-100|up|down], llm PROMPT, bt, bt scan, bt pair, userrun NAME, "
                           "ls [DIR], cat NAME, write NAME.EXT TEXT, rm NAME, mkdir NAME (the last three in JT/, admins only; ~ is your own home folder, JT/HOME/YOU, open to you alone: write ~/NOTE.TXT TEXT, cat ~/NOTE.TXT, ls ~), users, admins, adduser NAME PASSWORD, deluser NAME PASSWORD (the password is never shown), "
                           "sudo COMMAND (an admin's password for five minutes, sudo -k forgets it), passwd [NAME], makeadmin NAME, unadmin NAME, logout, snapshot (copy your home folder, the last three are kept), snapshots, restore N yes (put it back as that snapshot was), boottime (when the first key came and what held the keys up), sam QUESTION (ask Samantha), reboot, shutdown, help.";
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
void logout_request(void);   /* main.c: the login screen comes back on the next pass of the main loop */
void cmd_run(const char *q, unsigned n, void (*out)(const char *));
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
void cmd_password_cancel(void) { pw_clear(); }   /* ask.c: Esc at the prompt, or a sign-out */
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
static int passwd_apply(const char *target, const char *pass) {
    cmd_out("passwd: working, the password is being stretched\n");
    int r = accounts_passwd(target, pass);
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
    static const char *const names[] = {"browse", "open", "back", "forward", "reload", "llm", "bt", "userrun"};
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
    int v;
    if (!num(w1, slen_(w1), &v)) { cmd_out("restore: use restore N yes (N from snapshots)\n"); return; }
    if (!(w2[0] == 'y' && w2[1] == 'e' && w2[2] == 's' && !w2[3])) {
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
    if (word_is(q, n, "users")) { users_command(); return; }
    if (word_is(q, n, "admins")) { admins_command(); return; }
    if (word_is(q, n, "sudo")) { sudo_command(q, 0); return; }
    if (is(q, n, "sudo")) { sudo_command(q + 5, n - 5); return; }
    if (word_is(q, n, "passwd") || is(q, n, "passwd")) { passwd_command(q + 6 + (n > 6), n > 6 ? n - 7 : 0); return; }
    if (word_is(q, n, "makeadmin") || is(q, n, "makeadmin")) { admin_command(q + 9 + (n > 9), n > 9 ? n - 10 : 0, 1); return; }
    if (word_is(q, n, "unadmin") || is(q, n, "unadmin")) { admin_command(q + 7 + (n > 7), n > 7 ? n - 8 : 0, 0); return; }
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
    if (beep_command(q, n)) return;
    if (mic_command(q, n)) return;
    if (vol_command(q, n)) return;
    if (n > 8 && q[0] == 'u' && q[1] == 's' && q[2] == 'e' && q[3] == 'r' && q[4] == 'r' && q[5] == 'u' && q[6] == 'n' && q[7] == ' ') {
        const char *name = q + 8; unsigned m = n - 8;
        while (m && name[0] == ' ') { name++; m--; }
        if (m) { userrun_command(name, m); return; }
    }
    if (ask_command(q, n)) return;
    if (browse_command(q, n)) return;
    if (is(q, n, "llm")) { ask_claude(q, n); return; }
    not_found(q, n);
}
