/* search: filter the current directory as you type, as a real ring-3 program.
 *
 * The sixteenth app to leave the kernel (roadmap 2.0), done the way
 * user/reminders.c was. Same job kernel/search.h did in ring 0: a query box,
 * a case-insensitive substring filter over the directory listing recomputed
 * on every keystroke, up/down or a click to pick, Enter on a folder to step
 * into it, Enter on a file to show its real bytes, Esc to close. Built with
 * no kernel include path, linked flat, loaded off the VFS by exec_user, and
 * it reaches the machine only through int 0x80: SYS_WINDOW_OPEN and
 * SYS_WINDOW_POLL for the window, the ordinary open/read/close for a file's
 * bytes, and the one new call this port needed, SYS_READDIR (1.9.12), which
 * fills fixed-size records with a directory's names, sizes and kinds.
 *
 * The in-kernel copy stepped into a folder with vfs_chdir, which moved the
 * shell's own current directory from inside an app. This one keeps its own
 * cwd string instead and hands it to SYS_READDIR and SYS_OPEN as a relative
 * path ("DOCS", then "DOCS/SUB"); the kernel walks in and back out inside
 * the call, so nothing outside the app moves. ramfs has no directories, so
 * on a diskless boot every row is a file, exactly as before.
 *
 * A flat binary has no .bss, so the record table and the file buffer live in
 * the zeroed pages just past _user_end, which the link script defines.
 *
 * Glyphs: the kernel's 8x16 VGA fallback font, same as the other ring-3
 * apps. The backquote key (`) is the deliberate crash: a write through a
 * null pointer, a page fault at ring 3, reaped by the kernel.
 * tools/checks/ring3search-check.py drives all of it, including the three
 * refusals probed at startup (a kernel pointer, an over-long path, a
 * directory that is not there), each one a line on the serial log.
 */
#include "jtsys.h"
#include "../drivers/vgafont.h"

#define BG    0x00FAF8F6 /* GUI_BG */
#define INK   0x001C1C1E
#define HINT  0x0075726E
#define DIM   0x00807468
#define SEL   0x00EDE6DC
#define WHITE 0x00FFFFFF
#define GREEN 0x00375A4A
#define RED   0x00A33B3B

#define QUERY_MAX 32
#define BOX_Y   44   /* the query box; the hint line sits at 20 */
#define LIST_Y  72   /* the result list starts here */
#define ROW_Y   78   /* first row's text y */
#define ROW_H   22
#define FILE_MAX 4096

extern char _user_end[];

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
static struct jt_dirent *ents JT_DATA = 0;   /* JT_READDIR_MAX records, past _user_end */
static char *filebuf JT_DATA = 0;            /* FILE_MAX + 1 bytes, after the records */
static int count JT_DATA = 0;
static char cwd[JT_PATH_MAX + 1] JT_DATA = {0};
static char query[QUERY_MAX] JT_DATA = {0};
static int qlen JT_DATA = 0;
static int matches[JT_READDIR_MAX] JT_DATA = {0};
static int nmatch JT_DATA = 0;
static int sel JT_DATA = 0;

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
    for (int r = 0; r < 16; r++)
        for (int c = 0; c < 8; c++) {
            if (!(g[r] & (0x80 >> c))) continue;
            int px = x + c, py = y + r;
            if (px < 0 || px >= (int)win.width || py < 0 || py >= (int)win.height) continue;
            win.pixels[(unsigned)py * win.width + (unsigned)px] = fg;
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
static void say(const char *pfx, unsigned v) { /* one line, one write: what the check reads */
    char line[64]; int l = 0;
    while (*pfx) line[l++] = *pfx++;
    l += utoa10(v, line + l); line[l++] = '\n';
    jt_write(1, line, (unsigned)l);
}
static void says(const char *pfx, const char *s) { /* same, with a string */
    char line[120]; int l = 0;
    while (*pfx) line[l++] = *pfx++;
    while (*s && l < 110) line[l++] = *s++;
    line[l++] = '\n';
    jt_write(1, line, (unsigned)l);
}

static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* Real substring match, case-insensitive. An empty query matches everything,
   the same "type nothing, see the full list" the in-kernel copy started from. */
static int contains_ci(const char *hay, const char *needle) {
    if (!needle[0]) return 1;
    for (int i = 0; hay[i]; i++) {
        int j = 0;
        while (needle[j] && lower((unsigned char)hay[i + j]) == lower((unsigned char)needle[j])) j++;
        if (!needle[j]) return 1;
    }
    return 0;
}

/* One SYS_READDIR for the directory cwd names. Returns the kernel's answer
   (the entry count, or -errno); count is what fits. */
static int reload(void) {
    int r = jt_readdir(cwd, ents, JT_READDIR_MAX);
    count = r < 0 ? 0 : (r > JT_READDIR_MAX ? JT_READDIR_MAX : r);
    int dirs = 0;
    for (int i = 0; i < count; i++) dirs += ents[i].is_dir ? 1 : 0;
    {   char line[64]; int l = 0;
        const char *pfx = "search: listed ";
        while (*pfx) line[l++] = *pfx++;
        l += utoa10((unsigned)count, line + l);
        pfx = " dirs "; while (*pfx) line[l++] = *pfx++;
        l += utoa10((unsigned)dirs, line + l); line[l++] = '\n';
        jt_write(1, line, (unsigned)l);
    }
    return r;
}

static void refilter(void) {
    query[qlen] = 0;
    nmatch = 0;
    for (int i = 0; i < count; i++)
        if (contains_ci(ents[i].name, query)) matches[nmatch++] = i;
    if (sel >= nmatch) sel = nmatch > 0 ? nmatch - 1 : 0;
    if (sel < 0) sel = 0;
    say("search: matches ", (unsigned)nmatch);
}

static int rows_fit(void) {
    int n = 0;
    while (ROW_Y + n * ROW_H + 20 <= (int)win.height - 16) n++;
    return n;
}

/* Chrome drawn once per screen: the hint line never changes while typing. */
static void draw_chrome(void) {
    rect(0, 0, (int)win.width, (int)win.height, BG);
    text("type to filter   up/down to pick   enter opens   esc closes", 20, 20, HINT);
}

/* Only the query box and the result list, the chrome/content split the
   in-kernel copy had so filtering as you type never repaints everything. */
static void draw_content(void) {
    int w = (int)win.width;
    rect(20, BOX_Y, w - 40, 20, WHITE);
    query[qlen] = 0;
    text(query, 24, BOX_Y + 2, INK);
    rect(16, LIST_Y, w - 32, (int)win.height - LIST_Y - 16, BG);
    jt_write(1, "searchcontent\n", 14); /* one per redraw: the discriminating marker the check counts */
    if (!count) { text("(no files, or no filesystem mounted)", 20, ROW_Y, DIM); return; }
    if (!nmatch) { text("No matches.", 20, ROW_Y, DIM); return; }
    int fit = rows_fit();
    for (int r = 0; r < nmatch && r < fit; r++) {
        int y = ROW_Y + r * ROW_H;
        struct jt_dirent *e = &ents[matches[r]];
        if (r == sel) rect(16, y - 4, w - 32, 20, SEL);
        char label[JT_DIRENT_NAME + 2];
        int i = 0;
        while (e->name[i] && i < JT_DIRENT_NAME - 1) { label[i] = e->name[i]; i++; }
        if (e->is_dir) label[i++] = '/';
        label[i] = 0;
        text(label, 28, y, e->is_dir ? GREEN : INK);
    }
}

/* Read a file whole through open/read/close (read hands back at most 255
   bytes a call) and show it wrapped at word boundaries in the 8x16 font, the
   way `cat` would. Esc or a click goes back to the list. */
static void path_join(char *out, const char *leaf) {
    int l = 0;
    for (const char *s = cwd; *s && l < JT_PATH_MAX; s++) out[l++] = *s;
    if (l && l < JT_PATH_MAX) out[l++] = '/';
    for (const char *s = leaf; *s && l < JT_PATH_MAX; s++) out[l++] = *s;
    out[l] = 0;
}
static void show_file(const char *name) {
    char path[JT_PATH_MAX + 1];
    path_join(path, name);
    says("search: open ", path);
    int n = -1;
    int fd = jt_open(path, JT_O_RDONLY);
    if (fd >= 0) {
        n = 0;
        for (;;) {
            int got = jt_read(fd, filebuf + n, (unsigned)(FILE_MAX - n) > 255u ? 255u : (unsigned)(FILE_MAX - n));
            if (got <= 0 || n >= FILE_MAX) break;
            n += got;
        }
        jt_close(fd);
    }
    rect(0, 0, (int)win.width, (int)win.height, BG);
    text(name, 20, 20, INK);
    if (n < 0) {
        text("Could not read this file.", 20, 50, RED);
        jt_write(1, "search: file unreadable\n", 24);
    } else {
        filebuf[n] = 0;
        say("search: file bytes ", (unsigned)n);
        {   char head[48]; int h = 0;
            while (h < 40 && filebuf[h] && filebuf[h] != '\n' && filebuf[h] != '\r') { head[h] = filebuf[h]; h++; }
            head[h] = 0;
            says("search: head ", head);
        }
        int right = 20 + (((int)win.width - 40) / 8) * 8, x = 20, y = 44, bottom = (int)win.height - 40;
        int i = 0;
        while (filebuf[i] && y + 16 <= bottom) {
            char c = filebuf[i];
            if (c == '\r') { i++; continue; }
            if (c == '\n') { x = 20; y += 18; i++; continue; }
            if (c == ' ') { x += 8; i++; if (x + 8 > right) { x = 20; y += 18; } continue; }
            int wl = 0;
            while (filebuf[i + wl] && filebuf[i + wl] != ' ' && filebuf[i + wl] != '\n' && filebuf[i + wl] != '\r') wl++;
            if (x > 20 && x + wl * 8 > right) { x = 20; y += 18; } /* a word that fits on a fresh line moves there whole */
            for (int k = 0; k < wl && y + 16 <= bottom; k++) {
                if (x + 8 > right) { x = 20; y += 18; if (y + 16 > bottom) break; } /* longer than a line: cut it */
                glyph((unsigned char)filebuf[i + k], x, y, INK); x += 8;
            }
            i += wl;
        }
    }
    text("esc or click to go back", 20, (int)win.height - 30, HINT);
    jt_write(1, "searchfile\n", 11);
    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        flags = 0;
        if (r == -11 /* -EAGAIN */) { jt_sched_yield(); continue; }
        if (r != 1) return;
        if (ev.kind == JT_EV_CLICK) return;
        if (ev.kind == JT_EV_KEY && ev.a == JT_KEY_ESC) return;
    }
}

/* Enter on a folder: try the longer path first, adopt it only if the kernel
   lists it. A refusal leaves the listing exactly where it was. */
static void enter_dir(const char *name) {
    char saved[JT_PATH_MAX + 1];
    for (int i = 0; i <= JT_PATH_MAX; i++) saved[i] = cwd[i];
    path_join(cwd, name);
    says("search: dir ", name);
    if (reload() < 0) {
        for (int i = 0; i <= JT_PATH_MAX; i++) cwd[i] = saved[i];
        jt_write(1, "search: dir refused\n", 20);
        reload();
    } else {
        says("search: cwd ", cwd);
    }
    qlen = 0; sel = 0;
    refilter();
}

/* The three refusals the privilege boundary owes, probed once so the check
   can read them off the serial log. Each line ends in the positive errno. */
static void probe(void) {
    char longpath[80];
    for (int i = 0; i < 79; i++) longpath[i] = 'A';
    longpath[79] = 0;
    int r = jt_readdir("", (struct jt_dirent *)0xC0100000u, 1); /* kernel text: not user memory */
    say("search: probe efault ", (unsigned)(-r));
    r = jt_readdir(longpath, ents, 1);
    say("search: probe einval ", (unsigned)(-r));
    r = jt_readdir("NOSUCHDIR", ents, 1);
    say("search: probe enoent ", (unsigned)(-r));
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open(&win) != 0 || !win.pixels) {
        jt_write(2, "search: no window\n", 18);
        jt_exit(1);
    }
    {   /* one line, one write: what tools/checks/ring3search-check.py asserts on */
        char line[48]; int l = 0;
        const char *pfx = "search: ring-3 window ";
        while (*pfx) line[l++] = *pfx++;
        l += utoa10(win.width, line + l); line[l++] = 'x';
        l += utoa10(win.height, line + l); line[l++] = '\n';
        jt_write(1, line, (unsigned)l);
    }

    ents = (struct jt_dirent *)(((unsigned)_user_end + 15u) & ~15u);
    filebuf = (char *)(ents + JT_READDIR_MAX);
    probe();
    cwd[0] = 0;
    reload();
    qlen = 0; sel = 0;
    refilter();
    draw_chrome();
    draw_content();

    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        flags = 0;
        if (r == -11 /* -EAGAIN */) { jt_sched_yield(); continue; }
        if (r != 1) break;

        int open_sel = 0;
        if (ev.kind == JT_EV_CLICK) {
            /* A hit on a row opens it like Enter; a miss closes the app, the
               same "click anywhere else dismisses" every list app here has. */
            int hit = -1, fit = rows_fit();
            for (int i = 0; i < nmatch && i < fit; i++) {
                int y = ROW_Y + i * ROW_H;
                if (ev.a >= 16 && ev.a < (int)win.width - 16 && ev.b >= y - 4 && ev.b < y + 16) { hit = i; break; }
            }
            if (hit < 0) break;
            sel = hit; open_sel = 1;
        } else if (ev.kind == JT_EV_KEY) {
            int k = ev.a;
            if (k == JT_KEY_ESC) break;
            if (k == '`') {
                jt_write(1, "search: crashing on purpose\n", 28);
                *(volatile int *)0 = 1;
            }
            if (k == JT_KEY_UP) { if (sel > 0) sel--; }
            else if (k == JT_KEY_DOWN) { if (sel < nmatch - 1) sel++; }
            else if (k == 8) { if (qlen > 0) { qlen--; refilter(); } }
            else if (k == JT_KEY_ENTER) open_sel = 1;
            else if (k >= 32 && k < 127 && qlen < QUERY_MAX - 1) { query[qlen++] = (char)k; refilter(); }
        } else { flags = JT_POLL_PRESENT; continue; }

        if (open_sel && nmatch) {
            struct jt_dirent *e = &ents[matches[sel]];
            if (e->is_dir) enter_dir(e->name);
            else { show_file(e->name); draw_chrome(); }
        }
        draw_content(); flags = JT_POLL_PRESENT;
    }
    jt_write(1, "search: closed\n", 15);
    jt_exit(0);
}
