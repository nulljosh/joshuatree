/* hamurabi: the 1968 game of ruling a city for ten years, as a ring-3 program. This is the first
 * slice: the title screen with the real drawn scene, and nothing behind its buttons yet.
 *
 * The game is shown as Hamurapi (the store name Hamurabi was taken); only the credit to the 1968 game and the
 * internal names (this file, HAMURABI.BIN, open=hamurabi, the hamurabi_ prefixes) keep the old spelling.
 *
 * The scene is a port of web/play/scene.js from the Hamurabi repo: a sky, two ridges of far hills,
 * a town of houses around a ziggurat with a king on top, a river with a boat, fields, sheep and
 * villagers pacing their lanes. Every sprite comes from one 256x149 sheet (user/hamurabi_sprites.h,
 * made by tools/gen/gen_hamurabi_sprites.py). The sheet is stored as run-length coded palette
 * indices; at start it is unpacked into a 38 KB byte image on the SYS_BRK heap, so the flat binary
 * stays far under its 128 KB window. A sprite pixel is drawn as a whole S x S block, S the largest
 * whole number the window allows, so the pixels stay crisp. Semi-see-through shades (the shadows
 * and ghosts) are alpha-blended.
 *
 * No floats and no 64-bit division: the scene's sine is a 65-entry table, its random numbers are
 * the same integer hash the web uses, and time is the 100 Hz tick counter turned into milliseconds.
 * The ABI hands a program clicks, keys and wheel ticks but no pointer movement, so there is no
 * hover; a click shows the button pressed for a moment, and the arrow keys move a focus ring that
 * Enter presses. A button logs "hamurabi: <label> chosen" on the serial port and does nothing else
 * yet. Esc closes. Resizing re-lays the scene out for the new window.
 */
#include "jtsys.h"
#include "libjt/text.h"
#include "hamurabi_sprites.h"

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;

#define INK    0x00232338u /* house ink */
#define ACCENT 0x00B5502Cu /* terracotta */
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
static int focus JT_DATA = -1, pressed JT_DATA = -1;
static u32 pressed_until JT_DATA = 0;

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
static int isin(int a_q8) { return sin_turns((u32)((a_q8 * 40743) / 1000)); }
/* sin(t * w + ph): t in seconds, w in Q8 radians a second, ph in Q8 radians. One cycle at a time keeps it in 32 bits. */
static int tsin(int w_q8, int ph_q8) {
    u32 per = 1608496u / (u32)w_q8;
    return isin((int)(((tm % per) * (u32)w_q8) / 1000u) + ph_q8);
}
/* How far something moving at sp_q4 (a sixteenth of an art pixel per second) has gone: Q4, no overflow for days. */
static u32 mv(u32 sp_q4) { return (tm / 1000u) * sp_q4 + (tm % 1000u) * sp_q4 / 1000u; }

/* The web scene's own hash, so every sprite keeps the habits it has there: 0..99999. */
static u32 rnd(u32 i, u32 salt) {
    u32 x = i * 7919u + salt * 104729u + 12345u;
    x = (x ^ (x >> 15)) * 0x2C1B3C6Du;
    x = (x ^ (x >> 12)) * 0x297A2D39u;
    x ^= x >> 15;
    return x % 100000u;
}

/* ---------- pixels ---------- */
static u32 mix(u32 a, u32 b, int k) { /* k 0..256 from a to b */
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
static void arect(int x16, int y16, int w16, int h16, u32 rgb, int a) {
    int x0 = (x16 * S + 8) >> 4, y0 = (y16 * S + 8) >> 4;
    int x1 = ((x16 + w16) * S + 8) >> 4, y1 = ((y16 + h16) * S + 8) >> 4;
    if (x1 <= x0) x1 = x0 + 1;
    if (y1 <= y0) y1 = y0 + 1;
    blendrect(x0, y0, x1 - x0, y1 - y0, rgb, a);
}
static void arect1(int x, int y, int w, int h, u32 rgb, int a) { arect(x * 16, y * 16, w * 16, h * 16, rgb, a); }

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
            int a = (int)(c >> 24), x0 = px0 + sx * S;
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
            blendrect(X - half, Y + dy, 2 * half, 1, 0, 33);
        }
    }
    spr(id, cx - sw / 2, foot - sh + 1 - lift, flip);
}

/* ---------- the scene ---------- */
static void scene(void) {
    int cw = (int)cv.width, chh = (int)cv.height;
    int hz = HZ;
    /* sky: pale rose at the top to pale blue at the horizon */
    for (int y = 0; y < hz * S && y < chh; y++) {
        int k = hz * S > 1 ? y * 256 / (hz * S - 1) : 0;
        fillrect(0, y, cw, 1, mix(0xFCF3F2u, 0xECF0F4u, k));
    }
    /* the sun and its glow */
    int sx = W * 30 / 100, sy = hz * 80 / 100;
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

    /* the town: ziggurat, torches, king */
    int zw = sw_of(HAM_SPR_ZIGGURAT), zh = sh_of(HAM_SPR_ZIGGURAT);
    int zcx = 5 + zw / 2, zfoot = hz + 10, ztop = zfoot - zh + 1;
    int granaryX = W - 20, riverTop = UH * 68 / 100, fieldTop = UH * 78 / 100;
    stand(HAM_SPR_ZIGGURAT, zcx, zfoot, 0, 1, 0);
    zig_x = zcx - zw / 2; zig_y = zfoot - zh + 1;
    int fl = (int)((tm / 125u) & 1);
    glow(zcx - zw / 2 + 10, ztop + 13, 9 + fl, 0xFFB84D, 89);
    glow(zcx + zw / 2 - 10, ztop + 13, 9 + fl, 0xFFB84D, 89);
    spr(HAM_SPR_FLAME_0 + fl, zcx - zw / 2 + 8, ztop + 17 - sh_of(HAM_SPR_FLAME_0) + 1, 0);
    spr(HAM_SPR_FLAME_0 + 1 - fl, zcx + zw / 2 - 8 - sw_of(HAM_SPR_FLAME_0), ztop + 17 - sh_of(HAM_SPR_FLAME_0) + 1, 0);
    {   /* the king waves now and then, every third spell of 2.2 seconds */
        int waving = (tm / 2200u) % 3 == 0;
        stand(HAM_SPR_KING_0 + (waving ? (int)((tm / 400u) & 1) : 0), zcx - 9, ztop + 9, 0, 0, 0);
    }
    /* houses, two staggered rows, with cooking smoke over every other one in the back */
    {
        int x0 = zcx + zw / 2 + 14, x1 = granaryX - 34;
        int cols = imax(1, (x1 - x0) / 20 + 1), n = imin(imax(100 / 6, 1), cols * 2);
        for (int row = 0; row < 2; row++) for (int i = row; i < n; i += 2) {
            int col = i / 2, id = HAM_SPR_HOUSE_A + (i * 7 + 1) % 3;
            int cx = x0 + col * 20 + row * 9, foot = hz + 9 + row * 12;
            stand(id, cx, foot, 0, 1, 0);
            if (row == 0 && col % 2 == 0) for (int k = 0; k < 3; k++) {
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
    stand(HAM_SPR_GRANARY, granaryX, hz + 11, 0, 1, 0);
    for (int i = 0; i < 4; i++) /* 2,800 grain: four sacks */
        stand(HAM_SPR_SACK, granaryX - 19 - i * 7, hz + 12, 0, 1, 0);
    {   /* a caravan on the near bank, with long gaps between visits */
        u32 span = (u32)(W * 22 / 10 + 90) * 16u, cx16 = (mv(7u * 16u) + 40u * 16u) % span;
        int cx = (int)(cx16 >> 4) - 30, bob = (int)((tm / 500u) & 1);
        stand(HAM_SPR_CAMEL_0 + bob, cx, riverTop - 1, 0, 1, 0);
        stand(HAM_SPR_CAMEL_0 + 1 - bob, cx - 30, riverTop - 1, 0, 1, 0);
    }

    /* the river with its boat, ripples and a jumping fish */
    {
        int top = riverTop, hgt = 7;
        for (int y = top * S; y < (top + hgt) * S && y < chh; y++) {
            int k = y - top * S;
            fillrect(0, y, cw, 1, mix(0xB5D6F2u, 0x8FBAE6u, k * 256 / (hgt * S)));
        }
        arect1(-4, top, W + 8, 1, 0xFFFFFF, 178);
        arect1(-4, top + hgt, W + 8, 1, 0, 26);
        int i = 0;
        for (int x = -(int)(mv(6u * 16u) >> 4) % 12; x < W; x += 12, i++)
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

    /* fields: forty plots at most, 800 acres sown; one wheat sprite each, swaying */
    {
        int fieldRows = imax(1, (UH - 4 - fieldTop) / 13), fieldCols = imax(4, (W - 12) / 11);
        int spread16 = (W - 12) * 16 / fieldCols;
        int all = imin(40, fieldRows * fieldCols), planted = imin(all, (800 + 24) / 25);
        for (int i = 0; i < all; i++) {
            int x16 = 96 + (i % fieldCols) * spread16 + (spread16 - 144) / 2, y = fieldTop + (i / fieldCols) * 13, sown = i < planted;
            arect(x16, (y + 2) * 16, 144, 176, 0, sown ? 15 : 7);
            arect(x16, (y + 12) * 16, 144, 16, 0, sown ? 13 : 5);
            if (!sown) continue;
            for (int f = 0; f < 3; f++) arect(x16 + 16 + f * 48, (y + 3) * 16, 16, 144, 0, 9);
            spr(HAM_SPR_WHEAT_0_0 + (int)(((tm / 500u) + (u32)(i * 37 / 100)) & 1), (x16 + 8) >> 4, y, 0);
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
        stand(HAM_SPR_FLAG_0 + (int)((tm / 333u) & 1), 4, fieldTop + 1, 0, 0, 0);
        for (int b = 0; b < 4; b++) {   /* butterflies */
            int open = ((tm / 111u + (u32)b) & 1) == 0;
            int bx16 = W * (20 + 20 * b) * 16 / 100 + tsin(179, b * 512) * 14 * 16 / 4096;
            int by16 = (fieldTop - 4 + (b & 1) * 8) * 16 + tsin(333, b * 256) * 5 * 16 / 4096;
            arect(bx16, by16, open ? 48 : 16, open ? 16 : 32, b % 2 == 0 ? ACCENT : 0x3B7BC6, 255);
        }
        {   /* an ox works the top row while you decide */
            u32 span = (u32)(W + 40) * 16u, u = mv(8u * 16u) % (span * 2u);
            int right = u < span;
            stand(HAM_SPR_OX_0 + (int)((tm / 333u) & 1), (int)((right ? u : span * 2u - u) >> 4) - 20, fieldTop + 1, !right, 1, 0);
        }
    }

    /* the people: twenty-five villagers on their own lanes, drawn back to front */
    {
        struct folk { int x, y, right, id; } fk[40];
        int laneMin = imax(hz + 24, UH * 52 / 100), laneMax = imax(laneMin + 8, UH * 64 / 100);
        int after = 25, margin = 8, span = W - 2 * margin;
        for (int id = 0; id < after; id++) {
            u32 sp = 80u + 112u * rnd((u32)id, 3) / 100000u;
            u32 u = (rnd((u32)id, 4) * 2u * (u32)span * 16u / 100000u + mv(sp)) % (2u * (u32)span * 16u);
            struct folk f;
            f.right = u < (u32)span * 16u;
            f.x = margin + (int)((f.right ? u : 2u * (u32)span * 16u - u) >> 4);
            f.y = laneMin + (int)(rnd((u32)id, 5) * (u32)(laneMax - laneMin) / 100000u);
            f.id = id;
            int j = id;
            while (j > 0 && fk[j - 1].y > f.y) { fk[j] = fk[j - 1]; j--; }
            fk[j] = f;
        }
        /* now and then two of them settle it the old way */
        u32 round = tm / 9000u, bout_age = tm % 9000u;
        int brawler = bout_age < 2600 ? (int)(rnd(round, 70) * (u32)after / 100000u) : -1;
        for (int n = 0; n < after; n++) {
            struct folk *f = &fk[n];
            if (f->id == brawler) {
                stand(HAM_SPR_SCUFFLE_0 + (int)((tm / 111u) & 1), f->x, f->y + 1, 0, 1, 0);
                for (int k = 0; k < 3; k++) {
                    u32 q = tm / 166u + (u32)k;
                    arect1(f->x - 9 + (int)(rnd(q, 71) * 18u / 100000u), f->y - 14 + (int)(rnd(q, 72) * 8u / 100000u), 1, 1, 0xE8B02E, 255);
                }
            } else {
                int step = (int)((tm / 166u + (u32)f->id) & 3);
                stand(ham_villager[f->id % HAM_LOOKS][step], f->x, f->y, !f->right, 1, step & 1);
            }
        }
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
static void big_text(const char *s, int cx, int top, int f, u32 rgb) {
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
            if (cov) blendrect(ox0 + ox, top + oy, 1, 1, rgb, cov);
        }
    }
}

struct button { int x, y, w, h; const char *label; int primary; };
static struct button btn[3] JT_DATA = {{0}};
static int panel_y JT_DATA = 0, heading_bottom JT_DATA = 0;

static const char TITLE[] = "Hamurapi"; /* the big title; the credit line below keeps the 1968 game's own name */
static const char *const LABELS[3] = { "Start a new game", "Classic 1968", "Watch a demo" };

static void layout(void) {
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

static void ui(void) {
    int cw = (int)cv.width, chh = (int)cv.height;
    /* heading panel */
    int f = cw >= 900 ? 4 : cw >= 560 ? 3 : 2;
    if (chh < 300 && f > 2) f = 2;
    const char *sub = "Be king for 10 years. Feed your people. Keep your crown.";
    if (jt_text_width(JT_FACE_BODY, sub) > cw - 80) sub = "Feed your people. Keep your crown.";
    int tw = jt_text_width(JT_FACE_BOLD, TITLE) * f, sw = jt_text_width(JT_FACE_BODY, sub);
    int hw = imax(tw, sw) + 56, hh = 10 + 16 * f + 2 + 16 + 12, hx = (cw - hw) / 2, hy = 10;
    glass(hx, hy, hw, hh, 22);
    big_text(TITLE, cw / 2, hy + 10 - 2 * f / 2, f, INK);
    text_c(JT_FACE_BODY, cw / 2, hy + 10 + 16 * f - 2, MUTED, sub);
    heading_bottom = hy + hh;

    /* button panel */
    int bw = btn[0].w + 28, bx = btn[0].x - 14, by = panel_y, bh = 14 + 44 + 8 + 36 + 8 + 16 + 12;
    glass(bx, by, bw, bh, 22);
    for (int i = 0; i < 3; i++) {
        struct button *b = &btn[i];
        int down = pressed == i, dy = down ? 1 : 0;
        u32 fill = b->primary ? (down ? PRESSED_ACCENT : ACCENT) : (down ? 0xD9DADFu : 0xFFFFFFu);
        if (!b->primary) rrect(b->x, b->y + dy, b->w, b->h, 12, 0xDDDEE3u, 255); /* a 1-pixel border so a white button reads on the white panel */
        if (b->primary) rrect(b->x, b->y + dy, b->w, b->h, 12, fill, 255);
        else rrect(b->x + 1, b->y + dy + 1, b->w - 2, b->h - 2, 11, fill, 255);
        if (focus == i) { /* focus ring: two pixels of accent just outside the button */
            for (int k = 0; k < 2; k++) {
                u32 ring = ACCENT;
                blendrect(b->x - 2 + k + 8, b->y - 2 + k, b->w + 4 - 2 * k - 16, 1, ring, 255);
                blendrect(b->x - 2 + k + 8, b->y + b->h + 1 - k, b->w + 4 - 2 * k - 16, 1, ring, 255);
                blendrect(b->x - 2 + k, b->y - 2 + k + 8, 1, b->h + 4 - 2 * k - 16, ring, 255);
                blendrect(b->x + b->w + 1 - k, b->y - 2 + k + 8, 1, b->h + 4 - 2 * k - 16, ring, 255);
            }
        }
        text_c(JT_FACE_BOLD, b->x + b->w / 2, b->y + dy + (b->h - 16) / 2, b->primary ? 0xFFFFFFu : INK, b->label);
    }
    static const char *const CAP[3] = {
        "Based on Hamurabi by Doug Dyment, 1968. Copyright 2026 Joshua Trommel. MIT License.",
        "Based on Hamurabi by Doug Dyment, 1968. Copyright 2026 Joshua Trommel.",
        "Based on Hamurabi by Doug Dyment, 1968." };
    for (int i = 0; i < 3; i++) if (i == 2 || jt_text_width(JT_FACE_BODY, CAP[i]) <= bw - 28) {
        text_c(JT_FACE_BODY, cw / 2, by + 14 + 44 + 8 + 36 + 8 - 1, MUTED, CAP[i]);
        break;
    }
}

/* One frame: paint it all into the back buffer, then copy it over the window in one quick pass. */
static void draw(void) {
    cv.width = win.width; cv.height = win.height; cv.pitch = win.width;
    layout();
    scene();
    ui();
    u32 n = win.width * win.height;
    for (u32 i = 0; i < n; i++) win.pixels[i] = cv.pixels[i];
}

/* ---------- input ---------- */
static void say(const char *a, const char *b, const char *c) {
    char line[96]; int n = 0;
    for (const char *s = a; *s && n < 90; s++) line[n++] = *s;
    for (const char *s = b; *s && n < 90; s++) line[n++] = *s;
    for (const char *s = c; *s && n < 94; s++) line[n++] = *s;
    line[n++] = '\n';
    jt_write(1, line, (unsigned)n);
}

static u32 now_ms(void) { struct jt_tasks t; jt_tasks(&t, -1); return t.ticks * 10u; }

static void choose(int i, u32 now) {
    pressed = i; pressed_until = now + 180;
    say("hamurabi: ", LABELS[i], " chosen");
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
    {   /* the sheet, the title's scratch and the back buffer live on the SYS_BRK heap */
        unsigned base = (unsigned)jt_brk(0), need = (SHEET_BYTES + MASK_BYTES + BB_BYTES + 4095u) & ~4095u;
        if (win.width * win.height * 4u > BB_BYTES) { jt_write(2, "hamurabi: window too big\n", 25); jt_exit(1); }
        unsigned top = (unsigned)jt_brk(base + need); /* the heap lives at 0xFF000000: "negative" as an int, so errors are -4095..-1 only */
        if (top >= 0xFFFFF000u || top < base + need) { jt_write(2, "hamurabi: no heap\n", 18); jt_exit(1); }
        sheet = (u8 *)base;
        mask = (u32 *)(base + SHEET_BYTES);
        cv.pixels = (u32 *)(base + SHEET_BYTES + MASK_BYTES);
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
    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        if (r == 1 && jt_window_resized(&ev, &win)) { draw(); flags = JT_POLL_PRESENT; continue; } /* JT_EV_RESIZE: remapped, repaint at the new size */
        flags = 0;
        u32 now = now_ms();
        tm = now - start;
        if (r == 1) {
            if (ev.kind == JT_EV_KEY) {
                int k = ev.a;
                if (k == JT_KEY_ESC) break;
                if (k == JT_KEY_RIGHT || k == JT_KEY_DOWN || k == '\t') focus = focus < 0 ? 0 : imin(focus + 1, 2);
                else if (k == JT_KEY_LEFT || k == JT_KEY_UP) focus = focus < 0 ? 0 : imax(focus - 1, 0);
                else if (k == JT_KEY_ENTER || k == ' ') choose(focus < 0 ? 0 : focus, now);
            } else if (ev.kind == JT_EV_CLICK) {
                layout();
                for (int i = 0; i < 3; i++)
                    if (ev.a >= btn[i].x && ev.a < btn[i].x + btn[i].w && ev.b >= btn[i].y && ev.b < btn[i].y + btn[i].h) { choose(i, now); break; }
            }
            draw(); flags = JT_POLL_PRESENT; next = now + 80;
            continue;
        }
        if (r != -11 /* -EAGAIN */) break;
        if (pressed >= 0 && now >= pressed_until) { pressed = -1; next = 0; }
        if (now >= next) { draw(); flags = JT_POLL_PRESENT; next = now + 80; }
        else jt_sched_yield();
    }
    jt_write(1, "hamurabi: closed\n", 17);
    jt_exit(0);
}
