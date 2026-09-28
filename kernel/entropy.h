/* Kernel entropy pool: the one place the kernel gets unpredictable bytes.
   HMAC_DRBG (SHA-256, vendored BearSSL under third_party/bearssl/, never a
   homemade primitive) seeded at boot from RDRAND when CPUID says the CPU
   has it, RDTSC jitter samples, and the PIT tick count, then topped up
   with the RDTSC stamp of every timer and keyboard interrupt. QEMU's
   default CPU has no RDRAND, so the jitter path stands on its own.

   Boot logs one serial line, "entropy: sources=...", naming what was
   actually available; tools/checks/entropy-check.py asserts on it.
   Password salts in kernel/auth.h are the first consumer. */
#ifndef ENTROPY_H
#define ENTROPY_H

/* Seed the DRBG and log the sources. Safe to call before interrupts are
   on; entropy_bytes() calls it lazily if nothing has yet. */
void entropy_init(void);

/* Fill buf with n unpredictable bytes. Reseeds from the interrupt pool
   and fresh jitter before every draw. */
void entropy_bytes(void *buf, unsigned int n);

/* Called from irq_handler for IRQ0/IRQ1: folds the current RDTSC stamp
   into the pool. Cheap, no hashing on the interrupt path. */
void entropy_irq_sample(unsigned int irq_no);

/* Bit flags of entropy_sources after entropy_init(). */
#define ENTROPY_SRC_RDRAND 1u
#define ENTROPY_SRC_JITTER 2u
#define ENTROPY_SRC_TICKS  4u
extern unsigned int entropy_sources;

/* First 16 bytes drawn at boot, kept for tools/checks/entropy-check.py to
   read out of guest memory and compare across two boots. Never reused. */
extern unsigned char entropy_boot_sample[16];

#endif
