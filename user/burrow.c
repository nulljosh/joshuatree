/* burrow: the Files app as a real ring-3 program.
 *
 * Same look and behavior as kernel/files.h: a List / Icons toolbar, the
 * current folder as an icon grid or a plain list, folders first, arrows
 * move the selection, Enter opens a folder, Backspace goes up, a click
 * selects (a second click on the selected folder opens it), Esc closes.
 * It keeps its own cwd string and hands it to SYS_READDIR, the way
 * user/search.c does, so nothing outside the app moves. Type is the
 * antialiased libjt face. The backquote key is the deliberate crash.
 *
 * Not reproduced (no syscall for it yet): the persisted view choice
 * (SETTINGS.TXT), opening a file in another app, drag to Trash.
 */
#include "jtsys.h"
#include "libjt/text.h"

#define BG     0x00FAF8F6
#define INK    0x001C1C1E
#define DIM    0x00807468
#define SEL    0x00EDE6DC
#define BTN    0x00EFEBE4
#define BTN_ON 0x00E5DCCC
#define BARK   0x00A8875A
#define CREAM  0x00F3EEE5
#define FOLD   0x00E5DCCC
#define LINES  0x00CFC4B2

#define TB_Y 36
#define TB_W 64
#define TB_H 22
#define LIST_Y0 76
#define LIST_H 18
#define TILE 84
#define ICON 40
#define ICON_TOP 76

extern char _user_end[];

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
static struct jt_dirent *raw JT_DATA = 0;
static int order[JT_READDIR_MAX] JT_DATA = {0};
static int count JT_DATA = 0;
static char cwd[JT_PATH_MAX + 1] JT_DATA = {0};
static int sel JT_DATA = 0;
static int view JT_DATA = 1; /* 0 list, 1 icons */

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

/* Fit a label to maxw pixels by cutting characters, so it never spills into the next tile. */
static void label(char *out, const struct jt_dirent *e, int maxw) {
    int n = 0;
    while (e->name[n] && n < JT_DIRENT_NAME - 2) { out[n] = e->name[n]; n++; }
    if (e->is_dir) out[n++] = '/';
    out[n] = 0;
    while (n > 1 && jt_text_width(JT_FACE_BODY, out) > maxw) out[--n] = 0;
}

static int tb_x(int i) { return 20 + i * (TB_W + 8); }
static int tb_at(int cx, int cy) {
    if (cy < TB_Y || cy >= TB_Y + TB_H) return -1;
    for (int i = 0; i < 2; i++) if (cx >= tb_x(i) && cx < tb_x(i) + TB_W) return i;
    return -1;
}
static int cols(void) { int w = ((int)win.width - 40) / TILE; return w < 1 ? 1 : w; }

/* Folders first, each group in the kernel's own listing order. */
static void reload(void) {
    int r = jt_readdir(cwd, raw, JT_READDIR_MAX);
    int n = r < 0 ? 0 : (r > JT_READDIR_MAX ? JT_READDIR_MAX : r);
    count = 0;
    for (int i = 0; i < n; i++) if (raw[i].is_dir) order[count++] = i;
    for (int i = 0; i < n; i++) if (!raw[i].is_dir) order[count++] = i;
    if (sel >= count) sel = count > 0 ? count - 1 : 0;
}

static void glyph(int x, int y, int size, int is_dir) {
    if (is_dir) {
        rect(x, y + size / 6, size * 2 / 5, size / 6, BARK);
        rect(x, y + size / 3, size, size - size / 3, CREAM);
        rect(x, y + size / 3, size, 2, BARK);
        rect(x, y + size - 2, size, 2, BARK);
        rect(x, y + size / 3, 2, size - size / 3, BARK);
        rect(x + size - 2, y + size / 3, 2, size - size / 3, BARK);
    } else {
        int fold = size / 4;
        rect(x, y, size, size, CREAM);
        rect(x, y, 2, size, BARK);
        rect(x + size - 2, y, 2, size, BARK);
        rect(x, y, size, 2, BARK);
        rect(x, y + size - 2, size, 2, BARK);
        rect(x + size - fold, y, fold, 2, FOLD);
        rect(x + size - fold, y, 2, fold, FOLD);
        for (int i = 0; i < 3; i++) rect(x + size / 5, y + size / 2 + i * 6, size * 3 / 5, 2, LINES);
    }
}

static void draw(void) {
    char lb[JT_DIRENT_NAME + 2];
    rect(0, 0, (int)win.width, (int)win.height, BG);
    const char *names[2] = {"List", "Icons"};
    for (int i = 0; i < 2; i++) {
        rect(tb_x(i), TB_Y, TB_W, TB_H, i == view ? BTN_ON : BTN);
        jt_text_draw(&win, i == view ? JT_FACE_BOLD : JT_FACE_BODY, tb_x(i) + 8, TB_Y + 3, INK, names[i]);
    }
    if (cwd[0]) text(cwd, tb_x(2) + 8, TB_Y + 3, DIM);
    if (!count) { text("(empty, or no filesystem mounted)", 20, LIST_Y0, DIM); }
    else if (view == 1) {
        int c = cols();
        for (int i = 0; i < count; i++) {
            int x = 20 + (i % c) * TILE, y = ICON_TOP + (i / c) * TILE;
            if (y + TILE > (int)win.height) break;
            const struct jt_dirent *e = &raw[order[i]];
            if (i == sel) rect(x - 4, y - 4, TILE - 8, TILE - 8, SEL);
            glyph(x + (TILE - 8 - ICON) / 2, y, ICON, (int)e->is_dir);
            label(lb, e, TILE - 8);
            text(lb, x, y + ICON + 6, INK);
        }
    } else {
        for (int i = 0; i < count; i++) {
            int y = LIST_Y0 + i * LIST_H;
            if (y + LIST_H > (int)win.height - 24) break;
            const struct jt_dirent *e = &raw[order[i]];
            if (i == sel) rect(16, y - 3, (int)win.width - 32, LIST_H, SEL);
            label(lb, e, (int)win.width - 48);
            text(lb, 20, y - 1, INK);
        }
    }
    text("1/2 view   arrows move   enter opens   backspace up   esc closes", 20, (int)win.height - 28, DIM);
}

static void open_dir(const char *name) {
    char saved[JT_PATH_MAX + 1];
    int l = 0;
    for (int i = 0; i <= JT_PATH_MAX; i++) saved[i] = cwd[i];
    while (cwd[l]) l++;
    if (l && l < JT_PATH_MAX) cwd[l++] = '/';
    for (const char *s = name; *s && l < JT_PATH_MAX; s++) cwd[l++] = *s;
    cwd[l] = 0;
    if (jt_readdir(cwd, raw, JT_READDIR_MAX) < 0) {
        for (int i = 0; i <= JT_PATH_MAX; i++) cwd[i] = saved[i];
    } else sel = 0;
    reload();
}
static void go_up(void) {
    int l = 0;
    while (cwd[l]) l++;
    while (l > 0 && cwd[l - 1] != '/') l--;
    if (l > 0) l--; /* drop the slash too; no slash left means back to the root */
    cwd[l] = 0;
    sel = 0;
    reload();
}
static void open_sel(void) {
    if (count && raw[order[sel]].is_dir) open_dir(raw[order[sel]].name);
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open(&win) != 0 || !win.pixels) { jt_write(2, "burrow: no window\n", 18); jt_exit(1); }
    raw = (struct jt_dirent *)(((unsigned)_user_end + 15u) & ~15u);
    cwd[0] = 0;
    reload();
    draw();
    jt_write(1, "burrow: ring-3 window\n", 22);
    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        flags = 0;
        if (r == -11) { jt_sched_yield(); continue; }
        if (r != 1) break;
        if (ev.kind == JT_EV_CLICK) {
            if (ev.a < 0 || ev.b < 0 || ev.a >= (int)win.width || ev.b >= (int)win.height) break; /* chrome X or dock */
            int tb = tb_at(ev.a, ev.b);
            if (tb >= 0) view = tb;
            else {
                int hit = -1;
                if (view == 1) {
                    int c = cols();
                    int col = (ev.a - 16) / TILE, row = (ev.b - (ICON_TOP - 4)) / TILE;
                    if (ev.a >= 16 && ev.b >= ICON_TOP - 4 && col < c) hit = row * c + col;
                } else if (ev.b >= LIST_Y0 - 3) hit = (ev.b - (LIST_Y0 - 3)) / LIST_H;
                if (hit >= 0 && hit < count) {
                    if (hit == sel) open_sel(); else sel = hit;
                }
            }
        } else if (ev.kind == JT_EV_KEY) {
            int k = ev.a;
            if (k == JT_KEY_ESC) break;
            if (k == '`') { jt_write(1, "burrow: crashing on purpose\n", 28); *(volatile int *)0 = 1; }
            int c = view == 1 ? cols() : 1;
            if (k == '1') view = 0;
            else if (k == '2') view = 1;
            else if (k == 8) go_up();
            else if (k == JT_KEY_ENTER) open_sel();
            else if (k == JT_KEY_UP) { if (sel - c >= 0) sel -= c; }
            else if (k == JT_KEY_DOWN) { if (sel + c < count) sel += c; }
            else if (k == JT_KEY_LEFT && view == 1) { if (sel > 0) sel--; }
            else if (k == JT_KEY_RIGHT && view == 1) { if (sel < count - 1) sel++; }
        } else { flags = JT_POLL_PRESENT; continue; }
        draw();
        flags = JT_POLL_PRESENT;
    }
    jt_write(1, "burrow: closed\n", 15);
    jt_exit(0);
}
