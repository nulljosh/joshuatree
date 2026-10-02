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
#include "../drivers/vgafont.h"

#define BG    0x00FAF8F6 /* GUI_BG */
#define ROW   0x00F1EDE7
#define SELBG 0x00E2D8CC
#define INK   0x001C1C1E
#define HINT  0x0075726E

#define FB_LIST_X 20
#define FB_LIST_W 220
#define FB_INFO_X 260
#define FB_TOP    56
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
static void glyph(unsigned char ch, int x, int y, unsigned fg) {
    if (ch < VGAFONT_FIRST || ch > VGAFONT_LAST) ch = '?';
    const unsigned char *g = vgafont_glyphs + (ch - VGAFONT_FIRST) * 16;
    for (int r = 0; r < 16; r++) {
        int py = y + r;
        if (py < 0 || py >= (int)win.height) continue;
        for (int c = 0; c < 8; c++) {
            int px = x + c;
            if (px < 0 || px >= (int)win.width) continue;
            if (g[r] & (0x80 >> c)) win.pixels[(unsigned)py * win.width + (unsigned)px] = fg;
        }
    }
}
static void text(const char *s, int x, int y, unsigned fg) {
    for (; *s; s++, x += 8) glyph((unsigned char)*s, x, y, fg);
}
static int utoa10(unsigned v, char *buf) {
    char tmp[12]; int tn = 0, n = 0;
    do { tmp[tn++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (tn) buf[n++] = tmp[--tn];
    buf[n] = 0;
    return n;
}

/* Greedy word wrap by 8px cells, same idea as render_wrapped_text. */
static void fb_wrap(const char *s, int x, int y, int w, int ymax, unsigned fg) {
    int cols = w / 8;
    while (*s && y + 16 <= ymax) {
        while (*s == ' ') s++;
        int n = 0, brk = -1;
        while (s[n] && n < cols) { if (s[n] == ' ') brk = n; n++; }
        if (s[n] && brk > 0) n = brk;
        for (int i = 0; i < n; i++) glyph((unsigned char)s[i], x + i * 8, y, fg);
        s += n; y += 20;
    }
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
        text(rank, FB_LIST_X + 6, y + 6, fg);
        /* Trim by cell count so a long title ends in "..." inside its row. */
        const char *name = FB_FIELDS[i].name;
        int room = (FB_LIST_W - 36) / 8, len = 0;
        while (name[len]) len++;
        if (len <= room) text(name, FB_LIST_X + 30, y + 6, fg);
        else {
            for (int k = 0; k < room - 3; k++) glyph((unsigned char)name[k], FB_LIST_X + 30 + k * 8, y + 6, fg);
            text("...", FB_LIST_X + 30 + (room - 3) * 8, y + 6, fg);
        }
    }
    const FbField *f = &FB_FIELDS[fb_sel];
    text(f->name, FB_INFO_X, FB_TOP, INK);
    text(f->studies, FB_INFO_X, FB_TOP + 20, HINT);
    int wy = FB_TOP + 44;
    fb_wrap(f->explanation, FB_INFO_X, wy, (int)win.width - FB_INFO_X - 24, (int)win.height - 50, INK);
    text("up/down or click to select   esc closes", 20, (int)win.height - 30, HINT);
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
        if (r == 1 && jt_window_resized(&ev, &win)) { fb_draw(); flags = JT_POLL_PRESENT; continue; } /* JT_EV_RESIZE: remapped, repaint at the new size */
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
