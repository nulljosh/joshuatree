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

#define DMA_CHUNK    0x10000u
extern u8 sb16_dma_buf[];              /* boot/linker.ld, 64KB, 64KB-aligned */
#define KERNEL_VIRTUAL_BASE 0xC0000000u

static int present = 0;
static volatile int irq_done = 0;
static void (*progress_fn)(unsigned int) = 0;
static u32 play_start = 0;

void sb16_set_progress(void (*fn)(unsigned int elapsed_ticks)) { progress_fn = fn; }

static void io_delay(void) { for (int i = 0; i < 16; i++) (void)inb(0x80); }

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

    /* Wait for IRQ 5, bounded by the clip length plus one second of slack
       (ticks() runs at 100Hz). */
    u32 limit = (n * 100u) / rate + 100u, start = ticks();
    while (!irq_done && ticks() - start < limit) {
        if (progress_fn) progress_fn(ticks() - play_start);
        __asm__ volatile("pause");
    }
    if (!irq_done) { serial_puts("sb16: transfer timed out\n"); return 0; }
    return 1;
}

int sb16_play(const unsigned char *pcm, unsigned int len, unsigned int rate) {
    if (!present || !pcm || !len) return 0;
    if (rate < 4000) rate = 4000;
    if (rate > 44100) rate = 44100;
    play_start = ticks();
    while (len) {
        u32 n = len > DMA_CHUNK ? DMA_CHUNK : len;
        if (pcm != sb16_dma_buf)
            for (u32 i = 0; i < n; i++) sb16_dma_buf[i] = pcm[i];
        if (!play_chunk(n, rate)) return 0;
        pcm += n; len -= n;
    }
    return 1;
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
