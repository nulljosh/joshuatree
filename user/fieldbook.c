/* fieldbook: every field of science and math explained plainly, as a real ring-3 program.
 *
 * The ninth app to leave the kernel (roadmap 2.0), done the way
 * user/plan.c was. Same twelve fields and two-pane layout
 * kernel/fieldbook.h ran in ring 0: a ranked list on the left, up/down or a
 * click selects, the field name, its studies line and the explanation wrap
 * on the right. Built with no kernel include path, linked flat, loaded off
 * the VFS by exec_user, and it reaches the machine only through int 0x80:
 * SYS_WINDOW_OPEN for a framebuffer, SYS_WINDOW_POLL for input and the
 * present, SYS_EXIT to leave.
 *
 * Glyphs: the kernel's 8x16 VGA fallback font, same as the other ring-3
 * apps. The backquote key (`) is the deliberate crash: a write through a
 * null pointer, a page fault at ring 3, reaped by the kernel.
 * tools/checks/ring3fieldbook-check.py presses it on purpose.
 */
#include "jtsys.h"
#include "libjt/text.h"

#define BG    0x00FAF8F6 /* GUI_BG */
#define ROW   0x00F1EDE7
#define SELBG 0x00E2D8CC
#define INK   0x001C1C1E
#define HINT  0x0075726E

#define FB_LIST_X 20
#define FB_LIST_W 220
#define FB_INFO_X 260
#define FB_TOP    40
#define FB_ITEM_H 28

typedef struct { const char *name; const char *studies; const char *explanation; } FbField;
static const FbField FB_FIELDS[] = {
    {"Physics", "studies matter, energy, and forces", "Examines how objects move and interact through gravity, electromagnetism, and other fundamental forces. Builds the foundation for understanding everything from atoms to galaxies."},
    {"Chemistry", "studies atoms, bonds, and reactions", "Explores how elements combine into molecules and how they transform through reactions. Explains why materials behave as they do and how new substances form."},
    {"Biology", "studies living organisms and systems", "Investigates how cells, organisms, and ecosystems work from the molecular level to entire populations. Explains how life adapts, reproduces, and evolves."},
    {"Astronomy", "studies stars, planets, and galaxies", "Examines the structure of the universe, the life cycles of stars, and the physics of cosmic phenomena. Reveals our place in an expanding cosmos of billions of galaxies."},
    {"Geology", "studies rocks, minerals, and Earth's structure", "Investigates the composition and history of the planet, from surface rocks to the molten core. Explains how continents drift, mountains form, and the Earth evolves over deep time."},
    {"Ecology", "studies organisms and their environments", "Examines how species interact with each other and their surroundings through food webs and nutrient cycles. Shows how energy flows through nature and ecosystems maintain balance."},
    {"Neuroscience", "studies the brain and nervous system", "Investigates how neurons communicate, how signals travel through the brain, and how thought and sensation arise. Bridges biology and psychology by studying the physical basis of mind."},
    {"Computer Science", "studies computation and algorithms", "Explores how problems can be solved by step-by-step logical procedures and how to make those procedures efficient. Underlies all digital technology, from phones to the internet."},
    {"Statistics", "studies data, uncertainty, and probability", "Teaches how to extract meaning from data, estimate unknown quantities, and understand variability. Essential for science, medicine, economics, and any field dealing with real-world uncertainty."},
    {"Calculus", "studies rates of change and accumulation", "Analyzes how quantities change continuously and how to compute areas under curves. Powers physics, engineering, and economics by handling change mathematically."},
    {"Topology", "studies shapes, surfaces, and continuity", "Examines properties that stay the same when shapes are stretched or bent without tearing. Discovers surprising connections between different geometric objects."},
    {"Number Theory", "studies integers and prime numbers", "Investigates the structure of whole numbers, divisibility, and patterns hidden in sequences. Provides the mathematical foundation for cryptography and computer security."},
};
#define FB_COUNT ((int)(sizeof(FB_FIELDS) / sizeof(FB_FIELDS[0])))

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
static int fb_sel JT_DATA = 0;

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
/* s cut to maxw px with a trailing "...", drawn in the given face */
static void etext(int face, const char *s, int x, int y, int maxw, unsigned fg) {
    char t[72]; int n = 0;
    while (s[n] && n < 64) { t[n] = s[n]; n++; }
    t[n] = 0;
    if (jt_text_width(face, t) > maxw) {
        while (n > 0) {
            t[n] = '.'; t[n + 1] = '.'; t[n + 2] = '.'; t[n + 3] = 0;
            if (jt_text_width(face, t) <= maxw) break;
            t[--n] = 0;
        }
        if (n == 0) t[0] = 0;
    }
    jt_text_draw(&win, face, x, y, fg, t);
}
static int utoa10(unsigned v, char *buf) {
    char tmp[12]; int tn = 0, n = 0;
    do { tmp[tn++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (tn) buf[n++] = tmp[--tn];
    buf[n] = 0;
    return n;
}

/* Greedy word wrap by pixel width, breaking only at spaces. */
static void fb_wrap(const char *s, int x, int y, int w, int ymax, unsigned fg) {
    char line[120]; int ll = 0;
    while (*s && y + 18 <= ymax) {
        int we = 0;
        while (s[we] && s[we] != ' ') we++;
        char trial[120]; int tl = 0;
        for (int i = 0; i < ll; i++) trial[tl++] = line[i];
        if (ll) trial[tl++] = ' ';
        for (int i = 0; i < we && tl < 118; i++) trial[tl++] = s[i];
        trial[tl] = 0;
        if (ll && jt_text_width(JT_FACE_BODY, trial) > w) {
            line[ll] = 0; text(line, x, y, fg); y += 20; ll = 0;
            continue;
        }
        for (int i = 0; i <= tl; i++) line[i] = trial[i];
        ll = tl; s += we;
        while (*s == ' ') s++;
    }
    if (ll && y + 18 <= ymax) { line[ll] = 0; text(line, x, y, fg); }
}

static void fb_draw(void) {
    rect(0, 0, (int)win.width, (int)win.height, BG);
    int max_items = (int)win.height - FB_TOP - 50;
    int shown = FB_COUNT < max_items / FB_ITEM_H ? FB_COUNT : max_items / FB_ITEM_H;
    for (int i = 0; i < shown; i++) {
        int y = FB_TOP + i * FB_ITEM_H;
        unsigned fg = i == fb_sel ? INK : HINT;
        rect(FB_LIST_X, y, FB_LIST_W, FB_ITEM_H - 2, i == fb_sel ? SELBG : ROW);
        char rank[4]; int r = 0, n = i + 1;
        if (n >= 10) rank[r++] = (char)('0' + n / 10);
        rank[r++] = (char)('0' + n % 10);
        rank[r++] = '.'; rank[r] = 0;
        text(rank, FB_LIST_X + 6, y + 5, fg);
        etext(JT_FACE_BODY, FB_FIELDS[i].name, FB_LIST_X + 30, y + 5, FB_LIST_W - 36, fg);
    }
    const FbField *f = &FB_FIELDS[fb_sel];
    etext(JT_FACE_BOLD, f->name, FB_INFO_X, FB_TOP, (int)win.width - FB_INFO_X - 24, INK);
    etext(JT_FACE_BODY, f->studies, FB_INFO_X, FB_TOP + 24, (int)win.width - FB_INFO_X - 24, HINT);
    int wy = FB_TOP + 52;
    fb_wrap(f->explanation, FB_INFO_X, wy, (int)win.width - FB_INFO_X - 24, (int)win.height - 50, INK);
    etext(JT_FACE_BODY, "up/down or click to select   esc closes", 20, (int)win.height - 30, (int)win.width - 40, HINT);
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open(&win) != 0 || !win.pixels) {
        jt_write(2, "fieldbook: no window\n", 21);
        jt_exit(1);
    }
    {   /* one line, one write: what tools/checks/ring3fieldbook-check.py asserts on */
        char line[56]; int l = 0;
        const char *pfx = "fieldbook: ring-3 window ";
        while (*pfx) line[l++] = *pfx++;
        l += utoa10(win.width, line + l); line[l++] = 'x';
        l += utoa10(win.height, line + l); line[l++] = '\n';
        jt_write(1, line, (unsigned)l);
    }

    fb_sel = 0;
    fb_draw();

    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        flags = 0;
        if (r == -11 /* -EAGAIN */) { jt_sched_yield(); continue; }
        if (r != 1) break;

        int old = fb_sel;
        if (ev.kind == JT_EV_CLICK) {
            if (ev.b >= FB_TOP && ev.a >= FB_LIST_X && ev.a < FB_LIST_X + FB_LIST_W) {
                int sel = (ev.b - FB_TOP) / FB_ITEM_H;
                if (sel < FB_COUNT) fb_sel = sel;
                else break;
            } else break; /* the titlebar X, or anywhere off the list */
        } else if (ev.kind == JT_EV_KEY) {
            if (ev.a == JT_KEY_ESC) break;
            if (ev.a == '`') {
                jt_write(1, "fieldbook: crashing on purpose\n", 31);
                *(volatile int *)0 = 1;
            }
            if (ev.a == JT_KEY_UP && fb_sel > 0) fb_sel--;
            else if (ev.a == JT_KEY_DOWN && fb_sel < FB_COUNT - 1) fb_sel++;
        } else {
            flags = JT_POLL_PRESENT;
            continue;
        }
        if (fb_sel != old) {
            /* One line, one write: the marker tools/checks/ring3fieldbook-check.py
               reads to confirm the real selection logic ran, not just the draw. */
            char line[28]; int l = 0;
            const char *pfx = "fieldbook: sel "; while (*pfx) line[l++] = *pfx++;
            l += utoa10((unsigned)fb_sel, line + l); line[l++] = '\n';
            jt_write(1, line, (unsigned)l);
        }
        fb_draw(); flags = JT_POLL_PRESENT;
    }
    jt_write(1, "fieldbook: closed\n", 18);
    jt_exit(0);
}
