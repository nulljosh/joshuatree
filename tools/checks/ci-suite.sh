#!/bin/bash
# The whole regression suite, run as one thing, reporting everything.
#
# Why this file exists. check.yml used to list ~40 checks as ~40 separate
# workflow steps, which meant the job stopped at the first failure and
# every check after it never ran. On a suite this size that is actively
# misleading: a single timing flake in step 12 hides the real state of the
# other 28, so "CI is red" told you nothing about how red, and a genuine
# regression could sit behind a flake for days. Worse, fixing the flake and
# pushing only revealed the NEXT failure, one run (and six minutes) at a
# time.
#
# So: every check runs, every result is recorded, the summary at the end
# lists all of them, and the exit code is failure if any failed. One run
# now tells you the complete picture.
#
# Retries. Checks marked `retry` in the manifest below get exactly one
# automatic re-run before being called a failure, and only those. The line
# between the two groups is not "flaky things we want to pass" -- it is
# whether the check drives a real QEMU guest through wall-clock waits, in
# which case a shared, loaded runner can genuinely be too slow through no
# fault of the kernel, or whether it is a pure host/static check whose
# result is a function of the tree alone and cannot legitimately differ
# between two runs a second apart. A static check that fails twice is not
# flaky, it is broken, so retrying it would only hide a real regression.
# A retry that succeeds is still printed as FLAKY in the summary, never
# silently laundered into a pass, so a check that starts needing its retry
# regularly is visible and can be fixed properly.
#
# Adding a check: one line in the manifest. Removing one: delete its line.
# Nothing in .github/ needs to change either way.

set -uo pipefail
cd "$(dirname "$0")/../.."

# retry?  name                                                          command
manifest() {
cat <<'EOF'
once |Dock slot constants agree with kernel.c (static drift guard)|python3 ./tools/checks/dockslots-check.py
retry|Boot check|./check.sh
retry|ISO boot (CD-ROM and USB/raw-disk paths)|./tools/checks/iso-boot-check.sh
once |PNG decoder, host harness|./tools/checks/png-host-check.sh
retry|PNG decoder, in-kernel|./tools/checks/png-check.sh
retry|AA text spacing, in-kernel|./tools/checks/textspacing-check.sh
retry|AA text stems are dense but still antialiased|python3 ./tools/checks/textsharp-check.py
retry|FAT filesystem cycle hang (regression test)|./tools/checks/fatcyclehang-check.sh
retry|Chat history (VFS-backed, past the old 512-byte cap)|./tools/checks/chat-check.sh
retry|No-disk boot falls back to ramfs with seeded demo files|./tools/checks/ramfs-demo-check.sh
retry|Shell regression suite (heap, task, preempt, kill, ring3, ps)|./tools/checks/shellregress-check.sh
retry|Ring-3 reference program against the v1 syscall ABI|./tools/checks/usertest-check.sh
retry|Ring-3 program writing a real file against the v2 syscall ABI|./tools/checks/notetest-check.sh
retry|Shell launches a ring-3 program by bare name, case-insensitively|./tools/checks/shellname-check.sh
retry|QEMU vmmouse absolute-pointer round trip|./tools/checks/vmmouse-check.sh
once |Calendar date math, host harness|./tools/checks/check-calendar.sh
retry|Settings click acts on the row actually clicked|./tools/checks/settingsclick-check.sh
retry|Wallpaper defaults to Satellite on a fresh boot|./tools/checks/walldefault-check.sh
retry|Idle tour's Settings visit doesn't change the wallpaper theme|python3 ./tools/checks/walldemo-regression-check.py
retry|Notes editor chrome doesn't redraw on plain keystrokes|./tools/checks/editorflash-check.sh
retry|Mail, Reminders and Calculator prompts redraw content, not chrome, per keystroke|./tools/checks/gui-prompt-keystroke-check.sh
retry|Notes typing, typography, pointer controls, persistence|python3 ./tools/checks/editor_qa.py
retry|Terminal and Chat chrome don't redraw on plain keystrokes|./tools/checks/termchatflash-check.sh
retry|Apple-menu hover stays cheap, clock redraws on a minute change|./tools/checks/menuclock-check.sh
retry|Multi-window chrome doesn't redraw on plain keystrokes|./tools/checks/mwkeyflash-check.sh
retry|Drawing lands offscreen, window_present puts it on screen|./tools/checks/backbuffer-check.sh
retry|Multi-window apps draw exactly one toolbar, not two|./tools/checks/mwdupetoolbar-check.sh
once |JPEG decoder, host harness|./tools/checks/jpeg-host-check.sh
retry|JPEG decoder, in-kernel|./tools/checks/jpeg-check.sh
retry|Dock apps open/close from the pointer alone|python3 ./tools/checks/appclose-check.py
retry|Dock hover survives mid-animation|python3 ./tools/checks/dockhover-check.py
retry|Launchpad tile click launches, doesn't just close the folder|python3 ./tools/checks/launchpad-click-check.py
retry|Apps folder layout (no black band, no row spill, no ghost icons)|python3 ./tools/checks/appsfolder-layout-check.py
retry|Multi-window (click-to-focus, real z-order compositing)|python3 ./tools/checks/multiwindow-check.py
retry|Window snapping (title-bar drag to edge/corner, real pixel proof)|python3 ./tools/checks/windowsnap-check.py
retry|Windowed apps start under the title bar, Calendar fits six weeks|python3 ./tools/checks/apptop-check.py
retry|Calendar Day, Week, Month and Year views|python3 ./tools/checks/calviews-check.py
retry|Dock icon edge quality (no staircased corners)|python3 ./tools/checks/iconedge-check.py
retry|Dock icon halo (clean clip to the tray, no glyph bleed)|python3 ./tools/checks/iconhalo-check.py
retry|Titlebar traffic-light AA (real coverage blend, not binary)|python3 ./tools/checks/titlebar-aa-check.py
retry|Dock tray corner AA (real coverage blend, not binary)|python3 ./tools/checks/traycorner-check.py
retry|Portfolio catalog opens, lists the fleet, and the list scrolls|python3 ./tools/checks/portfolio-check.py
retry|Boot logo AA (no false interior seams at overlapping capsule joints)|python3 ./tools/checks/bootlogo-check.py
once |Landing eyebrow tracks roadmap Latest, H1 stays the brand line|./tools/checks/landing-headline-check.sh
once |Idle tour still cycles all 8 real dock apps|node ./tools/checks/tourappcount-check.mjs
once |Idle tour autoplay fix is in place (tourArmed reset)|node ./tools/checks/idletour-arm-reset-check.mjs
once |RTC local-time shift math (v86's CMOS answers in UTC)|node ./tools/checks/rtc-timezone-check.mjs
once |Worker /api/proxy allowlist|node ./tools/checks/worker-proxy-check.mjs
EOF
}

# A wedged QEMU (hung boot, a script blocked forever on a socket that will
# never answer) must not eat the job's whole 20-minute budget and take
# every check after it down with a timeout instead of a result. Each check
# gets its own ceiling, so a hang is one named FAIL and the suite moves on.
PER_CHECK_TIMEOUT=${PER_CHECK_TIMEOUT:-300}
if command -v timeout >/dev/null 2>&1; then
    TIMEOUT_BIN=timeout
elif command -v gtimeout >/dev/null 2>&1; then
    TIMEOUT_BIN=gtimeout          # macOS with coreutils installed
else
    TIMEOUT_BIN=""                # macOS without it: no ceiling, results identical
fi
RUN_ONE() {
    if [ -n "$TIMEOUT_BIN" ]; then
        "$TIMEOUT_BIN" --signal=KILL "$PER_CHECK_TIMEOUT" bash -c "$1"
    else
        bash -c "$1"
    fi
}

pass=0; fail=0; flaky=0
summary=""
failed_names=""

while IFS='|' read -r mode name command; do
    [ -z "${name:-}" ] && continue
    mode=$(echo "$mode" | tr -d ' ')

    echo "::group::$name"
    start=$(date +%s)
    RUN_ONE "$command"
    status=$?

    if [ $status -ne 0 ] && [ "$mode" = "retry" ]; then
        echo "--- $name failed (exit $status), one automatic retry (timing-sensitive check) ---"
        RUN_ONE "$command"
        retry_status=$?
        if [ $retry_status -eq 0 ]; then
            elapsed=$(( $(date +%s) - start ))
            echo "::endgroup::"
            echo "FLAKY  $name (failed once, passed on retry, ${elapsed}s)"
            summary="${summary}FLAKY  ${name}"$'\n'
            flaky=$((flaky + 1)); pass=$((pass + 1))
            continue
        fi
        status=$retry_status
    fi

    elapsed=$(( $(date +%s) - start ))
    echo "::endgroup::"
    if [ $status -eq 0 ]; then
        echo "PASS   $name (${elapsed}s)"
        summary="${summary}PASS   ${name}"$'\n'
        pass=$((pass + 1))
    else
        echo "::error title=$name::check failed with exit $status"
        echo "FAIL   $name (exit $status, ${elapsed}s)"
        summary="${summary}FAIL   ${name} (exit ${status})"$'\n'
        failed_names="${failed_names}  - ${name}"$'\n'
        fail=$((fail + 1))
    fi
done < <(manifest)

echo
echo "================ regression suite summary ================"
printf '%s' "$summary"
echo "=========================================================="
echo "$pass passed, $fail failed, $flaky needed their one retry"

# The summary is also written to the job summary panel, so the full picture
# is readable without scrolling a 40-check log.
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    {
        echo "### Regression suite"
        echo
        echo "\`$pass passed, $fail failed, $flaky flaky\`"
        echo
        echo '```'
        printf '%s' "$summary"
        echo '```'
    } >> "$GITHUB_STEP_SUMMARY"
fi

if [ $fail -gt 0 ]; then
    echo
    echo "FAIL: $fail check(s) failed:"
    printf '%s' "$failed_names"
    exit 1
fi

echo "PASS: every check in the suite passed"
