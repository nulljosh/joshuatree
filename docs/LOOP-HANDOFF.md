# Joshua Tree loop handoff (2026-10-06, night)

## What the loop is

The Pi loop. Joshua Tree booted on a real Raspberry Pi 4 today; the loop keeps merging green PRs, puts each merged build on the SD card, and works down the list below toward 3.0 (the desktop on a real Pi, where you can type and click).

## Where things stand (2026-10-06, 19:30)

- Main is 2.13.0. On the real Pi 4 at 1920x1080: the desktop boots, the console is readable, the Steve Jobs line is drawn, and a USB keyboard (NuPhy, behind the VL805 hub) types on screen. `docs/RASPBERRY-PI.md` has the log and photos.
- Open PRs, all drafts waiting on checks: #432 Pi log (2.13.1), #433 Claude app through a Mac relay (2.14.0), #434 USB hot-plug and port diagnostics (2.13.2). Merge each when green, rebase the next.
- The real desktop and dock on ARM is scoped, not started: the dock painters live inside `kernel/kernel.c` and must move to a shared `kernel/gui_paint.c` first (five slices, see roadmap). Version 2.15.0 is reserved for it.
- Flashing: `tools/flash-pi.sh` on the card in the Mac's reader. Joshua swaps the card by hand; no serial cable yet (he is buying a USB-C to TTL 3.3 V one this week).
- The main checkout at `~/Documents/Code/joshuatree` still holds a stale local commit on `main`; work from worktrees branched off `origin/main`.
- Helpers: at most three, two if any is Opus or Fable; each opens a draft PR, runs `tools/ci-local.sh` (about 35 minutes, never two at once with benchmarks), `gh pr ready` only when green. Main merges and flashes.
- Benchmarks are deferred while suites run; `/jt-bench` once the Mac is quiet.

## Next, in order

1. Merge #432, #433, #434 as they go green; flash the card after #434.
2. Dock and desktop on ARM, five slices: png/inflate plus wallpaper and the real menu bar; extract `kernel/gui_paint.c`; the ARM dock and window chrome; cursor and console typing; checks, docs, 2.15.0.
3. Dock launches apps on ARM (the first app on the Pi).
4. SD card reads through EMMC2, then Ethernet through the Genet MAC, then sound.
5. Serial loader the day the cable arrives: new kernels over the wire, no card swaps.
6. A wired mouse or the Logitech receiver on the Pi, once Joshua has one.

## Restart prompt

```
/loop until it's done! Joshua Tree on the Pi: read docs/LOOP-HANDOFF.md, merge open PRs when green, flash the card after USB hot-plug lands, then work the Next list in order (dock and desktop on ARM in five slices, app launching, SD and Ethernet, serial loader when the cable arrives). Draft PRs, ci-local green before ready, max three helpers, two if Opus or Fable. TLDR after each round.
```
