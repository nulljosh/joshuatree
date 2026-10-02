/* 1.7.7: Keyrate as a real ring-3 process, and the supervisor around it.
   1.7.11: Toroid joins it, and the launcher became one table (RING3_APPS).
   1.7.12: Calculator joins it, the third app out.
   1.7.14: Quotes joins it, the fourth app out.
   2.0: Bookrank joins it, the fifth app out.
   1.8.22: Homeqi joins it, the sixth app out.
   1.9.1: Lexly joins it, the seventh app out.
   1.9.2: Plan joins it, the eighth app out.
   1.9.3: Fieldbook joins it, the ninth app out.
   1.9.4: Clock joins it, the tenth app out.
   1.9.5: Portfolio joins it, the eleventh app out.
   1.9.6: Activity joins it, the twelfth app out.
   1.9.7: Contacts joins it, the thirteenth app out.
   1.9.8: Sparkjar joins it, the fourteenth app out.
   1.9.9: Reminders joins it, the fifteenth app out.
   1.9.11: Curbfind joins it, the sixteenth app out, with one new syscall (SYS_HTTP_GET).
   1.9.12: Calendar joins it, the seventeenth app out.
   1.9.13: Search joins it, the eighteenth app out, with one new syscall (SYS_READDIR).
   1.9.19: Epiphany joins it, the nineteenth app out, on SYS_HTTP_GET like Curbfind.

   Roadmap 2.0 says apps leave the kernel, so a crash in one cannot take
   the machine down. Keyrate, the smallest real app, went first; Toroid,
   Calculator, Quotes, Bookrank, Homeqi, Lexly, Plan, Fieldbook, Clock, Portfolio, Activity, Contacts, Sparkjar, Reminders, Curbfind, Calendar and Search followed the same path. Each app's dock entry lands here,
   and everything else stays as it was: gui_launch_from_dock has already
   drawn the window chrome and set the viewport by the time this runs,
   just as for an in-kernel app.

   What "protected" means here, concretely: the program runs at CPL 3 with
   its own page directory and IOPL 0, reaches the kernel only through int
   0x80, and draws only into the framebuffer SYS_WINDOW_OPEN handed it. A
   bad pointer, a divide by zero, a privileged instruction: idt.c's
   ring-3 path reaps the task and the kernel keeps running. task_exit_with
   calls syscall_release_task, which drops the window, so by the time
   exec_user returns here the program is gone and its window with it,
   whichever way it ended. This function then logs what happened and
   returns to the desktop, which repaints. No panic, no reboot.

   Each binary rides along in kernel.elf (drivers/user_<app>.h, generated
   by the Makefile from the real built user/<app>.bin) and is seeded onto
   the VFS on first launch, the same way `usertest` seeds HELLO.BIN: the
   headless checks and the browser demo have no disk. */
#include "ring3app.h"
#include "irqlock.h"
#include "exec.h"
#include "syscall.h"
#include "task.h"
#include "vfs.h"
#include "serial.h"
#include "user_keyrate.h"
#include "user_toroid.h"
#include "user_calculator.h"
#include "user_quotes.h"
#include "user_bookrank.h"
#include "user_homeqi.h"
#include "user_lexly.h"
#include "user_plan.h"
#include "user_fieldbook.h"
#include "user_clock.h"
#include "user_portfolio.h"
#include "user_activity.h"
#include "user_contacts.h"
#include "user_sparkjar.h"
#include "user_reminders.h"
#include "user_curbfind.h"
#include "user_calendar.h"
#include "user_search.h"
#include "user_epiphany.h"
#include "user_weather.h"
#include "user_stocks.h"
#include "user_burrow.h"
#include "user_mail.h"
#include "user_notes.h"
#include "user_terminal.h"
#include "user_samantha.h"
#include "user_fbpoke.h"
#include "user_brkpoke.h"
#include "pmm.h"
#include "brk.h"
#include "app.h"
#include "window.h"
#include "irq.h"
#include "mouse.h"

static void put_dec(char *out, int v) {
    char tmp[12]; int i = 0, n = 0;
    unsigned u = v < 0 ? (unsigned)(-v) : (unsigned)v;
    if (v < 0) out[n++] = '-';
    do { tmp[i++] = (char)('0' + u % 10); u /= 10; } while (u);
    while (i) out[n++] = tmp[--i];
    out[n] = 0;
}

static const char *EXC_SHORT[32] = {
    "divide-by-zero", "debug", "NMI", "breakpoint", "overflow", "bound-range", "invalid-opcode",
    "device-not-available", "double-fault", "coprocessor-overrun", "invalid-TSS", "segment-not-present",
    "stack-fault", "general-protection", "page-fault", "reserved", "x87-fp", "alignment-check",
    "machine-check", "SIMD-fp", "virtualization", "control-protection", "reserved", "reserved",
    "reserved", "reserved", "reserved", "reserved", "reserved", "reserved", "reserved", "reserved",
};

int gui_app_view_size(unsigned int *w, unsigned int *h);

/* 1.7.11: one launcher for every app that runs at ring 3. A row is the
   app's name, the binary embedded in kernel.elf (drivers/user_<app>.h,
   generated from the real built user/<app>.bin) and the VFS filename it
   is seeded under on first launch. APPS[] in kernel.c points each ring-3
   app's `open` at the small wrapper below its row. */
struct ring3_app { const char *name; const unsigned char *bin; unsigned int len; const char *file; };
static const struct ring3_app RING3_APPS[] = {
    {"Keyrate",    user_keyrate,    USER_KEYRATE_LEN,    "KEYRATE.BIN"},
    {"Toroid",     user_toroid,     USER_TOROID_LEN,     "TOROID.BIN"},
    {"Calculator", user_calculator, USER_CALCULATOR_LEN, "CALC.BIN"},
    {"Quotes",     user_quotes,     USER_QUOTES_LEN,     "QUOTES.BIN"},
    {"Bookrank",   user_bookrank,   USER_BOOKRANK_LEN,   "BOOKRANK.BIN"},
    {"Homeqi",     user_homeqi,     USER_HOMEQI_LEN,     "HOMEQI.BIN"},
    {"Lexly",      user_lexly,      USER_LEXLY_LEN,      "LEXLY.BIN"},
    {"Plan",       user_plan,       USER_PLAN_LEN,       "PLAN.BIN"},
    {"Fieldbook",  user_fieldbook,  USER_FIELDBOOK_LEN,  "FIELDBOOK.BIN"},
    {"Clock",      user_clock,      USER_CLOCK_LEN,      "CLOCK.BIN"},
    {"Portfolio",  user_portfolio,  USER_PORTFOLIO_LEN,  "PORTFOLIO.BIN"},
    {"Activity",   user_activity,   USER_ACTIVITY_LEN,   "ACTIVITY.BIN"},
    {"Contacts",   user_contacts,   USER_CONTACTS_LEN,   "CONTACTS.BIN"},
    {"Sparkjar",   user_sparkjar,   USER_SPARKJAR_LEN,   "SPARKJAR.BIN"},
    {"Reminders",  user_reminders,  USER_REMINDERS_LEN,  "REMINDERS.BIN"},
    {"Curbfind",   user_curbfind,   USER_CURBFIND_LEN,   "CURBFIND.BIN"},
    {"Calendar",   user_calendar,   USER_CALENDAR_LEN,   "CALENDAR.BIN"},
    {"Search",     user_search,     USER_SEARCH_LEN,     "SEARCH.BIN"},
    {"Epiphany",   user_epiphany,   USER_EPIPHANY_LEN,   "EPIPHANY.BIN"},
    {"Weather",    user_weather,    USER_WEATHER_LEN,    "WEATHER.BIN"},
    {"Burrow",     user_burrow,     USER_BURROW_LEN,     "BURROW.BIN"},
    {"Stocks",     user_stocks,     USER_STOCKS_LEN,     "STOCKS.BIN"},
    {"Mail",       user_mail,       USER_MAIL_LEN,       "MAIL.BIN"},
    {"Notes",      user_notes,      USER_NOTES_LEN,      "NOTES.BIN"},
    {"Terminal",   user_terminal,   USER_TERMINAL_LEN,   "TERMINAL.BIN"},
    {"Samantha",   user_samantha,   USER_SAMANTHA_LEN,   "SAMANTHA.BIN"}, /* slice 2: windowable only under the ring3samantha cmdline flag; the dock still opens the kernel chat */
};
/* 1.9.26: set by the ring3samantha cmdline word (ring3samantha_cmdline, called from kmain). */
static int ring3samantha_on = 0;
void ring3samantha_cmdline(const char *cl) {
    for (const char *p = cl; p && *p; p++) {
        const char *k = "ring3samantha", *q = p;
        while (*k && *q == *k) { q++; k++; }
        if (!*k && (p == cl || p[-1] == ' ') && (*q == 0 || *q == ' ')) { ring3samantha_on = 1; serial_puts("ring3app: samantha at ring 3 (ring3samantha)\n"); return; }
    }
}
static int ring3app_is_samantha(const char *n) { const char *k = "Samantha"; while (*k && *n == *k) { n++; k++; } return !*k && !*n; }

static int ring3app_seed(const struct ring3_app *a) {
    unsigned char probe[1];
    if (vfs_read_file(a->file, probe, 1) < 0 &&
        !vfs_write_file(a->file, a->bin, a->len)) {
        /* The root filesystem holds a handful of files. Once earlier ring-3
           apps have each left their binary behind (and the user has saved a
           few files), there is no slot for this one. The other apps' copies
           are re-seeded on their next launch, so evict them and retry once. */
        for (unsigned int i = 0; i < sizeof RING3_APPS / sizeof RING3_APPS[0]; i++)
            if (&RING3_APPS[i] != a) vfs_delete(RING3_APPS[i].file);
        if (!vfs_write_file(a->file, a->bin, a->len)) {
            serial_puts("ring3app: could not seed "); serial_puts(a->file); serial_puts(", not started\n");
            return -1;
        }
    }
    return 0;
}

/* 1.9.23: the non-blocking launcher behind a compositor window. The
   program is scheduled beside the desktop loop (exec_user_window), draws
   into a framebuffer that exists only in its own directory, and gets its
   keys and clicks from the window's event ring, never from a global key
   pull. Returns the task id for kernel.c's window row, or -1, in which
   case the caller falls back to the blocking path. */
static const char *r3w_file[TASK_SLOTS];
int jt_phone_mode(void); /* kernel.c: boot_to_phone */
int ring3app_launch_window(const char *name, unsigned int w, unsigned int h) {
    const struct ring3_app *a = 0;
    /* Stocks also exits to be relaunched for a fresh quote fetch. */
    if (name[0] == 'S' && name[1] == 't' && name[2] == 'o' && name[3] == 'c' && name[4] == 'k' && name[5] == 's' && !name[6]) return -1; /* -1 = blocking path; 0 is a valid task id */
    for (unsigned int i = 0; i < sizeof RING3_APPS / sizeof RING3_APPS[0]; i++) {
        const char *p = RING3_APPS[i].name, *q = name;
        while (*p && *p == *q) { p++; q++; }
        if (!*p && !*q) { a = &RING3_APPS[i]; break; }
    }
    if (!a || (ring3app_is_samantha(name) && !ring3samantha_on) || ring3app_seed(a) < 0) return -1;
    int phone = jt_phone_mode();
    const char *argv[] = { a->file, "phone" };
    void *image = 0;
    serial_puts("ring3app: launching "); serial_puts(a->file); serial_puts(" at ring 3 as a window\n");
    __asm__ volatile ("cli"); /* the task must not get a tick before its window row exists */
    unsigned int f = irq_save(); /* 1.9.23: the slot is marked used before its private mapping and window exist; a tick in between would run the task against nothing */
    int id = exec_user_window(a->file, argv, phone ? 2 : 1, &image);
    if (id >= 0 && !syscall_window_register(id, w, h, image)) { task_kill(id); id = -1; }
    irq_restore(f);
    __asm__ volatile ("sti");
    if (id < 0) { serial_puts("ring3app: window launch failed, falling back\n"); return -1; }
    r3w_file[id] = a->file;
    return id;
}
/* The compositor's blit of one window's private buffer into the viewport
   kernel.c has already set, through window_pixel, so clipping and the back
   buffer come for free. The program never touches the screen. */
void ring3app_window_blit(int task, int vw, int vh) {
    unsigned int fw = 0, fh = 0; int dirty = 0;
    const unsigned int *fb = syscall_window_fb(task, &fw, &fh, &dirty);
    if (!fb || vw <= 0 || vh <= 0) return;
    unsigned int cw = fw < (unsigned int)vw ? fw : (unsigned int)vw;
    unsigned int ch = fh < (unsigned int)vh ? fh : (unsigned int)vh;
    for (unsigned int yy = 0; yy < ch; yy++)
        for (unsigned int xx = 0; xx < cw; xx++)
            window_pixel((int)xx, (int)yy, fb[yy * fw + xx]);
}
/* Called by the compositor when it finds a window's task gone: the same
   crash/exit line the blocking launcher logs, so the crash checks read it. */
void ring3app_window_reaped(int task, int status) {
    const char *file = (task >= 0 && task < TASK_SLOTS && r3w_file[task]) ? r3w_file[task] : "?";
    char num[12]; put_dec(num, status);
    serial_puts("ring3app: "); serial_puts(file);
    if (status < 0 && -status < 32) { serial_puts(" crashed ("); serial_puts(EXC_SHORT[-status]); serial_puts("), window torn down, desktop alive\n"); }
    else { serial_puts(" exited "); serial_puts(num); serial_puts(", window torn down, desktop alive\n"); }
    if (task >= 0 && task < TASK_SLOTS) r3w_file[task] = 0;
}

/* 1.9.24: kernel.c asks here whether an APPS[] name is a ring-3 program, so
   every RING3_APPS row can open as a compositor window with no second list. */
int ring3app_is_windowable(const char *name) {
    if (!name) return 0;
    /* Weather exits 7 to ask the blocking launcher for a refetch; a window has no such loop. */
    if (name[0] == 'W' && name[1] == 'e' && name[2] == 'a' && name[3] == 't' && name[4] == 'h' && name[5] == 'e' && name[6] == 'r' && !name[7]) return 0;
    if (ring3app_is_samantha(name)) return ring3samantha_on; /* the kernel chat stays the default dock launch */
    if (name[0] == 'S' && name[1] == 't' && name[2] == 'o' && name[3] == 'c' && name[4] == 'k' && name[5] == 's' && !name[6]) return 0; /* exits 16+ to ask the blocking launcher for a refetch */
    for (unsigned int i = 0; i < sizeof RING3_APPS / sizeof RING3_APPS[0]; i++) {
        const char *p = RING3_APPS[i].name, *q = name;
        while (*p && *p == *q) { p++; q++; }
        if (!*p && !*q) return 1;
    }
    return 0;
}

static int ring3app_launch(const struct ring3_app *a) {
    if (ring3app_seed(a) < 0) return -1;
    /* The viewport must fit the ring-3 framebuffer, or SYS_WINDOW_OPEN
       fails and the program exits before it draws a thing. Checked here,
       loudly, so a viewport that outgrows .userfb (boot/linker.ld) shows
       up as one serial line rather than an app that "never opens". */
    unsigned int vw = 0, vh = 0;
    if (!gui_app_view_size(&vw, &vh)) {
        /* gui_app_windowed is 0 or app_view_w/h are 0: the caller opened
           this app without going through gui_launch_from_dock's windowed
           setup (e.g. a keyboard path that called gui_launch_apps directly
           instead of gui_launch_from_dock(GUI_APPS_FOLDER)). Launching
           anyway means SYS_WINDOW_OPEN fails ENODEV and the app exits
           before it draws, which reads as "never opens". Refuse loudly and
           hand control back to the desktop instead. */
        serial_puts("ring3app: BUG no app viewport, not launching\n");
        return -1;
    }
    if (vw * vh * 4 > JT_USER_FB_BYTES) {
        serial_puts("ring3app: BUG app viewport does not fit JT_USER_FB, grow .userfb in boot/linker.ld\n");
        return -1;
    }
    serial_puts("ring3app: launching "); serial_puts(a->file); serial_puts(" at ring 3\n");
    int status = -1;
    const char *argv[] = { a->file };
    (void)mouse_get_wheel(); /* drop a wheel tick banked before the window existed, so a list app does not open already scrolled */
    /* 1.9.12: the same baseline every blocking in-kernel app takes before
       its first poll. gui_poll_event reports a click off mouse_click_edge,
       which also counts presses banked by the backdoor mouse; a multi-window
       session (Mail, Files, Weather) tracks its own presses and never takes
       them, so the click that closed it was still banked and the next
       ring-3 program's first poll read it as a click on itself and closed.
       apptop-check.py (Mail, then Calendar from the dock) caught it. */
    mouse_click_edge_sync();
    if (!exec_user(a->file, argv, 1, &status)) {
        serial_puts("ring3app: exec_user failed (not found, too big, or no free task slot)\n");
        return -1;
    }
    /* exec_user returned, so the task slot is free and syscall_release_task
       has already run for it. A negative status is idt.c's -(vector): the
       program crashed and was reaped. Zero or positive is its own exit
       code. Either way the desktop is what comes next. */
    char num[12]; put_dec(num, status);
    serial_puts("ring3app: "); serial_puts(a->file);
    if (status < 0 && -status < 32) {
        serial_puts(" crashed ("); serial_puts(EXC_SHORT[-status]);
        serial_puts("), window torn down, desktop alive\n");
    } else {
        serial_puts(" exited "); serial_puts(num); serial_puts(", window torn down, desktop alive\n");
    }
    if (syscall_window_owner() >= 0) serial_puts("ring3app: BUG window still owned after the task ended\n");
    return status;
}
void keyrate_ring3_open(void)    { ring3app_launch(&RING3_APPS[0]); }
void toroid_ring3_open(void)     { ring3app_launch(&RING3_APPS[1]); }
void calculator_ring3_open(void) { ring3app_launch(&RING3_APPS[2]); }
void quotestreak_ring3_open(void){ ring3app_launch(&RING3_APPS[3]); }
void bookrank_ring3_open(void)   { ring3app_launch(&RING3_APPS[4]); }
void homeqi_ring3_open(void)     { ring3app_launch(&RING3_APPS[5]); }
void lexly_ring3_open(void)      { ring3app_launch(&RING3_APPS[6]); }
void plan_ring3_open(void)       { ring3app_launch(&RING3_APPS[7]); }
void fieldbook_ring3_open(void)  { ring3app_launch(&RING3_APPS[8]); }
void clock_ring3_open(void)      { ring3app_launch(&RING3_APPS[9]); }
void portfolio_ring3_open(void)  { ring3app_launch(&RING3_APPS[10]); }
void activity_ring3_open(void)   { ring3app_launch(&RING3_APPS[11]); }
void contacts_ring3_open(void)   { ring3app_launch(&RING3_APPS[12]); }
void sparkjar_ring3_open(void)   { ring3app_launch(&RING3_APPS[13]); }
void reminders_ring3_open(void)  { ring3app_launch(&RING3_APPS[14]); }
void curbfind_ring3_open(void)   { ring3app_launch(&RING3_APPS[15]); }
void calendar_ring3_open(void)   { ring3app_launch(&RING3_APPS[16]); }
void search_ring3_open(void)     { ring3app_launch(&RING3_APPS[17]); }
void epiphany_ring3_open(void)   { ring3app_launch(&RING3_APPS[18]); }
void burrow_ring3_open(void)     { ring3app_launch(&RING3_APPS[20]); }
int  stocks_ring3_run(void)      { return ring3app_launch(&RING3_APPS[21]); } /* exit status, kernel.c's stocks_ring3_open decodes 16 + sel*5 + range (+64 refresh) */
void mail_ring3_open(void)       { ring3app_launch(&RING3_APPS[22]); }
void terminal_ring3_open(void)   { ring3app_launch(&RING3_APPS[24]); }
void notes_ring3_launch(void)    { ring3app_launch(&RING3_APPS[23]); } /* kernel.c's notes_ring3_open runs the legacy NOTES.TXT migration first */
int  weather_ring3_run(void)     { return ring3app_launch(&RING3_APPS[19]); } /* exit status, kernel.c's weather_ring3_open loops on 7 */

/* 1.7.8: `fbpoke` boot flag. After the auto-opened Keyrate has exited,
   run user/fbpoke.c with no window: it must be refused a pointer into
   the released framebuffer and must page-fault storing into it. The
   check reads this function's lines; a program that "exited" here
   instead of crashing is the 1.7.7 hole reopened. */
static int fbpoke_armed = 0;
static void fbpoke_run(void) {
    unsigned char probe[1];
    if (vfs_read_file("FBPOKE.BIN", probe, 1) < 0 &&
        !vfs_write_file("FBPOKE.BIN", user_fbpoke, USER_FBPOKE_LEN)) {
        serial_puts("ring3app: could not seed FBPOKE.BIN, not started\n");
        return;
    }
    serial_puts("ring3app: launching FBPOKE.BIN at ring 3, no window\n");
    int status = -1;
    const char *argv[] = { "FBPOKE.BIN" };
    if (!exec_user("FBPOKE.BIN", argv, 1, &status)) { serial_puts("ring3app: exec_user failed for FBPOKE.BIN\n"); return; }
    char num[12]; put_dec(num, status);
    if (status < 0 && -status < 32) {
        serial_puts("ring3app: FBPOKE.BIN crashed ("); serial_puts(EXC_SHORT[-status]);
        serial_puts("), released framebuffer stayed supervisor-only\n");
    } else {
        serial_puts("ring3app: BUG FBPOKE.BIN exited "); serial_puts(num); serial_puts(", the released framebuffer was still writable\n");
    }
}

/* 1.9.27: `brkpoke` boot flag, the SYS_BRK leak probe: pmm free frames and brk
   live pages before and after a program that grows 3MB and crashes; equal
   means nothing leaked. tools/checks/ring3brk-check.py reads these lines. */
static int brkpoke_armed = 0;
static void brkpoke_run(void) {
    unsigned char probe[1];
    if (vfs_read_file("BRKPOKE.BIN", probe, 1) < 0 &&
        !vfs_write_file("BRKPOKE.BIN", user_brkpoke, USER_BRKPOKE_LEN)) {
        serial_puts("ring3app: could not seed BRKPOKE.BIN, not started\n");
        return;
    }
    char num[12];
    put_dec(num, (int)pmm_free_frames()); serial_puts("ring3app: brkpoke baseline free="); serial_puts(num);
    put_dec(num, (int)brk_live_pages()); serial_puts(" live="); serial_puts(num); serial_puts("\n");
    serial_puts("ring3app: launching BRKPOKE.BIN at ring 3, no window\n");
    int status = -1;
    const char *argv[] = { "BRKPOKE.BIN" };
    if (!exec_user("BRKPOKE.BIN", argv, 1, &status)) { serial_puts("ring3app: exec_user failed for BRKPOKE.BIN\n"); return; }
    if (status < 0 && -status < 32) {
        serial_puts("ring3app: BRKPOKE.BIN crashed ("); serial_puts(EXC_SHORT[-status]); serial_puts(")\n");
    } else {
        put_dec(num, status);
        serial_puts("ring3app: BUG BRKPOKE.BIN exited "); serial_puts(num); serial_puts(" instead of crashing\n");
    }
    put_dec(num, (int)pmm_free_frames()); serial_puts("ring3app: brkpoke after free="); serial_puts(num);
    put_dec(num, (int)brk_live_pages()); serial_puts(" live="); serial_puts(num); serial_puts("\n");
}

static int ring3app_autoopen_slot = -1; /* APPS[] index to open, -1 when unarmed */
void ring3app_autoopen_arm(const char *cl){
    for (const char *pc = cl; pc && *pc; pc++) {
        if (pc[0]=='o' && pc[1]=='p' && pc[2]=='e' && pc[3]=='n' && pc[4]=='=' && pc[5]=='k' && pc[6]=='e' && pc[7]=='y' && pc[8]=='r') { ring3app_autoopen_slot = 9; serial_puts("autoopen=keyrate\n"); }
        if (pc[0]=='o' && pc[1]=='p' && pc[2]=='e' && pc[3]=='n' && pc[4]=='=' && pc[5]=='t' && pc[6]=='o' && pc[7]=='r' && pc[8]=='o') { ring3app_autoopen_slot = 14; serial_puts("autoopen=toroid\n"); }
        if (pc[0]=='o' && pc[1]=='p' && pc[2]=='e' && pc[3]=='n' && pc[4]=='=' && pc[5]=='c' && pc[6]=='a' && pc[7]=='l' && pc[8]=='c') { ring3app_autoopen_slot = 19; serial_puts("autoopen=calculator\n"); }
        if (pc[0]=='o' && pc[1]=='p' && pc[2]=='e' && pc[3]=='n' && pc[4]=='=' && pc[5]=='q' && pc[6]=='u' && pc[7]=='o' && pc[8]=='t') { ring3app_autoopen_slot = 11; serial_puts("autoopen=quotes\n"); }
        if (pc[0]=='o' && pc[1]=='p' && pc[2]=='e' && pc[3]=='n' && pc[4]=='=' && pc[5]=='b' && pc[6]=='o' && pc[7]=='o' && pc[8]=='k') { ring3app_autoopen_slot = 10; serial_puts("autoopen=bookrank\n"); }
        if (pc[0]=='o' && pc[1]=='p' && pc[2]=='e' && pc[3]=='n' && pc[4]=='=' && pc[5]=='h' && pc[6]=='o' && pc[7]=='m' && pc[8]=='e') { ring3app_autoopen_slot = 16; serial_puts("autoopen=homeqi\n"); }
        if (pc[0]=='o' && pc[1]=='p' && pc[2]=='e' && pc[3]=='n' && pc[4]=='=' && pc[5]=='l' && pc[6]=='e' && pc[7]=='x' && pc[8]=='l') { ring3app_autoopen_slot = 13; serial_puts("autoopen=lexly\n"); }
        if (pc[0]=='o' && pc[1]=='p' && pc[2]=='e' && pc[3]=='n' && pc[4]=='=' && pc[5]=='p' && pc[6]=='l' && pc[7]=='a' && pc[8]=='n') { ring3app_autoopen_slot = 12; serial_puts("autoopen=plan\n"); }
        if (pc[0]=='o' && pc[1]=='p' && pc[2]=='e' && pc[3]=='n' && pc[4]=='=' && pc[5]=='f' && pc[6]=='i' && pc[7]=='e' && pc[8]=='l') { ring3app_autoopen_slot = 17; serial_puts("autoopen=fieldbook\n"); }
        if (pc[0]=='o' && pc[1]=='p' && pc[2]=='e' && pc[3]=='n' && pc[4]=='=' && pc[5]=='c' && pc[6]=='l' && pc[7]=='o' && pc[8]=='c') { ring3app_autoopen_slot = 25; serial_puts("autoopen=clock\n"); }
        if (pc[0]=='o' && pc[1]=='p' && pc[2]=='e' && pc[3]=='n' && pc[4]=='=' && pc[5]=='p' && pc[6]=='o' && pc[7]=='r' && pc[8]=='t' && pc[9]=='f' && pc[10]!='o') { ring3app_autoopen_slot = 23; serial_puts("autoopen=portfolio\n"); }
        if (pc[0]=='o' && pc[1]=='p' && pc[2]=='e' && pc[3]=='n' && pc[4]=='=' && pc[5]=='a' && pc[6]=='c' && pc[7]=='t' && pc[8]=='i' && pc[9]=='v') { ring3app_autoopen_slot = 24; serial_puts("autoopen=activity\n"); }
        if (pc[0]=='o' && pc[1]=='p' && pc[2]=='e' && pc[3]=='n' && pc[4]=='=' && pc[5]=='c' && pc[6]=='o' && pc[7]=='n' && pc[8]=='t') { ring3app_autoopen_slot = 18; serial_puts("autoopen=contacts\n"); }
        if (pc[0]=='o' && pc[1]=='p' && pc[2]=='e' && pc[3]=='n' && pc[4]=='=' && pc[5]=='s' && pc[6]=='p' && pc[7]=='a' && pc[8]=='r') { ring3app_autoopen_slot = 15; serial_puts("autoopen=sparkjar\n"); }
        if (pc[0]=='o' && pc[1]=='p' && pc[2]=='e' && pc[3]=='n' && pc[4]=='=' && pc[5]=='r' && pc[6]=='e' && pc[7]=='m' && pc[8]=='i') { ring3app_autoopen_slot = 4; serial_puts("autoopen=reminders\n"); }
        if (pc[0]=='o' && pc[1]=='p' && pc[2]=='e' && pc[3]=='n' && pc[4]=='=' && pc[5]=='c' && pc[6]=='u' && pc[7]=='r' && pc[8]=='b') { ring3app_autoopen_slot = 8; serial_puts("autoopen=curbfind\n"); }
        if (pc[0]=='o' && pc[1]=='p' && pc[2]=='e' && pc[3]=='n' && pc[4]=='=' && pc[5]=='c' && pc[6]=='a' && pc[7]=='l' && pc[8]=='e') { ring3app_autoopen_slot = 2; serial_puts("autoopen=calendar\n"); }
        if (pc[0]=='o' && pc[1]=='p' && pc[2]=='e' && pc[3]=='n' && pc[4]=='=' && pc[5]=='s' && pc[6]=='e' && pc[7]=='a' && pc[8]=='r') { ring3app_autoopen_slot = 21; serial_puts("autoopen=search\n"); }
        if (pc[0]=='o' && pc[1]=='p' && pc[2]=='e' && pc[3]=='n' && pc[4]=='=' && pc[5]=='e' && pc[6]=='p' && pc[7]=='i' && pc[8]=='p') { ring3app_autoopen_slot = 22; serial_puts("autoopen=epiphany\n"); }
        if (pc[0]=='o' && pc[1]=='p' && pc[2]=='e' && pc[3]=='n' && pc[4]=='=' && pc[5]=='w' && pc[6]=='e' && pc[7]=='a' && pc[8]=='t') { ring3app_autoopen_slot = 7; serial_puts("autoopen=weather\n"); }
        if (pc[0]=='o' && pc[1]=='p' && pc[2]=='e' && pc[3]=='n' && pc[4]=='=' && pc[5]=='b' && pc[6]=='u' && pc[7]=='r' && pc[8]=='r') { ring3app_autoopen_slot = 0; serial_puts("autoopen=burrow\n"); }
        if (pc[0]=='o' && pc[1]=='p' && pc[2]=='e' && pc[3]=='n' && pc[4]=='=' && pc[5]=='m' && pc[6]=='a' && pc[7]=='i' && pc[8]=='l') { ring3app_autoopen_slot = 1; serial_puts("autoopen=mail\n"); }
        if (pc[0]=='o' && pc[1]=='p' && pc[2]=='e' && pc[3]=='n' && pc[4]=='=' && pc[5]=='s' && pc[6]=='t' && pc[7]=='o' && pc[8]=='c') { ring3app_autoopen_slot = 20; serial_puts("autoopen=stocks\n"); }
        if (pc[0]=='o' && pc[1]=='p' && pc[2]=='e' && pc[3]=='n' && pc[4]=='=' && pc[5]=='n' && pc[6]=='o' && pc[7]=='t' && pc[8]=='e') { ring3app_autoopen_slot = 3; serial_puts("autoopen=notes\n"); }
        if (pc[0]=='o' && pc[1]=='p' && pc[2]=='e' && pc[3]=='n' && pc[4]=='=' && pc[5]=='t' && pc[6]=='e' && pc[7]=='r' && pc[8]=='m') { ring3app_autoopen_slot = 5; serial_puts("autoopen=terminal\n"); }
        if (pc[0]=='f' && pc[1]=='b' && pc[2]=='p' && pc[3]=='o' && pc[4]=='k' && pc[5]=='e') { fbpoke_armed = 1; serial_puts("fbpoke armed\n"); }
        if (pc[0]=='b' && pc[1]=='r' && pc[2]=='k' && pc[3]=='p' && pc[4]=='o' && pc[5]=='k' && pc[6]=='e') { brkpoke_armed = 1; serial_puts("brkpoke armed\n"); }
    }
}
void ring3app_autoopen_run(int mx, int my){
    if (ring3app_autoopen_slot < 0) return;
    int slot = ring3app_autoopen_slot; ring3app_autoopen_slot = -1;
    editor_mouse_x = mx; editor_mouse_y = my;
    gui_launch_from_dock(slot); /* Keyrate's, Toroid's, Calculator's, Quotes', Bookrank's, Homeqi's, Lexly's, Plan's, Fieldbook's, Clock's, Portfolio's, Activity's, Contacts', Sparkjar's, Reminders', Curbfind's, Calendar's or Search's APPS slot */
    if (fbpoke_armed) { fbpoke_armed = 0; fbpoke_run(); }
    if (brkpoke_armed) { brkpoke_armed = 0; brkpoke_run(); }
    gui_draw_desktop(-1, -1, 0, 0);
    cursor_saved_x = cursor_saved_y = -1;
    gui_cursor_save(mx, my);
    gui_draw_cursor(mx, my);
    serial_puts("autoopen: back on the desktop\n");
}

/* 1.7.7: the size of the app viewport a dock launch opened, for
   kernel/syscall.c's SYS_WINDOW_OPEN. 0 when no app window is open. */
int gui_app_view_size(unsigned int *w, unsigned int *h){
    if (!gui_app_windowed || app_view_w <= 0 || app_view_h <= 0) return 0;
    *w = (unsigned int)app_view_w; *h = (unsigned int)app_view_h;
    return 1;
}

/* 1.7.7: the non-blocking twin of kernel.c's get_key_or_click_until, for
   kernel/syscall.c's SYS_WINDOW_POLL. It runs inside the int 0x80 gate
   with interrupts off, so it must never wait: one look at the keyboard
   queue and the mouse, then back. Returns JT_EV_KEY (1) with the key in
   *a (the same ASCII/KEY_* values the blocking loop hands in-kernel apps),
   JT_EV_CLICK (2) with the pointer in app-window coordinates, JT_EV_WHEEL
   (3) with +1/-1 in *a, or 0 when nothing happened. A 0xE0 prefix whose
   second byte has not arrived yet (it cannot, with IF clear) is remembered
   for the next call rather than dropped, so arrows still work. */
static int gui_poll_pending_e0 = 0;
int gui_poll_event(int *a, int *b){
    *a = 0; *b = 0;
    gui_app_mouse_tick();
    int sc = kbd_pop();
    if (sc >= 0) {
        if (sc == 0xE0 && !gui_poll_pending_e0) { gui_poll_pending_e0 = 1; sc = kbd_pop(); if (sc < 0) return 0; }
        if (gui_poll_pending_e0) {
            gui_poll_pending_e0 = 0;
            if (sc == 0x48) { *a = KEY_UP; return 1; }
            if (sc == 0x50) { *a = KEY_DOWN; return 1; }
            if (sc == 0x4B) { *a = KEY_LEFT; return 1; }
            if (sc == 0x4D) { *a = KEY_RIGHT; return 1; }
            if (sc == 0x47) { *a = KEY_HOME; return 1; }
            if (sc == 0x4F) { *a = KEY_END; return 1; }
            if (sc == 0x53) { *a = KEY_DELETE; return 1; }
            return 0;
        }
        if (sc & 0x80) return 0;
        gui_close_was_click = 0;
        if (kbd_ctrl) {
            int code = sc & 0x7F;
            if (code == 0x2E) { *a = KEY_COPY; return 1; }
            if (code == 0x2D) { *a = KEY_CUT; return 1; }
            if (code == 0x2F) { *a = KEY_PASTE; return 1; }
            if (code == 0x1F) { *a = KEY_SAVE; return 1; }
        }
        char c = kbd_map(sc);
        if (c == '\n') { *a = KEY_ENTER; return 1; }
        if (c == 27)   { *a = KEY_ESC; return 1; }
        if (c) { *a = (int)(unsigned char)c; return 1; }
        return 0;
    }
    if (mouse_click_edge()) { gui_close_was_click = 1; *a = app_cursor_x - app_view_x; *b = app_cursor_y - app_view_y; return 2; }
    int wheel = mouse_get_wheel();
    if (wheel) { *a = wheel > 0 ? 1 : -1; return 3; }
    return 0;
}
