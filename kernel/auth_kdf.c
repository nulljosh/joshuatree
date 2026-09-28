/* PBKDF2-HMAC-SHA256 (RFC 8018 section 5.2) for kernel/auth.h. The HMAC
   and SHA-256 are BearSSL's, vendored under third_party/bearssl and
   already linked for kernel/entropy.c's DRBG; this file adds only the
   PBKDF2 block loop on top of br_hmac, never a primitive of its own.

   Proven against the published SHA-256 vectors (the RFC 7914 section 11
   pair and the RFC 6070 set recomputed for SHA-256) by tools/auth-host,
   run by tools/checks/auth-check.sh. Compiled both for the kernel and
   natively for that harness, so the exact bytes that ship are the ones
   the vectors cover. */
#include "auth_kdf.h"
#include "libc.h"
#include "bearssl_hash.h"
#include "bearssl_hmac.h"

void auth_pbkdf2_sha256(const void *password, unsigned int password_len,
                        const void *salt, unsigned int salt_len,
                        unsigned int iterations,
                        unsigned char *out, unsigned int out_len) {
    br_hmac_key_context kc;
    br_hmac_key_init(&kc, &br_sha256_vtable, password, password_len);
    if (iterations == 0) iterations = 1;
    unsigned int block = 1;
    while (out_len > 0) {
        unsigned char u[32], t[32];
        unsigned char be[4] = { (unsigned char)(block >> 24), (unsigned char)(block >> 16),
                                (unsigned char)(block >> 8), (unsigned char)block };
        br_hmac_context hc;
        br_hmac_init(&hc, &kc, 0);
        br_hmac_update(&hc, salt, salt_len);
        br_hmac_update(&hc, be, 4);
        br_hmac_out(&hc, u);
        memcpy(t, u, 32);
        for (unsigned int i = 1; i < iterations; i++) {
            br_hmac_init(&hc, &kc, 0);
            br_hmac_update(&hc, u, 32);
            br_hmac_out(&hc, u);
            for (int j = 0; j < 32; j++) t[j] ^= u[j];
        }
        unsigned int take = out_len < 32 ? out_len : 32;
        memcpy(out, t, take);
        out += take;
        out_len -= take;
        block++;
    }
    /* kc holds the HMAC-padded password. Wipe through a volatile pointer
       so -O2 cannot drop the store as dead (auth.h learned this the hard
       way with its message buffer). */
    { volatile unsigned char *vp = (volatile unsigned char *)&kc; for (unsigned int i = 0; i < sizeof(kc); i++) vp[i] = 0; }
}
