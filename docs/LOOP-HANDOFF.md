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
4. M3e, Music on the Pi (Joshua, 2026-10-11: "get the music app working next, QA it too"): `JT_SYS_AUDIO` over the PWM driver from 2.43.0 (8-bit mono into the ring, the volume from 2.48.0 applied), `JT_SYS_BRK` for the track buffer, `JT_SYS_READDIR` and `JT_SYS_READFILE` over the SD read slice, so `user/music.c` runs unchanged and plays a WAV or MP3 from the card through the jack. Check: QEMU with a FAT image holding a short WAV, the UART `music:` lines and a ring sum that matches the file. Board test: a song out of the speaker.
   QA, in parallel on i386: run Music headless, every key and mouse target in its header comment, screenshots at each state held to the retina bar, then fix what is found in the same PR. Known already: Music and Movies are not on the dock, only in Apps.
M3b and M3c part 1 are PRs #511 and #515. Part 2 (the window calls) comes next; the loader runs EL0 synchronously with IRQs masked, so WINDOW_POLL and YIELD must pump input and present the frame themselves, or EL0 must become resumable. That design call is part 2's first line.

## Bluetooth mouse, the click half of the gate

Joshua has no USB mouse, only a Bluetooth one: the Incase version of the Microsoft Bluetooth Ergonomic mouse (three channels, a pairing button), which is Bluetooth Low Energy, so the path is HID over GATT, not classic. The radio is the same CYW43455 as Wi-Fi; Bluetooth talks HCI over the PL011 UART. Today `tools/pi-config.txt` has `dtoverlay=disable-bt` and the kernel's console sits on that PL011 (`UART_BASE 0xFE201000`), so slice 1 moves the console to the mini UART (or drops the serial console on the Pi build) and gives the PL011 to the radio. QEMU has no Bluetooth chip, so every check drives a fake controller over a chardev socket.

1. BT1: HCI over the PL011 at the chip's rate, the BCM4345C0 `.hcd` patch loaded with the vendor download commands (vendored like the Wi-Fi firmware, licence noted in `docs/hardware/LICENSE-NOTICE.md`), HCI reset and read local version, LE scan, and `bt scan` printing what it hears. Check: a Python fake controller answers the scripted HCI and advertises a mouse.
2. BT2: LE connect, SMP Just Works pairing and bonding (keys kept in RAM until SD writes land, so re-pair after a reboot), GATT discovery of the HID service, report notifications on, boot mouse reports into the existing pointer (`input_event` with REL_X, REL_Y, BTN_LEFT). Check: the fake controller sends reports and the cursor moves and clicks under QEMU.

## Driver queue (Joshua, 2026-10-11: "loop over those until we have full support")

On the board today: HDMI framebuffer, USB keyboard and (in QEMU) mouse over xHCI, Wi-Fi over SDIO with WPA2, DHCP, DNS, TCP and TLS, the network clock, the RNG, the green light, reboot and shutdown through the PM block, and 3.5 mm sound (#506). What the box still needs, in the order the loop takes them, one agent and one PR each, every one with a QEMU check and a board test line:

1. Bluetooth, BT1 then BT2 (above): the mouse.
2. SD card through EMMC2, read then write, so files and settings survive a reboot and the local model's weights can load from the card. This is the "files that stay" third of 3.0. Reference: Circle's `lib/sdcard` and the BCM2711 EMMC2 register map; QEMU's raspi4b has an SD controller for the check.
3. USB audio class over xHCI (the first isochronous transfers): the Yeti mic in, a USB speaker out. Needed for voice in (5.0) and the box's mic.
4. PWM audio by DMA (the polled FIFO stalls the desktop on long sounds), then HDMI audio.
5. USB mass storage (bulk only), as the second way to keep files.
6. SoC temperature and throttle readout from the mailbox, with a notice; Ethernet through the Genet MAC as the wired backup.
7. Touch (USB touchscreen first, DSI later), the camera over CSI, the V3D GPU: 4.0 and after.

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
