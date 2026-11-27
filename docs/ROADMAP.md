# Joshua Tree roadmap and history

Where it is going, and what has shipped. The open items are kept here; finished work is in the Shipped list and the GitHub releases.

## Roadmap

A freestanding kernel for i386 and the Raspberry Pi 4, no libc. This is the forward plan; what shipped is in the [Shipped list](#shipped) and the [releases](https://github.com/nulljosh/joshuatree/releases).

**Latest**: 13.22: GPU drawing.

<!-- NOTE: The **Latest** field is public-facing copy synced to the landing page's h1/eyebrow. Must read as a feature announcement ("Introducing X."), never a changelog line. Update alongside version bumps. tools/gen/inject-landing-headline.sh reads this line automatically. -->

**Model tag on each item**: `[Haiku]` mechanical, known-correct shape, cheap. `[Sonnet]` general feature work with a clear pattern to follow. `[Opus]` anything where a subtly wrong answer still boots fine: privilege isolation, exact register/stack layouts, wire-protocol bytes, memory-model changes. `[Joshua]` a design or scope call, not code. Re-tag if an item turns out easier or harder once opened. (2026-11-27: Fable left the plan, so its items are Opus now.)

## Next priorities (reviewed 2026-11-27)
**Now: 14.0, graphics acceleration** (Joshua, 2026-11-02: "do v12-15"). The Pi 4's V3D GPU draws and composites the desktop. After it: 15 a second chip family.
**Polish still open** (Joshua, 2026-10-26, from the Snow Leopard and Mac OS X introductions, and posts.design): long movies streamed from the card with trim, Dock Exposé, Stacks for Downloads and Documents, window buttons that react on hover, Finder icon sizes with thumbnails, full-text history in the browser, the Launchpad card fading in, and Cmd+M for the kernel-drawn windows. The magnifying Dock (6.4.0), the genie (6.5.0) and Top Sites (6.37.0) are done.
- [ ] [Opus] Later, not a 7.0 blocker: TLS 1.3 (BearSSL stops at 1.2, and most sites still offer 1.2), Ethernet through the Pi 4's GENET, and keep-alive (`net.c` holds one connection, so requests send `Connection: close`; 6.45.0's TLS session resumption already skips the costly part of each new connection, decided 2026-10-28).
**Still open from the 2026-10-11 review:**
- Codex through the relay on the Mac, read-only, never a bare-metal OAuth client on the Pi (research decision 2026-10-10).
- Pi security on the board: verify the TLS relay and HTTPS clock, then drop the build-time clock bootstrap.
- Self-update with a fallback kernel. Check: a fake release that boots once and falls back.
- The local model with real weights read from the card (`tools/checks/arm64-llm-check.py` loading from a FAT image).
- A trademark search before any rename.

## Next few weeks
- [ ] [Opus] The browser grows up, after the text browser ships (Joshua, 2026-10-12: "then we'll get JS working etcetera"). In order:
  1. Images (the Pi already has a PNG decoder, JPEG next).
  2. CSS layout (block and inline flow, colours, fonts, a box model, no floats at first).
  3. Forms and cookies.
  4. A small JavaScript engine (an embeddable one such as QuickJS, ported freestanding, with a DOM just big enough for the pages Joshua actually reads).
- [ ] [Sonnet] The dock grows to the default macOS set that exists on Joshua Tree.
  - Music, Books, Clock, Calculator and the Browser join the 11 tiles (Apps, Finder, Mail, Calendar, Notes, Reminders, Terminal, Samantha, Weather, Stocks, Trash).
  - One flat house-style icon each from the icon composer, in the Mac's order. The dock narrows its tiles to fit.
  - `GUI_ICON_COUNT`, the dock geometry numbers in the checks (`iconlight-check.py`, `dockhover-check.py`, `iconart-check.py`) and `docs/ARCHITECTURE.md` (Design rules) all move together.
  - An app that has no ARM build yet keeps its tile and says "not on ARM yet".
  - Check: the dock checks pass at the new count.
- [ ] [Opus] Samantha on the device, no relay.
  - A small chat model (Qwen 0.5B class, int8, under 1 GB) baked into the card image and run by the kernel's own inference code.
  - `arch/arm64/llm.c` is a float32 llama2 forward pass today. It needs int8 weights, a tokenizer for that model and a KV cache that fits the Pi's 4 GB.
  - The relay stays the default when the Mac answers. The local model answers when it does not.
  - Check: `tools/checks/arm64-llm-check.py` extended to the real model's tokenizer and a fixed prompt.
- [ ] [Sonnet] Dock magnification on the i386 desktop, to match the Pi's (7.4.0 has the Settings row there: Off, Small, Large), through a shared `kernel/dock_draw.c`.
  - Check: an i386 dock hover check like `tools/checks/arm64-dockmag-check.py`.
- [ ] [Sonnet] `sam "what is 5+5"` takes about a minute on the board while the relay answers in seconds.
  - Read 2026-10-12, not fixed. Relay discovery is already cached (`relay_ip` in `arch/arm64/ask.c`) and `ask_poll` adds no sleep, so the time is somewhere else.
  - Suspects, in order:
    1. A fresh TLS handshake per question without session resume on the Pi (software crypto).
    2. A dead baked `CLAUDE_HOST` costing a 3 s connect plus two 1.5 s probes on each question.
    3. The relay running several agent steps (each one a full POST) on the small local model.
  - Time each step with `ticks()` stamps on the UART on the board first.
  - Check: a stub-relay check that fails when a question costs more than one handshake.
  - At 11:20 PM on 11 October the Clock and the dock tile said the 11th and the month grid circled the 12th. The app read `SYS_TIME` as raw UTC; on the Pi `SYS_TIME` now returns Vancouver wall time through `clock_offset` in `arch/arm64/main.c`, the one time-zone path the Clock already used.
  - Check: `tools/checks/arm64-calicon-check.py` boots 06:20 UTC on 12 October, finds the circled day in the Calendar window (the 11th), and a mutation with the grid back on UTC (`--mutate`) circles the 12th and fails.
- [ ] [Sonnet] Keyboard shortcuts in every app.
  - The Command key (the GUI or Windows key on a PC keyboard) with A select all, C copy, X cut, V paste, Z undo, S save, W close the window, Q quit, N new, F find, Tab to switch windows.
  - Plus Ctrl for the same on a keyboard with no GUI key.
  - One table in the shell. Apps handle the actions.
  - Done on the Pi: C X V A S W Q N, Space and Tab (`arm64-cmdtab-check.py`); N is a new note in Notes on both kernels. Z undo is in Notes on both kernels (7.5.0, `textselect-check.py`). Left: Z in the other editors, and F find, which each app has to handle.
  - Check: an arm64 key check that sends each chord through QMP and sees its serial line.
- [ ] [Sonnet] The first few seconds after the Pi desktop appears take no keys. Find why and fix it. Since 4.28.1 `boottime` in the Terminal says `first key N ms after the desktop` and `slow step NAME N ms`; read those on the board, then fix the step it names. (The splash, its bar and the login screen shipped: `arm64-splash-check.py`, `arm64-splashbar-check.py`, `arm64-login-check.py`.)
- [ ] [Sonnet] A small voxel building game as a native Joshua Tree app, software-rendered: walk, place and break blocks.
  - Pick a name that is not Minecraft.
  - Check: a host test of the block grid and ray pick, plus a `tools/checks/` boot check that opens the app and sees a frame.
- [ ] [Joshua] Higgsfield video through the Mac relay, under Joshua's spending rule.
  - Never automatic. Always an explicit yes from Joshua with the cost shown first. One clip at a time. Nothing from Samantha on her own.
  - Check: a relay test that a request without a fresh yes is refused and that Samantha's actions cannot start one.

<details><summary>Details (0 lines)</summary>


</details>

## The long road: version 3 to 10, and 100 (proposal, 2026-10-05)
### [5.0] Talk to it: Claude Code in Joshua Tree
- [ ] [5.0] [Opus] Claude phase 2: tools on Joshua Tree itself, so Claude can read and change the OS's own files. Needs real TLS on the box or a trusted relay protocol that calls back into the machine.

## Pickup (written 2026-10-03, night)
### Landing and demo, to A+
- [ ] [Sonnet] The tour is silent after the intro video.
  - Joshua speaks each stop in his cloned voice (`/api/speak` with `voice: "joshua"`, see the personas doc in the Turing repo).
  - The caption is on screen and the speaker button is respected.
  - Check: extend `tools/checks/portfolio-mute-check.mjs` so muted means no audio request.
- [ ] [Sonnet] All icons share one design system.
  - Rule decided 2.12.2 in `docs/ARCHITECTURE.md` (Design rules): a fleet icon keeps its own tile colour and picture (Tonchi stays #2E86DE). It shares the squircle, edge, lip and glyph margin. Its tile stays flat.
  - Still open:
    - Windgate's faint ring runs past the margin (the check exempts fleet icons).
    - Tonchi's picture is a speech bubble.
    - The `APPS` fallback colours for Quotes, Fieldbook and Tonchi are purple or green.
    - App windows keep their own accents (Epiphany and Windgate are blue).
  - Next: make `iconinset-check.py` measure fleet margins, then fix what it finds.
  - Details in PR 425.
- [ ] [Joshua] Judge the live landing against the Plank landing, the bar for the whole site. List what still falls short, in his words.
- [x] Trim the repeats (13.21.2): top divider, stats row and the Pi tile that repeated Protected are gone; Download PC ISO joins the hero; the desktop clips are one scroll-snap strip.
- [ ] Reorder toward apple.com/macos (Joshua, 2026-11-18, graded B-): a sticky local nav (Overview, Apps, Pi, Specs, a Try it pill), then hero, the clips strip, three one-idea chapters (38 apps, protected apps, a real Pi), specs, and one closer with the film, waitlist and Build. The six Nimbus build steps move to their own page.
  - [x] 13.21.2: the local nav (fixed, 52 px, Overview, Apps, Pi, Specs and a Try it pill) and an app wall that ends on a full row (a grid, Depths full width as the feature). `landing-layout-check.mjs` fails if the nav lets go on scroll, links to a missing section, or the wall's last row is short.
  - [ ] On a phone the wall is 19 tiles in one column, about 3,000 px of scroll: make it a sideways strip like the desktop clips.
  - [ ] Three one-idea chapters, then one closer (film, waitlist, Build); the six Nimbus build steps move to their own page.
  - Check: `landing-layout-check.mjs` and `mobile-overflow-check.mjs` in CI, plus a 1280 and a 390 screenshot.

### From the notebook (Joshua, 2026-10-04)
- [ ] [Opus] Time Machine: a browse-the-past view. Home-folder snapshots exist since 4.12.0 (`snapshot`, `restore N yes` in the Terminal); a window to look through them does not.
- [ ] [Sonnet] Fullscreen avatar, custom: Samantha (or Joshua's face) full screen as a mode, with the face picked in Settings. The page also lists video, audio and GUI mode as three ways to talk to her.
- [ ] [Opus] An integrated LLM that runs on the box. The small forward-pass runtime landed; real weights from the SD card remain in Next priorities. A full Turing model still needs a memory and runtime decision.
- [ ] [Sonnet] Improved chat app: more tools and persistent memory across boots (a file the app reads at start).
- [ ] [Sonnet] Spotlight: one key opens a search box over apps, files, contacts, events. The Search app does files only; the existing Spotlight-style item under Desktop and apps is the same item.
- [ ] [Sonnet] GarageBand-lite: record and layer a few tracks from the Sound Blaster, then play them back. Movie trim (iMovie) is already listed.
- [ ] [Sonnet] Sharp image everywhere, no visible pixels: audit the icons and small type at retina scale, same bar as the JT retina polish rule.
- [ ] [Opus] The big promise, in his words: say "computer, run the simulation", "build me a game", "publish and monetize my apps", "add X feature", "patch Y bug", and the OS does it. Samantha plus a coding agent plus the publish flow. Scope it as a doc before any code.
#### Every fleet app in the Launchpad (Joshua, 2026-10-04)
- [ ] [Sonnet] Brick: live rental listings.
- [ ] [Sonnet] Hagaki: inbox triage (smart folders over the Mail app's messages).
- [ ] [Sonnet] Nimble: instant answers from its real API. The Terminal command and Samantha's plugin shipped in 5.1.0; the window and its tile are left.
- [ ] [Sonnet] Sidewise: news with the bias rating per source.
- [ ] [Sonnet] Wordroot: look up a word's origin.
- [ ] [Sonnet] Intake (Healstack): today's supplement stack and log.
- [ ] [Sonnet] Curvely: plot an equation. Numen: a calculator canvas. Both run offline.
- [ ] [Sonnet] Inkpress: RSS reader over a Worker feed route.
- [ ] [Sonnet] Homeward (lost pets) and HomeQi (room check): list views over their APIs.
- [ ] [Sonnet] Dream and Costanza: read-only feeds of entries and poems.
- [ ] [Sonnet] Cadence and Tripwire: commit streak and API drift status.
### Voice chat, lag and sharing (Joshua, 2026-10-04)
- [ ] [Sonnet] Read `voicetime` off the live demo for ten messages and post the numbers here. The web portfolio gets the same line (`console.info`) next.
- [ ] [Sonnet] Skip `/api/pick` when the message plainly is not a tool request, or have `/api/chat` pick the tool in the same call. One round trip less on every message.
- [ ] [Sonnet] Stream every stage: STT, LLM tokens, ElevenLabs over its WebSocket TTS, audio chunks played as they land. Add barge-in: talking over her stops her.
- [ ] [Sonnet] One shared voice module, the portfolio's code, used by both. The OS stays thin: it opens the voice page or calls a small server, no audio pipeline or TLS of its own.
- [ ] [Sonnet] Hardening: keys server-side only, a per-session cost cap, reconnect on a dropped socket, Claude as the fallback when Turing is down.
- [ ] [Joshua] Live face: Simli or HeyGen LiveAvatar (credit based, about $30 to $500 a month). Veo and Higgsfield stay for pre-rendered ads and intros only.
### CI and speed
- [ ] [Sonnet] Convert the remaining fixed scratch paths in `tools/checks/tmp-paths-baseline.txt` to private directories and sockets. The one-suite lock already prevents collisions; keep it until the baseline is empty. Check: `tools/checks/tmp-paths-check.py`.
- [ ] [Sonnet] Six of the last ten red runs were slow-runner timing flakes (Chat tool scenes, the phone mute button, Keyrate, the Apps folder layout): each suite shard already has its own runner and runs its checks serially. Measure CPU load and guest startup before changing concurrency, and watch the next ten runs.
CI now installs ARM QEMU, so ARM regressions run instead of skipping for a missing emulator.
- [ ] [Sonnet] Reduce local CI time without raising the two-job concurrency cap. Four concurrent jobs previously exhausted the Mac's memory; measure slow checks and rebalance first.
### Raspberry Pi and ARM64
- [ ] [Opus] Admin and sudo (Joshua, 2026-10-06: "fix it soon"). Done on the Pi in 4.8.0 to 4.10.0: accounts, the login screen, an admin flag, `sudo`, `makeadmin` and `unadmin` (`tools/checks/arm64-sudo-check.py`). Left: the same tier on i386.
  - An admin flag per account, a password prompt before anything that installs, deletes a user or changes system settings, and a normal-user role that cannot. On i386 it touches `kernel/auth.h`, Settings and the syscall gate.
  - Per-user home folders shipped in 4.11.0; file permissions follow (6.0).
- [ ] [Sonnet] `get`, the Joshua Tree installer: a public recipes repo in the Homebrew shape (name, URL, checksum), a C command that downloads an app over Wi-Fi and drops it on the card. Needs Wi-Fi stage 2 and SD writes.
- [ ] [Sonnet] mruby as a ring-3 app: Ruby scripts compiled to bytecode on the Mac, run on Joshua Tree through `user/libjt`. Full Ruby and Homebrew stay out of reach (they need git, curl, a shell and a compiler).
- [ ] [Sonnet] Jellyfin app: list and stream Joshua's movies from the Jellyfin server on the Mac (installed 2026-10-06) through the Movies player. Needs Wi-Fi stage 2 and sound.
- [ ] [Joshua] The real-board proof of 4.6: the Yeti in a Pi port, `mic` shows the room, hold Space and talk, Samantha answers out loud. Everything else of voice mode and the USB audio driver shipped in 4.6.0 against QEMU.
- [ ] [Opus] Pi camera (needs the part, about $25): a CSI driver, the picture on screen.
- [ ] [Opus] Touchscreen (needs the part, about $60): the i386 touch driver's shape on the Pi's DSI or USB touch.
- [ ] [Opus] Ethernet through the Genet MAC, as the wired backup.
- [ ] [Opus] Serial loader the day the cable arrives: new kernels over the wire, no card swaps.
- [ ] [Opus] The self-update loop (Joshua, 2026-10-06: "make tweaks to the OS from inside the OS, push changes and hot swap").
  1. Ask Claude in the Claude app.
  2. The relay's Claude Code edits the repo on the Mac and opens a draft PR.
  3. CI goes green.
  4. The Pi pulls the new `kernel8.img` over Wi-Fi from a release, writes it to the card and reboots into it.
  - Needs Wi-Fi stage 2, SD writes and plain-HTTP downloads from a mirror.
- [ ] [Opus] The other three cores: wake them from the spin table, give each a stack, run the desktop on one and the drivers on another.
- [ ] [Opus] The Pi 5 (M5): the RP1 chip over PCIe for every peripheral.
Gaps against Linux distributions (Kali, Ubuntu, Arch), 2026-10-06 night. Most are already on the version road (4.0 processes and a real shell, 6.0 users and backups, 7.0 TLS and a browser, 8.0 a compiler and `get`); these are the ones it did not name:
- [ ] [Opus] An SSH server: log in to the Pi from the Mac over Wi-Fi, with its own crypto (ed25519, ChaCha20-Poly1305). It makes the serial cable optional for everything but early boot.
- [ ] [Opus] ext4 read-only, so a Linux-formatted USB stick or card can be opened. FAT stays the default.
- [ ] [Sonnet] A packet capture and ping/traceroute/port-scan toolbox (raw sockets), the part of Kali that is a few tools, not the whole distro.
- [ ] [Opus] Wi-Fi monitor mode, only if the CYW43455 firmware allows it. Kali's wireless tools depend on it. Low priority, owner's own network only.
- [ ] [Sonnet] A sandbox for untrusted apps, the small version of containers: a ring-3 app with no network and a private folder, per `get` recipe.
The rest of the Linux gaps, tagged with the version that owns them (Joshua, 2026-10-06 night: "add those gaps to the roadmap too"). These are the road items in `## The long road`, written out as work you can pick up:
- [ ] [Opus] [4.0] Processes the Unix way: fork and exec, pipes, signals, a process table, exit codes. The base for everything below.
- [ ] [Opus] [4.0] A real shell and Terminal: a command line with pipes, redirects, job control, a PATH and scripts, so `ls | grep` works. Tabs and splits come after.
- [ ] [Opus] [6.0] Users and permissions: accounts, file owners and modes, a login screen, an admin role that can install. Joshua's "admin privileges" idea starts here.
- [ ] [Opus] [7.0] TLS 1.3 in the kernel or a ring-3 library: the gate for HTTPS, so websites, Plex and a real Claude API connection work without a relay.
- [ ] [Sonnet] Driver breadth, by what Joshua owns: USB mass storage, USB audio, USB Ethernet, a USB serial adapter, a Bluetooth adapter. Each is a class driver on the xHCI code that already works.
- [ ] [Opus] Power management: suspend and resume, CPU frequency scaling, the board's low-power states. Linux does this for free; a hobby OS never does until someone sits down.
- [ ] [Opus] VNC server: the Pi's screen on the Mac over Wi-Fi, so debugging stops needing photos and demos need no monitor. After Wi-Fi stage 2.
- [ ] [Opus] A USB stick as the first disk: mass storage over the xHCI driver we have, likely faster to "files survive a reboot" than EMMC2.
- [ ] [Opus] Doom: the classic proof a platform is real, and Joshua's chosen benchmark (2026-10-06: "if we can get doom working that's sort of a benchmark"). Full speed with sound on the Pi is the flag right after 3.0. Keyboard first, gamepad next. Depths (4.23.0) is our own raycaster in its spirit; the real port still needs a small C library, the GPL doomgeneric source kept apart from the Apache tree, and Freedoom data loaded from the disk, not baked into the kernel.
- [ ] [Sonnet] USB gamepad: a HID report parser beside the keyboard and mouse. Doom and Hamurapi on the couch.
- [ ] [Opus] An Atari-style console: our own 6502 core and a tiny TIA-like video chip. Only homebrew games whose authors allow redistribution, never original cartridges (the ROMs are copyrighted). Avoid Tetris-like and Space-Invaders-like designs.
- [ ] [Sonnet] Drop files from the Mac over Wi-Fi: a tiny upload server on the Pi (plain HTTP PUT), no card swap.
- [ ] [Sonnet] `joshuatree.local`: mDNS so the Pi announces its name on the network.
- [ ] [Opus] GPU 3D: the V3D block for real 3D, the way Onyx did it. Samantha's face in 3D is the first use.
- [ ] [Sonnet] I2C and SPI drivers for breadboard sensors and small screens.
- [ ] [Sonnet] Console emulators (NES first), later.
Sources: Onyx on Circle (Adafruit blog, 2026-09-29), Circle's feature list (github.com/rsta2/circle), rpi4-osdev, AROS on the Pi (Hackaday, 2026-08-23).
Round 2, 2026-10-06 night (Raspberry Pi OS Trixie and RISC OS on the Pi 4). Small, all after Wi-Fi:
- [ ] [Opus] Both HDMI ports: the Pi 4 drives two monitors; a second framebuffer through the mailbox display id, the desktop spanning or mirroring.
- [ ] [Sonnet] A Screen settings page on the Pi: resolution, scale, which HDMI, saved on the card.
- [ ] [Sonnet] Search that understands plain words, Spotlight style: one box that finds files, apps, contacts and actions ("open the clock", "new note") through Samantha. Starts with the Search app we already have.
- [ ] [Opus] A screen reader, the rest (9.1.0 reads the dock): speak the focused window title and its controls.
- [ ] [Sonnet] A phone link: show a phone's live activities (a timer, a delivery, a call) in the menu bar. Needs Wi-Fi and a small relay; later.
- [ ] [Sonnet] Shortcuts: small chains of actions (open this, type that, play a song) built from blocks, run by a key or by Samantha. Pairs with the Plank language at 8.0.
- [ ] [Sonnet] A built-in network speed test in the Wi-Fi menu, and a signal bar in the menu bar. After Wi-Fi stage 2.
- [ ] [Sonnet] Game mode: a full-screen mode that quiets notifications and background work, for Doom and the handheld idea.
- [ ] [Sonnet] More wallpaper formats (WebP and AVIF through the decoder code we have, JPEG and PNG already work) and a slideshow.
- [ ] [Sonnet] A Start-style launcher you can customise: pin, reorder and hide apps in the Launchpad.
- [ ] [Sonnet] Camera controls in Settings, once the Pi camera driver exists.
Sources: Tom's Guide and TechRadar on macOS Tahoe 26, Pureinfotech and Digital Citizen on Windows 11 2026.
- [ ] [Opus] ARM framebuffer write-combining: map the framebuffer Normal non-cacheable in MAIR, so a live desktop needs no cache cleans. Split out of M1d: QEMU cannot see cache attributes, and a wrong mapping can blank the Pi's screen, so it needs a board test of its own.
- [ ] [Opus] M2: IP, DHCP and a TCP connection on top of the ARM network card (port the i386 stack above the NIC), and the net and disk drivers moved to interrupts too (input already is). Then M3 (EL0 userland and the syscall layer) and M4 (SD through EMMC2, USB through xHCI, Ethernet through the Genet MAC). 3.0.0 ships when M4 shows the desktop on a real Pi. `docs/ARCHITECTURE.md` (ARM64 and the Raspberry Pi) has the milestones.
- [ ] [Opus] ARM IP stack, stage 2: TCP that survives a lost segment (retransmit, reorder, a real window) before the Claude app runs on ARM over Wi-Fi; net and disk drivers on interrupts too (input already is).
### x86-64 (Joshua, 2026-10-06 night: "let's do x86-64 next")
- [ ] [Opus] x86-64 slice 2, a machine: the 64-bit IDT and exception frames, the APIC timer, the heap and the page allocator, a keyboard and a mouse.
- [ ] [Opus] x86-64 slice 3, the shared drivers: split the code that assumes 32-bit pointers (`u32` addresses in `kernel/syscall.c`, the PCI and network drivers) behind a pointer-size type, and share the PCIe and xHCI code the ARM port already proved on the Pi.
- [ ] [Opus] x86-64 slice 4, apps: the 64-bit syscall entry (`syscall`/`sysret`), a 64-bit `user/libjt`, and the ring-3 apps rebuilt for it. The 32-bit app ABI stays supported on i386.
- [ ] [Opus] x86-64 slice 5, a real PC: UEFI boot from a USB stick with a GOP framebuffer, USB keyboard and mouse, NVMe or AHCI disks. The first photo from a real x86-64 board closes the slice.
- [ ] [Sonnet] A single `make` per target and one CI line each, so a change to shared code is checked on all three.
- [ ] [Opus] RISC-V (later): the open chip family. Same shape as the ARM port; a cheap board and QEMU's `virt` machine make it the cheapest fourth port to try.
### From the YouTube playlists (checked 2026-10-06)
- [ ] [Sonnet] MBR partition tables and FAT32: `drivers/fat.c` is FAT16 on one volume from sector 0, but a Pi SD card is an MBR partition table with a FAT32 boot partition. Needed before the SD driver can read the card it boots from.
- [ ] [Sonnet] ICMP: answer and send pings, so `ping` works on both builds.
- [ ] [Sonnet] An ELF loader beside the flat binaries, so apps built by normal toolchains can run.
- [ ] [Opus] POSIX compatibility for the common calls (open, read, write, fork, exec), part of 4.0.
### From SerenityOS and Haiku (research round 4, 2026-10-06 night)
- [ ] [Sonnet] Drag and drop everywhere: a file from Files onto an app, an image into Notes, text between windows. Haiku's signature feel.
- [ ] [Sonnet] One decoder library for every format (Haiku's "translators"): each image, sound and video format decoded in one place, so every app that opens a picture opens all of them.
- [ ] [Sonnet] A crash reporter app: when a ring-3 app dies, a window says which one, why and where, with a "send log" button (SerenityOS CrashServer). The kernel already survives the crash.
- [ ] [Sonnet] A hex editor and a system profiler (which app is using the CPU, sampled), for debugging the OS from inside the OS.
- [ ] [Opus] File tags and live queries (Haiku BFS): attributes on any file (artist, author, status) and saved searches that update themselves. Builds on the Search app.
- [ ] [Opus] Hardening (SerenityOS): no memory both writable and executable (W^X), randomised kernel and app addresses (ASLR), and per-app promises of what it may touch (pledge and unveil). The sandbox item grows into this.
- [ ] [Opus] A graphical debugger: step a ring-3 app, see its registers and memory. Needed once apps are built on the box (8.0).
- [ ] [Sonnet] Fast boot and shutdown as a number on the landing page: power to desktop on the Pi, timed from the photo, beaten each release.
- [ ] [Sonnet] More protocols for the small web: Gemini (tiny pages) and IMAP for the Mail app, after TLS.
- [ ] [Opus] Verified boot: the Pi checks the kernel's signature before running it, so a tampered card refuses to boot. Pairs with the fallback kernel.
- [ ] [Sonnet] Reset to factory (ChromeOS powerwash): one Settings button that wipes users and files back to a clean first boot.
- [ ] [Sonnet] Guest mode: a session with no account that leaves nothing behind.
- [ ] [Sonnet] Ask-first permissions (Android): the first time an app wants the camera, the mic, the network or your files, a prompt asks you, and Settings can take it back.
- [ ] [Sonnet] Quick settings: one panel from the menu bar for Wi-Fi, sound, brightness, Bluetooth and do-not-disturb.
- [ ] [Sonnet] Split screen and picture-in-picture: two apps side by side, a movie floating over the desktop.
- [ ] [Sonnet] Clipboard history and an emoji picker, both from one key.
- [ ] [Sonnet] Screen recording to a file, from the screenshot key's menu.
### From the hobby OS comparison (2026-10-08)
- [ ] [Opus] Bad-argument tests for every system call (xv6 `usertests`): bad pointers, huge lengths and closed handles. Check: a ring-3 program runs them all and the kernel returns an error for each and keeps running.
- [ ] [Sonnet] A ports folder: build scripts for outside C programs against libjt, Lua first. Check: the ported Lua runs a script as a ring-3 app and prints the right answer.
- [ ] [Opus] One driver out of the kernel: the mouse as a ring-3 program. Check: kill it, the kernel keeps running, restarts it, and the pointer moves again.
### Known limits to recheck
- [ ] [Sonnet] `SYS_READFILE` reads with interrupts off, so loading mid-song can glitch the audio.
- [ ] [Sonnet] Music and Movies live in the Apps folder only, not on the dock.
- [ ] [Sonnet] Silent movie clips play about twice too fast on this QEMU build.

<details><summary>Details (0 lines)</summary>


</details>

## Architecture to A+ (refreshed 2026-10-11)
- [ ] Window open takes about 640 ms for a ring-3 app (CI frametime check, 2.0.0): profile the seed, private page table and first draw; preseed binaries at boot and skip zeroing pages that get overwritten. [Sonnet]
- [ ] [Opus] What the ports will need from the ABI: a font syscall (Keyrate carries its own 8x16 bitmap), a tick clock finer than `SYS_TIME`'s seconds, more than one program window at a time, and the framebuffer pages flipped back to supervisor-only on release (today they are zeroed and re-mapped on the next open).
Measured against the closest from-scratch peers: SerenityOS (the one-person-scale benchmark), ToaruOS (own compositor, own libc), KolibriOS (tiny, runs on real PCs), Haiku, and against macOS/Linux. Ranked; the loop works top down. Each line points at the section that closes it.
- [ ] [Opus] HTTPS without the proxy: native TLS 1.3 in the kernel, then the web browser on top of it.
- [ ] [Opus] The lag (issue #14) profiled, fixed and measured, with frame time numbers in the release notes.
- [ ] [Sonnet] Nothing crashes: bad input in every text field, long lines, empty files, missing disk, no network. Each case gets a check.
  - 1.0.10: empty, oversized, corrupt-FAT and full-disk cases; `tools/checks/filerobust-check.py`.
- [ ] [Sonnet] Icons with taste: depth, soft light, real materials, consistent corner and light direction across all of them. Judged from the gallery at dock, hover and Apps grid sizes.
  - 0.89.0: the 11 dock icons redrawn Big Sur style (one top light, no outlines, 148px art at an exact 2:1); `tools/checks/iconlight-check.py`. Still to do: the 15 Apps-folder fleet icons. Calendar is now a picture, a page with a binding bar and a grid of day squares, with no date on i386 or the Pi: the Pi has no clock, and its blank page with a dash looked half drawn. Check: `tools/checks/calicon-check.py`.
- [ ] [Sonnet] Text sharp everywhere: no bitmap fallback font where the antialiased one should draw, no uneven letter gaps, baselines level.
  - 0.89.0: coverage-to-ink curve (stem darkening) sharpens every AA text path; `tools/checks/textsharp-check.py`.
  - 1.0.9: a retina-bar typography pass judged from real headless crops across the menu bar, a title bar, Notes' paragraph, Terminal's prompt/output, a Settings row, the dock hover label and the Apps folder grid; fixed the Apps folder's glass panel being 19 logical px short of its own grid. `tools/checks/baseline-check.py` measures container padding, baseline flatness and letter-gap variance on real pixels.
- [ ] [Haiku] Accessibility floor: every app opens and closes by keyboard alone. In progress in #127 (Enter on the desktop opens the Apps folder; the check still has to prove all 25). Ships as 1.0.x, not a blocker for the tag.
- [ ] [Opus] Kernel goes 1-bit: ordered dither (4x4 Bayer) replaces every grey and alpha blend.
- [ ] [Sonnet] Window chrome: 1px ink border, hatched title bar, no shadows.
- [ ] [Sonnet] All 23 app icons redrawn as 1-bit line glyphs.
- [ ] [Haiku] Boot splash: the badge drawn scanline by scanline.
- [ ] [Haiku] `landing/icon.svg`, root `icon.svg`, and the app `.icns` redrawn as the simplified tree.
- [ ] [Sonnet] File search, Spotlight-style. Needs an index-or-scan design, not a stub.
- [ ] [Opus] A second privilege tier (sudo/admin) on top of the accounts that already exist.
- [ ] [Opus] Text rendering: a dedicated pass on AA quality, separate from the font size/weight controls that already exist.
- [ ] [Opus] Multi-core (SMP).

## Apps after 2.2 (cheapest first, one agent and one PR each)
- [ ] [Sonnet] Voice Memos: record with `SYS_AUDIO_RECORD`, save a WAV to Files, play it back in Music.
- [ ] [Sonnet] Samantha media tools: "play something", "pause", "what's playing", one check each.
- [ ] [Sonnet] Movie trim: cut, split and join clips at frame boundaries and save a new AVI. The iMovie-lite step.
- [ ] [Sonnet] Preview: opens images and text files from any Files window. PDF is a later, bigger step.
- [ ] [Sonnet] Step sequencer: a drum and synth loop maker with 8-bit mono sound. The GarageBand-lite step.
- [ ] [Sonnet] Docs: Notes grows into a word processor: bold, italic, headings and lists, saved as plain Markdown so any machine can open it. No `.docx`.
- [ ] [Sonnet] Sheets: a grid with formulas (sum, average, min, max, plain arithmetic, cell references), saved as CSV. The app that makes it feel like a real computer.
- [ ] [Haiku] Slides: a Markdown file becomes full-screen slides, arrow keys to move, one title and a few lines per slide.
- [ ] [Opus] Stereo and 16-bit audio: a second `SYS_AUDIO` op so music stops sounding flat. Unblocks a real music studio (VERSIONS 54).
- [ ] [Opus] A real video codec on this CPU class, only after 3.0 hardware shows what it can decode.

## Our own computer
- [ ] [Opus] USB storage, so an external SSD mounts.
- [ ] [Opus] USB audio in, so a microphone works. Needs the sound work below.
- [ ] [Opus] Wired internet on real PCs: an Intel e1000 driver next to rtl8139 and ne2k, proven with QEMU `-device e1000`. Most PCs from the last 15 years have an Intel or Realtek chip. Stretch for 1.0; if it slips, the release notes say wired internet is QEMU-only.
- [ ] [Opus] Drivers for physical hardware, not just QEMU's emulated devices: AHCI for real SATA disks, on top of the e1000 work above.
- [ ] [Opus] FAT32, read and write: open a normal USB stick or SD card. Today the disk format is Joshua Tree's own, so nothing from another computer opens.
- [ ] [Opus] Load ELF programs from disk at runtime, so apps can ship outside the kernel. First step to installable apps.
- [ ] [Opus] User profiles and sign-in done properly: login screen at boot, a home folder and settings per person, passwords stored hashed and never in the clear, lock screen (the Pi's auto lock shipped in 13.24.0), an admin level for risky actions (issue #28). Accounts exist today; this is the pass that makes them trustworthy.
- [ ] [Opus] Sound on real PCs: an AC97 or Intel HD Audio driver next to the Sound Blaster one (1.6.9), which only exists in emulators.
- [ ] [Sonnet] Laptop basics: battery level (10.1.0), trackpad (10.1.0), USB precision pads and Sleep (11.0.0); lid close still open.
- [ ] [Joshua] One real PC booted from the USB stick, keyboard and mouse working, photographed. The USB image and non-emulator graphics are only proven in QEMU so far.
- [ ] [Opus] Install to disk from the USB stick.
- [ ] [Joshua] Trademark search and registration (CIPO or USPTO) for Joshua Tree and the mark before the first boxes ship; a quick search found Joshua Tree Technologies LLC and a JOSHUA TREE VOICE mark, and many STRATA marks in the software and computer classes, so clear the names first.

## Samantha
- [ ] [Sonnet] Samantha tools for every app: open, read and write Notes, Files, Calendar, Reminders, Weather, Stocks. A few apps a night, one check each.
- [ ] [Sonnet] She says nothing when "Tap to boot" lifts the landing poster: the demo has no greeting line, only the opt-in `?tour` questions, so it needs one short spoken hello sent through her existing speak path, which depends on the voice relay and ElevenLabs key that live outside this repo.
- [ ] [Joshua] Google project for mail sign-in (OAuth client id), then [Sonnet] sign in with Google in Mail.
- [ ] [Sonnet] Lip-synced face: one Higgsfield lip-sync render of a sentence about Joshua Tree, cut into a viseme library, the server sends a mouth timeline with each reply. The face benchmark (PR #241) must grade it A+ (sync is the gap: best so far 55/100).
- [ ] [Sonnet] Chat listens: speak to Samantha instead of typing. She already answers out loud with her face moving (1.6.9); hearing you needs a microphone path first.
- [ ] [Opus] Customization system: users can talk to Samantha to modify OS behavior, colors, fonts, layouts; settings persist on disk in a user-bootstrapped config.
- [ ] [Sonnet] Third-party LLM support: let users choose their preferred model provider (OpenAI, Anthropic, local, etc.).

## Desktop and apps
- [ ] [Opus] Per-window backing stores, not drawing straight into the shared framebuffer.
- [ ] [Opus] A compositor with damage tracking, plus the back buffer this kernel still lacks. Pi part done in 4.3.0: a RAM back buffer and one damage rectangle per window move or resize (`arch/arm64/main.c`, `fb_flush`, `desk_compose`). Still open: per-window stores and damage for app redraws, and all of it on i386.
- [ ] [Opus] Input routing by focus instead of the current global key/click pull.
- [ ] [Opus] Every app converted from a blocking loop to open/draw/on_key/on_click handlers, or its own task.
- [ ] [Sonnet] Right-click context menus.
- [x] [Sonnet] Lock screen, sleep, and ACPI shutdown. Lock screen and Sleep shipped on the Pi (11.0.0, 13.24.0); 13.25.0 makes the PC's Shut Down really power off through ACPI (`kernel/acpi.c`, `acpi-off-check.py`).
- [ ] [Sonnet] Maps app. The wallpaper already fetches map tiles.
- [ ] [Sonnet] Files app view options: list, grid, columns, with adjustable sorting and grouping.
- [ ] [Sonnet] Mail account configuration: setup for multiple accounts, IMAP/POP/SMTP settings.
- [ ] [Sonnet] Chat app redesign: expanded capabilities, better interface, rename to Sam.
- [ ] [Sonnet] Shell pipes, redirection, and environment variables.
- [ ] [Sonnet] File associations, opening a file in the right app.
- [ ] [Sonnet] Drag and drop.
- [ ] [Sonnet] Software update path.
- [ ] [Haiku] Accessibility: text size and high contrast.
- [ ] [Joshua] Landing page polish: slow the demo tour for intimate feel, device-frame chrome, loading states.
- [ ] [Haiku] Window edges and corners: eliminate pixelated rendering, ensure smooth antialiased edges.
- [ ] [Haiku] Dock icon padding and alignment: consistent spacing, balanced visual weight.
- [ ] [Haiku] Window title bar buttons: fit and finish, proper proportions and spacing.
- [ ] [Sonnet] Word processor (Notes) text: improve letter spacing, line height, paragraph margins for better readability.
- [ ] [Sonnet] Lazy-load the boot loading image itself so it never shows visibly pixelated while scaling in. (Found by eye in the 2026-09-21 QA tour, `tools/qa-demo.sh`.)
- [ ] [Sonnet] Mobile Joshua Tree: the landing demo and the OS usable on a phone screen. Mobile first by 2026-10-04.
- [ ] [Sonnet] Typeface support: proportional fonts beyond DejaVu, loaded from disk, picked in Settings. Reference look from Joshua: a tight grotesque sans for body and headlines, one display face for titles, hairline rules, flat colour blocks. Sans only in the UI chrome.
- [ ] [Sonnet] Music app enhancements: equalizer, better playback controls.
- [ ] [Sonnet] Video editor: basic timeline, trimming, and export.
- [ ] [Opus] Dual monitor support: a second framebuffer (QEMU `-device secondary-vga`), the desktop across both, windows dragged between them. Needs the compositor.
- [ ] [Sonnet] Installing and updating apps from inside the OS.
- [ ] [Sonnet] Native Bible app (public-domain KJV on the disk image, book and chapter picker, search).
- [ ] [Sonnet] Settings as a real native app in the dock: wallpaper, text size, system typeface, location, accounts, network status, about. One place, not scattered panels.
- [ ] [Sonnet] Languages: every UI string through one table, a Settings language picker, Latin-1 accents drawn (the DejaVu faces have the glyphs; the text paths drop bytes above 0x7F today).
- [ ] [Sonnet] Ring-3 programs a fresh shell ships with: `cat`, `wc`, `grep`, `calc`. A tiny C compiler is a stretch. First piece landed: `user/libjt/`, a small userland C library (string/ctype/stdlib/stdio) over `jtsys.h`, plus `user/wc.c` built against it (`docs/ARCHITECTURE.md` (System call ABI)'s "Writing a program with libjt"). Still open: porting `hello`/`note` onto it, the rest of `cat`/`grep`/`calc`. The shell installs `WC.BIN` into the active filesystem at boot, so `wc FILE` works without running a developer test first.
- [ ] [Sonnet] Native ports of fleet apps (Epiphany etc.) as ring-3 programs against the syscall ABI.
- [ ] [Sonnet] Moveable dock position, menu bar customization, a network status panel in Settings.
### Mac gaps (Joshua, 2026-10-07)
- [ ] [Sonnet] Messages.
- [ ] [Sonnet] FaceTime.
- [ ] [Sonnet] Preview (open and view documents and images).
- [ ] [Sonnet] System Settings as its own app, not panels inside Settings.
- [ ] [Sonnet] App Store.
- [ ] [Sonnet] Podcasts.
- [ ] [Sonnet] Control Center in the menu bar.

<details><summary>Details (0 lines)</summary>


</details>

## Tests and the loop
- [ ] [Sonnet] Dock polish: no white rim on icons, smooth tray and icon corners, hover label with a backing, loading bar drawn at full resolution, Trash visibly empty or full, Terminal out of the default dock (Files, Mail, Calendar, Notes, Reminders, Chat, Weather, Stocks, Settings, Trash).
- [ ] [Sonnet] Boot splash shows the real engraved tree mark at full resolution, not the stick tree.
- [ ] [Opus] Typography QA across Notes, the document app and the Terminal: spacing between letters, baselines, sizes and weights, every printable character, long lines, wrapping, selection. This kernel is a word processor from scratch, so text gets its own checks.
- [ ] [Sonnet] Chat QA: open Chat, switch between models, sign-in gate for subscription models.
- [ ] [Opus] Error handling audit: corrupt or oversized files, full disk, bad input in every text field, missing disk, network or mouse. Each case gets a check.
- [ ] [Opus] Samantha fixes her own bugs: a failing check becomes a draft PR she opens herself (Claude for the hard part at first), gated on `tools/ci-local.sh`. First target: the three compiler warnings in the build.

## Later
- [ ] [Joshua] V1 product vision: full-color UI with crisp icons and fonts, music and video at 100+ fps, clean typography, mobile-first design discipline across every surface.
- [ ] [Joshua] Public APIs and webhooks: exposing kernel features over HTTP for external automation and integration.
- [ ] [Joshua] Performance targets: 100+ fps sustained in all apps, instant launch times, memory efficiency on low-spec hardware.
- [ ] [Joshua] Plugins system. Needs a design pass on what a plugin can touch first.
- [ ] [Joshua] AI agent accounts: settings, bootstrapping, auth. Too undefined to scope yet.
- [ ] [Joshua] Boot-to-disk install flow with install-speed numbers. Needs hardware boot support first.

## Session task queue
Feeds the landing page's "Where it's going" card automatically via `tools/gen/landing-roadmap.py`. Keep titles short, bold, and current. Each item also needs a `(plain: ...)` phrase right after the title, a few plain words a 20-year-old visitor would understand with zero dev background, that phrase is what actually shows on the landing page, never the dev title. Internal refactor work that a visitor has no way to try (nothing to click, nothing that looks different) uses `(plain: skip)`, which the generator drops from the card entirely instead of translating it into vague visitor-facing words.
1. **One input bar and a talking tour** (plain: a demo that talks you through it) [Sonnet]: the phone shows two bars and the tour is silent after the intro.
2. **Pi relay security** (plain: skip) [Opus]: relay TLS is merged. Next: the Pi finds the relay by itself and Turing answers (PRs open), then a board retest.
3. **Per-check QMP ports** (plain: skip) [Haiku]: parallel test runs stop colliding on fixed ports.
4. **Every icon in one style** (plain: icons that match) [Sonnet]: the fleet icons keep their own tile colors.
5. **Photos, Solitaire, Voice Memos** (plain: photos, games and voice notes) [Haiku]: the "Apps after 2.2" list.
6. **Rich document app, richer Weather icons, native code editor, package tool** (plain: a word processor, nicer weather art, a code editor, installable apps) [Sonnet]: after the Pi boots.

## Landing roadmap summary
`tools/gen/landing-roadmap.py` reads this file's Session task queue and takes up to three open, numbered, bold task titles for the landing page's "Where it's going" card, skipping completed entries and escaping for HTML. `tools/checks/landing-roadmap-check.py` and `tools/gen/landing-roadmap.py --check` are the regression checks. A roadmap change triggers the landing deploy workflow, which regenerates the card before upload.

### Pi follow-ups
- [ ] [Opus] Rendered browse: the Mac relay renders JS pages in headless Chromium for `browse -r`; then QuickJS on the Pi with a tiny page model for simple scripts.

## Vision

A computer you talk to, that anyone can trust, that anyone can read. The version plan below is how it gets there.

## Version plan

Where this goes, one major version at a time. Up to 10 it's a plan: each version is one promise kept, and the roadmap holds the steps. Past 10 it's a direction, not a schedule. The loop works top down and moves this file as versions ship.

<details><summary>Details (118 lines)</summary>


### The plan to 10

  - 1.7.7 took the first step: Keyrate runs as a real ring-3 process with its own window, and crashing it on purpose leaves the desktop standing.
- 3.0: Hold it. Joshua Tree runs on a real Raspberry Pi 4: the desktop boots on the board and you can type and click on it, mouse included. Sound from the 3.5 mm jack, Samantha over Wi-Fi through the Mac relay, a new mark. The reference mini PC (UEFI, a real disk, a real network card) moves to a later 3.x.
- 3.1: The Samantha box ships as a kit. The printed case from `docs/hardware/`, a Pi 4, a 7 inch screen, a speaker and the card, bring your own parts (MONEY.md has the math).
- 4.0: Many windows. Real multitasking: a window server, real processes, pipes, a Terminal with tabs and splits. Three apps run at once, one crashes, the other two do not notice. Doom is the proof. May go mobile first: one app full screen, a home grid, gestures.
- 5.0: Talk to it. Samantha is the interface: a full-screen face, speech in and out, a small model on the box. A ten-minute spoken conversation that opens and changes things on the machine. The phone shape, if 4.0 started it, is finished here.
- 6.0: Yours. Accounts, a locked disk, snapshots and backups. Delete a file, go back in time, get it back.
- 7.0: Online. TLS 1.3, Wi-Fi, a real browser, mail and calendar that sync. A normal day of web, mail and calendar on it.
- 8.0: Build things. The Plank compiler on the box, an SDK, apps as signed bundles, a store. Someone who is not Joshua ships an app without touching the kernel.
- 9.0: The kit. The hardware is a product: Neo revisions, two displays, GPU, USB audio, storage and camera. A stranger builds one from the guide and it boots first try.
- 10.0: Daily driver. A security audit against the threat model, accessibility, languages, long-term support. Joshua uses it as his only computer for a month.

### 11 to 100: a direction, not a schedule

- **11 to 20: a family of machines**
  - 11 A laptop: battery, suspend, trackpad
  - 12 macOS 27 Golden Gate parity (Joshua, 2026-11-02: "bridge those gaps for v12"; Wi-Fi, the old 12, shipped in 7.0): GPU compositing and a glass slider, Spaces and Mission Control, Samantha as a system-wide assistant that writes Shortcuts, tab grouping in Blackcomb, Photos edits, parental controls.
  - 13 Bluetooth: headphones, keyboards
  - The rest are goals, not numbers: each takes the next major when its gate passes, in whatever order they finish (Wi-Fi, once the 12, shipped as 7.0). Steps along the way are minors. A goal that only the board can prove waits for Joshua's board test, so the count may stop well short of 20, and that is the point.
  - Printers and scanners: print a note to a real printer over IPP, scan into Photos over eSCL. Gate: both against the Mac's stub printer in QEMU, then a real printer on the desk.
  - External displays and HiDPI everywhere: both HDMI ports, and every app sharp at 2x. Gate: two monitors on the Pi (board).
  - Graphics acceleration on the reference board: the V3D draws the desktop (13.2.0 brought it up). Gate: the board.
  - A 64-bit PC, the third port (it replaces "an ARM port": the Pi has been ARM since 3.0): the x86-64 slices in the Raspberry Pi and ARM64 section. Gate: the desktop in `qemu-system-x86_64`, then a real UEFI PC.
  - Power management that beats the laptop's stock OS on battery life. Gate: the board, timed.
  - Three shipping devices: the dev kit, a laptop and a desktop. Gate: a stranger buys and boots each one.
  - No phone yet (Joshua, 2026-11-15: "it's a computer not a phone", "a working desktop first"): a phone port is for 2027.
  - v20 (Joshua, 2026-11-17, replaces the crayon idea): a redesign in the style of a flat mid-century concert poster (`docs/brand/v20-reference.jpg`). Cream paper with wide margins, blocks of cobalt blue and black, a red sun, a yellow crescent moon, white and yellow dots for clouds and stars, green triangles for the land, and one big serif face. Flat shapes only: no gradients, shadows or glass. Day and night as two panels: the desktop by day, a dark mode by night. Gate: Joshua signs off on the desktop, the dock, one app and the login screen on the board.
  - v20 (Joshua, 2026-11-17): Theming: three to five whole-system themes in Settings, the poster one first. 13.21.0's Accent choice (Blue by default) is the start.
- **21 to 30: an ecosystem**
  - 21 An SDK anyone can build apps with
  - 22 A package manager and updates for every app
  - 23 An app store
  - 24 Accounts and more than one person per machine
  - 25 Backups and restore
  - 26 Sync between your Joshua Tree devices
  - 27 Plugins for the desktop and for Samantha
  - 28 The first hundred apps by other people
  - 29 Localization into the top 25 languages
  - 30 Accessibility complete: screen reader, voice control, high contrast
- **31 to 40: Samantha grows up**
  - 31 You speak, she hears. Voice in, not just voice out
  - 32 She sees the screen and understands it
  - 33 She runs every app for you, end to end
  - 34 She remembers you, on the device, private
  - 35 She plans multi-step tasks and checks her own work
  - 36 Her face is a real-time, fully lip-synced video, on the device
  - 37 Custom characters: pick her face and voice, or make your own
  - 38 She learns from your corrections without the cloud
  - 39 Samantha for developers: she writes and ships apps with you
  - 40 Most people use Joshua Tree by talking
- **41 to 50: trust**
  - 41 A security audit by outsiders
  - 42 The kernel core formally verified
  - 43 Every app sandboxed with explicit permissions
  - 44 Encrypted disk by default
  - 45 Reproducible builds: anyone can prove the image matches the source
  - 46 A bug bounty
  - 47 Messaging, end to end encrypted
  - 48 Federated services you can host yourself
  - 49 Ten years of updates promised per device
  - 50 Trusted enough for banks and hospitals
- **51 to 60: pro tools**
  - 51 The terminal trades, through real brokers
  - 52 Research: fundamentals, filings, screeners
  - 53 A photo editor
  - 54 A music studio
  - 55 A video editor
  - 56 A full IDE
  - 57 A spreadsheet that runs a business
  - 58 A word processor that runs a publisher
  - 59 Design and 3D tools
  - 60 Professionals switch for the tools, not the novelty
- **61 to 70: scale**
  - 61 10,000 devices shipped
  - 62 An education edition for schools
  - 63 Fleet management for companies
  - 64 100 languages
  - 65 100,000 devices
  - 66 Manufacturing partners build their own Joshua Tree machines
  - 67 Joshua Tree certified hardware program
  - 68 A foundation to steward the OS
  - 69 A million devices
  - 70 Profitable on hardware alone
- **71 to 80: new shapes**
  - 71 The kitchen display: Samantha on the counter
  - 72 The car
  - 73 The watch
  - 74 Glasses
  - 75 One OS across all your devices, handing tasks between them
  - 76 Tiny embedded Joshua Tree for appliances
  - 77 Joshua Tree in space (a satellite or a rover payload)
  - 78 A home server edition
  - 79 Accessibility devices built on it
  - 80 Ten form factors, one kernel
- **81 to 90: it improves itself**
  - 81 Samantha writes most new code, reviewed
  - 82 Updates proven safe before they ship
  - 83 The OS tunes itself to your hardware
  - 84 Drivers written by Samantha from a chip's datasheet
  - 85 Every bug report fixed by Samantha within a day
  - 86 A verified compiler
  - 87 New apps generated from a sentence
  - 88 The OS explains every decision it makes
  - 89 People customize the OS by asking
  - 90 It keeps getting better on its own
- **91 to 100: the computer after the phone**
  - 91 10 million devices
  - 92 Talking replaces tapping for most people who use it
  - 93 100 million people
  - 94 Joshua Tree in every classroom that wants it
  - 95 The default OS on a major hardware brand
  - 96 A billion people
  - 97 The trillion dollar plan in the Money section, reached
  - 98 Fully open, fully verified, fully yours
  - 99 The next person builds their own OS on it
  - 100 A computer you talk to, that anyone can trust, that anyone can read

</details>

## Shipped

One line per major version. Each point release is in the GitHub releases and `git log`. The long write-ups from before this trim are in git history (`git show 35aa01cb:docs/roadmap-done.md`).

- **1.x, the base (2026)**: the phone shape, Samantha on a phone, touch and an on-screen keyboard.
- **3.0, Hold it (2026-10-12)**: the desktop on a real Raspberry Pi 4, with apps in user mode.
- **4.0, Many windows (2026-10-18)**: up to four apps at once, each with its own window, a fault ends only its own app.
- **5.0, Talk to it (2026-10-26)**: hold Space and talk to Samantha; she answers out loud and opens apps.
- **6.0, Yours (2026-10-26)**: accounts and sudo, home folders, snapshots and backups.
- **7.0, Online (2026-10-29)**: Wi-Fi without a restart, Blackcomb with pictures, Mail, CalDAV and a Bluetooth keyboard.
- **8.0, Build things (2026-11-01)**: an SDK, apps built and run on the Pi, and a gate passed by an app built by someone other than Joshua, Code, and the painted Joshua tree mark.
- **9.0, The kit (2026-11-02)**: USB touch panels, the step-by-step Nimbus guide, and the software half of the kit.
- **10.0, Daily driver (2026-11-02)**: a screen reader, twelve languages, signed Software Update, and a security audit.
- **11.0, A laptop (2026-11-02)**: USB precision touchpads, sleep, and battery readout.
- **12.0, macOS 27 parity (2026-11-02)**: Spaces, Mission Control, Shortcuts, tab groups, Photos edits and parental controls.
- **13.0, Bluetooth (2026-11-02)**: Bluetooth headphones and keyboards on the Pi's own radio.

## Loop handoff

Updated 2026-11-16, after midnight Vancouver time. The loop's working notes live in `~/.claude/loops/joshuatree.md` on Joshua's Mac; this is the public summary.

**What the loop is.** One task, one branch, one PR, merged the moment CI is green. Every code PR bumps `VERSION` and adds a line to the Shipped log. The repo requires branches to be up to date, so PRs merge one at a time, the next rebased on main.

**Board night, 2026-11-15 (stashed by Joshua's word, picked up next session).** Joshua ran a 13.3.0 card: most of it was broken. Open, in order:
1. Merge #1131 (one film on the landing, 13.9.2), #1132 (board fixes, 13.9.3) and #1133 (Paddle A+ pass, 13.9.4), one at a time, regenerating VERSION and landing facts between them.
2. Reflash the card once with 13.9.3: a stale card cannot set its clock (tls error 54 on every https site) and so cannot `update` to the fix. After that, sign each release with `tools/update-sign.sh` so `update` works.
3. Plain-HTTP downloads stop after the first segment on the board: a 2.1 MB MP3 said "reply cut short, 800 of 2100300 bytes". Reproduce in QEMU against a 2 MB file served from the host, then fix the receive path.
4. A 20 second freeze on the board, cause unknown; and the arrow vanished after a menu bar clock click (fixed blind in 13.9.3, TESTING.md 36 confirms).
5. The wallpaper from Joshua's art brief (branch `pi/wall-anime`): 90s cel dusk, rays, a hatched cloud tower, glints. The tower is still faceted; loop it to A+, then animate the glints and stars with small repaints, then an iPhone wallpaper (1290 x 2796) from the same generator.
6. Paddle, second /mobbin pass (Atari Recharged, Pongfinity, Pong Quest): a title screen with Easy, Normal and Hard; two players on one keyboard (w and s against the arrows); a win screen; special shots last.
7. A Joshua Tree board: a Compute Module 5 carrier designed in KiCad, with a 3D render like a real board's.
8. A verbose always-on self-test screen on the Pi, a boot mode for the bench.
9. Board 1's Blender mockup to A+ (scripts on `pi/wall-anime` in `docs/hardware/board1/`): labels clear of parts, real port shapes, the mark on the silkscreen.
10. Repo tidy: loose files and folders into place, README and docs simplified (Joshua's ask, 2026-11-16).

**Where things stand (2026-11-27).** 13.25.0 puts one real Pi screenshot in the README (Weather on the park, `tools/gen/readme-shots.py`, kept by `readme-banner-check.py`) and retags every [Fable] item [Opus], since Fable left the plan. It also makes the PC's Shut Down really power off: `kernel/acpi.c` reads the RSDP, FADT and `\_S5_` and writes PM1a_CNT, with the old halt screen only for a machine without ACPI (`acpi-off-check.py`, issue #1018). It signs in to Mail from the Mail window: its Sign in button asks the kernel for a sheet (server, address, app password) that the kernel draws, so the password never passes through the ring-3 app (`arm64-mail-sheet-check.py`). 13.24.0 lands three stale PRs on current main and closes a leak. Auto lock on the Pi: the saver or Sleep locks a signed-in desktop, only that person's password opens it, and a half-typed password is wiped when the saver returns (`arm64-lock-check.py`, `arm64-screen-idle-check.py`). Paddle's A+ pass: glide, mouse aim, the wheel, a trail and a big score (`ring3-paddle-check.py` now drives the wheel and a click). Keyrate 2.5.0 as one offline page (`keyrate-offline-check.py`). The leak: fat.c opens a file by its 8.3 form, so a standard user's `cat USERS.TXTX` printed every password hash (and `WIFI.TXTX` the network key); protected names now compare in 8.3 form, and RELAYKEY.TXT is protected too (`arm64-sudo-check.py`). 13.23.1: a held window is the whole pass on the Pi, `app_tick` gives the apps no 10 ms slices while one is dragged or resized (`arm64-ptrburst-check.py`); the board test is to drag with two apps open. 13.23.0 brings x86-64 slice 3: the multiboot memory map, a 4 KiB frame bitmap and kmalloc, and the PS/2 mouse on IRQ12 (`x86_64-mem-check.py`). 13.22.0 also lets a landing PR that touches docs, images, the architecture map, the sticker sheet or the landing's own checks skip the eleven-shard OS suite, and the landing-only job runs headline-sync, facts, roadmap, type-grid and versionsync. The PC's first plain wall is the Pi's blue `#3B6EA5` (was grey), which `wxtest` pins. `landing-shots.yml` refilms stills on a runner. The app wall no longer flashes as you scroll: clips load 600 px early and fade in over their stills (`landing-clip-still-check.py`). CI runs three checks at once on the i386 shards (`SUITE_JOBS`, ARM shards stay serial since each check rebuilds arch/arm64), aiming the slowest shard from ~1000 s at the ARM shards' ~800 s. It gives the Pi's V3D its first drawing work (gpu fill and copy, `arm64-gpu-check.py`), starts the desktop blue by default, and brings x86-64 slice 2: the IDT, the PIC and a 100 Hz PIT, and IRQ1 reading the keyboard (`x86_64-irq-check.py`). 13.21.7 puts the landing on one wallpaper, the Pi's painting of Joshua Tree at dusk, stops the app wall flashing on scroll, and shortens the hero readout to fit a phone. 13.21.6 keeps the hero's `joshua@tree %` readout inside the painted card on a phone (it ran into the right edge). 13.21.5 redesigns the landing hero as one painted card (the park at dusk, "A computer, from scratch." in its sky, frosted glass tiles, a floating nav pill), cuts the README to a few lines, redraws `architecture.svg` as three panels and adds the sticker sheet (`tools/gen/gen_stickers.py`). 13.21.4 repairs the CI shard balancer: it called gh with a flag gh lacks, read no logs, priced every check at 30 s and packed blind, so shard 8 ran 27 minutes of checks while others ran 11. It now packs from the median of real runs and refuses without them, the ARM checks spread over six shards (5 to 10), every shard installs Plank so no repacked check passes by skipping, and each shard proves no check sits on a shard CI never runs (`ci-tooling-check.py`). Slowest shard 1649 s to 926 s of checks. 13.21.2 trims the landing toward apple.com/macos: the top divider and the stats row that repeated the hero are gone, Download PC ISO sits next to Try it, the desktop clips are one sideways strip, and the Pi grid drops the tile that repeated the Protected section (`landing-facts-check.py`, `landing-shots-hero-check.mjs`). 13.21.1 gives tools/feature-clips.py the checks' clean FAT32 card, so the Spaces and Mission Control clips show Reminders as it really looks, not its "No card" line. 13.21.0 (2026-11-18) added update, Bluetooth and Samantha clips to the landing's desktop section, filmed by running their own checks through `tools/film/qemu-system-aarch64`, which records any ARM check unchanged. 13.20.0 (2026-11-18) turned the landing's app wall into short real clips: `tools/landing-shots.py --clips` films each of the 19 apps in i386 QEMU through a short scripted use (`PLAY`), the still stays as the poster, and each clip plays only while on screen and never under reduced motion. A new "desktop" section plays four Pi feature clips from `tools/feature-clips.py`: Spaces, Mission Control, Spotlight and Launchpad. Samantha, Bluetooth and update clips are next. 13.19.2 (2026-11-18) took the typewriter off the landing for good: the headline and the eyebrow now swap their text instantly, and hero-contrast-check.mjs fails if a per-letter animation comes back. 11.0.0, a laptop, is on main: deep idle, trackpad gestures from QEMU multitouch and USB precision touchpads, a MAX17048 battery gauge in the menu bar, and Sleep in the tree menu. 10.0.0 before it brought the screen reader, twelve languages and signed self-update. Lid close is the one 11.0 item left. 12.0.0 finished macOS 27 parity: Spaces, Mission Control, the Glass slider, Shortcuts, Blackcomb tab groups, Photos and parental controls. 13.0.0 brought classic Bluetooth: headphones, speakers and keyboards. 13.3.1 opened Spotify in Blackcomb (a `spotify:` page from the Mac's `tools/claude-relay/spotify.py`); 13.4.0 lets Blackcomb install a signed app it follows a link to, an admin's only once there are accounts.

**Waiting on Joshua.** Signing 11.0's kernel with `tools/update-sign.sh` on his Mac (the app key never leaves it), so a Pi's `update` offers the release. The board pass (`docs/TESTING.md`), with 11.0 flashed when the card is in. The physical halves of 9.0 and 10.0 (a stranger builds a Nimbus from the guide; a month as Joshua's only computer), the GPU and the camera stay open as board work.

**Next, in order.**
1. Whatever the board shows broken (`docs/TESTING.md`).
2. The doc trim: the Shipped log to a line per major (the release notes keep the rest), the 2026-10-03 Pickup to its open items, ARCHITECTURE's history-titled sections in the present tense.
3. 14.0's first slice: a render-only V3D job that clears a tile and stores it (no shaders), run by the `gpu` command, host-tested against the control-list bytes. 13.2.0's bring-up has not been run on the board yet, so this slice says so; after it, GPU work waits on Joshua's board result.
4. The `kernel/kernel.c` split, one file per PR, all checkable in QEMU.

Restart prompt:

```
/loop Joshua Tree after 10.0: (1) whatever the board pass in docs/TESTING.md shows broken; (2) the doc trim: ROADMAP's Shipped log to one line per major, the 2026-10-03 Pickup to its open items, ARCHITECTURE's history-titled sections in the present tense, with the doc checks run by hand since a prose-only PR skips the suite; (3) 12's macOS 27 parity in ROADMAP (Spaces, Mission Control, the Glass slider and Shortcuts are in; next Blackcomb tab groups), then 13 to 15. The GPU (V3D) and the camera need the board: say so, never fake them. Rules: every feature gets a check in tools/checks wired into ci-suite.sh that fails when the fix is reverted; run touched checks locally one QEMU at a time, then the pre-push hook, push, gh pr create, gh pr merge --auto --squash; one PR open at a time; on a red CI read the job log and fix, never retry blind; regenerate generated files on conflict; bump VERSION and the shipped log each code PR; update the Loop handoff in docs/ROADMAP.md each merge; no new .md files; never flash the card unless Joshua says card in; keys never printed; replies to Joshua are 1 line TLDRs
```

Rules kept:
- No bare `git stash` (shared across worktrees).
- Generated files (VERSION, landing/*) take main's side in a merge and are regenerated.
- No keys on the public card. Flash only the dev card, and only when Joshua says it is in.
- Every X.0.0 ships a refreshed logo and UI.
- No number in landing or ad copy without a source (the $35 Pi price was wrong).

## Ingested 2026-10-25
- [ ] Repost to r/osdev once the account has 5+ karma (see launch/roadmap.md).
