#ifndef ARM64_RNG_H
#define ARM64_RNG_H
/* Fill every byte or wipe the output and fail. Pi uses RNG200; virt is test-only. */
int tls_entropy(unsigned char *out, unsigned n);
#endif
