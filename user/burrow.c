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
 * The List / Icons choice persists in BURROW.TXT ("view=0" or "view=1"),
 * read at start and rewritten on every change through the ordinary file
 * syscalls, so SETTINGS.TXT's other keys are never touched. Serial markers
 * (burrow: view=N, burrow: saved view=N, burrow: cwd=PATH n=COUNT) let the
 * ring-3 check follow it. Not reproduced: opening a file in another app,
 * drag to Trash.
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

/* A file name as the line(s) it needs. A short name stays on one line. A longer one breaks onto a second
   line, and if that still does not fit it keeps "..." and the tail, so the extension survives. With
   lines = 1 (the list view) nothing wraps: the end is cut and "..." added. Buffers are LABEL_BUF wide. */
#define LABEL_BUF (JT_DIRENT_NAME + 6)
static int fits_n(const char *s, int n, int maxw) {
    char t[LABEL_BUF]; for (int i = 0; i < n; i++) t[i] = s[i];
    t[n] = 0; return jt_text_width(JT_FACE_BODY, t) <= maxw;
}
static void label(char *l1, char *l2, const struct jt_dirent *e, int maxw, int lines) {
    char full[LABEL_BUF]; int n = 0;
    while (e->name[n] && n < JT_DIRENT_NAME - 1) { full[n] = e->name[n]; n++; }
    if (e->is_dir) full[n++] = '/';
    full[n] = 0; l1[0] = 0; l2[0] = 0;
    if (fits_n(full, n, maxw)) { for (int i = 0; i <= n; i++) l1[i] = full[i]; return; }
    if (lines == 1) {
        for (int a = n - 1; a >= 1; a--) {
            for (int i = 0; i < a; i++) l1[i] = full[i];
            l1[a] = '.'; l1[a + 1] = '.'; l1[a + 2] = '.'; l1[a + 3] = 0;
            if (jt_text_width(JT_FACE_BODY, l1) <= maxw) return;
        }
        return;
    }
    int a = 1; while (a < n && fits_n(full, a + 1, maxw)) a++;
    for (int d = a; d > 1; d--)                       /* prefer to break before the dot: "WEATHER" then ".TXT" */
        if (full[d] == '.') { a = d; break; }
    for (int i = 0; i < a; i++) l1[i] = full[i];
    l1[a] = 0;
    const char *rest = full + a; int rl = n - a;
    if (fits_n(rest, rl, maxw)) { for (int i = 0; i <= rl; i++) l2[i] = rest[i]; return; }
    for (int t = rl - 1; t >= 1; t--) {
        l2[0] = l2[1] = l2[2] = '.';
        for (int k = 0; k < t; k++) l2[3 + k] = rest[rl - t + k];
        l2[3 + t] = 0;
        if (jt_text_width(JT_FACE_BODY, l2) <= maxw) return;
    }
}

static int tb_x(int i) { return 20 + i * (TB_W + 8); }
static int tb_at(int cx, int cy) {
    if (cy < TB_Y || cy >= TB_Y + TB_H) return -1;
    for (int i = 0; i < 2; i++) if (cx >= tb_x(i) && cx < tb_x(i) + TB_W) return i;
    return -1;
}
static int cols(void) { int w = ((int)win.width - 40) / TILE; return w < 1 ? 1 : w; }

static void view_load(void) {
    char b[16];
    int fd = jt_open("BURROW.TXT", JT_O_RDONLY);
    if (fd < 0) return;
    int n = jt_read(fd, b, sizeof b - 1);
    jt_close(fd);
    if (n >= 6 && b[0] == 'v' && b[1] == 'i' && b[2] == 'e' && b[3] == 'w' && b[4] == '=' && (b[5] == '0' || b[5] == '1'))
        view = b[5] - '0';
}
static void set_view(int v) {
    if (v == view) return;
    view = v;
    char b[8] = {'v', 'i', 'e', 'w', '=', (char)('0' + v), '\n', 0};
    int fd = jt_open("BURROW.TXT", JT_O_WRONLY | JT_O_CREAT | JT_O_TRUNC);
    if (fd < 0) return;
    jt_write(fd, b, 7);
    jt_close(fd);
    jt_write(1, v ? "burrow: saved view=1\n" : "burrow: saved view=0\n", 21); /* one write: the kernel logs each write as its own serial line */
}
static void say_cwd(void) {
    char b[JT_PATH_MAX + 32];
    int l = 0;
    const char *h = "burrow: cwd=";
    while (*h) b[l++] = *h++;
    for (int i = 0; cwd[i] && l < JT_PATH_MAX + 12; i++) b[l++] = cwd[i];
    b[l++] = ' '; b[l++] = 'n'; b[l++] = '=';
    char d[8]; int nd = 0, v = count;
    if (!v) d[nd++] = '0';
    while (v) { d[nd++] = (char)('0' + v % 10); v /= 10; }
    while (nd) b[l++] = d[--nd];
    b[l++] = '\n';
    jt_write(1, b, (unsigned)l);
}

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
    char lb[LABEL_BUF], lb2[LABEL_BUF];
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
            label(lb, lb2, e, TILE - 8, 2);
            text(lb, x, y + ICON + 6, INK);
            if (lb2[0]) text(lb2, x, y + ICON + 6 + LIST_H - 2, INK);
        }
    } else {
        for (int i = 0; i < count; i++) {
            int y = LIST_Y0 + i * LIST_H;
            if (y + LIST_H > (int)win.height - 24) break;
            const struct jt_dirent *e = &raw[order[i]];
            if (i == sel) rect(16, y - 3, (int)win.width - 32, LIST_H, SEL);
            label(lb, lb2, e, (int)win.width - 48, 1);
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
    say_cwd();
}
static void go_up(void) {
    int l = 0;
    while (cwd[l]) l++;
    while (l > 0 && cwd[l - 1] != '/') l--;
    if (l > 0) l--; /* drop the slash too; no slash left means back to the root */
    cwd[l] = 0;
    sel = 0;
    reload();
    say_cwd();
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
    view_load();
    reload();
    draw();
    jt_write(1, "burrow: ring-3 window\n", 22);
    jt_write(1, view ? "burrow: view=1\n" : "burrow: view=0\n", 15);
    say_cwd();
    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        if (r == 1 && jt_window_resized(&ev, &win)) { draw(); flags = JT_POLL_PRESENT; continue; } /* JT_EV_RESIZE: remapped, repaint at the new size */
        flags = 0;
        if (r == -11) { jt_sched_yield(); continue; }
        if (r != 1) break;
        if (ev.kind == JT_EV_CLICK) {
            int tb = tb_at(ev.a, ev.b);
            if (tb >= 0) set_view(tb);
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
            if (k == '1') set_view(0);
            else if (k == '2') set_view(1);
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
