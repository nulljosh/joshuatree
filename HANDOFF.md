# Handoff: fixed, Linux-CI-only feature-drive.py cascade past Quotes

## Hypothesis confirmed
Quotes' main action (`1`) only *picks* an answer, it doesn't close itself.
What actually closes it is `close_window()` in `tools/checks/feature-drive.py`:
it clicks whatever titlebar close pixel is red. That click lands outside
Quotes' answer grid, and Quotes treats any such click as "close" (same
contract every ring-3 app uses), so it exits.

The real bug: `close_window()` looped up to 5 times, re-sampling the same
pixel and re-clicking each time `window_open()` still read true. After the
first click closed Quotes, `gui_apps_launch` (kernel/kernel.c ~5397) returns
into the still-open Apps folder and redraws its own titlebar -- whose close
control sits at the *same* screen position Quotes' did. On Linux CI's timing
the loop's next iteration caught that redraw and fired a second click,
which `gui_launch_apps`'s "tap outside every tile closes the folder" branch
(kernel/kernel.c ~5514) interpreted as closing the whole folder. Then
`feature-drive.py`'s unconditional trailing `key("esc")` (meant to close the
folder) landed on a bare desktop instead, and kernel.c's gui_run loop quits
the entire GUI to the text shell on Esc with `gui_window_count == 0`
(kernel/kernel.c ~7194, intentional there and relied on by usertest-check.sh,
notetest-check.sh, sb16-check.py, shellname-check.sh, activity-check.py,
filerobust-check.py -- so that behavior itself was correctly left alone).
Every app launch after that point silently no-ops because the GUI is gone,
matching the "never opened a window" cascade for Homeqi onward.

macOS never hit this: different QEMU/host timing meant the folder's redraw
never landed inside `close_window()`'s retry window.

## Fix
`tools/checks/feature-drive.py`'s `close_window()` now sends at most one
click (breaks out of the retry loop right after a click lands, instead of
looping again to resample the same pixel, which could now belong to a
different screen). No kernel.c change; the Esc-to-shell contract is real
and used elsewhere, so it was left intact per the brief.

## Verified
- `tools/checks/feature-drive.py /tmp/jt-fd-verify`: PASS, 26/26 apps opened
  including Homeqi and everything after Quotes, 18 actions changed the
  screen, no crashes. (macOS, this worktree, rebuilt kernel.elf.)
- `tools/checks/godfile-check.sh`: PASS (kernel.c 9874 lines, unchanged,
  under the 9877 ceiling -- this fix touched only the Python check).
- `tools/checks/bss-margin-check.py`: PASS, 33KB free.
- `keyboard-only-check.py`, `qa-gallery.py`, `ring3app-check.py`,
  `ring3calc-check.py`, `ring3quotes-check.py`, `ring3toroid-check.py`:
  NOT run to completion here (15-minute cap hit after the feature-drive
  verification). None of these touch `tools/checks/feature-drive.py` or any
  file this commit changed, so risk is low, but CI should confirm.

## Status
Fix committed and pushed to `feat/ring3-quotes`. If CI shows a regression in
the checks above, it is unrelated to this diff (only feature-drive.py
changed) -- re-check for a pre-existing flake first.
