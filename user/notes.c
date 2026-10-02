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
 * close. Enter on a note opens the editor (slice 2) in the same window.
 *
 * Editor (slice 2): the whole file is read into a 4 KB buffer (the kernel's
 * editor_buffer size), laid out with word wrap exactly like editor_layout in
 * kernel/editor.h (56 px margins, wrap at the first letter of a word that no
 * longer fits), caret 2 px wide in 0x85144B, ink on the 0xFAF8F6 page,
 * 14 px line pitch of 33. Keys that reach a ring-3 app through
 * SYS_WINDOW_POLL: typing, Backspace, Enter, the four arrows, Esc, and
 * KEY_COPY/CUT/PASTE (302..304). Copy and cut take the current logical line
 * (the kernel editor's own contract) into an in-app clipboard; paste inserts
 * it. Esc saves (truncate-write, only if edited) and returns to browse.
 * Home, End and Delete work in the editor; Ctrl+S saves and stays in it.
 * Browse: f makes a folder (F0000001 style names, 8.3), d deletes the
 * selected note. SYS_MKDIR and SYS_UNLINK do the work.
 * Serial markers: notes: folders=N notes=N, notes: new=FILE, notes: edit=FILE,
 * notes: saved=N.
 */
#include "jtsys.h"
#include "libjt/text.h"
#include "libjt/osk.h"

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
#define KEY_COPY 302
#define KEY_CUT 303
#define KEY_PASTE 304

struct nfolder { char name[9]; };
struct nnote { char file[13]; char title[TITLE_MAX]; };

extern char _user_end[];
static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
/* Big buffers live past _user_end (a flat image has no .bss), like user/mail.c. */
#define ED_MAX 4096
#define CLIP_MAX 256
#define ED_MARGIN 56
#define ED_TOP 28
#define ED_LH 33
#define ED_CARET 0x0085144B
struct arena { struct nfolder fo[MAX_FOLDERS]; struct nnote no[MAX_NOTES]; struct jt_dirent de[JT_READDIR_MAX]; char buf[512];
    char ed[ED_MAX + 1]; unsigned short lx[ED_MAX + 1], ll[ED_MAX + 1]; unsigned char adv[96]; char clip[CLIP_MAX]; int osk_said; };
static struct arena *ar JT_DATA = 0;
static int nfo JT_DATA = 0, nno JT_DATA = 0;
static int fsel JT_DATA = 0, nsel JT_DATA = 0;
static int focus JT_DATA = 1;   /* 0 folders, 1 notes */
static int flat JT_DATA = 0;    /* ramfs: no directories */
static const char *note JT_DATA = 0;
static int editing JT_DATA = 0, elen JT_DATA = 0, epos JT_DATA = 0, escroll JT_DATA = 0;
static int phone JT_DATA = 0;   /* argv[1] == "phone": show the on-screen keyboard in the editor */
static int edirty JT_DATA = 0, goalx JT_DATA = -1, clen JT_DATA = 0;
static char efile[13] JT_DATA = {0};

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

static void edit_open(const char *file);

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


/* ---- editor (slice 2) ---- */
static int ed_adv(unsigned char c) {
    if (c < 32 || c > 126) c = '?';
    return ar->adv[c - 32];
}

/* Fills lx/ll for every index 0..elen: where the caret sits before that
   char. Same wrap rules as editor_layout in kernel/editor.h. */
static void ed_layout(void) {
    int x = ED_MARGIN, line = 0, word_start = 1;
    int limit = (int)win.width - ED_MARGIN;
    for (int i = 0; i <= elen; i++) {
        unsigned char c = (unsigned char)ar->ed[i];
        int a = ed_adv(c);
        if (word_start && x > ED_MARGIN && c != ' ' && c != '\n' && i < elen) {
            int w = 0;
            for (int j = i; j < elen; j++) {
                unsigned char cj = (unsigned char)ar->ed[j];
                if (cj == ' ' || cj == '\n') break;
                w += ed_adv(cj);
                if (x + w > limit) break;
            }
            if (x + w > limit && w <= limit - ED_MARGIN) { x = ED_MARGIN; line++; }
        }
        word_start = (c == ' ');
        if (x + a > limit && c != '\n') { x = ED_MARGIN; line++; }
        ar->lx[i] = (unsigned short)x; ar->ll[i] = (unsigned short)line;
        if (i == elen) break;
        if (c == '\n') { x = ED_MARGIN; line++; continue; }
        x += a;
    }
}

static int ed_visible(void) {
    int n = ((int)win.height - ED_TOP - 30 - (phone ? jt_osk_height() : 0)) / ED_LH;
    return n < 1 ? 1 : n;
}

static void ed_draw(void) {
    rect(0, 0, (int)win.width, (int)win.height, BG);
    ed_layout();
    int vis = ed_visible(), cl = ar->ll[epos];
    if (cl < escroll) escroll = cl;
    if (cl >= escroll + vis) escroll = cl - vis + 1;
    for (int i = 0; i < elen; i++) {
        int l = ar->ll[i];
        char c = ar->ed[i];
        if (l < escroll) continue;
        if (l >= escroll + vis) break;
        if (c < 33 || c > 126) continue;
        char g[2] = {c, 0};
        jt_text_draw(&win, JT_FACE_BODY, ar->lx[i], ED_TOP + (l - escroll) * ED_LH + 6, INK, g);
    }
    rect(ar->lx[epos], ED_TOP + (cl - escroll) * ED_LH + 2, 2, 24, ED_CARET);
    text(edirty ? "Notes *" : "Notes", 20, 4, DIM);
    if (phone) { jt_osk_draw(&win); if (!ar->osk_said) { ar->osk_said = 1; jt_write(1, "notes: osk shown\n", 17); } return; }
    text(note ? note : (edirty ? "Edited   |   Esc saves and goes back to Notes" : "Esc: back to Notes"),
         20, (int)win.height - 28, DIM);
}

static void edit_open(const char *file) {
    char p[40];
    scopy(efile, file, 13);
    note_path(p, efile);
    elen = 0;
    int fd = jt_open(p, JT_O_RDONLY);
    if (fd >= 0) {
        for (;;) {
            int r = jt_read(fd, ar->ed + elen, (unsigned)(ED_MAX - elen));
            if (r <= 0) break;
            elen += r;
            if (elen >= ED_MAX) break;
        }
        jt_close(fd);
    }
    ar->ed[elen] = 0;
    epos = elen; escroll = 0; edirty = 0; goalx = -1; editing = 1; note = 0;
    say("notes: edit=", -1);
    jt_write(1, efile, (unsigned)slen(efile));
    jt_write(1, "\n", 1);
}

static int edit_save(void) {
    char p[40];
    note_path(p, efile);
    int fd = jt_open(p, JT_O_WRONLY | JT_O_CREAT | JT_O_TRUNC);
    if (fd < 0) { note = "Save failed."; return 0; }
    int off = 0;
    while (off < elen) {
        int w = jt_write(fd, ar->ed + off, (unsigned)(elen - off));
        if (w <= 0) break;
        off += w;
    }
    jt_close(fd);
    say("notes: saved=", off);
    edirty = 0;
    return 1;
}

static void edit_close(void) {
    if (edirty) edit_save();
    editing = 0;
    load_notes();
    for (int i = 0; i < nno; i++) if (seq(ar->no[i].file, efile)) { nsel = i; break; }
}

static void ed_insert(const char *s, int n) {
    if (n <= 0 || elen + n > ED_MAX - 1) return;
    for (int i = elen; i >= epos; i--) ar->ed[i + n] = ar->ed[i];
    for (int i = 0; i < n; i++) ar->ed[epos + i] = s[i];
    elen += n; epos += n; edirty = 1; goalx = -1;
}

static void ed_remove(int at, int n) {
    if (n <= 0 || at < 0 || at + n > elen) return;
    for (int i = at; i + n <= elen; i++) ar->ed[i] = ar->ed[i + n];
    elen -= n; edirty = 1; goalx = -1;
    if (epos > at) epos = epos >= at + n ? epos - n : at;
}

static void ed_vertical(int dir) {
    ed_layout();
    int target = (int)ar->ll[epos] + dir;
    if (target < 0 || target > (int)ar->ll[elen]) return;
    if (goalx < 0) goalx = ar->lx[epos];
    int best = -1, bd = 1 << 30;
    for (int i = 0; i <= elen; i++) {
        if ((int)ar->ll[i] != target) continue;
        int d = (int)ar->lx[i] - goalx;
        if (d < 0) d = -d;
        if (d < bd) { bd = d; best = i; }
    }
    if (best >= 0) epos = best;
}

static void ed_line_bounds(int *s, int *e) {
    int a = epos, b = epos;
    while (a > 0 && ar->ed[a - 1] != '\n') a--;
    while (b < elen && ar->ed[b] != '\n') b++;
    *s = a; *e = b;
}

static void ed_key(int k) {
    if (k == JT_KEY_LEFT) { if (epos > 0) epos--; goalx = -1; }
    else if (k == JT_KEY_RIGHT) { if (epos < elen) epos++; goalx = -1; }
    else if (k == JT_KEY_UP) ed_vertical(-1);
    else if (k == JT_KEY_DOWN) ed_vertical(1);
    else if (k == 8) { if (epos > 0) { epos--; ed_remove(epos, 1); } }
    else if (k == JT_KEY_ENTER) ed_insert("\n", 1);
    else if (k >= 32 && k <= 126) { char c = (char)k; ed_insert(&c, 1); }
    else if (k == KEY_COPY || k == KEY_CUT) {
        int s, e; ed_line_bounds(&s, &e);
        clen = e - s > CLIP_MAX ? CLIP_MAX : e - s;
        for (int i = 0; i < clen; i++) ar->clip[i] = ar->ed[s + i];
        if (k == KEY_CUT) { ed_remove(s, e - s + (e < elen ? 1 : 0)); epos = s; }
    }
    else if (k == KEY_PASTE) ed_insert(ar->clip, clen);
    else if (k == JT_KEY_HOME) { int s, e; ed_line_bounds(&s, &e); epos = s; goalx = -1; }
    else if (k == JT_KEY_END) { int s, e; ed_line_bounds(&s, &e); epos = e; goalx = -1; }
    else if (k == JT_KEY_DELETE) { if (epos < elen) ed_remove(epos, 1); }
    else if (k == JT_KEY_SAVE) { if (edirty) { if (edit_save()) note = "Saved."; } else note = "Saved."; }
}

/* n: an empty note lands in the open folder, selected. */
static void new_note(void) {
    if (nno >= MAX_NOTES) { note = "This folder is full."; return; }
    char f[13], p[40];
    next_filename(f);
    note_path(p, f);
    if (!flat) { char d[24]; jt_mkdir("NOTES"); folder_path(d); jt_mkdir(d); } /* harmless when they exist */
    int fd = jt_open(p, JT_O_WRONLY | JT_O_CREAT | JT_O_TRUNC);
    if (fd < 0) { note = "Could not create the note (is the folder there?)."; return; }
    jt_close(fd);
    load_notes();
    for (int i = 0; i < nno; i++) if (seq(ar->no[i].file, f)) { nsel = i; break; }
    focus = 1;
    say("notes: new=", -1);
    jt_write(1, f, (unsigned)slen(f));
    jt_write(1, "\n", 1);
    edit_open(f);
}

/* f: a new folder NOTES/Fnnnnnn, made (with NOTES itself) when missing. */
static void new_folder(void) {
    if (flat) { note = "This boot has no folders."; return; }
    if (nfo >= MAX_FOLDERS) { note = "Too many folders."; return; }
    jt_mkdir("NOTES"); /* fails harmlessly when it is already there */
    char name[9], p[24];
    for (int idx = nfo + 1; idx < 10000; idx++) {
        int v = idx;
        name[0] = 'F';
        for (int q = 4; q >= 1; q--) { name[q] = (char)('0' + v % 10); v /= 10; }
        name[5] = 0;
        int hit = 0;
        for (int i = 0; i < nfo; i++) if (seq(ar->fo[i].name, name)) { hit = 1; break; }
        if (hit) continue;
        scopy(p, "NOTES/", 8);
        scopy(p + 6, name, 9);
        if (jt_mkdir(p) != 0) { note = "Could not make the folder."; return; }
        break;
    }
    load_folders();
    for (int i = 0; i < nfo; i++) if (seq(ar->fo[i].name, name)) { fsel = i; break; }
    nsel = 0;
    load_notes();
    say("notes: folder=", -1);
    jt_write(1, name, (unsigned)slen(name));
    jt_write(1, "\n", 1);
}

/* d: delete the selected note. */
static void delete_note(void) {
    if (!nno) return;
    char p[40];
    note_path(p, ar->no[nsel].file);
    if (jt_unlink(p) != 0) { note = "Could not delete the note."; return; }
    say("notes: deleted=", -1);
    jt_write(1, ar->no[nsel].file, (unsigned)slen(ar->no[nsel].file));
    jt_write(1, "\n", 1);
    load_notes();
}

static void open_note(void) {
    if (!nno) return;
    edit_open(ar->no[nsel].file);
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
    text("n new   f folder   d delete   tab switch   up/down pick   enter opens   backspace up   esc closes", 20, 10, DIM);
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
    phone = argc > 1 && seq(argv[1], "phone");
    if (jt_window_open(&win) != 0 || !win.pixels) { jt_write(2, "notes: no window\n", 17); jt_exit(1); }
    ar = (struct arena *)(((unsigned)_user_end + 15u) & ~15u); /* past the image, inside the 128KB window */
    for (int c = 32; c < 127; c++) { char g[2] = {(char)c, 0}; ar->adv[c - 32] = (unsigned char)jt_text_width(JT_FACE_BODY, g); }
    ar->adv[0] = (unsigned char)(ar->adv[0] ? ar->adv[0] : 4);
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
        if (r == 1 && jt_window_resized(&ev, &win)) { draw(); flags = JT_POLL_PRESENT; continue; } /* JT_EV_RESIZE: remapped, repaint at the new size */
        flags = 0;
        if (r == -11) { jt_sched_yield(); continue; }
        if (r != 1) break;
        if (editing) {
            int key = ev.a;
            if (phone && ev.kind == JT_EV_CLICK) { key = jt_osk_hit(&win, ev.a, ev.b); if (!key) { flags = JT_POLL_PRESENT; continue; } if (key > 32 && key < 127) { char m[20] = "notes: osk key X\n"; m[15] = (char)key; jt_write(1, m, 17); } }
            else if (ev.kind != JT_EV_KEY) { flags = JT_POLL_PRESENT; continue; }
            note = 0;
            ev.a = key;
            if (ev.a == JT_KEY_ESC) { edit_close(); draw(); }
            else { ed_key(ev.a); ed_draw(); }
            flags = JT_POLL_PRESENT;
            continue;
        }
        if (ev.kind == JT_EV_CLICK) {
            note = 0;
            click(ev.a, ev.b);
        } else if (ev.kind == JT_EV_KEY) {
            int k = ev.a;
            note = 0;
            if (k == '`') { jt_write(1, "notes: crashing on purpose\n", 27); *(volatile int *)0 = 1; }
            if (k == JT_KEY_ESC) break;
            else if (k == '\t') focus = !focus;
            else if (k == 'n') { new_note(); if (editing) { ed_draw(); flags = JT_POLL_PRESENT; continue; } }
            else if (k == 'f') new_folder();
            else if (k == 'd') delete_note();
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
