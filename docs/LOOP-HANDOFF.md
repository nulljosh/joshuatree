# Joshua Tree handoff (2026-10-11, night)

## The goal

Joshua, 2026-10-11: "/loop until v3". Version 3.0.0 "Hold it" cuts when the desktop boots on a real Raspberry Pi 4 and you can type and click on it, mouse included (`DECISIONS.md`, item 4). Every major version also ships a refreshed logo and a visible UI step, then the landing page gets a QA pass (Joshua: "QA landing page when we hit v3!").

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

## This week

- Reflash the card from a merged build with the LAN relay and TLS (`RELAY-TLS.md`), so Samantha answers on the board.
- The 3.0 logo and UI refresh, held to the retina bar.
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

- One `tools/ci-local.sh` at a time. No Blender or other heavy job while it runs (it made frametime-check flaky once).
- PRs open as drafts and go ready only on a green local gate. Joshua authorised merging on green for this loop.
- Up to three agents, two if they are Opus or Fable. Stop an agent once its PR is pushed; the main session queues its gate.
- Check `~/.claude/scripts/usage.sh` between tasks. At 5 to 10 percent remaining, run `/checkpoint` and taper off.
- Keys stay in `~/.config/joshuatree/` and `~/.claude-relay-token`. Never print or commit one. Card backups carry credentials; never copy them to outputs.
- Never wait on `ticks` in `arch/arm64/main.c`. Never drive GPIO 42 at boot. Headless only.

## Physical limits

Only 2.34.0 has had a full board pass (boot, Spotlight, Terminal, an HTTPS fetch). Clock, icons, power controls, Brick, notices, mouse, RNG, sound, typing into apps and Samantha over TLS still need the board. The Mac relay must be running with `--lan` and the TLS pair before a Samantha test. The ISO is for BIOS PCs, not the Pi; UEFI and real PC boot are unverified.
