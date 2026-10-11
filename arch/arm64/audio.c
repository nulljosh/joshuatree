/* Sound out of the Pi 4's 3.5 mm jack: PWM1 on GPIO 40 (left) and 41 (right), fed by hand from the CPU (polled, no DMA).
   Samples go through a small ring in memory and from the ring into the PWM FIFO. The jack is NEVER touched at boot and GPIO 42
   (the green light) is never touched at all; only `beep` (cmd.c) calls in here. Every wait is a counting loop that gives up and
   returns -1, never a wait on `ticks`. On a build without PI_BUILD (QEMU virt) there is no PWM: the ring is filled and drained
   into nothing, so a check can read back what the generator produced. The jack itself is unverified until the real board. */
#include "audio.h"
#ifdef AUDIO_HOST_TEST
unsigned audio_rd(unsigned long addr);
void audio_wr(unsigned long addr, unsigned v);
#define RD(a) audio_rd(a)
#define WR(a, v) audio_wr((a), (v))
#else
#define RD(a) (*(volatile unsigned *)(a))
#define WR(a, v) (*(volatile unsigned *)(a) = (v))
#endif
void kputs(const char *s);   /* main.c: the UART and the log of the window in front */

#define GPIO_BASE 0xFE200000UL   /* GPFSEL4 +0x10 (pins 40 to 49), pull control for pins 32 to 47 at +0xE8 */
#define PWM1_BASE 0xFE20C800UL   /* CTL 0x00, STA 0x04, RNG1 0x10, FIF1 0x18, RNG2 0x20 */
#define CM_BASE   0xFE101000UL   /* PWMCTL +0xA0, PWMDIV +0xA4 */
#define CM_PW 0x5A000000u
#define SPIN 2000000u            /* polls of one hardware flag before giving up (about a tenth of a second or more) */
#define RING_FRAMES 2048u
#define TONE_RATE 44100
#define AMP 6000                 /* under a fifth of full scale: kind to headphones */

static short ring[RING_FRAMES * 2];
static unsigned ring_head, ring_tail, ring_count;   /* frames */
static unsigned sum_hash;                           /* FNV-1a over every sample pushed, low byte first */
static unsigned sum_frames, drained, range;

static void put_str(const char *s) { kputs(s); }
static void put_dec(unsigned v) { char t[11]; unsigned n = 10; t[n] = 0; do t[--n] = (char)('0' + v % 10); while (v /= 10); kputs(t + n); }
static void put_hex(unsigned v) { char t[9]; for (int i = 0; i < 8; i++) t[i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15]; t[8] = 0; kputs(t); }
static void fail(const char *why) { put_str("audio: "); put_str(why); put_str("\n"); }

static void hash_short(short s) {
    sum_hash = (sum_hash ^ ((unsigned)s & 0xFF)) * 16777619u;
    sum_hash = (sum_hash ^ ((unsigned)s >> 8 & 0xFF)) * 16777619u;
}
static void ring_push(short l, short r) {
    ring[ring_head * 2] = l; ring[ring_head * 2 + 1] = r;
    ring_head = (ring_head + 1) % RING_FRAMES; ring_count++;
    hash_short(l); hash_short(r); sum_frames++;
}

#ifdef PI_BUILD
static int wait_flag(unsigned long reg, unsigned mask, unsigned want) {   /* until (reg & mask) == want, at most SPIN polls */
    for (unsigned i = 0; i < SPIN; i++) if ((RD(reg) & mask) == want) return 1;
    return 0;
}
static void hw_close(void) {   /* pins back to inputs, PWM and its clock off: the jack goes quiet. Safe to call twice. */
    WR(PWM1_BASE + 0x00, 0);
    WR(GPIO_BASE + 0x10, RD(GPIO_BASE + 0x10) & ~0x3Fu);   /* GPIO 40 and 41 only; bits 6 to 8 (GPIO 42) are left as they are */
    WR(CM_BASE + 0xA0, CM_PW | (RD(CM_BASE + 0xA0) & 0x6F));
}
static int hw_open(void) {
    WR(CM_BASE + 0xA0, CM_PW | (RD(CM_BASE + 0xA0) & 0x6F));   /* stop the clock before changing its divider */
    if (!wait_flag(CM_BASE + 0xA0, 0x80, 0)) { fail("clock will not stop"); return -1; }
    WR(CM_BASE + 0xA4, CM_PW | (2u << 12));                    /* 54 MHz oscillator / 2 = 27 MHz */
    WR(CM_BASE + 0xA0, CM_PW | 1u);
    WR(CM_BASE + 0xA0, CM_PW | 1u | 0x10u);
    if (!wait_flag(CM_BASE + 0xA0, 0x80, 0x80)) { fail("clock will not start"); WR(CM_BASE + 0xA0, CM_PW); return -1; }
    WR(PWM1_BASE + 0x00, 0);
    WR(PWM1_BASE + 0x04, 0x1FC);                               /* clear the sticky error flags */
    WR(PWM1_BASE + 0x10, range); WR(PWM1_BASE + 0x20, range);
    WR(PWM1_BASE + 0x00, 0x40);                                /* CLRF1: empty the FIFO */
    WR(PWM1_BASE + 0x00, 0x2121);                              /* both channels on, both fed from the FIFO */
    WR(GPIO_BASE + 0xE8, RD(GPIO_BASE + 0xE8) & ~0x0FF0000u);  /* no pull on pins 40 and 41 */
    WR(GPIO_BASE + 0x10, (RD(GPIO_BASE + 0x10) & ~0x3Fu) | 0x24u);   /* GPIO 40 and 41 to alt 0 = PWM1 channels 0 and 1 */
    return 0;
}
static int hw_put(short s) {   /* a sample into the FIFO, scaled from -32768..32767 into 0..range */
    if (!wait_flag(PWM1_BASE + 0x04, 1, 0)) { fail("PWM FIFO stays full"); return -1; }
    WR(PWM1_BASE + 0x18, (unsigned)(((int)s + 32768) * (int)range >> 16));
    return 0;
}
static int hw_finish(void) { if (wait_flag(PWM1_BASE + 0x04, 2, 2)) return 0; fail("PWM FIFO never emptied"); return -1; }
#else
static int hw_open(void) { return 0; }
static int hw_put(short s) { (void)s; return 0; }
static int hw_finish(void) { return 0; }
static void hw_close(void) {}
#endif

static int drain(void) {   /* the ring into the sink, a frame at a time */
    while (ring_count) {
        if (hw_put(ring[ring_tail * 2]) || hw_put(ring[ring_tail * 2 + 1])) return -1;
        ring_tail = (ring_tail + 1) % RING_FRAMES; ring_count--; drained++;
    }
    return 0;
}

/* Push `frames` frames through the ring from `src` (stereo) or, when src is 0, a triangle wave of `freq` Hz. */
static int stream(const short *src, int frames, int rate, int freq) {
    unsigned phase = 0, step = freq ? (unsigned)(((unsigned long)freq << 32) / (unsigned)rate) : 0;
    int bad = 0;
    ring_head = ring_tail = ring_count = 0; sum_hash = 2166136261u; sum_frames = drained = 0;
    range = 27000000u / (unsigned)rate;
    if (hw_open()) { hw_close(); return -1; }
    for (int i = 0; i < frames && !bad; ) {
        while (i < frames && ring_count < RING_FRAMES) {
            short l, r;
            if (src) { l = src[i * 2]; r = src[i * 2 + 1]; }
            else {
                int p = (int)(phase >> 18) - 8192;           /* -8192..8191 */
                int t = p < 0 ? -p : p;                      /* 0..8192, a triangle */
                l = r = (short)((t - 4096) * AMP / 4096);
                phase += step;
            }
            ring_push(l, r); i++;
        }
        bad = drain();
    }
    if (!bad && hw_finish()) bad = -1;
    hw_close();
    put_str("audio: "); put_dec(sum_frames); put_str(" frames, ring sum 0x"); put_hex(sum_hash); put_str("\n");
    if (bad) return -1;
#ifndef PI_BUILD
    fail("no PWM on this machine, samples went nowhere");
    return -1;
#else
    put_str("audio: played "); put_dec(drained); put_str(" frames\n");
    return 0;
#endif
}

int audio_play(const short *samples, int frames, int rate) {
    if (!samples || frames < 1 || rate < 8000 || rate > 48000) { fail("bad samples or rate"); return -1; }
    return stream(samples, frames, rate, 0);
}
int audio_tone(int freq_hz, int ms) {
    if (freq_hz < 20 || freq_hz > 20000 || ms < 1 || ms > 5000) { fail("tone needs 20..20000 Hz and 1..5000 ms"); return -1; }
    put_str("audio: tone "); put_dec((unsigned)freq_hz); put_str(" Hz "); put_dec((unsigned)ms); put_str(" ms\n");
    return stream(0, (int)((long)TONE_RATE * ms / 1000), TONE_RATE, freq_hz);
}
