/* Samantha's face in the Chat window.

   The first time Chat opens, this fetches her frames over plain HTTP
   (idle-0..23.jpg and talk-0..35.jpg under /face/, 360x360, two short
   loops cut from her character videos at 12fps by
   tools/gen/face_frames.py) and keeps them decoded in RAM. Like the Mac
   face window, she is a video, not a still: the idle loop plays whenever
   Chat is waiting (breathing, blinking), and while her voice plays the
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
   instead of cutting to a different head position. */
static unsigned char face_sig_idle[FACE_IDLE_MAX][256], face_sig_talk[FACE_TALK_MAX][256];
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
    char path[24]; int n = 0;
    const char *s = "/face/"; while (*s) path[n++] = *s++;
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
        while (face_idle_n < FACE_IDLE_MAX && (face_idle[face_idle_n] = face_fetch(file, "idle", face_idle_n))) { face_sig(face_idle[face_idle_n], face_sig_idle[face_idle_n]); face_idle_n++; }
        if (face_idle_n)
            while (face_talk_n < FACE_TALK_MAX && (face_talk[face_talk_n] = face_fetch(file, "talk", face_talk_n))) { face_sig(face_talk[face_talk_n], face_sig_talk[face_talk_n]); face_open[face_talk_n] = face_openness(face_talk[face_talk_n]); face_talk_n++; }
        if (face_talk_n) {
            face_open_lo = face_open_hi = face_open[0];
            for (int i = 1; i < face_talk_n; i++) { if (face_open[i] < face_open_lo) face_open_lo = face_open[i]; if (face_open[i] > face_open_hi) face_open_hi = face_open[i]; }
            if (face_open_hi == face_open_lo) face_open_hi++;
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

/* Draws one frame at face_x/face_y, face_side logical pixels, at full
   physical resolution with rounded corners. */
static void face_blit(const unsigned char *px) {
    if (!px || face_x < 0) return;
    int sc = (int)window_scale(); if (sc < 1) sc = 1;
    int side = face_side * sc, r = (face_side > FACE_SIDE ? 14 : 6) * sc;
    int ox = face_x * sc, oy = face_y * sc;
    for (int y = 0; y < side; y++) {
        const unsigned char *row = px + (unsigned int)(y * FACE_SRC / side) * FACE_SRC * 3;
        int cy = y < r ? r - y : (y >= side - r ? y - (side - r - 1) : 0);
        for (int x = 0; x < side; x++) {
            int cx = x < r ? r - x : (x >= side - r ? x - (side - r - 1) : 0);
            if (cx && cy && cx * cx + cy * cy > r * r) continue;
            const unsigned char *p = row + (x * FACE_SRC / side) * 3;
            window_pixel_phys(ox + x, oy + y, ((unsigned int)p[0] << 16) | ((unsigned int)p[1] << 8) | p[2]);
        }
    }
}

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

/* Idle loop position steps forward and back (ping-pong), so the end of
   the clip never cuts back to its start. */
static void chat_face_idle_step(void) {
    static int idir = 1;
    if (face_idle_n < 2) return;
    if (face_idle_at + idir < 0 || face_idle_at + idir >= face_idle_n) idir = -idir;
    face_idle_at += idir;
}

/* sb16_play's progress hook, 12 frames a second (her clip's own rate).
   Every step moves one or two frames along the real talk clip, always in
   the current direction, turning around only at the clip's ends, so each
   change is genuine motion and there is never a loop seam. Whether it
   steps one or two is the frame whose mouth (face_open) better matches how
   loud she is right now, so the clip leans into her voice. A real pause (0.6s quiet)
   relaxes into the idle loop at the idle frame nearest her pose. */
static void chat_face_tick(unsigned int elapsed) {
    static unsigned int next_at = 0, sound_at = 0;
    static const unsigned char *cur = 0;
    static int dir = 1;
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
        face_blit(face_idle[face_idle_at]);
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
            if (face_shown + dir < 0 || face_shown + dir >= n) dir = -dir;
            int a = face_shown + dir, b = face_shown + 2 * dir;
            pick = a;
            if (b >= 0 && b < n) {
                unsigned int da = face_open[a] > target ? face_open[a] - target : target - face_open[a];
                unsigned int db = face_open[b] > target ? face_open[b] - target : target - face_open[b];
                if (db < da) pick = b;
            }
        }
        face_shown = face_talk_at = pick;
        cur = face_sig_talk[pick];
        face_blit(face_talk[pick]);
    }
    window_present();
}

/* Chat's idle wait: the next frame of her idle loop. Call about 12 times
   a second while nothing else is happening. */
static void chat_face_idle_tick(void) {
    if (!face_idle_n || face_x < 0) return;
    chat_face_idle_step();
    face_blit(face_idle[face_idle_at]);
    window_present();
}

/* speak_text with her mouth following the audio. */
static void chat_face_speak(const char *host, unsigned short port, const char *text, unsigned int timeout) {
    int talk = face_talk_n && face_x >= 0;
    face_shown = -1;
    if (talk) sb16_set_progress(chat_face_tick);
    speak_text(host, port, text, timeout);
    sb16_set_progress(0);
    face_shown = -1;
    if (talk) { face_blit(face_idle[face_idle_at]); window_present(); }
}
