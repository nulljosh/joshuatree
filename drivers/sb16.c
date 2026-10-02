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
#include "irqlock.h"

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
static u32 play_start = 0;

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

/* 1.9.26: async queue state (see sb16_queue below). Shared with the IRQ: only touched with
   interrupts off, or from the IRQ itself. */
#define AUD_RING   32768u      /* power of two */
#define AUD_CHUNK  4096u       /* bytes per DMA transfer, a quarter second at 16 kHz */
static u8 aud_ring[AUD_RING];
static volatile u32 aud_head, aud_tail;     /* free-running counts; used = head - tail */
static volatile u32 aud_rate = 16000;
static volatile int aud_flying;             /* an async transfer is in flight */
static volatile int aud_end;                /* the producer said the clip is complete */
static volatile int aud_blocking;           /* sb16_play/record owns the DMA buffer */
static volatile u32 aud_done;               /* bytes of finished transfers */
static volatile u32 aud_chunk_n, aud_chunk_t0;
static void aud_kick(void);
void sb16_irq(void) {
    (void)inb(DSP_RSTATUS);            /* 8-bit transfer acknowledge */
    if (aud_flying) {                  /* an async queue transfer finished: chain the next */
        aud_flying = 0;
        aud_done += aud_chunk_n;
        aud_kick();
        return;
    }
    irq_done = 1;
}

/* Programs one single-cycle output transfer of n bytes (1..64KB) already sitting in the DMA
   buffer and starts it. Does not wait: play_chunk waits, the async queue's IRQ chains. */
static int dma_start_out(u32 n, u32 rate) {
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
    return 1;
}

/* 1.9.26: the async queue SYS_AUDIO_PLAY feeds (contract in sb16.h). State shared with the IRQ
   is only touched with interrupts off (irq_save), or from the IRQ itself. */

/* Start the next transfer if none is in flight and there is a full chunk (or the tail of an
   ended clip). Caller has interrupts off. */
static void aud_kick(void) {
    if (aud_flying || aud_blocking || !present) return;
    u32 used = aud_head - aud_tail;
    if (!used) { aud_end = 0; return; }
    if (used < AUD_CHUNK && !aud_end) return;       /* wait for more, no underrun clicks */
    u32 n = used > AUD_CHUNK ? AUD_CHUNK : used;
    for (u32 i = 0; i < n; i++) sb16_dma_buf[i] = aud_ring[(aud_tail + i) & (AUD_RING - 1)];
    aud_tail += n;
    if (!dma_start_out(n, aud_rate)) { aud_tail = aud_head; return; }   /* DSP wedged: drop the clip */
    aud_chunk_n = n; aud_chunk_t0 = ticks(); aud_flying = 1;
}

unsigned int sb16_queue(const unsigned char *pcm, unsigned int len, unsigned int rate, int end) {
    if (!present) return 0;
    unsigned int f = irq_save();
    if (aud_blocking) { irq_restore(f); return 0; }
    if (!aud_flying && aud_head == aud_tail) {      /* idle: this call picks the rate */
        if (rate < 4000) rate = 4000;
        if (rate > 44100) rate = 44100;
        aud_rate = rate; aud_done = 0; aud_end = 0;
    }
    u32 space = AUD_RING - (aud_head - aud_tail);
    if (len > space) len = space;
    for (u32 i = 0; i < len; i++) aud_ring[(aud_head + i) & (AUD_RING - 1)] = pcm[i];
    aud_head += len;
    if (end) aud_end = 1;
    aud_kick();
    irq_restore(f);
    return len;
}

void sb16_queue_status(struct sb16_qstat *st) {
    unsigned int f = irq_save();
    st->present = (u32)present;
    st->playing = (u32)(aud_flying || aud_head != aud_tail);
    st->queued = aud_head - aud_tail;
    st->space = AUD_RING - st->queued;
    st->rate = aud_rate;
    u32 played = aud_done;
    if (aud_flying) {                               /* progress inside the current transfer */
        u32 in = (ticks() - aud_chunk_t0) * (aud_rate / 100u);
        played += in > aud_chunk_n ? aud_chunk_n : in;
    }
    st->played = played;
    irq_restore(f);
}

void sb16_queue_stop(void) {
    unsigned int f = irq_save();
    aud_tail = aud_head; aud_end = 0;               /* the transfer in flight finishes, nothing follows */
    irq_restore(f);
}

static int play_chunk(u32 n, u32 rate) {
    if (!dma_start_out(n, rate)) return 0;

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

static int sb16_play_locked(const unsigned char *pcm, unsigned int len, unsigned int rate);
int sb16_play(const unsigned char *pcm, unsigned int len, unsigned int rate) {
    if (!present || !pcm || !len) return 0;
    { unsigned int f = irq_save(); int busy = aud_flying || aud_head != aud_tail; if (!busy) aud_blocking = 1; irq_restore(f); if (busy) return 0; }
    int ok = sb16_play_locked(pcm, len, rate);
    aud_blocking = 0;
    return ok;
}
static int sb16_play_locked(const unsigned char *pcm, unsigned int len, unsigned int rate) {
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
    { unsigned int f = irq_save(); int busy = aud_flying || aud_head != aud_tail; if (!busy) aud_blocking = 1; irq_restore(f); if (busy) return 0; }
    unsigned int captured = 0;
    serial_puts("sb16: record start\n");
    while (captured < max_len) {
        u32 n = (max_len - captured) > DMA_CHUNK ? DMA_CHUNK : (max_len - captured);
        if (!record_chunk(n, rate)) break;
        for (u32 i = 0; i < n; i++) out[captured + i] = sb16_dma_buf[i];
        captured += n;
    }
    aud_blocking = 0;
    serial_puts("sb16: record done n="); put_uint_serial(captured); serial_puts("\n");
    return (int)captured;
}

int sb16_beep(unsigned int freq, unsigned int ms) {
    if (!present || !freq || aud_flying) return 0;
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
