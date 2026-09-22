# Joshua Tree roadmap

Freestanding i386 kernel, no libc. This is the forward plan. What already
shipped lives in `git log`, `git tag -l "jt-v*"`, and the [GitHub
releases](https://github.com/nulljosh/joshuatree/releases), not here.

**Latest**: Calendar, now in Day, Week, Month and Year views.

<!-- NOTE: The **Latest** field is public-facing copy synced to the landing page's h1/eyebrow. Must read as a feature announcement ("Introducing X."), never a changelog line. Update alongside version bumps. tools/gen/inject-landing-headline.sh reads this line automatically. -->

**Model tag on each item**: `[Haiku]` mechanical, known-correct shape, cheap. `[Sonnet]` general feature work with a clear pattern to follow. `[Fable]` anything where a subtly wrong answer still boots fine: privilege isolation, exact register/stack layouts, wire-protocol bytes, memory-model changes. `[Joshua]` a design or scope call, not code. Re-tag if an item turns out easier or harder once opened.

## Beta, 0.9.0
Everything a stranger needs to use it for an hour in the browser or an emulator without getting stuck.
- [ ] [Sonnet] Shell launches a program by name (PR #64).
- [ ] [Sonnet] Ring-3 programs a fresh shell ships with: `cat`, `wc`, `grep`, `calc`. A tiny C compiler is a stretch.
- [ ] [Sonnet] Landing hero, Joshua's spec: demo full width and about 85% of the screen tall, headline and its typewriter line squeezed into the strip under it, sections snap like magnets on scroll, no full screen mode needed. Test on phone and desktop, including reload and the old exit button scrolling too far.
- [ ] [Sonnet] Dock polish: no white rim on icons, smooth tray and icon corners, hover label with a backing, loading bar drawn at full resolution, Trash visibly empty or full, Terminal out of the default dock (Files, Mail, Calendar, Notes, Reminders, Chat, Weather, Stocks, Settings, Trash).
- [ ] [Joshua] Icon set redrawn to Mac-grade taste: depth, soft light, real materials. The current set is sharp but reads like Windows.
- [ ] [Sonnet] Boot splash shows the real engraved tree mark at full resolution, not the stick tree.
- [ ] [Fable] Typography QA across Notes, the document app and the Terminal: spacing between letters, baselines, sizes and weights, every printable character, long lines, wrapping, selection. This kernel is a word processor from scratch, so text gets its own checks.
- [ ] [Sonnet] Settings gets a Location field (city or postal code), saved, used by weather and the wallpaper map. Location from the internet address says Vancouver for Langley and cannot do better.
- [ ] [Sonnet] Chat QA: open Chat, switch between models, sign-in gate for subscription models.
- [ ] [Sonnet] A headless test for every app (open, use, close) in `tools/checks/ci-suite.sh`.
- [ ] [Fable] Error handling audit: corrupt or oversized files, full disk, bad input in every text field, missing disk, network or mouse. Each case gets a check.
- [ ] [Haiku] Build and run documented and checked on Apple Silicon, Intel and AMD hosts.
- [ ] [Sonnet] Magnet-style window snapping, like the Magnet Mac app (dump, 2026-09-21): drag a window to a screen edge for a half, to a corner for a quarter, to the top for full. Show the target outline while dragging. Keyboard shortcuts for the same zones.
- [ ] [Sonnet] Settings as a real native app in the dock: wallpaper, text size, system typeface, location, accounts, network status, about. One place, not scattered panels.
- [ ] [Sonnet] Photos app: grid of the images on disk, click for full view, arrow keys to move. Built on `drivers/png.c`, plus baseline JPEG if the wallpaper decoder can be reused. Covers the image viewer gap.
- [ ] [Sonnet] Typeface support: proportional fonts beyond DejaVu, loaded from disk, picked in Settings. Reference look from Joshua: a tight grotesque sans for body and headlines, one display face for titles, hairline rules, flat colour blocks. Sans only in the UI chrome.
- [ ] [Haiku] Docs refresh: README, WHITEPAPER and ARCHITECTURE reworded and filled out to mirror the structure of the better fleet docs (nimble and tripwire are the reference).

## 1.0.0
Joshua's call, 2026-09-21: 1.0.0 is a super thorough QA release. Every feature works, nothing crashes, all text and icons are sharp, and the icons have taste, not a Microsoft look. Real hardware is trusted for now and proven after.
- [ ] [Haiku] QA gallery: one headless script opens every app from the dock and the Apps folder, uses it, closes it, saves a full resolution screenshot of each, and fails on any crash text in the serial log. Runs in `tools/checks/ci-suite.sh`.
- [ ] [Fable] Main session reviews every gallery screenshot by eye: blurry text, clipped labels, misaligned chrome, soft icons. Each finding becomes a bug line below and gets fixed before the tag.
- [ ] [Sonnet] Every feature exercised once: each app's main action (add a reminder, save a note, send the chat prompt, search, change wallpaper, snap a window), each menu item, each Settings row, each shell command in `help`.
- [ ] [Sonnet] Nothing crashes: bad input in every text field, long lines, empty files, missing disk, no network. Each case gets a check.
- [ ] [Sonnet] Icons with taste: depth, soft light, real materials, consistent corner and light direction across all of them. Judged from the gallery at dock, hover and Apps grid sizes.
- [ ] [Sonnet] Text sharp everywhere: no bitmap fallback font where the antialiased one should draw, no uneven letter gaps, baselines level.
- [ ] [Haiku] Release notes that say what is missing: no sound, no secure web of its own, one core, no install to disk.
- [x] Codename: 1.0 is **Hidden Valley**. Joshua Tree is the name of the computer and the company, the way Apple is; each major release is named after a real place in Joshua Tree National Park. Next in line: Skull Rock, Keys View, Cottonwood, Wonderland. Mojave is skipped, Apple used it. Domain `joshuatreeos.com`.

Decided, not doing: a C++ rewrite (no gain for a freestanding kernel, only risk), and moving the landing page to a `gh-pages` branch.

## After 1.0
What other small operating systems needed before people used them day to day.
- [ ] [Fable] The lag (issue #14) profiled, fixed and measured, with frame time numbers in the release notes.
- [ ] [Joshua] One real PC booted from the USB stick, keyboard and mouse working, photographed. The USB image and non-emulator graphics are only proven in QEMU so far.
- [ ] [Fable] Keyboards on real PCs: a USB keyboard driver, or release notes that say plainly it needs the BIOS legacy keyboard mode.
- [ ] [Sonnet] Saving on real PCs stated plainly: today only old IDE disks work.
- [ ] [Fable] User profiles and sign-in done properly: login screen at boot, a home folder and settings per person, passwords stored hashed and never in the clear, lock screen, an admin level for risky actions (issue #28). Accounts exist today; this is the pass that makes them trustworthy.
- [ ] [Fable] Sound: AC97 driver under QEMU, PCM out. Step one of v100 too.
- [ ] [Sonnet] Music app: WAV playback from disk, playlist, play, pause, skip, volume. Needs the sound driver.
- [ ] [Sonnet] Video playback: an MJPEG or raw-frame player synced to audio. Needs the sound driver and a JPEG decoder.
- [ ] [Fable] Better multitasking: more than two windows at once, an app switcher, apps that keep running in the background. This is the Multi-window section; snapping lands first on the windows that exist.
- [ ] [Fable] Dual monitor support: a second framebuffer (QEMU `-device secondary-vga`), the desktop across both, windows dragged between them. Needs the compositor.
- [ ] [Fable] A web browser, which needs secure connections first.
- [ ] [Fable] Install to disk from the USB stick.
- [ ] [Fable] Wi-Fi.
- [ ] [Sonnet] Installing and updating apps from inside the OS.
- [ ] [Sonnet] Laptop basics: battery level, trackpad, lid close.
- [ ] [Sonnet] Native Bible app (public-domain KJV on the disk image, book and chapter picker, search).
- [ ] [Haiku] Audit that every fleet app has a native port or a line here.

## Bugs
Found by eye in the 2026-09-21 QA tour (`tools/qa-demo.sh`, frames reviewed at full resolution):
- [ ] [Sonnet] Terminal text is unreadable in places: proportional antialiased letters are drawn into fixed-width cells, so "m" is crushed to look like "n" and "i" and "l" float with wide gaps ("hel p", "nen" for "mem"). The Terminal grid needs the Mono face at its real advance.
- [x] Calendar cut the last week of a five-row month in half at the bottom of the window, and never drew a sixth. Rows now size to the window (`tools/checks/apptop-check.py`).
- [ ] [Sonnet] Apps window: black band under the title bar, a fourth row drawn outside the panel and cut in half, two extra icons after Epiphany, "Apps" heading shown twice. In progress.
- [x] Mail, Calendar, Notes, Reminders and Chat left a blank strip about 50px tall under the title bar. They now shift up by `gui_app_dy()` in a window, like Stocks (`tools/checks/apptop-check.py`).
- [ ] [Haiku] `tools/checks/landing-headline-check.sh` fails on `demo-focused` still hiding the hero copy on click in `landing/index.html`. It was silently dead before (it read `roadmap.md` from the repo root); not in `tools/checks/ci-suite.sh` yet.
- [x] Contacts, Calculator, Search and Trash drew their first line at y=52 in a window too; now on `gui_app_dy()` and covered by `tools/checks/apptop-check.py`. Settings only opens full screen from the menu, so it has no window title bar to sit under.
- [x] `tools/checks/gui-prompt-keystroke-check.sh` failed its Calculator case: grid keys sent 0.1 s apart were dropped, so it typed into the Apps folder. Paced at 0.35 s and added to `tools/checks/ci-suite.sh`.
- [x] Dock hover label had no backing and collided with the bottom edge of an open window. Now a cream capsule with a hairline edge, clear of the tray (`tools/checks/dockhover-check.py`).
- [x] `tools/qa_demo_drive.py` was stale: dock geometry from before the eleventh icon, `.` sent as an invalid qcode (the driver, not the keymap), esc-closing Files quit the desktop (PR #83) so the rest of the tour recorded one still, and `screendump` drew stripes headless. It now reads the dock from `kernel/kernel.c`, maps `.` to `dot`, closes each app from its red button and captures with `pmemsave`; `tools/checks/dockslots-check.py` guards the derivation. Weather and Trash reviewed: both clean.
- [x] Esc with Files open quit the whole desktop to text mode (PR #83).
- [x] Apps opened from the Apps folder showed "Apps" in the window frame instead of their own name. `gui_apps_launch` retitles the frame and restores it (`tools/checks/apptop-check.py`).
- [x] The "Memory" text isn't boot output. A VGA text capture every 50 ms through boot shows only SeaBIOS and iPXE; the wording is the clock's notification panel, where `notif_friendly` in `kernel/kernel.c` renders klog's `pmm_init:` and `paging_install:` lines as "Memory initialized" and "Memory protection enabled".
- [ ] [Sonnet] Lazy-load the boot loading image itself so it never shows visibly pixelated while scaling in.

## Gaps vs macOS / Linux / Windows
Things a modern desktop OS has that this kernel doesn't yet.
- [ ] [Fable] No sound at all. Needs an audio driver (AC97 or SB16 under QEMU).
- [ ] [Fable] No native TLS. HTTPS only works through the worker's proxy.
- [ ] [Sonnet] Clipboard copy/paste.
- [ ] [Sonnet] Right-click context menus.
- [ ] [Sonnet] App switcher and global hotkeys.
- [ ] [Sonnet] Lock screen, sleep, and ACPI shutdown.
- [ ] [Sonnet] Image viewer.
- [ ] [Haiku] Clock app with timer and alarm.
- [ ] [Sonnet] Maps app. The wallpaper already fetches map tiles.
- [ ] [Haiku] Screenshot tool.
- [ ] [Sonnet] Text selection and undo in editors.
- [ ] [Sonnet] Shell pipes, redirection, and environment variables.
- [ ] [Sonnet] File associations, opening a file in the right app.
- [ ] [Sonnet] Drag and drop.
- [ ] [Sonnet] Notifications posted by apps.
- [ ] [Fable] Window resize, minimize, and maximize. Needs the compositor below.
- [ ] [Fable] Drivers for physical hardware, not just QEMU's emulated devices (AHCI, e1000).
- [ ] [Fable] Multi-core (SMP).
- [ ] [Sonnet] Software update path.
- [ ] [Haiku] Accessibility: text size and high contrast.
- [ ] In progress: Chat wired to a local Qwen/Bonsai model. Currently talks to Ollama.

## Real hardware
1.0 ships a USB-bootable ISO with a PS/2 fallback. USB is the 1.1 headline, built in this order.
- [ ] [Fable] xHCI USB host controller. Everything below hangs off it.
- [ ] [Fable] USB keyboard and mouse (HID). Also covers "Bluetooth" keyboards that ship with a USB dongle.
- [ ] [Fable] USB storage, so an external SSD mounts.
- [ ] [Fable] USB audio in, so a microphone works. Needs the sound work under Gaps too.
- [ ] [Fable] True Bluetooth: radio over USB, pairing, keyboard and mouse. Hardest, last.

## Multi-window
5 of 23 apps (Files, Weather, Mail, Calendar, Reminders) can open in their own window, capped at 2 at once. Full multi-window still needs:
- [ ] [Fable] Per-window backing stores, not drawing straight into the shared framebuffer.
- [ ] [Fable] A compositor with damage tracking, plus the back buffer this kernel still lacks.
- [ ] [Fable] Input routing by focus instead of the current global key/click pull.
- [ ] [Fable] Every app converted from a blocking loop to open/draw/on_key/on_click handlers, or its own task.
- [ ] [Haiku] Once that lands, wire up the two unlit traffic-light dots (minimize/maximize).

## Engraving design system
One ink on one paper, tone by hatching. Full rule in `CLAUDE.md`'s Theme section.
- [ ] [Fable] Kernel goes 1-bit: ordered dither (4x4 Bayer) replaces every grey and alpha blend.
- [ ] [Sonnet] Window chrome: 1px ink border, hatched title bar, no shadows.
- [ ] [Sonnet] All 23 app icons redrawn as 1-bit line glyphs.
- [ ] [Haiku] Boot splash: the badge drawn scanline by scanline.
- [ ] [Haiku] `landing/icon.svg`, root `icon.svg`, and the app `.icns` redrawn as the simplified tree.

## Bigger, not yet scheduled
- [ ] [Sonnet] File search, Spotlight-style. Needs an index-or-scan design, not a stub.
- [ ] [Sonnet] An Activity Monitor app over the shell's `ps`/`kill`/`mem`. (In PR #62, not merged.)
- [ ] [Fable] A second privilege tier (sudo/admin) on top of the accounts that already exist.
- [ ] [Sonnet] Moveable dock position, menu bar customization, a network status panel in Settings.
- [ ] [Fable] Text rendering: a dedicated pass on AA quality, separate from the font size/weight controls that already exist.
- [ ] [Sonnet] A native Word/Pages-style document app, distinct from Notes.
- [ ] [Sonnet] A native syntax-highlighting code editor.
- [ ] [Sonnet] A native fetch-and-install tool over this kernel's HTTP client, the buildable version of "a package manager."
- [ ] [Sonnet] Native ports of fleet apps (Epiphany etc.) as ring-3 programs against the syscall ABI.
- [ ] [Sonnet] A split/multiplexed terminal, the buildable substitute for tmux (tmux itself needs pty/job control this kernel lacks).
- [ ] [Joshua] Plugins system. Needs a design pass on what a plugin can touch first.
- [ ] [Joshua] AI agent accounts: settings, bootstrapping, auth. Too undefined to scope yet.
- [ ] [Joshua] Boot-to-disk install flow with install-speed numbers. Needs hardware boot support first.

## v100: talk to it with your voice
Not started. The standing long-horizon target: a microphone in, ElevenLabs TTS out. Order, most-blocking first:
1. HDA/AC97 audio driver.
2. USB host controller + USB audio class, for microphone input.
3. Capture loop, then speech-to-text and the ElevenLabs call (plain HTTP, the same shape `chat`'s Ollama call already proves).
4. Camera/video response is a separate, later branch off the same USB prerequisite.

## Free OS, hardware pays for it
The OS stays free. Monetization is custom hardware built to run it. Everything this kernel drives today runs on QEMU's emulated devices; porting to physical hardware comes first, not a coding task yet.

## Explicitly parked
- SMP: one CPU is plenty until everything above works.
- A filesystem journal / crash consistency: FAT read support is enough for now.
- A GUI toolkit (retained widgets, layout, scene graph): one hand-drawn screen doesn't justify one yet.
- Voice control via `gato` (`~/Documents/Code/gato`, a separate macOS app): different project on purpose.
- Steam/gaming support: needs OpenGL/Vulkan and anti-cheat compatibility, off the table for a hobby kernel.

## Needs a call from Joshua before scoping
- "Golden gate, new features?" Unclear reference, needs an answer before triage.
- Proprietary filesystem / server mode / GPU framework: three different-sized ideas in one note, needs unpacking.
- Whether `pmm_total_frames()`'s ~15M default frame ceiling can be raised safely. Not attempted.
- A comparative pass against apple.com/macos's own page structure and animation, for landing-page ideas.
- Device-frame chrome for the browser demo: still undone, needs a redesign that doesn't fight the live 16:9 canvas.

## Session task queue
Feeds the landing page's "Where it's going" card automatically via `tools/gen/landing-roadmap.py`. Keep titles short, bold, and current.
1. **Split kernel.c into per-subsystem files** [Sonnet]: ~7,800 lines, the one god file left here. Don't combine with other kernel.c work.
2. **Rich document app** [Sonnet]: Word/Pages-style paragraph and run formatting, distinct from Notes (which stays plain text).
3. **Native code editor and package tool** [Sonnet]: syntax highlighting, plus a fetch-and-install tool over this kernel's own HTTP client.

## Landing roadmap summary
`tools/gen/landing-roadmap.py` reads this file's Session task queue and takes up to three open, numbered, bold task titles for the landing page's "Where it's going" card, skipping completed entries and escaping for HTML. `tools/checks/landing-roadmap-check.py` and `tools/gen/landing-roadmap.py --check` are the regression checks. A roadmap change triggers the landing deploy workflow, which regenerates the card before upload.
