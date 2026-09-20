#!/bin/sh
# Exercise the same ramp function used by the kernel with controlled PIT ticks.
set -eu
cd "$(dirname "$0")/../.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/check.c" <<'EOF'
#include <stdio.h>
#include "kernel/dock_anim.h"
int main(void){
    int pos = 0, previous = 0, distinct = 0;
    for (int tick = 0; tick < 6; tick++) {
        pos = dock_anim_advance(pos, 9 * 16, 1, 9, 16, 6);
        if (pos / 16 != previous) distinct++;
        previous = pos / 16;
    }
    if (pos != 9 * 16 || distinct < 3) {
        fprintf(stderr, "FAIL: six-tick magnify ended at %d with %d sizes\n", pos, distinct);
        return 1;
    }
    pos = dock_anim_advance(0, 9 * 16, 6, 9, 16, 6);
    if (pos != 9 * 16) return 1;
    pos = dock_anim_advance(pos, 0, 6, 9, 16, 6);
    if (pos != 0) return 1;
    puts("PASS: controlled tick ramp shows intermediate sizes and reaches both targets");
    return 0;
}
EOF
${CC:-cc} -std=c11 -Wall -Wextra -Werror -I. "$tmp/check.c" -o "$tmp/check"
"$tmp/check"
