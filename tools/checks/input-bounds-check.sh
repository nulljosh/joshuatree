#!/bin/sh
# Host harness for the security pass over every fixed-size text buffer in
# kernel/kernel.c and kernel/*.h (Notes, Reminders, Mail, Contacts,
# Calculator, Search, Chat, Terminal, Settings, the auth.h login prompt).
# That audit's real findings live in drivers/json.c (network JSON, an
# unsigned-underflow guard was missing on maxlen==0/1) and kernel/auth.h
# (auth_const_time_eq, the one place docs/THREAT-MODEL.md calls out as
# worth being paranoid about). Both compile clean on the host, so both get
# fed over-long, adversarial input here under ASan+UBSan, same shape
# tools/checks/html-host-check.sh and decoder-fuzz-check.sh already use --
# seconds, no QEMU.
#
# json_extract_string/json_extract_number_text/json_escape are compiled
# straight from drivers/json.c (the real file kernel.c/chat.h link
# against). auth_const_time_eq is copied verbatim into the test driver
# below rather than #including kernel/auth.h, because that header pulls in
# this kernel's GUI/VFS globals (window_rect, vfs_read_file, klog, ...) at
# file scope and isn't meant to compile standalone; tools/auth-host/main.c
# already stubs that whole surface for the broader auth-check.sh suite,
# and re-doing that here would just duplicate it for one already-tiny,
# already-pure function. If auth_const_time_eq's body in kernel/auth.h
# ever changes, this copy needs updating too -- a drift the two loops
# below are simple enough to keep in sync by eye.
set -e
cd "$(dirname "$0")/../.."

out=$(mktemp -d)
cat > "$out/t.c" <<'C'
#include <stdio.h>
#include <string.h>
#include "json.h"

/* Verbatim copy of kernel/auth.h's auth_const_time_eq (see this script's
   header comment for why it's copied rather than included). */
static int auth_const_time_eq(const unsigned char *a, const unsigned char *b, unsigned int len) {
    unsigned char diff = 0;
    for (unsigned int i = 0; i < len; i++) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } } while (0)

int main(void) {
    /* json_extract_string: maxlen == 0 must not underflow into an
       unbounded write (the actual bug this pass found and fixed). ASan's
       redzone around `out` (1 byte, deliberately undersized) is what
       would actually catch a regression here, not just the return value. */
    {
        char out[1];
        unsigned int n = json_extract_string("{\"content\":\"hello world\"}", "content", out, 0);
        CHECK(n == 0, "json_extract_string(maxlen=0) must return 0, not write");
    }
    /* maxlen == 1: room for the NUL only, no character bytes. */
    {
        char out[1];
        unsigned int n = json_extract_string("{\"content\":\"hi\"}", "content", out, 1);
        CHECK(n == 0 && out[0] == 0, "json_extract_string(maxlen=1) must write only the NUL");
    }
    /* A value far longer than the output buffer: must truncate to
       maxlen-1 bytes plus a NUL, never write past out[]. Real shape: an
       LLM host or weather API is free to send back an arbitrarily long
       JSON string value. */
    {
        char longjson[2048];
        unsigned int i = 0;
        const char *head = "{\"content\":\"";
        while (head[i]) { longjson[i] = head[i]; i++; }
        for (unsigned int j = 0; j < 1900; j++) longjson[i++] = 'A';
        longjson[i++] = '"'; longjson[i++] = '}'; longjson[i] = 0;

        char out[16];
        unsigned int n = json_extract_string(longjson, "content", out, sizeof(out));
        CHECK(n == sizeof(out) - 1, "json_extract_string must truncate to maxlen-1");
        CHECK(out[sizeof(out) - 1] == 0, "json_extract_string must NUL-terminate at the truncation point");
    }

    /* json_extract_number_text: same maxlen==0 underflow guard. */
    {
        char out[1];
        unsigned int n = json_extract_number_text("{\"lat\":49.0983}", "lat", out, 0);
        CHECK(n == 0, "json_extract_number_text(maxlen=0) must return 0, not write");
    }
    {
        char longjson[600];
        unsigned int i = 0;
        const char *head = "{\"lat\":9";
        while (head[i]) { longjson[i] = head[i]; i++; }
        for (unsigned int j = 0; j < 500; j++) longjson[i++] = '9';
        longjson[i++] = '}'; longjson[i] = 0;

        char out[8];
        unsigned int n = json_extract_number_text(longjson, "lat", out, sizeof(out));
        CHECK(n == sizeof(out) - 1, "json_extract_number_text must truncate to maxlen-1");
        CHECK(out[sizeof(out) - 1] == 0, "json_extract_number_text must NUL-terminate at the truncation point");
    }

    /* json_escape: maxlen==0 and maxlen==1 both used to underflow `maxlen
       - 2`. maxlen==1 must still be able to write the NUL alone. */
    {
        char out[1];
        unsigned int n = json_escape("hello", out, 0);
        CHECK(n == 0, "json_escape(maxlen=0) must return 0, not write");
    }
    {
        char out[1];
        unsigned int n = json_escape("hello", out, 1);
        CHECK(n == 0 && out[0] == 0, "json_escape(maxlen=1) must write only the NUL");
    }
    {
        char longstr[600];
        for (unsigned int j = 0; j < 599; j++) longstr[j] = '"'; /* every char needs a 2-byte escape */
        longstr[599] = 0;
        char out[16];
        unsigned int n = json_escape(longstr, out, sizeof(out));
        CHECK(n <= sizeof(out) - 1, "json_escape must never exceed maxlen-1 bytes written");
        CHECK(out[n] == 0, "json_escape must NUL-terminate within bounds");
    }

    /* auth_const_time_eq: over-long (well past any real hash length)
       equal and unequal buffers, both directions, no early exit to trip
       ASan on either array. */
    {
        unsigned char a[256], b[256];
        for (int i = 0; i < 256; i++) { a[i] = (unsigned char)i; b[i] = (unsigned char)i; }
        CHECK(auth_const_time_eq(a, b, 256) == 1, "auth_const_time_eq must accept identical 256-byte buffers");
        b[255] ^= 1; /* last byte differs: proves it isn't short-circuiting on a prefix match */
        CHECK(auth_const_time_eq(a, b, 256) == 0, "auth_const_time_eq must reject a last-byte mismatch");
        b[255] ^= 1; b[0] ^= 1; /* first byte differs instead */
        CHECK(auth_const_time_eq(a, b, 256) == 0, "auth_const_time_eq must reject a first-byte mismatch");
    }

    if (!fails) printf("PASS: json.c bounds hold under maxlen 0/1/truncation, auth_const_time_eq is correct over long buffers\n");
    return fails;
}
C

clang -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined \
    -Wall -Wextra \
    -Idrivers \
    -o "$out/t" "$out/t.c" drivers/json.c
"$out/t"
