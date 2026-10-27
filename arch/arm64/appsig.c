/* appsig.c (6.54.0): does NAME.SIG sign this app? See appsig.h. BearSSL does the hash and the ECDSA check; nothing else here
   reads the signature, so tools/fuzz-host/fuzz_appsig.c can run this file on the host as the kernel runs it. 1 it does. */
#include "bearssl.h"
#include "appsig.h"
#ifndef APP_KEY_H
#define APP_KEY_H "app_key.h"
#endif
#include APP_KEY_H   /* app_key[65]: 0x04, then X and Y */

int appsig_ok(const char *name, unsigned n, const void *img, unsigned long len, const unsigned char *sig, unsigned siglen) {
    if (!n || n > 8 || siglen < APPSIG_MIN || siglen > APPSIG_MAX) return 0;
    br_sha256_context h;
    unsigned char d[32];
    br_sha256_init(&h);
    br_sha256_update(&h, "JTAPP1", 7);   /* the NUL too */
    br_sha256_update(&h, name, n);
    br_sha256_update(&h, "", 1);
    br_sha256_update(&h, img, len);
    br_sha256_out(&h, d);
    br_ec_public_key pk = { BR_EC_secp256r1, (unsigned char *)app_key, sizeof app_key };
    return br_ecdsa_i31_vrfy_asn1(&br_ec_all_m31, d, sizeof d, &pk, sig, siglen) == 1;
}
