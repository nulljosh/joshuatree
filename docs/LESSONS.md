# Lessons from the loop

What keeps going wrong when agents work this repo, and the rule that stops it. Each lesson is one line of problem and one line of rule. The commits named are where it happened; `docs/LOOP-HANDOFF.md` holds the standing rules these feed.

## Merges

**Generated files conflict on every merge.**
Problem: `VERSION`, `landing/index.html`, `landing/version.txt` and `docs/TESTING.md` change in almost every PR, so merging main always conflicts there, and hand edits got lost (5ff21afa, ecbe8ee5, 88d0ef6b restored landing work a merge dropped).
Rule: take main's side for those files, then regenerate them (bump `VERSION` above main, rerun the landing injectors and `tools/gen/testing-doc.py`), and re-apply your own landing edits on top.

**A Makefile conflict is never "pick one side".**
Problem: in the arm64 Makefile one side added the Calculator (`calc.o` on the link lines, built by a rule that uses `FPCC`, the floating-point compiler flags) and the other changed the network objects; taking one side lost the `calc.o` FPCC rule and link entries (6f4ce57e put them back).
Rule: read both sides of a Makefile conflict line by line and merge them by hand, then build every target (`make`, `make -C arch/arm64`, `make -C arch/arm64 pi`) before you commit.

**Conflict markers got committed and pushed.**
Problem: `<<<<<<<` lines were committed and pushed in `arch/arm64/Makefile` (6f4ce57e) and `docs/LOOP-HANDOFF.md` (40a4140e).
Rule: before every commit after a merge, `git diff --cached --check` must show no leftover conflict markers, and the build must pass.

**Two open PRs both bump the version.**
Problem: whichever merges second conflicts on `VERSION` and needs another bump.
Rule: merge them one at a time, in the order the handoff names, and rebump the second after the first lands.

## Checks

**Fixed sleeps make CI flaky.**
Problem: a check that sleeps a set time and then looks passes on a fast Mac and fails on a busy runner (fb6adbe3 and 3dff87ce swapped sleeps for polling).
Rule: poll for the state you expect with a deadline, never `sleep N` and hope.

**A new arm64 check that is not in the suite never runs.**
Problem: a check file in `tools/checks/` does nothing unless `tools/checks/ci-suite.sh` lists it.
Rule: add a manifest line to `ci-suite.sh` in the same PR; `tools/checks/suite-coverage-check.sh` fails if you forget.

**`docs/TESTING.md` drifts from the suite.**
Problem: adding or renaming a check leaves the testing page stale, and its check fails in CI (1a68634c, 583c747f were catch-up refreshes).
Rule: rerun `python3 tools/gen/testing-doc.py` whenever `ci-suite.sh` changes; `tools/checks/testing-doc-check.sh` enforces it.

**Draft PRs never run CI.**
Problem: the loop opens PRs as drafts so pushes do not burn runner time, which also means a draft shows no red even when it is broken.
Rule: run `tools/ci-local.sh` (one at a time) on the branch, and only `gh pr ready` once it passes; GitHub CI then runs on the ready PR.

## The real Pi

**`ticks` stops counting on the Pi.**
Problem: the timer handler stops advancing `ticks` early in boot, so any wait on it spins forever; the boot-time LED blink waited on it and left the USB keyboard dead (fa6995f4).
Rule: time anything on ARM64 with the generic counter (`cntpct_el0`), never `ticks`; `tools/checks/arm64-led-check.py` fails if the LED wait reads `ticks`.

**The board finds bugs QEMU cannot.**
Problem: QEMU has no Wi-Fi chip and a tidy network, so the WPA2 stall (the router wanted its exact security element repeated back), lost DNS replies and "no such host" (NXDOMAIN) answers only showed up on the real Pi (the clock's DNS retries, 32584eeb and 53516ddb, came from there).
Rule: a network or hardware feature is not done until it works on the board; batch board tests and ask Joshua for one card swap per round.

## Process

**A debug QEMU run with no timeout took the Mac down.**
Problem: QEMU with `-d int -D file` ran for two days and filled the disk.
Rule: every traced QEMU run gets `timeout` in front, and the log is deleted after.

**Pulling the SD card mid-flash froze the Mac's disk.**
Problem: a cut-off `tools/flash-pi.sh` run left the disk stuck until a restart.
Rule: never pull the card while the flash script runs; wait for it to finish.

**Merging with `--admin` skips the checks the loop relies on.**
Problem: an admin merge lands code nobody saw go green.
Rule: never use `--admin`; `gh pr merge --auto --squash` once the PR is ready and green.
