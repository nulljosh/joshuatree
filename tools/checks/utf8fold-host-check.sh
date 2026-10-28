#!/bin/sh
# Host harness for arch/arm64/utf8.h (6.78.1): the Terminal draws a byte as its Latin-1 character, so a program's
# UTF-8 is folded on the way to the screen. "héllo, 世界" printed "hÃ©llo, ä¸..." before.
set -e
cd "$(dirname "$0")/../.."
out=$(mktemp -d)
cat > "$out/t.c" <<'C'
#include <stdio.h>
#include <string.h>
#include "utf8.h"
static int check(const char *in, const char *want) {
    char buf[64]; unsigned st = 0, n = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) { int b = utf8_fold(&st, *p); if (b >= 0) buf[n++] = (char)b; }
    buf[n] = 0;
    if (strcmp(buf, want)) { printf("FAIL: %s -> \"%s\"\n", in, buf); return 1; }
    return 0;
}
int main(void) {
    int f = 0;
    f |= check("plain ascii", "plain ascii");
    f |= check("h\xc3\xa9llo, \xe4\xb8\x96\xe7\x95\x8c", "h\xe9llo, ??");   /* é is Latin-1's 0xE9; 世界 is past it */
    f |= check("25\xc2\xb0" "C \xf0\x9f\x8c\xb2!", "25\xb0" "C ?!");      /* a degree sign, then a four-byte emoji */
    f |= check("a\xa9" "b", "a?b");                                         /* a stray continuation byte */
    f |= check("x\xc3y", "xy");                                             /* a sequence cut short by an ASCII byte */
    if (!f) printf("PASS: the Terminal folds UTF-8 to Latin-1 cells: é stays é, past Latin-1 is ?, broken bytes never show raw\n");
    return f;
}
C
cc -Wall -Iarch/arm64 -o "$out/t" "$out/t.c"
"$out/t"
