# Joshua Tree roadmap

Freestanding i386 kernel, no libc. This is the forward plan. What already
shipped lives in `git log`, `git tag -l "jt-v*"`, and the [GitHub
releases](https://github.com/nulljosh/joshuatree/releases), not here.

See `docs/BLUEPRINT.md` for the structural plan of where this OS goes after 1.0.

**Latest**: Introducing Samantha on your phone. Open the demo on a phone and she boots straight up, face and all.

<!-- NOTE: The **Latest** field is public-facing copy synced to the landing page's h1/eyebrow. Must read as a feature announcement ("Introducing X."), never a changelog line. Update alongside version bumps. tools/gen/inject-landing-headline.sh reads this line automatically. -->

**Model tag on each item**: `[Haiku]` mechanical, known-correct shape, cheap. `[Sonnet]` general feature work with a clear pattern to follow. `[Fable]` anything where a subtly wrong answer still boots fine: privilege isolation, exact register/stack layouts, wire-protocol bytes, memory-model changes. `[Joshua]` a design or scope call, not code. Re-tag if an item turns out easier or harder once opened.

## Now (set 2026-09-27)
Samantha runs the machine. Mobile first by 2026-10-04: phone mode, the landing demo and the OS both usable on a phone screen. The PR merge queue moves one at a time, biggest first (see `docs/LOOP-HANDOFF.md` for exactly what's queued today). Full items live in the themed sections below, not here.

## Toward 2.0: apps leave the kernel
- [x] [Fable] Step one, 1.7.7: Keyrate is the first app running as a real ring-3 process (`user/keyrate.c`, launched by `kernel/ring3app.c`) with its own window through two new syscalls (`SYS_WINDOW_OPEN`, `SYS_WINDOW_POLL`) and real crash isolation: a null write inside it is reaped by the kernel, the window is torn down, the desktop comes back. Proven by `tools/checks/ring3app-check.py`. Not 2.0 yet.
- [x] [Fable] Step two, 1.7.11: Toroid runs at ring 3 (`user/toroid.c`, bit-packed grids in its own .data), `kernel/ring3app.c` is one table-driven launcher (`RING3_APPS`), and the in-kernel copies of both Keyrate and Toroid are deleted. `tools/checks/ring3toroid-check.py` proves it draws, closes both ways, crashes safely.
- [x] [Opus] Step three, 1.7.12: Calculator runs at ring 3 (`user/calculator.c`, the same recursive-descent grammar evaluated straight into a double instead of an expr_node tree, since a flat binary has no .bss and no kmalloc), still one row in `RING3_APPS`, and its in-kernel copy is deleted. `tools/checks/ring3calc-check.py` proves it evaluates through the real parser (`12*3 = 36`, `5/0 = 0`), draws, closes both ways, crashes safely.
- [ ] [Sonnet] Port the remaining apps the same way, one PR each, smallest first (Quotes next). Each PR: `user/<app>.c`, a row in `RING3_APPS`, the in-kernel copy deleted once the check passes.
- [ ] [Fable] What the ports will need from the ABI: a font syscall (Keyrate carries its own 8x16 bitmap), a tick clock finer than `SYS_TIME`'s seconds, more than one program window at a time, and the framebuffer pages flipped back to supervisor-only on release (today they are zeroed and re-mapped on the next open).

## Architecture to A+ (refreshed 2026-09-27)
Measured against the closest from-scratch peers: SerenityOS (the one-person-scale benchmark), ToaruOS (own compositor, own libc), KolibriOS (tiny, runs on real PCs), Haiku, and against macOS/Linux. Ranked; the loop works top down. Each line points at the section that closes it.

1. **Real hardware.** Boots only in QEMU: no xHCI USB, no AHCI, no e1000; keyboard needs legacy BIOS mode, saving needs an old IDE disk. The hardware business depends on this. See Our own computer.
2. **A compositor.** Apps draw straight to the framebuffer in blocking loops: at most two windows, no resize or minimize, nothing runs in the background. See Desktop and apps.
3. **Native TLS.** HTTPS goes through the worker proxy, so a browser can't happen yet.
4. **Sound beyond the demo.** The Sound Blaster driver plays audio in QEMU (1.6.9), but every peer ships a music player, and real PCs need AC97 or HD Audio. See Desktop and apps, Our own computer.
5. **Desktop basics.** Undo, right-click menus, drag and drop, an app switcher, a screenshot key. SerenityOS, ToaruOS and KolibriOS all have these (the clipboard landed in 1.0.6, text selection in 1.2.0).
6. **Apps from outside the kernel.** Most apps compile into the kernel; three ring-3 programs exist (Keyrate, Toroid, Calculator). No installer, no update path.
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
- [ ] [Sonnet] An Activity Monitor app over the shell's `ps`/`kill`/`mem`. (In PR #62, not merged.)
- [ ] [Fable] A second privilege tier (sudo/admin) on top of the accounts that already exist.
- [ ] [Fable] Text rendering: a dedicated pass on AA quality, separate from the font size/weight controls that already exist.
- [ ] [Fable] Multi-core (SMP).

Decided, not doing: a C++ rewrite (no gain for a freestanding kernel, only risk), and moving the landing page to a `gh-pages` branch.

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
5 of 23 apps (Files, Weather, Mail, Calendar, Reminders) can open in their own window, capped at 2 at once.

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
1. **Samantha speaks on the landing demo** (plain: hear her voice on the demo) [Sonnet]: measure a real reply end to end (serial speak status, output RMS) now that the tour reboot keeps facehost.
2. **Touch and an on-screen keyboard** (plain: type to her on your phone) [Sonnet]: VERSIONS 1.9.
3. **Apps leave the kernel, each in its own protected space** (plain: apps that can't crash each other) [Sonnet]: the 2.0 gate; one app per PR with its crash check.
4. **Per-check QMP ports** (plain: skip) [Haiku]: parallel test runs stop colliding on fixed ports.
5. **Real Activity and Clock icons** (plain: skip) [Sonnet]: they show placeholder art on the phone grid.
6. **Music and Video players** (plain: music and video apps) [Sonnet]: VERSIONS 2.1 and 2.2.
7. **Rich document app, richer Weather icons, native code editor, package tool** (plain: a word processor, nicer weather art, a code editor, installable apps) [Sonnet]: after 2.0.

## Landing roadmap summary
`tools/gen/landing-roadmap.py` reads this file's Session task queue and takes up to three open, numbered, bold task titles for the landing page's "Where it's going" card, skipping completed entries and escaping for HTML. `tools/checks/landing-roadmap-check.py` and `tools/gen/landing-roadmap.py --check` are the regression checks. A roadmap change triggers the landing deploy workflow, which regenerates the card before upload.
