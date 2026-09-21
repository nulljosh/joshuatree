# Joshua Tree roadmap

Freestanding i386 kernel, no libc. This is the forward plan. What already
shipped lives in `git log`, `git tag -l "jt-v*"`, and the [GitHub
releases](https://github.com/nulljosh/joshuatree/releases), not here.

**Latest**: Portfolio, a catalog of every app in the fleet, right on the desktop.

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

## 1.0.0
A release that can be defended. On top of the beta and the frozen syscall ABI (`docs/SYSCALL-ABI.md`):
- [ ] [Fable] The lag (issue #14) profiled, fixed and measured, with frame time numbers in the release notes.
- [ ] [Joshua] One real PC booted from the USB stick, keyboard and mouse working, photographed. The USB image and non-emulator graphics are only proven in QEMU so far.
- [ ] [Fable] Keyboards on real PCs: a USB keyboard driver, or release notes that say plainly it needs the BIOS legacy keyboard mode.
- [ ] [Sonnet] Saving on real PCs stated plainly: today only old IDE disks work.
- [ ] [Fable] User profiles and sign-in done properly: login screen at boot, a home folder and settings per person, passwords stored hashed and never in the clear, lock screen, an admin level for risky actions (issue #28). Accounts exist today; this is the pass that makes them trustworthy.
- [ ] [Haiku] Release notes that say what is missing: no sound, no secure web of its own, one core, no install to disk.
- [ ] [Joshua] Codename for the release. The name stays Joshua Tree, domain `joshuatreeos.com`.

Decided, not doing: a C++ rewrite (no gain for a freestanding kernel, only risk), and moving the landing page to a `gh-pages` branch.

## After 1.0
What other small operating systems needed before people used them day to day.
- [ ] [Fable] A web browser, which needs secure connections first.
- [ ] [Fable] Install to disk from the USB stick.
- [ ] [Fable] Wi-Fi.
- [ ] [Sonnet] Installing and updating apps from inside the OS.
- [ ] [Sonnet] Laptop basics: battery level, trackpad, lid close.
- [ ] [Sonnet] Native Bible app (public-domain KJV on the disk image, book and chapter picker, search).
- [ ] [Haiku] Audit that every fleet app has a native port or a line here.

## Bugs
- [ ] [Sonnet] Apps opened from the Apps folder show "Apps" in the window frame instead of their own name (`GUI_LABELS[GUI_APPS_FOLDER]`).
- [ ] [Haiku] "Memory management" boot text isn't in kernel/boot/drivers/landing source; `klog` is serial-only. Confirm with a boot frame capture.
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
