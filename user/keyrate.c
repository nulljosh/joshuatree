/* keyrate: the typing test, as a real ring-3 program.
 *
 * The first app to leave the kernel (roadmap 2.0, step one). It is the
 * same test drivers/app_keyrate.c runs in ring 0: endless random words,
 * typed characters turn brown, a live words-per-minute readout, esc or a
 * click closes. The difference is where it runs. This file is compiled
 * with no kernel include path, linked flat like hello and note, loaded
 * off the VFS by exec_user, and reaches the machine only through int
 * 0x80: SYS_WINDOW_OPEN for a framebuffer, SYS_WINDOW_POLL for input,
 * SYS_TIME for the clock, SYS_EXIT to leave. A bug here is this program's
 * problem, not the kernel's.
 *
 * The backquote key (`) is the deliberate crash: it writes through a null
 * pointer, which is a page fault at ring 3. The kernel reaps this task,
 * drops its window and goes back to the desktop. tools/checks/
 * ring3app-check.py presses it on purpose and asserts exactly that.
 *
 * Glyphs: antialiased DejaVu via libjt/text.h. There is
 * no font syscall and a program draws its own pixels, so it carries its
 * own letters. No .bss allowed in a flat binary, so every global is
 * initialised and everything else lives on the one 4KB stack page.
 */
#include "jtsys.h"
#include "libjt/string.h"
#include "libjt/text.h"

#define BG   0x00FAF8F6
#define INK  0x001C1C1E
#define DONE 0x00884B16
#define HINT 0x0075726E

static const char *WORDS[] = {
    "the","quick","brown","fox","jumps","over","lazy","dog","time","people",
    "water","first","would","these","other","after","words","world","school",
    "still","every","great","might","under","never","found","those","while",
    "place","right","small","sound","between","name","home","read","hand",
    "large","spell","add","even","land","here","must","big","high","such",
    "follow","act","why","ask","men","change","went","light","kind","off",
    "need","house","try","again","animal","point","mother","near","self",
    "work","part","take","get","made","live","where","much","back","only",
};
#define WORD_COUNT (int)(sizeof(WORDS) / sizeof(WORDS[0]))

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};

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

static void text(const char *s, int x, int y, unsigned fg) { jt_text_draw(&win, JT_FACE_BODY, x, y, fg, s); }

/* One glyph at a time so typed and untyped letters take different inks;
   returns the advance. */
static int put1(char c, int x, int y, unsigned fg) {
    char t[2] = { c, 0 };
    jt_text_draw(&win, JT_FACE_BODY, x, y, fg, t);
    return jt_text_width(JT_FACE_BODY, t);
}

static int gen_words(char *buf, int cap, unsigned *rng) {
    int len = 0;
    while (len < cap - 12) {
        if (len > 0) buf[len++] = ' ';
        *rng = *rng * 1103515245u + 12345u;
        const char *w = WORDS[((*rng >> 16) & 0x7fff) % WORD_COUNT];
        while (*w) buf[len++] = *w++;
    }
    buf[len] = 0;
    return len;
}

static int utoa10(unsigned v, char *buf) {
    char tmp[12]; int tn = 0, n = 0;
    do { tmp[tn++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (tn) buf[n++] = tmp[--tn];
    buf[n] = 0;
    return n;
}

/* One frame, then wait for the next event. The wait is poll + yield, not a
   blocking read: int 0x80 runs with interrupts off, so the kernel cannot
   sleep a program inside it. Presenting only on the poll after a redraw
   keeps the copy off the idle path. */
static int next_event(struct jt_event *ev, int present) {
    unsigned flags = present ? JT_POLL_PRESENT : 0;
    for (;;) {
        int r = jt_window_poll(ev, flags);
        if (r == 1) return 1;
        if (r != -11 /* -EAGAIN */) return 0;
        flags = 0;
        jt_sched_yield();
    }
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open(&win) != 0 || !win.pixels) {
        jt_write(2, "keyrate: no window\n", 19);
        jt_exit(1);
    }
    {   /* one line, one write: what tools/checks/ring3app-check.py asserts on */
        char line[48]; int l = 0;
        const char *pfx = "keyrate: ring-3 window ";
        while (*pfx) line[l++] = *pfx++;
        l += utoa10(win.width, line + l); line[l++] = 'x';
        l += utoa10(win.height, line + l); line[l++] = '\n';
        jt_write(1, line, (unsigned)l);
    }

    char target[256];
    unsigned now = 0; jt_time(&now);
    unsigned rng = now | 1;
    int tlen = gen_words(target, sizeof target, &rng);
    int pos = 0, started = 0, total_typed = 0;
    unsigned start_s = 0;
    int W = (int)win.width, H = (int)win.height;

    rect(0, 0, W, H, BG);
    for (;;) {
        rect(20, 40, W - 40, H - 76, BG);
        rect(20, H - 34, W - 40, 22, BG);
        {   /* wrap at word boundaries: a word that will not fit moves down whole */
            int x = 20, y = 44, i = 0;
            while (i < tlen) {
                int e = i, ww = 0;
                while (e < tlen && target[e] != ' ') { char t[2] = { target[e], 0 }; ww += jt_text_width(JT_FACE_BODY, t); e++; }
                if (x > 20 && x + ww > W - 20) { x = 20; y += 24; }
                for (; i < e; i++) x += put1(target[i], x, y, i < pos ? DONE : INK);
                if (i < tlen) { x += put1(' ', x, y, i < pos ? DONE : INK); i++; }
            }
        }
        if (started) {
            unsigned t = 0; jt_time(&t);
            unsigned elapsed = t - start_s; /* whole seconds: the finest clock the ABI offers */
            int wpm = elapsed > 0 ? (int)(((unsigned)(total_typed + pos) * 12u) / elapsed) : 0; /* (chars/5) per minute */
            char buf[32]; int n = utoa10((unsigned)wpm, buf);
            buf[n++] = ' '; buf[n++] = 'w'; buf[n++] = 'p'; buf[n++] = 'm'; buf[n] = 0;
            text(buf, 20, H - 30, DONE);
        } else {
            text("type to begin, esc to close", 20, H - 30, HINT);
        }

        struct jt_event ev;
        if (!next_event(&ev, 1)) break;
        if (jt_window_resized(&ev, &win)) { W = (int)win.width; H = (int)win.height; rect(0, 0, W, H, BG); continue; } /* JT_EV_RESIZE */
        if (ev.kind != JT_EV_KEY) continue;
        if (ev.a == JT_KEY_ESC) break;
        if (ev.a == '`') {
            /* The deliberate crash. Announced first so the check can tell a
               requested fault from an accidental one. */
            jt_write(1, "keyrate: crashing on purpose\n", 29);
            *(volatile int *)0 = 1;
        }
        if (ev.a > 255) continue;
        if (!started) { started = 1; jt_time(&start_s); }
        if ((char)ev.a == target[pos] && ++pos >= tlen) {
            total_typed += tlen;
            tlen = gen_words(target, sizeof target, &rng);
            pos = 0;
        }
    }
    jt_write(1, "keyrate: closed\n", 16);
    jt_exit(0);
}
