#!/bin/sh
# MANUAL: never ran on the GitHub runner before 2026-09-23; promote to ci-suite.sh one at a time after three green runs on main.
# Regression test: DNS transaction ID validation in dns_resolve (drivers/net.c).
#
# Real bug, root-caused from a direct report: "Weather shows the city
# correctly but every other field is unavailable", on the real running OS
# with real QEMU networking (rtl8139 + SLIRP user net), not the browser/v86
# demo. weather_fetch does two DNS lookups back to back in one pass: first
# ip-api.com (inside geo_fetch, for the location/city), then
# api.open-meteo.com (for the actual forecast). dns_resolve always sent the
# exact same hardcoded transaction ID (0x1234) for every query it ever
# made, and accepted the first UDP packet arriving on src port 53 / dst
# port 53000 as the answer, with no check that its ID (or its question
# name) matched the query this particular call just sent.
#
# A late duplicate/retransmitted answer to the FIRST query (ordinary real
# DNS behavior under real network latency, which this sandbox's fast local
# SLIRP round trip does not reproduce, which is exactly why
# tools/checks/geo-check.sh passed clean here every single time it was
# run) arriving while the SECOND dns_resolve call is listening would be
# silently accepted as if it answered api.open-meteo.com, handing
# weather_fetch ip-api.com's real IP address instead. http_get then sends
# "Host: api.open-meteo.com" to ip-api.com's own server, gets back
# whatever that unrelated server does with the mismatched Host header
# (not real Open-Meteo JSON either way), the "current":{ search in
# json_current_number never matches, and weather_fetch returns before
# setting weather_have -- while geo_city is already populated and stays
# that way from the FIRST, genuinely successful ip-api.com fetch moments
# earlier. Exactly the reported symptom: city correct, everything
# downstream of it unavailable.
#
# Real fix (drivers/net.c): a real, per-call transaction ID (ticks()
# derived) instead of the fixed 0x1234, and a real check that a
# candidate answer's ID matches the ID this call actually sent, skipping
# (not accepting) anything that doesn't. This is a unit test of that
# acceptance logic, the same host-side-reimplementation pattern
# tools/checks/appsfolder-mousescroll-check.sh already uses for the wheel
# byte parser: build two synthetic DNS response packets, one with the
# right transaction ID (must be accepted) and one with a stray, wrong ID
# (must be rejected, not fed to the caller as if it were real), and
# assert both against the exact accept/reject shape dns_resolve now uses.
set -e
cd "$(dirname "$0")/../.."

TEST_C=$(mktemp /tmp/test-dns-txid-XXXXXX.c)
trap "rm -f $TEST_C /tmp/test-dns-txid" EXIT

cat > "$TEST_C" <<'EOTEST'
#include <stdio.h>
#include <string.h>
#include <stdint.h>

typedef uint8_t  u8;
typedef uint16_t u16;

static u16 htons_(u16 v) { return (u16)((v << 8) | (v >> 8)); }

/* Builds a minimal DNS response header (12 bytes: ID, flags, QDCOUNT,
   ANCOUNT, NSCOUNT, ARCOUNT), the only part dns_resolve's txid check
   looks at before deciding whether to keep parsing this packet at all. */
static void build_dns_header(u8 *dns, u16 id, u16 ancount) {
    u16 *h = (u16 *)dns;
    h[0] = htons_(id);
    h[1] = htons_(0x8180); /* standard response, recursion available */
    h[2] = htons_(1);      /* QDCOUNT */
    h[3] = htons_(ancount);
    h[4] = 0; h[5] = 0;
}

/* Replicates the exact acceptance check dns_resolve now runs on every
   candidate UDP packet: read the response's transaction ID and compare
   it against the ID this call actually sent, exactly the same
   "resp_id != txid -> continue" shape drivers/net.c uses. Returns 1 if
   this packet would be accepted as the real answer, 0 if it would be
   skipped as a stray/late answer to a different query. */
static int dns_response_accepted(const u8 *dns, u16 txid) {
    u16 resp_id = htons_(*(const u16 *)dns);
    if (resp_id != txid) return 0;
    return 1;
}

int main(void) {
    u8 dns[12];

    /* Test 1: a real answer to THIS query (matching ID) must be accepted.
       This is the ordinary case every existing geo-check.sh run already
       exercises against the real kernel. */
    build_dns_header(dns, 0x55AA, 1);
    if (!dns_response_accepted(dns, 0x55AA)) {
        fprintf(stderr, "FAIL: Test 1 - a correct-ID answer was rejected\n");
        return 1;
    }

    /* Test 2: the real bug's exact shape -- a stray answer to a DIFFERENT
       query (the previous call's leftover/duplicate answer) must be
       rejected, not silently accepted as this call's real result. Before
       the fix, dns_resolve never even looked at this field, so every
       call effectively used the pre-fix equivalent of "accept anything",
       which this same helper with a hardcoded ID would model as always
       returning 1 regardless of txid -- the real, broken shape that let
       weather_fetch's api.open-meteo.com lookup get handed back
       ip-api.com's leftover answer. */
    build_dns_header(dns, 0x1234, 1); /* the exact fixed ID every pre-fix call used */
    if (dns_response_accepted(dns, 0x9999)) {
        fprintf(stderr, "FAIL: Test 2 - a stray wrong-ID answer was accepted (the real bug's exact shape)\n");
        return 1;
    }

    /* Test 3: two back-to-back calls, modeling weather_fetch's real
       geo_fetch-then-forecast sequence, must not cross-accept each
       other's answers even when both arrive before either call's own
       check runs (the real race: a late duplicate of query 1's answer
       sitting in the queue when query 2 starts listening). */
    u16 txid1 = 0x2222, txid2 = 0x3333; /* ticks()-derived IDs differ call to call */
    build_dns_header(dns, txid1, 1);
    int accepted_for_query2 = dns_response_accepted(dns, txid2);
    if (accepted_for_query2) {
        fprintf(stderr, "FAIL: Test 3 - query 1's stale answer was accepted for query 2 (the exact weather_fetch race)\n");
        return 1;
    }
    build_dns_header(dns, txid2, 1);
    if (!dns_response_accepted(dns, txid2)) {
        fprintf(stderr, "FAIL: Test 3 - query 2's own real answer was rejected\n");
        return 1;
    }

    printf("PASS: all DNS transaction ID checks\n");
    return 0;
}
EOTEST

clang -o /tmp/test-dns-txid "$TEST_C" 2>&1 || { echo "FAIL: could not compile DNS txid test"; exit 1; }
/tmp/test-dns-txid || exit 1

# Second part: the real fix must actually be present in drivers/net.c, not
# just plausible in a reimplementation -- dns_resolve must (a) send a
# per-call ID instead of the old fixed 0x1234, and (b) check a candidate
# answer's ID before accepting it.
grep -q 'dns_build_query(query, hostname, 0x1234)' drivers/net.c && { echo "FAIL: drivers/net.c still sends the fixed 0x1234 transaction ID"; exit 1; }
grep -q 'resp_id != txid' drivers/net.c || { echo "FAIL: drivers/net.c does not validate the response transaction ID"; exit 1; }

# Third part: the kernel still boots clean with the real fix in place.
make -s kernel.elf
out=$( (sleep 2; echo 'xp /1xw 0x9000'; sleep 1; echo quit) \
       | qemu-system-i386 -kernel kernel.elf -display none -monitor stdio 2>&1 | tr '\r' '\n' )
echo "$out" | grep -qi '0xb007c0de' || { echo "FAIL: kernel did not reach gui_run"; exit 1; }

echo "PASS: DNS transaction ID validation verified and kernel boots cleanly"
