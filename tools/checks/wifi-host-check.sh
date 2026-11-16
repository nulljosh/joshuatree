#!/bin/sh
# Host-side test of arch/arm64/wifi_proto.h (the Wi-Fi driver's pure logic) with the host clang: SDPCM pack/parse
# round trip and the length-complement check, a BCDC reply, an escan result event with two BSS entries shaped like a
# brcmfmac trace, and NVRAM "key=value\0" packing with the trailer. Fails without the header, passes with it.
set -e
D=$(cd "$(dirname "$0")/../.." && pwd); T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
cat > "$T/t.c" <<'EOF'
#include <stdio.h>
#include <string.h>
#include "wifi_proto.h"
static int n; static char seen[64];
static void cb(int rssi, unsigned ch, const char *s, unsigned l) { n++; snprintf(seen, sizeof seen, "%d ch%u %.*s", rssi, ch, (int)l, s); }
int main(void) {
    unsigned char f[256], p[4] = {1,2,3,4}; unsigned off, len, ch;
    unsigned k = sdpcm_pack(f, 7, SDPCM_CONTROL, BCDC_GET_VAR, 9, 0, p, 4);
    if (k != 12 + 16 + 4 || sdpcm_parse(f, k, &off, &len) != SDPCM_CONTROL || off != 12 || len != 20) return 1;
    if (bcdc_reply(f + off, len, 9, &ch) != 0 || ch != 16 || f[off + 16] != 1) return 2;
    f[2] ^= 1; if (sdpcm_parse(f, k, &off, &len) != -1) return 3;   /* bad complement must be refused */
    unsigned char e[12 + 2 * 128] = {0}; wr16(e + 10, 2);
    for (int i = 0; i < 2; i++) { unsigned char *b = e + 12 + i * 128; wr32(b, 128); b[18] = 4; memcpy(b + 19, i ? "Shaw" : "Cafe", 4); wr16(b + 72, i ? 6 : 36); wr16(b + 78, (unsigned short)(i ? -51 : -70)); }
    if (escan_walk(e, sizeof e, cb) != 2 || n != 2 || strcmp(seen, "-51 ch6 Shaw")) { printf("%s\n", seen); return 4; }
    const char *nv = "# comment\nmacaddr=00:90:4c:c5:12:38\n\nboardrev=0x1301\n"; unsigned char o[128];
    unsigned m = nvram_pack(nv, strlen(nv), o, sizeof o);
    /* 26 + 16 bytes of entries, padded to 44, plus the 4-byte trailer: 11 words */
    if (m != 48 || memcmp(o, "macaddr=00:90:4c:c5:12:38", 26) || o[25] || rd32(o + 44) != (11u | (~11u & 0xffff) << 16)) return 5;
    if (fw_padded(3) != 4 || fw_padded(8) != 8) return 6;
    /* 7.5.4: a 1700-byte frame is read whole (it was left half read), a 2049-byte or broken one is terminated */
    unsigned char h[4]; memset(h, 0, 4); if (f2_rest(h) != 0) return 7;
    wr16(h, 40); wr16(h + 2, ~40u & 0xffff); if (f2_rest(h) != 0) return 8;
    wr16(h, 1700); wr16(h + 2, ~1700u & 0xffff); if (f2_rest(h) != 1636) return 9;
    wr16(h, 2048); wr16(h + 2, ~2048u & 0xffff); if (f2_rest(h) != 1984) return 10;
    wr16(h, 2049); wr16(h + 2, ~2049u & 0xffff); if (f2_rest(h) != -1) return 11;
    wr16(h, 1700); wr16(h + 2, 0x1234); if (f2_rest(h) != -1) return 12;
    /* 13.9.5: send credit, and the queue that keeps frames read while waiting for it */
    if (!credit_out(5, 5) || !credit_out(4, 5) || credit_out(6, 5) || credit_out(2, 250) || !credit_out(250, 2)) return 13;
    static struct rxq q; unsigned char fr[RXQ_MAX + 1], got[RXQ_MAX];
    if (rxq_get(&q, got, sizeof got) != 0) return 14;
    for (unsigned k = 0; k < RXQ_N; k++) { memset(fr, (int)k + 1, 100 + k); if (!rxq_put(&q, fr, 100 + k)) return 15; }
    if (rxq_put(&q, fr, 100) || q.lost != 1) return 16;                       /* full: the newest is dropped and counted */
    if (rxq_put(&q, fr, RXQ_MAX + 1) || rxq_put(&q, fr, 0) || q.lost != 3) return 17;   /* too long or empty never goes in */
    for (unsigned k = 0; k < RXQ_N; k++) { if (rxq_get(&q, got, sizeof got) != 100 + k || got[0] != k + 1 || got[99 + k] != k + 1) return 18; }   /* oldest first, whole */
    if (rxq_get(&q, got, sizeof got) != 0) return 19;
    memset(fr, 7, 300); rxq_put(&q, fr, 300); if (rxq_get(&q, got, 50) != 50 || q.count) return 20;   /* a short buffer gets what fits */
    unsigned char bd[40] = {0x20, 0, 0, 1, 9, 9, 9, 9, 0xAA, 0xBB}; const unsigned char *eth;
    if (bdc_eth(bd, 40, &eth) != 32 || eth[0] != 0xAA) return 21;             /* the data offset is in 4-byte words */
    if (bdc_eth(bd, 8, &eth) != 0 || bdc_eth(bd, 3, &eth) != 0) return 22;    /* nothing past the header: no frame */
    puts("wifi-host-check ok"); return 0;
}
EOF
clang -std=c11 -Wall -Wextra -Werror -I "$D/arch/arm64" -o "$T/t" "$T/t.c" && "$T/t"
