/* Sound Blaster 16 driver. See sb16.h for the contract.

   The DMA buffer is a 64KB NOLOAD section the linker script pins at a
   64KB-aligned physical address (boot/linker.ld's .dmabuf), so a transfer
   never crosses a 64KB boundary and stays under the ISA DMA 16MB ceiling.
   It lives inside the kernel's base map, reachable at the higher-half alias,
   and sits before _kernel_end, so pmm.c never hands its frames out. */
#include "sb16.h"
#include "serial.h"
#include "pic.h"
#include "irq.h"

typedef unsigned int  u32;
typedef unsigned short u16;
typedef unsigned char u8;

static inline u8 inb(u16 p){ u8 v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void outb(u16 p, u8 v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }

#define SB_BASE      0x220
#define DSP_RESET    (SB_BASE + 0x6)
#define DSP_READ     (SB_BASE + 0xA)
#define DSP_WRITE    (SB_BASE + 0xC)
#define DSP_RSTATUS  (SB_BASE + 0xE)   /* bit 7: data ready; reading it acks 8-bit IRQs */
#define SB_IRQ       5
#define POLL_LIMIT   100000u

#define DMA_CHUNK    0x8000u   /* 32KB: v86 (the landing demo) plays one full 64KB transfer as a beep; QEMU was fine either way */
extern u8 sb16_dma_buf[];              /* boot/linker.ld, 64KB, 64KB-aligned */
#define KERNEL_VIRTUAL_BASE 0xC0000000u

static int present = 0;
static volatile int irq_done = 0;
static void (*progress_fn)(unsigned int) = 0;
static u32 play_off = 0;          /* bytes of this clip already played by earlier chunks */
/* A chunk that makes no DMA progress for this long is given up on. The
   browser demo keeps audio locked until the visitor's first click, and the
   old fixed (chunk length + 1s) limit expired while it was still locked, so
   she spoke the first two seconds and then went silent. */
#define STALL_TICKS  1200u        /* 12s at 100Hz */

void sb16_set_progress(void (*fn)(unsigned int elapsed_ticks)) { progress_fn = fn; }

static void io_delay(void) { for (int i = 0; i < 16; i++) (void)inb(0x80); }

static void put_uint_serial(unsigned int v) {
    char d[12]; int n = 0;
    if (v == 0) d[n++] = '0';
    while (v) { d[n++] = (char)('0' + v % 10); v /= 10; }
    char out[12]; int o = 0;
    while (n) out[o++] = d[--n];
    out[o] = 0;
    serial_puts(out);
}

static int dsp_write(u8 v) {
    for (u32 t = 0; t < POLL_LIMIT; t++)
        if (!(inb(DSP_WRITE) & 0x80)) { outb(DSP_WRITE, v); return 1; }
    return 0;
}

static int dsp_read(u8 *out) {
    for (u32 t = 0; t < POLL_LIMIT; t++)
        if (inb(DSP_RSTATUS) & 0x80) { *out = inb(DSP_READ); return 1; }
    return 0;
}

int sb16_init(void) {
    present = 0;
    outb(DSP_RESET, 1);
    io_delay();                        /* spec asks for at least 3 microseconds */
    outb(DSP_RESET, 0);
    u8 v = 0;
    /* An absent card floats the bus to 0xFF, so require the exact 0xAA. */
    for (int tries = 0; tries < 8 && v != 0xAA; tries++)
        if (!dsp_read(&v)) break;
    if (v != 0xAA) { serial_puts("sb16: not found\n"); return 0; }
    present = 1;
    pic_set_mask(SB_IRQ, 0);
    dsp_write(0xD1);                   /* speaker on */
    serial_puts("sb16: found at 0x220, irq 5, dma 1\n");
    return 1;
}

int sb16_present(void) { return present; }

void sb16_irq(void) {
    (void)inb(DSP_RSTATUS);            /* 8-bit transfer acknowledge */
    irq_done = 1;
}

/* How many of the n bytes of the running transfer the DMA controller has
   handed to the DSP so far (channel 1 count register: counts down, wraps to
   0xFFFF at terminal count). This is the real playback position, so the
   mouth follows the sound even when the speaker started late. */
static u32 dma_done(u32 n) {
    outb(0x0C, 0x00);
    u32 lo = inb(0x03), hi = inb(0x03), left = (hi << 8 | lo) + 1;
    return left > n ? n : n - left;
}

/* One single-cycle transfer of n bytes (1..64KB) already sitting in the
   DMA buffer. */
static int play_chunk(u32 n, u32 rate) {
    u32 phys = (u32)sb16_dma_buf - KERNEL_VIRTUAL_BASE;
    u32 cnt = n - 1;
    irq_done = 0;

    outb(0x0A, 0x05);                  /* mask channel 1 */
    outb(0x0C, 0x00);                  /* reset the byte flip-flop */
    outb(0x0B, 0x49);                  /* single mode, increment, memory to device, channel 1 */
    outb(0x02, (u8)phys);
    outb(0x02, (u8)(phys >> 8));
    outb(0x83, (u8)(phys >> 16));      /* channel 1 page register */
    outb(0x0C, 0x00);
    outb(0x03, (u8)cnt);
    outb(0x03, (u8)(cnt >> 8));
    outb(0x0A, 0x01);                  /* unmask channel 1 */

    if (!dsp_write(0x41) || !dsp_write((u8)(rate >> 8)) || !dsp_write((u8)rate)) return 0;
    if (!dsp_write(0xC0) || !dsp_write(0x00) ||           /* 8-bit single-cycle output, mono unsigned */
        !dsp_write((u8)cnt) || !dsp_write((u8)(cnt >> 8))) return 0;

    /* Wait for IRQ 5 while the count register moves; give up only when it
       stalls. progress_fn gets elapsed ticks of the clip as played (the
       DMA runs about one 1KB block ahead of the sound, so back off 40ms). */
    u32 last = 0, moved = ticks();
    while (!irq_done) {
        u32 pos = dma_done(n);
        if (pos != last) { last = pos; moved = ticks(); }
        else if (ticks() - moved > STALL_TICKS) break;
        if (progress_fn) {
            u32 at = play_off + pos, back = rate / 25u;
            progress_fn((at > back ? at - back : 0) * 100u / rate);
        }
        __asm__ volatile("pause");
    }
    if (!irq_done) { serial_puts("sb16: transfer timed out\n"); return 0; }
    return 1;
}

int sb16_play(const unsigned char *pcm, unsigned int len, unsigned int rate) {
    if (!present || !pcm || !len) return 0;
    if (rate < 4000) rate = 4000;
    if (rate > 44100) rate = 44100;
    play_off = 0;
    while (len) {
        u32 n = len > DMA_CHUNK ? DMA_CHUNK : len;
        if (pcm != sb16_dma_buf)
            for (u32 i = 0; i < n; i++) sb16_dma_buf[i] = pcm[i];
        if (!play_chunk(n, rate)) return 0;
        pcm += n; len -= n; play_off += n;
    }
    return 1;
}

/* One single-cycle ADC (record) transfer of n bytes into sb16_dma_buf,
   the mirror of play_chunk but with the DMA controller programmed for
   "write to memory" (mode 0x45: same single/increment/no-autoinit/
   channel-1 bits as play_chunk's 0x49, transfer-type bits flipped from
   10 (read, memory->device) to 01 (write, device->memory)), and the
   DSP's old-style ADC pair (0x40 time constant, then 0x24 length-1 lo/hi)
   instead of play_chunk's new-style 0x41 rate / 0xC0 mode / length pair.
   The old pair is DSP-2.xx compatible, so it is the one command every
   real SB16 and QEMU's `-device sb16` both honor identically. */
static int record_chunk(u32 n, u32 rate) {
    u32 phys = (u32)sb16_dma_buf - KERNEL_VIRTUAL_BASE;
    u32 cnt = n - 1;
    irq_done = 0;

    outb(0x0A, 0x05);                  /* mask channel 1 */
    outb(0x0C, 0x00);                  /* reset the byte flip-flop */
    outb(0x0B, 0x45);                  /* single mode, increment, write to memory, channel 1 */
    outb(0x02, (u8)phys);
    outb(0x02, (u8)(phys >> 8));
    outb(0x83, (u8)(phys >> 16));      /* channel 1 page register */
    outb(0x0C, 0x00);
    outb(0x03, (u8)cnt);
    outb(0x03, (u8)(cnt >> 8));
    outb(0x0A, 0x01);                  /* unmask channel 1 */

    /* time constant = 256 - 1000000/rate (8-bit DSP-2.xx formula). */
    u8 tc = (u8)(256u - 1000000u / rate);
    if (!dsp_write(0x40) || !dsp_write(tc)) return 0;
    if (!dsp_write(0x24) || !dsp_write((u8)cnt) || !dsp_write((u8)(cnt >> 8))) return 0;

    u32 limit = (n * 100u) / rate + 100u, start = ticks();
    while (!irq_done && ticks() - start < limit) __asm__ volatile("pause");
    if (!irq_done) { serial_puts("sb16: record timed out\n"); return 0; }
    return 1;
}

int sb16_record(unsigned char *out, unsigned int max_len, unsigned int rate) {
    if (!present || !out || !max_len) return 0;
    if (rate < 4000) rate = 4000;
    if (rate > 44100) rate = 44100;
    unsigned int captured = 0;
    serial_puts("sb16: record start\n");
    while (captured < max_len) {
        u32 n = (max_len - captured) > DMA_CHUNK ? DMA_CHUNK : (max_len - captured);
        if (!record_chunk(n, rate)) break;
        for (u32 i = 0; i < n; i++) out[captured + i] = sb16_dma_buf[i];
        captured += n;
    }
    serial_puts("sb16: record done n="); put_uint_serial(captured); serial_puts("\n");
    return (int)captured;
}

int sb16_beep(unsigned int freq, unsigned int ms) {
    if (!present || !freq) return 0;
    const u32 rate = 22050;
    u32 n = rate * ms / 1000;
    if (n > DMA_CHUNK) n = DMA_CHUNK;
    if (!n) return 0;
    /* Triangle wave from a 16.16 phase accumulator: no floating point, and
       its energy sits almost entirely on the fundamental. */
    if (freq > rate / 2) freq = rate / 2;
    u32 step = (freq << 16) / rate, phase = 0;
    for (u32 i = 0; i < n; i++) {
        u32 p = phase & 0xFFFF;                                        /* 0..65535 */
        int tri = p < 0x8000 ? (int)(p >> 7) : (int)((0xFFFF - p) >> 7); /* 0..255 */
        sb16_dma_buf[i] = (u8)(28 + tri * 200 / 255);                  /* 28..228, centred on 128 */
        phase += step;
    }
    serial_puts("sb16: beep\n");
    return sb16_play(sb16_dma_buf, n, rate);
}
