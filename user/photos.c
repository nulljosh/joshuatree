/* Photos (12.0): the pictures in the Files area's PHOTOS folder as a grid, one full size, and edits that never touch
 * the file, as in Apple's Photos.
 *
 * Grid: every .JPG, .JPEG and .PNG in PHOTOS/, sorted by name, four across. Arrows move, Enter or a click opens one.
 * View: the photo fitted to the window. Left and Right go to the previous and next, Esc back to the grid.
 * Edits, in the view: r turns it a quarter clockwise, b black and white, + and - the light in steps of 20 (-80 to 80),
 * 0 reverts. They are kept in PHOTOEDT.TXT in the Files area, one line a photo, "NAME ROT BW LIGHT", and applied
 * every time the photo is drawn, in the view and in the grid. The picture on the card is never rewritten.
 *
 * Decoding is libjt's copy of the kernel decoders (drivers/jpeg.c, drivers/png.c), the whole picture at once, so a
 * photo over MAX_PIXELS is listed but shown as too big. The grid keeps a small copy of each.
 *
 * Serial lines (tools/checks/arm64-photos-check.py reads them): photos=list n=N, photos=view NAME WxH,
 * photos=edit NAME rot=R bw=B light=L, photos=saved, photos: closed.
 */
#include "jtsys.h"
#include "libjt/text.h"
#include "libjt/stdlib.h"
#include "libjt/string.h"

int jpeg_decode(const unsigned char *data, unsigned int len, unsigned char **out, unsigned int *w, unsigned int *h, unsigned int *channels);
int png_decode(const unsigned char *data, unsigned int len, unsigned char **out, unsigned int *w, unsigned int *h, unsigned int *channels);

#define BG     0x00FAF8F6
#define INK    0x001C1C1E
#define DIM    0x0075726E
#define ACCENT 0x00B5502C
#define STAGE  0x00111111
#define PALE   0x00ECE6DE

#define DIR        "PHOTOS"
#define EDITS      "PHOTOEDT.TXT"
#define MAX_PHOTOS 32
#define MAX_FILE   (3u * 1024u * 1024u)
#define MAX_PIXELS (1024u * 768u)   /* the decoded RGB and our copy of it both fit the 8 MB heap */
#define THUMB      160
#define HEAD_H     30
#define COLS       4

struct pic { unsigned w, h; unsigned *px; };            /* 0x00RRGGBB, row after row */
struct photo { char name[JT_DIRENT_NAME]; struct pic thumb; int rot, bw, light, bad; };

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
static struct jt_dirent raw[JT_READDIR_MAX] JT_DATA;
static struct photo ph[MAX_PHOTOS] JT_DATA;
static int n JT_DATA = 0, sel JT_DATA = 0, viewing JT_DATA = 0;
static struct pic full JT_DATA = {0, 0, 0};
static int full_for JT_DATA = -1;

/* ---- small helpers ---------------------------------------------------- */
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
static int text(int face, const char *s, int x, int y, unsigned fg) { return jt_text_draw(&win, face, x, y, fg, s); }
static int put_u(char *b, int k, unsigned v) { char t[12]; int m = 0; do t[m++] = (char)('0' + v % 10); while (v /= 10); while (m) b[k++] = t[--m]; return k; }
static int put_i(char *b, int k, int v) { if (v < 0) { b[k++] = '-'; v = -v; } return put_u(b, k, (unsigned)v); }
static int put_s(char *b, int k, const char *s) { while (*s) b[k++] = *s++; return k; }
static void out(char *b, int k) { b[k++] = '\n'; jt_write(1, b, (unsigned)k); }
static int has_ext(const char *s, const char *e) {
    int l = 0, m = 0; while (s[l]) l++; while (e[m]) m++;
    if (l <= m) return 0;
    for (int i = 0; i < m; i++) if ((s[l - m + i] | 32) != e[i]) return 0;
    return 1;
}
static void path_of(const char *name, char *p) { int l = put_s(p, 0, DIR "/"); l = put_s(p, l, name); p[l] = 0; }

/* ---- decoding --------------------------------------------------------- */
static int decode(const char *name, struct pic *p) {   /* 0 on success; p->px is malloc'd */
    char path[JT_PATH_MAX + 1]; path_of(name, path);
    unsigned char *buf = malloc(MAX_FILE);
    if (!buf) return -1;
    int got = jt_readfile(path, buf, MAX_FILE);
    unsigned char *rgb = 0; unsigned w = 0, h = 0, ch = 0; int r = -1;
    if (got > 0) r = has_ext(name, ".png") ? png_decode(buf, (unsigned)got, &rgb, &w, &h, &ch)
                                         : jpeg_decode(buf, (unsigned)got, &rgb, &w, &h, &ch);
    free(buf);
    if (r != 0 || !rgb || !w || !h) { if (rgb) free(rgb); return -1; }
    if (w * h > MAX_PIXELS) { free(rgb); return -2; }
    p->px = malloc(w * h * 4u);
    if (!p->px) { free(rgb); return -1; }
    for (unsigned i = 0; i < w * h; i++) {
        const unsigned char *s = rgb + i * ch;
        unsigned R = s[0], G = ch >= 3 ? s[1] : s[0], B = ch >= 3 ? s[2] : s[0];
        p->px[i] = R << 16 | G << 8 | B;
    }
    free(rgb); p->w = w; p->h = h;
    return 0;
}
static void shrink(const struct pic *src, struct pic *dst) {   /* a copy at most THUMB on its long side, by box sampling */
    unsigned k = src->w > src->h ? (src->w + THUMB - 1) / THUMB : (src->h + THUMB - 1) / THUMB;
    if (!k) k = 1;
    dst->w = src->w / k ? src->w / k : 1; dst->h = src->h / k ? src->h / k : 1;
    dst->px = malloc(dst->w * dst->h * 4u);
    if (!dst->px) { dst->w = dst->h = 0; return; }
    for (unsigned y = 0; y < dst->h; y++)
        for (unsigned x = 0; x < dst->w; x++) {
            unsigned r = 0, g = 0, b = 0;
            for (unsigned j = 0; j < k; j++) for (unsigned i = 0; i < k; i++) {
                unsigned c = src->px[(y * k + j) * src->w + x * k + i]; r += c >> 16 & 255; g += c >> 8 & 255; b += c & 255;
            }
            dst->px[y * dst->w + x] = (r / (k * k)) << 16 | (g / (k * k)) << 8 | b / (k * k);
        }
}

/* ---- edits -------------------------------------------------------------- */
static unsigned edited(const struct photo *p, unsigned c) {
    int r = c >> 16 & 255, g = c >> 8 & 255, b = c & 255;
    if (p->bw) r = g = b = (r * 77 + g * 150 + b * 29) >> 8;
    r += p->light; g += p->light; b += p->light;
    r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
    return (unsigned)r << 16 | (unsigned)g << 8 | (unsigned)b;
}
static unsigned sample(const struct pic *s, int rot, unsigned u, unsigned v) {   /* pixel (u, v) of the picture turned rot quarters */
    unsigned x = rot == 0 ? u : rot == 1 ? v : rot == 2 ? s->w - 1 - u : s->w - 1 - v;
    unsigned y = rot == 0 ? v : rot == 1 ? s->h - 1 - u : rot == 2 ? s->h - 1 - v : u;
    return s->px[y * s->w + x];
}
static void draw_pic(const struct photo *p, const struct pic *s, int x0, int y0, int bw, int bh) {   /* fitted, centred */
    if (!s->px) return;
    unsigned W = p->rot & 1 ? s->h : s->w, H = p->rot & 1 ? s->w : s->h;
    unsigned dw = (unsigned)bw, dh = W ? (unsigned)((unsigned long)H * dw / W) : 0;
    if (dh > (unsigned)bh) { dh = (unsigned)bh; dw = (unsigned)((unsigned long)W * dh / H); }
    int ox = x0 + (bw - (int)dw) / 2, oy = y0 + (bh - (int)dh) / 2;
    for (unsigned y = 0; y < dh; y++) {
        int sy = oy + (int)y; if (sy < 0 || sy >= (int)win.height) continue;
        unsigned *row = win.pixels + (unsigned)sy * win.width;
        for (unsigned x = 0; x < dw; x++) {
            int sx = ox + (int)x; if (sx < 0 || sx >= (int)win.width) continue;
            row[sx] = edited(p, sample(s, p->rot, x * W / dw, y * H / dh));
        }
    }
}
static void load_edits(void) {
    static char buf[MAX_PHOTOS * 48];
    int got = jt_readfile(EDITS, buf, sizeof buf - 1);
    for (int i = 0; got > 0 && i < got;) {
        int e = i; while (e < got && buf[e] != '\n') e++;
        int s = i; while (s < e && buf[s] != ' ') s++;
        for (int k = 0; k < n; k++) {
            int l = 0; while (ph[k].name[l]) l++;
            if (l != s - i || memcmp(ph[k].name, buf + i, (unsigned)l)) continue;
            int v[3] = {0, 0, 0}, f = 0;
            for (int j = s; j < e && f < 3; f++) {
                while (j < e && buf[j] == ' ') j++;
                int neg = j < e && buf[j] == '-'; if (neg) j++;
                while (j < e && buf[j] >= '0' && buf[j] <= '9') v[f] = v[f] * 10 + buf[j++] - '0';
                if (neg) v[f] = -v[f];
            }
            ph[k].rot = v[0] & 3; ph[k].bw = v[1] != 0; ph[k].light = v[2] < -80 ? -80 : v[2] > 80 ? 80 : v[2];
        }
        i = e + 1;
    }
}
static void save_edits(void) {
    static char buf[MAX_PHOTOS * 48]; int k = 0;
    for (int i = 0; i < n; i++) {
        if (!ph[i].rot && !ph[i].bw && !ph[i].light) continue;
        k = put_s(buf, k, ph[i].name); buf[k++] = ' '; k = put_u(buf, k, (unsigned)ph[i].rot); buf[k++] = ' ';
        k = put_u(buf, k, (unsigned)ph[i].bw); buf[k++] = ' '; k = put_i(buf, k, ph[i].light); buf[k++] = '\n';
    }
    char l[40]; int m = put_s(l, 0, jt_writefile(EDITS, buf, (unsigned)k) == k ? "photos=saved" : "photos=save failed"); out(l, m);
}

/* ---- the library -------------------------------------------------------- */
static void load(void) {
    int r = jt_readdir(DIR, raw, JT_READDIR_MAX);
    for (int i = 0; i < r && n < MAX_PHOTOS; i++) {
        if (raw[i].is_dir || !(has_ext(raw[i].name, ".jpg") || has_ext(raw[i].name, ".jpeg") || has_ext(raw[i].name, ".png"))) continue;
        int k = n++;
        while (k > 0 && strcmp(ph[k - 1].name, raw[i].name) > 0) { ph[k] = ph[k - 1]; k--; }
        memset(&ph[k], 0, sizeof ph[k]); memcpy(ph[k].name, raw[i].name, JT_DIRENT_NAME);
    }
    for (int i = 0; i < n; i++) {
        struct pic big = {0, 0, 0};
        int r2 = decode(ph[i].name, &big);
        if (r2 == 0) { shrink(&big, &ph[i].thumb); free(big.px); } else ph[i].bad = r2 == -2 ? 2 : 1;
    }
    load_edits();
    char l[40]; int m = put_s(l, 0, "photos=list n="); m = put_u(l, m, (unsigned)n); out(l, m);
}
static void announce_edit(void) {
    const struct photo *p = &ph[sel]; char l[120]; int k = put_s(l, 0, "photos=edit "); k = put_s(l, k, p->name);
    k = put_s(l, k, " rot="); k = put_u(l, k, (unsigned)p->rot); k = put_s(l, k, " bw="); k = put_u(l, k, (unsigned)p->bw);
    k = put_s(l, k, " light="); k = put_i(l, k, p->light); out(l, k);
}
static void open_view(void) {
    if (!n) return;
    if (full_for != sel) {
        if (full.px) free(full.px);
        full.px = 0; full.w = full.h = 0; full_for = sel;
        if (!ph[sel].bad && decode(ph[sel].name, &full) != 0) ph[sel].bad = 1;
    }
    viewing = 1;
    char l[120]; int k = put_s(l, 0, "photos=view "); k = put_s(l, k, ph[sel].name); l[k++] = ' ';
    k = put_u(l, k, full.w); l[k++] = 'x'; k = put_u(l, k, full.h); out(l, k);
}

/* ---- drawing ------------------------------------------------------------ */
static void cell(int i, int *x, int *y, int *w, int *h) {
    int W = (int)win.width, gap = 12, cw = (W - gap * (COLS + 1)) / COLS;
    *w = cw; *h = cw * 3 / 4;
    *x = gap + (i % COLS) * (cw + gap); *y = HEAD_H + gap + (i / COLS) * (*h + gap);
}
static void draw(void) {
    int W = (int)win.width, H = (int)win.height;
    if (viewing) {
        rect(0, 0, W, H, STAGE);
        const struct photo *p = &ph[sel];
        text(JT_FACE_BOLD, p->name, 12, 8, 0x00F2ECE1);
        text(JT_FACE_BODY, "r turn   b black and white   + - light   0 revert   esc grid", 150, 8, 0x00A39C92);
        if (p->bad) text(JT_FACE_BODY, p->bad == 2 ? "This photo is too big to show." : "This photo will not open.", 12, HEAD_H + 20, 0x00F2ECE1);
        else draw_pic(p, &full, 12, HEAD_H, W - 24, H - HEAD_H - 12);
        return;
    }
    rect(0, 0, W, H, BG);
    text(JT_FACE_BOLD, "Photos", 12, 8, INK);
    if (!n) { text(JT_FACE_BODY, "No photos yet. Put .JPG or .PNG files in a PHOTOS folder in Files.", 12, HEAD_H + 20, DIM); return; }
    for (int i = 0; i < n; i++) {
        int x, y, w, h; cell(i, &x, &y, &w, &h);
        if (y > H) break;
        if (i == sel) rect(x - 3, y - 3, w + 6, h + 6, ACCENT);
        rect(x, y, w, h, PALE);
        if (ph[i].bad) text(JT_FACE_BODY, ph[i].name, x + 6, y + h / 2 - 6, DIM);
        else draw_pic(&ph[i], &ph[i].thumb, x, y, w, h);
    }
}

/* ---- input -------------------------------------------------------------- */
static void edit(int rot, int bw, int light, int revert) {
    struct photo *p = &ph[sel];
    if (revert) { p->rot = p->bw = p->light = 0; }
    p->rot = (p->rot + rot) & 3;
    if (bw) p->bw = !p->bw;
    p->light += light; if (p->light > 80) p->light = 80; if (p->light < -80) p->light = -80;
    announce_edit(); save_edits();
}
static void on_key(int k) {
    if (viewing) {
        if (k == JT_KEY_ESC) viewing = 0;
        else if (k == JT_KEY_LEFT && sel > 0) { sel--; open_view(); }
        else if (k == JT_KEY_RIGHT && sel + 1 < n) { sel++; open_view(); }
        else if (k == 'r') edit(1, 0, 0, 0);
        else if (k == 'b') edit(0, 1, 0, 0);
        else if (k == '+' || k == '=') edit(0, 0, 20, 0);
        else if (k == '-') edit(0, 0, -20, 0);
        else if (k == '0') edit(0, 0, 0, 1);
        return;
    }
    if (k == JT_KEY_ESC) { jt_write(1, "photos: closed\n", 15); jt_exit(0); }
    if (!n) return;
    if (k == JT_KEY_RIGHT && sel + 1 < n) sel++;
    else if (k == JT_KEY_LEFT && sel > 0) sel--;
    else if (k == JT_KEY_DOWN && sel + COLS < n) sel += COLS;
    else if (k == JT_KEY_UP && sel >= COLS) sel -= COLS;
    else if (k == JT_KEY_ENTER) open_view();
}
static void on_click(int x, int y) {
    if (viewing) return;
    for (int i = 0; i < n; i++) {
        int cx, cy, w, h; cell(i, &cx, &cy, &w, &h);
        if (x >= cx && x < cx + w && y >= cy && y < cy + h) { if (i == sel) open_view(); else sel = i; return; }
    }
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open(&win) != 0 || !win.pixels) { jt_write(2, "photos: no window\n", 18); jt_exit(1); }
    load();
    draw();
    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        if (r == 1 && jt_window_resized(&ev, &win)) { draw(); flags = JT_POLL_PRESENT; continue; }
        if (r == -11) { jt_sched_yield(); flags = 0; continue; }
        if (r != 1) break;
        if (ev.kind == JT_EV_KEY) on_key(ev.a);
        else if (ev.kind == JT_EV_CLICK) on_click(ev.a, ev.b);
        draw(); flags = JT_POLL_PRESENT;
    }
    jt_write(1, "photos: closed\n", 15);
    jt_exit(0);
}
