# Joshua Tree handoff (2026-10-11, night)

## The goal

Joshua, 2026-10-11: "/loop until v3" and, later that night from the board, "Focus on user mode by v3." Version 3.0.0 "Hold it" cuts when the desktop boots on a real Raspberry Pi 4, you can type and click on it, mouse included, and one app from the shared `user/` tree runs on the Pi in user mode (`DECISIONS.md`, item 4). Every major version also ships a refreshed logo and a visible UI step, then the landing page gets a QA pass (Joshua: "QA landing page when we hit v3!").

Order of work (Joshua, 2026-10-11): usable first, then stability, then polish. 4.0 and 5.0 may go mobile first, copying iOS the way the desktop copies macOS.

## Where it stands

Main is 2.41.0 (the boot splash draws the tree on). Open draft PRs, each with its own local gate queued one at a time:

- #505, 2.42.0: the Samantha box, a printable desk case (`docs/hardware/samantha_cad.py`). Local gate green, marked ready, waiting on GitHub CI.
- #506, 2.43.0: sound out of the 3.5 mm jack, `beep` and `beep FREQ MS`.
- #507, 2.44.0: typing into apps on ARM, a click focuses a window. This is the typing half of the 3.0 gate.
- #508, 2.45.0: Samantha's voice through the Mac relay (ElevenLabs, capped per request and per day).
- Coming, 2.46.0: the Pi finds the relay on the LAN by itself and TLS verifies a fixed name, so the Mac's address no longer has to match the card.

The dev card carries a test build of 2.41.0 plus #506 and #507, flashed 2026-10-11 02:05 before their gates ran. A glitch on the board may not be a real bug; rerun it on a merged build before filing anything.

## Tonight

1. Merge #505 when GitHub is green, then #506, #507 and #508 as each local gate passes (`gh pr ready`, wait for green, squash merge, delete the branch). On a conflict in a generated file take main's side and regenerate.
2. Pick the 3.0 mark from the three candidates (contact sheet in the session scratchpad), then ship it through `tools/gen/logo.py` with the UI step.
3. Board pass from Joshua: `beep`, typing in the Calculator, a click moving focus. A USB mouse on the Pi closes the click half of the gate.

## User mode on ARM (M3), the v3 centrepiece

Today one EL0 test program writes and exits over `svc` (`arch/arm64/user.S`, `tools/checks/arm64-m3-check.py`). The 31 desktop apps are i386 ring-3 programs built against `user/jtsys.h` and `user/libjt`; on the Pi only Terminal, Samantha, Calculator, Clock and Brick exist, written again in C against the kernel. Slices, one agent and one PR each:

1. M3b: `user/jtsys.h` gains an aarch64 `svc` path with the same numbers, `user/libjt` and `user/hello.c` build for aarch64 as a flat position-independent binary, and the ARM loader runs it: the EL0 handler answers `JT_SYS_WRITE` and `JT_SYS_EXIT`. Check: the UART shows hello's line and exit code.
2. M3c: `JT_SYS_WINDOW_OPEN`, `WINDOW_POLL`, `TIME` and `TEXT` on ARM over the window code from #507, so `user/calendar.c` runs unchanged on the Pi as the first ported app, embedded in `kernel8.img` the way `drivers/user_*.h` embed on i386. Check: Calendar opens from its dock tile and draws the month.
3. M3d: `OPEN`, `READ`, `WRITE`, `CLOSE` and `READDIR` over a RAM VFS seeded from the card, then Notes, Reminders and Weather. Check: a note typed on the Pi reads back.

## This week

- Reflash the card from a merged build with the LAN relay and TLS (`RELAY-TLS.md`), so Samantha answers on the board.
- M3b and M3c above. The 3.0 logo (option A, the crayon tree tightened with a terracotta ground, picked 2026-10-11) and UI refresh, held to the retina bar.
- Cut 3.0.0 once the board pass is clean, then QA the landing page: every link, the demo boot, the phone layout, the new mark everywhere.

## This month

- Print `fit_test.stl`, measure the real screen, speaker and board, then the first full case.
- SD card writes, so files survive a reboot, then the self-update path with a fallback kernel.
- The local model with real weights read from the card.

## Rest of the year

- 4.0 "Many windows": the window server, real processes, pipes, a Terminal with tabs.
- The Samantha box as a kit: board, card, case. Film the first print and every fix.
- Show it in person at Vancouver meetups, and the Show HN with the 30 second boot video (`LAUNCH.md`).

## Rules in force

- Graphs stay current (Joshua, 2026-10-11). After each MINOR merge, regenerate `progress.svg` (`tools/gen/progress.sh`), `docs/memory-map.svg` (`tools/gen/memory-map.py`, needs `make kernel.elf`) and `architecture.svg` (`tools/gen/map.py`, after the other two) and ship them in the next code PR or a `.1` patch; an SVG is not prose, so the bump check wants a version.
- Benchmarks stay current (Joshua, 2026-10-11). `docs/BENCHMARKS.md` and the README table are at 2.30.6; before each MINOR cut, rerun `tools/bench.sh` on a quiet Mac (no CI running) and paste both tables, and refresh the Pi table from the board (`[[bench]]` through Samantha today; a plain `bench` command in `arch/arm64/cmd.c` is the next small slice so it needs no relay).
- One `tools/ci-local.sh` at a time. No Blender or other heavy job while it runs (it made frametime-check flaky once).
- PRs open as drafts and go ready only on a green local gate. Joshua authorised merging on green for this loop.
- Up to three agents, two if they are Opus or Fable. Stop an agent once its PR is pushed; the main session queues its gate.
- Check `~/.claude/scripts/usage.sh` between tasks. At 5 to 10 percent remaining, run `/checkpoint` and taper off.
- Keys stay in `~/.config/joshuatree/` and `~/.claude-relay-token`. Never print or commit one. Card backups carry credentials; never copy them to outputs.
- Never wait on `ticks` in `arch/arm64/main.c`. Never drive GPIO 42 at boot. Headless only.

## Physical limits

Only 2.34.0 has had a full board pass (boot, Spotlight, Terminal, an HTTPS fetch). Clock, icons, power controls, Brick, notices, mouse, RNG, sound, typing into apps and Samantha over TLS still need the board. The Mac relay must be running with `--lan` and the TLS pair before a Samantha test. The ISO is for BIOS PCs, not the Pi; UEFI and real PC boot are unverified.
