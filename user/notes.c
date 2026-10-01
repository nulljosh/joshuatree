/* notes: the Notes browse view as a real ring-3 program (slice 1 of 3).
 *
 * Same look as gui_draw_notes_content in kernel/editor.h: a FOLDERS column
 * and a NOTES column, the focused column's header band lit, the selected row
 * shaded, each note shown by its first line. Notes live where the kernel
 * keeps them: NOTES/<folder>/N0000001.TXT, plain text, the first line is the
 * title. A ramfs boot has no directories, so there the one folder is "Notes"
 * and the notes are the root files N<digit>*.TXT and NOTES.TXT, exactly the
 * kernel's flat fallback.
 *
 * Keys: up/down pick, Enter on a folder goes into it, Backspace or left goes
 * back up, tab swaps columns, n makes a new empty note in the open folder,
 * Esc closes, backquote is the deliberate crash. Clicks select and never
 * close. Enter on a note is the slice 2 stub (the editor).
 * Serial markers: notes: folders=N notes=N, notes: new=FILE.
 */
#include "jtsys.h"
#include "libjt/text.h"

#define BG    0x00FAF8F6
#define INK   0x001C1C1E
#define DIM   0x0075726E
#define FADE  0x00A29A90
#define SEL   0x00EDE6DC
#define SELOFF 0x00F4F0EA
#define BAND  0x00EAE4DC
#define RULE  0x00E4DDD3

#define MAX_FOLDERS 16
#define MAX_NOTES 64
#define TITLE_MAX 40
#define FOLDER_W 150
#define LIST_W 220
#define COL_Y 40
#define ROW_H 22

struct nfolder { char name[9]; };
struct nnote { char file[13]; char title[TITLE_MAX]; };

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
extern char _user_end[];
/* Big buffers live past _user_end (a flat image has no .bss), like user/mail.c. */
struct arena { struct nfolder fo[MAX_FOLDERS]; struct nnote no[MAX_NOTES]; struct jt_dirent de[JT_READDIR_MAX]; char buf[512]; };
static struct arena *ar JT_DATA = 0;
static int nfo JT_DATA = 0, nno JT_DATA = 0;
static int fsel JT_DATA = 0, nsel JT_DATA = 0;
static int focus JT_DATA = 1;   /* 0 folders, 1 notes */
static int flat JT_DATA = 0;    /* ramfs: no directories */
static const char *note JT_DATA = 0;

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
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void scopy(char *d, const char *s, int max) {
    int i = 0;
    while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}
static int seq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static void say(const char *s, int n) {
    char b[64]; int l = 0;
    while (*s && l < 48) b[l++] = *s++;
    if (n >= 0) {
        char d[8]; int nd = 0, v = n;
        if (!v) d[nd++] = '0';
        while (v) { d[nd++] = (char)('0' + v % 10); v /= 10; }
        while (nd) b[l++] = d[--nd];
    }
    b[l++] = '\n';
    jt_write(1, b, (unsigned)l);
}

/* Path of the open folder's directory: "NOTES/<folder>" or "" (flat root). */
static void folder_path(char *out) {
    out[0] = 0;
    if (flat || !nfo) return;
    scopy(out, "NOTES/", 8);
    scopy(out + 6, ar->fo[fsel].name, 9);
}
static void note_path(char *out, const char *file) {
    folder_path(out);
    int l = slen(out);
    if (l) out[l++] = '/';
    scopy(out + l, file, 13);
}

static void load_folders(void) {
    nfo = 0;
    int n = jt_readdir("NOTES", ar->de, JT_READDIR_MAX);
    if (n < 0) {                       /* no directories on this boot */
        flat = 1;
        scopy(ar->fo[0].name, "Notes", 9);
        nfo = 1; fsel = 0;
        return;
    }
    flat = 0;
    if (n > JT_READDIR_MAX) n = JT_READDIR_MAX;
    for (int i = 0; i < n && nfo < MAX_FOLDERS; i++) {
        if (!ar->de[i].is_dir || ar->de[i].name[0] == '.') continue;
        scopy(ar->fo[nfo].name, ar->de[i].name, 9);
        nfo++;
    }
    if (!nfo) { scopy(ar->fo[0].name, "NOTES", 9); nfo = 1; }
    if (fsel >= nfo) fsel = nfo - 1;
    if (fsel < 0) fsel = 0;
}

static void load_notes(void) {
    char dir[24];
    nno = 0;
    folder_path(dir);
    int n = jt_readdir(dir[0] ? dir : ".", ar->de, JT_READDIR_MAX);
    if (n < 0) n = 0;
    if (n > JT_READDIR_MAX) n = JT_READDIR_MAX;
    for (int i = 0; i < n && nno < MAX_NOTES; i++) {
        const char *nm = ar->de[i].name;
        if (ar->de[i].is_dir) continue;
        if (flat) {
            if (!((nm[0] == 'N' && nm[1] >= '0' && nm[1] <= '9') || seq(nm, "NOTES.TXT"))) continue;
        } else if (nm[0] == '.') continue;
        scopy(ar->no[nno].file, nm, 13);
        nno++;
    }
    for (int i = 0; i < nno; i++) {
        char p[40];
        note_path(p, ar->no[i].file);
        int fd = jt_open(p, JT_O_RDONLY);
        int len = 0;
        if (fd >= 0) { len = jt_read(fd, ar->buf, 511); jt_close(fd); }
        if (len < 0) len = 0;
        int t = 0;
        while (t < len && ar->buf[t] != '\n' && t < TITLE_MAX - 1) { ar->no[i].title[t] = ar->buf[t]; t++; }
        ar->no[i].title[t] = 0;
        if (!t) scopy(ar->no[i].title, "(empty note)", TITLE_MAX);
    }
    if (nsel >= nno) nsel = nno - 1;
    if (nsel < 0) nsel = 0;
}

/* "N" + 7 digits + ".TXT", first index not already taken in the open folder. */
static void next_filename(char *out) {
    int idx = nno + 1;
    for (;;) {
        out[0] = 'N';
        int v = idx;
        for (int p = 7; p >= 1; p--) { out[p] = (char)('0' + v % 10); v /= 10; }
        out[8] = '.'; out[9] = 'T'; out[10] = 'X'; out[11] = 'T'; out[12] = 0;
        int hit = 0;
        for (int i = 0; i < nno; i++) if (seq(ar->no[i].file, out)) { hit = 1; break; }
        if (!hit) return;
        idx++;
    }
}

/* n: an empty note lands in the open folder, selected. */
static void new_note(void) {
    if (nno >= MAX_NOTES) { note = "This folder is full."; return; }
    char f[13], p[40];
    next_filename(f);
    note_path(p, f);
    int fd = jt_open(p, JT_O_WRONLY | JT_O_CREAT | JT_O_TRUNC);
    if (fd < 0) { note = "Could not create the note (is the folder there?)."; return; }
    jt_close(fd);
    load_notes();
    for (int i = 0; i < nno; i++) if (seq(ar->no[i].file, f)) { nsel = i; break; }
    focus = 1;
    say("notes: new=", -1);
    jt_write(1, f, (unsigned)slen(f));
    jt_write(1, "\n", 1);
    /* SLICE 2: opening the editor on this fresh note goes here, as it does
       in kernel/editor.h notes_new_note (it opens the editor right away). */
}

static void open_note(void) {
    if (!nno) return;
    /* SLICE 2 STUB: Enter on a note opens the editor on ar->no[nsel].file in
       the open folder (note_path). Slice 1 is browse only. */
    note = "The editor arrives in the next slice.";
}

static void fit(char *out, const char *s, int maxw) {
    scopy(out, s, TITLE_MAX);
    int n = slen(out);
    while (n > 1 && jt_text_width(JT_FACE_BODY, out) > maxw) out[--n] = 0;
}

static void draw_list(int x, int w, const char *title, int focused, int count, int sel) {
    int top = COL_Y + 24;
    if (focused) rect(x, top - 6, w, 18, BAND);
    text(title, x + 12, top - 3, DIM);
    if (!count) { text("(empty)", x + 12, top + 22, FADE); return; }
    for (int i = 0; i < count; i++) {
        int y = top + 22 + i * ROW_H;
        if (y + ROW_H > (int)win.height) break;
        if (i == sel) rect(x, y - 4, w, 20, focused ? SEL : SELOFF);
    }
}

static void draw(void) {
    char t[TITLE_MAX];
    rect(0, 0, (int)win.width, (int)win.height, BG);
    text("n new   tab switch   up/down pick   enter opens   backspace up   esc closes", 20, 10, DIM);
    draw_list(20, FOLDER_W, "FOLDERS", focus == 0, nfo, fsel);
    for (int i = 0; i < nfo; i++) text(ar->fo[i].name, 32, COL_Y + 46 + i * ROW_H - 4, INK);
    int nx = 20 + FOLDER_W + 16;
    draw_list(nx, LIST_W, "NOTES", focus == 1, nno, nsel);
    for (int i = 0; i < nno; i++) {
        fit(t, ar->no[i].title, LIST_W - 24);
        text(t, nx + 12, COL_Y + 46 + i * ROW_H - 4, INK);
    }
    rect(nx + LIST_W, COL_Y + 24, 1, (int)win.height - COL_Y - 40, RULE);
    text("select a note and press enter to write", nx + LIST_W + 24, COL_Y + 30, FADE);
    if (note) text(note, 20, (int)win.height - 28, DIM);
}

static void click(int x, int y) {
    int nx = 20 + FOLDER_W + 16;
    int row0 = COL_Y + 24 + 22 - 4;
    if (y < row0) return;
    int hit = (y - row0) / ROW_H;
    if (x >= 20 && x < 20 + FOLDER_W) {
        focus = 0;
        if (hit < nfo && hit != fsel) { fsel = hit; nsel = 0; load_notes(); }
    } else if (x >= nx && x < nx + LIST_W) {
        focus = 1;
        if (hit < nno) nsel = hit;
    }
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open(&win) != 0 || !win.pixels) { jt_write(2, "notes: no window\n", 17); jt_exit(1); }
    ar = (struct arena *)(((unsigned)_user_end + 15u) & ~15u);
    load_folders();
    load_notes();
    draw();
    jt_write(1, "notes: ring-3 window\n", 21);
    {
        char b[40]; int l = 0;
        const char *a = "notes: folders=";
        while (*a) b[l++] = *a++;
        b[l++] = (char)('0' + nfo / 10 % 10); b[l++] = (char)('0' + nfo % 10);
        a = " notes=";
        while (*a) b[l++] = *a++;
        b[l++] = (char)('0' + nno / 10 % 10); b[l++] = (char)('0' + nno % 10);
        b[l++] = '\n';
        jt_write(1, b, (unsigned)l);
    }
    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        flags = 0;
        if (r == -11) { jt_sched_yield(); continue; }
        if (r != 1) break;
        if (ev.kind == JT_EV_CLICK) {
            note = 0;
            click(ev.a, ev.b);
        } else if (ev.kind == JT_EV_KEY) {
            int k = ev.a;
            note = 0;
            if (k == '`') { jt_write(1, "notes: crashing on purpose\n", 27); *(volatile int *)0 = 1; }
            if (k == JT_KEY_ESC) break;
            else if (k == '\t') focus = !focus;
            else if (k == 'n') new_note();
            else if (focus == 0) {
                if (k == JT_KEY_UP && fsel > 0) { fsel--; nsel = 0; load_notes(); }
                else if (k == JT_KEY_DOWN && fsel < nfo - 1) { fsel++; nsel = 0; load_notes(); }
                else if (k == JT_KEY_ENTER || k == JT_KEY_RIGHT) focus = 1;
            } else {
                if (k == JT_KEY_UP && nsel > 0) nsel--;
                else if (k == JT_KEY_DOWN && nsel < nno - 1) nsel++;
                else if (k == 8 || k == JT_KEY_LEFT) focus = 0;
                else if (k == JT_KEY_ENTER) open_note();
            }
        } else { flags = JT_POLL_PRESENT; continue; }
        draw();
        flags = JT_POLL_PRESENT;
    }
    jt_write(1, "notes: closed\n", 14);
    jt_exit(0);
}
