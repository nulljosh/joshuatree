#!/usr/bin/env bash
# Real check for kernel/calendar.h's date math, the part tagged [Fable] in
# roadmap.md for its "looks right, subtly isn't" risk: compiles the same
# cal_dow / cal_days_in_month / cal_is_leap the kernel links (pulled in
# under CALENDAR_MATH_ONLY, nothing else from the kernel) with the host
# clang, then walks every single day from 1900-01-01 to 2099-12-31 and
# compares against libc's own tm_wday from timegm(3). 73,049 dates, any
# mismatch fails. Run it after touching calendar.h's math at all.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/t.c" << 'EOF'
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <time.h>
#define CALENDAR_MATH_ONLY
#include "kernel/calendar.h"
int main(void){
    long checked = 0, bad = 0;
    for (int y = 1900; y <= 2099; y++)
        for (int m = 1; m <= 12; m++) {
            struct tm probe = {0};
            probe.tm_year = y - 1900; probe.tm_mon = m; probe.tm_mday = 0; /* day 0 of next month = last day of this one */
            timegm(&probe);
            if (probe.tm_mday != cal_days_in_month(y, m)) { bad++; printf("days_in_month %d-%02d: got %d want %d\n", y, m, cal_days_in_month(y, m), probe.tm_mday); }
            for (int d = 1; d <= cal_days_in_month(y, m); d++) {
                struct tm t = {0};
                t.tm_year = y - 1900; t.tm_mon = m - 1; t.tm_mday = d;
                timegm(&t);
                int want = t.tm_wday, got = cal_dow(y, m, d);
                checked++;
                if (want != got) { bad++; if (bad < 20) printf("dow %d-%02d-%02d: got %d want %d\n", y, m, d, got, want); }
            }
        }
    printf("%ld dates checked, %ld mismatches\n", checked, bad);
    return bad ? 1 : 0;
}
EOF
clang -Wall -Wextra -I. -o "$tmp/t" "$tmp/t.c"
"$tmp/t"
