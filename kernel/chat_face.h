/* Samantha's face in the Chat window.

   The first time Chat opens, this fetches a small set of 120x120 PNG
   frames over plain HTTP (idle-0..3 and talk-0..7 under /face/ on
   joshuatree.heyitsmejosh.com, cut from her character videos by
   tools/gen/face_frames.sh) and keeps them decoded in RAM. Chat shows an
   idle frame in a square at the top right. While her reply plays on the
   Sound Blaster, sb16_play's wait loop calls chat_face_tick, which steps
   through the talk frames; when the audio ends she goes back to idle.

   Everything is optional. A missing host, a 404, a bad PNG or a frame of
   the wrong size just means fewer frames; no idle-0 means no face at all,
   and Chat looks and works exactly as it did before. One fetch per boot,
   fixed caps, and a failed first frame stops the rest so a dead network
   costs one timeout, not twelve.

   facehost=HOST[:PORT] on the kernel command line points the fetch
   somewhere else (tools/checks/chat-face-check.py serves solid colors). */
#include "sb16.h"

#define FACE_IDLE_MAX  4
#define FACE_TALK_MAX  8
#define FACE_SRC       120            /* source frame side, pixels */
#define FACE_SIDE      60             /* on-screen side, logical pixels (120 physical at 2x) */
#define FACE_FILE_MAX  (32u * 1024u)  /* one PNG; real frames are ~12KB */
#define FACE_TIMEOUT   300            /* ~3s per frame */
#define FACE_STEP      10             /* ticks per talk frame, ~10 per second */
#define FACE_HOST_MAX  64

static char face_host[FACE_HOST_MAX] = "joshuatree.heyitsmejosh.com";
static unsigned short face_port = 80;
static unsigned char *face_idle[FACE_IDLE_MAX], *face_talk[FACE_TALK_MAX]; /* RGB, FACE_SRC^2 * 3 */
static int face_idle_n = 0, face_talk_n = 0, face_tried = 0;
static int face_x = -1, face_y = -1;  /* viewport-local logical top-left, -1 = not placed */
static int face_shown = -1;           /* talk frame on screen during playback */

/* kmain hands over the boot command line; only facehost= is read. */
static void chat_face_cmdline(const char *cl) {
    for (const char *p = cl; p && *p; p++) {
        if (p[0]=='f' && p[1]=='a' && p[2]=='c' && p[3]=='e' && p[4]=='h' && p[5]=='o' && p[6]=='s' && p[7]=='t' && p[8]=='=') {
            p += 9; int n = 0;
            while (*p && *p != ' ' && *p != ':' && n < FACE_HOST_MAX - 1) face_host[n++] = *p++;
            face_host[n] = 0;
            if (*p == ':') {
                unsigned int pt = 0; p++;
                while (*p >= '0' && *p <= '9') pt = pt * 10 + (unsigned int)(*p++ - '0');
                if (pt && pt < 65536) face_port = (unsigned short)pt;
            }
            return;
        }
    }
}

/* One frame: 200, a PNG, exactly FACE_SRC square. Returns an RGB buffer
   or 0. */
static unsigned char *face_fetch(unsigned char *file, const char *kind, int i) {
    char path[24]; int n = 0;
    const char *s = "/face/"; while (*s) path[n++] = *s++;
    while (*kind) path[n++] = *kind++;
    path[n++] = '-'; path[n++] = (char)('0' + i);
    s = ".png"; while (*s) path[n++] = *s++;
    path[n] = 0;
    int got = http_get_timeout(face_host, path, face_port, file, FACE_FILE_MAX, FACE_TIMEOUT);
    if (got <= 0 || http_last_status() != 200) return 0;
    unsigned char *px = 0; unsigned int w = 0, h = 0, ch = 0;
    if (png_decode(file, (unsigned int)got, &px, &w, &h, &ch) != 0 || !px) return 0;
    if (w != FACE_SRC || h != FACE_SRC || (ch != 3 && ch != 4)) { kfree(px); return 0; }
    if (ch == 4) /* squeeze RGBA down to RGB in place */
        for (unsigned int k = 0; k < FACE_SRC * FACE_SRC; k++) {
            px[k * 3] = px[k * 4]; px[k * 3 + 1] = px[k * 4 + 1]; px[k * 3 + 2] = px[k * 4 + 2];
        }
    return px;
}

static void chat_face_load(void) {
    if (face_tried) return;
    face_tried = 1;
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
    if (!net_init(0x0A00020F)) return;
    unsigned char *file = kmalloc(FACE_FILE_MAX);
    if (!file) return;
    while (face_idle_n < FACE_IDLE_MAX && (face_idle[face_idle_n] = face_fetch(file, "idle", face_idle_n))) face_idle_n++;
    if (face_idle_n)
        while (face_talk_n < FACE_TALK_MAX && (face_talk[face_talk_n] = face_fetch(file, "talk", face_talk_n))) face_talk_n++;
    kfree(file);
    serial_puts("face: idle="); { char d[2] = { (char)('0' + face_idle_n), 0 }; serial_puts(d); }
    serial_puts(" talk="); { char d[2] = { (char)('0' + face_talk_n), 0 }; serial_puts(d); }
    serial_puts("\n");
}

/* Logical pixels Chat's text column gives up on the right for the face. */
static int chat_face_reserve(void) { return face_idle_n ? FACE_SIDE + 16 : 0; }

/* Draws one frame at full physical resolution, corners rounded. */
static void face_blit(const unsigned char *px) {
    if (!px || face_x < 0) return;
    int sc = (int)window_scale(); if (sc < 1) sc = 1;
    int side = FACE_SIDE * sc, r = 6 * sc;
    int ox = face_x * sc, oy = face_y * sc;
    for (int y = 0; y < side; y++) {
        int sy = y * FACE_SRC / side;
        for (int x = 0; x < side; x++) {
            int cx = x < r ? r - x : (x >= side - r ? x - (side - r - 1) : 0);
            int cy = y < r ? r - y : (y >= side - r ? y - (side - r - 1) : 0);
            if (cx && cy && cx * cx + cy * cy > r * r) continue;
            const unsigned char *p = px + (sy * FACE_SRC + x * FACE_SRC / side) * 3;
            window_pixel_phys(ox + x, oy + y, ((unsigned int)p[0] << 16) | ((unsigned int)p[1] << 8) | p[2]);
        }
    }
}

/* Places the face at the top right of the current Chat view and shows the
   first idle frame. A no-op without frames. */
static void chat_face_draw(int T) {
    if (!face_idle_n) return;
    face_x = (int)window_width() - 20 - FACE_SIDE;
    face_y = T + 44;
    face_blit(face_idle[0]);
}

/* sb16_play's progress hook: next talk frame every FACE_STEP ticks. */
static void chat_face_tick(unsigned int elapsed) {
    int f = (int)((elapsed / FACE_STEP) % (unsigned int)face_talk_n);
    if (f == face_shown) return;
    face_shown = f;
    face_blit(face_talk[f]);
    window_present();
}

/* speak_text with her mouth moving while the audio plays. */
static void chat_face_speak(const char *host, unsigned short port, const char *text, unsigned int timeout) {
    int talk = face_talk_n && face_x >= 0;
    face_shown = -1;
    if (talk) sb16_set_progress(chat_face_tick);
    speak_text(host, port, text, timeout);
    sb16_set_progress(0);
    if (talk) { face_blit(face_idle[0]); window_present(); }
}
