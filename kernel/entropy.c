/* See entropy.h. The DRBG is BearSSL's br_hmac_drbg over br_sha256; this
   file only gathers raw material and hands it over. */
#include "entropy.h"
#include "irq.h"
#include "serial.h"
#include "libc.h"
#include "bearssl_rand.h"
#include "bearssl_hash.h"

unsigned int entropy_sources = 0;
unsigned char entropy_boot_sample[16];

static br_hmac_drbg_context drbg;
static int drbg_ready = 0;

/* Interrupt pool: 32 RDTSC low words, one slot per interrupt, xor-folded
   so a slot is never simply overwritten. Read and then zeroed at reseed. */
#define IRQ_POOL_WORDS 32
static volatile unsigned int irq_pool[IRQ_POOL_WORDS];
static volatile unsigned int irq_pool_pos = 0;
static volatile unsigned int irq_pool_count = 0;

static inline unsigned int rdtsc_lo(void) {
    unsigned int lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return lo;
}

void entropy_irq_sample(unsigned int irq_no) {
    unsigned int pos = irq_pool_pos;
    irq_pool[pos] ^= rdtsc_lo() ^ (irq_no << 28);
    irq_pool_pos = (pos + 1) % IRQ_POOL_WORDS;
    irq_pool_count++;
}

/* CPUID.1:ECX bit 30 is RDRAND. CPUID itself exists on every i386 QEMU
   models or any real machine this kernel boots on (i586 and up). */
static int cpu_has_rdrand(void) {
    unsigned int eax, ebx, ecx, edx;
    __asm__ volatile ("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1), "c"(0));
    (void)eax; (void)ebx; (void)edx;
    return (ecx >> 30) & 1;
}

/* rdrand %eax, encoded as bytes so the assembler never needs -mrdrnd.
   Carry flag set means a valid word; retry a few times per the SDM. */
static int rdrand32(unsigned int *out) {
    for (int tries = 0; tries < 10; tries++) {
        unsigned int v; unsigned char ok;
        __asm__ volatile (".byte 0x0f, 0xc7, 0xf0\n\tsetc %1" : "=a"(v), "=qm"(ok));
        if (ok) { *out = v; return 1; }
    }
    return 0;
}

/* One jitter sample: time a short, data-dependent loop with RDTSC. The
   low bits of the delta move with cache state, host scheduling under
   QEMU and interrupt arrival, none of which a caller can replay. */
static unsigned int jitter_sample(unsigned int salt) {
    unsigned int t0 = rdtsc_lo();
    volatile unsigned int sink = salt;
    for (unsigned int i = 0; i < (t0 & 0x3f) + 16; i++) sink = sink * 2654435761u + i;
    (void)sink;
    return rdtsc_lo() - t0;
}

static void gather_jitter(unsigned char *out, unsigned int n) {
    /* Each output byte is the xor of eight deltas' low bytes, so a few
       degenerate samples cannot flatten the whole thing. */
    for (unsigned int i = 0; i < n; i++) {
        unsigned char acc = 0;
        for (int k = 0; k < 8; k++) acc ^= (unsigned char)jitter_sample(i * 8 + k);
        out[i] = acc;
    }
}

void entropy_init(void) {
    if (drbg_ready) return;
    unsigned char seed[32 + 64 + 8];
    unsigned int len = 0;
    entropy_sources = 0;

    if (cpu_has_rdrand()) {
        unsigned int got = 0;
        for (int i = 0; i < 8; i++) {
            unsigned int w;
            if (!rdrand32(&w)) break;
            memcpy(seed + len, &w, 4); len += 4; got++;
        }
        if (got == 8) entropy_sources |= ENTROPY_SRC_RDRAND;
    }

    gather_jitter(seed + len, 64); len += 64;
    entropy_sources |= ENTROPY_SRC_JITTER;

    { unsigned int t = ticks(), c = rdtsc_lo();
      memcpy(seed + len, &t, 4); memcpy(seed + len + 4, &c, 4); len += 8;
      if (t) entropy_sources |= ENTROPY_SRC_TICKS; }

    br_hmac_drbg_init(&drbg, &br_sha256_vtable, seed, len);
    drbg_ready = 1;
    { volatile unsigned char *vp = seed; for (unsigned int i = 0; i < sizeof(seed); i++) vp[i] = 0; }

    serial_puts("entropy: sources=");
    if (entropy_sources & ENTROPY_SRC_RDRAND) serial_puts("rdrand,");
    serial_puts("jitter");
    if (entropy_sources & ENTROPY_SRC_TICKS) serial_puts(",ticks");
    serial_puts(" irq\n");

    entropy_bytes(entropy_boot_sample, sizeof(entropy_boot_sample));
}

void entropy_bytes(void *buf, unsigned int n) {
    if (!drbg_ready) entropy_init();
    /* Fold in everything new since the last draw: the interrupt pool,
       its count, a fresh burst of jitter and the current TSC. */
    unsigned char extra[IRQ_POOL_WORDS * 4 + 16 + 8];
    for (unsigned int i = 0; i < IRQ_POOL_WORDS; i++) {
        unsigned int w = irq_pool[i]; irq_pool[i] = 0;
        memcpy(extra + i * 4, &w, 4);
    }
    gather_jitter(extra + IRQ_POOL_WORDS * 4, 16);
    { unsigned int c = irq_pool_count, t = rdtsc_lo();
      memcpy(extra + IRQ_POOL_WORDS * 4 + 16, &c, 4);
      memcpy(extra + IRQ_POOL_WORDS * 4 + 20, &t, 4); }
    br_hmac_drbg_update(&drbg, extra, sizeof(extra));
    br_hmac_drbg_generate(&drbg, buf, n);
}
