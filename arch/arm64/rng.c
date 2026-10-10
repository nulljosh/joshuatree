#include "rng.h"

#ifdef PI_BUILD
/* BCM2711 RNG200 registers; use the free-running counter, not interrupt ticks. */
#ifdef RNG_HOST_TEST
unsigned rng_read(unsigned offset);
void rng_write(unsigned offset, unsigned value);
unsigned long rng_counter(void);
unsigned long rng_frequency(void);
#else
static unsigned rng_read(unsigned offset) { return *(volatile unsigned *)(0xFE104000UL + offset); }
static void rng_write(unsigned offset, unsigned value) { *(volatile unsigned *)(0xFE104000UL + offset) = value; }
static unsigned long rng_counter(void) { unsigned long v; __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v)); return v; }
static unsigned long rng_frequency(void) { unsigned long v; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v)); return v; }
#endif
int tls_entropy(unsigned char *out, unsigned n) {
    unsigned done = 0;
    unsigned long start = rng_counter(), limit = rng_frequency();
    if (!n) return 1;
    if (!limit) goto fail;
    rng_write(0, (rng_read(0) & ~0x1FFFU) | 1U);
    while (done < n) {
        if (rng_counter() - start >= limit || (rng_read(0x18) & 0x80000020U)) goto fail;
        if (!(rng_read(0x24) & 0xFFU)) continue;
        unsigned word = rng_read(0x20);
        /* Reject a health fault raised while the word was being read. */
        if (rng_read(0x18) & 0x80000020U) goto fail;
        for (unsigned i = 0; i < 4 && done < n; i++, done++) out[done] = (unsigned char)(word >> (8 * i));
    }
    return 1;
fail:
    for (unsigned i = 0; i < n; i++) ((volatile unsigned char *)out)[i] = 0;
    return 0;
}
#else
/* shortcut: virt's timer seed is only for QEMU checks; never link this object into a Pi image. */
int tls_entropy(unsigned char *out, unsigned n) {
    unsigned long c, mix = 0x9E3779B97F4A7C15UL;
    for (unsigned i = 0; i < n; i++) {
        __asm__ volatile("mrs %0, cntpct_el0" : "=r"(c));
        mix = (mix ^ c) * 0x100000001B3UL;
        for (unsigned k = 0; k < (unsigned)(c & 63); k++) __asm__ volatile("" ::: "memory");
        out[i] = (unsigned char)(mix >> 56);
    }
    return 1;
}
#endif
