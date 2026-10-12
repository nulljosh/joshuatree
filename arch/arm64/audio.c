/* Sound out of the Pi 4's 3.5 mm jack: PWM1 on GPIO 40 (left) and 41 (right). `beep` (cmd.c) is fed by hand from the CPU
   (polled): samples go through a small ring in memory and from the ring into the PWM FIFO. An app's sound (M3e, JT_SYS_AUDIO,
   further down) goes through DMA instead, so the desktop never waits on it. The jack is NEVER touched at boot and GPIO 42
   (the green light) is never touched at all; only `beep`, the volume keys and an app that queues sound reach the hardware.
   Every wait is a counting loop that gives up and returns -1, never a wait on the kernel's tick count. On a build without
   PI_BUILD (QEMU virt) there is no PWM: the ring is filled and drained into nothing, so a check can read back what the
   generator produced. The jack itself is unverified until the real board. */
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
static unsigned sum_abs;                            /* sum of |sample| over every sample pushed (wraps at 2^32): halves when the volume halves */
static int volume = AUDIO_DEFAULT_VOLUME, last_volume = AUDIO_DEFAULT_VOLUME;   /* percent; last_volume is the level mute returns to */

int audio_volume(void) { return volume; }
void audio_set_volume(int percent) { volume = percent < 0 ? 0 : percent > 100 ? 100 : percent; if (volume) last_volume = volume; }
int audio_mute(void) { audio_set_volume(volume ? 0 : last_volume); return volume; }

static void put_str(const char *s) { kputs(s); }
static void put_dec(unsigned v) { char t[11]; unsigned n = 10; t[n] = 0; do t[--n] = (char)('0' + v % 10); while (v /= 10); kputs(t + n); }
static void put_hex(unsigned v) { char t[9]; for (int i = 0; i < 8; i++) t[i] = "0123456789abcdef"[(v >> (28 - 4 * i)) & 15]; t[8] = 0; kputs(t); }
static void fail(const char *why) { put_str("audio: "); put_str(why); put_str("\n"); }

static void hash_short(short s) {
    sum_hash = (sum_hash ^ ((unsigned)s & 0xFF)) * 16777619u;
    sum_hash = (sum_hash ^ ((unsigned)s >> 8 & 0xFF)) * 16777619u;
}
static short scale(short s) { return (short)((int)s * volume / 100); }   /* toward zero, never past the input: the PWM range cannot be exceeded */
static void ring_push(short l, short r) {
    l = scale(l); r = scale(r);
    sum_abs += (unsigned)(l < 0 ? -l : l) + (unsigned)(r < 0 ? -r : r);
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

/* ---- M3e: an app's sound, JT_SYS_AUDIO. main.c answers the call and checks every pointer; this is the queue behind it.
   An app queues 8-bit unsigned mono PCM into a 32 KB ring (audio_app_queue), as on i386, and the call never waits. The
   desktop's loop calls audio_pump on every pass: what the hardware has taken is counted and turned back into silence, and
   the ring's next bytes go into the PWM ring, each one a stereo frame of (byte - 128) * 256 scaled by the master volume, as
   the PWM's two 32-bit words (0..range). The pump keeps about an eighth of a second ahead of the hardware, no more, so a
   stop is heard quickly. On the Pi a DMA channel (5, paced by DREQ 1, PWM1's on the BCM2711) copies the PWM ring into
   PWM1's FIFO round and round, so the sound never waits for the CPU. QEMU virt has no PWM, so there the generic counter
   takes the ring at the stream's rate and an app's clock runs as it would on the board. (QEMU's raspi4b ignores DREQ and
   would spin on the looping control block: play app sound on virt.) `played` is the app's own frames the hardware has
   taken since the queue was last idle, so a player's clock is the sound actually heard. When all of it has been taken the
   stream ends, the PWM stops and one line says what went out: `audio: app stream N frames at R Hz, ring sum 0x..., abs A,
   volume V, played P` (N frames fed; the FNV-1a sum and the sum of |sample| over them, both channels, as beep's line has).
   beep is refused while a stream runs. */
#define AQ_SIZE 32768u          /* the app's ring, one byte a frame (a power of two) */
#define PCM_FRAMES 16384u       /* the PWM ring: frames of two words, 128 KB, 0.37 s at 44.1 kHz */
#define GUARD 64u               /* frames left between the hardware and new sound: what the DMA may have read ahead */
static unsigned char aq[AQ_SIZE];
static unsigned aq_head, aq_tail, aq_rate = 16000;   /* free-running byte counts; queued = head - tail */
static int aq_end, app_live;                          /* the clip's last bytes are in; the stream runs (the PWM is on) */
static unsigned pcm[PCM_FRAMES * 2] __attribute__((aligned(64)));
static unsigned long pump_at, pcm_wr, run0;          /* free-running frames: taken at the last pump, the next to write, the current run's first */
static unsigned app_played, app_fed, app_hash, app_abs, silence;
static unsigned fnv_short(unsigned h, short s) { h = (h ^ ((unsigned)s & 0xFF)) * 16777619u; return (h ^ ((unsigned)s >> 8 & 0xFF)) * 16777619u; }
static void pcm_put(unsigned long k, unsigned w) { unsigned i = (unsigned)(k % PCM_FRAMES) * 2; pcm[i] = w; pcm[i + 1] = w; }
static void pcm_clean(unsigned long from, unsigned long to) {   /* frames [from, to) of the ring out of the data cache, to RAM, where the DMA reads */
#if defined(PI_BUILD) && !defined(AUDIO_HOST_TEST)
    if (to - from > PCM_FRAMES) from = to - PCM_FRAMES;
    while (from < to) {
        unsigned long i = from % PCM_FRAMES, n = PCM_FRAMES - i;
        if (n > to - from) n = to - from;
        for (unsigned long a = (unsigned long)&pcm[i * 2] & ~63UL; a < (unsigned long)&pcm[(i + n) * 2]; a += 64) __asm__ volatile ("dc cvac, %0" :: "r"(a) : "memory");
        from += n;
    }
    __asm__ volatile ("dsb sy" ::: "memory");
#else
    (void)from; (void)to;
#endif
}
#ifdef PI_BUILD
#define DMA_CH 5                                                  /* a channel Linux leaves to the ARM */
#define DMA_REG(o) (0xFE007000UL + DMA_CH * 0x100UL + (o))         /* CS 0x00, CONBLK_AD 0x04, SOURCE_AD 0x0C */
#define DMA_ENABLE 0xFE007FF0UL
#define BUS(p) ((unsigned)(unsigned long)(p) | 0xC0000000u)       /* RAM as the legacy DMA engines see it (the low GiB) */
static struct dma_cb { unsigned ti, src, dst, len, stride, next, pad[2]; } dma_cb __attribute__((aligned(32)));
static unsigned long hw_done;   /* frames the DMA has taken, laps counted */
static unsigned dma_at;         /* the ring frame its source address was at, last look */
static int pcm_start(void) {
    if (hw_open()) { hw_close(); return -1; }
    dma_cb.ti = (1u << 26) | (1u << 16) | (1u << 8) | (1u << 6) | (1u << 3);   /* no wide bursts, PERMAP 1 (PWM1), SRC_INC, DEST_DREQ, WAIT_RESP */
    dma_cb.src = BUS(pcm); dma_cb.dst = 0x7E20C818u;   /* PWM1's FIF1 at its bus address */
    dma_cb.len = sizeof pcm; dma_cb.stride = 0; dma_cb.next = BUS(&dma_cb); dma_cb.pad[0] = dma_cb.pad[1] = 0;   /* round and round */
#ifndef AUDIO_HOST_TEST
    __asm__ volatile ("dc cvac, %0\n dsb sy" :: "r"(&dma_cb) : "memory");
#endif
    hw_done = 0; dma_at = 0;
    WR(DMA_ENABLE, RD(DMA_ENABLE) | 1u << DMA_CH);
    WR(DMA_REG(0x00), 1u << 31);                                  /* reset the channel */
    WR(PWM1_BASE + 0x08, (1u << 31) | (7u << 8) | 7u);            /* DMAC: DMA requests on, panic and DREQ at 7 words */
    WR(DMA_REG(0x04), BUS(&dma_cb));
    WR(DMA_REG(0x00), (1u << 28) | (8u << 20) | (8u << 16) | 1u); /* wait for outstanding writes, priorities 8, active */
    return 0;
}
static unsigned long pcm_done(void) {   /* the DMA's source address, as frames taken; a lap the pump never saw is lost */
    unsigned src = RD(DMA_REG(0x0C)), base = BUS(pcm);
    if (src >= base && src < base + (unsigned)sizeof pcm) {
        unsigned at = (src - base) / 8;
        hw_done += (at + PCM_FRAMES - dma_at) % PCM_FRAMES;
        dma_at = at;
    }
    return hw_done;
}
static void pcm_stop(void) {
    WR(DMA_REG(0x00), 1u << 31);   /* reset: the channel stops */
    WR(PWM1_BASE + 0x08, 0);
    hw_close();
}
#else
static unsigned long pcm_t0;
static unsigned long cnt(void) { unsigned long c; __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(c)); return c; }
static unsigned long cnt_freq(void) { unsigned long f; __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(f)); return f ? f : 62500000; }
static int pcm_start(void) { pcm_t0 = cnt(); return 0; }
static unsigned long pcm_done(void) { return (cnt() - pcm_t0) * aq_rate / cnt_freq(); }
static void pcm_stop(void) {}
#endif
static void app_finish(void) {
    pcm_stop();
    app_live = 0; aq_end = 0;
    put_str("audio: app stream "); put_dec(app_fed); put_str(" frames at "); put_dec(aq_rate); put_str(" Hz, ring sum 0x"); put_hex(app_hash);
    put_str(", abs "); put_dec(app_abs); put_str(", volume "); put_dec((unsigned)volume); put_str(", played "); put_dec(app_played); put_str("\n");
}
static void app_begin(void) {
    range = 27000000u / aq_rate;
    silence = (unsigned)(32768 * (int)range >> 16);   /* sample 0: the PWM's midpoint */
    for (unsigned i = 0; i < PCM_FRAMES * 2; i++) pcm[i] = silence;
    pcm_clean(0, PCM_FRAMES);
    app_hash = 2166136261u; app_abs = app_fed = 0;
    pump_at = 0; pcm_wr = run0 = GUARD;
    if (pcm_start()) { fail("app sound did not start"); aq_tail = aq_head; aq_end = 0; return; }
    app_live = 1;
}
void audio_pump(void) {
    if (!app_live) {
        unsigned queued = aq_head - aq_tail;
        if (queued < 1024 && !(aq_end && queued)) return;   /* as on i386: start at 1 KB, or at once with the clip's last bytes */
        app_begin();
        if (!app_live) return;
    }
    unsigned long done = pcm_done(), from = done - pump_at > PCM_FRAMES ? done - PCM_FRAMES : pump_at;
    unsigned long lo = pump_at > run0 ? pump_at : run0, hi = done < pcm_wr ? done : pcm_wr;
    if (hi > lo) app_played += (unsigned)(hi - lo);       /* the app's own frames the hardware took since the last pump */
    for (unsigned long k = from; k < done; k++) pcm_put(k, silence);   /* behind it, silence again, for its next lap */
    pcm_clean(from, done);
    pump_at = done;
    if (aq_head == aq_tail) { if (done >= pcm_wr) app_finish(); return; }   /* all of it sent: the stream ends once the hardware has taken it */
    if (pcm_wr < done) pcm_wr = run0 = done + GUARD;      /* it ran dry: carry on just ahead of it */
    unsigned long wr0 = pcm_wr, ahead = done + aq_rate / 8;
    while (pcm_wr < ahead && aq_head != aq_tail) {
        short v = scale((short)(((int)aq[aq_tail++ % AQ_SIZE] - 128) * 256));
        pcm_put(pcm_wr++, (unsigned)(((int)v + 32768) * (int)range >> 16));
        app_abs += 2u * (unsigned)(v < 0 ? -v : v);
        app_hash = fnv_short(fnv_short(app_hash, v), v);   /* left, then right */
        app_fed++;
    }
    pcm_clean(wr0, pcm_wr);
}
int audio_app_queue(const unsigned char *p, unsigned len, unsigned rate, int end) {   /* bytes taken, 0 when the ring is full */
    if (!app_live && aq_head == aq_tail) {   /* idle: this call picks the rate, and played starts again from 0 */
        aq_rate = rate < 4000 ? 4000 : rate > 44100 ? 44100 : rate;
        app_played = 0; aq_end = 0;
    }
    unsigned space = AQ_SIZE - (aq_head - aq_tail);
    if (len > space) { len = space; end = 0; }   /* cut short: the END goes with the rest */
    for (unsigned i = 0; i < len; i++) aq[(aq_head + i) % AQ_SIZE] = p[i];
    aq_head += len;
    if (end) aq_end = 1;
    audio_pump();
    return (int)len;
}
void audio_app_status(unsigned st[7]) {   /* struct jt_audio_status: version, size, playing, queued, space, rate, played */
    audio_pump();
    unsigned q = aq_head - aq_tail;
    st[0] = 1; st[1] = 28; st[2] = app_live || q; st[3] = q; st[4] = AQ_SIZE - q; st[5] = aq_rate; st[6] = app_played;
}
void audio_app_stop(void) { aq_tail = aq_head; aq_end = 0; }   /* what is queued goes; what is in the PWM ring plays out */
int audio_busy(void) { return app_live || aq_head != aq_tail; }

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
    if (audio_busy()) { fail("busy, an app is playing"); return -1; }   /* M3e: the PWM is the app's until its stream ends */
    ring_head = ring_tail = ring_count = 0; sum_hash = 2166136261u; sum_abs = 0; sum_frames = drained = 0;
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
    put_str("audio: "); put_dec(sum_frames); put_str(" frames, ring sum 0x"); put_hex(sum_hash);
    put_str(", abs "); put_dec(sum_abs); put_str(", volume "); put_dec((unsigned)volume); put_str("\n");
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
