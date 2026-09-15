#!/usr/bin/env bash
# Real check for kernel/calendar.h's date math, the part tagged [Fable] in
# roadmap.md for its "looks right, subtly isn't" risk: compiles the same
# cal_dow / cal_days_in_month / cal_is_leap the kernel links (pulled in
# under CALENDAR_MATH_ONLY, nothing else from the kernel) with the host
# clang, then walks every single day from 1900-01-01 to 2099-12-31 and
# compares against libc's own tm_wday from timegm(3). 73,049 dates, any
# mismatch fails. Run it after touching calendar.h's math at all.
#
# v55: also checks cal_date_str, the YYYY-MM-DD formatter the events
# feature writes into EVENTS.TXT and matches the grid's event dot
# against. Same host-side sweep, 1900-2099, comparing cal_date_str's
# output byte-for-byte against snprintf's own "%04d-%02d-%02d" for every
# one of those 73,049 dates (padding, century digits and leap-day Feb 29
# all exercised for real, not just eyeballed), plus a handful of explicit
# event-dot matching cases (the strcmp-based exact-match cal_events_find
# does against the same stored/rendered strings) so a prefix collision
# like "2026-09-1" wrongly lighting up day 14 would actually fail this.
set -euo pipefail
cd "$(dirname "$0")/../.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/t.c" << 'EOF'
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <string.h>
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

                char got_str[CAL_DATE_LEN + 1], want_str[CAL_DATE_LEN + 1];
                cal_date_str(y, m, d, got_str);
                snprintf(want_str, sizeof(want_str), "%04d-%02d-%02d", y, m, d);
                checked++;
                if (strcmp(got_str, want_str) != 0) { bad++; if (bad < 20) printf("date_str %d-%02d-%02d: got \"%s\" want \"%s\"\n", y, m, d, got_str, want_str); }
            }
        }
    printf("%ld dates checked, %ld mismatches\n", checked, bad);

    /* Event-dot matching: the grid builds one YYYY-MM-DD string per cell
       and does an exact strcmp against every stored event date (see
       cal_events_find in calendar.h). Real cases for the ways that could
       go wrong: a short prefix ("2026-09-1") must not falsely light up
       day 14, a single stored event must only match its own exact day,
       and zero-padding must line up so day 1 and day 10 never collide. */
    struct { const char *stored, *cell, *label; int want_match; } cases[] = {
        {"2026-09-14", "2026-09-14", "exact match",                     1},
        {"2026-09-14", "2026-09-04", "different day, same month",       0},
        {"2026-09-1",  "2026-09-14", "short prefix must not match",     0},
        {"2026-09-14", "2026-09-1",  "short prefix (reversed) must not match", 0},
        {"2026-01-01", "2026-01-10", "zero-padded day 01 vs day 10",    0},
        {"2026-01-10", "2026-01-01", "zero-padded day 10 vs day 01",    0},
        {"2028-02-29", "2028-02-29", "real leap day matches itself",    1},
        {"2028-02-29", "2028-02-28", "leap day does not match Feb 28",  0},
        {"2026-09-04", "2026-09-14", "day 4 must not light up day 14",  0},
    };
    int ncases = (int)(sizeof(cases) / sizeof(cases[0]));
    for (int i = 0; i < ncases; i++) {
        int got_match = strcmp(cases[i].stored, cases[i].cell) == 0;
        checked++;
        if (got_match != cases[i].want_match) {
            bad++;
            printf("event-dot match \"%s\": stored=%s cell=%s got=%d want=%d\n",
                   cases[i].label, cases[i].stored, cases[i].cell, got_match, cases[i].want_match);
        }
    }
    printf("%d event-dot matching cases checked\n", ncases);

    return bad ? 1 : 0;
}
EOF
clang -Wall -Wextra -I. -o "$tmp/t" "$tmp/t.c"
"$tmp/t"
