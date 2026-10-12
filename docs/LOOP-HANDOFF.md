# Joshua Tree handoff (2026-10-12)

## The goal

Joshua, 2026-10-11: "/loop until v3", then "v3 today. No issues, no stale branches. All PRs merged safely. All changes flashed to SD." Version 3.0.0 "Hold it" cuts when the desktop boots on a real Raspberry Pi 4 and you can type and click on it, mouse included, with apps in user mode. The board pass that proves it is [BOARD-TEST.md](BOARD-TEST.md).

## Where it stands

Every PR in the v3 queue is merged (2.43.0 to 2.62.0, twenty of them, 2026-10-11 into the night): typing into apps, Samantha from Turing through the Mac relay, relay discovery over UDP with pinned TLS, volume, user mode on ARM (hello, Calendar, Notes, Music at EL0 with windows, files and sound), Bluetooth radio and mouse pairing, Caps Lock and key repeat, SD card read and write, Notes autosave, the 3.0 mark, Samantha's clock, Pi screen sleep, Music's QA pass with ID3 titles, covers, durations and draggable bars, the Calendar landing tile, and Tree Sans with a type system on the landing page. Zero open issues, no stale branches.

The dev card carries main 2.62.0 (flashed 2026-10-12 at the desk). The Tree Sans OS build (2.63.0, PR open) and this release are not on it yet.

## Not yet seen on the board

Bluetooth pairing has only run against QEMU's fake radio. Sound out of the jack, Music through DMA, SD writes, user mode apps and screen sleep are all proven in QEMU and never on the real Pi. The board pass is the gate: when it passes, 3.0.0 is released; what fails becomes the first 3.0.x.

## After 3.0

4.0: many windows at once, mobile first. 5.0: talk to it. A Joshua Tree MCP server (boot, type, screenshot, serial, one check, flash, queue) is the first tool after 3.0.

## Rules kept

No bare `git stash` (shared across worktrees). Generated files (VERSION, landing/*, docs/TESTING.md) take main's side in a merge and are regenerated. No keys on the public card. Flash only the dev card. Every X.0.0 ships a refreshed logo and UI; 3.0 has the crayon tree on terracotta and Tree Sans.
