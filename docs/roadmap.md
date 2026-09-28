# Joshua Tree roadmap

Freestanding i386 kernel, no libc. This is the forward plan. What already
shipped lives in `git log`, `git tag -l "jt-v*"`, and the [GitHub
releases](https://github.com/nulljosh/joshuatree/releases), not here.

See `docs/BLUEPRINT.md` for the structural plan of where this OS goes after 1.0.

**Latest**: Introducing Samantha at boot. Joshua Tree starts on her face, and you just tell her what to do.

<!-- NOTE: The **Latest** field is public-facing copy synced to the landing page's h1/eyebrow. Must read as a feature announcement ("Introducing X."), never a changelog line. Update alongside version bumps. tools/gen/inject-landing-headline.sh reads this line automatically. -->

**Model tag on each item**: `[Haiku]` mechanical, known-correct shape, cheap. `[Sonnet]` general feature work with a clear pattern to follow. `[Fable]` anything where a subtly wrong answer still boots fine: privilege isolation, exact register/stack layouts, wire-protocol bytes, memory-model changes. `[Joshua]` a design or scope call, not code. Re-tag if an item turns out easier or harder once opened.

## Biggest gaps vs a shipping OS (refreshed 2026-09-27)
Measured against the closest from-scratch peers: SerenityOS (the one-person-scale benchmark), ToaruOS (own compositor, own libc), KolibriOS (tiny, runs on real PCs), Haiku, and against macOS/Linux. Ranked; the loop works top down. Each line points at the items below that close it.

1. **Real hardware.** Boots only in QEMU: no xHCI USB, no AHCI, no e1000; keyboard needs legacy BIOS mode, saving needs an old IDE disk. The hardware business depends on this. See Real hardware, e1000 under 1.0.0.
2. **A compositor.** Apps draw straight to the framebuffer in blocking loops: at most two windows, no resize or minimize, nothing runs in the background. See Multi-window.
3. **Native TLS.** HTTPS goes through the worker proxy, so a browser can't happen yet.
4. **Sound beyond the demo.** The Sound Blaster driver plays audio in QEMU (1.6.9), but every peer ships a music player, and real PCs need AC97 or HD Audio. See Music app, Sound under After 1.0.
5. **Desktop basics.** Undo, right-click menus, drag and drop, an app switcher, a screenshot key. SerenityOS, ToaruOS and KolibriOS all have these (the clipboard landed in 1.0.6, text selection in 1.2.0).
6. **Apps from outside the kernel.** All 25 apps compile into the kernel; two ring-3 programs exist. No installer, no update path.
7. **Everyday apps peers ship.** An image viewer, a music player, a few games. KolibriOS ships dozens in under 2MB. See Photos, Music, Basic games under After 1.0.

## This week (set 2026-09-27)
Samantha runs the machine. Joshua's list, in order.

- [ ] [Sonnet] Samantha's mail tools ship: "read my email" and "email Mom that I'm late" work in Chat, with a scenario check.
- [ ] [Sonnet] Samantha tools for every app: open, read and write Notes, Files, Calendar, Reminders, Weather, Stocks. A few apps a night, one check each.
- [ ] [Sonnet] Boot straight into Samantha: a kernel command-line flag (`samantha`) opens her full screen after boot.
- [ ] [Fable] HTTPS without the proxy: native TLS 1.3 in the kernel, then the web browser on top of it.
- [ ] [Joshua] Google project for mail sign-in (OAuth client id), then [Sonnet] sign in with Google in Mail.
- [ ] [Sonnet] Lip-synced face: one Higgsfield lip-sync render of a sentence about Joshua Tree, cut into a viseme library, the server sends a mouth timeline with each reply. the face benchmark (PR #241) must grade it A+ (sync is the gap: best so far 55/100).
- [ ] [Sonnet] Mobile Joshua Tree: the landing demo and the OS usable on a phone screen.
- [ ] [Fable] Samantha fixes her own bugs: a failing check becomes a draft PR she opens herself (Claude for the hard part at first), gated on `tools/ci-local.sh`. First target: the three compiler warnings in the build.

## From the nanobyte "Building an OS" series (set 2026-09-27)
What that series covers that Joshua Tree still doesn't, ranked for a dev kit that boots real PCs.

- [ ] [Fable] FAT32, read and write: open a normal USB stick or SD card. Today the disk format is Joshua Tree's own, so nothing from another computer opens.
- [ ] [Fable] Load ELF programs from disk at runtime, so apps can ship outside the kernel. First step to installable apps (gap 6 above).
- [ ] [Sonnet] Crash reports with function names: build a symbol table into the kernel and print `panic in <function>+offset` over serial and on screen.

## Our own computer (set 2026-09-27)
Joshua Tree boots in QEMU and a browser. A computer means it boots a real box on a desk. Ranked by what blocks that first.

- [ ] [Joshua] Pick one reference machine (a common mini PC) and target its real chips. Every driver below aims at that box first.
- [ ] [Fable] UEFI boot and a native-resolution framebuffer (GOP). Modern PCs don't boot legacy BIOS images.
- [ ] [Fable] USB (xHCI): keyboard, mouse and a USB stick. Real machines have no PS/2 port.
- [ ] [Fable] A real disk: AHCI first, NVMe next.
- [ ] [Sonnet] A real network card: e1000 (draft PR 151) then Realtek RTL8111, the chip in most mini PCs.
- [ ] [Fable] HD Audio, so sound works outside the emulator's Sound Blaster.
- [ ] [Fable] ACPI: shut down, restart, sleep. Today the power button is QEMU's.
- [ ] [Fable] 64-bit and more than one CPU core. Every desktop chip since 2006 is 64-bit and multi-core.

## A Bloomberg terminal on it (set 2026-09-27)
Epiphany is the terminal; Stocks stays the basic native app. The live quotes and charts through the Worker (1.0.4) are shared. A terminal is keyboard-first, dense and live, so everything below lands in Epiphany.

- [ ] [Sonnet] Command bar with function codes: `AAPL GP` price graph, `AAPL DES` description, `TOP` headlines, `WEI` world indices, `FX`, `CRYPTO`. Type, Enter, it's there.
- [ ] [Sonnet] Streaming quotes: the Worker relays ticks every second, prices flash green or red as they move.
- [ ] [Sonnet] Real charts: candlesticks, volume, 1m/5m/1D ranges, moving averages and RSI, a crosshair that reads the value.
- [ ] [Sonnet] Four panels at once, keyboard-switched, and an amber-on-black terminal theme.
- [ ] [Sonnet] News wire: ticker-tagged headlines from the sidewise RSS API, newest first.
- [ ] [Sonnet] Alerts: "tell me when NVDA crosses 150", then a notification and Samantha says it out loud.
- [ ] [Sonnet] Portfolio P&L from Epiphany: positions, cost basis, day and total gain.
- [ ] [Sonnet] Ask Samantha "how's Apple doing": quote, day move and top headline, spoken.
- [ ] [Fable] Frame budget: a full terminal redraw under 16ms, measured and printed in the release notes.

## The loop itself (set 2026-09-27)
What slowed tonight down, fixed so it can't again.

- [ ] [Sonnet] Bump the version at merge time, not per PR. Four parallel PRs all claimed 1.6.21 tonight and every merge after the first needs a rebump.
- [ ] [Sonnet] A merge queue script: take the next draft PR, rebase it, run `tools/ci-local.sh`, flip it ready, merge, repeat. One at a time.
- [ ] [Haiku] The Dock icon-edge check retries its VM connection on a reset instead of failing (flaked once on 2026-09-27).
- [ ] [Sonnet] A size budget check on the kernel's reserved memory, so a feature can't silently grow into the app area again (PR 239 did, by 4KB, only on Linux).

## Architecture to A+ (set 2026-09-27, graded C+)
Measured: kernel.c is about 9,800 lines and 84 more files are pasted into it at build time, so the whole OS compiles as one unit. All 25 apps run inside the kernel and are wired in through hand-edited switch statements on GUI_LABELS. Generated artwork lives in C headers (the wallpaper alone is 17,000 lines). Each step below leaves the OS working and shippable.

- [ ] [Fable] App interface: every app provides open, draw, key and close in one `struct app` registered in a single table; the GUI_LABELS switches go away. Move one app (Keyrate) to its own `.c` as the pattern.
- [ ] [Sonnet] Move the remaining apps behind the interface, a few per PR, each in its own `.c`.
- [ ] [Fable] Split kernel.c into separately compiled parts (window manager, desktop, network glue), each behind a small header. The god-file check's limit ratchets down with every split.
- [ ] [Sonnet] Artwork as data: wallpaper, icons and marks packed into binary blobs at build time (objcopy), not C source.
- [ ] [Sonnet] Layer rules, checked: drivers never call the desktop, apps only use the app interface and syscalls. A check reads the include graph and fails on a reach-through.
- [ ] [Fable] Apps out of the kernel: one app at a time becomes a ring-3 program on libjt, so a crashing app can't take the machine down.

## Beta, 0.9.0
Everything a stranger needs to use it for an hour in the browser or an emulator without getting stuck.
- [ ] [Sonnet] Dock polish: no white rim on icons, smooth tray and icon corners, hover label with a backing, loading bar drawn at full resolution, Trash visibly empty or full, Terminal out of the default dock (Files, Mail, Calendar, Notes, Reminders, Chat, Weather, Stocks, Settings, Trash).
- [ ] [Sonnet] Boot splash shows the real engraved tree mark at full resolution, not the stick tree.
- [ ] [Fable] Typography QA across Notes, the document app and the Terminal: spacing between letters, baselines, sizes and weights, every printable character, long lines, wrapping, selection. This kernel is a word processor from scratch, so text gets its own checks.
- [ ] [Sonnet] Chat QA: open Chat, switch between models, sign-in gate for subscription models.
- [ ] [Sonnet] A headless test for every app (open, use, close) in `tools/checks/ci-suite.sh`.
- [ ] [Fable] Error handling audit: corrupt or oversized files, full disk, bad input in every text field, missing disk, network or mouse. Each case gets a check.
- [ ] [Haiku] Build and run documented and checked on Apple Silicon, Intel and AMD hosts.

## 1.0.0
Joshua's call, 2026-09-22: **1.0 is a Snow Leopard release.** No new features. Stability, reliability and speed only: every item below makes what already exists crash less, lose less and run faster. New apps and features wait for 1.1 (see After 1.0).
Joshua's call, 2026-09-21: 1.0.0 is a super thorough QA release. Every feature works, nothing crashes, all text and icons are sharp, and the icons have taste, not a Microsoft look. Real hardware is trusted for now and proven after.
- [ ] [Fable] The lag (issue #14) profiled, fixed and measured, with frame time numbers in the release notes.
- [ ] [Sonnet] Nothing crashes: bad input in every text field, long lines, empty files, missing disk, no network. Each case gets a check.
  - 1.0.10: empty, oversized, corrupt-FAT and full-disk cases; `tools/checks/filerobust-check.py`.
- [ ] [Sonnet] Icons with taste: depth, soft light, real materials, consistent corner and light direction across all of them. Judged from the gallery at dock, hover and Apps grid sizes.
  - 0.89.0: the 11 dock icons redrawn Big Sur style (one top light, no outlines, 148px art at an exact 2:1); `tools/checks/iconlight-check.py`. Still to do: the 15 Apps-folder fleet icons, and a live date on Calendar's tile.
- [ ] [Sonnet] Text sharp everywhere: no bitmap fallback font where the antialiased one should draw, no uneven letter gaps, baselines level.
  - 0.89.0: coverage-to-ink curve (stem darkening) sharpens every AA text path; `tools/checks/textsharp-check.py`.
  - 1.0.9: a retina-bar typography pass judged from real headless crops (`/tmp/jt-loop/typography-crops`, this pass's own review output, not committed to the repo) across the menu bar, a title bar, Notes' paragraph, Terminal's prompt/output, a Settings row, the dock hover label and the Apps folder grid. Most of those already read clean (proportional AA, dense stems, level baselines, even letter gaps) thanks to the earlier v44/v50/v77/v78/v79/v82/0.89.0 passes; the one real defect was the Apps folder's glass panel, sized 19 logical px short of the grid it actually draws (`APPS_VIS_ROWS * cell_h` measured from the panel's own top edge, when the grid really starts 70px lower to clear its own hint line), so the bottom visible row's labels sat only ~9 logical px above the panel's real edge, title-bar-tight everywhere else in this UI. Fixed (`APPS_PANEL_H` 375 -> 410, plus a second copy of the same stale-pixel-height bug in the click hit test); `tools/checks/baseline-check.py` measures container padding, baseline flatness and letter-gap variance on real pixels so this class of bug (a fine but not fully sufficient panel/cell-height budget) cannot come back silently.
- [ ] [Haiku] Accessibility floor: every app opens and closes by keyboard alone. In progress in #127 (Enter on the desktop opens the Apps folder; the check still has to prove all 25). Ships as 1.0.x, not a blocker for the tag.
- [ ] [Fable] Wired internet on real PCs: an Intel e1000 driver next to rtl8139 and ne2k, proven with QEMU `-device e1000`. Most PCs from the last 15 years have an Intel or Realtek chip, so this is what makes the network real off the emulator. Stretch for 1.0; if it slips, the release notes say wired internet is QEMU-only.
- [ ] [Joshua] Decided for 1.0, stated in the release notes: no Wi-Fi, no Bluetooth (both need firmware blobs and a full 802.11 or BT stack, months of work each), English only (every UI string is compiled in; a language table is a 1.1 project), no screen reader. Keyboard-only use and large text in Notes are the accessibility floor.

Decided, not doing: a C++ rewrite (no gain for a freestanding kernel, only risk), and moving the landing page to a `gh-pages` branch.

## After 1.0
What other small operating systems needed before people used them day to day.
- [ ] [Joshua] V1 product vision: full-color UI with crisp icons and fonts, music and video at 100+ fps, clean typography, mobile-first design discipline across every surface.
- [ ] [Fable] Bluetooth: a USB HCI transport and enough of the stack for a keyboard and mouse.
- [ ] [Sonnet] Languages: every UI string through one table, a Settings language picker, Latin-1 accents drawn (the DejaVu faces have the glyphs; the text paths drop bytes above 0x7F today).
- [ ] [Sonnet] Ring-3 programs a fresh shell ships with: `cat`, `wc`, `grep`, `calc`. A tiny C compiler is a stretch. First piece landed: `user/libjt/`, a small userland C library (string/ctype/stdlib/stdio) over `jtsys.h`, plus `user/wc.c` built against it (`docs/SYSCALL-ABI.md`'s "Writing a program with libjt"). Still open: porting `hello`/`note` onto it, wiring `WC.BIN` into the disk image and shell dispatch the way `HELLO.BIN`/`NOTE.BIN` already are, and the rest of `cat`/`grep`/`calc`.
- [x] [Sonnet] Settings gets a Location field (city or postal code), saved, used by weather and the wallpaper map. Location from the internet address says Vancouver for Langley and cannot do better. Done in v1.0.5: `loc_geocode` resolves the typed text through Open-Meteo's own geocoding endpoint and writes straight into the same `geo_lat`/`geo_lon`/`geo_city` fields `weather_fetch_inner` and the map's `wall_fetch` already read, persisted through `SETTINGS.TXT` alongside wind/dock/wall; `tools/checks/location-check.py` proves the geocode, the save/reload round trip, and the not-found/empty fallback headlessly against a local mock server, never real internet.
- [ ] [Sonnet] Settings as a real native app in the dock: wallpaper, text size, system typeface, location, accounts, network status, about. One place, not scattered panels.
- [ ] [Sonnet] Photos app: grid of the images on disk, click for full view, arrow keys to move. Built on `drivers/png.c`, plus baseline JPEG if the wallpaper decoder can be reused. Covers the image viewer gap.
- [ ] [Sonnet] Typeface support: proportional fonts beyond DejaVu, loaded from disk, picked in Settings. Reference look from Joshua: a tight grotesque sans for body and headlines, one display face for titles, hairline rules, flat colour blocks. Sans only in the UI chrome.
- [ ] [Sonnet] Keyboard shortcuts for the snap zones (halves, quarters, full), split out of the snapping item that shipped in #82.
- [ ] [Joshua] One real PC booted from the USB stick, keyboard and mouse working, photographed. The USB image and non-emulator graphics are only proven in QEMU so far.
- [ ] [Fable] Keyboards on real PCs: a USB keyboard driver, or release notes that say plainly it needs the BIOS legacy keyboard mode.
- [ ] [Sonnet] Saving on real PCs stated plainly: today only old IDE disks work.
- [ ] [Fable] User profiles and sign-in done properly: login screen at boot, a home folder and settings per person, passwords stored hashed and never in the clear, lock screen, an admin level for risky actions (issue #28). Accounts exist today; this is the pass that makes them trustworthy.
- [ ] [Fable] Sound on real PCs: an AC97 or Intel HD Audio driver next to the Sound Blaster one (1.6.9), which only exists in emulators.
- [ ] [Sonnet] Music app: WAV playback from disk, playlist, play, pause, skip, volume. Unblocked: the Sound Blaster driver shipped in 1.6.9.
- [ ] [Sonnet] Video playback: an MJPEG or raw-frame player synced to audio. Needs the sound driver and a JPEG decoder.
- [ ] [Sonnet] Music app enhancements: equalizer, better playback controls.
- [ ] [Sonnet] Video editor: basic timeline, trimming, and export.
- [ ] [Haiku] Scientific Calculator app: standard and scientific modes, memory functions.
- [ ] [Sonnet] Basic games: Pong, Chess, Conway's Game of Life, fully playable in the OS.
- [ ] [Sonnet] Chat listens: speak to Samantha instead of typing. She already answers out loud with her face moving (1.6.9); hearing you needs a microphone path first.
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
- [ ] [Sonnet] Lazy-load the boot loading image itself so it never shows visibly pixelated while scaling in.

## Gaps vs macOS / Linux / Windows
Things a modern desktop OS has that this kernel doesn't yet.
- [ ] [Fable] No native TLS. HTTPS only works through the worker's proxy.
- [x] [Sonnet] Clipboard copy/paste. Shipped 1.0.6: one global 4KB buffer, Ctrl+C/X/V in Notes, Terminal, and every field built on `gui_prompt.h` (Mail, Reminders, Calculator). Until 1.2.0 there was no selection model, so Ctrl+C/X acted on the current line or field. Notes now uses its selection when one is active and falls back to the line otherwise.
- [ ] [Sonnet] Right-click context menus.
- [ ] [Sonnet] App switcher and global hotkeys.
- [ ] [Sonnet] Lock screen, sleep, and ACPI shutdown.
- [ ] [Sonnet] Image viewer.
- [ ] [Haiku] Clock app with timer and alarm.
- [ ] [Sonnet] Maps app. The wallpaper already fetches map tiles.
- [ ] [Haiku] Screenshot tool.
- [x] 1.2.0: Text selection in Notes. Shift+arrow extends it, Ctrl+A selects everything, a light-blue band highlights it, Ctrl+C/X/typing/Backspace/Delete act on it, Escape or a plain arrow clears it. The landing demo's Notes scene selects and copies its last word for real. Check: `tools/checks/textselect-check.py` (highlight position, clipboard hashes, cut, type-over, and a collapsed selection that must not eat the next character).
- [ ] [Sonnet] Undo in editors.
- [ ] [Sonnet] Files app view options: list, grid, columns, with adjustable sorting and grouping.
- [ ] [Haiku] About This Computer: remove uptime counter, show clean system info.
- [ ] [Sonnet] Mail account configuration: setup for multiple accounts, IMAP/POP/SMTP settings.
- [ ] [Sonnet] Chat app redesign: expanded capabilities, better interface, rename to Sam.
- [ ] [Sonnet] Notes typography and rendering: eliminate pixel artifacts, ensure retina-sharp text everywhere.
- [ ] [Sonnet] Shell pipes, redirection, and environment variables.
- [ ] [Sonnet] File associations, opening a file in the right app.
- [ ] [Sonnet] Drag and drop.
- [ ] [Sonnet] Notifications posted by apps.
- [ ] [Fable] Window resize, minimize, and maximize. Needs the compositor below.
- [ ] [Fable] Drivers for physical hardware, not just QEMU's emulated devices (AHCI, e1000).
- [ ] [Fable] Multi-core (SMP).
- [ ] [Sonnet] Software update path.
- [ ] [Haiku] Accessibility: text size and high contrast.
- [x] [Sonnet] Chat talks to Samantha (Turing's Ollama-compatible `/api/chat` at `turing.heyitsmejosh.com`) by default, natively over HTTP and in the browser demo through the worker proxy. `tools/checks/chat-samantha-check.py`.
- [x] 1.0.12: Chat talks to Samantha, the model from the Turing project, over the network by default. If a host answers with a redirect to HTTPS (this kernel has no TLS), Chat says so instead of failing quietly. Proven against the real host in CI. Checks: `tools/checks/chat-samantha-check.py`, `tools/checks/chatapp-check.py`.
- [x] 1.1.0: Samantha can act, not just answer. Chat asks Turing's picker first; when it names something this OS can do (a reminder, a note, opening an app, the cached weather, today's calendar), the kernel does it locally and skips the model. Plain questions go to Samantha as before. Proven against the real picker in CI. Check: `tools/checks/chattools-check.py`.
- [x] 1.1.1: CI hygiene. A run superseded by a newer push now ends as cancelled instead of failed, so it stops emailing. Version names are bare numbers (`1.0.13`, not `jt-v1.0.13`). Two checks that a leftover merge conflict had silently skipped run again.
- [x] 1.1.2: The CI network job's wallpaper checks had been failing silently since the default theme became Satellite: they compared satellite tiles against map tiles. They now boot in the theme they test, the map compose matches the host byte for byte on a real runner, and an offline compose check runs in the suite. CI runs on pull requests only, so there is no duplicate run on main. Check: `tools/checks/wallcompose-check.py`.
- [x] 1.1.4: The landing demo's pointer glides to where it is going instead of teleporting: each tour move is an eased walk of about half a second. Check: `tools/checks/cursorglide-check.mjs`.
- [x] 1.1.5: Reliability. Chat now gives up on a silent Samantha host after a bounded wait (about 10s for the picker, 45s for a reply) instead of freezing the GUI for minutes. The clipboard and file checks no longer flake or hang on a slow runner. A stray `node_modules` symlink that 1.1.4 committed is removed and ignored. Check: `tools/checks/chat-timeout-check.py`.
- [ ] [Joshua] Landing page polish: slow the demo tour for intimate feel, device-frame chrome, loading states.
- [ ] [Haiku] Window edges and corners: eliminate pixelated rendering, ensure smooth antialiased edges.
- [ ] [Haiku] Dock icon padding and alignment: consistent spacing, balanced visual weight.
- [ ] [Haiku] Calendar icon: add visual date representation, proper padding around the numeral.
- [ ] [Haiku] Menu bar weather: correct text color for visibility across themes.
- [ ] [Haiku] Window title bar buttons: fit and finish, proper proportions and spacing.
- [ ] [Sonnet] Word processor (Notes) text: improve letter spacing, line height, paragraph margins for better readability.

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
- [x] 1.0.11: Every window drags live by its title bar, including the single-window apps that used to close on any click. Only the strip of wallpaper you uncover repaints, so it stays smooth. Check: `tools/checks/windowdrag-check.py`.
- [x] 1.0.13: The landing demo shows things instead of claiming them: the tour drags the Notes window by its title bar, and a headless browser check boots the kernel in v86 and proves Chat's reply actually renders on screen. That check gates every merge. Check: `tools/checks/demochat-check.mjs`.

## Engraving design system
One ink on one paper, tone by hatching. Full rule in `CLAUDE.md`'s Theme section.
- [ ] [Fable] Kernel goes 1-bit: ordered dither (4x4 Bayer) replaces every grey and alpha blend.
- [ ] [Sonnet] Window chrome: 1px ink border, hatched title bar, no shadows.
- [ ] [Sonnet] All 23 app icons redrawn as 1-bit line glyphs.
- [ ] [Haiku] Boot splash: the badge drawn scanline by scanline.
- [ ] [Haiku] `landing/icon.svg`, root `icon.svg`, and the app `.icns` redrawn as the simplified tree.

## Bigger, not yet scheduled
- [ ] [Fable] Samantha on-device: run the Turing project's model inside this kernel instead of over the network. Today she is far too big for a 32-bit kernel with integer-only math, so the first step is a much smaller model and an int8 matmul path. Proof: a headless check that answers one fixed question offline.
- [ ] [Fable] Customization system: users can talk to Samantha to modify OS behavior, colors, fonts, layouts; settings persist on disk in a user-bootstrapped config.
- [ ] [Sonnet] Third-party LLM support: let users choose their preferred model provider (OpenAI, Anthropic, local, etc.).
- [ ] [Joshua] Public APIs and webhooks: exposing kernel features over HTTP for external automation and integration.
- [ ] [Joshua] Performance targets: 100+ fps sustained in all apps, instant launch times, memory efficiency on low-spec hardware.
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
The goal: you talk to the computer, and Samantha answers out loud with her face on screen.

Where it stands:
1. **It can make sound.** The Sound Blaster driver plays audio in QEMU (1.6.9).
2. **She speaks.** Chat reads her answers aloud: her server turns the reply into audio the kernel plays as-is (1.6.9).
3. **It can't hear you yet.** A microphone needs USB support first, then a way to send your voice to Whisper.
4. **Her face is on screen.** Chat shows her face and moves her mouth while she talks, from still frames cut out of her video loops (1.6.9). The landing demo is next.

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
Feeds the landing page's "Where it's going" card automatically via `tools/gen/landing-roadmap.py`. Keep titles short, bold, and current. Each item also needs a `(plain: ...)` phrase right after the title, a few plain words a 20-year-old visitor would understand with zero dev background — that phrase is what actually shows on the landing page, never the dev title. Internal refactor work that a visitor has no way to try (nothing to click, nothing that looks different) uses `(plain: skip)`, which the generator drops from the card entirely instead of translating it into vague visitor-facing words.
1. ~~**Portfolio apps get real demos**~~ Done 2026-09-26: the portfolio tour now drives each dock app with real keypresses (embed.js PORTFOLIO_SCRIPTS), Epiphany carries a 40-ticker live watchlist (/api/quotes), and Curbfind shows live Craigslist deals for the visitor's own city (/api/deals off Cloudflare geo-IP).
2. **Check the portfolio in X's in-app browser** (plain: skip) [Haiku]: `?full` now boots right away instead of waiting on an IntersectionObserver. Confirm on a phone by opening heyitsmejosh.com from a post in the X app. If it still hangs, capture what shows.
3. **Rich document app** (plain: a word processor) [Sonnet]: Word/Pages-style paragraph and run formatting, distinct from Notes (which stays plain text).
4. **Richer Weather icons** (plain: nicer weather art) [Sonnet]: Joshua's note, 2026-09-25: "that cloud icon is nice, bump it". Carry the big current-conditions cloud's shaded style into every glyph: sun, rain, snow, fog, storm, larger and layered, in the forecast cards and the menu bar too. Same pass for the temperature type: the big reading in the real TTF display face with a proper degree sign and optical C/F unit, and the menu bar's "14∘" drawn with a real degree glyph at the right size and baseline.
5. **Split kernel.c into per-subsystem files** (plain: skip) [Sonnet]: ~7,800 lines, the one god file left here. Don't combine with other kernel.c work. Internal refactor only, nothing a visitor can see or try, so it's excluded from the landing card.
6. **Native code editor** (plain: a code editor) [Sonnet]: syntax highlighting for the native editor app.
7. **Package/install tool** (plain: apps you can install) [Sonnet]: a fetch-and-install tool over this kernel's own HTTP client.

## Landing roadmap summary
`tools/gen/landing-roadmap.py` reads this file's Session task queue and takes up to three open, numbered, bold task titles for the landing page's "Where it's going" card, skipping completed entries and escaping for HTML. `tools/checks/landing-roadmap-check.py` and `tools/gen/landing-roadmap.py --check` are the regression checks. A roadmap change triggers the landing deploy workflow, which regenerates the card before upload.

## 1.0.4: Stocks uses market data

The fixed watchlist now fetches real quotes and chart closes through the existing Worker. This covers native Joshua Tree and the same kernel embedded in the portfolio. Prices refresh once a minute while Stocks is open, with R for retry, UTC quote timestamps and stale/unavailable states. The provider may delay quotes; closed markets show the last session. Synthetic charts and invented daily statistics are removed. Epiphany's sample portfolio is kept separate. Worker route checks, an ASan/UBSan harness of the actual C parser, a live upstream request and a headless boot verify the path.

## 1.3.0: Chat reads like a product, and the demo proves it

Chat's window used to open on a debug header ("samantha turing.heyitsmejosh.com:80 ready"), a bare ">>>" prompt and one wall-of-text "what can you do?" reply on the landing page. The header now just says "Samantha" and its state; the model/host/port are still the real values Settings edits, they just aren't printed on screen anymore. Every line is now labeled "You:" or "Samantha:" instead of the old REPL-style prompt, and the footer only lists the three keys. The landing tour's Chat scene now runs five real actions in sequence, paced so a visitor can read each one: add a reminder, take a note, check the weather, read today's calendar, and open another app, each showing its own confirmation line as it happens. `tools/checks/demochat-check.mjs` asserts every scene by intercepting the same `/api/pick` round trip the kernel makes for each one.

## 1.3.1: Calendar dock tile padding and menu bar weather color

The Calendar dock icon's day number ("25") used to run edge to edge on the tile with almost no side margin, its stroke crossing into the tile's own bottom curve: sized for the bigger Apps-folder grid tile and never checked against the dock's own smaller one. Shrunk to match the same inset every other dock glyph keeps off the squircle. Menu bar weather text now uses the clock's own ink color instead of a warm brown, so it reads on every wallpaper theme. `tools/checks/calicon-check.py` gained a margin check (fails if date ink lands in the tile's own side margin or against its bottom edge, confirmed failing on the old sizing and passing on the fix).
