# Joshua Tree roadmap

Freestanding i386 kernel, no libc. This is the forward plan. What already
shipped lives in `git log`, `git tag -l "jt-v*"`, and the [GitHub
releases](https://github.com/nulljosh/joshuatree/releases), not here.

See `docs/BLUEPRINT.md` for the structural plan of where this OS goes after 1.0.

**Latest**: Joshua on the web, Music and Movies, and a Raspberry Pi build.

<!-- NOTE: The **Latest** field is public-facing copy synced to the landing page's h1/eyebrow. Must read as a feature announcement ("Introducing X."), never a changelog line. Update alongside version bumps. tools/gen/inject-landing-headline.sh reads this line automatically. -->

**Model tag on each item**: `[Haiku]` mechanical, known-correct shape, cheap. `[Sonnet]` general feature work with a clear pattern to follow. `[Fable]` anything where a subtly wrong answer still boots fine: privilege isolation, exact register/stack layouts, wire-protocol bytes, memory-model changes. `[Joshua]` a design or scope call, not code. Re-tag if an item turns out easier or harder once opened.

## Now (set 2026-10-03)
Samantha runs the machine, and Joshua is the face of the web portfolio. The phone demo, the OS and the landing all work on a phone. Everything below is what is left, in the order to pick it up. Merge one PR at a time, green first. `docs/LOOP-HANDOFF.md` has the restart prompt and the exact state. Full items live in the themed sections further down.

## Pickup (written 2026-10-03, night)
Main is 2.6.13 and live. CI takes about 10 minutes. The 3.0.0 gate is one thing: the desktop boots on a real Raspberry Pi 4.

### Landing and demo, to A+
- [ ] [Sonnet] Phone shows two input bars: the OS draws its own chat bar and the page draws a real composer for the phone keyboard. Keep one visible. The OS bar can hide while the composer is up, or the composer can be the only bar and feed the OS. Check: `tools/checks/phone-boot-check.py` plus a screenshot of the phone tour.
- [ ] [Sonnet] The tour is silent after the intro video. Joshua speaks each stop in his cloned voice (`/api/speak` with `voice: "joshua"`, see the personas doc in the Turing repo), with the caption on screen and the speaker button respected. Check: extend `tools/checks/portfolio-mute-check.mjs` so muted means no audio request.
- [ ] [Sonnet] Real-Chrome QA of the whole tour after any tour change, desktop and phone. Headless Chromium has no H.264, so it skips the intro video: run the checks with the system Chrome (`CHROMIUM_PATH`). The last full desktop pass was clean (intro, then Epiphany, Curbfind, Bookrank, Lexly, Sparkjar). The phone tour was last checked before the 2.6.13 fixes.
- [x] [Haiku] App tiles on the landing go stale. Done: `tools/landing-shots.py` pins the QEMU clock to 2026-10-03 and the Calendar tile is retaken; the other tiles only redraw when their app changes. Original note: `python3 tools/landing-shots.py calendar` draws today's date, so the Calendar tile ages by the day. Pin the QEMU clock (`-rtc base=...`) in `tools/landing-shots.py` so every tile is the same on every run, then retake all of them.
- [ ] [Sonnet] All icons share one design system. The fleet icons are imported untouched and keep their own tile colors (`tools/gen/import_fleet_icons.py`: `TILE` and `GLYPH`), so the Apps folder reads uneven. Decide the rule (Joshua's call: Lexly stays sky blue, #2E86DE), then bring the rest in line with the shared tile, light and margin in `tools/gen/restyle_icons.py`. Keep the `icon*-check.py` set green.
- [x] [Haiku] Done 2026-10-04: Lexly's `icon.svg` is now the sky blue one and the `GLYPH` recolor is gone. Original note: Lexly's own repo disagrees with itself: `icon.svg` is a black tile with blue dots, `assets/icon.svg` and the store icon are sky blue. Make `icon.svg` the blue one so the import needs no recolor, then drop the `GLYPH` workaround.
- [ ] [Joshua] Judge the live landing against the Plank landing, the bar for the whole site. List what still falls short, in his words.

### From the notebook (Joshua, 2026-10-04)
Two notebook pages checked against the tree. Already shipped and not listed: menu bar with weather, dock, Activity, Trash, clipboard, Burrow (the Finder), Epiphany tabs, Launchpad-style Apps folder, Music, Movies, landing page, docs at 100 percent, CI, Samantha chat with tools. Wi-Fi was ruled out for 1.0 and is under Our own computer.
- [ ] [Sonnet] Rename the Strata enclosure to Neo (Joshua, 2026-10-04): `docs/HARDWARE.md`, `docs/hardware/PI-CASE.md` (Strata Pi becomes Neo Pi), the CAD scripts and their output names, the landing. The notebook pitches Neo on mobility and security; what mobility means for a Pi box (battery, portable monitor, carry case) is still [Joshua].
- [ ] [Joshua] Competitor research as a doc: Apple Mac mini against our box on RAM (8 to 16 GB), integrated CPU, multi-display over HDMI, internal or external design, USB-C ports. A good-computer checklist for `docs/HARDWARE.md`.
- [ ] [Fable] Time Machine: snapshots of the disk with a browse-the-past view. Nothing exists; needs a FAT snapshot design first.
- [ ] [Sonnet] Fullscreen avatar, custom: Samantha (or Joshua's face) full screen as a mode, with the face picked in Settings. The page also lists video, audio and GUI mode as three ways to talk to her.
- [ ] [Fable] Integrated LLM that runs on the box, not through the proxy. Needs the Pi to have the memory and a runtime; decide once M4 is real.
- [ ] [Sonnet] Improved chat app: more tools and persistent memory across boots (a file the app reads at start).
- [ ] [Sonnet] Spotlight: one key opens a search box over apps, files, contacts, events. The Search app does files only; the existing Spotlight-style item under Desktop and apps is the same item.
- [ ] [Sonnet] Dock position setting: left, bottom, or hidden. Already listed under Desktop and apps as Moveable dock position; this is the second ask for it.
- [ ] [Fable] Multitasking through a compositor: the same compositor item as under Architecture, bumped because the notebook lists it as a headline feature.
- [ ] [Sonnet] GarageBand-lite: record and layer a few tracks from the Sound Blaster, then play them back. Movie trim (iMovie) is already listed.
- [ ] [Sonnet] Sharp image everywhere, no visible pixels: audit the icons and small type at retina scale, same bar as the JT retina polish rule.
- [ ] [Fable] The big promise, in his words: say "computer, run the simulation", "build me a game", "publish and monetize my apps", "add X feature", "patch Y bug", and the OS does it. Samantha plus a coding agent plus the publish flow. Scope it as a doc before any code.
- [ ] [Joshua] Constraints and design system as written rules: one page of what the OS never does (no pixels, no clutter) and the shared icon rules. The icon item under Landing already covers the second half.

### Voice chat, lag and sharing (Joshua, 2026-10-04)
Checked 2026-10-04: neither side listens yet. Both are typed text in, her voice out, so "voice chat" still needs speech-to-text. Each message in the OS is three round trips in a row: `/api/pick` (which tool), `/api/chat` (the full reply, no streaming), then `/api/speak` per sentence piece. Order: measure, cut round trips, stream, then listen.
- [x] [Sonnet] OS side, 2.6.22: one `voicetime:` serial line per message (pick, chat, first sound, in ms from Enter), asserted by `tools/checks/chat-face-check.py`.
- [ ] [Sonnet] Read `voicetime` off the live demo for ten messages and post the numbers here. The web portfolio gets the same line (`console.info`) next.
- [ ] [Sonnet] Skip `/api/pick` when the message plainly is not a tool request, or have `/api/chat` pick the tool in the same call. One round trip less on every message.
- [ ] [Fable] Speech-to-text: mic in, words out. Web first (browser mic plus a hosted STT), then the OS once it has audio input (an SB16 capture path or the Pi's USB mic).
- [ ] [Sonnet] Stream every stage: STT, LLM tokens, ElevenLabs over its WebSocket TTS, audio chunks played as they land. Add barge-in: talking over her stops her.
- [ ] [Sonnet] One shared voice module, the portfolio's code, used by both. The OS stays thin: it opens the voice page or calls a small server, no audio pipeline or TLS of its own.
- [ ] [Sonnet] Hardening: keys server-side only, a per-session cost cap, reconnect on a dropped socket, Claude as the fallback when Turing is down.
- [ ] [Joshua] Live face: Simli or HeyGen LiveAvatar (credit based, about $30 to $500 a month). Veo and Higgsfield stay for pre-rendered ads and intros only.
- [ ] [Joshua] ElevenLabs Agents could run the whole loop with any LLM endpoint as the brain (about $0.08 to $0.10 a minute). Decide: their loop, or ours.

### CI and speed
- [x] [Sonnet] Clock icon check flake fixed, 2026-10-04: on a slow runner the face sits still, white and handless for seconds, and the check took that as settled. Reproduced locally under CPU load with the exact CI numbers; it now waits for the hands to have ink (`tools/checks/clockicon-check.py`).
- [ ] [Sonnet] Six of the last ten red runs were slow-runner timing flakes (Chat tool scenes, the phone mute button, Keyrate, the Apps folder layout): eight QEMUs share one runner. Find out how many cores the runner has, cap QEMUs per runner or move to 10 shards (the balancer says about 319 s of checks per shard, 12 shards about 266 s), and watch the next ten runs.
- [ ] [Haiku] Re-balance after adding checks. New manifest lines default to 30 s until timed: run `python3 tools/gen/ci-balance.py <run-id>` on a green run (`--check` shows the numbers first) and commit the result.
- [ ] [Haiku] About a third of recent runs were cancelled by force-pushes to an open PR. Push once per PR, or fold PRs together before CI starts.
- [x] [Haiku] Found 2026-10-04: the repo's `core.hooksPath` was an absolute path into the main checkout, so every worktree ran whatever hook that checkout's branch had. Now the relative `tools/hooks` (what `make hooks` sets), so each worktree runs its own. Original note: Three red runs today were things the pre-push hook already covers (unlisted check, kernel.c over its line ceiling). The hook did not run on those pushes. Find out why (hook not installed in the worktree, or `--no-verify`) and make it hard to skip. It runs the god-file guard now.
- [ ] [Sonnet] `tools/ci-local.sh` takes about 27 minutes (8 shards, 2 at a time). Run 4 at a time on the M4 and use the balanced manifest.

### Raspberry Pi and ARM64
- [ ] [Joshua] Buy the board (Pi 4B 4 GB, 5 V 3 A supply, 16 GB+ microSD, 3.3 V USB serial cable CP2102 or FTDI, jumper wires) plus a USB-C microSD reader and a USB-A to USB-C adapter, because the Mac mini has no SD slot. Best Buy Bellingham lists CanaKit kits but check stock by phone first. Canada Computers and Memory Express are the Vancouver options for the serial cable.
- [ ] [Joshua] First real boot over serial, following `docs/RASPBERRY-PI.md`. Photograph the console. Whatever the chip does differently from QEMU becomes the next task.
- [x] [Fable] M1c part one, 2.6.21: ramfb framebuffer on QEMU virt through fw_cfg, a first desktop drawn into it, proven by a QEMU screendump (`tools/checks/arm64-m1c-check.py`).
- [x] [Fable] M1c part two, 2.6.22: the Pi build asks the GPU for a framebuffer through the VideoCore mailbox and draws the same desktop, proven on QEMU's Pi 4B model by screendump (`tools/checks/arm64-m1c-check.py`). Real board still to try.
- [x] [Sonnet] M1c boot log on screen, 2.6.22: every line the ARM kernel prints over serial is also drawn in the window, so a first boot with a bad serial cable still shows how far it got (`tools/checks/arm64-m1c-check.py` counts the text pixels).
- [x] [Sonnet] M1d part one, 2.6.23: the ARM screen draws smooth DejaVu text (the i386 desktop's own rasterizer, `drivers/ttf.c`, built for aarch64 with the FPU on): a menu bar title, a titled window and an anti-aliased boot log. `tools/checks/arm64-m1c-check.py` counts the soft edge pixels.
- [ ] [Fable] M1d part two: the real window and dock drawing code (`drivers/window.c`) running on the ARM build instead of rectangles, and the framebuffer mapped write-combining so a live desktop needs no cache cleans.
- [x] [Fable] M2 keyboard and mouse, 2.6.22: one modern virtio-mmio input driver reads key down and up, pointer position and clicks, polled (`tools/checks/arm64-m2-check.py`).
- [x] [Fable] M2 disk, 2.6.22: a virtio-blk driver reads a known sector off a disk image (`tools/checks/arm64-m2-check.py`). Writes and a FAT reader on top are next.
- [x] [Fable] M2 network, 2.6.22: a virtio-net driver sends an ARP request to the router and prints its real answer (`tools/checks/arm64-m2-check.py`).
- [ ] [Fable] M2: IP, DHCP and a TCP connection on top of the ARM network card (port the i386 stack above the NIC), and the net and disk drivers moved to interrupts too (input already is). Then M3 (EL0 userland and the syscall layer) and M4 (SD through EMMC2, USB through xHCI, Ethernet through the Genet MAC). 3.0.0 ships when M4 shows the desktop on a real Pi. `docs/ARM64.md` has the milestones.
- [ ] [Fable] Wi-Fi on the Pi 4 (CYW43455 over SDIO) needs a firmware blob and an 802.11 stack. Not scheduled: Ethernet first.

### Known limits to recheck
- [ ] [Sonnet] `SYS_READFILE` reads with interrupts off, so loading mid-song can glitch the audio.
- [ ] [Sonnet] Music and Movies live in the Apps folder only, not on the dock.
- [ ] [Sonnet] Silent movie clips play about twice too fast on this QEMU build.
- [ ] [Haiku] Check that PR 387 (the portfolio demo starts at once and tours the launchpad) landed, and that no stray branch or worktree is left behind.

## Toward 2.0: apps leave the kernel
- [x] [Fable] Step one, 1.7.7: Keyrate is the first app running as a real ring-3 process (`user/keyrate.c`, launched by `kernel/ring3app.c`) with its own window through two new syscalls (`SYS_WINDOW_OPEN`, `SYS_WINDOW_POLL`) and real crash isolation: a null write inside it is reaped by the kernel, the window is torn down, the desktop comes back. Proven by `tools/checks/ring3app-check.py`. Not 2.0 yet.
- [x] [Fable] Step two, 1.7.11: Toroid runs at ring 3 (`user/toroid.c`, bit-packed grids in its own .data), `kernel/ring3app.c` is one table-driven launcher (`RING3_APPS`), and the in-kernel copies of both Keyrate and Toroid are deleted. `tools/checks/ring3toroid-check.py` proves it draws, closes both ways, crashes safely.
- [x] [Opus] Step three, 1.7.12: Calculator runs at ring 3 (`user/calculator.c`, the same recursive-descent grammar evaluated straight into a double instead of an expr_node tree, since a flat binary has no .bss and no kmalloc), still one row in `RING3_APPS`, and its in-kernel copy is deleted. `tools/checks/ring3calc-check.py` proves it evaluates through the real parser (`12*3 = 36`, `5/0 = 0`), draws, closes both ways, crashes safely.
- [x] [Opus] Step four, 1.7.14: Quotes runs at ring 3 (`user/quotes.c`, the same fixed deck and answer-rotation, streak and best kept in its own `.data`), still one row in `RING3_APPS`, and its in-kernel copy is deleted. `tools/checks/ring3quotes-check.py` proves it draws the option grid, answers right and wrong through the real logic, closes both ways, crashes safely.
- [x] [Opus] Step six, 1.9.1: Lexly runs at ring 3 (`user/lexly.c`, the same 30-word deck and drill), one more row in `RING3_APPS`, and its in-kernel copy is deleted. `tools/checks/ring3lexly-check.py` proves it opens, draws, scores keys, crashes safely and leaves the desktop alive.
- [x] [Opus] Step seven, 1.9.2: Plan runs at ring 3 (user/plan.c, removed in 2.0.0 with the app, the same five milestones and two-pane layout), one more row in `RING3_APPS`, and its in-kernel copy is deleted. tools/checks/ring3plan-check.py (removed with it) proved it opens, draws, selects by key and click, crashes safely and leaves the desktop alive.
- [x] [Opus] Step eight, 1.9.3: Fieldbook runs at ring 3 (`user/fieldbook.c`, the same twelve fields and two-pane layout), one more row in `RING3_APPS`, and its in-kernel copy is deleted. `tools/checks/ring3fieldbook-check.py` proves it opens, draws, selects by key and click, crashes safely and leaves the desktop alive.
- [x] [Opus] Step nine, 1.9.4: Clock runs at ring 3 (`user/clock.c`, the same time, timer and alarm, reading `SYS_TIME`), one more row in `RING3_APPS`, and its in-kernel copy is deleted. `tools/checks/ring3clock-check.py` proves it opens, draws the moving time, takes a timer, closes on Esc and leaves the desktop alive.
- [x] [Opus] Step ten, 1.9.5: Portfolio runs at ring 3 (`user/portfolio.c`, the same About block and fleet catalog), one more row in `RING3_APPS`, and its in-kernel copy is deleted. `tools/checks/ring3portfolio-check.py` proves it opens, draws, selects by key and click, closes on Esc and leaves the desktop alive.
- [x] [Opus] Step eleven, 1.9.6: Activity runs at ring 3 (`user/activity.c`, the same task list, memory line and Kill button), one more row in `RING3_APPS`, one new syscall (`tasks`, 386) for the scheduler and memory numbers, and its in-kernel copy is deleted. `tools/checks/ring3activity-check.py` proves it opens, draws, refreshes on its own, selects, has the kernel refuse to kill the shell, closes on Esc and leaves the desktop alive.
- [x] [Opus] Step twelve, 1.9.7: Contacts runs at ring 3 (`user/contacts.c`, the same list, add prompt, person view and delete, `CONTACTS.TXT` rewritten through the existing file calls), one more row in `RING3_APPS`, and its in-kernel copy is deleted. `tools/checks/ring3contacts-check.py` proves it opens, draws, adds and deletes through the real prompt, keeps the file across fresh runs, closes on Esc and leaves the desktop alive.
- [x] [Opus] Step thirteen, 1.9.8: Sparkjar runs at ring 3 (`user/sparkjar.c`, the same ranked idea list with upvote and re-sort, votes kept for the run only, no new syscall), one more row in `RING3_APPS`, and its in-kernel copy is deleted. `tools/checks/ring3sparkjar-check.py` proves it opens, draws, moves the selection, re-sorts on the upvote that passes the leader, closes on Esc and leaves the desktop alive.
- [x] [Opus] Step fourteen, 1.9.9: Reminders runs at ring 3 (`user/reminders.c`, the same checklist with add prompt, tick and delete, `REMINDERS.TXT` rewritten through the existing file calls), one more row in `RING3_APPS`, and its in-kernel copy is deleted; Samantha's reminder tools keep working by reading the file fresh on each call. `tools/checks/ring3reminders-check.py` proves it opens, draws, adds, ticks and deletes through the real keys, keeps the file across fresh runs, closes on Esc and leaves the desktop alive.
- [x] [Fable] Step fifteen, 1.9.11: Curbfind runs at ring 3 (`user/curbfind.c`, the same ranked deal list, detail pane and Vancouver samples), one more row in `RING3_APPS`, one new syscall (`http_get`, 387) for the live rows with the host fixed kernel-side and the path checked before the network is touched, and its in-kernel copy is deleted. `tools/checks/ring3curbfind-check.py` proves it opens, draws, falls back to the samples with no NIC, selects by key and click, has every bad path and pointer refused, closes on Esc and leaves the desktop alive.
- [x] [Opus] Step sixteen, 1.9.12: Calendar runs at ring 3 (`user/calendar.c`, the same Day, Week, Month and Year views and one event per day, today from `SYS_TIME` with the date math in the program, `EVENTS.TXT` rewritten through the existing file calls), one more row in `RING3_APPS`, and its in-kernel copy is deleted; Samantha's calendar tool keeps working by reading the file fresh on each call. `tools/checks/ring3calendar-check.py` proves it opens, draws the grid, saves an event through the real editor, keeps the file across a fresh run, answers Samantha, closes on Esc and leaves the desktop alive.
- [x] [Fable] Step seventeen, 1.9.13: Search runs at ring 3 (`user/search.c`, the same query box, live filter, folder step-in and file view), one more row in `RING3_APPS`, one new syscall (`readdir`, 388) that lists a directory into fixed-size records by a path relative to the shell's directory, and its in-kernel copy is deleted. The app keeps its own cwd string, so stepping into a folder no longer moves the kernel's cwd; `open` takes the same relative paths. `tools/checks/ring3search-check.py` proves it opens, filters as you type, shows a file's real bytes, steps into a FAT folder and back out with the kernel's cwd untouched, has the kernel refuse a bad pointer, an over-long path and a missing folder, closes on Esc, crashes safely and leaves the desktop alive.
- [x] [Sonnet] Phone visitors can type to the landing demo: phones get a chat bar and a Send button under the demo, and what is typed shows live in the kernel's own input line. Before, Samantha, Notes and Terminal took typed keys and a phone had no way to send any. `tools/checks/mobile-type-check.mjs` types on an iPhone profile and proves the letters reach Samantha's screen once each, a correction sends Backspace and Send sends Enter.
- [x] [Sonnet] Landing QA fixes from the desktop pass: the demo no longer traps a keyboard user (Tab walks past it, Enter takes it over, Shift+Escape gives it back, and it shows a focus ring), the page has one `<main>` landmark, the version eyebrow is a paragraph not a heading, canonical and social meta are set, a stray sentence under the Strata render is gone, and on phones the Full screen button moved top right with an icon. `tools/checks/landing-demo-ui-check.mjs` proves each one and fails on the old page.
- [x] [Sonnet] Landing QA fixes from the phone pass: Full screen and Send are icons, tapping the chat bar puts the demo in full screen and the screen shrinks to fit above the keyboard so she stays in view, a phone on its side gets a real demo instead of a 128x72 strip, an iPad gets the chat bar, and the waitlist fields are 44px. Covered by `tools/checks/landing-demo-ui-check.mjs` and `tools/checks/mobile-type-check.mjs`.
- [x] [Sonnet] Landing reads in plain words, and the developer specs moved, not went away: benchmarks, the growth chart, the documented percent, architecture and build facts sit in collapsed Tech specs accordions near the bottom. The footer is now a four-column directory like apple.com. Covered by `tools/checks/landing-specs-footer-check.mjs`, and `tools/checks/landing-layout-check.mjs` opens every accordion before it measures.
- [x] [Sonnet] Crash check: one check crashes every ring-3 app on purpose and proves the desktop survives each one. Contacts, Sparkjar, Reminders, Curbfind and Calendar gained the backquote crash key the other twelve already had. `tools/checks/ring3crash-all-check.py` reads `RING3_APPS`, so a new app is covered automatically, and checks that the task is reaped, the window is gone, the dock is drawn and Mail still opens (all 18 apps).
- [ ] Window open takes about 640 ms for a ring-3 app (CI frametime check, 2.0.0): profile the seed, private page table and first draw; preseed binaries at boot and skip zeroing pages that get overwritten. [Sonnet]
- [ ] [Sonnet] Port the remaining apps the same way, one PR each. Each PR: `user/<app>.c`, a row in `RING3_APPS`, the in-kernel copy deleted once the check passes.
- [ ] [Fable] What the ports will need from the ABI: a font syscall (Keyrate carries its own 8x16 bitmap), a tick clock finer than `SYS_TIME`'s seconds, more than one program window at a time, and the framebuffer pages flipped back to supervisor-only on release (today they are zeroed and re-mapped on the next open).

## Architecture to A+ (refreshed 2026-09-27)
Measured against the closest from-scratch peers: SerenityOS (the one-person-scale benchmark), ToaruOS (own compositor, own libc), KolibriOS (tiny, runs on real PCs), Haiku, and against macOS/Linux. Ranked; the loop works top down. Each line points at the section that closes it.

1. **Real hardware.** Boots only in QEMU: no xHCI USB, no AHCI, no e1000; keyboard needs legacy BIOS mode, saving needs an old IDE disk. The hardware business depends on this. See Our own computer.
2. **A compositor.** Apps draw straight to the framebuffer in blocking loops: at most two windows, no resize or minimize, nothing runs in the background. See Desktop and apps.
3. **Native TLS.** HTTPS goes through the worker proxy, so a browser can't happen yet.
4. **Sound beyond the demo.** The Sound Blaster driver plays audio in QEMU (1.6.9), but every peer ships a music player, and real PCs need AC97 or HD Audio. See Desktop and apps, Our own computer.
5. **Desktop basics.** Undo, right-click menus, drag and drop, an app switcher, a screenshot key. SerenityOS, ToaruOS and KolibriOS all have these (the clipboard landed in 1.0.6, text selection in 1.2.0).
6. **Apps from outside the kernel.** All twenty-five apps are ring-3 programs (Keyrate, Toroid, Calculator, Quotes, Bookrank, Lexly, Plan, Fieldbook, Clock, Portfolio, Activity, Contacts, Sparkjar, Reminders, Curbfind, Calendar, Search, Epiphany, Weather, Burrow, Stocks, Mail, Notes, Terminal, Samantha). No installer, no update path.
7. **Everyday apps peers ship.** An image viewer, a music player, a few games. KolibriOS ships dozens in under 2MB. See Desktop and apps.

Kernel.c is ~9,800 lines with 84 files pasted in; an Opus agent is building the app interface (`feat/app-interface`) so apps move to ring 3. Grading is currently C+.

- [ ] [Fable] Crash reports with function names: build a symbol table into the kernel and print `panic in <function>+offset` over serial and on screen.
- [ ] [Fable] HTTPS without the proxy: native TLS 1.3 in the kernel, then the web browser on top of it.
- [ ] [Fable] The lag (issue #14) profiled, fixed and measured, with frame time numbers in the release notes.
- [ ] [Sonnet] Nothing crashes: bad input in every text field, long lines, empty files, missing disk, no network. Each case gets a check.
  - 1.0.10: empty, oversized, corrupt-FAT and full-disk cases; `tools/checks/filerobust-check.py`.
- [ ] [Sonnet] Icons with taste: depth, soft light, real materials, consistent corner and light direction across all of them. Judged from the gallery at dock, hover and Apps grid sizes.
  - 0.89.0: the 11 dock icons redrawn Big Sur style (one top light, no outlines, 148px art at an exact 2:1); `tools/checks/iconlight-check.py`. Still to do: the 15 Apps-folder fleet icons, and a live date on Calendar's tile.
- [ ] [Sonnet] Text sharp everywhere: no bitmap fallback font where the antialiased one should draw, no uneven letter gaps, baselines level.
  - 0.89.0: coverage-to-ink curve (stem darkening) sharpens every AA text path; `tools/checks/textsharp-check.py`.
  - 1.0.9: a retina-bar typography pass judged from real headless crops across the menu bar, a title bar, Notes' paragraph, Terminal's prompt/output, a Settings row, the dock hover label and the Apps folder grid; fixed the Apps folder's glass panel being 19 logical px short of its own grid. `tools/checks/baseline-check.py` measures container padding, baseline flatness and letter-gap variance on real pixels.
- [ ] [Haiku] Accessibility floor: every app opens and closes by keyboard alone. In progress in #127 (Enter on the desktop opens the Apps folder; the check still has to prove all 25). Ships as 1.0.x, not a blocker for the tag.
- [ ] [Fable] Kernel goes 1-bit: ordered dither (4x4 Bayer) replaces every grey and alpha blend.
- [ ] [Sonnet] Window chrome: 1px ink border, hatched title bar, no shadows.
- [ ] [Sonnet] All 23 app icons redrawn as 1-bit line glyphs.
- [ ] [Haiku] Boot splash: the badge drawn scanline by scanline.
- [ ] [Haiku] `landing/icon.svg`, root `icon.svg`, and the app `.icns` redrawn as the simplified tree.
- [ ] [Haiku] Audit that every fleet app has a native port or a line in this roadmap.
- [ ] [Sonnet] File search, Spotlight-style. Needs an index-or-scan design, not a stub.
- [x] [Sonnet] An Activity Monitor app over the shell's `ps`/`kill`/`mem`. Shipped in 1.9.6, at ring 3.
- [ ] [Fable] A second privilege tier (sudo/admin) on top of the accounts that already exist.
- [ ] [Fable] Text rendering: a dedicated pass on AA quality, separate from the font size/weight controls that already exist.
- [ ] [Fable] Multi-core (SMP).

Decided, not doing: a C++ rewrite (no gain for a freestanding kernel, only risk), and moving the landing page to a `gh-pages` branch.

## Apps after 2.2 (cheapest first, one agent and one PR each)

- [ ] [Haiku] Photos: browse and view JPEG and PNG from Files, next and previous by arrow key. The decoders already exist.
- [ ] [Haiku] Minesweeper, then Solitaire. One app each, each with a boot check that plays a scripted game.
- [ ] [Sonnet] Voice Memos: record with `SYS_AUDIO_RECORD`, save a WAV to Files, play it back in Music.
- [ ] [Sonnet] Samantha media tools: "play something", "pause", "what's playing", one check each.
- [ ] [Sonnet] Movie trim: cut, split and join clips at frame boundaries and save a new AVI. The iMovie-lite step.
- [ ] [Sonnet] Preview: opens images and text files from any Files window. PDF is a later, bigger step.
- [ ] [Sonnet] Step sequencer: a drum and synth loop maker with 8-bit mono sound. The GarageBand-lite step.
- [ ] [Sonnet] Docs: Notes grows into a word processor: bold, italic, headings and lists, saved as plain Markdown so any machine can open it. No `.docx`.
- [ ] [Sonnet] Sheets: a grid with formulas (sum, average, min, max, plain arithmetic, cell references), saved as CSV. The app that makes it feel like a real computer.
- [ ] [Haiku] Slides: a Markdown file becomes full-screen slides, arrow keys to move, one title and a few lines per slide.
- [ ] [Fable] Stereo and 16-bit audio: a second `SYS_AUDIO` op so music stops sounding flat. Unblocks a real music studio (VERSIONS 54).
- [ ] [Fable] A real video codec on this CPU class, only after 3.0 hardware shows what it can decode.

## Toward 2.2: Media apps
- [ ] [Sonnet] 2.2.0: Music and Video. Play WAV and MP3 songs from disk; play motion-JPEG AVI movies with sound in sync. Both are protected apps that cannot crash the desktop. Verified by `tools/checks/media-host-check.sh`, `tools/checks/avi-host-check.sh`, `tools/checks/fpu-check.sh`, plus boot checks music-check.py and movie-check.py (being added by concurrent agents).

## Our own computer
The OS stays free. Monetization is custom hardware built to run it. Everything this kernel drives today runs on QEMU's emulated devices; porting to physical hardware comes first, not a coding task yet. 1.0 ships a USB-bootable ISO with a PS/2 fallback; USB is the 1.1 headline, built in this order.

- [ ] [Fable] xHCI USB host controller. Everything below hangs off it.
- [ ] [Fable] USB keyboard and mouse (HID). Also covers "Bluetooth" keyboards that ship with a USB dongle.
- [ ] [Fable] USB storage, so an external SSD mounts.
- [ ] [Fable] USB audio in, so a microphone works. Needs the sound work below.
- [ ] [Fable] True Bluetooth: radio over USB, pairing, keyboard and mouse. Hardest, last.
- [ ] [Fable] Wired internet on real PCs: an Intel e1000 driver next to rtl8139 and ne2k, proven with QEMU `-device e1000`. Most PCs from the last 15 years have an Intel or Realtek chip. Stretch for 1.0; if it slips, the release notes say wired internet is QEMU-only.
- [ ] [Fable] Drivers for physical hardware, not just QEMU's emulated devices: AHCI for real SATA disks, on top of the e1000 work above.
- [ ] [Fable] FAT32, read and write: open a normal USB stick or SD card. Today the disk format is Joshua Tree's own, so nothing from another computer opens.
- [ ] [Fable] Load ELF programs from disk at runtime, so apps can ship outside the kernel. First step to installable apps.
- [ ] [Sonnet] Saving on real PCs stated plainly: today only old IDE disks work.
- [ ] [Fable] User profiles and sign-in done properly: login screen at boot, a home folder and settings per person, passwords stored hashed and never in the clear, lock screen, an admin level for risky actions (issue #28). Accounts exist today; this is the pass that makes them trustworthy.
- [ ] [Fable] Sound on real PCs: an AC97 or Intel HD Audio driver next to the Sound Blaster one (1.6.9), which only exists in emulators.
- [ ] [Sonnet] Laptop basics: battery level, trackpad, lid close.
- [ ] [Joshua] One real PC booted from the USB stick, keyboard and mouse working, photographed. The USB image and non-emulator graphics are only proven in QEMU so far.
- [ ] [Fable] Install to disk from the USB stick.
- [ ] [Fable] Wi-Fi.
- [ ] [Joshua] Trademark search and registration (CIPO or USPTO) for Joshua Tree and the mark before the first boxes ship; a quick search found Joshua Tree Technologies LLC and a JOSHUA TREE VOICE mark, and many STRATA marks in the software and computer classes, so clear the names first.

## Bloomberg terminal
Epiphany is the terminal. Stocks stays a basic ticker widget and never grows into this.

- [ ] [Sonnet] Epiphany command bar inside Joshua Tree: `AAPL GP`, `AAPL DES` style commands pulling live market data (PR #252), landing eventually as a native ring-3 port (see Native ports of fleet apps below).

## Samantha
1. **It can make sound.** The Sound Blaster driver plays audio in QEMU (1.6.9).
2. **She speaks.** Chat reads her answers aloud: her server turns the reply into audio the kernel plays as-is (1.6.9).
3. **It can't hear you yet.** A microphone needs USB support first, then a way to send your voice to Whisper.
4. **Her face is on screen.** Chat shows her face and moves her mouth while she talks, from still frames cut out of her video loops (1.6.9).

- [ ] [Sonnet] Samantha's mail tools ship: "read my email" and "email Mom that I'm late" work in Chat, with a scenario check.
- [ ] [Sonnet] Samantha tools for every app: open, read and write Notes, Files, Calendar, Reminders, Weather, Stocks. A few apps a night, one check each.
- [ ] [Sonnet] Boot straight into Samantha: a kernel command-line flag (`samantha`) opens her full screen after boot.
- [ ] [Joshua] Google project for mail sign-in (OAuth client id), then [Sonnet] sign in with Google in Mail.
- [ ] [Sonnet] Lip-synced face: one Higgsfield lip-sync render of a sentence about Joshua Tree, cut into a viseme library, the server sends a mouth timeline with each reply. The face benchmark (PR #241) must grade it A+ (sync is the gap: best so far 55/100).
- [ ] [Sonnet] Chat listens: speak to Samantha instead of typing. She already answers out loud with her face moving (1.6.9); hearing you needs a microphone path first.
- [ ] [Fable] Samantha on-device: run the Turing project's model inside this kernel instead of over the network. Today she is far too big for a 32-bit kernel with integer-only math, so the first step is a much smaller model and an int8 matmul path. Proof: a headless check that answers one fixed question offline.
- [ ] [Fable] Customization system: users can talk to Samantha to modify OS behavior, colors, fonts, layouts; settings persist on disk in a user-bootstrapped config.
- [ ] [Sonnet] Third-party LLM support: let users choose their preferred model provider (OpenAI, Anthropic, local, etc.).

## Desktop and apps
3 of 26 apps (Burrow, Weather, Mail) can open in their own window, capped at 2 at once; Reminders (1.9.9) and Calendar (1.9.12) left that set for ring 3.

- [x] [Sonnet] Files is now Burrow, with a kit fox peeking out of its burrow for an icon. Samantha still opens it for "files" and "file browser". `tools/checks/burrow-rename-check.py` proves the name, the aliases and the new icon art.
- [ ] [Sonnet] Calendar's Year view shows each day as a dot, not a number. A ring-3 window is 345px tall, which leaves about 10px per week row, and a digit needs 16. Either scale the digit font down for the mini months or let the year view use the full window height. `tools/checks/calviews-check.py` now proves the week bands of dots, so it should be tightened to digits when this lands.
- [ ] [Fable] Per-window backing stores, not drawing straight into the shared framebuffer.
- [ ] [Fable] A compositor with damage tracking, plus the back buffer this kernel still lacks.
- [ ] [Fable] Input routing by focus instead of the current global key/click pull.
- [ ] [Fable] Every app converted from a blocking loop to open/draw/on_key/on_click handlers, or its own task.
- [ ] [Sonnet] Window resize, minimize, and maximize. Needs the compositor above; once it lands, wire up the two unlit traffic-light dots.
- [ ] [Sonnet] Right-click context menus.
- [ ] [Sonnet] App switcher and global hotkeys.
- [ ] [Sonnet] Lock screen, sleep, and ACPI shutdown.
- [ ] [Haiku] Clock app with timer and alarm.
- [ ] [Sonnet] Maps app. The wallpaper already fetches map tiles.
- [ ] [Haiku] Screenshot tool.
- [ ] [Sonnet] Undo in editors.
- [ ] [Sonnet] Files app view options: list, grid, columns, with adjustable sorting and grouping.
- [ ] [Haiku] About This Computer: remove uptime counter, show clean system info.
- [ ] [Sonnet] Mail account configuration: setup for multiple accounts, IMAP/POP/SMTP settings.
- [ ] [Sonnet] Chat app redesign: expanded capabilities, better interface, rename to Sam.
- [ ] [Sonnet] Shell pipes, redirection, and environment variables.
- [ ] [Sonnet] File associations, opening a file in the right app.
- [ ] [Sonnet] Drag and drop.
- [ ] [Sonnet] Notifications posted by apps.
- [ ] [Sonnet] Software update path.
- [ ] [Haiku] Accessibility: text size and high contrast.
- [ ] [Joshua] Landing page polish: slow the demo tour for intimate feel, device-frame chrome, loading states.
- [ ] [Haiku] Window edges and corners: eliminate pixelated rendering, ensure smooth antialiased edges.
- [ ] [Haiku] Dock icon padding and alignment: consistent spacing, balanced visual weight.
- [ ] [Haiku] Window title bar buttons: fit and finish, proper proportions and spacing.
- [ ] [Sonnet] Word processor (Notes) text: improve letter spacing, line height, paragraph margins for better readability.
- [ ] [Sonnet] Lazy-load the boot loading image itself so it never shows visibly pixelated while scaling in. (Found by eye in the 2026-09-21 QA tour, `tools/qa-demo.sh`.)
- [ ] [Sonnet] Mobile Joshua Tree: the landing demo and the OS usable on a phone screen. Mobile first by 2026-10-04.
- [ ] [Sonnet] Photos app: grid of the images on disk, click for full view, arrow keys to move. Built on `drivers/png.c`, plus baseline JPEG if the wallpaper decoder can be reused.
- [ ] [Sonnet] Typeface support: proportional fonts beyond DejaVu, loaded from disk, picked in Settings. Reference look from Joshua: a tight grotesque sans for body and headlines, one display face for titles, hairline rules, flat colour blocks. Sans only in the UI chrome.
- [ ] [Sonnet] Keyboard shortcuts for the snap zones (halves, quarters, full), split out of the snapping item that shipped in #82.
- [ ] [Sonnet] Music app: WAV playback from disk, playlist, play, pause, skip, volume. Unblocked: the Sound Blaster driver shipped in 1.6.9.
- [ ] [Sonnet] Video playback: an MJPEG or raw-frame player synced to audio. Needs the sound driver and a JPEG decoder.
- [ ] [Sonnet] Music app enhancements: equalizer, better playback controls.
- [ ] [Sonnet] Video editor: basic timeline, trimming, and export.
- [ ] [Haiku] Scientific Calculator app: standard and scientific modes, memory functions.
- [ ] [Sonnet] Basic games: Pong, Chess, Conway's Game of Life, fully playable in the OS.
- [ ] [Fable] Dual monitor support: a second framebuffer (QEMU `-device secondary-vga`), the desktop across both, windows dragged between them. Needs the compositor.
- [ ] [Sonnet] Installing and updating apps from inside the OS.
- [ ] [Sonnet] Native Bible app (public-domain KJV on the disk image, book and chapter picker, search).
- [ ] [Sonnet] Settings as a real native app in the dock: wallpaper, text size, system typeface, location, accounts, network status, about. One place, not scattered panels.
- [ ] [Sonnet] Languages: every UI string through one table, a Settings language picker, Latin-1 accents drawn (the DejaVu faces have the glyphs; the text paths drop bytes above 0x7F today).
- [ ] [Sonnet] Ring-3 programs a fresh shell ships with: `cat`, `wc`, `grep`, `calc`. A tiny C compiler is a stretch. First piece landed: `user/libjt/`, a small userland C library (string/ctype/stdlib/stdio) over `jtsys.h`, plus `user/wc.c` built against it (`docs/SYSCALL-ABI.md`'s "Writing a program with libjt"). Still open: porting `hello`/`note` onto it, wiring `WC.BIN` into the disk image and shell dispatch the way `HELLO.BIN`/`NOTE.BIN` already are, and the rest of `cat`/`grep`/`calc`.
- [ ] [Sonnet] Native ports of fleet apps (Epiphany etc.) as ring-3 programs against the syscall ABI.
- [ ] [Sonnet] A split/multiplexed terminal, the buildable substitute for tmux (tmux itself needs pty/job control this kernel lacks).
- [ ] [Sonnet] Moveable dock position, menu bar customization, a network status panel in Settings.

## Tests and the loop
Everything a stranger needs to use it for an hour in the browser or an emulator without getting stuck. See `docs/LOOP-HANDOFF.md` for what's merging right now.

- [ ] [Sonnet] Notes repaints its whole buffer on every key. `tools/checks/editorflash-check.sh` still counts the old in-kernel `editorchrome` marker, which ring-3 Notes never writes. The work: repaint only the text area on plain keys, draw the chrome once, and assert the present counts. Not done in the 2.0.0 CI slice.
- [ ] [Sonnet] Dock polish: no white rim on icons, smooth tray and icon corners, hover label with a backing, loading bar drawn at full resolution, Trash visibly empty or full, Terminal out of the default dock (Files, Mail, Calendar, Notes, Reminders, Chat, Weather, Stocks, Settings, Trash).
- [ ] [Sonnet] Boot splash shows the real engraved tree mark at full resolution, not the stick tree.
- [ ] [Fable] Typography QA across Notes, the document app and the Terminal: spacing between letters, baselines, sizes and weights, every printable character, long lines, wrapping, selection. This kernel is a word processor from scratch, so text gets its own checks.
- [ ] [Sonnet] Chat QA: open Chat, switch between models, sign-in gate for subscription models.
- [ ] [Sonnet] A headless test for every app (open, use, close) in `tools/checks/ci-suite.sh`.
- [ ] [Fable] Error handling audit: corrupt or oversized files, full disk, bad input in every text field, missing disk, network or mouse. Each case gets a check.
- [ ] [Haiku] Build and run documented and checked on Apple Silicon, Intel and AMD hosts.
- [ ] [Fable] Samantha fixes her own bugs: a failing check becomes a draft PR she opens herself (Claude for the hard part at first), gated on `tools/ci-local.sh`. First target: the three compiler warnings in the build.

## Later
Joshua's call, 2026-09-22: **1.0 is a Snow Leopard release.** No new features. Stability, reliability and speed only. New apps and features wait for 1.1.
Joshua's call, 2026-09-21: 1.0.0 is a super thorough QA release. Every feature works, nothing crashes, all text and icons are sharp, and the icons have taste, not a Microsoft look. Real hardware is trusted for now and proven after.
Joshua's call, 2026-09-21: Decided for 1.0, stated in the release notes: no Wi-Fi, no Bluetooth (both need firmware blobs and a full 802.11 or BT stack, months of work each), English only (every UI string is compiled in; a language table is a 1.1 project), no screen reader. Keyboard-only use and large text in Notes are the accessibility floor.

- [ ] [Joshua] V1 product vision: full-color UI with crisp icons and fonts, music and video at 100+ fps, clean typography, mobile-first design discipline across every surface.
- [ ] [Fable] A web browser, which needs secure connections first.
- [ ] [Joshua] Public APIs and webhooks: exposing kernel features over HTTP for external automation and integration.
- [ ] [Joshua] Performance targets: 100+ fps sustained in all apps, instant launch times, memory efficiency on low-spec hardware.
- [ ] [Joshua] Plugins system. Needs a design pass on what a plugin can touch first.
- [ ] [Joshua] AI agent accounts: settings, bootstrapping, auth. Too undefined to scope yet.
- [ ] [Joshua] Boot-to-disk install flow with install-speed numbers. Needs hardware boot support first.
- [ ] [Sonnet] Ring-3 text stem darkening: libjt/text.c blends its 4-bit atlas with only the pre-boost, not the kernel text_ink curve, so ring-3 stems measure core 0.23-0.33 against the kernel text's 0.64 in textsharp-check.py. Port the curve into libjt and raise the check's bars back.

Explicitly parked:
- SMP: one CPU is plenty until everything above works (tracked as Multi-core above, but not scheduled).
- A filesystem journal / crash consistency: FAT read support is enough for now.
- A GUI toolkit (retained widgets, layout, scene graph): one hand-drawn screen doesn't justify one yet.
- Voice control via `gato` (`~/Documents/Code/gato`, a separate macOS app): different project on purpose.
- Steam/gaming support: needs OpenGL/Vulkan and anti-cheat compatibility, off the table for a hobby kernel.

Needs a call from Joshua before scoping:
- "Golden gate, new features?" Unclear reference, needs an answer before triage.
- Proprietary filesystem / server mode / GPU framework: three different-sized ideas in one note, needs unpacking.
- Whether `pmm_total_frames()`'s ~15M default frame ceiling can be raised safely. Not attempted.
- A comparative pass against apple.com/macos's own page structure and animation, for landing-page ideas.
- Device-frame chrome for the browser demo: still undone, needs a redesign that doesn't fight the live 16:9 canvas.

## Session task queue
Feeds the landing page's "Where it's going" card automatically via `tools/gen/landing-roadmap.py`. Keep titles short, bold, and current. Each item also needs a `(plain: ...)` phrase right after the title, a few plain words a 20-year-old visitor would understand with zero dev background, that phrase is what actually shows on the landing page, never the dev title. Internal refactor work that a visitor has no way to try (nothing to click, nothing that looks different) uses `(plain: skip)`, which the generator drops from the card entirely instead of translating it into vague visitor-facing words.
1. **One input bar and a talking tour** (plain: a demo that talks you through it) [Sonnet]: the phone shows two bars and the tour is silent after the intro.
2. **Joshua Tree on a Raspberry Pi** (plain: a real computer you can hold) [Fable]: ARM64 M1c to M4, gated on the first real boot.
3. **Per-check QMP ports** (plain: skip) [Haiku]: parallel test runs stop colliding on fixed ports.
4. **Every icon in one style** (plain: icons that match) [Sonnet]: the fleet icons keep their own tile colors.
5. **Photos, Minesweeper, Solitaire, Voice Memos** (plain: photos, games and voice notes) [Haiku]: the "Apps after 2.2" list.
6. **Rich document app, richer Weather icons, native code editor, package tool** (plain: a word processor, nicer weather art, a code editor, installable apps) [Sonnet]: after the Pi boots.

## Top of the queue after 2.0.0
- [x] Restore the full-bleed Joshua face in ring-3 portfolio mode (2.5.1, user/samantha.c draws the 320 px frame full screen with a glass bar; the kernel gives the portfolio chat a frameless full-screen window).

## Landing roadmap summary
`tools/gen/landing-roadmap.py` reads this file's Session task queue and takes up to three open, numbered, bold task titles for the landing page's "Where it's going" card, skipping completed entries and escaping for HTML. `tools/checks/landing-roadmap-check.py` and `tools/gen/landing-roadmap.py --check` are the regression checks. A roadmap change triggers the landing deploy workflow, which regenerates the card before upload.
