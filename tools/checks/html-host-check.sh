#!/bin/sh
# Host harness for drivers/html.c's entity decoding: named entities become
# their closest ASCII and an unknown one is dropped, never printed raw
# (Plan's page showed "&middot;" on screen).
set -e
cd "$(dirname "$0")/../.."
out=$(mktemp -d)
cat > "$out/t.c" <<'C'
#include <stdio.h>
#include <string.h>
#include "html.h"
static int check(const char *in, const char *want) {
    char buf[256]; html_to_text(in, buf, sizeof buf);
    if (strcmp(buf, want)) { printf("FAIL: %s -> \"%s\", want \"%s\"\n", in, buf, want); return 1; }
    return 0;
}
int main(void) {
    int f = 0;
    f |= check("<p>Joshua Trommel &middot; The next ten years</p>", "Joshua Trommel - The next ten years");
    f |= check("a &lt;b&gt; &amp; &quot;c&quot;", "a <b> & \"c\"");
    f |= check("it&rsquo;s&nbsp;fine&hellip;", "it's fine.");
    f |= check("x &zwnj; y", "x  y");
    f |= check("AT&T & co", "AT&T & co");
    if (!f) printf("PASS: html entities decode to ASCII, unknown ones are dropped, bare & survives\n");
    return f;
}
C
cc -Idrivers -o "$out/t" "$out/t.c" drivers/html.c
"$out/t"
