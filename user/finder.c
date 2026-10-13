/* finder: the Finder window as a ring-3 program, built today only for the ARM64 (Pi) desktop (3.14.0).
 *
 * A sidebar (Home, and Documents, Music and Trash when the card's JT/ folder has them), a file list with a small flat icon,
 * the name and the size per row, and a status line. Rows are 24 px, text is the house body face, nothing is chunky.
 * Everything reads through SYS_READDIR and SYS_READFILE, paths relative to the card's JT/ folder like Notes' paths.
 *
 * Keys: Up and Down choose, Enter opens a folder or previews a .TXT file read-only (Notes takes no path yet), Backspace goes
 * up a folder (or closes the preview), Left goes to the sidebar, Right and Tab back to the list, Esc closes (the preview
 * first). A click chooses a row, a second click on it opens it. With no card or no JT/ folder the list is what the build
 * carries itself, the apps, and the status line says so.
 * Serial markers (tools/checks/arm64-finder-check.py): finder: dir=PATH n=N, finder: select=NAME, finder: open=NAME,
 * finder: up, finder: preview=NAME bytes=N, finder: builtin, finder: closed.
 */
#include "jtsys.h"
#include "libjt/text.h"

#define BG     0x00FAF8F6
#define INK    0x001C1C1E
#define DIM    0x0075726E
#define FADE   0x00A29A90
#define SEL    0x00EDE6DC
#define SELOFF 0x00F4F0EA
#define SIDE   0x00F2EEE8
#define RULE   0x00E4DDD3
#define ACCENT 0x00B5502C
#define ACCENT_DK 0x008E3A1E
#define WHITE  0x00FFFFFF

#define SB_W   132
#define TOP_H  32
#define ROW_H  24
#define FOOT_H 22
#define MAX_E  64
#define PV_MAX 2048
#define PATH_MAX_LEN 62

extern char _user_bss_end[];   /* the arena goes past the .bss, which user_c.ld puts right after the image */
struct arena { struct jt_dirent de[MAX_E]; char pv[PV_MAX + 1]; };
static struct arena *ar;

static struct jt_window_info win;
static char cwd[PATH_MAX_LEN + 1];   /* relative to JT/, "" is the root */
static int nent, sel, top, builtin, focus_side, place, previewing, pv_len;
static char pv_name[32];
static int place_n;
static const char *place_name[4], *place_dir[4];
static int total_cut;

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int ieq(const char *a, const char *b) {
    for (;; a++, b++) { char x = *a, y = *b; if (x >= 'a' && x <= 'z') x -= 32; if (y >= 'a' && y <= 'z') y -= 32; if (x != y) return 0; if (!x) return 1; }
}
static void say(const char *s) { jt_write(1, s, (unsigned)slen(s)); }
static void say_num(int v) {
    char b[12]; int n = 0; if (v < 0) { jt_write(1, "-", 1); v = -v; }
    do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) jt_write(1, &b[--n], 1);
}
static int is_docs(const char *n) {   /* Documents is 9 letters, so an 8.3 card holds it as DOCUME~1 (or a short DOCS) */
    return ieq(n, "DOCUMENTS") || ieq(n, "DOCS") || (ieq(n, "DOCUME~1") || ieq(n, "DOCUME~2"));
}
static const char *pretty(const char *n) { return is_docs(n) ? "Documents" : n; }
static int has_ext(const char *name, const char *ext) {
    int n = slen(name), e = slen(ext);
    return n > e + 1 && name[n - e - 1] == '.' && ieq(name + n - e, ext);
}

/* ---- flat shapes ---- */
static void fill(int x, int y, int w, int h, unsigned c) {
    int W = (int)win.width, H = (int)win.height, pitch = (int)(win.pitch / 4);
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    for (int j = 0; j < h; j++) { unsigned *p = win.pixels + (y + j) * pitch + x; for (int i = 0; i < w; i++) p[i] = c; }
}
static void disc(int cx, int cy, int r, unsigned c) {
    for (int j = -r; j <= r; j++) for (int i = -r; i <= r; i++) if (i * i + j * j <= r * r) fill(cx + i, cy + j, 1, 1, c);
}
enum { K_DIR, K_TXT, K_MUSIC, K_IMG, K_FILE };
static int kind_of(const struct jt_dirent *d) {
    if (d->is_dir) return K_DIR;
    if (has_ext(d->name, "TXT") || has_ext(d->name, "MD")) return K_TXT;
    if (has_ext(d->name, "WAV") || has_ext(d->name, "MP3")) return K_MUSIC;
    if (has_ext(d->name, "PNG") || has_ext(d->name, "JPG") || has_ext(d->name, "JPEG")) return K_IMG;
    return K_FILE;
}
/* 16 x 16 glyphs, bold and flat, no text in them. */
static void icon(int x, int y, int kind) {
    switch (kind) {
    case K_DIR:
        fill(x, y + 2, 7, 4, ACCENT_DK);
        fill(x, y + 4, 16, 10, ACCENT);
        fill(x, y + 4, 16, 2, ACCENT_DK);
        break;
    case K_MUSIC:
        fill(x + 7, y + 1, 2, 11, ACCENT);
        fill(x + 7, y + 1, 7, 3, ACCENT);
        disc(x + 5, y + 12, 3, ACCENT);
        break;
    case K_IMG:
        fill(x, y + 2, 16, 12, ACCENT_DK);
        fill(x + 1, y + 3, 14, 10, 0x00F2E3D8);
        disc(x + 11, y + 6, 2, ACCENT);
        for (int i = 0; i < 6; i++) fill(x + 1 + i, y + 12 - i, 14 - 2 * i < 1 ? 1 : 8 - i, 1, ACCENT_DK);
        break;
    default:
        fill(x + 2, y + 1, 12, 14, FADE);
        fill(x + 3, y + 2, 10, 12, WHITE);
        fill(x + 10, y + 1, 4, 4, SEL);
        fill(x + 10, y + 1, 1, 4, FADE); fill(x + 10, y + 4, 4, 1, FADE);
        if (kind == K_TXT) { fill(x + 5, y + 7, 6, 1, FADE); fill(x + 5, y + 9, 6, 1, FADE); fill(x + 5, y + 11, 4, 1, FADE); }
        break;
    }
}
static void place_glyph(int x, int y, int p) {   /* a sidebar glyph, 14 px: Home, Documents, Music, Trash */
    const char *d = place_dir[p];
    if (!d[0]) {   /* Home: a house */
        fill(x + 2, y + 6, 10, 7, ACCENT);
        for (int i = 0; i < 6; i++) fill(x + 6 - i, y + i, 2 * i + 2, 1, ACCENT_DK);
        fill(x + 6, y + 9, 3, 4, SIDE);
    } else if (ieq(d, "MUSIC")) icon(x - 1, y - 1, K_MUSIC);
    else if (ieq(d, "TRASH")) {
        fill(x + 2, y + 2, 10, 2, ACCENT_DK); fill(x + 5, y, 4, 2, ACCENT_DK);
        fill(x + 3, y + 4, 8, 10, ACCENT);
        fill(x + 5, y + 6, 1, 6, SIDE); fill(x + 8, y + 6, 1, 6, SIDE);
    } else icon(x - 1, y - 1, K_DIR);
}

/* ---- text ---- */
static int fit(char *out, int cap, const char *s, int maxw) {   /* s cut with ... to fit maxw px */
    int n = 0;
    for (; s[n] && n < cap - 1; n++) out[n] = s[n];
    out[n] = 0;
    if (jt_text_width(JT_FACE_BODY, out) <= maxw) return n;
    while (n > 0) {
        n--;
        out[n] = '.'; out[n + 1] = '.'; out[n + 2] = '.'; out[n + 3] = 0;
        if (jt_text_width(JT_FACE_BODY, out) <= maxw) return n + 3;
    }
    out[0] = 0; return 0;
}
static void text_at(int face, int x, int cy, unsigned c, const char *s) {   /* vertically centred on cy */
    jt_text_draw(&win, face, x, cy - jt_text_height(face) / 2, c, s);
}
static void size_str(unsigned v, char *o) {
    char t[12]; int n = 0, k = 0; unsigned whole = v, frac = 0; const char *u = " B";
    if (v >= 1048576u) { whole = v >> 20; frac = ((v & 1048575u) * 10) >> 20; u = " MB"; }
    else if (v >= 1024u) { whole = v >> 10; frac = ((v & 1023u) * 10) >> 10; u = " KB"; }
    do { t[n++] = (char)('0' + whole % 10); whole /= 10; } while (whole);
    while (n) o[k++] = t[--n];
    if (frac && u[1] != 'B') { o[k++] = '.'; o[k++] = (char)('0' + frac); }
    for (int i = 0; u[i]; i++) o[k++] = u[i];
    o[k] = 0;
}

/* ---- listing ---- */
static const char *const BUILTIN[] = { "Calendar", "Notes", "Music", "Books" };
static void load(void) {
    nent = 0; sel = 0; top = 0; total_cut = 0;
    int n = jt_readdir(cwd, ar->de, MAX_E);
    if (n < 0) {
        builtin = 1;
        for (int i = 0; i < 4; i++) {
            struct jt_dirent *d = &ar->de[i];
            int k = 0; for (; BUILTIN[i][k]; k++) d->name[k] = BUILTIN[i][k];
            d->name[k] = 0; d->size = 0; d->is_dir = 0;
        }
        nent = 4;
        say("finder: builtin\n");
        return;
    }
    builtin = 0;
    if (n > MAX_E) { total_cut = n - MAX_E; n = MAX_E; }
    int m = 0;
    for (int i = 0; i < n; i++) {   /* drop . and .. */
        const char *s = ar->de[i].name;
        if (s[0] == '.' && (!s[1] || (s[1] == '.' && !s[2]))) continue;
        if (m != i) { unsigned char *a = (unsigned char *)&ar->de[m], *b = (unsigned char *)&ar->de[i]; for (unsigned k = 0; k < sizeof ar->de[0]; k++) a[k] = b[k]; }
        m++;
    }
    for (int i = 1; i < m; i++) {   /* folders first, then by name; insertion sort, byte swaps */
        for (int j = i; j > 0; j--) {
            struct jt_dirent *a = &ar->de[j - 1], *b = &ar->de[j];
            int swap = (b->is_dir && !a->is_dir);
            if (!swap && (!b->is_dir == !a->is_dir)) { const char *p = a->name, *q = b->name; while (*p && *p == *q) { p++; q++; } swap = (unsigned char)*q < (unsigned char)*p; }
            if (!swap) break;
            unsigned char *pa = (unsigned char *)a, *pb = (unsigned char *)b;
            for (unsigned k = 0; k < sizeof *a; k++) { unsigned char t = pa[k]; pa[k] = pb[k]; pb[k] = t; }
        }
    }
    nent = m;
    say("finder: dir="); say(cwd[0] ? cwd : "."); say(" n="); say_num(nent); say("\n");
}
static void find_places(void) {   /* the sidebar: Home always, the others when the root has the folder */
    place_n = 0;
    place_name[place_n] = "Home"; place_dir[place_n++] = "";
    static const char *const want[] = { "Documents", "Music", "Trash" };
    struct jt_dirent *d = ar->de;
    int n = jt_readdir("", d, MAX_E);
    if (n < 0) return;
    if (n > MAX_E) n = MAX_E;
    for (int w = 0; w < 3; w++)
        for (int i = 0; i < n; i++)
            if (d[i].is_dir && (w == 0 ? is_docs(d[i].name) : ieq(d[i].name, want[w]))) {
                static char dirs[3][12]; int k = 0;
                for (; d[i].name[k] && k < 11; k++) dirs[w][k] = d[i].name[k];
                dirs[w][k] = 0;
                place_name[place_n] = want[w]; place_dir[place_n++] = dirs[w];
                break;
            }
}
static void go_place(int p) {
    place = p; cwd[0] = 0;
    const char *s = place_dir[p]; int k = 0;
    for (; s[k] && k < PATH_MAX_LEN; k++) cwd[k] = s[k];
    cwd[k] = 0;
    previewing = 0;
    load();
}
static void join(char *out, const char *dir, const char *name) {
    int k = 0;
    for (; dir[k] && k < PATH_MAX_LEN; k++) out[k] = dir[k];
    if (k && k < PATH_MAX_LEN) out[k++] = '/';
    for (int i = 0; name[i] && k < PATH_MAX_LEN; i++) out[k++] = name[i];
    out[k] = 0;
}
static void go_up(void) {
    int k = slen(cwd);
    if (!k) return;
    while (k > 0 && cwd[k - 1] != '/') k--;
    if (k > 0) k--;
    cwd[k] = 0;
    say("finder: up\n");
    load();
}
static void open_sel(void) {
    if (builtin || sel < 0 || sel >= nent) return;
    struct jt_dirent *d = &ar->de[sel];
    say("finder: open="); say(d->name); say("\n");
    char path[PATH_MAX_LEN + 1];
    join(path, cwd, d->name);
    if (d->is_dir) { int k = 0; for (; path[k]; k++) cwd[k] = path[k]; cwd[k] = 0; load(); return; }
    if (kind_of(d) != K_TXT) return;
    int n = jt_readfile(path, ar->pv, PV_MAX);
    if (n < 0) { say("finder: preview failed\n"); return; }
    pv_len = n; ar->pv[n] = 0;
    for (int i = 0; i < n; i++) if ((unsigned char)ar->pv[i] < 32 && ar->pv[i] != '\n') ar->pv[i] = ' ';
    int k = 0; for (; d->name[k] && k < 31; k++) pv_name[k] = d->name[k];
    pv_name[k] = 0;
    previewing = 1;
    say("finder: preview="); say(pv_name); say(" bytes="); say_num(n); say("\n");
}

/* ---- drawing ---- */
static int visible(void) { return ((int)win.height - TOP_H - FOOT_H) / ROW_H; }
static void draw_preview(int x0) {
    int w = (int)win.width - x0 - 24, y = TOP_H + 14, lh = jt_text_height(JT_FACE_BODY) + 3;
    text_at(JT_FACE_BOLD, x0 + 12, y + 8, INK, pv_name);
    y += 26;
    fill(x0 + 12, y - 5, w, 1, RULE);
    char line[96]; int ln = 0;
    for (int i = 0; i <= pv_len && y + lh < (int)win.height - FOOT_H; i++) {
        char c = i < pv_len ? ar->pv[i] : '\n';
        int flush = (c == '\n');
        if (!flush) {
            line[ln] = c; line[ln + 1] = 0;
            if (ln >= 94 || jt_text_width(JT_FACE_BODY, line) > w) {   /* wrap at the last space when there is one */
                int cut = ln; while (cut > 0 && line[cut] != ' ') cut--;
                if (cut == 0) cut = ln;
                char keep[96]; int kn = 0;
                for (int k = cut + (line[cut] == ' '); k <= ln; k++) keep[kn++] = line[k];
                line[cut] = 0;
                text_at(JT_FACE_BODY, x0 + 12, y + lh / 2, INK, line); y += lh;
                for (int k = 0; k < kn; k++) line[k] = keep[k];
                ln = kn; line[ln] = 0;
                if (y + lh >= (int)win.height - FOOT_H) break;
                continue;
            }
            ln++;
        } else { line[ln] = 0; if (ln) text_at(JT_FACE_BODY, x0 + 12, y + lh / 2, INK, line); y += lh; ln = 0; }
    }
}
static void draw(void) {
    int W = (int)win.width, H = (int)win.height;
    fill(0, 0, W, H, BG);
    fill(0, 0, SB_W, H, SIDE);
    fill(SB_W, 0, 1, H, RULE);
    /* sidebar */
    text_at(JT_FACE_BODY, 12, 14, FADE, builtin ? "Built in" : "Places");
    for (int p = 0; p < place_n && !builtin; p++) {
        int y = TOP_H + p * 26;
        if (p == place) fill(6, y, SB_W - 12, 24, focus_side ? SEL : SELOFF);
        place_glyph(14, y + 5, p);
        text_at(JT_FACE_BODY, 36, y + 12, p == place ? INK : DIM, place_name[p]);
    }
    /* top bar: where we are */
    int x0 = SB_W + 1;
    fill(x0, TOP_H - 1, W - x0, 1, RULE);
    char title[64]; int k = 0;
    const char *base = cwd;
    for (int i = 0; cwd[i]; i++) if (cwd[i] == '/') base = cwd + i + 1;
    if (builtin) { const char *t = "Applications"; for (; t[k]; k++) title[k] = t[k]; }
    else if (!cwd[0]) { const char *t = place_name[0]; for (; t[k]; k++) title[k] = t[k]; }
    else { const char *pb = pretty(base); for (; pb[k] && k < 40; k++) title[k] = (pb != base || !k || pb[k] < 'A' || pb[k] > 'Z') ? pb[k] : (char)(pb[k] + 32); }
    title[k] = 0;
    if (k && !builtin && cwd[0] && title[0] >= 'a' && title[0] <= 'z') title[0] -= 32;
    text_at(JT_FACE_BOLD, x0 + 14, TOP_H / 2, INK, title);
    if (cwd[0] && !builtin) text_at(JT_FACE_BODY, W - 12 - jt_text_width(JT_FACE_BODY, cwd), TOP_H / 2, FADE, cwd);
    if (previewing) draw_preview(x0);
    else {
        int vis = visible();
        if (sel < top) top = sel;
        if (sel >= top + vis) top = sel - vis + 1;
        for (int r = 0; r < vis && top + r < nent; r++) {
            int i = top + r, y = TOP_H + r * ROW_H;
            struct jt_dirent *d = &ar->de[i];
            if (i == sel) fill(x0 + 6, y + 1, W - x0 - 12, ROW_H - 2, focus_side ? SELOFF : SEL);
            icon(x0 + 14, y + 4, kind_of(d));
            char nm[48]; fit(nm, sizeof nm, pretty(d->name), W - x0 - 14 - 22 - 90);
            text_at(JT_FACE_BODY, x0 + 38, y + ROW_H / 2, INK, nm);
            char sz[16];
            if (builtin) { sz[0] = 0; const char *t = "built in"; int q = 0; for (; t[q]; q++) sz[q] = t[q]; sz[q] = 0; }
            else if (d->is_dir) { sz[0] = '-'; sz[1] = '-'; sz[2] = 0; }
            else size_str(d->size, sz);
            text_at(JT_FACE_BODY, W - 16 - jt_text_width(JT_FACE_BODY, sz), y + ROW_H / 2, DIM, sz);
        }
        if (nent == 0) text_at(JT_FACE_BODY, x0 + 38, TOP_H + 20, FADE, "This folder is empty");
    }
    /* status line */
    fill(x0, H - FOOT_H, W - x0, 1, RULE);
    char st[80]; int q = 0;
    if (builtin) { const char *t = "No card or no JT folder: showing the apps built in"; for (; t[q]; q++) st[q] = t[q]; }
    else if (previewing) { const char *t = "Read only. Backspace to close"; for (; t[q]; q++) st[q] = t[q]; }
    else {
        int v = nent; char nb[12]; int n = 0;
        do { nb[n++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (n) st[q++] = nb[--n];
        const char *t = nent == 1 ? " item" : " items"; for (int i = 0; t[i]; i++) st[q++] = t[i];
    }
    st[q] = 0;
    text_at(JT_FACE_BODY, x0 + 14, H - FOOT_H / 2, DIM, st);
}

static void select_row(int i) {
    if (i < 0 || i >= nent) return;
    sel = i;
    say("finder: select="); say(ar->de[sel].name); say("\n");
}
/* Returns 1 when the window should close. */
static int on_key(int k) {
    if (k == JT_KEY_ESC) { if (previewing) { previewing = 0; return 0; } return 1; }
    if (previewing) { if (k == 8 || k == 127 || k == JT_KEY_LEFT || k == JT_KEY_ENTER) previewing = 0; return 0; }
    if (k == '`') { say("finder: crashing on purpose\n"); *(volatile int *)0 = 1; }
    if (k == '\t') { focus_side = !focus_side && !builtin; return 0; }
    if (k == JT_KEY_LEFT) { if (!builtin) focus_side = 1; return 0; }
    if (k == JT_KEY_RIGHT) { focus_side = 0; return 0; }
    if (focus_side) {
        if (k == JT_KEY_UP && place > 0) go_place(place - 1);
        else if (k == JT_KEY_DOWN && place + 1 < place_n) go_place(place + 1);
        else if (k == JT_KEY_ENTER) focus_side = 0;
        return 0;
    }
    if (k == JT_KEY_UP) select_row(sel > 0 ? sel - 1 : 0);
    else if (k == JT_KEY_DOWN) select_row(sel + 1 < nent ? sel + 1 : sel);
    else if (k == JT_KEY_ENTER) open_sel();
    else if (k == 8 || k == 127) go_up();
    return 0;
}
static void on_click(int x, int y) {
    if (previewing) return;
    if (x < SB_W) {
        int p = (y - TOP_H) / 26;
        if (!builtin && y >= TOP_H && p >= 0 && p < place_n) { focus_side = 1; go_place(p); }
        return;
    }
    focus_side = 0;
    if (y < TOP_H || y >= (int)win.height - FOOT_H) return;
    int i = top + (y - TOP_H) / ROW_H;
    if (i < 0 || i >= nent) return;
    if (i == sel) open_sel(); else select_row(i);
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open(&win) != 0 || !win.pixels) { say("finder: no window\n"); jt_exit(1); }
    ar = (struct arena *)(((unsigned long)_user_bss_end + 15ul) & ~15ul);
    say("finder: window "); say_num((int)win.width); say("x"); say_num((int)win.height); say("\n");
    find_places();
    go_place(0);
    draw();
    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        if (r == 1 && jt_window_resized(&ev, &win)) { draw(); flags = JT_POLL_PRESENT; continue; }
        flags = 0;
        if (r == -11) { jt_sched_yield(); continue; }
        if (r != 1) break;
        if (ev.kind == JT_EV_CLICK) {
            if (ev.a < 0 || ev.b < 0 || ev.a >= (int)win.width || ev.b >= (int)win.height) break;   /* the red dot */
            on_click(ev.a, ev.b);
        } else if (ev.kind == JT_EV_KEY) {
            if (on_key(ev.a)) break;
        }
        draw(); flags = JT_POLL_PRESENT;
    }
    say("finder: closed\n");
    jt_exit(0);
}
