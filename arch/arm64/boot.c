/* boot.c: cut out of main.c (4.23.3, the god-file split). Input from every driver (Linux evdev events), key repeat,
   and main(): the boot sequence and the poll loop. Built as part of main.o (the Makefile's MAINSRC), with main.c's
   flags; what it shares with main.c is declared in kmain.h. */
#include "kmain.h"
/* ---- Input, from any driver: Linux evdev events (virtio input speaks them natively; the USB HID driver in xhci.c
   translates its reports into them). The pointer starts mid-screen; a tablet sets it, a mouse moves it. ---- */
#include "virtio.h"   /* struct input_event, and the virtio devices (moved to virtio.c in 4.23.2) */
int ask_key(unsigned code, unsigned value);   /* ask.c: the prompt's line editor */
int ask_secret(void);                         /* ask.c: the line being typed holds a password */
int ask_char(unsigned code);                  /* ask.c: a key code as the character it types, Shift included */
void ask_poll(void);
#ifdef PI_BUILD
#include "screen_idle.h"
#endif
#include "login_ui.h"   /* 4.9.0: the login screen, over everything while JT/USERS.TXT has an account and nobody has signed in */
/* Key repeat. A USB keyboard (SET_IDLE(0)) reports only a change, so a held key sends one press and then nothing.
   The last typed key down (a typed character or Backspace; not a function key, Brick's keys, a modifier or a chord)
   is remembered with the generic counter's time. After KEY_REPEAT_DELAY_MS the main loop feeds it back through
   input_event as value 2, which every handler treats as a press, so it lands wherever the first press went (Terminal,
   Calculator, Spotlight), every KEY_REPEAT_RATE_MS, until its release or any other key or button goes down.
   The counter, never `ticks`, which stops counting. */
#define KEY_REPEAT_DELAY_MS 400
#define KEY_REPEAT_RATE_MS 80
static unsigned rep_code;            /* the key being repeated, 0 for none */
static unsigned long rep_next;       /* when it repeats next, in counter milliseconds */
static unsigned long key_ms(void) {
    unsigned long f, c; __asm__ volatile ("mrs %0, cntfrq_el0\n mrs %1, cntpct_el0" : "=r"(f), "=r"(c));
    return c / ((f ? f : 54000000) / 1000);
}
static void app_key(unsigned code) {   /* a key down for the app in front, as the codes user/jtsys.h gives every program */
    static const unsigned short map[][2] = { {1, 259}, {28, 258}, {96, 258}, {14, 8}, {15, 9}, {103, 256}, {108, 257},
                                             {105, 261}, {106, 262}, {102, 305}, {107, 306}, {111, 307} };
    int k = 0;
    for (unsigned i = 0; i < sizeof map / sizeof map[0]; i++) if (map[i][0] == code) k = map[i][1];
    if (!k && (ctrl_held || gui_held)) k = code == 46 ? 302 : code == 45 ? 303 : code == 47 ? 304 : code == 31 ? 308 : code == 30 ? 315 : code == 49 ? 316 : code == 44 ? 317 : 0;   /* Ctrl or Cmd with C X V S A N Z */
    else if (!k) k = ask_char(code);
    if (k) app_push(1, k, 0);
}
/* The Pi took no keys for its first few seconds on the desktop (roadmap). These stamps find out why on the board: when the
   first key arrived after the desktop, and any step of the main loop that held the keys up for 100 ms or more in the first
   half minute. They go to the UART and to `boottime` in the Terminal, never the Console, whose picture checks hold still.
   The login screen's own wait for a wrong password is not counted. */
static unsigned long desk_ms;   /* key_ms() as the main loop starts */
static char stamps[512];
static unsigned stamps_n;
const char *boot_stamps(void) { return stamps; }   /* cmd.c: `boottime` */
static void stamp(const char *s) { con_quiet = 1; uart_puts(s); while (*s && stamps_n < sizeof stamps - 1) stamps[stamps_n++] = *s++; }
static void stamp_dec(unsigned long v) { char b[24]; int k = 23; b[k] = 0; do b[--k] = (char)('0' + v % 10); while ((v /= 10) && k); stamp(b + k); }
#define IDLE_SYNC_CS 6000u   /* 6.87.0: a minute with no key, click or move */
static unsigned input_last;      /* notice_now() at the last of them */
static void idle_tick(void) {   /* the work that waits for nobody to be using the Pi: CalDAV's sync (ics.c) */
    if (notice_now() - input_last >= IDLE_SYNC_CS && !apps_live() && !audio_busy() && !ask_pending() && !bt_settling()) ics_idle_sync();
}
static void pass_note(const char *what, unsigned long t0, int login) {
    unsigned long ms = key_ms() - t0;
    if (ms < 100 || !desk_ms || login || login_live || t0 - desk_ms > 30000) return;   /* later, a `sam` or a heavy app is slow on purpose */
    stamp("slow step "); stamp(what); stamp(" "); stamp_dec(ms); stamp(" ms, "); stamp_dec(t0 - desk_ms); stamp(" ms after the desktop\n");
}
#define TIMED(f) do { unsigned long t_ = key_ms(); int l_ = login_live; f; pass_note(#f, t_, l_); } while (0)
void input_event(struct input_event e) {
    static int first_key;
    if (e.type == 1 && e.value == 1 && desk_ms && !first_key && !login_live) {
        first_key = 1; stamp("first key "); stamp_dec(key_ms() - desk_ms); stamp(" ms after the desktop\n");
    }
    if (e.type == 1 || e.type == 3 || (e.type == 2 && e.value)) input_last = notice_now();   /* 6.87.0: for idle_tick */
#ifdef PI_BUILD
    if (e.type == 1 || e.type == 3 || (e.type == 2 && e.value)) screen_activity();   /* any key or button or a move wakes the screen */
#endif
    if (e.type == 1 && e.value < 2) {                                                                      /* a real press or release; value 2 is our own repeat */
        if (!e.value) { if (e.code == rep_code) rep_code = 0; }
        else if ((e.code == 14 || (e.code < 58 && ask_char(e.code))) && !(brick_live && !spot_live) && !ctrl_held && !gui_held && !alt_held) {
            rep_code = e.code; rep_next = key_ms() + KEY_REPEAT_DELAY_MS;
        } else rep_code = 0;
    }
    if (login_live) { login_event(e); return; }                                                            /* the login screen has every key; the mouse is dropped */
    if (e.type == 1 && e.code >= 272 && e.code <= 274) {                                                 /* BTN_LEFT, RIGHT, MIDDLE */
        con_quiet = 1;   /* the UART only */
        uart_puts("key "); uart_dec(e.code); uart_puts(e.value ? " down\n" : " up\n");
        if (e.code == 272 && e.value) pointer_click();
        else if (e.code == 272) drag_end();
        return;
    }
    if (e.type == 1 && (e.code == 29 || e.code == 97)) ctrl_held = e.value != 0;                          /* Ctrl, either side; the key still echoes below */
    if (e.type == 1 && (e.code == 125 || e.code == 126)) gui_held = e.value != 0;   /* Cmd (left 0x08, right 0x80 in a boot report) */
    if (e.type == 1 && (e.code == 56 || e.code == 100)) alt_held = e.value != 0;    /* Alt, Option on a Mac board */
    if (e.type == 1 && e.value == 1) kd_key(e.code);                                                       /* not our repeats: the debug line shows the real press */
    if (e.type == 1 && (e.code == 42 || e.code == 54)) ask_key(e.code, e.value);   /* Shift, before ui_key: Spotlight and the Calculator read its state from ask.c too, and take every key */
    if (e.type == 1 && e.code == 58) {                                                                     /* Caps Lock: flips ask.c's state, nothing else sees it */
        ask_key(e.code, e.value);
        con_quiet = 1;
        uart_puts("key 58 "); uart_puts(e.value ? "down\n" : "up\n");
        return;
    }
    if (e.type == 1 && e.value && ui_key(e.code)) {                                                      /* Spotlight, the dock keys, F2 and Ctrl+T */
        con_quiet = 1;
        if (!set_secret()) { uart_puts("key "); uart_dec(e.code); uart_puts(" down\n"); }   /* 4.14.0: not while Settings' Users pane takes a password */
        return;
    }
    if (e.type == 1 && brick_live && !spot_live) { brick_key(e.code, e.value); return; }
    if (e.type == 1 && app_front && e.code != 42 && e.code != 54) {   /* the app window in front: its keys go to its queue (Shift still to ask.c) */
        if (e.value) { con_quiet = 1; uart_puts("key "); uart_dec(e.code); uart_puts(" down\n"); app_key(e.code); }
        return;
    }
    if (e.type == 1 && e.value && calc_front && e.code != 42 && e.code != 54) {                           /* a key for the Calculator in front (Shift still goes to ask.c) */
        con_quiet = 1;
        uart_puts("key "); uart_dec(e.code); uart_puts(" down\n");
        if (e.code == 1) { calc_close(); return; }                                                          /* Esc */
        int ch = e.code == 15 ? '\t' : e.code == 14 ? '\b' : e.code == 28 || e.code == 96 ? '\n' : ask_char(e.code);
        if (ch) calc_did(calc_type(ch));
        return;
    }
    if (e.type == 1 && calc_front && e.code != 42 && e.code != 54) return;                                 /* its key ups */
    if (e.type == 1) {                                                                                     /* EV_KEY: keys */
        int held = cur_hold(), term = term_front();
        if (!con_key(e.code, e.value)) {   /* the scroll keys are not logged: that would add lines to the picture they move */
            /* Only the Terminal takes typing; Shift always reaches ask.c, as the Calculator reads its state there */
            if (term || e.code == 42 || e.code == 54) ask_key(e.code, e.value);
            con_quiet = 1;   /* typed keys show on the ask> row, or nowhere; the echo stays on the UART and off the Console */
            if (!(term && ask_secret()) && !set_secret()) { uart_puts("key "); uart_dec(e.code); uart_puts(e.value ? " down\n" : " up\n"); }   /* 4.10.0: not while a password is being typed */
        }
        cur_release(held);
    }
    else if (e.type == 3) {                                                                                /* EV_ABS: the tablet, 0..32767 */
        if (e.code == 0) mouse_x = e.value * fb_w / 32768; else if (e.code == 1) mouse_y = e.value * fb_h / 32768;
        mouse_moved = 1;
    } else if (e.type == 2) {                                                                              /* EV_REL: a mouse, clamped to the screen */
        int v = (int)e.value;
        if (e.code == 8) { wheel_acc += v; return; }   /* REL_WHEEL: only the Browser reads it, once per report (the SYN below), so a burst of notches is one scroll and one paint */
        if (e.code == 0) { int x = (int)mouse_x + v; mouse_x = x < 0 ? 0 : x >= (int)fb_w ? (int)fb_w - 1 : (unsigned)x; }
        else if (e.code == 1) { int y = (int)mouse_y + v; mouse_y = y < 0 ? 0 : y >= (int)fb_h ? (int)fb_h - 1 : (unsigned)y; }
        mouse_moved = 1;
    } else if (e.type == 0 && (mouse_moved || wheel_acc)) {                                                /* EV_SYN: one report done */
        if (wheel_acc) { int v = wheel_acc; wheel_acc = 0; browser_wheel(v); }
        if (!mouse_moved) return;
        con_quiet = 1;   /* the UART only: every report would scroll the Console */
        uart_puts("mouse "); uart_dec(mouse_x); uart_putc(','); uart_dec(mouse_y); uart_putc('\n');
        mouse_moved = 0;
        pointer_moved();
    }
}
void kinput(unsigned type, unsigned code, int value) { input_event((struct input_event){ (unsigned short)type, (unsigned short)code, (unsigned)value }); }
static void key_repeat_tick(void) {   /* the main loop, right after the polls: a release that queued while the loop was busy is seen first */
    if (!rep_code || key_ms() < rep_next) return;
    rep_next = key_ms() + KEY_REPEAT_RATE_MS;   /* from now, not from the last due time: a late loop never bursts */
    input_event((struct input_event){ 1, (unsigned short)rep_code, 2 });
}
int usb_init(void);    /* xhci.c */
int sd_mount(void);    /* emmc.c */
int wifi_init(void);
#ifdef PI_BUILD
#define WIFI_ON_PI 1
#else
#define WIFI_ON_PI 0
#endif   /* wifi.c: M4 Wi-Fi stage 1, polled, every wait bounded */
#ifdef WIN_GATE
/* wingate-kernel8.img only (arm64-windows-check.py): three apps booted, slot 1 is Toroid. A second after all three have drawn,
   the backquote it crashes on is queued for it; two seconds later the UART says which slots still have an app and how many
   frames each has presented since, so the check can see the other two kept running. */
static void wingate_tick(void) {
    static unsigned long t0; static int phase; static unsigned p0[APP_SLOTS];
    unsigned long now = cnt_now();
    if (phase == 0) {
        if (!(apps[0].shown && apps[1].shown && apps[2].shown)) return;
        t0 = now; phase = 1;
    } else if (phase == 1 && now > t0 + cnt_hz()) {
        struct app *keep = ap; ap = &apps[1]; app_push(1, '`', 0); ap = keep;
        for (unsigned i = 0; i < APP_SLOTS; i++) p0[i] = apps[i].presents;
        t0 = now; phase = 2;
    } else if (phase == 2 && now > t0 + 3 * cnt_hz()) {
        for (unsigned i = 0; i < 3; i++) { struct app *keep = ap; ap = &apps[i]; if (i != 1) app_push(1, ' ', 0); ap = keep; }   /* poke the survivors */
        t0 = now; phase = 3;
    } else if (phase == 3 && now > t0 + 2 * cnt_hz()) {
        con_quiet = 1; uart_puts("wingate:");
        for (unsigned i = 0; i < 3; i++) { uart_puts(" slot"); uart_dec(i); uart_puts(apps[i].cur ? " live " : " gone "); uart_dec(apps[i].presents - p0[i]); }
        uart_putc('\n'); phase = 4;
    }
}
#endif
#ifdef HANG_TEST
/* hangtest builds only (arm64-watchdog-check.py). Slot 0's Calendar spins on its first instruction; the watchdog must end it
   alone. Once it is gone, slot 1's Notes is poked and counted, and the UART says whether it kept presenting. */
static void hang_tick(void) {
    static unsigned long t0; static int phase; static unsigned p0;
    unsigned long now = cnt_now();
    if (phase == 0) { if (!apps[0].cur && apps[1].cur) { p0 = apps[1].presents; t0 = now; phase = 1; } }
    else if (phase == 1 && now > t0 + cnt_hz()) {
        struct app *keep = ap; ap = &apps[1]; app_push(1, ' ', 0); ap = keep;
        t0 = now; phase = 2;
    } else if (phase == 2 && now > t0 + cnt_hz()) {
        con_quiet = 1; uart_puts("hang: slot0 gone slot1 "); uart_puts(apps[1].cur ? "live " : "gone "); uart_dec(apps[1].presents - p0); uart_putc('\n'); phase = 3;
    }
}
#endif
void usb_poll(void);
#ifdef NOTES_SELFTEST
/* notesw-kernel8.img and notesr-kernel8.img only (arm64-notes-check.py). QEMU's raspi4b has no USB, so made-up boot
   keyboard reports go through xhci.c's hid_kbd, the parser a real keyboard's reports take, one key every 150 ms of the
   generic counter (never `ticks`). Right five times and Enter walk the dock to its fifth tile, Notes, the way a Pi with
   only a keyboard opens it. '|' waits for Notes' first frame. notesw then types n (a new note), a line and Esc (Esc
   saves it and goes back to the list); notesr presses Enter (the note opens in the editor). Then `notestest: done`, and
   Notes stays open for the check's screendump. */
void hid_kbd(const unsigned char *r, unsigned len, unsigned char *p);
static void notes_tick(void) {
    static const char *const script = NOTES_SELFTEST == 1 ? ">>>>>\n|nhello from the pi\033" : NOTES_SELFTEST == 2 ? ">>>>>\n|\n"
        : NOTES_SELFTEST == 3 ? ">>>>>>\n|buy milk\ncall mom\n^^ " : NOTES_SELFTEST == 5 ? ">>>>>\n|nautosaved with no clock!@" : ">>>>>>\n|";   /* 3, 4: Reminders (arm64-reminders-check.py): the sixth tile, two adds, Up Up, Space on the first; then just open it; 5: Notes with no clock set, a line typed and left, '!' waits four seconds, '@' clicks the red dot (arm64-notes-check.py) */
    static unsigned at; static unsigned long next; static unsigned char last[8]; static int done;
    unsigned long now = cnt_now();
    if (!next) next = now + cnt_hz();   /* a second after the loop starts: the desktop has settled */
    if (done || now < next) return;
    if (!script[at]) { done = 1; uart_puts("notestest: done\n"); return; }   /* a second after the last key: it has drawn */
    next = now + cnt_hz() / 1000 * 150;
    char c = script[at];
    if (c == '|') {
        if (!app_cur || !app_shown) return;   /* not up yet: ask again on the next step */
        uart_puts("notestest: notes is up\n");
        at++; next = now + 2 * cnt_hz();   /* two seconds to take a screendump of the list before the next key */
        return;
    }
    if (c == '!') { uart_puts("notestest: waiting\n"); at++; next = now + 4 * cnt_hz(); return; }   /* long enough for the two second autosave to fire, with Esc never pressed */
    if (c == '@') {   /* the red dot of the front app window, through the pointer's own click path */
        int s = (int)window_scale();
        mouse_x = (app_lx + 24) * s; mouse_y = (app_ly + 16) * s;
        uart_puts("notestest: red dot\n"); pointer_click();
        at++; next = now + 2 * cnt_hz(); return;
    }
    unsigned char down[8] = { 0 }, none[8] = { 0 };
    down[2] = c == '>' ? 0x4F : c == '\n' ? 0x28 : c == '\033' ? 0x29 : c == ' ' ? 0x2C : c == '^' ? 0x52 : (unsigned char)(0x04 + c - 'a');   /* HID usages: Right, Enter, Esc, Space, a to z */
    hid_kbd(down, 8, last); hid_kbd(none, 8, last);
    if (!script[++at]) next = now + cnt_hz();
}
#endif


void net_stack_demo(void);
void net_clock_sync(void);
int bt_starting(void);   /* bt.c: the radio is still loading its patch */
void tls_demo(void);
unsigned hear_fake(short *pcm, unsigned max) {   /* ask.c's `hear` on the heartest build: half a second of a 440 Hz square wave instead of a microphone; (0, 0) asks whether this is that build */
#ifdef HEAR_TEST
    if (!pcm) return 1;
    unsigned n = 8000 < max ? 8000 : max;
    for (unsigned i = 0; i < n; i++) pcm[i] = (short)(((i * 440 / 16000) & 1) ? 6000 : -6000);
    return n;
#else
    (void)pcm; (void)max; return 0;
#endif
}
void input_drain(void) { usb_poll(); virtio_input_poll(); key_repeat_tick(); audio_pump(); }   /* ask.c, between agent steps and in every wait while `sam` thinks: the keys that queued, a held key repeating, and the sound */

void main(void) {
    unsigned long el;
    uart_init();
    __asm__ volatile ("mrs %0, CurrentEL" : "=r"(el));
    uart_puts("Joshua Tree on ARM64\n");
    { const char *sp = "ABCDEFGHIJKLMNOPQRSTUVWXYZ\nabcdefghijklmnopqrstuvwxyz\n0123456789 .,;:!?&@#$%\n"; while (*sp) console_putc(*sp++); }   /* the type specimen: screen only, at the top of the log */
    uart_puts("booted at EL"); uart_putc((char)('0' + boot_el)); uart_puts("\n");
    uart_puts(((el >> 2) & 3) == 1 ? "EL1\n" : "not EL1\n");
    uart_puts("M0 ok\n");
    m1_selftest();
    user_demo();
    if (virtio_net_init()) virtio_net_probe();
    if (virtio_blk_init()) virtio_blk_probe();
    net_stack_demo();   /* ip.c: DHCP and an HTTP POST through the shared stack, silent with no card */
    tls_demo();         /* ip.c: the HTTPS proof, only in a TLSPORT= build */
#ifndef QUIET_BOOT
    fb_diag();   /* last, so it is the newest line on the screen */
#endif   /* quiet boot: splash_finish prints it, once text_init has measured the font */
    int inputs = virtio_input_init();
    if (inputs) { uart_puts("M2 input ready, devices "); uart_dec((unsigned)inputs); uart_putc('\n'); }
    int usb_ok = usb_init();   /* USB first: the keyboard is the way in, and Wi-Fi bring-up is a blocking stretch of seconds on the real Pi */
#ifdef USB_AUDIO_TEST   /* the usbaudiotest build: a USB speaker plays 440 Hz for half a second at boot (arm64-usbaudio-check.py) */
    { int usb_spk_tone(int freq, int ms); usb_spk_tone(440, 500); }
#endif
    sd_mount();                /* emmc.c: the SD card's FAT partition for ls and cat; one `sd ...` line, every wait bounded */
    accounts_load();           /* accounts.c: JT/USERS.TXT, the names `users` lists */
    sudo_load();               /* sudo.c: JT/SUDOERS.TXT, who may sudo */
    settings_load();           /* settings.c: JT/SETTINGS.TXT, before the Pi's desktop is first painted (the splash waits) */
    SPLASH(2, "sd");
#ifdef SD_SELFTEST   /* the sd builds (arm64-sd-check.py) run the Terminal's own commands at boot on the check's card */
    {
        void cmd_run(const char *q, unsigned n, void (*out)(const char *));
        int emmc_read(unsigned lba, unsigned count, void *buf); int emmc_write(unsigned lba, unsigned count, const void *buf);
        unsigned long emmc_blocks(void); int blockdev_write_sector(unsigned lba, const void *buf);
        static char chain[1040] = "write CHAIN.TXT ";   /* 1020 digits: two one-sector clusters, linked */
        for (unsigned i = 0; i < 1020; i++) chain[16 + i] = (char)('0' + i % 10);
#if SD_SELFTEST == 1   /* sdtest: ls and cat, then two reads that must be refused */
        static const char *const line[] = { "ls", "cat config.txt", "cat BIG.TXT", "cat BAD.TXT", "cat NOPE.TXT" };
#elif SD_SELFTEST == 2   /* sdwtest: write, replace, delete, a folder, a chain and the refusals, then read it back */
        static const char *const line[] = { "cat JT/NOTE.TXT", "write NOTE.TXT the first line", "write NOTE.TXT it survives a reboot",
                                            "write GONE.TXT bye", "rm GONE.TXT", "mkdir LOGS", chain, "write config.txt hello",
                                            "write kernel8.img x", "write ../config.txt x", "rm start4.elf", "mkdir overlays",
                                            "ls JT", "cat JT/NOTE.TXT", "cat JT/GONE.TXT" };
#else   /* sdrtest: the same card after a reboot */
        static const char *const line[] = { "cat JT/NOTE.TXT", "rm NOTE.TXT", "rm CHAIN.TXT", "cat JT/NOTE.TXT", "ls JT" };
#endif
        for (unsigned i = 0; i < sizeof line / sizeof line[0]; i++) {
            unsigned n = 0; while (line[i][n]) n++;
            uart_puts("sdtest> "); uart_puts(line[i] == chain ? "write CHAIN.TXT (1020 digits)" : line[i]); uart_puts("\n");
            cmd_run(line[i], n, 0);
        }
        static unsigned char blk[4096], back[4096];
        unsigned last = (unsigned)emmc_blocks();
#if SD_SELFTEST == 1
        uart_puts("sdtest> read past the card\n");
        emmc_read(last, 1, blk);
#elif SD_SELFTEST == 2   /* CMD25: eight blocks at the card's very end, past the partition, only on the check's own card */
        uart_puts("sdtest> write past the card, and the boot sector\n");
        emmc_write(last, 1, blk);
        blockdev_write_sector(0, blk);
        uart_puts("sdtest> multi-block write\n");
        if (!emmc_read(2048, 1, blk) || blk[3] != 'J' || blk[4] != 'T' || blk[5] != 'C' || blk[6] != 'H') uart_puts("sdtest not the check's card, no raw write\n");
        else {
            for (unsigned i = 0; i < sizeof blk; i++) blk[i] = (unsigned char)(i * 7 + (i >> 9));
            int ok = emmc_write(last - 8, 8, blk) && emmc_read(last - 8, 8, back);
            for (unsigned i = 0; ok && i < sizeof blk; i++) if (back[i] != blk[i]) ok = 0;
            uart_puts(ok ? "sdtest multi-block write ok\n" : "sdtest multi-block write FAIL\n");
        }
#endif
        (void)last; (void)back;
        uart_puts("sdtest done\n");
    }
#endif
#ifdef BT_NOWIFI   /* the radio test build: Wi-Fi stays off so Bluetooth has the shared antenna to itself */
    menubar_wifi(0);
#else
    if (!wifi_init()) menubar_wifi(0);
#endif
#ifdef PI_BUILD
    if (wifi_nic_up()) { net_stack_demo(); net_clock_sync(); menubar_wifi(2); menubar_notify("Wi-Fi joined"); }   /* the address from the router, then the time */
#endif   /* prints `wifi ...` lines; on QEMU it ends at `wifi no host` and the desktop carries on */
    SPLASH(3, "wifi");
    SPLASH(4, "clock");   /* net_clock_sync ran inside the join above, or there was no network to ask */
    bt_init();   /* bt.c: powers the radio and returns; the main loop's bt_poll does the rest */
#ifdef QUIET_BOOT
    if (splash_on) {   /* the splash waits for the radio to load its patch (about half a minute on a Pi), or to say it will not: 60 s at most */
        unsigned long t0 = key_ms();
        while (bt_starting() && key_ms() - t0 < 60000) { bt_poll(); splash_creep(); }
    }
    SPLASH(5, "bt");
    splash_finish();
#endif
    login_show();   /* login_ui.h: with an account on the card the desktop waits behind the login screen; with none this does nothing */
#ifdef NOTICE_SELFTEST
    menubar_notify("Note kept in memory");
#endif
#if defined(WIFI_ICON_TEST) && WIFI_ICON_TEST == 2
    menubar_wifi(2);   /* a joined fan, which QEMU never reaches on its own */
#endif
#ifdef POWER_SELFTEST
    void hid_kbd(const unsigned char *, unsigned, unsigned char *);
    unsigned char previous[8] = {0};
    const unsigned char chord[8] = { 0x05, 0, POWER_SELFTEST == 1 ? 0x4D : 0x4C };
    const unsigned char none[8] = {0}, escape[8] = { 0, 0, 0x29 }, enter[8] = { 0, 0, 0x28 };
    hid_kbd(chord, 8, previous); hid_kbd(none, 8, previous);
    hid_kbd(escape, 8, previous); hid_kbd(none, 8, previous);
    uart_puts("power: cancellation survived\n");
    hid_kbd(chord, 8, previous); hid_kbd(none, 8, previous);
    hid_kbd(enter, 8, previous);
#endif
#ifdef POWER_FILE_TEST   /* tools/checks/arm64-power-check.py: boot 1 leaves a file unsaved and types `reboot`; boot 2 reads it back and types `shutdown` */
    {
        void cmd_run(const char *q, unsigned n, void (*out)(const char *));
        void ufile_selftest_dirty(const char *name, const char *text);
        cmd_run("cat JT/PWRBOOT.TXT", 18, grab);
        if (!has("boot-one")) {
            cmd_run("write PWRBOOT.TXT boot-one", 26, 0);
            ufile_selftest_dirty("PWR.TXT", "kept-by-flush");
            uart_puts("power: file left unsaved, typing reboot\n");
            cmd_run("reboot", 6, 0);
        } else {
            seen[0] = 0; cmd_run("cat JT/PWR.TXT", 15, grab);
            uart_puts(has("kept-by-flush") ? "power: file survived the reboot\n" : "power: file LOST in the reboot\n");
            cmd_run("shutdown", 8, 0);
        }
    }
#endif
#ifdef TREEMENU_SELFTEST   /* 1 restart, 2 shut down: the tree clicked open and driven by the board's own key events, then the power prompt confirmed */
    {
        int px = sg(12), py = sg(MENUBAR_H) / 2;
        kinput(3, 0, px * 32768 / (int)fb_w + 1); kinput(3, 1, py * 32768 / (int)fb_h + 1); kinput(0, 0, 0);
        kinput(1, 272, 1); kinput(1, 272, 0);
        kinput(1, 1, 1);   /* Esc closes the card */
        uart_puts(spot_live ? "treemenu: esc FAILED\n" : "treemenu: esc survived\n");
        kinput(1, 68, 1);   /* F10 opens it again */
        kinput(1, 108, 1); if (TREEMENU_SELFTEST == 2) kinput(1, 108, 1);
        kinput(1, 28, 1);   /* Enter on Restart or Shut Down: the Terminal's prompt */
        kinput(1, 28, 1);   /* Enter confirms: the watchdog */
    }
#endif
#ifdef KEY_SELFTEST
    key_selftest();
#endif
#ifdef PI_BUILD
    screen_last = notice_now();
#ifdef SCREEN_SELFTEST
    screen_last -= SCREEN_IDLE_CS; screen_tick();
    kinput(1, 42, 1); kinput(1, 42, 0);
    screen_last -= SCREEN_IDLE_CS; screen_tick();
    kinput(2, 0, 1); kinput(0, 0, 0);
    uart_puts("screen: input selftest done\n");
#endif
#endif
#if defined(NOTES_SELFTEST) || defined(APP_BOOT)
    usb_ok |= !inputs;   /* QEMU's raspi4b has no USB to find: the loop runs anyway, for notes_tick's made-up keys */
#endif
    if (usb_ok || inputs) {
        /* USB is polled, so nothing interrupts on its own: the virtual timer (INTID 27) wakes wfi every 2 ms. IRQs stay, so nothing interrupts on its own: the virtual timer (INTID 27) wakes wfi every 2 ms. IRQs stay
           masked around wfi (a pending one still wakes it) and the timer is stopped before they are let through again,
           so its handler never runs; the short unmask is for the virtio devices, whose handler acknowledges them. */
        *(volatile unsigned char *)(GICD_BASE + 0x400 + 27) = 0x80;
        GICD(0x100) = 1u << 27;
        unsigned long step = timer_step / 25;   /* timer_step is 50 ms */
        desk_ms = key_ms();
#ifdef PI_BUILD
        (void)step;
        for (;;) {   /* nothing on the Pi sleeps: no wake source to trust yet, so spin and poll */
            TIMED(usb_poll()); TIMED(virtio_input_poll()); TIMED(key_repeat_tick()); TIMED(ask_poll()); TIMED(bt_poll()); TIMED(brick_tick()); TIMED(menubar_tick()); TIMED(keydbg_tick()); TIMED(app_tick()); TIMED(dock_dots_tick()); TIMED(traffic_tick()); TIMED(audio_pump()); TIMED(screen_tick()); TIMED(login_tick()); browser_tick(); idle_tick();   /* only the display sleeps: USB stays polled */
#ifdef NOTES_SELFTEST
            notes_tick();
#endif
#ifdef WIN_GATE
            wingate_tick();
#endif
#ifdef HANG_TEST
            hang_tick();
#endif
        }
#else
        for (;;) {   /* ask_pending: a `sam` queued by Enter is run by the next pass, and a wfi with no timer would sleep through it (the key-up IRQ was already taken) */
            if (usb_ok || brick_live || notice_up() || rep_code || bt_busy() || apps_live() || audio_busy() || ask_pending() || browser_busy()) __asm__ volatile ("msr daifset, #2\n msr cntv_tval_el0, %0\n msr cntv_ctl_el0, %1\n isb\n wfi\n"
                              " msr cntv_ctl_el0, xzr\n isb\n msr daifclr, #2\n isb" :: "r"(step), "r"(1UL) : "memory");
            else __asm__ volatile ("msr daifset, #2\n msr cntv_tval_el0, %0\n msr cntv_ctl_el0, %1\n isb\n wfi\n"
                                   " msr cntv_ctl_el0, xzr\n isb\n msr daifclr, #2\n isb" :: "r"(step * 500), "r"(1UL) : "memory");   /* 6.87.0: idle, still a pass a second, so idle_tick runs */
            TIMED(usb_poll()); TIMED(virtio_input_poll()); TIMED(key_repeat_tick()); TIMED(ask_poll()); TIMED(bt_poll()); TIMED(brick_tick()); TIMED(menubar_tick()); TIMED(keydbg_tick()); TIMED(app_tick()); TIMED(dock_dots_tick()); TIMED(traffic_tick()); TIMED(login_tick()); browser_tick(); idle_tick();   /* an app parked on yield runs again here; browser_tick fetches an image and idle_tick may sync, slow on purpose, so untimed */
#ifdef HANG_TEST
            hang_tick();
#endif
            audio_pump();   /* M3e: the app's sound, from its ring into the PWM's, never waiting */
        }
#endif
    }
    for (;;) __asm__ volatile ("wfe");
}

