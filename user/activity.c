/* activity: uptime, free memory and the scheduler's task slots, as a real
 * ring-3 program.
 *
 * The twelfth app to leave the kernel (roadmap 2.0), done the way
 * user/portfolio.c was. Same screen kernel/activity.h drew in ring 0: an
 * uptime and memory line, the six scheduler slots with their state, and a
 * Kill button. Up and down (or a click) select a slot, k or the button kills
 * it, and the window refreshes about once a second on its own. It reaches the
 * machine only through int 0x80: SYS_WINDOW_OPEN, SYS_WINDOW_POLL, SYS_EXIT,
 * and the one new call this port needed, SYS_TASKS (1.9.6), which returns
 * the ticks, the memory and which slots are live, and kills a slot through
 * the same task_kill the shell's own `kill` uses.
 *
 * Slot 0 is the shell and desktop, and this program's own slot is the app
 * itself; the kernel refuses to kill either, and the message says so. Other
 * slots are labeled "Task N" by their real number, since the scheduler keeps
 * no names.
 *
 * Glyphs: antialiased DejaVu via libjt/text.h. The backquote key (`) is the
 * deliberate crash: a write through a null pointer, a page fault at ring 3,
 * reaped by the kernel. tools/checks/ring3activity-check.py presses it.
 */
#include "jtsys.h"
#include "libjt/text.h"

#define BG    0x00FAF8F6 /* GUI_BG */
#define INK   0x001C1C1E
#define HINT  0x0075726E
#define DIM   0x00807468
#define GREEN 0x00375A4A
#define RED   0x00A13F3F
#define PALE  0x00F5F0EB
#define SELBG 0x00EDE6DC

#define ROWS    6   /* TASK_SLOTS: every scheduler slot, free or used */
#define TOP     88  /* first row's y */
#define ROW_H   22
#define BTN_Y   (TOP + ROWS * ROW_H + 14)
#define REFRESH 100 /* ticks, one second at the 100Hz PIT */

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
static struct jt_tasks snap JT_DATA = {0, 0, 0, 0, 0};
static int sel JT_DATA = 0;
static char msg[32] JT_DATA = {0};
static unsigned msg_until JT_DATA = 0;

static void rect(int x, int y, int w, int h, unsigned c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)win.width)  w = (int)win.width - x;
    if (y + h > (int)win.height) h = (int)win.height - y;
    for (int yy = 0; yy < h; yy++) {
        unsigned *row = win.pixels + (unsigned)(y + yy) * win.width + (unsigned)x;
        for (int xx = 0; xx < w; xx++) row[xx] = c;
    }
}
static int text(const char *s, int x, int y, unsigned fg) { /* returns the x after the last glyph */
    return jt_text_draw(&win, JT_FACE_BODY, x, y, fg, s);
}
static int btext(const char *s, int x, int y, unsigned fg) {
    return jt_text_draw(&win, JT_FACE_BOLD, x, y, fg, s);
}
static int utoa10(unsigned v, char *buf) {
    char tmp[12]; int tn = 0, n = 0;
    do { tmp[tn++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (tn) buf[n++] = tmp[--tn];
    buf[n] = 0;
    return n;
}
static void say(const char *pfx, unsigned v) { /* one line, one write: what the check reads */
    char line[48]; int l = 0;
    while (*pfx) line[l++] = *pfx++;
    l += utoa10(v, line + l); line[l++] = '\n';
    jt_write(1, line, (unsigned)l);
}
static void set_msg(const char *m, unsigned now) {
    int i = 0;
    while (m[i] && i < 31) { msg[i] = m[i]; i++; }
    msg[i] = 0;
    msg_until = now + 200; /* about two seconds */
}

static void draw(unsigned now) {
    int w = (int)win.width;
    char num[12];
    rect(0, 0, w, (int)win.height, BG);
    int x = text("Uptime:", 20, 40, HINT) + 6;
    utoa10(snap.ticks / 100, num);
    x = text(num, x, 40, INK);
    text("s", x, 40, INK);

    x = text("Memory:", 180, 40, HINT) + 6;
    utoa10(snap.free_kb, num);
    x = text(num, x, 40, INK);
    x = text("K free /", x, 40, INK) + 6;
    utoa10(snap.total_kb, num);
    x = text(num, x, 40, INK);
    text("K total", x, 40, INK);

    btext("PID", 20, 66, HINT);
    btext("NAME", 80, 66, HINT);
    btext("STATE", 200, 66, HINT);

    for (int i = 0; i < ROWS; i++) {
        int y = TOP + i * ROW_H;
        int used = (snap.used >> i) & 1;
        if (i == sel) rect(16, y - 4, w - 32, 20, SELBG);
        utoa10((unsigned)i, num);
        text(num, 20, y, INK);
        if (i == 0) text("Shell/GUI", 80, y, INK);
        else if (used) { char nm[16] = "Task "; utoa10((unsigned)i, nm + 5); text(nm, 80, y, INK); }
        else text("--", 80, y, DIM);
        if ((unsigned)i == snap.current) text("running (this)", 200, y, INK);
        else if (used) text("running", 200, y, GREEN);
        else text("free", 200, y, DIM);
    }

    rect(20, BTN_Y, 96, 24, RED);
    btext("Kill", 20 + (96 - jt_text_width(JT_FACE_BOLD, "Kill")) / 2, BTN_Y + 4, PALE);
    if (msg_until && now < msg_until) text(msg, 130, BTN_Y + 4, RED);
    text("up/down select   k kills   esc closes", 20, BTN_Y + 40, HINT);
    jt_write(1, "activitycontent\n", 16); /* one marker per redraw, what the checks count */
}

static void kill_selected(unsigned now) {
    int r = jt_tasks(&snap, sel);
    if (r == 0) set_msg("kill signal sent", now);
    else if (sel == 0) set_msg("can't kill the shell", now);
    else if ((unsigned)sel == snap.current) set_msg("can't kill this app", now);
    else set_msg("that slot is already free", now);
    say("activity: kill ", (unsigned)sel);
    say("activity: kill result ", (unsigned)(r < 0 ? -r : 0));
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open(&win) != 0 || !win.pixels) {
        jt_write(2, "activity: no window\n", 20);
        jt_exit(1);
    }
    {   /* one line, one write: what tools/checks/ring3activity-check.py asserts on */
        char line[48]; int l = 0;
        const char *pfx = "activity: ring-3 window ";
        while (*pfx) line[l++] = *pfx++;
        l += utoa10(win.width, line + l); line[l++] = 'x';
        l += utoa10(win.height, line + l); line[l++] = '\n';
        jt_write(1, line, (unsigned)l);
    }

    jt_tasks(&snap, -1);
    unsigned next = snap.ticks + REFRESH;
    draw(snap.ticks);

    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        flags = 0;
        if (r == -11 /* -EAGAIN */) {
            jt_tasks(&snap, -1);
            if (snap.ticks >= next) { next = snap.ticks + REFRESH; draw(snap.ticks); flags = JT_POLL_PRESENT; }
            else jt_sched_yield();
            continue;
        }
        if (r != 1) break;

        jt_tasks(&snap, -1);
        int old = sel;
        if (ev.kind == JT_EV_CLICK) {
            if (ev.a >= 20 && ev.a < 116 && ev.b >= BTN_Y && ev.b < BTN_Y + 24) { kill_selected(snap.ticks); draw(snap.ticks); flags = JT_POLL_PRESENT; continue; }
            int hit = -1;
            for (int i = 0; i < ROWS; i++) {
                int y = TOP + i * ROW_H;
                if (ev.a >= 16 && ev.a < (int)win.width - 16 && ev.b >= y - 4 && ev.b < y + 16) { hit = i; break; }
            }
            if (hit < 0) break; /* the titlebar X, or anywhere off a row */
            sel = hit;
        } else if (ev.kind == JT_EV_KEY) {
            int k = ev.a;
            if (k == JT_KEY_ESC) break;
            if (k == '`') {
                jt_write(1, "activity: crashing on purpose\n", 30);
                *(volatile int *)0 = 1;
            }
            if (k == JT_KEY_UP && sel > 0) sel--;
            else if (k == JT_KEY_DOWN && sel < ROWS - 1) sel++;
            else if (k == 'k') { kill_selected(snap.ticks); draw(snap.ticks); flags = JT_POLL_PRESENT; continue; }
        } else {
            flags = JT_POLL_PRESENT;
            continue;
        }
        if (sel != old) say("activity: sel ", (unsigned)sel);
        draw(snap.ticks); flags = JT_POLL_PRESENT;
    }
    jt_write(1, "activity: closed\n", 17);
    jt_exit(0);
}
