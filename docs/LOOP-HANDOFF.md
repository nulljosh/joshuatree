# Joshua Tree loop handoff (2026-11-16, morning)

## What the loop is

Claude works the roadmap's goals toward v14, one pull request at a time, until weekly usage reaches 75% or nothing an emulator can prove is left. Each step is a minor version, each fix a patch. A goal only becomes the next major when its gate truly passes, and a gate that needs the board waits for Joshua's board test. Goal 14 is printing and scanning. The goals and gates are in `docs/ROADMAP.md`, under Version plan. The target was 20 until today; Joshua narrowed it to 14.

## Where things stand

- Main is on 13.9.2. PR 1134 (roadmap handoff docs) is merged.
- PRs 1121, 1127 and 1130 were closed as superseded.
- PR 1132 (13.9.3, board test fixes) is rebased onto main, in CI, set to auto-merge. It also makes the flaky Settings check wait for the whole line and print the full second-boot log when it fails.
- PR 1135 (13.11.0 to 13.14.0) is open, in CI, set to auto-merge: print a text file to a network printer, find printers on the network, Cmd+P in Notes, and a scan command that pulls a page from a network scanner into Photos. It is tested against a stub printer and a stub scanner in the emulator on the Mac, not yet on a real printer.
- Print and scan now release their memory after each job. Before that, the third scan ran the kernel heap out of memory on the Pi build.
- The Playwright browser cache was a link to the LaCie drive, which is gone, so local test runs were failing silently. The dead link is removed and the browsers are reinstalled.
- Branch `pi/wifi-rx` (worktree `~/Documents/Code/jt-wifirx`, one local commit, NOT pushed, no PR yet) is the likely fix for big plain-HTTP downloads stopping after 800 bytes on the real board. The Wi-Fi driver was throwing away incoming frames while it waited for permission to send. It passes the emulator and host tests. Only a board test can confirm it. It needs a rebase, a bump to 13.14.1 and a docs line before a PR.
- Parked until after 14: PR 1133 (Paddle polish) and PR 1128 (auto lock).

## Next, in order

1. Let PR 1132 and PR 1135 merge on green. Do not touch them by hand.
2. Rebase `pi/wifi-rx`, bump to 13.14.1, add the docs line, open a PR, merge on green. Its real test is on the board.
3. Joshua reflashes the card and, on real hardware, prints a note, scans a page and downloads a big file over plain HTTP. Those three checks close goal 14. The printing and scanning gates in QEMU are already provable; the board steps are not.
4. Then the next goal in the Version plan, with the same rules. Parked PRs 1133 and 1128 wait until 14 is done.

## Restart prompt

```
/loop check joshuatree until v14: work toward v14, the printing and scanning goal (docs/ROADMAP.md, Version plan), honest semver: a minor per step, a patch per fix, a major only when a goal's gate truly passes (board-only gates wait for Joshua's board test). Start from the state in docs/LOOP-HANDOFF.md: PRs 1132 and 1135 are auto-merge on green, do not touch them; the Wi-Fi fix sits on local branch pi/wifi-rx and needs a rebase, a 13.14.1 bump and a docs line before its PR. One PR per item off origin/main, local checks green, open ready, merge on green, no --admin. Park PRs 1133 and 1128 until 14 is done. Each major, unasked: fresh landing screenshots (tools/landing-shots.py), a fable-ad trailer of real screens with an audible hit on every cut; fix the roadmap Latest line. Keep docs slim and human, update the book. Keep docs/LOOP-HANDOFF.md current. Stop at weekly usage 75% or when nothing provable is left.
```
