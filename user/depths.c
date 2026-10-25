/* depths: Depths, a first-person maze shooter in the spirit of Doom, as a ring-3 program. All of it is ours: the
 * raycaster, the maze, the creatures and the colours.
 *
 * A grid raycaster in fixed point (positions in 1/65536 of a tile, a 256-step circle, a quarter sine table), one ray
 * per two logical columns, walls shaded by distance and by which face was hit. Creatures are billboards drawn after
 * the walls against a depth buffer, so a wall hides them. They walk toward you when they can see you (no wall on the
 * straight line between) and bite when they reach you. Up and Down (or w and s) walk, Left and Right (or a and d) turn, space fires at whatever is in the
 * crosshair. Find the pale door to get out. Space after the end plays again, esc closes.
 *
 * Same clock as user/paddle.c: one step per kernel tick from SYS_TASKS. A frame is rendered only when something
 * changed, at most every third tick.
 *
 * tools/checks/ring3-depths-check.py fires at the creature ahead of you at the start and expects "depths: hit 1",
 * and watches a turn and a step change the picture.
 */
#include "jtsys.h"
#include "libjt/text.h"
#include "libjt/hires.h"

#define CEIL  0x002A2724
#define FLOOR 0x005A534B
#define WALL  0x00B5502C
#define WALL2 0x008E3A1E
#define DOOR  0x00FAF8F4
#define BODY  0x006B4423
#define HURT  0x00FAF8F4
#define EYES  0x00F4D35E
#define INK   0x001C1C1E
#define HINT  0x00B3ADA0
#define HUD   0x00141311

#define MW 16
#define MH 16
#define ONE 65536
#define K 10813          /* the camera plane, 0.66 in Q14: about a 66 degree view */
#define CS 2             /* logical columns per ray */
#define HUDH 30
#define MAXE 8
#define MAXCOL 1024

static const char MAP[MH][MW + 1] = {
    "################",
    "#P...M.#.......#",
    "#.####.#.#####.#",
    "#.#....#.....#.#",
    "#.#.####.###.#.#",
    "#...#..M...#...#",
    "###.#.####.#.###",
    "#...#.#..#.#...#",
    "#.###.#..#.###.#",
    "#.....#.M#.....#",
    "#.#####..#####.#",
    "#.#....M.....#.#",
    "#.#.###..###.#.#",
    "#...#......#..E#",
    "#.....#.M......#",
    "################",
};
static const short SIN[64] = {
    0, 402, 804, 1205, 1606, 2006, 2404, 2801, 3196, 3590, 3981, 4370, 4756, 5139, 5520, 5897,
    6270, 6639, 7005, 7366, 7723, 8076, 8423, 8765, 9102, 9434, 9760, 10080, 10394, 10702, 11003, 11297,
    11585, 11866, 12140, 12406, 12665, 12916, 13160, 13395, 13623, 13842, 14053, 14256, 14449, 14635, 14811, 14978,
    15137, 15286, 15426, 15557, 15679, 15791, 15893, 15986, 16069, 16143, 16207, 16261, 16305, 16340, 16364, 16379,
};
/* sqrt(1 - (i/16)^2) * 256: the outline of a creature, column by column */
static const short PROF[17] = {256, 255, 254, 251, 247, 242, 236, 228, 222, 212, 200, 186, 170, 150, 124, 89, 0};

struct foe { int x, y, alive, flash, bite; };
static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
static struct foe foes[MAXE] JT_DATA = {{0, 0, 0, 0, 0}};
static int zbuf[MAXCOL / CS] JT_DATA = {0};
static int nfoes JT_DATA = 0, px JT_DATA = 0, py JT_DATA = 0, ang JT_DATA = 0, health JT_DATA = 0, kills JT_DATA = 0;
static int over JT_DATA = 0, flash JT_DATA = 0, dirty JT_DATA = 1, awake JT_DATA = 0;   /* awake: you walked, turned or fired */

static void say(const char *s) { unsigned n = 0; while (s[n]) n++; jt_write(1, s, n); }
static int utoa10(unsigned v, char *buf) {
    char tmp[12]; int tn = 0, n = 0;
    do { tmp[tn++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (tn) buf[n++] = tmp[--tn];
    buf[n] = 0;
    return n;
}
static void say_n(const char *pfx, int v) {
    char line[48]; int l = 0;
    while (*pfx) line[l++] = *pfx++;
    l += utoa10((unsigned)v, line + l); line[l++] = '\n'; line[l] = 0;
    say(line);
}
static unsigned now(void) { struct jt_tasks t = {0, 0, 0, 0, 0}; jt_tasks(&t, 0); return t.ticks; }
static int iabs(int v) { return v < 0 ? -v : v; }
static int s64(int i) { return i >= 64 ? 16384 : SIN[i]; }
static int sinq(int a) { a &= 255; return a < 64 ? s64(a) : a < 128 ? s64(128 - a) : a < 192 ? -s64(a - 128) : -s64(256 - a); }
static int cosq(int a) { return sinq(a + 64); }
static char at(int x, int y) { return (x < 0 || y < 0 || x >= MW || y >= MH) ? '#' : MAP[y][x]; }
static int solid(int x, int y) { char c = at(x, y); return c == '#' || c == 'E'; }

static int vw(void) { return (int)win.width > MAXCOL ? MAXCOL : (int)win.width; }
static int vh(void) { return (int)win.height - HUDH; }
static void rect(int x, int y, int w, int h, unsigned c) { if (w > 0 && h > 0) jt_fill(&win, x, y, w, h, c); }
static int text(const char *s, int x, int y, unsigned fg, int face) { return jt_text_draw(&win, face, x, y, fg, s); }
static unsigned shade(unsigned c, int f) {   /* f out of 256 */
    return ((((c >> 16) & 255) * (unsigned)f >> 8) << 16) | ((((c >> 8) & 255) * (unsigned)f >> 8) << 8) | ((c & 255) * (unsigned)f >> 8);
}

static void start(void) {
    nfoes = 0; kills = 0; health = 100; over = 0; ang = 0; flash = 0; awake = 0;
    for (int y = 0; y < MH; y++) for (int x = 0; x < MW; x++) {
        if (MAP[y][x] == 'P') { px = x * ONE + ONE / 2; py = y * ONE + ONE / 2; }
        if (MAP[y][x] == 'M' && nfoes < MAXE) { struct foe *f = &foes[nfoes++]; f->x = x * ONE + ONE / 2; f->y = y * ONE + ONE / 2; f->alive = 1; f->flash = 0; f->bite = 0; }
    }
    dirty = 1;
}

/* The camera: dir is the way you face, plane the screen's half-width, both Q14. */
static int dirx(void) { return cosq(ang); }
static int diry(void) { return sinq(ang); }
static int planex(void) { return sinq(ang) * K >> 14; }
static int planey(void) { return -cosq(ang) * K >> 14; }

static void cast(void) {
    int W = vw(), H = vh(), dx = dirx(), dy = diry(), plx = planex(), ply = planey();
    for (int x = 0, col = 0; x < W; x += CS, col++) {
        int cam = ((2 * x + CS - W) << 14) / W;
        int rdx = dx + (plx * cam >> 14), rdy = dy + (ply * cam >> 14);
        int mx = px >> 16, my = py >> 16, stx, sty, side = 0;
        int ddx = rdx ? iabs((1 << 30) / rdx) : 0, ddy = rdy ? iabs((1 << 30) / rdy) : 0;
        int sdx, sdy;
        if (!rdx) { sdx = 0x7FFFFFFF; stx = 0; }
        else if (rdx < 0) { stx = -1; sdx = (int)((long long)(px - (mx << 16)) * ddx >> 16); }
        else { stx = 1; sdx = (int)((long long)(((mx + 1) << 16) - px) * ddx >> 16); }
        if (!rdy) { sdy = 0x7FFFFFFF; sty = 0; }
        else if (rdy < 0) { sty = -1; sdy = (int)((long long)(py - (my << 16)) * ddy >> 16); }
        else { sty = 1; sdy = (int)((long long)(((my + 1) << 16) - py) * ddy >> 16); }
        for (int i = 0; i < 64; i++) {
            if (sdx < sdy) { sdx += ddx; mx += stx; side = 0; } else { sdy += ddy; my += sty; side = 1; }
            if (solid(mx, my)) break;
        }
        int perp = side == 0 ? sdx - ddx : sdy - ddy;
        if (perp < 4096) perp = 4096;
        zbuf[col] = perp;
        int lh = (H << 16) / perp;
        if (lh > H * 4) lh = H * 4;
        int y0 = H / 2 - lh / 2, y1 = y0 + lh;
        int f = 256 - (perp >> 12) * 2; if (f < 70) f = 70;
        unsigned base = at(mx, my) == 'E' ? DOOR : side ? WALL2 : WALL;
        rect(x, 0, CS, y0, CEIL);
        rect(x, y0 < 0 ? 0 : y0, CS, (y1 > H ? H : y1) - (y0 < 0 ? 0 : y0), shade(base, f));
        rect(x, y1, CS, H - y1, FLOOR);
    }
}

/* Where a foe falls on screen: its screen x, its depth (Q16) and its size, or 0 when it is behind you. */
static int project(const struct foe *f, int *sx, int *ty, int *sh) {
    int W = vw(), H = vh(), spx = f->x - px, spy = f->y - py;
    int tx = (int)(((long long)diry() * spx - (long long)dirx() * spy) >> 14) / 1000 * 1515;
    int t = (int)(((long long)(-planey()) * spx + (long long)planex() * spy) >> 14) / 1000 * 1515;
    if (t < 8192) return 0;
    int r = tx / (t >> 12);   /* Q12, -4096..4096 across the screen */
    if (iabs(r) > 8192) return 0;
    *sx = W / 2 + (W / 2 * r >> 12); *ty = t; *sh = (H << 16) / t;
    return 1;
}

static void sprites(void) {
    int W = vw(), H = vh(), order[MAXE], depth[MAXE], n = 0;
    for (int i = 0; i < nfoes; i++) {
        int sx, ty, sh;
        if (foes[i].alive && project(&foes[i], &sx, &ty, &sh)) { order[n] = i; depth[n] = ty; n++; }
    }
    for (int a = 0; a < n; a++) for (int b = a + 1; b < n; b++) if (depth[b] > depth[a]) {   /* far first */
        int t = depth[a]; depth[a] = depth[b]; depth[b] = t; t = order[a]; order[a] = order[b]; order[b] = t;
    }
    for (int k = 0; k < n; k++) {
        struct foe *f = &foes[order[k]];
        int sx, ty, sh;
        project(f, &sx, &ty, &sh);
        int r = sh / 4, cy = H / 2 + sh / 4;
        if (r < 1) continue;
        unsigned c = f->flash ? HURT : BODY;
        for (int x = (sx - r) / CS * CS; x <= sx + r; x += CS) {
            if (x < 0 || x >= W || ty >= zbuf[x / CS]) continue;
            int u = iabs(x - sx) * 16 / r; if (u > 16) continue;
            int hh = r * PROF[u] >> 8;
            rect(x, cy - hh, CS, 2 * hh, c);
        }
        int e = r / 5 < 2 ? 2 : r / 5, ey = cy - r / 3;
        for (int s = -1; s <= 1; s += 2) {
            int ex = sx + s * r / 3 - e / 2;
            if (ex >= 0 && ex < W && ty < zbuf[ex / CS]) rect(ex, ey, e, e, EYES);
        }
    }
}

static void hud(void) {
    int W = (int)win.width, H = vh();
    char num[12];
    /* the crosshair and the gun */
    rect(W / 2 - 6, H / 2, 13, 1, DOOR); rect(W / 2, H / 2 - 6, 1, 13, DOOR);
    rect(W / 2 - 14, H - 34, 28, 34, INK); rect(W / 2 - 5, H - 52, 10, 20, INK);
    if (flash) rect(W / 2 - 8, H - 64, 16, 12, EYES);
    rect(0, H, W, HUDH, HUD);
    int x = text("Health ", 14, H + 7, HINT, JT_FACE_BODY);
    utoa10((unsigned)(health < 0 ? 0 : health), num); x = text(num, x, H + 7, DOOR, JT_FACE_BOLD);
    x = text("   Down ", x, H + 7, HINT, JT_FACE_BODY);
    utoa10((unsigned)kills, num); x = text(num, x, H + 7, DOOR, JT_FACE_BOLD);
    x = text(" of ", x, H + 7, HINT, JT_FACE_BODY); utoa10((unsigned)nfoes, num); text(num, x, H + 7, DOOR, JT_FACE_BOLD);
    const char *msg = over == 1 ? "You got out. Space plays again." : over == 2 ? "They got you. Space plays again."
                    : "arrows walk and turn   space fires   find the pale door";
    text(msg, W - 14 - jt_text_width(JT_FACE_BODY, msg), H + 7, over == 1 ? 0x0097C9A0 : HINT, JT_FACE_BODY);
}
static void render(void) { cast(); sprites(); hud(); dirty = 0; }

static void walk(int sign) {
    int step = sign * (ONE / 5), nx = px + (int)((long long)dirx() * step >> 14), ny = py + (int)((long long)diry() * step >> 14);
    int m = ONE / 5, ex = nx + (nx > px ? m : -m), ey = ny + (ny > py ? m : -m);
    if (at(ex >> 16, py >> 16) == 'E' || at(px >> 16, ey >> 16) == 'E') { over = 1; say("depths: escaped\n"); return; }
    if (!solid(ex >> 16, py >> 16)) px = nx;
    if (!solid(px >> 16, ey >> 16)) py = ny;
}
/* Whether a creature can see you: the straight line between you, sampled every quarter tile, crosses no wall. */
static int sees(const struct foe *f) {
    int dx = px - f->x, dy = py - f->y, n = (iabs(dx) > iabs(dy) ? iabs(dx) : iabs(dy)) / (ONE / 4) + 1;
    for (int i = 1; i < n; i++) if (solid((f->x + dx / n * i) >> 16, (f->y + dy / n * i) >> 16)) return 0;
    return 1;
}
static void fire(void) {   /* the nearest creature in front of you, inside the crosshair's cone (wider up close), that no wall hides */
    int W = vw(), best = -1, bestd = 0x7FFFFFFF, wall = zbuf[(W / 2) / CS];
    flash = 6;
    for (int i = 0; i < nfoes; i++) {
        if (!foes[i].alive) continue;
        int spx = foes[i].x - px, spy = foes[i].y - py;
        int depth = (int)(((long long)dirx() * spx + (long long)diry() * spy) >> 14);
        int lat = (int)(((long long)diry() * spx - (long long)dirx() * spy) >> 14);
        if (depth <= 0 || depth >= wall + ONE / 4 || iabs(lat) > ONE * 45 / 100 + depth / 8 || !sees(&foes[i])) continue;   /* a wall between: no shot */
        if (depth < bestd) { bestd = depth; best = i; }
    }
    if (best < 0) return;
    foes[best].alive = 0; kills++;
    say_n("depths: hit ", kills);
}
static void tick(void) {
    if (flash) { flash--; dirty = 1; }
    if (over || !awake) return;
    for (int i = 0; i < nfoes; i++) {
        struct foe *f = &foes[i];
        if (!f->alive) continue;
        if (f->flash) f->flash--;
        int dx = px - f->x, dy = py - f->y, ax = iabs(dx) >> 8, ay = iabs(dy) >> 8;
        int d2 = ax * ax + ay * ay;   /* Q16 squared tiles */
        if (d2 > 7 * 7 * 65536 || !sees(f)) continue;   /* out of range, or a wall between you: it waits */
        if (d2 < 52000) {   /* about 0.9 of a tile: it bites */
            if (++f->bite >= 80) { f->bite = 0; health -= 10; f->flash = 8; say_n("depths: health ", health < 0 ? 0 : health); if (health <= 0) { over = 2; say("depths: down\n"); } }
            dirty = 1;
            continue;
        }
        int sp = ONE / 160, nx = f->x + (dx > 0 ? sp : -sp), ny = f->y + (dy > 0 ? sp : -sp);
        if (iabs(dx) > ONE / 8 && !solid(nx >> 16, f->y >> 16)) f->x = nx;
        if (iabs(dy) > ONE / 8 && !solid(f->x >> 16, ny >> 16)) f->y = ny;
        dirty = 1;
    }
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open_hires(&win) != 0 || !win.pixels) { say("depths: no window\n"); jt_exit(1); }
    {   char line[48]; int l = 0;
        const char *pfx = "depths: ring-3 window ";
        while (*pfx) line[l++] = *pfx++;
        l += utoa10(win.width, line + l); line[l++] = 'x';
        l += utoa10(win.height, line + l); line[l++] = '\n';
        jt_write(1, line, (unsigned)l);
    }
    start();
    render();
    say("depths: ready\n");
    unsigned flags = JT_POLL_PRESENT, last = now(), drawn = last;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        if (r == 1 && jt_window_resized(&ev, &win)) { render(); flags = JT_POLL_PRESENT; continue; }
        flags = 0;
        if (r == 1) {
            if (ev.kind == JT_EV_KEY) {
                int k = ev.a;
                if (k == '`') { say("depths: crashing on purpose\n"); *(volatile unsigned *)0 = 1; }
                if (k == JT_KEY_ESC) break;
                if (!over) awake = 1;
                if (over) { if (k == ' ') start(); }
                else if (k == JT_KEY_UP || k == 'w') walk(1);
                else if (k == JT_KEY_DOWN || k == 's') walk(-1);
                else if (k == JT_KEY_LEFT || k == 'a') ang = (ang + 8) & 255;
                else if (k == JT_KEY_RIGHT || k == 'd') ang = (ang - 8) & 255;
                else if (k == ' ') fire();
                render(); drawn = now(); flags = JT_POLL_PRESENT;
            }
            continue;
        }
        if (r != -11 /* -EAGAIN */) break;
        unsigned t = now();
        if (t != last) {
            unsigned n = t - last; if (n > 5) n = 5;
            while (n--) tick();
            last = t;
            if (dirty && t - drawn >= 3) { render(); drawn = t; flags = JT_POLL_PRESENT; }
        } else jt_sched_yield();
    }
    say("depths: closed\n");
    jt_exit(0);
}
