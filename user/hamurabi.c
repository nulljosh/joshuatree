/* hamurabi: the 1968 game of ruling a city for ten years, as a ring-3 program. The title screen, the cards of the
 * story, the orders panel, the year's report and the ending are all here, on top of the rules in hamurabi_rules.h.
 *
 * The game is shown as Hamurapi (the store name Hamurabi was taken); only the credit to the 1968 game and the
 * internal names (this file, HAMURABI.BIN, open=hamurabi, the hamurabi_ prefixes) keep the old spelling.
 *
 * The scene is a port of web/play/scene.js from the Hamurabi repo: a sky that runs from dawn to dusk over the ten
 * years, two ridges of far hills, a town of houses around a ziggurat with a king on top, a river with a boat,
 * fields, sheep and villagers pacing their lanes. It reacts to the game: a villager for every four people, a
 * house for every six, a plot for every 25 acres (sown for every 25 you order planted), a sack for every 700
 * bushels, the year's harvest and rats as numbers floating up, ghosts for the starved, a green fog and sick
 * villagers for the plague, rain on a ruined reign and fireworks on a great one. Every sprite comes from one
 * 256x149 sheet (user/hamurabi_sprites.h, made by tools/gen/gen_hamurabi_sprites.py). The sheet is stored as
 * run-length coded palette indices; at start it is unpacked into a 38 KB byte image on the SYS_BRK heap, so the
 * flat binary stays far under its 128 KB window. A sprite pixel is drawn as a whole S x S block, S the largest
 * whole number the window allows, so the pixels stay crisp. Semi-see-through shades (the shadows and ghosts) are
 * alpha-blended.
 *
 * No floats and no 64-bit division: the scene's sine is a 65-entry table, its random numbers are the same
 * integer hash the web uses, and time is the 100 Hz tick counter turned into milliseconds. The ABI hands a
 * program clicks, keys and wheel ticks but no pointer movement, so there is no hover; a click shows the button
 * pressed for a moment before it acts, and the arrow keys move a focus ring that Enter presses.
 *
 * Playing: "Start a new game" is the story on the normal rules (a card at the gate of some years), "Classic 1968"
 * has no cards, "Watch a demo" is the robot king ruling a classic reign by himself until any key or click. Orders
 * are clamped exactly as the web game does, so a player can never submit an illegal year. Each reign seeds its
 * dice from the clock and says so on the serial port, with every year's orders, report and result, which is what
 * tools/checks/ring3hamurabi-check.py replays on the host against the same hamurabi_rules.h. The backquote key
 * crashes the program on purpose (the all-apps crash check presses it). Esc goes back a screen; on the title it
 * closes. Resizing re-lays the scene out for the new window.
 */
#include "jtsys.h"
#include "libjt/text.h"
#include "hamurabi_sprites.h"
#include "hamurabi_rules.h"

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;
#define NOINL __attribute__((noinline))

#define INK    0x00232338u /* house ink */
#define ACCENT 0x00B5502Cu /* terracotta */
#define GOOD   0x0033692Au /* a good harvest's number */
#define MUTED  0x00666B75u
#define PRESSED_ACCENT 0x008C3E20u

static struct jt_window_info win JT_DATA = {0, 0, 0, 0}; /* the real window: only ever written by present() in one quick copy */
static struct jt_window_info cv JT_DATA = {0, 0, 0, 0};  /* the back buffer every draw goes to, same size, on the heap; drawing straight into the window let the compositor catch half-painted frames */
static u8 *sheet JT_DATA = 0;     /* HAM_SHEET_W x HAM_SHEET_H palette indices, unpacked at start */
static u32 *mask JT_DATA = 0;     /* scratch for the big title */
static u32 tm JT_DATA = 0;        /* milliseconds since the program started */
static int S JT_DATA = 2;         /* screen pixels per art pixel */
static int W JT_DATA = 0, UH JT_DATA = 0, HZ JT_DATA = 0, TOTAL JT_DATA = 0; /* the scene's width, play height, horizon and full height in art pixels */
static int zig_x JT_DATA = 0, zig_y JT_DATA = 0;          /* where the ziggurat's top-left landed, for the check */
static int focus JT_DATA = -1;                           /* the title's focused button */
static int pressed_act JT_DATA = 0, pressed_arg JT_DATA = 0, pend_act JT_DATA = 0, pend_arg JT_DATA = 0; /* a button drawn pressed, then acted on */
static u32 pressed_until JT_DATA = 0;
static int spr_alpha JT_DATA = 255;                       /* 0..255 multiplier on every sprite pixel: ghosts, the fade of the dead and of the plague */

#define MASK_BYTES 16384
#define BB_BYTES 0x220000u /* the kernel's whole window framebuffer (JT_USER_FB_BYTES in kernel/memmap.h): the back buffer never needs more */
#define SHEET_BYTES (HAM_SHEET_W * HAM_SHEET_H)

/* ---------- small maths ---------- */
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static int iclamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static u32 isqrt(u32 v) {
    u32 r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    while (b) { if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; }
    return r;
}

static const short SINQ[66] = {
    0, 101, 201, 301, 401, 501, 601, 700, 799, 897, 995, 1092, 1189, 1285, 1380, 1474, 1567, 1660, 1751, 1842, 1931, 2019,
    2106, 2191, 2276, 2359, 2440, 2520, 2598, 2675, 2751, 2824, 2896, 2967, 3035, 3102, 3166, 3229, 3290, 3349, 3406, 3461,
    3513, 3564, 3612, 3659, 3703, 3745, 3784, 3822, 3857, 3889, 3920, 3948, 3973, 3996, 4017, 4036, 4052, 4065, 4076, 4085,
    4091, 4095, 4096, 4096,
};
/* sin of a turn fraction (0..65535 is one whole turn), as Q12. */
static int sin_turns(u32 ph) {
    ph &= 0xFFFF;
    u32 quad = ph >> 14, r = ph & 0x3FFF;
    if (quad & 1) r = 0x4000 - r;
    u32 i = r >> 8, f = r & 255;
    int v = SINQ[i] + (((SINQ[i + 1] - SINQ[i]) * (int)f) >> 8);
    return quad >= 2 ? -v : v;
}
/* sin of an angle in Q8 radians, as Q12. */
static NOINL int isin(int a_q8) { return sin_turns((u32)((a_q8 * 40743) / 1000)); }
/* sin(t * w + ph): t in seconds, w in Q8 radians a second, ph in Q8 radians. One cycle at a time keeps it in 32 bits. */
static NOINL int tsin(int w_q8, int ph_q8) {
    u32 per = 1608496u / (u32)w_q8;
    return isin((int)(((tm % per) * (u32)w_q8) / 1000u) + ph_q8);
}
/* How far something moving at sp_q4 (a sixteenth of an art pixel per second) has gone: Q4, no overflow for days. */
static NOINL u32 mvt(u32 at, u32 sp_q4) { return (at / 1000u) * sp_q4 + (at % 1000u) * sp_q4 / 1000u; }
static NOINL u32 mv(u32 sp_q4) { return mvt(tm, sp_q4); }

/* The web scene's own hash, so every sprite keeps the habits it has there: 0..99999. */
static NOINL u32 rnd(u32 i, u32 salt) {
    u32 x = i * 7919u + salt * 104729u + 12345u;
    x = (x ^ (x >> 15)) * 0x2C1B3C6Du;
    x = (x ^ (x >> 12)) * 0x297A2D39u;
    x ^= x >> 15;
    return x % 100000u;
}

/* ---------- pixels ---------- */
static NOINL u32 mix(u32 a, u32 b, int k) { /* k 0..256 from a to b */
    u32 r = ((a >> 16 & 255) * (u32)(256 - k) + (b >> 16 & 255) * (u32)k) >> 8;
    u32 g = ((a >> 8 & 255) * (u32)(256 - k) + (b >> 8 & 255) * (u32)k) >> 8;
    u32 bl = ((a & 255) * (u32)(256 - k) + (b & 255) * (u32)k) >> 8;
    return r << 16 | g << 8 | bl;
}
static void blendpx(u32 *d, u32 rgb, int a) { /* a 0..255 */
    if (a >= 255) { *d = rgb; return; }
    if (a <= 0) return;
    u32 o = *d, ia = (u32)(255 - a), ua = (u32)a;
    u32 t, r, g, b;
    t = (rgb >> 16 & 255) * ua + (o >> 16 & 255) * ia; r = (t + 128 + ((t + 128) >> 8)) >> 8;
    t = (rgb >> 8 & 255) * ua + (o >> 8 & 255) * ia;   g = (t + 128 + ((t + 128) >> 8)) >> 8;
    t = (rgb & 255) * ua + (o & 255) * ia;             b = (t + 128 + ((t + 128) >> 8)) >> 8;
    *d = r << 16 | g << 8 | b;
}
static void blendrect(int x, int y, int w, int h, u32 rgb, int a) {
    int W0 = (int)cv.width, H0 = (int)cv.height;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W0) w = W0 - x;
    if (y + h > H0) h = H0 - y;
    if (w <= 0 || h <= 0 || a <= 0) return;
    for (int yy = 0; yy < h; yy++) {
        u32 *row = cv.pixels + (u32)(y + yy) * cv.width + (u32)x;
        if (a >= 255) for (int xx = 0; xx < w; xx++) row[xx] = rgb;
        else for (int xx = 0; xx < w; xx++) blendpx(&row[xx], rgb, a);
    }
}
static void fillrect(int x, int y, int w, int h, u32 rgb) { blendrect(x, y, w, h, rgb, 255); }
/* A rectangle in art pixels, corners in sixteenths. */
static NOINL void arect(int x16, int y16, int w16, int h16, u32 rgb, int a) {
    int x0 = (x16 * S + 8) >> 4, y0 = (y16 * S + 8) >> 4;
    int x1 = ((x16 + w16) * S + 8) >> 4, y1 = ((y16 + h16) * S + 8) >> 4;
    if (x1 <= x0) x1 = x0 + 1;
    if (y1 <= y0) y1 = y0 + 1;
    blendrect(x0, y0, x1 - x0, y1 - y0, rgb, a);
}
static NOINL void arect1(int x, int y, int w, int h, u32 rgb, int a) { arect(x * 16, y * 16, w * 16, h * 16, rgb, a); }

/* A soft round glow: alpha falls off linearly from the centre. Centre and radius in art pixels. */
static void glow(int cx, int cy, int rad, u32 rgb, int a) {
    int R = rad * S, X = cx * S, Y = cy * S, W0 = (int)cv.width, H0 = (int)cv.height;
    for (int y = imax(Y - R, 0); y < imin(Y + R, H0); y++) {
        for (int x = imax(X - R, 0); x < imin(X + R, W0); x++) {
            int dx = x - X, dy = y - Y;
            u32 d2 = (u32)(dx * dx + dy * dy);
            if (d2 >= (u32)(R * R)) continue;
            int al = a * (R - (int)isqrt(d2)) / R;
            blendpx(&cv.pixels[(u32)y * cv.width + (u32)x], rgb, al);
        }
    }
}

/* ---------- sprites ---------- */
static int sw_of(int id) { return ham_spr[id][2]; }
static int sh_of(int id) { return ham_spr[id][3]; }

static void spr(int id, int ax, int ay, int flip) {
    const short *r = ham_spr[id];
    int w = r[2], h = r[3], W0 = (int)cv.width, H0 = (int)cv.height;
    int px0 = ax * S, py0 = ay * S;
    if (px0 >= W0 || py0 >= H0 || px0 + w * S <= 0 || py0 + h * S <= 0) return;
    for (int sy = 0; sy < h; sy++) {
        int y0 = py0 + sy * S;
        if (y0 >= H0 || y0 + S <= 0) continue;
        const u8 *src = sheet + (r[1] + sy) * HAM_SHEET_W + r[0];
        for (int sx = 0; sx < w; sx++) {
            u8 idx = src[flip ? w - 1 - sx : sx];
            if (!idx) continue;
            u32 c = ham_palette[idx];
            int a = (int)(c >> 24) * spr_alpha / 255, x0 = px0 + sx * S;
            if (x0 >= W0 || x0 + S <= 0) continue;
            for (int yy = imax(y0, 0); yy < imin(y0 + S, H0); yy++) {
                u32 *row = cv.pixels + (u32)yy * cv.width;
                for (int xx = imax(x0, 0); xx < imin(x0 + S, W0); xx++) {
                    if (a == 255) row[xx] = c & 0xFFFFFF;
                    else blendpx(&row[xx], c & 0xFFFFFF, a);
                }
            }
        }
    }
}
/* A sprite standing on the ground: shadow under the feet, centred on cx, bottom row on foot. lift bobs the body, not the shadow. */
static void stand(int id, int cx, int foot, int flip, int shadow, int lift) {
    int sw = sw_of(id), sh = sh_of(id);
    if (shadow) {
        int rx = sw * 42 * S / 100, ry = imax(1, 16 * S / 10), X = (cx + 1) * S, Y = foot * S;
        for (int dy = -ry; dy <= ry; dy++) {
            int half = (int)((u32)rx * isqrt((u32)(ry * ry - dy * dy)) / (u32)ry);
            blendrect(X - half, Y + dy, 2 * half, 1, 0, 33 * spr_alpha / 255);
        }
    }
    spr(id, cx - sw / 2, foot - sh + 1 - lift, flip);
}


/* ---------- the game ---------- */
enum { PH_TITLE, PH_CARD, PH_ORDERS, PH_REPORT, PH_OVER };
enum { M_STORY, M_CLASSIC, M_DEMO };
enum { A_NONE, A_TITLE, A_CHOICE, A_STEP, A_BAR, A_QUICK, A_SUBMIT, A_NEXT, A_AGAIN, A_MENU };

static int phase JT_DATA = PH_TITLE, mode JT_DATA = M_STORY;
static hamurabi_city city JT_DATA;
static hamurabi_rng rng JT_DATA, srng JT_DATA;
static const hamurabi_rules *rules JT_DATA = 0;
static hamurabi_orders ord JT_DATA;
static hamurabi_year_report yr JT_DATA;
static u32 seed JT_DATA = 0, seen JT_DATA = 0;
static const hamurabi_omen *omen JT_DATA = 0;   /* the card's omen, or 0 for the opening and the turning points */
static const char *card_title JT_DATA = 0, *card_text JT_DATA = 0;
static char note[200] JT_DATA = {0};            /* what came of the last choice */
static int last_plant JT_DATA = 0, last_guard JT_DATA = 0, last_starved JT_DATA = 0;
static int harvested JT_DATA = 0, plagues JT_DATA = 0;
static u8 pips[HAMURABI_YEARS] JT_DATA = {0};   /* 0 not yet, 1 a fair year, 2 hunger or plague */
static int row JT_DATA = 0, cfocus JT_DATA = 0, ofocus JT_DATA = 0;
static u32 rep_start JT_DATA = 0;               /* tm when the report began: the scene's clock for the year's events */
static u32 demo_t JT_DATA = 0;
static int demo_set JT_DATA = 0;

/* What the scene shows. rt is the time since the report began, or -1 when there is none. */
static struct { int year, people, acres, grain, planted, starved, grade, omen, cats, rt; } sn JT_DATA;

static int villagers(int p) { return p <= 0 ? 0 : iclamp(p / 4, 1, 40); }
static int graves(int starved) { return imin((starved + 3) / 4, 12); }
/* smoothstep of num/den, 0..256 */
static NOINL int smooth256(int num, int den) {
    int c = den > 0 ? iclamp(num * 256 / den, 0, 256) : 256;
    return c * c * (768 - 2 * c) / 65536;
}

static NOINL void snapshot(void) {
    sn.year = 1; sn.people = 100; sn.acres = 1000; sn.grain = 2800; sn.planted = 800; sn.starved = 0;
    sn.grade = -1; sn.omen = -1; sn.cats = 0; sn.rt = -1;
    if (phase == PH_TITLE) return;
    sn.year = imin(city.year, HAMURABI_YEARS); sn.people = city.people; sn.acres = city.acres;
    sn.grain = city.grain; sn.starved = city.starved_total; sn.planted = 0;
    if (phase == PH_CARD) { sn.omen = omen ? (int)(omen - hamurabi_omens) : -1; sn.cats = city.guarded; }
    if (phase == PH_ORDERS) { sn.acres = city.acres + ord.buy; sn.planted = ord.plant; sn.cats = city.guarded; }
    if (phase == PH_REPORT) { sn.year = city.year; sn.planted = last_plant; sn.cats = last_guard; sn.rt = (int)(tm - rep_start); }
    if (phase == PH_OVER) { sn.planted = imin(city.acres, city.people * HAMURABI_TEND); sn.grade = hamurabi_grade(&city); }
}

/* ---------- the scene ---------- */
static int laneMin JT_DATA, laneMax JT_DATA, riverTop JT_DATA, fieldTop JT_DATA, fieldRows JT_DATA, fieldCols JT_DATA, spread16 JT_DATA, granaryX JT_DATA;

struct folk { int x, y, right, id, alpha; };
#define FOLK_MAX 96
#define FOLK_BYTES 2048
static struct folk *fk JT_DATA = 0;

/* where villager id has wandered to at time `at` (ms): they pace their own lane */
static void wander(int id, u32 at, int *x, int *y, int *right) {
    int margin = 8, span = W - 2 * margin;
    u32 sp = 80u + 112u * rnd((u32)id, 3) / 100000u, sp16 = (u32)span * 16u;
    u32 u = (rnd((u32)id, 4) * 2u * sp16 / 100000u + mvt(at, sp)) % (2u * sp16);
    *right = u < sp16;
    *x = margin + (int)((*right ? u : 2u * sp16 - u) >> 4);
    *y = laneMin + (int)(rnd((u32)id, 5) * (u32)(laneMax - laneMin) / 100000u);
}

/* A number that floats up and fades, like "+4,000": drawn into the scratch mask, then blended with a white edge. */
static void label(const char *s, int ax, int ay, int from, u32 rgb) {
    int age = sn.rt - from;
    if (sn.rt < 0 || age <= 0 || age >= 1800) return;
    int a = age < 1300 ? 255 : 255 - (age - 1300) * 255 / 500;
    int mw = jt_text_width(JT_FACE_BOLD, s) + 6, mh = 22;
    if (mw * mh * 4 > MASK_BYTES) return;
    for (int i = 0; i < mw * mh; i++) mask[i] = 0xFFFFFF;
    struct jt_window_info m = { (u32)mw, (u32)mh, (u32)mw, mask };
    jt_text_draw(&m, JT_FACE_BOLD, 3, 3, 0, s);
    int x0 = ax * S - mw / 2, y0 = ay * S - age * 9 * S / 1000 - mh / 2;
    for (int pass = 0; pass < 2; pass++)
        for (int yy = 0; yy < mh; yy++) for (int xx = 0; xx < mw; xx++) {
            int cov = (255 - (int)(mask[yy * mw + xx] & 255)) * a / 255;
            if (cov <= 0) continue;
            if (pass) blendrect(x0 + xx, y0 + yy, 1, 1, rgb, cov);
            else for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) blendrect(x0 + xx + dx, y0 + yy + dy, 1, 1, 0xFFFFFF, cov);
        }
}
static void signed_label(char sign, int v, int ax, int ay, int from, u32 rgb) {
    char b[24]; int n = 0, k = 0; char t[12];
    b[n++] = sign;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) { b[n++] = t[--k]; if (k && k % 3 == 0) b[n++] = ','; }
    b[n] = 0;
    label(b, ax, ay, from, rgb);
}

static NOINL void scene(void) {
    int cw = (int)cv.width, chh = (int)cv.height;
    int hz = HZ, rep = sn.rt >= 0, rt = sn.rt;
    int starved = rep ? yr.starved : 0;

    /* the reign is one long day: dawn in year one, a starry evening by year ten */
    int glide = rep ? smooth256(rt, 3000) : 256;
    int p = iclamp((sn.year - 2) * 256 + glide, 0, 9 * 256) / 9;
    int dawn = smooth256(64 - p, 64), dusk = smooth256(p - 141, 115);
    u32 sky0 = mix(mix(0xFBFBFAu, 0xFCF3F2u, dawn), 0x8FA6D1u, dusk), sky1 = mix(0xECF0F4u, 0xD6E0F0u, dusk);
    for (int y = 0; y < hz * S && y < chh; y++) {
        int k = hz * S > 1 ? y * 256 / (hz * S - 1) : 0;
        fillrect(0, y, cw, 1, mix(sky0, sky1, k));
    }
    if (dusk > 77) for (int i = 0; i < 40; i++)
        arect1((int)(rnd((u32)i, 50) * (u32)W / 100000u), (int)(rnd((u32)i, 51) * (u32)(hz * 7 / 10) / 100000u), 1, 1, 0xFFFFFF, (dusk - 77) * 190 / 179);
    /* the sun and its glow */
    int sx = W * (7680 + 45 * p) / 25600, sy = hz * (80 * 4096 - 26 * isin(p * 804 / 256)) / (100 * 4096);
    glow(sx, sy, 34, ACCENT, 56);
    spr(HAM_SPR_SUN_0 + (int)((tm / 700u) & 1), sx - sw_of(HAM_SPR_SUN_0) / 2, sy - sh_of(HAM_SPR_SUN_0) / 2, 0);
    /* two ridges of far hills */
    for (int k = 0; k < 2; k++) {
        u32 color = k ? 0xDADEE3u : 0xE6E9EDu;
        int n = (W + 8) / 3 + 2;
        for (int c = 0; c < cw; c++) {
            int ax16 = c * 16 / S + 64;                         /* art x in sixteenths, shifted so the first vertex (x = -4) is 0 */
            int i = ax16 / 48, fr = ax16 % 48;
            if (i >= n - 1) i = n - 2;
            int y16[2];
            for (int j = 0; j < 2; j++) {
                int x = -4 + (i + j) * 3;
                int s1 = isin(x * (k ? 8704 : 5376) / 1000 + (k ? 870 : 333)); /* sin(x * (0.021 + 0.013k) + 1.3 + 2.1k) */
                int s2 = isin(x * 14592 / 1000 + k * 256);                      /* sin(x * 0.057 + k) */
                y16[j] = hz * 16 - (9 - k * 4) * 16 - (7 - k * 2) * 16 * s1 / 4096 - 3 * 16 * s2 / 4096;
            }
            int top = (y16[0] + (y16[1] - y16[0]) * fr / 48) * S / 16;
            if (top < 0) top = 0;
            if (top < hz * S) fillrect(c, top, 1, hz * S - top, color);
        }
    }
    /* clouds and birds */
    {
        static const struct { u8 id; u8 x100, y100; u8 sp2; } CL[4] = {
            { HAM_SPR_CLOUD_A, 18, 18, 14 }, { HAM_SPR_CLOUD_B, 55, 34, 8 }, { HAM_SPR_CLOUD_A, 82, 10, 11 }, { HAM_SPR_CLOUD_B, 33, 6, 6 } };
        int span = W + 70;
        for (int i = 0; i < 4; i++) {
            u32 x16 = ((u32)span * 16u * CL[i].x100 / 100u + mv((u32)CL[i].sp2 * 8u)) % ((u32)span * 16u);
            spr(CL[i].id, (int)(x16 >> 4) - 40, hz * CL[i].y100 / 100 + 3 + (i & 1) * 3, 0);
        }
        span = W + 40;
        for (int b = 0; b < 3; b++) {
            u32 x16 = ((u32)W * 16u * rnd((u32)b, 1) / 100000u + mv((u32)(14 + b * 5) * 16u)) % ((u32)span * 16u);
            int y = hz * (25 + (int)(20u * rnd((u32)b, 2) / 100000u)) / 100 + tsin(256, b * 512) * 3 / 4096;
            spr(HAM_SPR_BIRD_0 + (int)((tm / 333u + (u32)b) & 1), (int)(x16 >> 4) - 20, y, 0);
        }
    }
    /* ground */
    for (int y = hz * S; y < chh; y++) {
        int k = (chh - hz * S) > 1 ? (y - hz * S) * 256 / (chh - hz * S - 1) : 0;
        fillrect(0, y, cw, 1, mix(0xF4F4F1u, 0xE6E6E2u, k));
    }
    {   /* grass flecks */
        int n = W * (TOTAL - hz) / 70;
        for (int i = 0; i < n; i++)
            arect1((int)(rnd((u32)i, 20) * (u32)W / 100000u), hz + 2 + (int)(rnd((u32)i, 21) * (u32)(TOTAL - hz - 2) / 100000u),
                   rnd((u32)i, 22) < 30000 ? 2 : 1, 1, 0, 14);
    }

    /* the layout every layer shares */
    int zw = sw_of(HAM_SPR_ZIGGURAT), zh = sh_of(HAM_SPR_ZIGGURAT);
    int zcx = 5 + zw / 2, zfoot = hz + 10, ztop = zfoot - zh + 1;
    granaryX = W - 20; riverTop = UH * 68 / 100; fieldTop = UH * 78 / 100;
    laneMin = imax(hz + 24, UH * 52 / 100); laneMax = imax(laneMin + 8, UH * 64 / 100);
    fieldRows = imax(1, (UH - 4 - fieldTop) / 13); fieldCols = imax(4, (W - 12) / 11);
    spread16 = (W - 12) * 16 / fieldCols;
    int flood = sn.omen == 4;

    /* the town: ziggurat, torches, king */
    stand(HAM_SPR_ZIGGURAT, zcx, zfoot, 0, 1, 0);
    zig_x = zcx - zw / 2; zig_y = zfoot - zh + 1;
    int fl = (int)((tm / 125u) & 1);
    glow(zcx - zw / 2 + 10, ztop + 13, 9 + fl, 0xFFB84D, 89);
    glow(zcx + zw / 2 - 10, ztop + 13, 9 + fl, 0xFFB84D, 89);
    spr(HAM_SPR_FLAME_0 + fl, zcx - zw / 2 + 8, ztop + 17 - sh_of(HAM_SPR_FLAME_0) + 1, 0);
    spr(HAM_SPR_FLAME_0 + 1 - fl, zcx + zw / 2 - 8 - sw_of(HAM_SPR_FLAME_0), ztop + 17 - sh_of(HAM_SPR_FLAME_0) + 1, 0);
    if (sn.grade != 0) {   /* a deposed king leaves the roof empty */
        int grieving = rep && ((starved > 0 && rt > 1800 && rt < 5000) || (yr.plague && rt > 4400 && rt < 7000));
        int waving = sn.grade == 3 || (rep && yr.yield >= 4 && rt > 800 && rt < 2600) || (tm / 2200u) % 3 == 0;
        stand(grieving ? HAM_SPR_KING_2 : HAM_SPR_KING_0 + (waving ? (int)((tm / 400u) & 1) : 0), zcx - 9, ztop + 9, 0, 0, 0);
    }
    /* houses, two staggered rows, with cooking smoke over every other one in the back */
    {
        int x0 = zcx + zw / 2 + 14, x1 = granaryX - 34;
        int cols = imax(1, (x1 - x0) / 20 + 1), n = imin(imax(sn.people / 6, 1), cols * 2);
        for (int r = 0; r < 2; r++) for (int i = r; i < n; i += 2) {
            int col = i / 2, id = HAM_SPR_HOUSE_A + (i * 7 + 1) % 3;
            int cx = x0 + col * 20 + r * 9, foot = hz + 9 + r * 12;
            stand(id, cx, foot, 0, 1, 0);
            if (r == 0 && col % 2 == 0) for (int k = 0; k < 3; k++) {
                int age = (int)(((tm % 2857u) * 256u / 2857u + (u32)(k * 85 + i * 95)) & 255u); /* 0..255 is one puff's life */
                int sx16 = cx * 16 + 48 + isin(age * 6 + i * 256) * 24 / 4096;
                int sy16 = (foot - sh_of(id)) * 16 - age * 13 * 16 / 256;
                int sz = 24 + age * 32 / 256;
                arect(sx16, sy16, sz, sz, 0, 41 * (256 - age) / 256);
            }
        }
    }
    {   /* palms */
        static const u8 PX[3] = {40, 66, 90};
        for (int i = 0; i < 3; i++)
            if (W * PX[i] / 100 < granaryX - 26) stand(HAM_SPR_PALM_0 + (int)(((tm / 833u) + (u32)i) & 1), W * PX[i] / 100, hz + 11 + (i & 1) * 2, 0, 1, 0);
    }
    stand(HAM_SPR_GRANARY, granaryX + (rep && yr.rats > 0 && rt > 1400 && rt < 3200 ? (int)((tm / 50u) & 1) : 0), hz + 11, 0, 1, 0);
    for (int i = 0; i < imin(imax(sn.grain / 700, 0), 10); i++) {   /* a sack for every 700 bushels, a second tier above six */
        int tier = i < 6 ? 0 : 1, k = tier ? i - 6 : i;
        stand(HAM_SPR_SACK, granaryX - 19 - k * 7 - tier * 4, hz + 12 - tier * 5, 0, !tier, 0);
    }
    if (sn.cats) for (int k = 0; k < 2; k++) {   /* they pace in front of the barn all year */
        int u = (int)((mv(9u * 16u) >> 4) + (u32)(k * 17)) % 44, right = u < 22;
        stand(HAM_SPR_CAT_0 + (int)(((tm / 200u) + (u32)k) & 1), granaryX - 36 + (right ? u : 44 - u) + k * 6, hz + 16 + k * 4, !right, 1, 0);
    }
    {   /* a caravan on the near bank, with long gaps between visits */
        u32 span = (u32)(W * 22 / 10 + 90) * 16u, cx16 = (mv(7u * 16u) + 40u * 16u) % span;
        int cx = (int)(cx16 >> 4) - 30, bob = (int)((tm / 500u) & 1);
        stand(HAM_SPR_CAMEL_0 + bob, cx, riverTop - 1, 0, 1, 0);
        stand(HAM_SPR_CAMEL_0 + 1 - bob, cx - 30, riverTop - 1, 0, 1, 0);
    }

    /* the river with its boat, ripples and a jumping fish; it rises when the omen says so */
    {
        int top = riverTop - (flood ? 5 : 0), hgt = flood ? 13 : 7;
        for (int y = top * S; y < (top + hgt) * S && y < chh; y++) {
            int k = y - top * S;
            fillrect(0, y, cw, 1, mix(0xB5D6F2u, 0x8FBAE6u, k * 256 / (hgt * S)));
        }
        arect1(-4, top, W + 8, 1, 0xFFFFFF, 178);
        arect1(-4, top + hgt, W + 8, 1, 0, 26);
        int i = 0;
        for (int x = -(int)(mv((flood ? 22u : 6u) * 16u) >> 4) % 12; x < W; x += 12, i++)
            arect(x * 16, top * 16 + 32 + (i % 3) * 24, 80, 16, 0xFFFFFF, 191);
        int boatX = (int)((mv(9u * 16u) >> 4) % (u32)(W + 60)) - 30;
        for (int k = 1; k <= 3; k++) arect1(boatX - 9 - k * 5, top + 5 + (k & 1), 3, 1, 0xFFFFFF, 204 - k * 51);
        stand(HAM_SPR_BOAT, boatX, top + 5 + tsin(512, 0) * 6 / 10 / 4096, 0, 0, 0);
        u32 jump = tm / 3700u, age = tm % 3700u;
        if (age < 900) {
            int k = (int)(age * 256u / 900u);              /* 0..255 across the jump */
            int fx = 20 + (int)(rnd(jump, 60) * (u32)(W - 40) / 100000u);
            int arc = isin(k * 804 / 256) * 9 / 4096;       /* sin(k * pi) */
            spr(HAM_SPR_FISH, fx + k * 10 / 256, top + 1 - arc, 0);
            if (k < 38 || k > 218) arect1(fx + (k < 128 ? 0 : 10) - 1, top + 1, 8, 1, 0xFFFFFF, 230);
        }
    }

    /* fields: a plot for every 25 acres you hold, sown for every 25 you order planted */
    {
        int all = imin(imax(sn.acres, 0) / 25, fieldRows * fieldCols), planted = imin(all, (sn.planted + 24) / 25);
        int stage = rep ? (rt < 500 ? 1 : yr.yield >= 3 ? 2 : 3) : sn.grade >= 0 ? 2 : 0;
        for (int i = 0; i < all; i++) {
            int x16 = 96 + (i % fieldCols) * spread16 + (spread16 - 144) / 2, y = fieldTop + (i / fieldCols) * 13, sown = i < planted;
            arect(x16, (y + 2) * 16, 144, 176, 0, sown ? 15 : 7);
            arect(x16, (y + 12) * 16, 144, 16, 0, sown ? 13 : 5);
            if (!sown) continue;
            for (int f = 0; f < 3; f++) arect(x16 + 16 + f * 48, (y + 3) * 16, 16, 144, 0, 9);
            spr(HAM_SPR_WHEAT_0_0 + stage * 2 + (int)(((tm / 500u) + (u32)(i * 37 / 100)) & 1), (x16 + 8) >> 4, y, 0);
        }
        /* sheep graze whatever open ground is left below the fields */
        int pasture = fieldTop + ((all + fieldCols - 1) / fieldCols) * 13 + 10, floorY = TOTAL - 6;
        if (floorY - pasture > 12) {
            int n = imin(6, (floorY - pasture) / 10 + 2);
            for (int k = 0; k < n; k++) {
                u32 span = (u32)(W - 30) * 16u;
                u32 u = ((rnd((u32)k, 80) * 2u * span / 100000u) + mv(24u + 32u * rnd((u32)k, 81) / 100000u)) % (2u * span);
                int grazing = ((tm / 2500u + rnd((u32)k, 82) * 5u / 100000u) % 3u) == 0;
                int x = 15 + (int)((u < span ? u : 2u * span - u) >> 4);
                stand(HAM_SPR_SHEEP_0 + (grazing ? 1 : (int)(((tm / 500u) + (u32)k) & 1)), x, pasture + (int)(rnd((u32)k, 83) * (u32)(floorY - pasture) / 100000u), u >= span, 1, 0);
            }
        }
        if (all > 0) stand(HAM_SPR_FLAG_0 + (int)((tm / 333u) & 1), 4, fieldTop + 1, 0, 0, 0);
        if (!rep) for (int b = 0; b < 4; b++) {   /* butterflies */
            int open = ((tm / 111u + (u32)b) & 1) == 0;
            int bx16 = W * (20 + 20 * b) * 16 / 100 + tsin(179, b * 512) * 14 * 16 / 4096;
            int by16 = (fieldTop - 4 + (b & 1) * 8) * 16 + tsin(333, b * 256) * 5 * 16 / 4096;
            arect(bx16, by16, open ? 48 : 16, open ? 16 : 32, b % 2 == 0 ? ACCENT : 0x3B7BC6, 255);
        }
        if (!rep && sn.grade < 0 && planted > 0) {   /* an ox works the top row while you decide */
            u32 span = (u32)(W + 40) * 16u, u = mv(8u * 16u) % (span * 2u);
            int right = u < span;
            stand(HAM_SPR_OX_0 + (int)((tm / 333u) & 1), (int)((right ? u : span * 2u - u) >> 4) - 20, fieldTop + 1, !right, 1, 0);
        }
    }

    /* graves: one stone for every four who ever starved; this year's appear with the ghosts */
    {
        int before = graves(sn.starved - starved), shown = rep && rt < 2400 ? before : graves(sn.starved);
        for (int k = 0; k < shown; k++) stand(HAM_SPR_TOMBSTONE, W - 8 - k * 9, laneMax + 6 - (k & 1) * 3, 0, 1, 0);
    }

    /* the people: a villager for every four, on their own lanes, drawn back to front */
    {
        int n = 0, after = villagers(sn.people), id;
        int sick = rep && yr.plague && rt > 3400 && rt < 6600;
        int party = sn.grade == 3 || sn.omen == 6 || (rep && yr.yield >= 4 && rt > 800 && rt < 2600);
        if (rep) {
            int before = villagers(yr.people_before), alive = villagers(yr.people_before - starved);
            int grown = imax(alive, villagers(yr.people_before - starved + yr.born));
            for (id = 0; id < before && n < FOLK_MAX; id++) if (id < alive || rt <= 1800) {
                struct folk *f = &fk[n++];
                wander(id, tm, &f->x, &f->y, &f->right); f->id = id;
                f->alpha = id < alive && yr.plague && id >= after && rt > 4400 ? imax(0, 255 - (rt - 4400) * 255 / 800) : 255;
            }
            for (id = alive; id < grown && n < FOLK_MAX; id++) {   /* newcomers walk in from the left */
                int start = 2400 + (id - alive) * 120;
                if (rt <= start) continue;
                struct folk *f = &fk[n++];
                int qx, k = smooth256(rt - start, 2400);
                wander(id, tm, &qx, &f->y, &f->right);
                f->x = (-10 * (256 - k) + qx * k) / 256; if (k < 256) f->right = 1;
                f->id = id; f->alpha = yr.plague && id >= after && rt > 4400 ? imax(0, 255 - (rt - 4400) * 255 / 800) : 255;
            }
        } else for (id = 0; id < after && n < FOLK_MAX; id++) {
            struct folk *f = &fk[n++];
            wander(id, tm, &f->x, &f->y, &f->right); f->id = id; f->alpha = 255;
        }
        if (sn.omen == 2) for (int k = 0; k < 7; k++) {   /* a flooded town's families wait at the gate */
            struct folk *f = &fk[n++];
            f->x = 6 + (k % 4) * 7 + k / 4 * 3; f->y = laneMin + 2 + k / 4 * 7; f->right = 1; f->id = 100 + k; f->alpha = -1;
        }
        for (int j = 1; j < n; j++) { struct folk t = fk[j]; int q = j; while (q > 0 && fk[q - 1].y > t.y) { fk[q] = fk[q - 1]; q--; } fk[q] = t; }
        /* now and then two of them settle it the old way */
        u32 round = tm / 9000u, bout_age = tm % 9000u;
        int brawler = !rep && sn.grade < 0 && sn.omen < 0 && after >= 6 && bout_age < 2600 ? (int)(rnd(round, 70) * (u32)after / 100000u) : -1;
        for (int j = 0; j < n; j++) {
            struct folk *f = &fk[j];
            if (f->alpha == 0) continue;
            int look = f->id % HAM_LOOKS, step = (int)((tm / 166u + (u32)f->id) & 3);
            spr_alpha = f->alpha < 0 ? 255 : f->alpha;
            if (f->id == brawler) {
                stand(HAM_SPR_SCUFFLE_0 + (int)((tm / 111u) & 1), f->x, f->y + 1, 0, 1, 0);
                for (int k = 0; k < 3; k++) {
                    u32 q = tm / 166u + (u32)k;
                    arect1(f->x - 9 + (int)(rnd(q, 71) * 18u / 100000u), f->y - 14 + (int)(rnd(q, 72) * 8u / 100000u), 1, 1, 0xE8B02E, 255);
                }
            } else if (f->alpha < 0) stand(ham_villager[look][1], f->x, f->y, 0, 1, 0);
            else if (sick) stand(HAM_SPR_VILLAGER_SICK_0 + step, f->x, f->y, !f->right, 1, 0);
            else if (party && f->id % 2 == 0) stand(ham_cheer[look][(tm / 200u + (u32)f->id) & 1], f->x, f->y, 0, 1, 0);
            else stand(ham_villager[look][step], f->x, f->y, !f->right, 1, step & 1);
            spr_alpha = 255;
        }
        if (sn.omen == 0) for (int k = 0; k < 3; k++) stand(HAM_SPR_CAMEL_0 + (k & 1), 26 + k * 30, laneMax + 3, 0, 1, 0);
    }

    /* the year plays out: grain to the granary, rats, ghosts, the plague */
    if (rep) {
        int gx = granaryX, gy = hz - 8;
        for (int i = 0; i < imin(yr.harvest / 150, 40); i++) {   /* grain flies to the granary */
            int q = (rt - 200 - i * 45) * 256 / 1200;
            if (q <= 0 || q >= 256) continue;
            int fx = 10 + (int)((u32)spread16 * (rnd((u32)i, 6) * (u32)fieldCols / 100000u) / 16u), fy = fieldTop + (int)(rnd((u32)i, 7) * (u32)fieldRows / 100000u) * 13;
            spr(HAM_SPR_GRAIN, fx + (gx - fx) * q / 256, fy + (gy - fy) * q / 256 - isin(q * 804 / 256) * 18 / 4096, 0);
        }
        signed_label('+', yr.harvest, gx - 44, hz + 2, 900, yr.yield >= 3 ? GOOD : INK);
        if (yr.rats > 0) {
            for (int i = 0; i < imin(yr.rats / 60 + 3, 14); i++) {
                int start = 300 + i * 120, inP = smooth256(rt - start, 1300), outP = smooth256(rt - 3000 - i * 50, 1200);
                if (rt <= start || outP >= 256) continue;
                int x = (W + 8) + (gx - 10 - (W + 8)) * inP / 256 + (W + 8 - gx + 10) * outP / 256 + (int)(rnd((u32)i, 12) * 8u / 100000u);
                spr(HAM_SPR_RAT_0 + (int)((tm / 125u + (u32)i) & 1), x, hz + 9 + (int)(rnd((u32)i, 8) * 9u / 100000u), outP > 0);
            }
            signed_label('-', yr.rats, gx - 44, hz + 12, 1600, ACCENT);
        }
        if (starved > 0) {   /* the dead leave as ghosts */
            int alive = villagers(yr.people_before - starved), before = villagers(yr.people_before);
            for (int id = alive; id < before; id++) {
                int age = rt - 1800;
                if (age <= 0 || age >= 3000) continue;
                int qx, qy, qr;
                wander(id, rep_start + 1800u, &qx, &qy, &qr);
                spr_alpha = imax(0, 255 - age * 255 / 3000);
                spr(HAM_SPR_GHOST, qx - 5 + isin(age * 3 * 256 / 1000 + id * 256) * 3 / 2 / 4096, qy - 13 - age * 12 / 1000, 0);
                spr_alpha = 255;
            }
            signed_label('-', starved, W / 2, laneMin - 16, 1900, ACCENT);
        }
        if (yr.born > 0) signed_label('+', yr.born, 22, laneMin - 16, 2600, INK);
        if (yr.plague) {
            if (rt > 3200 && rt < 7200) { spr_alpha = 230; spr(HAM_SPR_PLAGUE, -36 + (W + 72) * (rt - 3200) / 4000, laneMin - 30, 0); spr_alpha = 255; }
            for (int i = 0; i < 6; i++) {
                int age = rt - 4400 - i * 150;
                if (age <= 0 || age >= 1800) continue;
                spr_alpha = 255 - age * 255 / 1800;
                spr(HAM_SPR_SKULL, W * (15 + 14 * i) / 100, laneMin - 6 - age * 12 / 1000, 0);
                spr_alpha = 255;
            }
            signed_label('-', yr.people_before - starved + yr.born - city.people, W / 2, laneMin - 26, 4500, ACCENT);
        }
        /* weather: dust on a failed harvest, fog with the plague */
        if (yr.yield <= 2 && rt > 400 && rt < 7000) {
            int a = imin(256, (rt - 400) * 256 / 800) * imin(256, (7000 - rt) * 256 / 1500) / 256;
            for (int i = 0; i < 46; i++) {
                int x = (int)((rnd((u32)i, 30) * (u32)(W + 30) / 100000u + (mv((50u + 60u * rnd((u32)i, 31) / 100000u) * 16u) >> 4)) % (u32)(W + 30)) - 15;
                arect1(x, hz + (int)(rnd((u32)i, 32) * (u32)(UH - hz) / 100000u), 3 + (int)(rnd((u32)i, 33) * 5u / 100000u), 1, 0, 26 * a / 256);
            }
        }
        if (yr.plague && rt > 3200 && rt < 7400) {
            int a = imin(256, (rt - 3200) * 256 / 1000) * imin(256, (7400 - rt) * 256 / 1200) / 256;
            blendrect(0, 0, cw, chh, 0x739E33, 33 * a / 256);
            for (int i = 0; i < 30; i++) {
                int y = UH - (int)((rnd((u32)i, 34) * (u32)UH / 100000u + (mv((6u + 8u * rnd((u32)i, 35) / 100000u) * 16u) >> 4)) % (u32)UH);
                arect1((int)(rnd((u32)i, 36) * (u32)W / 100000u) + isin((int)(tm % 6283u * 256u / 1000u) + i * 256) * 4 / 4096, y, 1, 1, 0x5E802B, 128 * a / 256);
            }
        }
    }
    if (sn.grade == 0) {   /* rain on a ruined reign */
        blendrect(0, 0, cw, chh, 0x333D4D, 36);
        for (int i = 0; i < 90; i++) {
            int fall = (int)((rnd((u32)i, 37) * (u32)TOTAL / 100000u + (mv((150u * 16u) + 960u * rnd((u32)i, 38) / 100000u) >> 4)) % (u32)TOTAL);
            int x = ((int)(rnd((u32)i, 39) * (u32)(W + 40) / 100000u) - fall / 4 + W + 40) % (W + 40);
            arect1(x, fall, 1, 4, 0x5C7394, 115);
        }
    }
    if (sn.grade == 3) {   /* fireworks and confetti on a great one */
        static const u32 COL[4] = { 0xB5502C, 0x3B7BC6, 0xE8B02E, 0x58993B };
        for (int b = 0; b < 3; b++) {
            u32 clk = tm + (u32)b * 630u, k0 = clk / 1700u;
            int age = (int)(clk % 1700u);
            int cx = W * (20 + (int)(60u * rnd(k0 * 3u + (u32)b, 40) / 100000u)) / 100, cy = hz * (25 + (int)(50u * rnd(k0 * 3u + (u32)b, 41) / 100000u)) / 100;
            if (age >= 1300) continue;
            int t = imin(256, age * 256 / 800), e = 256 - t, d = 26 * (256 - e * e / 256) / 256;
            for (int k = 0; k < 18; k++) {
                int a = k * 1608 / 18;   /* Q8 radians round a whole turn */
                arect1(cx + isin(a + 402) * d / 4096, cy + isin(a) * d / 4096 + age * age / 166667, 2, 2, COL[(k0 + (u32)b) % 4], 255 - age * 255 / 1300);
            }
        }
        for (int i = 0; i < 70; i++)
            arect1((int)(rnd((u32)i, 11) * (u32)W / 100000u) + isin((int)(tm % 3142u * 512u / 1000u) + i * 256) * 3 / 4096,
                   (int)(((mv((192u + 288u * rnd((u32)i, 9) / 100000u)) >> 4) + rnd((u32)i, 10) * (u32)UH / 100000u) % (u32)(UH + 10)) - 5, 2, 2, COL[i % 4], 255);
    }
}

/* ---------- the panels ---------- */
static void rrect(int x, int y, int w, int h, int r, u32 rgb, int a) {
    if (r * 2 > h) r = h / 2;
    if (r * 2 > w) r = w / 2;
    for (int yy = 0; yy < h; yy++) {
        int cy = yy < r ? r : yy >= h - r ? h - 1 - r : -1;
        if (cy < 0) { blendrect(x, y + yy, w, 1, rgb, a); continue; }
        blendrect(x + r, y + yy, w - 2 * r, 1, rgb, a);
        for (int side = 0; side < 2; side++) for (int k = 0; k < r; k++) {
            int xx = side ? w - 1 - k : k, cx = side ? w - 1 - r : r;
            int dx16 = (cx - xx) * 16, dy16 = (cy - yy) * 16;
            int d16 = (int)isqrt((u32)(dx16 * dx16 + dy16 * dy16));
            int cov = iclamp(r * 16 - d16 + 8, 0, 16);
            if (cov) blendrect(x + xx, y + yy, 1, 1, rgb, a * cov / 16);
        }
    }
}
/* A soft shadow round the panel, fading over 14 pixels and dropped 6 down, drawn only outside the panel's rounded outline. */
static void shadow(int x, int y, int w, int h, int r) {
    int sy = y + 6, R = 14;
    for (int py = sy - R; py < sy + h + R; py++) {
        int cy = iclamp(py, sy + r, sy + h - 1 - r);
        for (int px = x - R; px < x + w + R; px++) {
            if (py == cy && px >= x + r && px < x + w - r) { px = x + w - r - 1; continue; } /* straight middle of the panel: under it */
            int cx = iclamp(px, x + r, x + w - 1 - r);
            int d = (int)isqrt((u32)((px - cx) * (px - cx) + (py - cy) * (py - cy))) - r;
            if (d <= 0 || d >= R) continue;
            blendrect(px, py, 1, 1, 0, 30 * (R - d) * (R - d) / (R * R));
        }
    }
}
static void glass(int x, int y, int w, int h, int r) {
    shadow(x, y, w, h, r);
    rrect(x, y, w, h, r, 0xFFFFFF, 214);
}

static int text_c(int face, int cx, int y, u32 rgb, const char *s) {
    int w = jt_text_width(face, s);
    jt_text_draw(&cv, face, cx - w / 2, y, rgb, s);
    return w;
}

/* The title, a 16-pixel bold face enlarged f times, smoothed and then sharpened so the edges stay clean. */
static void big_text(const char *s, int cx, int top, int f, u32 rgb, int alpha) {
    int tw = jt_text_width(JT_FACE_BOLD, s), mw = tw + 4, mh = 20;
    if ((u32)(mw * mh * 4) > MASK_BYTES) return;
    for (int i = 0; i < mw * mh; i++) mask[i] = 0xFFFFFF;
    struct jt_window_info m = { (u32)mw, (u32)mh, (u32)mw, mask };
    jt_text_draw(&m, JT_FACE_BOLD, 2, 1, 0, s);
    int ow = mw * f, oh = mh * f, ox0 = cx - ow / 2;
    for (int oy = 0; oy < oh; oy++) {
        int v = ((2 * oy + 1) * 256) / (2 * f) - 128, y0 = v >> 8, fy = v & 255;
        for (int ox = 0; ox < ow; ox++) {
            int u = ((2 * ox + 1) * 256) / (2 * f) - 128, x0 = u >> 8, fx = u & 255;
            int c[4];
            for (int q = 0; q < 4; q++) {
                int xx = iclamp(x0 + (q & 1), 0, mw - 1), yy = iclamp(y0 + (q >> 1), 0, mh - 1);
                c[q] = 255 - (int)(mask[yy * mw + xx] & 255);
            }
            int top2 = c[0] * (256 - fx) + c[1] * fx, bot = c[2] * (256 - fx) + c[3] * fx;
            int cov = (top2 * (256 - fy) + bot * fy) >> 16;
            cov = iclamp((cov - 128) * 3 + 128, 0, 255);
            if (cov) blendrect(ox0 + ox, top + oy, 1, 1, rgb, cov * alpha / 255);
        }
    }
}

struct button { int x, y, w, h; const char *label; int primary; };
static struct button btn[3] JT_DATA = {{0}};
static int panel_y JT_DATA = 0;

static const char TITLE[] = "Hamurapi"; /* the big title; the credit line below keeps the 1968 game's own name */
static const char *const LABELS[3] = { "Start a new game", "Classic 1968", "Watch a demo" };

static NOINL void layout(void) {
    int cw = (int)cv.width, chh = (int)cv.height;
    S = imax(1, imin(imin(cw / 240, chh / 160), 6));
    int bw = imin(620, cw - 24), bh = 14 + 44 + 8 + 36 + 8 + 16 + 12;
    int bx = (cw - bw) / 2, by = chh - 10 - bh;
    btn[0] = (struct button){ bx + 14, by + 14, bw - 28, 44, LABELS[0], 1 };
    int half = (bw - 28 - 8) / 2;
    btn[1] = (struct button){ bx + 14, by + 14 + 44 + 8, half, 36, LABELS[1], 0 };
    btn[2] = (struct button){ bx + 14 + half + 8, by + 14 + 44 + 8, bw - 28 - half - 8, 36, LABELS[2], 0 };
    panel_y = by;
    W = (cw + S - 1) / S;
    UH = imax(100, (chh - (bh + 20)) / S); /* 100 keeps the king's head on screen: his top row sits 39 above the horizon */
    HZ = UH * 40 / 100;
    TOTAL = (chh + S - 1) / S;
}

/* ---------- words and numbers ---------- */
struct sb { char *p; int n; };
#define SB(b) char b##_buf[200]; struct sb b = { b##_buf, 0 }; b##_buf[0] = 0
static NOINL void sb_c(struct sb *b, char c) { if (b->n < 190) { b->p[b->n++] = c; b->p[b->n] = 0; } }
static NOINL void sb_s(struct sb *b, const char *s) { while (*s) sb_c(b, *s++); }
static NOINL void sb_u(struct sb *b, u32 v, int commas) {
    char t[12]; int k = 0;
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) { sb_c(b, t[--k]); if (commas && k && k % 3 == 0) sb_c(b, ','); }
}
static NOINL void sb_i(struct sb *b, int v, int commas) { if (v < 0) { sb_c(b, '-'); v = -v; } sb_u(b, (u32)v, commas); }
static NOINL void logsb(struct sb *b) { sb_c(b, '\n'); jt_write(1, b->p, (unsigned)b->n); }

static void cpy(char *d, const char *s) { while ((*d++ = *s++)) { } }
static const char *const GRADE[4] = { "F", "C", "B", "A+" };
static const char *const MODE[3] = { "story", "classic", "demo" };

static NOINL void say(const char *a, const char *b, const char *c) {
    SB(s);
    sb_s(&s, a); sb_s(&s, b); sb_s(&s, c);
    logsb(&s);
}
static NOINL void log_orders(const char *what) {
    SB(s);
    sb_s(&s, "hamurabi: "); sb_s(&s, what); sb_s(&s, " year "); sb_i(&s, city.year, 0);
    sb_s(&s, " buy "); sb_i(&s, ord.buy, 0); sb_s(&s, " feed "); sb_i(&s, ord.feed, 0); sb_s(&s, " plant "); sb_i(&s, ord.plant, 0);
    logsb(&s);
}

/* Greedy word wrap in `face`, at most maxl lines of lh pixels; returns the lines used. With draw set it also paints them. */
static int wrap(int face, int x, int y, int maxw, int lh, u32 rgb, const char *s, int maxl, int draw) {
    char line[128]; int n = 0, lines = 0;
    line[0] = 0;
    for (;;) {
        while (*s == ' ') s++;
        if (!*s) break;
        int wl = 0;
        while (s[wl] && s[wl] != ' ') wl++;
        if (wl > 60) wl = 60;
        char t[128]; int tn = 0;
        for (int i = 0; i < n; i++) t[tn++] = line[i];
        if (n) t[tn++] = ' ';
        for (int i = 0; i < wl; i++) t[tn++] = s[i];
        t[tn] = 0;
        if (n && (tn > 120 || jt_text_width(face, t) > maxw)) {
            if (lines >= maxl) return lines;
            if (draw) jt_text_draw(&cv, face, x, y + lines * lh, rgb, line);
            lines++; n = 0; line[0] = 0;
            continue;
        }
        for (int i = 0; i <= tn; i++) line[i] = t[i];
        n = tn; s += wl;
    }
    if (n && lines < maxl) { if (draw) jt_text_draw(&cv, face, x, y + lines * lh, rgb, line); lines++; }
    return lines;
}

/* ---------- the rules the orders panel keeps ---------- */
static int max_buy(void) { return city.grain / city.price; }
static int max_feed(void) { return imax(0, city.grain - ord.buy * city.price); }
static int max_plant(void) { return imax(0, imin(imin(city.acres + ord.buy, city.people * HAMURABI_TEND), city.grain - ord.buy * city.price - ord.feed)); }
/* Keeps each order inside what the earlier ones leave possible. */
static void clamp_orders(void) {
    ord.buy = iclamp(ord.buy, -city.acres, max_buy());
    ord.feed = iclamp(ord.feed, 0, max_feed());
    ord.plant = iclamp(ord.plant, 0, max_plant());
}
/* Feed everyone if the grain allows, plant what can be planted, buy and sell nothing. */
static void sensible(void) {
    ord.buy = 0;
    ord.feed = imin(city.grain, city.people * HAMURABI_FOOD);
    ord.plant = imin(imin(city.acres, city.people * HAMURABI_TEND), city.grain - ord.feed);
}
static int *rowp(int r) { return r == 0 ? &ord.buy : r == 1 ? &ord.feed : &ord.plant; }
static int row_min(int r) { return r == 0 ? -city.acres : 0; }
static int row_max(int r) { return r == 0 ? max_buy() : r == 1 ? max_feed() : max_plant(); }
static const int STEP[3] = { 1, 20, 10 }, BIG[3] = { 10, 200, 100 };
static void row_set(int r, int v) { *rowp(r) = v; clamp_orders(); }

/* ---------- the flow of a reign ---------- */
static u32 now_ms(void) { struct jt_tasks t; jt_tasks(&t, -1); return t.ticks * 10u; }

static void to_title(void) {
    phase = PH_TITLE; focus = -1; demo_t = 0; demo_set = 0; pressed_act = 0; pend_act = 0;
    jt_write(1, "hamurabi: phase title\n", 22);
}
static NOINL void to_orders(void) {
    phase = PH_ORDERS; row = 0; sensible(); demo_t = 0; demo_set = 0;
    log_orders("phase orders");
}
/* Whether the card's i-th choice can be paid for: a choice that cannot is drawn grey and cannot be picked. */
static int afford(int i) { return hamurabi_can_afford(&omen->choice[i], &city); }
static NOINL void enter_year(void) {
    omen = 0; card_title = 0;
    if (mode == M_STORY) {
        int y = city.year, i;
        if (y == 1) { card_title = hamurabi_intro_title; card_text = hamurabi_intro_text; }
        for (i = 0; i < HAMURABI_INTERLUDES; i++)
            if (hamurabi_interludes[i].year == y) { card_title = hamurabi_interludes[i].title; card_text = hamurabi_interludes[i].text; }
        if (!card_title) {
            omen = hamurabi_omen_for(&city, seen, &srng);
            if (omen) { seen |= 1u << (int)(omen - hamurabi_omens); card_title = omen->title; card_text = omen->text; }
        }
    }
    if (!card_title) { to_orders(); return; }
    phase = PH_CARD; cfocus = 0;
    if (omen && !afford(0)) cfocus = 1;
    SB(s);
    sb_s(&s, "hamurabi: phase card year "); sb_i(&s, city.year, 0); sb_c(&s, ' ');
    sb_s(&s, omen ? omen->id : city.year == 1 ? "intro" : "interlude");
    logsb(&s);
}
static NOINL void begin_reign(int m) {
    mode = m; rules = &hamurabi_rules_normal;
    unsigned t = 0; jt_time(&t);
    seed = t + now_ms() / 10u;   /* the clock, plus the tick count so two reigns in one second still differ */
    rng = hamurabi_rng_new(seed); srng = hamurabi_rng_new((unsigned long long)seed ^ 0x5707ULL);
    city = hamurabi_new_city(rules);
    seen = 0; plagues = 0; harvested = 0; last_starved = 0; note[0] = 0; demo_t = 0; demo_set = 0;
    for (int i = 0; i < HAMURABI_YEARS; i++) pips[i] = 0;
    SB(s);
    sb_s(&s, "hamurabi: seed "); sb_u(&s, seed, 0); sb_s(&s, " mode "); sb_s(&s, MODE[m]);
    logsb(&s);
    enter_year();
}
static NOINL void choose_card(int i) {
    if (phase != PH_CARD) return;
    note[0] = 0;
    if (omen) {
        const hamurabi_choice *ch = &omen->choice[i];
        if (!afford(i)) return;
        hamurabi_outcome out = hamurabi_apply_choice(ch, &city, &srng, &rng);
        SB(n);
        sb_s(&n, out.said);
        if (out.peek_yield) {
            sb_s(&n, " A "); sb_s(&n, out.peek_yield >= 4 ? "big" : out.peek_yield <= 2 ? "small" : "fair");
            sb_s(&n, " harvest is coming: "); sb_i(&n, out.peek_yield, 0); sb_s(&n, " bushels an acre.");
        }
        cpy(note, n_buf);
        SB(s);
        sb_s(&s, "hamurabi: choice "); sb_i(&s, i, 0); sb_s(&s, " people "); sb_i(&s, city.people, 0);
        sb_s(&s, " acres "); sb_i(&s, city.acres, 0); sb_s(&s, " grain "); sb_i(&s, city.grain, 0);
        logsb(&s);
        say("hamurabi: note ", note, "");
    }
    to_orders();
}
static NOINL void do_submit(void) {
    if (phase != PH_ORDERS || hamurabi_check(&city, &ord)) return;
    log_orders("orders");
    last_plant = ord.plant; last_guard = city.guarded;
    yr = hamurabi_step(&city, &ord, &rng, rules);
    harvested += yr.harvest; plagues += yr.plague; last_starved = yr.starved;
    pips[yr.year - 1] = (yr.starved > 0 || yr.plague) ? 2 : 1;
    {
        SB(s);
        sb_s(&s, "hamurabi: report year "); sb_i(&s, yr.year, 0); sb_s(&s, " yield "); sb_i(&s, yr.yield, 0);
        sb_s(&s, " harvest "); sb_i(&s, yr.harvest, 0); sb_s(&s, " rats "); sb_i(&s, yr.rats, 0);
        sb_s(&s, " born "); sb_i(&s, yr.born, 0); sb_s(&s, " plague "); sb_i(&s, yr.plague, 0);
        logsb(&s);
    }
    {
        SB(s);
        sb_s(&s, "hamurabi: year "); sb_i(&s, yr.year, 0); sb_s(&s, " people "); sb_i(&s, city.people, 0);
        sb_s(&s, " acres "); sb_i(&s, city.acres, 0); sb_s(&s, " grain "); sb_i(&s, city.grain, 0);
        sb_s(&s, " starved "); sb_i(&s, yr.starved, 0);
        logsb(&s);
    }
    phase = PH_REPORT; rep_start = tm; demo_t = 0;
}
static NOINL void next_year(void) {
    if (phase != PH_REPORT) return;
    if (!city.over) { enter_year(); return; }
    phase = PH_OVER; ofocus = 0; demo_t = 0;
    say("hamurabi: over grade ", GRADE[hamurabi_grade(&city)], "");
}
/* The robot king rules a classic reign by himself, with pauses a person can follow. */
static NOINL void demo_tick(u32 now) {
    if (phase == PH_ORDERS) {
        if (!demo_t) demo_t = now + 900;
        else if (now >= demo_t) {
            if (!demo_set) { ord = hamurabi_ruler_legal(&city, &hamurabi_ruler_default); demo_set = 1; demo_t = now + 1100; }
            else { do_submit(); demo_set = 0; }
        }
    } else if (phase == PH_REPORT) {
        if (!demo_t) demo_t = now + (yr.plague ? 6500u : 3500u);
        else if (now >= demo_t) next_year();
    }
}

/* ---------- drawing the screens ---------- */
struct hit { short x, y, w, h; u8 act, arg; };
static struct hit hits[24] JT_DATA;
static int nhits JT_DATA = 0;
static void hit_add(int x, int y, int w, int h, int act, int arg) {
    if (nhits < 24) hits[nhits++] = (struct hit){ (short)x, (short)y, (short)w, (short)h, (u8)act, (u8)arg };
}

static void ring(int x, int y, int w, int h) {   /* focus ring: two pixels of accent just outside the box */
    for (int k = 0; k < 2; k++) {
        blendrect(x - 2 + k + 8, y - 2 + k, w + 4 - 2 * k - 16, 1, ACCENT, 255);
        blendrect(x - 2 + k + 8, y + h + 1 - k, w + 4 - 2 * k - 16, 1, ACCENT, 255);
        blendrect(x - 2 + k, y - 2 + k + 8, 1, h + 4 - 2 * k - 16, ACCENT, 255);
        blendrect(x + w + 1 - k, y - 2 + k + 8, 1, h + 4 - 2 * k - 16, ACCENT, 255);
    }
}
static void button(int x, int y, int w, int h, const char *label, int primary, int enabled, int focused, int act, int arg) {
    int down = enabled && pressed_act == act && pressed_arg == arg, dy = down ? 1 : 0;
    if (enabled) hit_add(x, y, w, h, act, arg);
    u32 fill = primary ? (!enabled ? 0xA9ADB5u : down ? PRESSED_ACCENT : ACCENT) : (down ? 0xD9DADFu : 0xFFFFFFu);
    if (focused && enabled) rrect(x - 2, y - 2, w + 4, h + 4, 14, primary ? INK : ACCENT, 255);   /* focus ring: two pixels round the button, ink on a terracotta one */
    if (primary) rrect(x, y + dy, w, h, 12, fill, 255);
    else { rrect(x, y + dy, w, h, 12, 0xDDDEE3u, 255); rrect(x + 1, y + dy + 1, w - 2, h - 2, 11, fill, 255); }  /* a 1-pixel border so a white button reads on the white panel */
    text_c(JT_FACE_BOLD, x + w / 2, y + dy + (h - 16) / 2, primary ? 0xFFFFFFu : enabled ? INK : 0xA9ADB5u, label);
}
static void round_btn(int x, int y, int d, int plus, int act, int arg) {
    hit_add(x, y, d, d, act, arg);
    rrect(x, y, d, d, d / 2, ACCENT, 255);
    rrect(x + 2, y + 2, d - 4, d - 4, d / 2 - 2, 0xFFFFFF, 255);
    fillrect(x + d / 2 - 5, y + d / 2 - 1, 10, 2, ACCENT);
    if (plus) fillrect(x + d / 2 - 1, y + d / 2 - 5, 2, 10, ACCENT);
}
static void pill(int x, int y, int w, const char *s, int act) {
    glass(x, y, w, 34, 17);
    if (act) hit_add(x, y, w, 34, act, 0);
    text_c(JT_FACE_BOLD, x + w / 2, y + 9, INK, s);
}

static NOINL void hud(void) {
    int cw = (int)cv.width;
    if (phase == PH_TITLE || phase == PH_OVER) return;
    int cell = imin(96, (cw - 24 - 32) / 4), w = cell * 4 + 32, h = 56, x = (cw - w) / 2, y = 8;
    glass(x, y, w, h, 18);
    for (int i = 0; i < 4; i++) {
        SB(v);
        int cx = x + 16 + cell * i + cell / 2;
        if (i == 0) { sb_i(&v, phase == PH_REPORT ? yr.year : imin(city.year, HAMURABI_YEARS), 0); sb_s(&v, " of 10"); }
        else sb_i(&v, i == 1 ? city.people : i == 2 ? city.acres : city.grain, 1);
        text_c(JT_FACE_BOLD, cx, y + 8, INK, v_buf);
        text_c(JT_FACE_BODY, cx, y + 24, MUTED, i == 0 ? "Year" : i == 1 ? "People" : i == 2 ? "Acres" : "Grain");
    }
    for (int i = 0; i < HAMURABI_YEARS; i++) {   /* a pip a year: dark when it went well, terracotta when people starved or the plague came */
        int px = x + w / 2 - (HAMURABI_YEARS * 13) / 2 + i * 13;
        u32 c = pips[i] == 2 ? ACCENT : pips[i] == 1 ? INK : 0xD6D6DAu;
        int cur = !pips[i] && i + 1 == city.year;
        fillrect(px, y + 44, cur ? 11 : 8, 4, c);
    }
    if (cw >= 620) {
        if (mode == M_DEMO) pill(cw - 12 - 156, 8, 156, "Any key to stop", 0);
        else pill(cw - 12 - 84, 8, 84, "Menu", A_MENU);
    }
}

static NOINL void ui_title(void) {
    int cw = (int)cv.width, chh = (int)cv.height;
    /* heading panel */
    int f = cw >= 900 ? 4 : cw >= 560 ? 3 : 2;
    if (chh < 300 && f > 2) f = 2;
    const char *sub = "Be king for 10 years. Feed your people. Keep your crown.";
    if (jt_text_width(JT_FACE_BODY, sub) > cw - 80) sub = "Feed your people. Keep your crown.";
    int tw = jt_text_width(JT_FACE_BOLD, TITLE) * f, sw = jt_text_width(JT_FACE_BODY, sub);
    int hw = imax(tw, sw) + 56, hh = 10 + 16 * f + 2 + 16 + 12, hx = (cw - hw) / 2, hy = 10;
    glass(hx, hy, hw, hh, 22);
    big_text(TITLE, cw / 2, hy + 10 - 2 * f / 2, f, INK, 255);
    text_c(JT_FACE_BODY, cw / 2, hy + 10 + 16 * f - 2, MUTED, sub);

    /* button panel */
    int bw = btn[0].w + 28, bx = btn[0].x - 14, by = panel_y, bh = 14 + 44 + 8 + 36 + 8 + 16 + 12;
    glass(bx, by, bw, bh, 22);
    for (int i = 0; i < 3; i++) button(btn[i].x, btn[i].y, btn[i].w, btn[i].h, btn[i].label, btn[i].primary, 1, focus == i, A_TITLE, i);
    static const char *const CAP[3] = {
        "Based on Hamurabi by Doug Dyment, 1968. Copyright 2026 Joshua Trommel. MIT License.",
        "Based on Hamurabi by Doug Dyment, 1968. Copyright 2026 Joshua Trommel.",
        "Based on Hamurabi by Doug Dyment, 1968." };
    for (int i = 0; i < 3; i++) if (i == 2 || jt_text_width(JT_FACE_BODY, CAP[i]) <= bw - 28) {
        text_c(JT_FACE_BODY, cw / 2, by + 14 + 44 + 8 + 36 + 8 - 1, MUTED, CAP[i]);
        break;
    }
}

static NOINL void ui_card(void) {
    int cw = (int)cv.width, chh = (int)cv.height;
    int pw = imin(cw - 24, 640), px = (cw - pw) / 2, inner = pw - 28;
    int nl = wrap(JT_FACE_BODY, 0, 0, inner, 18, 0, card_text, 4, 0);
    int two = omen != 0, side = 0;
    if (two) {
        int half = (inner - 8) / 2;
        side = jt_text_width(JT_FACE_BOLD, omen->choice[0].label) + 28 <= half && jt_text_width(JT_FACE_BOLD, omen->choice[1].label) + 28 <= half;
    }
    int btns_h = two && !side ? 44 + 8 + 40 : 44;
    int ph = 14 + 22 + 4 + nl * 18 + 12 + btns_h + 14, py = chh - 10 - ph;
    glass(px, py, pw, ph, 22);
    jt_text_draw(&cv, JT_FACE_BOLD, px + 14, py + 14, INK, card_title);
    wrap(JT_FACE_BODY, px + 14, py + 14 + 22 + 4, inner, 18, INK, card_text, 4, 1);
    int by = py + ph - 14 - btns_h;
    if (!two) { button(px + 14, by, inner, 44, "Continue", 1, 1, 0, A_CHOICE, 0); return; }
    for (int i = 0; i < 2; i++) {
        int on = afford(i);
        if (side) { int half = (inner - 8) / 2; button(px + 14 + i * (half + 8), by, half, 44, omen->choice[i].label, !i, on, cfocus == i, A_CHOICE, i); }
        else button(px + 14, by + i * 52, inner, i ? 40 : 44, omen->choice[i].label, !i, on, cfocus == i, A_CHOICE, i);
    }
}

static const char *const WHY[8] = {
    "", "You do not own that much land to sell.", "You do not have the grain to buy that much land.", "Orders cannot be negative.",
    "You do not have that much grain to feed people.", "You do not own that much land to plant.", "There is not enough grain left for seed.",
    "You do not have enough people to farm that much." };

static NOINL void ui_orders(void) {
    int cw = (int)cv.width, chh = (int)cv.height;
    int pw = imin(cw - 24, 1040), px = (cw - pw) / 2, inner = pw - 28;
    const char *top = note;
    if (!top[0]) top = mode == M_DEMO ? "The robot king is ruling. Press any key to stop." : mode == M_STORY && city.year <= 4 ? hamurabi_hints[city.year - 1]
        : "Up and Down pick a row. Left and Right change it, Shift for big steps. Enter ends the year.";
    int nl = imin(wrap(JT_FACE_BODY, 0, 0, inner - 24, 16, 0, top, 2, 0), 2), nh = nl ? nl * 16 + 14 + 10 : 0;
    int colh = 98, ph = 12 + nh + colh + 12, py = chh - 10 - ph, y = py + 12;
    glass(px, py, pw, ph, 22);
    if (nl) {
        rrect(px + 14, y, inner, nl * 16 + 14, 12, 0xFFFFFF, 160);
        wrap(JT_FACE_BODY, px + 14 + 12, y + 7, inner - 24, 16, INK, top, 2, 1);
        y += nh;
    }
    int sumw = pw >= 700 ? 190 : 150, gap = 16, colw = (inner - sumw - 3 * gap) / 3;
    for (int r = 0; r < 3; r++) {
        int x0 = px + 14 + r * (colw + gap), v = *rowp(r), lo = row_min(r), hi = row_max(r), en = hi > lo && mode != M_DEMO;
        if (r == row) ring(x0 - 6, y - 4, colw + 12, colh + 2);
        jt_text_draw(&cv, JT_FACE_BOLD, x0, y, r == row ? ACCENT : INK, r == 0 ? "Land" : r == 1 ? "Feed" : "Plant");
        if (r > 0) {
            const char *q = r == 1 ? "Everyone" : "All I can";
            int qw = jt_text_width(JT_FACE_BODY, q);
            jt_text_draw(&cv, JT_FACE_BODY, x0 + colw - qw, y, ACCENT, q);
            if (mode != M_DEMO) hit_add(x0 + colw - qw - 4, y - 2, qw + 8, 20, A_QUICK, r);
        }
        SB(vb); SB(db); SB(d2);
        if (r == 0) { sb_s(&vb, v >= 0 ? "Buy " : "Sell "); sb_i(&vb, v < 0 ? -v : v, 1); }
        else sb_i(&vb, v, 1);
        if (r == 0) {
            sb_s(&db, v >= 0 ? "costs " : "pays "); sb_i(&db, (v < 0 ? -v : v) * city.price, 1); sb_s(&db, " bushels");
            sb_s(&d2, "at "); sb_i(&d2, city.price, 0); sb_s(&d2, " bushels an acre");
        } else if (r == 1) {
            sb_s(&db, "feeds "); sb_i(&db, imin(v / HAMURABI_FOOD, city.people), 1); sb_s(&db, " of "); sb_i(&db, city.people, 1); sb_s(&db, " people");
            sb_s(&d2, "20 bushels each");
        } else {
            sb_s(&db, "acres to sow");
            sb_s(&d2, "seed: "); sb_i(&d2, v, 1); sb_s(&d2, " bushels");
        }
        jt_text_draw(&cv, JT_FACE_BOLD, x0, y + 18, INK, vb_buf);
        jt_text_draw(&cv, JT_FACE_BODY, x0, y + 36, MUTED, db_buf);
        jt_text_draw(&cv, JT_FACE_BODY, x0, y + 52, MUTED, d2_buf);
        int cy = y + 83, bx = x0 + 28 + 12, bw = colw - 2 * (28 + 12);
        round_btn(x0, cy - 14, 28, 0, A_STEP, r * 2);
        round_btn(x0 + colw - 28, cy - 14, 28, 1, A_STEP, r * 2 + 1);
        hit_add(bx - 6, cy - 14, bw + 12, 28, A_BAR, r);
        rrect(bx, cy - 3, bw, 6, 3, en ? 0xDDDEE3u : 0xE9EAEDu, 255);
        if (hi > lo) {
            int kx = bx + (v - lo) * bw / (hi - lo);
            if (lo < 0) fillrect(bx + (0 - lo) * bw / (hi - lo), cy - 7, 2, 14, 0xB9BBC2u);
            if (en) {
                rrect(bx, cy - 3, imax(kx - bx, 6), 6, 3, ACCENT, 255);
                rrect(kx - 7, cy - 7, 14, 14, 7, ACCENT, 255);
                rrect(kx - 5, cy - 5, 10, 10, 5, 0xFFFFFF, 255);
            }
        }
    }
    int sx = px + 14 + 3 * (colw + gap) , err = hamurabi_check(&city, &ord);
    int fed = imin(ord.feed / HAMURABI_FOOD, city.people), left = city.grain - ord.buy * city.price - ord.feed - ord.plant;
    SB(lb); SB(wb);
    sb_i(&lb, left, 1);
    if (err) sb_s(&wb, WHY[err]);
    else if (city.people - fed > 0) {
        sb_i(&wb, city.people - fed, 1); sb_s(&wb, " will go hungry.");
        if (ord.buy >= 0 && city.grain - ord.buy * city.price < city.people * HAMURABI_FOOD) sb_s(&wb, " Sell land to buy food.");
    } else sb_s(&wb, "Everyone eats");
    jt_text_draw(&cv, JT_FACE_BODY, sx, y, MUTED, "Grain left");
    jt_text_draw(&cv, JT_FACE_BOLD, sx + sumw - jt_text_width(JT_FACE_BOLD, lb_buf), y, INK, lb_buf);
    wrap(JT_FACE_BODY, sx, y + 20, sumw, 16, err || city.people - fed > 0 ? ACCENT : MUTED, wb_buf, 2, 1);
    button(sx, y + colh - 42, sumw, 42, "End the year", 1, !err && mode != M_DEMO, 0, A_SUBMIT, 0);
}

static NOINL void ui_report(void) {
    int cw = (int)cv.width, chh = (int)cv.height;
    int pw = imin(cw - 24, 700), px = (cw - pw) / 2, bw = 170, tw = pw - 28 - bw - 16;
    char txt[6][100]; u8 bad[6]; int n = 0;
    {
        SB(s);
        sb_s(&s, yr.yield <= 2 ? "Bad" : yr.yield >= 4 ? "Great" : "Good"); sb_s(&s, " harvest: "); sb_i(&s, yr.yield, 0);
        sb_s(&s, yr.yield == 1 ? " bushel an acre, " : " bushels an acre, "); sb_i(&s, yr.harvest, 1); sb_s(&s, " in all.");
        cpy(txt[n], s_buf);
        bad[n++] = yr.yield <= 2;
    }
    {
        SB(s);
        if (yr.rats > 0) { sb_s(&s, "Rats ate "); sb_i(&s, yr.rats, 1); sb_s(&s, " bushels."); } else sb_s(&s, "No rats this year.");
        cpy(txt[n], s_buf);
        bad[n++] = yr.rats > 0;
    }
    {
        SB(s);
        if (yr.starved > 0) { sb_i(&s, yr.starved, 1); sb_s(&s, yr.starved == 1 ? " person starved." : " people starved."); } else sb_s(&s, "Nobody starved.");
        cpy(txt[n], s_buf);
        bad[n++] = yr.starved > 0;
    }
    if (yr.born > 0) {
        SB(s);
        sb_i(&s, yr.born, 1); sb_s(&s, yr.born == 1 ? " new person moved in." : " new people moved in.");
        cpy(txt[n], s_buf);
        bad[n++] = 0;
    }
    if (yr.plague) {
        cpy(txt[n], "A sickness killed half your people.");
        bad[n++] = 1;
    }
    if (city.over) {
        SB(s);
        if (city.over == 1) { sb_s(&s, "You let "); sb_i(&s, yr.starved, 1); sb_s(&s, " people starve in one year. The people threw you out!"); }
        else sb_s(&s, "Your 10 years are over.");
        cpy(txt[n], s_buf);
        bad[n++] = city.over == 1;
    }
    int rows = 0;
    for (int i = 0; i < n; i++) rows += wrap(JT_FACE_BODY, 0, 0, tw, 18, 0, txt[i], 2, 0);
    int ph = imax(14 + 22 + rows * 18 + 14, 14 + 44 + 14), py = chh - 10 - ph, y = py + 14;
    glass(px, py, pw, ph, 22);
    SB(h);
    sb_s(&h, "Year "); sb_i(&h, yr.year, 0);
    jt_text_draw(&cv, JT_FACE_BOLD, px + 14, y, INK, h_buf);
    y += 22;
    for (int i = 0; i < n; i++) y += 18 * wrap(JT_FACE_BODY, px + 14, y, tw, 18, bad[i] ? ACCENT : INK, txt[i], 2, 1);
    button(px + pw - 14 - bw, py + ph - 14 - 44, bw, 44, city.over ? "See your grade" : "Next year", 1, mode != M_DEMO, 0, A_NEXT, 0);
}

static NOINL void ui_over(void) {
    int cw = (int)cv.width, chh = (int)cv.height;
    int g = hamurabi_grade(&city), pw = imin(cw - 24, 780), px = (cw - pw) / 2, lw = 130, bw = 176;
    int mx = px + 14 + lw, mw = pw - 28 - lw - bw - 16;
    SB(rs);
    if (city.over == 1) { sb_s(&rs, "You let "); sb_i(&rs, last_starved, 1); sb_s(&rs, " people starve in one year. The people threw you out!"); }
    else sb_s(&rs, "Your 10 years are over.");
    if (mode == M_DEMO) sb_s(&rs, " The robot played this one.");
    const char *hint = g != 3 && city.over != 1 ? "For an A+, let almost nobody starve and keep 10 acres for each person." : "";
    int rl = wrap(JT_FACE_BODY, 0, 0, mw, 16, 0, rs_buf, 2, 0), el = wrap(JT_FACE_BODY, 0, 0, mw, 16, 0, hamurabi_epilogues[g], 3, 0);
    int hl = hint[0] ? wrap(JT_FACE_BODY, 0, 0, mw, 16, 0, hint, 2, 0) : 0;
    int ph = imax(14 + rl * 16 + 6 + el * 16 + (hl ? 4 + hl * 16 : 0) + 12 + 34 + 14, 14 + 100 + 14), py = chh - 10 - ph, y = py + 14;
    glass(px, py, pw, ph, 22);
    big_text(GRADE[g], px + 14 + lw / 2, py + (ph - 100) / 2 + 6, 5, g == 0 ? ACCENT : INK, 255);
    y += 16 * wrap(JT_FACE_BODY, mx, y, mw, 16, MUTED, rs_buf, 2, 1) + 6;
    y += 16 * wrap(JT_FACE_BODY, mx, y, mw, 16, INK, hamurabi_epilogues[g], 3, 1);
    if (hl) y += 4 + 16 * wrap(JT_FACE_BODY, mx, y + 4, mw, 16, MUTED, hint, 2, 1);
    y = py + ph - 14 - 34;
    {
        static const char *const LAB[5] = { "People", "Acres each", "Starved", "Harvested", "Plagues" };
        for (int i = 0; i < 5; i++) {
            SB(v);
            if (i == 0) sb_i(&v, city.people, 1);
            else if (i == 1) { if (city.people <= 0) sb_s(&v, "none"); else { int e = city.acres * 10 / city.people; sb_i(&v, e / 10, 0); sb_c(&v, '.'); sb_i(&v, e % 10, 0); } }
            else if (i == 2) sb_i(&v, city.starved_total, 1);
            else if (i == 3) sb_i(&v, harvested, 1);
            else sb_i(&v, plagues, 0);
            int x = mx + i * mw / 5;
            jt_text_draw(&cv, JT_FACE_BOLD, x, y, INK, v_buf);
            jt_text_draw(&cv, JT_FACE_BODY, x, y + 18, MUTED, LAB[i]);
        }
    }
    if (mode == M_DEMO) wrap(JT_FACE_BODY, px + pw - 14 - bw, py + 14, bw, 18, MUTED, "Press any key to go back to the menu.", 3, 1);
    else {
        button(px + pw - 14 - bw, py + 14, bw, 44, "Play again", 1, 1, ofocus == 0, A_AGAIN, 0);
        button(px + pw - 14 - bw, py + 14 + 52, bw, 40, "Menu", 0, 1, ofocus == 1, A_MENU, 0);
    }
}

/* One frame: paint it all into the back buffer, then copy it over the window in one quick pass. */
static void draw(void) {
    cv.width = win.width; cv.height = win.height; cv.pitch = win.width;
    layout();
    snapshot();
    scene();
    nhits = 0;
    hud();
    switch (phase) {
    case PH_TITLE: ui_title(); break;
    case PH_CARD: ui_card(); break;
    case PH_ORDERS: ui_orders(); break;
    case PH_REPORT: ui_report(); break;
    default: ui_over(); break;
    }
    u32 n = win.width * win.height;
    for (u32 i = 0; i < n; i++) win.pixels[i] = cv.pixels[i];
}

/* ---------- input ---------- */
/* A button press: logged on the title, drawn pressed for a moment, then acted on. */
static NOINL void activate(int act, int arg, u32 now) {
    if (pend_act) return;
    pressed_act = act; pressed_arg = arg; pressed_until = now + 200; pend_act = act; pend_arg = arg;
    if (act == A_TITLE) say("hamurabi: ", LABELS[arg], " chosen");
}
static NOINL void run_pending(void) {
    int act = pend_act, arg = pend_arg;
    pend_act = 0; pressed_act = 0;
    switch (act) {
    case A_TITLE: begin_reign(arg == 0 ? M_STORY : arg == 1 ? M_CLASSIC : M_DEMO); break;
    case A_CHOICE: choose_card(arg); break;
    case A_SUBMIT: do_submit(); break;
    case A_NEXT: next_year(); break;
    case A_AGAIN: begin_reign(mode); break;
    case A_MENU: to_title(); break;
    }
}
static void quick(int r) { row_set(r, r == 1 ? imin(max_feed(), city.people * HAMURABI_FOOD) : row_max(r)); }

static NOINL int on_key(int k, u32 now) {   /* returns 1 to close the program */
    if (k == '`') {
        jt_write(1, "hamurabi: crashing on purpose\n", 30);
        *(volatile int *)0 = 1;
    }
    if (pend_act) return 0;
    if (phase == PH_TITLE) {
        if (k == JT_KEY_ESC) return 1;
        if (k == JT_KEY_RIGHT || k == JT_KEY_DOWN || k == '\t') focus = focus < 0 ? 0 : imin(focus + 1, 2);
        else if (k == JT_KEY_LEFT || k == JT_KEY_UP) focus = focus < 0 ? 0 : imax(focus - 1, 0);
        else if (k == JT_KEY_ENTER || k == ' ') activate(A_TITLE, focus < 0 ? 0 : focus, now);
        return 0;
    }
    if (mode == M_DEMO || k == JT_KEY_ESC) { to_title(); return 0; }   /* the demo stops on any key */
    if (phase == PH_CARD) {
        int n = omen ? 2 : 1, dir = (k == JT_KEY_LEFT || k == JT_KEY_UP) ? -1 : (k == JT_KEY_RIGHT || k == JT_KEY_DOWN || k == '\t') ? 1 : 0;
        if (dir && n == 2 && afford(cfocus ^ 1)) cfocus ^= 1;
        else if ((k == '1' || k == '2') && k - '1' < n) { if (!omen || afford(k - '1')) activate(A_CHOICE, k - '1', now); }
        else if ((k == JT_KEY_ENTER || k == ' ') && (!omen || afford(cfocus))) activate(A_CHOICE, cfocus, now);
    } else if (phase == PH_ORDERS) {
        if (k == JT_KEY_UP || k == JT_KEY_SUP) row = (row + 2) % 3;
        else if (k == JT_KEY_DOWN || k == JT_KEY_SDOWN || k == '\t') row = (row + 1) % 3;
        else if (k == JT_KEY_LEFT || k == JT_KEY_RIGHT || k == JT_KEY_SLEFT || k == JT_KEY_SRIGHT) {
            int big = k == JT_KEY_SLEFT || k == JT_KEY_SRIGHT, d = (k == JT_KEY_LEFT || k == JT_KEY_SLEFT) ? -1 : 1;
            row_set(row, *rowp(row) + d * (big ? BIG[row] : STEP[row]));
        }
        else if (k == JT_KEY_HOME) row_set(row, row_min(row) < 0 ? 0 : row_min(row));
        else if (k == JT_KEY_END) quick(row);
        else if (k == JT_KEY_ENTER || k == ' ') activate(A_SUBMIT, 0, now);
    } else if (phase == PH_REPORT) {
        if (k == JT_KEY_ENTER || k == ' ' || k == JT_KEY_RIGHT) activate(A_NEXT, 0, now);
    } else {
        if (k == JT_KEY_LEFT || k == JT_KEY_UP) ofocus = 0;
        else if (k == JT_KEY_RIGHT || k == JT_KEY_DOWN || k == '\t') ofocus = 1;
        else if (k == JT_KEY_ENTER || k == ' ') activate(ofocus ? A_MENU : A_AGAIN, 0, now);
    }
    return 0;
}

static NOINL void on_click(int mx, int my, u32 now) {
    if (pend_act) return;
    if (phase != PH_TITLE && mode == M_DEMO) { to_title(); return; }   /* the demo stops on any click */
    for (int i = nhits - 1; i >= 0; i--) {
        struct hit *h = &hits[i];
        if (mx < h->x || mx >= h->x + h->w || my < h->y || my >= h->y + h->h) continue;
        if (h->act == A_STEP) { int r = h->arg >> 1; row = r; row_set(r, *rowp(r) + ((h->arg & 1) ? 1 : -1) * STEP[r]); }
        else if (h->act == A_QUICK) { row = h->arg; quick(h->arg); }
        else if (h->act == A_BAR) {
            int r = h->arg, lo = row_min(r), hi = row_max(r), bx = h->x + 6, bw = h->w - 12;
            row = r;
            if (hi > lo) {
                int v = lo + (mx - bx) * (hi - lo) / bw;
                if (mx <= bx) v = lo; else if (mx >= bx + bw) v = hi;
                else v = lo + (v - lo + STEP[r] / 2) / STEP[r] * STEP[r];   /* a click lands on a whole ration, a whole person's tending, a whole acre */
                row_set(r, v);
            }
        } else activate(h->act, h->arg, now);
        return;
    }
}

static void unpack(void) {
    int p = 0;
    for (unsigned i = 0; i + 1 < HAM_SHEET_RLE_LEN && p < SHEET_BYTES; i += 2)
        for (int k = 0; k < ham_sheet_rle[i] && p < SHEET_BYTES; k++) sheet[p++] = ham_sheet_rle[i + 1];
}

static void num(char *out, int *n, int v) {
    char t[12]; int k = 0;
    if (v < 0) { out[(*n)++] = '-'; v = -v; }
    do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k) out[(*n)++] = t[--k];
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open(&win) != 0 || !win.pixels) {
        jt_write(2, "hamurabi: no window\n", 20);
        jt_exit(1);
    }
    {   /* one line, one write: what tools/checks/ring3hamurabi-check.py asserts on */
        char line[48]; int l = 0;
        const char *pfx = "hamurabi: ring-3 window ";
        while (*pfx) line[l++] = *pfx++;
        num(line, &l, (int)win.width); line[l++] = 'x'; num(line, &l, (int)win.height); line[l++] = '\n';
        jt_write(1, line, (unsigned)l);
    }
    {   /* the sheet, the title's scratch, the walkers and the back buffer live on the SYS_BRK heap */
        unsigned base = (unsigned)jt_brk(0), need = (SHEET_BYTES + MASK_BYTES + FOLK_BYTES + BB_BYTES + 4095u) & ~4095u;
        if (win.width * win.height * 4u > BB_BYTES) { jt_write(2, "hamurabi: window too big\n", 25); jt_exit(1); }
        unsigned top = (unsigned)jt_brk(base + need); /* the heap lives at 0xFF000000: "negative" as an int, so errors are -4095..-1 only */
        if (top >= 0xFFFFF000u || top < base + need) { jt_write(2, "hamurabi: no heap\n", 18); jt_exit(1); }
        sheet = (u8 *)base;
        mask = (u32 *)(base + SHEET_BYTES);
        fk = (struct folk *)(base + SHEET_BYTES + MASK_BYTES);
        cv.pixels = (u32 *)(base + SHEET_BYTES + MASK_BYTES + FOLK_BYTES);
    }
    unpack();

    u32 start = now_ms(), next = 0;
    tm = 0;
    draw();
    {   /* where the ziggurat landed, so the check can compare real pixels with the sheet */
        char line[64]; int l = 0;
        const char *pfx = "hamurabi: scene scale ";
        while (*pfx) line[l++] = *pfx++;
        num(line, &l, S);
        const char *z = " ziggurat ";
        while (*z) line[l++] = *z++;
        num(line, &l, zig_x); line[l++] = ','; num(line, &l, zig_y); line[l++] = '\n';
        jt_write(1, line, (unsigned)l);
    }
    {   /* where the three buttons are, so the check clicks them without re-deriving the layout */
        char line[96]; int l = 0;
        const char *pfx = "hamurabi: buttons";
        while (*pfx) line[l++] = *pfx++;
        for (int i = 0; i < 3; i++) {
            line[l++] = ' ';
            num(line, &l, btn[i].x); line[l++] = ','; num(line, &l, btn[i].y); line[l++] = ',';
            num(line, &l, btn[i].w); line[l++] = ','; num(line, &l, btn[i].h);
        }
        line[l++] = '\n';
        jt_write(1, line, (unsigned)l);
    }
    {   /* the title it draws, so the check can assert the product name */
        char line[48]; int l = 0;
        const char *pfx = "hamurabi: title ";
        while (*pfx) line[l++] = *pfx++;
        for (const char *t = TITLE; *t; t++) line[l++] = *t;
        line[l++] = '\n';
        jt_write(1, line, (unsigned)l);
    }
    {   /* how long one frame takes, in 10 ms ticks, so a slow scene shows in the log */
        char line[48]; int l = 0;
        const char *pfx = "hamurabi: first frame ms ";
        while (*pfx) line[l++] = *pfx++;
        num(line, &l, (int)(now_ms() - start)); line[l++] = '\n';
        jt_write(1, line, (unsigned)l);
    }
    jt_write(1, "hamurabi: phase title\n", 22);
    unsigned flags = JT_POLL_PRESENT;
    int dirty = 0, quit = 0;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        if (r == 1 && jt_window_resized(&ev, &win)) { draw(); flags = JT_POLL_PRESENT; continue; } /* JT_EV_RESIZE: remapped, repaint at the new size */
        flags = 0;
        u32 now = now_ms();
        tm = now - start;
        if (pend_act && now >= pressed_until) { run_pending(); dirty = 1; }
        if (mode == M_DEMO && phase != PH_TITLE) { int p0 = phase; demo_tick(now); if (phase != p0) dirty = 1; }
        if (r == 1) {
            if (ev.kind == JT_EV_KEY) quit = on_key(ev.a, now);
            else if (ev.kind == JT_EV_CLICK) on_click(ev.a, ev.b, now);
            else if (ev.kind == JT_EV_WHEEL && phase == PH_ORDERS && mode != M_DEMO) row_set(row, *rowp(row) + ev.a * STEP[row]);
            if (quit) break;
            dirty = 1;
            continue;   /* take every waiting event before painting once */
        }
        if (r != -11 /* -EAGAIN */) break;
        if (dirty || now >= next) { draw(); flags = JT_POLL_PRESENT; next = now + 80; dirty = 0; }
        else jt_sched_yield();
    }
    jt_write(1, "hamurabi: closed\n", 17);
    jt_exit(0);
}
