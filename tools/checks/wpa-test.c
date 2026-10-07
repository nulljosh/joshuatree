/* Host test for arch/arm64/wpa.h, the WPA2 handshake crypto: SHA-1 ("abc"), HMAC-SHA1 (RFC 2202 case 1), the 802.11
   pairwise-key PRF (checked against Python's hmac), and AES key unwrap (RFC 3394 section 4.1).
   cc -O1 -o /tmp/wpa-test tools/checks/wpa-test.c && /tmp/wpa-test */
#include <stdio.h>
#include <string.h>
#include "../../arch/arm64/wpa.h"
static int hex_is(const unsigned char *b, unsigned n, const char *want, const char *what) {
    char got[256]; for (unsigned i = 0; i < n; i++) sprintf(got + 2 * i, "%02x", b[i]);
    int ok = strcmp(got, want) == 0;
    printf("%s: %s\n", ok ? "PASS" : "FAIL", what); if (!ok) printf("  got  %s\n  want %s\n", got, want);
    return ok;
}
int main(void) {
    int ok = 1; unsigned char out[48];
    wpa_sha1 c; wpa_sha1_init(&c); wpa_sha1_add(&c, (const unsigned char *)"abc", 3); wpa_sha1_end(&c, out);
    ok &= hex_is(out, 20, "a9993e364706816aba3e25717850c26c9cd0d89d", "SHA-1 of abc");
    unsigned char k[20]; memset(k, 0x0b, 20);
    wpa_hmac(k, 20, (const unsigned char *)"Hi There", 8, 0, 0, out);
    ok &= hex_is(out, 20, "b617318655057264e28bc0b6fb378c8ef146be00", "HMAC-SHA1, RFC 2202 case 1");
    unsigned char pmk[32], aa[6] = {0, 1, 2, 3, 4, 5}, spa[6] = {0, 1, 2, 3, 4, 6}, an[32], sn[32];
    for (int i = 0; i < 32; i++) pmk[i] = (unsigned char)i;
    memset(an, 7, 32); memset(sn, 9, 32);
    wpa_ptk(pmk, aa, spa, an, sn, out);
    ok &= hex_is(out, 48, "0d77f8ed14b269b9ecd9434b913ccdc273fbb1894d360d3463e649e2c566333b65ab3933afa17d45388a5bc4a926e06a", "pairwise keys (PRF-384)");
    unsigned char kek[16], ct[24] = {0x1F,0xA6,0x8B,0x0A,0x81,0x12,0xB4,0x47,0xAE,0xF3,0x4B,0xD8,0xFB,0x5A,0x7B,0x82,0x9D,0x3E,0x86,0x23,0x71,0xD2,0xCF,0xE5};
    for (int i = 0; i < 16; i++) kek[i] = (unsigned char)i;
    int good = wpa_unwrap(kek, ct, 24, out);
    printf("%s: unwrap check value\n", good ? "PASS" : "FAIL"); ok &= good;
    ok &= hex_is(out, 16, "00112233445566778899aabbccddeeff", "AES key unwrap, RFC 3394 4.1");
    return ok ? 0 : 1;
}
