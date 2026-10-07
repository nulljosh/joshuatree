# Joshua Tree loop handoff (2026-10-07, 07:30)

## What the loop is

The Pi loop. Joshua Tree runs on a real Raspberry Pi 4 with a USB keyboard. The loop merges green PRs one at a time, builds the newest SD card image, and works down the roadmap's Top 10 toward 3.0 (type and click on the real Pi).

## The goal

Joshua's /goal: we can build a mini Joshua Tree with Claude Code inside Joshua Tree on the Pi. The Claude app (merged) works in QEMU through a relay on the Mac. The ARM build has the shared IP stack and the Console's ask> row talks to the relay on QEMU. Wi-Fi has to fill in its four network calls, then Claude can run on the Pi.

## Overnight rules (Joshua asleep, 2026-10-06 to 07)

- Never ask him a question; pick the sensible default and write it down here.
- Claude opens pull requests and never merges by hand; `gh pr merge --auto --squash` only once a PR is ready and its local run was green. Never force-push main. Never use `--admin`.
- Max three helpers, two if any is Opus or Fable. Stop starting new work at 90 percent of either weekly limit.
- One `tools/ci-local.sh` at a time (it takes a lock now). Disk above 6 GB free.
- At most four open PRs and zero issues. Delete a branch once its PR merges.
- The SD card needs Joshua: the newest image is built in worktree `jt-card3`; a watcher flashes it when the card mounts.
- Never print or commit the Wi-Fi password, tokens or keys. Nothing destructive on his files or the LaCie.

## Where things stand

- Main is 2.24.0 once this PR lands. On the real Pi: 1080p desktop, readable console, the tribute, USB keyboard with hot-plug. Merged this week and not yet seen on the board: the Satellite wallpaper and real menu bar, Claude in the Console, the crash screen, the real dock, its hover label and the window chrome. The Wi-Fi code is on the card and its power-on bug is fixed; Wi-Fi is still unconfirmed on the real Pi.
- The mouse (slice 4, 2.24.0): the i386 arrow now lives in `kernel/gui_paint.c` and a USB mouse moves it on ARM; the dock's label follows it, the Console's red button closes it and a dock click opens it again. Proven in QEMU only (`tools/checks/arm64-mouse-check.py`). Joshua's mouse is Bluetooth, which the Pi build cannot use; a wired USB mouse runs the short test list in `docs/RASPBERRY-PI.md`.
- Open PRs: this one (desktop slice 4, the mouse) only. Zero issues.
- The newest card image is rebuilt in `jt-card3` after each merge that touches `arch/arm64`.
- Standing answers are in `docs/DECISIONS.md`; the parts list is `docs/SHOPPING.md`; the Top 10 is at the top of `docs/roadmap.md`. Joshua's next steps: buy a wired USB mouse, buy a 3.3V serial cable. HN post drafted in `docs/LAUNCH.md`, waits for the mouse working on the Pi.
- Hardware: the first kit is the Pi with the OS on an SD card, no price and no number yet.

## Next, in order

1. Joshua swaps in the newest card; he photographs the Console. Page Up shows the Wi-Fi lines; the pinned row shows the latest `wifi` line. If it says `WL_ON reads 1` and then a `sdio card rca` line, the chip is alive.
2. Keep merging green PRs one at a time; rebase the rest above main's VERSION. Check the test variants in `arch/arm64/Makefile` still link the shared network and ask objects after any Makefile merge.
3. Desktop slice 5: typing into apps on the ARM desktop, then the live Clock and Calendar faces. Buy or borrow a wired USB mouse for the slice 4 test list.
4. Wi-Fi stage 2: join the network from the ignored config file, fill the four network calls, then TCP that survives loss.
5. Admin and sudo; SD card writes with MBR and FAT32; the self-update loop and the fallback kernel; Doom.

## Restart prompt

```
/loop until it's done! Joshua Tree on the Pi: read docs/LOOP-HANDOFF.md, merge open PRs one at a time when green, keep the newest card image built in jt-card3, then work the Next list in order. Draft PRs, one ci-local run at a time (it locks), max three helpers (two if Opus), stop new work at 90 percent weekly usage. Keep the repo, branches and PR list clean (zero issues, at most four PRs). TLDR after each round.
```
