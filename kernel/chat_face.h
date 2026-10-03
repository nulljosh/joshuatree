/* Samantha's face in the Chat window.

   The first time Chat opens, this fetches her frames over plain HTTP
   (idle-0..23.jpg and talk-0..35.jpg under /face/, 360x360, two short
   loops cut from her character videos at 12fps by
   tools/gen/face_frames.py) and keeps them decoded in RAM. Like the Mac
   face window, she is a video, not a still: the idle loop plays whenever
   Chat is waiting (breathing, blinking; idle-0 must be an open-eyed frame
   so Chat never opens on shut eyes, tools/gen/face_frames.py starts the loop
   on her open stretch), and while her voice plays the
   talk loop advances only while there is sound (speak_level), holding
   on pauses. The empty Chat shows a small face at the top right; once a
   conversation starts she fills the window above her latest reply.

   Everything is optional. A missing host, a 404, a bad PNG or a frame of
   the wrong size just means fewer frames; no idle-0 means no face at all,
   and Chat looks and works exactly as it did before. One fetch per boot,
   fixed caps, and a failed first frame stops the rest so a dead network
   costs one timeout, not twelve.

   Off unless facehost=HOST[:PORT] is on the kernel command line
   (facehost=joshuatree.heyitsmejosh.com for her real frames;
   tools/checks/chat-face-check.py serves solid colors). */
#include "sb16.h"
#include "speak.h"
#include "jpeg.h"

#define FACE_IDLE_MAX  24
#define FACE_TALK_MAX  60
#define FACE_SRC       320            /* source frame side, pixels */
#define FACE_SIDE      60             /* small face, logical pixels, empty Chat */
#define FACE_BIG_MAX   300            /* big face, logical pixels */
#define FACE_FILE_MAX  (64u * 1024u)  /* one JPEG; real frames are ~25KB */
#define FACE_TIMEOUT   300            /* ~3s per frame */
#define FACE_STEP      10             /* ticks per talk frame, ~10 per second */
#define FACE_HOST_MAX  64

static char face_host[FACE_HOST_MAX] = "joshuatree.heyitsmejosh.com";
static unsigned short face_port = 80;
static unsigned char *face_idle[FACE_IDLE_MAX], *face_talk[FACE_TALK_MAX]; /* RGB, FACE_SRC^2 * 3 */
static int face_idle_n = 0, face_talk_n = 0, face_tried = 0;
static int face_host_set = 0;        /* 1 once facehost= was on the command line */
static int face_x = -1, face_y = -1;  /* viewport-local logical top-left, -1 = not placed */
static int face_side = FACE_SIDE;     /* logical side of the face on screen now */
static int face_shown = -1;           /* talk frame on screen during playback */
static int face_idle_at = 0;          /* idle loop position */
static int face_talk_at = 0;          /* talk loop position */
/* A 16x16 grey fingerprint of every frame, so a switch between the idle and
   talk clips can land on the frame that looks most like the current one
   instead of cutting to a different head position. Heap-allocated (not
   .bss) because FACE_IDLE_MAX + FACE_TALK_MAX frames at 256 bytes each is
   21KB that only exists once facehost= is on the command line and a face
   is actually loading; carrying it in .bss year-round crowds the ring-3
   .userimg window right next to it (see boot/linker.ld). Null until
   chat_face_load allocates it. */
static unsigned char (*face_sig_idle)[256] = 0, (*face_sig_talk)[256] = 0;
static unsigned int face_open[FACE_TALK_MAX];   /* how open the mouth is in each talk frame */
static unsigned int face_open_lo = 0, face_open_hi = 1;

/* Mouth contrast in a box at a character-creator portrait's usual mouth
   spot (centred, 58-74% down): teeth by a dark gap is open, flat lips shut.
   Same measure for any character, no per-face tuning. */
static unsigned int face_openness(const unsigned char *px) {
    int x0 = FACE_SRC * 40 / 100, x1 = FACE_SRC * 60 / 100, y0 = FACE_SRC * 58 / 100, y1 = FACE_SRC * 74 / 100;
    unsigned int n = 0, sum = 0, sq = 0;
    for (int y = y0; y < y1; y += 2)
        for (int x = x0; x < x1; x += 2) {
            const unsigned char *p = px + (y * FACE_SRC + x) * 3;
            unsigned int g = (p[0] * 3 + p[1] * 4 + p[2]) >> 3;
            sum += g; sq += g * g; n++;
        }
    unsigned int m = sum / n;
    return sq / n - m * m;                    /* variance */
}

static void face_sig(const unsigned char *px, unsigned char *sig) {
    for (int gy = 0; gy < 16; gy++)
        for (int gx = 0; gx < 16; gx++) {
            const unsigned char *p = px + ((gy * FACE_SRC / 16 + FACE_SRC / 32) * FACE_SRC + gx * FACE_SRC / 16 + FACE_SRC / 32) * 3;
            sig[gy * 16 + gx] = (unsigned char)((p[0] * 3 + p[1] * 4 + p[2]) >> 3);
        }
}

/* Index in sigs[0..n) closest to sig. */
static int face_nearest(unsigned char (*sigs)[256], int n, const unsigned char *sig) {
    int best = 0; unsigned int best_d = 0xFFFFFFFFu;
    for (int i = 0; i < n; i++) {
        unsigned int d = 0;
        for (int k = 0; k < 256; k++) { int v = (int)sigs[i][k] - (int)sig[k]; d += (unsigned int)(v < 0 ? -v : v); }
        if (d < best_d) { best_d = d; best = i; }
    }
    return best;
}

/* kmain hands over the boot command line; only facehost= is read. */
static void chat_face_cmdline(const char *cl) {
    for (const char *p = cl; p && *p; p++) {
        if (p[0]=='f' && p[1]=='a' && p[2]=='c' && p[3]=='e' && p[4]=='h' && p[5]=='o' && p[6]=='s' && p[7]=='t' && p[8]=='=') {
            p += 9; int n = 0;
            while (*p && *p != ' ' && *p != ':' && n < FACE_HOST_MAX - 1) face_host[n++] = *p++;
            face_host[n] = 0;
            face_host_set = n > 0;
            if (*p == ':') {
                unsigned int pt = 0; p++;
                while (*p >= '0' && *p <= '9') pt = pt * 10 + (unsigned int)(*p++ - '0');
                if (pt && pt < 65536) face_port = (unsigned short)pt;
            }
            return;
        }
    }
}

/* One frame: 200, a baseline JPEG, exactly FACE_SRC square. Returns an RGB buffer
   or 0. */
static unsigned char *face_fetch(unsigned char *file, const char *kind, int i) {
    char path[32]; int n = 0;
    /* Portfolio mode (kernel.c's portfolio_dock, "portfolio" on the command
       line) is Joshua's own site, so the face is his: landing/face-joshua/,
       cut by tools/gen/face_frames.py with FACE_OUT. Everywhere else it is
       Samantha's landing/face/. */
    const char *s = portfolio_dock ? "/face-joshua/" : "/face/"; while (*s) path[n++] = *s++;
    while (*kind) path[n++] = *kind++;
    path[n++] = '-'; if (i >= 10) path[n++] = (char)('0' + i / 10); path[n++] = (char)('0' + i % 10);
    s = ".jpg"; while (*s) path[n++] = *s++;
    path[n] = 0;
    int got = http_get_timeout(face_host, path, face_port, file, FACE_FILE_MAX, FACE_TIMEOUT);
    if (got <= 0 || http_last_status() != 200) return 0;
    unsigned char *px = 0; unsigned int w = 0, h = 0, ch = 0;
    if (jpeg_decode(file, (unsigned int)got, &px, &w, &h, &ch) != 0 || !px) return 0;
    if (w != FACE_SRC || h != FACE_SRC || ch != 3) { kfree(px); return 0; }
    return px;
}

static void chat_face_load(void) {
    if (face_tried) return;
    face_tried = 1;
    /* Opt-in: without facehost= Chat never touches the network for her face.
       A default fetch blocked Chat's first paint for seconds on every boot
       with a NIC, which broke every check that times how fast Chat opens. */
    if (!face_host_set) return;
    window_present(); /* show the empty window while the frames load */
    /* Every other HTTP caller in this kernel (chat_send/chat_pick above,
       stocks.h, curbfind.h, epiphany.h) gates on net_init() before ever
       calling http_get_timeout, because net.c's send/receive helpers
       (arp_resolve, dns_resolve, tcp_get_timeout) call straight through
       active_send/active_receive with no null check of their own -- they
       trust the caller already confirmed a NIC exists. This function used
       to skip that gate and call http_get_timeout directly. With no NIC
       initialized yet (the common case: Chat is often the first app in a
       boot to touch the network, and headless CI's app-gallery check boots
       with no -net device at all), active_send/active_receive are still
       null function pointers, and dns_resolve's first send is a call
       through address 0 -- a ring-0 exception that halts the whole kernel,
       not just Chat. Confirmed locally: booting with no -net device and
       opening Chat crashed with "exception: ring-0 invalid-opcode, halted"
       right after "chatchrome", with this gate removed. No NIC now just
       means no face, exactly like every sibling feature. */
    unsigned char *file = net_init(0x0A00020F) ? kmalloc(FACE_FILE_MAX) : 0;
    if (file) {
        /* Both sig tables or neither: face_nearest/chat_face_tick assume
           face_idle_n/face_talk_n > 0 implies a live sig table, so a
           partial allocation must not leave either counter set. */
        face_sig_idle = (unsigned char (*)[256])kmalloc(FACE_IDLE_MAX * 256u);
        face_sig_talk = (unsigned char (*)[256])kmalloc(FACE_TALK_MAX * 256u);
        if (face_sig_idle && face_sig_talk) {
            while (face_idle_n < FACE_IDLE_MAX && (face_idle[face_idle_n] = face_fetch(file, "idle", face_idle_n))) { face_sig(face_idle[face_idle_n], face_sig_idle[face_idle_n]); face_idle_n++; }
            if (face_idle_n)
                while (face_talk_n < FACE_TALK_MAX && (face_talk[face_talk_n] = face_fetch(file, "talk", face_talk_n))) { face_sig(face_talk[face_talk_n], face_sig_talk[face_talk_n]); face_open[face_talk_n] = face_openness(face_talk[face_talk_n]); face_talk_n++; }
            if (face_talk_n) {
                face_open_lo = face_open_hi = face_open[0];
                for (int i = 1; i < face_talk_n; i++) { if (face_open[i] < face_open_lo) face_open_lo = face_open[i]; if (face_open[i] > face_open_hi) face_open_hi = face_open[i]; }
                if (face_open_hi == face_open_lo) face_open_hi++;
            }
        } else {
            /* No memory for the sig tables: same as a dead network or a
               404 on frame 0, no face this boot. */
            if (face_sig_idle) { kfree(face_sig_idle); face_sig_idle = 0; }
            if (face_sig_talk) { kfree(face_sig_talk); face_sig_talk = 0; }
        }
        kfree(file);
    }
    /* Always say how it went once facehost= asked for a face, even when the
       card or memory wasn't there: silence reads as a hang to anyone watching. */
    serial_puts("face: idle="); { char d[3] = { (char)('0' + face_idle_n / 10), (char)('0' + face_idle_n % 10), 0 }; serial_puts(face_idle_n >= 10 ? d : d + 1); }
    serial_puts(" talk="); { char d[3] = { (char)('0' + face_talk_n / 10), (char)('0' + face_talk_n % 10), 0 }; serial_puts(face_talk_n >= 10 ? d : d + 1); }
    serial_puts("\n");
}

/* Logical pixels the empty Chat's text column gives up on the right. */
static int chat_face_reserve(void) { return face_idle_n ? FACE_SIDE + 16 : 0; }

/* Portfolio mode: the face is the whole screen. chat_boot_samantha_open sets
   face_full and face_band_h; every frame the kernel plays then lands here
   instead of in a rounded square. The 320 px frame is scaled to the screen's
   height (capped, so a retina screen does not pay for a million pixels a
   frame), top-aligned under the titlebar and centered. Its background is a
   plain wall, so each side is one flat wall color (the average of that edge
   over the frame's upper half, above his shoulders) and the frame's outer
   columns feather into it. Each row's own edge pixel smeared JPEG noise
   into stripes, a seam and a dark bar where the sweater reaches the edge.
   On a narrow phone the sides are cropped instead, and the 720 cap is
   skipped there (only fw columns are drawn, so the cost is already bounded)
   or the face stopped short of the screen's bottom. Rows stop above the
   glass bar, which is drawn once (face_glass_band) and never repainted. */
static int face_full = 0, face_band_h = 0, face_full_top = 0;
#define FACE_WALL_COLS 4    /* source columns averaged for each side's wall color */
#define FACE_FEATHER   24   /* source columns at each edge that fade into the wall */
/* Average color of FACE_WALL_COLS source columns from c0, over the upper half (all wall). */
static unsigned int face_wall(const unsigned char *px, int c0) {
    unsigned int r = 0, g = 0, b = 0, n = 0;
    for (int y = 0; y < FACE_SRC / 2; y++) for (int x = c0; x < c0 + FACE_WALL_COLS; x++) {
        unsigned int o = ((unsigned int)y * FACE_SRC + (unsigned int)x) * 3u;
        r += px[o]; g += px[o + 1]; b += px[o + 2]; n++;
    }
    return ((r / n) << 16) | ((g / n) << 8) | (b / n);
}
/* a toward b by w/256. */
static unsigned int face_lerp(unsigned int a, unsigned int b, unsigned int w) {
    unsigned int R = (((a >> 16) & 255) * (256 - w) + ((b >> 16) & 255) * w) >> 8;
    unsigned int G = (((a >> 8) & 255) * (256 - w) + ((b >> 8) & 255) * w) >> 8;
    unsigned int B = ((a & 255) * (256 - w) + (b & 255) * w) >> 8;
    return (R << 16) | (G << 8) | B;
}
static void face_blit_full(const unsigned char *px, const unsigned char *mix) {
    if (!px) return;
    int sc = (int)window_scale(); if (sc < 1) sc = 1;
    int fw = (int)window_width() * sc, fh = (int)window_height() * sc;
    int top = face_full_top * sc, limit = fh - face_band_h * sc;
    int side = limit - top; if (side > 720 * sc && side < fw) side = 720 * sc;   /* fit above the glass bar, so the bar never hides his neck */
    int ox = (fw - side) / 2;
    int x0 = ox < 0 ? 0 : ox, x1 = ox + side > fw ? fw : ox + side;
    if (x1 > 4096) x1 = 4096;
    int rows = limit - top; if (rows > side) rows = side;
    /* Per-column source index, rebuilt only when the geometry changes: the
       division per pixel was most of the cost of a frame. */
    static unsigned short colmap[4096];
    static unsigned short fwt[256];          /* feather weight out of 256, 0 = outer edge column */
    static unsigned int wall_l = 0, wall_r = 0;
    static int map_side = -1, map_ox = 0, map_fw = 0, fpx = 0;
    if (map_side != side || map_ox != ox || map_fw != fw) {
        for (int x = x0; x < x1; x++) colmap[x] = (unsigned short)((x - ox) * FACE_SRC / side);
        wall_l = face_wall(px, 4); wall_r = face_wall(px, FACE_SRC - 4 - FACE_WALL_COLS);   /* 4 in: JPEG's outermost columns ring */
        fpx = ox > 0 ? side * FACE_FEATHER / FACE_SRC : 0; if (fpx > 256) fpx = 256;
        for (int i = 0; i < fpx; i++) fwt[i] = (unsigned short)(256 * (fpx - i) / fpx);
        /* The wall beside and below the frame is painted once: it is a plain
           wall, it does not move. */
        for (int y = top; y < fh; y++) {   /* down to the screen bottom: the glass bar blurs these rows */
            if (y - top >= side) { window_fill_rect_phys(0, y, fw, 1, wall_l); continue; }   /* below the face: the wall, so the glass bar reads light, not a gray slab */
            if (x0 > 0) window_fill_rect_phys(0, y, x0, 1, wall_l);
            if (x1 < fw) window_fill_rect_phys(x1, y, fw - x1, 1, wall_r);
        }
        map_side = side; map_ox = ox; map_fw = fw;
    }
    /* The face itself: one row pointer per row, 32-bit stores, damage
       declared once. Through window_pixel_phys this was ~4 frames a second
       in v86 (live recording, 2026-10-02); the clip wants 12. */
    for (int y = top; y < top + rows; y++) {
        unsigned int ro = (unsigned int)((y - top) * FACE_SRC / side) * FACE_SRC * 3;
        unsigned int *row = window_phys_row(y);
        if (row) {
            if (mix) for (int x = x0; x < x1; x++) { unsigned int o = ro + colmap[x] * 3u; row[x] = (((px[o] + mix[o]) >> 1) << 16) | (((px[o + 1] + mix[o + 1]) >> 1) << 8) | ((px[o + 2] + mix[o + 2]) >> 1); }
            else for (int x = x0; x < x1; x++) { unsigned int o = ro + colmap[x] * 3u; row[x] = (px[o] << 16) | (px[o + 1] << 8) | px[o + 2]; }
            for (int i = 0; i < fpx; i++) { row[x0 + i] = face_lerp(row[x0 + i], wall_l, fwt[i]); row[x1 - 1 - i] = face_lerp(row[x1 - 1 - i], wall_r, fwt[i]); }
        } else {
            for (int x = x0; x < x1; x++) { unsigned int o = ro + colmap[x] * 3u; window_pixel_phys(x, y, (px[o] << 16) | (px[o + 1] << 8) | px[o + 2]); }
        }
    }
    window_damage(x0, top, x1 - x0, rows);
}

/* The glass bar: the bottom face_band_h logical rows, full width, of whatever
   is on screen right now, box-blurred and lightened, with a bright hairline
   on top. Called once after the first full frame; the pixels are kept in
   face_glass so the input line can repaint its own rows without redoing the
   blur. Returns 0 if memory is short (the bar then stays a plain light
   panel). */
static unsigned int *face_glass = 0;
static int face_glass_w = 0, face_glass_h = 0, face_glass_y = 0;
static void face_glass_band(void) {
    int sc = (int)window_scale(); if (sc < 1) sc = 1;
    int fw = (int)window_width() * sc, gh = face_band_h * sc, y0 = (int)window_height() * sc - gh;
    if (!face_glass || face_glass_w != fw || face_glass_h != gh) {
        if (face_glass) kfree(face_glass);
        face_glass = (unsigned int *)kmalloc((unsigned int)(fw * gh) * 4u);
        face_glass_w = fw; face_glass_h = gh;
    }
    face_glass_y = y0;
    unsigned int *g = face_glass;
    if (!g) { window_fill_rect_phys(0, y0, fw, gh, 0x00F2EEE8); return; }
    for (int y = 0; y < gh; y++) for (int x = 0; x < fw; x++) g[y * fw + x] = window_get_pixel_phys(x, y0 + y);
    int r = 7 * sc;
    static unsigned int *tmp = 0; static int tmp_n = 0;
    if (!tmp || tmp_n < fw * gh) { if (tmp) kfree(tmp); tmp = (unsigned int *)kmalloc((unsigned int)(fw * gh) * 4u); tmp_n = fw * gh; }
    if (tmp) {
        for (int pass = 0; pass < 2; pass++) {   /* horizontal, then vertical, running sums */
            int len = pass ? gh : fw, lines = pass ? fw : gh;
            for (int l = 0; l < lines; l++) {
                int sr = 0, sg = 0, sb = 0, cnt = 0;
                #define GP(i) (pass ? g[(i) * fw + l] : g[l * fw + (i)])
                for (int i = 0; i < len + r; i++) {
                    if (i < len) { unsigned int c = GP(i); sr += (c >> 16) & 255; sg += (c >> 8) & 255; sb += c & 255; cnt++; }
                    int o = i - r - 1;
                    if (o >= 0) { unsigned int c = GP(o); sr -= (c >> 16) & 255; sg -= (c >> 8) & 255; sb -= c & 255; cnt--; }
                    int d = i - r;
                    if (d >= 0 && d < len && cnt > 0) {
                        unsigned int v = ((unsigned int)(sr / cnt) << 16) | ((unsigned int)(sg / cnt) << 8) | (unsigned int)(sb / cnt);
                        if (pass) tmp[d * fw + l] = v; else tmp[l * fw + d] = v;
                    }
                }
                #undef GP
            }
            for (int i = 0; i < fw * gh; i++) g[i] = tmp[i];
        }
    }
    for (int y = 0; y < gh; y++) for (int x = 0; x < fw; x++) {   /* lighten toward white, hairline on top */
        unsigned int c = g[y * fw + x];
        unsigned int k = y < sc ? 200 : 96;                       /* white mix out of 256 */
        unsigned int R = (((c >> 16) & 255) * (256 - k) + 255 * k) >> 8, G = (((c >> 8) & 255) * (256 - k) + 255 * k) >> 8, B = ((c & 255) * (256 - k) + 255 * k) >> 8;
        g[y * fw + x] = (R << 16) | (G << 8) | B;
        window_pixel_phys(x, y0 + y, g[y * fw + x]);
    }
}
/* Repaint a logical rectangle inside the bar from the saved glass (the input line clearing itself). */
static void face_glass_restore(int x, int y, int w, int gh) {
    if (!face_glass) return;
    int sc = (int)window_scale(); if (sc < 1) sc = 1;
    for (int py = y * sc; py < (y + gh) * sc; py++) for (int px = x * sc; px < (x + w) * sc; px++) {
        int gy = py - face_glass_y; if (gy < 0 || gy >= face_glass_h || px >= face_glass_w) continue;
        window_pixel_phys(px, py, face_glass[gy * face_glass_w + px]);
    }
}

/* Draws one frame at face_x/face_y, face_side logical pixels, at full
   physical resolution with rounded corners. */
/* Draws a frame, or a 50/50 blend of two (mix = 0 for none), at
   face_x/face_y, face_side logical pixels, full physical resolution,
   rounded corners. The blend is the in-between frame that doubles her
   motion to about 24 a second and turns clip switches into crossfades. */
static void face_blit_mix(const unsigned char *px, const unsigned char *mix) {
    if (face_full) { face_blit_full(px, mix); return; }
    if (!px || face_x < 0) return;
    int sc = (int)window_scale(); if (sc < 1) sc = 1;
    int side = face_side * sc, r = (face_side > FACE_SIDE ? 14 : 6) * sc;
    int ox = face_x * sc, oy = face_y * sc;
    for (int y = 0; y < side; y++) {
        unsigned int ro = (unsigned int)(y * FACE_SRC / side) * FACE_SRC * 3;
        int cy = y < r ? r - y : (y >= side - r ? y - (side - r - 1) : 0);
        for (int x = 0; x < side; x++) {
            int cx = x < r ? r - x : (x >= side - r ? x - (side - r - 1) : 0);
            if (cx && cy && cx * cx + cy * cy > r * r) continue;
            unsigned int o = ro + (unsigned int)(x * FACE_SRC / side) * 3;
            unsigned int R = px[o], G = px[o + 1], B = px[o + 2];
            if (mix) { R = (R + mix[o]) >> 1; G = (G + mix[o + 1]) >> 1; B = (B + mix[o + 2]) >> 1; }
            window_pixel_phys(ox + x, oy + y, (R << 16) | (G << 8) | B);
        }
    }
}

static void face_blit(const unsigned char *px) { face_blit_mix(px, 0); }

/* Small face at the top right of the empty Chat. */
static void chat_face_draw(int T) {
    if (!face_idle_n) return;
    face_side = FACE_SIDE;
    face_x = (int)window_width() - 20 - FACE_SIDE;
    face_y = T + 44;
    face_blit(face_idle[face_idle_at]);
}

/* Big face centered in [top, bottom); returns the y just below it, or top
   when there is no face. */
static int chat_face_draw_big(int top, int bottom) {
    if (!face_idle_n) return top;
    int side = bottom - top;
    if (side > FACE_BIG_MAX) side = FACE_BIG_MAX;
    if (side < FACE_SIDE) return top;
    face_side = side;
    face_x = ((int)window_width() - side) / 2;
    face_y = top;
    face_blit(face_shown >= 0 && face_shown < face_talk_n ? face_talk[face_shown] : face_idle[face_idle_at]);
    return top + side;
}

/* Idle loop position: forward, wrapping at a cut where the last frame
   matches the first (tools/gen/face_frames.py). */
static void chat_face_idle_step(void) {
    if (face_idle_n) face_idle_at = (face_idle_at + 1) % face_idle_n;   /* forward; the seamless cut and the blend hide the wrap */
}

/* sb16_play's progress hook, 12 frames a second (her clip's own rate).
   Every step moves one or two frames along the real talk clip, always in
   the current direction, turning around only at the clip's ends, so each
   change is genuine motion and there is never a loop seam. Whether it
   steps one or two is the frame whose mouth (face_open) better matches how
   loud she is right now, so the clip leans into her voice. A real pause (0.6s quiet)
   relaxes into the idle loop at the idle frame nearest her pose. */
static void chat_face_tick(unsigned int elapsed) {
    static unsigned int next_at = 0, sound_at = 0, full_at = 0;
    static const unsigned char *cur = 0, *shown_px = 0, *pending_px = 0;
    static int dir = 1;
    if (pending_px && elapsed >= full_at) {          /* second half of a step: the real frame */
        face_blit(pending_px); shown_px = pending_px; pending_px = 0;
        window_present();
    }
    if (face_shown != -1 && elapsed < next_at) return;
    if (face_shown == -1) { sound_at = elapsed; cur = face_sig_idle[face_idle_at]; }
    next_at = elapsed + 8;
    int level = (int)speak_level(elapsed);
    if (level >= 4) sound_at = elapsed;
    if (elapsed - sound_at > 60) {
        if (face_shown >= 0) face_idle_at = face_nearest(face_sig_idle, face_idle_n, cur);
        else chat_face_idle_step();
        face_shown = -2;
        cur = face_sig_idle[face_idle_at];
        pending_px = face_idle[face_idle_at];
    } else {
        int n = face_talk_n, pick;
        if (face_shown < 0) pick = face_nearest(face_sig_talk, n, cur);   /* coming from idle: nearest pose */
        else {
            /* target openness on a square-root curve of loudness, so soft syllables still part her lips */
            unsigned int lv = (unsigned int)(level > 40 ? 40 : level), t = 0, x = lv * 1000 / 40;
            while ((t + 1) * (t + 1) <= x * 1000) t++;
            unsigned int target = face_open_lo + (face_open_hi - face_open_lo) * t / 1000;
            /* Always keep moving the same way: one frame or two, whichever
               mouth fits the sound better. Turning around only at the clip's
               ends keeps it seamless; letting the voice pick the direction
               made her rock between two frames and look frozen. */
            /* Forward only: the clip wraps (face_frames.py cuts it where the
               last frame matches the first, and the in-between blend hides
               the rest). Playing it backwards read as a head shake. */
            (void)dir;
            int a = (face_shown + 1) % n, b = (face_shown + 2) % n;
            pick = a;
            {
                unsigned int da = face_open[a] > target ? face_open[a] - target : target - face_open[a];
                unsigned int db = face_open[b] > target ? face_open[b] - target : target - face_open[b];
                if (db < da) pick = b;
            }
        }
        face_shown = face_talk_at = pick;
        cur = face_sig_talk[pick];
        pending_px = face_talk[pick];
    }
    /* first half of the step: the in-between blend of what's on screen and what's next */
    if (shown_px && shown_px != pending_px) face_blit_mix(pending_px, shown_px);
    else { face_blit(pending_px); shown_px = pending_px; pending_px = 0; }
    full_at = elapsed + 4;
    window_present();
}

/* Chat's idle wait: the next frame of her idle loop. Call about 12 times
   a second while nothing else is happening. */
static void chat_face_idle_tick(void) {
    if (!face_idle_n || (face_x < 0 && !face_full)) return;
    chat_face_idle_step();
    face_blit(face_idle[face_idle_at]);
    window_present();
}

/* Same loop at twice the rate: call every ~40 ms. Odd calls show the
   in-between blend of this frame and the next, even calls the real next
   frame, so a blink eases shut and open (~24 a second) instead of
   snapping through three 12fps frames. */
static void chat_face_idle_half(void) {
    static int half = 0;
    if (!face_idle_n || (!face_full && face_x < 0)) return;   /* full-screen portfolio never sets face_x */
    if ((half ^= 1)) face_blit_mix(face_idle[(face_idle_at + 1) % face_idle_n], face_idle[face_idle_at]);
    else { chat_face_idle_step(); face_blit(face_idle[face_idle_at]); }
    window_present();
}

/* speak_text with her mouth following the audio. */
static void chat_face_speak(const char *host, unsigned short port, const char *text, unsigned int timeout) {
    int talk = face_talk_n && (face_x >= 0 || face_full);
    face_shown = -1;
    if (talk) sb16_set_progress(chat_face_tick);
    speak_text(host, port, text, timeout);
    sb16_set_progress(0);
    face_shown = -1;
    if (talk) { face_blit(face_idle[face_idle_at]); window_present(); }
}
