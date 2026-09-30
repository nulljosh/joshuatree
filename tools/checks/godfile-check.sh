#!/usr/bin/env bash
# God-file guard. Keeps hand-written .c/.h files under kernel/, drivers/,
# lib/ and user/ from silently growing into another kernel.c: 10,048 lines
# in one translation unit before this check existed, with many code-bearing
# headers #included straight into it.
#
# Rule: any hand-written file over LIMIT lines fails, unless it's in the
# EXEMPT list (generated data, not something a person edits by hand) or the
# RATCHET list (already over LIMIT when this check landed; those get a
# named ceiling at their current size, and the ceiling may only go down as
# they're split, never up).
set -euo pipefail
cd "$(dirname "$0")/../.."

LIMIT=2000

# Generated data headers: pixel/font/test data written by a tools/gen
# script, not something a person hand-edits line by line. No line-count
# ceiling applies to these.
EXEMPT=(
    "drivers/wallpaper.h"
    "drivers/wall_sat.h"
    "kernel/wall_sat.h"
    "kernel/boot_mark.h"
    "drivers/editor_fonts.h"
    "kernel/icon_art.h"
    "drivers/icon_art.h"
    "drivers/stb_truetype.h"
    "drivers/png_testdata.h"
    "drivers/jpeg_testdata.h"
    "drivers/dejavu_font.h"
    "drivers/dejavu_bold_font.h"
    "drivers/dejavu_serif_font.h"
    "drivers/dejavu_serif_bold_font.h"
    "drivers/dejavu_mono_font.h"
    "drivers/dejavu_mono_bold_font.h"
)

# Ratchet: files already over LIMIT the day this check landed. Ceiling is
# their line count that day. May shrink, must never grow past the number
# listed here. (Plain case statement, not an associative array: this
# script must run under macOS's stock bash 3.2, which has no `declare -A`.)
ratchet_ceiling() {
    case "$1" in
        # 2026-09-27: 1.7.0's integration release deliberately merged two
        # old draft PRs (#219 Clock, #257 app switcher + screenshot key)
        # on top of the 9620 ceiling guard-checks (#250) set earlier the
        # same night, real feature lines, not silent bloat. Ratcheted up
        # once to the merged total; still only shrinks from here.
        # 2026-09-28: 1.7.4's integration release deliberately merged
        # three more draft PRs (#258 DHCP, #259 drunk mode, #260 voice-in)
        # on top of that 9841 ceiling, each adding real kmain command-line
        # handling and feature code, not silent bloat. Ratcheted up once
        # more to the merged total; still only shrinks from here.
        # 2026-09-28: dock band/tray/icon drawing moved to
        # kernel/dock_draw.c/.h (refactor/kernel-split-1). Ceiling lowered
        # to match, never to go back up.
        # 2026-09-30: 1.9.12 moved Calendar to ring 3 (user/calendar.c).
        # 2026-09-30: 1.9.13 dropped the search.h include. Down one more.
        kernel/kernel.c) echo 9354 ;;
        *) echo "$LIMIT" ;;
    esac
}

is_exempt() {
    local f="$1"
    for e in "${EXEMPT[@]}"; do
        [ "$f" = "$e" ] && return 0
    done
    case "$f" in
        *_font.h|*_testdata.h) return 0 ;;
    esac
    return 1
}

fail=0
while IFS= read -r -d '' f; do
    is_exempt "$f" && continue
    lines=$(wc -l < "$f" | tr -d ' ')
    ceiling="$(ratchet_ceiling "$f")"
    if [ "$lines" -gt "$ceiling" ]; then
        echo "FAIL: $f is $lines lines, over its ceiling of $ceiling"
        fail=1
    fi
done < <(find kernel drivers lib user -type f \( -name '*.c' -o -name '*.h' \) -print0 2>/dev/null)

if [ "$fail" -eq 0 ]; then
    echo "godfile-check: OK, all hand-written files under their ceiling"
fi
exit $fail
