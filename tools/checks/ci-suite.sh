#!/bin/bash
# The whole regression suite, run as one thing, reporting everything.
#
# Why this file exists. check.yml used to list ~40 checks as ~40 separate
# workflow steps, which meant the job stopped at the first failure and
# every check after it never ran. On a suite this size that is actively
# misleading: a single timing flake in step 12 hides the real state of the
# other 28, so "CI is red" told you nothing about how red, and a genuine
# regression could sit behind a flake for days. Worse, fixing the flake and
# pushing only revealed the NEXT failure, one run (and six minutes) at a
# time.
#
# So: every check runs, every result is recorded, the summary at the end
# lists all of them, and the exit code is failure if any failed. One run
# now tells you the complete picture.
#
# Retries. Checks marked `retry` in the manifest below get exactly one
# automatic re-run before being called a failure, and only those. The line
# between the two groups is not "flaky things we want to pass" -- it is
# whether the check drives a real QEMU guest through wall-clock waits, in
# which case a shared, loaded runner can genuinely be too slow through no
# fault of the kernel, or whether it is a pure host/static check whose
# result is a function of the tree alone and cannot legitimately differ
# between two runs a second apart. A static check that fails twice is not
# flaky, it is broken, so retrying it would only hide a real regression.
# A retry that succeeds is still printed as FLAKY in the summary, never
# silently laundered into a pass, so a check that starts needing its retry
# regularly is visible and can be fixed properly.
#
# Adding a check: one line in the manifest. Removing one: delete its line.
# Nothing in .github/ needs to change either way.

set -uo pipefail

# Every check's tempfile.mkdtemp(prefix='jt-...') used to land in the shared
# TMPDIR and never get deleted: thousands of 37-75MB dirs, 12GB+, a full
# disk on 2026-10-01. One scratch TMPDIR per suite run, gone on exit.
. "$(dirname "$0")/../ci-lock.sh"
ci_lock_acquire || exit 1
# Under /tmp, not inside the caller's TMPDIR: macOS sets that to /var/folders/<..>/T/, and nested
# under it a check's QMP socket (jt-arm-discovery-XXXXXXXX/discovery.qmp, jt-arm64-sharp2x-reminders-...)
# runs past the 104-byte unix socket limit and connect() fails with "AF_UNIX path too long".
export TMPDIR=$(mktemp -d /tmp/jts.XXXXXX)
trap 'rm -rf "$TMPDIR"; ci_lock_release' EXIT
# SHARD=i runs only the checks assigned to shard i below, so CI can split the
# suite across parallel runners. Unset, it runs everything, same as before.
# Each check carries an explicit shard number (2nd manifest field) instead
# of a round-robin `(NR-1) % SHARDS`: round-robin ignored real duration, so
# whichever shard happened to draw feature-drive.py (208s) and the two
# other 150s+ checks by pure luck of manifest order ran ~2x longer than
# the others every single time. The numbers below are a longest-first
# (LPT) greedy packing of each check's measured wall time from a real run
# (see PR that introduced this comment), so all shards land within ~1s
# of each other instead of one shard alone gating the job. 11 shards since
# 4.12.2, about 650s of checks each (8 at about 300s from 1.6.10 until the ARM checks outgrew shard 6): GitHub gives a public repo
# the extra runners for free, so the wall time roughly halves. Re-run the packing (any LPT/bin
# packing script works) if the suite's shape changes enough to matter --
# a new slow check, or several checks added/removed.
cd "$(dirname "$0")/../.."

# ARM checks (those that run qemu-system-aarch64 or build arch/arm64; tools/gen/ci-balance.py decides) live on
# shards 6, 8, 9 and 10, the ubuntu-26.04 runner with raspi4b. On CI each shard is its own runner; tools/ci-local.sh
# runs those four in turn, since locally they share arch/arm64 and each check cleans it.
# retry?  shard  name                                                   command
manifest() {
cat <<'EOF'
once |8|ARM menu notices: bounded messages, expiry, pixels and input|python3 ./tools/checks/arm64-notice-check.py
once |8|Pi screen sleep: idle deadline, wrap, active apps, refused mailbox requests and keyboard/mouse wake|python3 ./tools/checks/arm64-screen-idle-check.py
once |9|Brick on ARM: collision rules, lives and win under sanitizers; keyboard/mouse play, timed animation, pause, restart, Spotlight and close in QEMU|python3 ./tools/checks/arm64-brick-check.py
once |8|Spotlight on ARM lists every app including the Calculator and Settings on an empty query, typing calc shows it and Enter opens the window, in headless QEMU|python3 ./tools/checks/arm64-spotlight-check.py
once |10|Accounts on the Pi: accounts.c under sanitizers (add, check, refusals, corrupt files, failed writes, PBKDF2 vectors); in headless QEMU adduser, users and deluser work, the password never shows or reaches the UART, and JT/USERS.TXT is a real PBKDF2 record that survives a reboot|python3 ./tools/checks/arm64-accounts-check.py
once |8|Login on the Pi: login.c on the host under sanitizers; in headless QEMU a card with an account boots to the login screen, a wrong password costs a second and stays, the right one brings the desktop and the name, logout returns, a card with no account has no login, and a login that accepts any password is caught|python3 ./tools/checks/arm64-login-check.py
once |6|Admins and sudo on the Pi: sudo.c under sanitizers (first admin, five minute memory, makeadmin, unadmin, damaged SUDOERS.TXT, passwd, same-time unknown name); in headless QEMU a normal user is refused every privileged command, sudo asks a starred password and remembers it, passwd changes it, no password or key code reaches the UART, the quiet boot shows the login card first, and a sudo that skips the check is caught|python3 ./tools/checks/arm64-sudo-check.py
once |8|Home folders on the Pi: home.c and ufile.c under sanitizers over an in-memory FAT (an app's root is JT/HOME/<name>, flush after sign-out, another's folder refused by any path, cat/ls/write/rm walker, Samantha's save, folder-name collisions); in headless QEMU a normal user works in ~ without sudo, cannot touch an admin's folder, an admin needs sudo for his, adduser makes the folder and deluser leaves it, and two mutations are caught|python3 ./tools/checks/arm64-home-check.py
once |4|Home snapshots: snap.c and the real fat.c on a mkfs.fat image under sanitizers (nested copy, SNAP never copied into itself, newest three kept, a snapshot without _DONE ignored and cleaned, restore puts files back and removes later ones, fat_rmdir, fsck.fat clean)|python3 ./tools/checks/arm64-snap-check.py
once |9|Settings on the Pi: settings.c parsing and file text under sanitizers; in headless QEMU Spotlight and the menu bar icons open the window, six sections with their own glyphs, a wallpaper change repaints the whole desktop and a window closed afterwards leaves the new colour, volume and dock size step, and JT/SETTINGS.TXT survives a reboot|python3 ./tools/checks/arm64-settings-check.py
once |7|Crash-safe card: the real fat.c on FAT16 and FAT32 mkfs.fat images under sanitizers, power cut after every sector write of replace (more and less data), delete, mkdir and rmdir; fsck.fat -n reports at most unreachable clusters or unequal FAT copies, the mount scan repairs them and fsck.fat is then clean, the file is exactly old or new, and the entry-before-data mutation fails|python3 ./tools/checks/fat-crash-check.py
once |9|Settings Users pane on the Pi: in headless QEMU the sixth section lists the accounts, adds one (a mismatched second password and a wrong admin password are refused, passwords are stars and never reach the UART), makes an admin on sudo's remembered window, resets a password, removes with a confirm step, keeps it across a reboot, refuses a normal user every admin action while he changes his own password with the old one, and a pane that skips the admin check is caught|python3 ./tools/checks/arm64-users-check.py
retry|8|Stocks on ARM: the tile and Spotlight open the Pi window, the watchlist pills and the line chart are drawn from the labelled sample, Down changes the chart, Esc and the red dot close it, in headless QEMU|python3 ./tools/checks/arm64-stocks-check.py
once |10|Games on the Pi: Spotlight opens Mines, Toroid, Hamurapi, Keyrate, Solitaire, Paddle, Bricks, Depths and Snake at EL0, a reveal (key and click), a generation, a year and a typed word each change the pixels, Esc closes each, in headless QEMU|python3 ./tools/checks/arm64-games-check.py
once |8|Movies on the Pi: Spotlight opens the unchanged user/movies.c at EL0, a motion-JPEG AVI plays all 40 frames in order and its frame code is on the screen, a file that is not a clip shows an error, and the desktop survives, in headless QEMU|python3 ./tools/checks/arm64-movies-check.py
once |6|Magnifying dock on the Pi: the tile under the pointer grows into the room above the tray and follows the pointer, a click still opens it, and leaving puts every pixel back (the plain hover label too), in headless QEMU|python3 ./tools/checks/arm64-dockmag-check.py
once |6|Genie on the Pi: the yellow dot minimizes Calendar's window into its dock tile over five frames or more, the desktop comes back exactly, the app keeps running, and its tile brings the window back pixel for pixel, in headless QEMU|python3 ./tools/checks/arm64-genie-check.py
once |5|Traffic lights on the Pi: the front window's dots are plain until the pointer is over them, a window behind another goes grey, and focus moves the colour, in headless QEMU|python3 ./tools/checks/arm64-traffic-check.py
once |9|Pi keyboard shutdown and restart: confirmed HID chords halt or reboot raspi4b, Escape cancels, busy and hidden prompts refuse (host and QEMU)|python3 ./tools/checks/arm64-power-check.py
once |6|ARM tree menu: click or F10 opens About, Restart, Shut Down and Log Out (pixels, keys, outside click), power rows confirm and refuse mid-answer, Log Out closes windows, raspi4b restarts and halts|python3 ./tools/checks/arm64-treemenu-check.py
once |5|The whole web's roots: the Pi's trust anchors hold every root in Mozilla's list, each once, ISRG Root X1 among them, from a bundle under 18 months old (host only)|python3 ./tools/checks/tls-roots-check.py
once |5|CI tooling packs ARM checks across the ARM shards only and rejects fixed test paths before builds (host only)|python3 ./tools/checks/ci-tooling-check.py
once |7|Dock slot constants agree with kernel.c (static drift guard)|python3 ./tools/checks/dockslots-check.py
once |7|No check hard-codes a fixed temp path or socket, so two suites cannot corrupt each other (static, baseline only shrinks)|python3 ./tools/checks/tmp-paths-check.py
once |7|docs/ARCHITECTURE.md states only what the source says: icon shape and light, fonts, colours, dock and window numbers, caption timings (static)|python3 ./tools/checks/design-doc-check.py
once |7|Kernel memory keeps 16KB clear of the program window (toolchain drift guard)|python3 ./tools/checks/bss-margin-check.py
once |7|Samantha's face loops wrap without a seam|python3 ./tools/checks/face-frames-check.py
once |7|docs/ARCHITECTURE.md lists every check in this suite|./tools/checks/testing-doc-check.sh
once |7|The landing logo rebuilds byte for byte from tools/gen/logo.py|./tools/checks/logo-check.sh
once |7|Landing app tiles are not older than the app code that draws them (source hashes, no QEMU)|python3 ./tools/checks/landing-shots-fresh-check.py
once |7|The landing demo downloads the kernel once, gzipped|./tools/checks/kernel-gz-check.sh
once |7|Samantha box CAD: every fit assert holds (bosses clear the Pi, ports and screen, one-piece shell, the box drops into its stand) and all five files are written; the old stand slot stops it (skips without uv and build123d)|python3 ./tools/checks/samantha-box-check.py
retry|0|Boot check|./check.sh
retry|3|DHCP client leases real SLIRP config, DNS+HTTP still work, nodhcp keeps the old fixed path|./tools/checks/dhcp-check.sh
retry|0|Benchmarks run and report every number|./tools/checks/bench-check.sh
retry|1|ISO boot (CD-ROM and USB/raw-disk paths)|./tools/checks/iso-boot-check.sh
once |1|PNG decoder, host harness|./tools/checks/png-host-check.sh
once |3|Vendored BearSSL TLS 1.2 client subset compiles for aarch64 freestanding (static)|python3 ./tools/checks/bearssl-tls-check.py
once |2|Embedded apps are stored compressed: every one inflates to its exact binary, damaged copies are refused (host harness)|python3 ./tools/checks/user-compress-check.py
once |4|Hamurabi game rules: SplitMix64, golden checksum, invariants, i386-freestanding|./tools/checks/hamurabi-rules-check.sh
once |7|Hamurabi sprite sheet header is current with art/hamurabi/sprites.png and sprites.json (static)|python3 ./tools/gen/gen_hamurabi_sprites.py --check
once |5|Burrow: Files renamed, Samantha still opens it as files / file browser, icon art is the new fox|python3 ./tools/checks/burrow-rename-check.py
retry|4|Burrow icon view wraps a long file name onto a second line instead of cutting it|python3 ./tools/checks/burrow-labels-check.py
retry|5|Mail, Notes and Weather read past byte 255 of their data file (SYS_READ moves 255 bytes a call)|python3 ./tools/checks/read-long-files-check.py
retry|2|Clock opens and its countdown timer updates live|python3 ./tools/checks/clock-check.py
once |2|TTF rasterizer, host harness|./tools/checks/ttf-host-check.sh
once |7|libjt string/stdlib, host harness|./tools/checks/libjt-host-check.sh
once |7|God-file guard (no hand-written .c/.h over its line ceiling)|./tools/checks/godfile-check.sh
retry|0|PNG decoder, in-kernel|./tools/checks/png-check.sh
retry|2|AA text spacing, in-kernel|./tools/checks/textspacing-check.sh
retry|3|AA text stems are dense but still antialiased|python3 ./tools/checks/textsharp-check.py
retry|7|FAT filesystem cycle hang (regression test)|./tools/checks/fatcyclehang-check.sh
retry|5|File robustness: empty, oversized, corrupt-FAT and full-disk cases|python3 ./tools/checks/filerobust-check.py
retry|3|Chat defaults to Samantha (Turing) and surfaces an HTTPS-redirect host clearly|python3 ./tools/checks/chat-samantha-check.py
retry|4|GUI Chat app asks Samantha and renders the reply on screen|python3 ./tools/checks/chatapp-check.py
retry|7|Chat's tools (reminder, note, open app) work locally via /api/pick, ordinary questions still reach Samantha|python3 ./tools/checks/chattools-check.py
retry|3|Chat mail tools: "read my email" lists a real message, "email <someone> <text>" lands one in Mail|python3 ./tools/checks/mailtools-check.py
retry|7|Chat notes/reminders tools: list_reminders and read_notes work via the local keyword fallback when the picker doesn't know them|python3 ./tools/checks/notestools-check.py
retry|3|"samantha" boot flag opens Chat's full-screen avatar view, input focused, before the desktop|python3 ./tools/checks/samantha-boot-check.py
retry|4|Samantha tells the time and the date from her own clock; pagebar hides only her bar|python3 ./tools/checks/samantha-clock-check.py
retry|4|"phone" boot flag opens a real 430x932 portrait frame straight into Samantha's view|python3 ./tools/checks/phone-boot-check.py
retry|0|Phone Samantha back chevron exits her view and F2/Esc hints are hidden on phones|python3 ./tools/checks/phone-samantha-back-check.py
retry|3|Samantha is full screen: room around her (her wall in the side bands, eyes 40 percent down), glass input, fading captions that stay off her lips and eyes, scrollback, the typing bugs, a mouth drawn from her voice, Esc and the red dot|python3 ./tools/checks/samantha-fullscreen-check.py
retry|2|Touch: a tap opens the on-screen keyboard on phone and a tapped key reaches the Notes editor|python3 ./tools/checks/touch-osk-check.py
retry|1|No-disk boot falls back to ramfs with seeded demo files|./tools/checks/ramfs-demo-check.sh
retry|4|Shell regression suite (heap, task, preempt, kill, ring3, ps)|./tools/checks/shellregress-check.sh
retry|4|Small tool picker in C agrees with its Python reference (pickcheck, no network)|./tools/checks/picker-check.sh
retry|1|Ring-3 reference program against the v1 syscall ABI|./tools/checks/usertest-check.sh
retry|5|Ring-3 program writing a real file against the v2 syscall ABI|./tools/checks/notetest-check.sh
retry|1|Keyrate runs as a ring-3 process with its own window; crashing it leaves the desktop alive (v3 syscall ABI)|python3 ./tools/checks/ring3app-check.py
retry|5|Toroid runs as a ring-3 process through the table-driven launcher: draws generations, closes both ways, crashes safely, desktop alive|python3 ./tools/checks/ring3toroid-check.py
retry|7|Calculator runs as a ring-3 process through the table-driven launcher: evaluates 12*3=36 and 5/0=0 through the real parser, closes both ways, crashes safely, desktop alive|python3 ./tools/checks/ring3calc-check.py
retry|0|Quotes runs as a ring-3 process through the table-driven launcher: draws the option grid, answers right and wrong through the real logic, closes both ways, crashes safely, desktop alive|python3 ./tools/checks/ring3quotes-check.py
retry|1|Bookrank runs as a ring-3 process through the table-driven launcher: draws the ranked list, moves the selection by keyboard and mouse through the real logic, closes both ways, crashes safely, desktop alive|python3 ./tools/checks/ring3bookrank-check.py
retry|1|Bookrank pulls the real shelf live (Worker text from a stub), scrolls it, bounds a hostile reply, and shows the ten samples for junk, a 500, an oversize body and no NIC|python3 ./tools/checks/ring3bookrank-live-check.py
retry|2|Patois runs as a ring-3 process through the table-driven launcher: draws the word and choices, answers right and wrong through the real drill, closes both ways, crashes safely, desktop alive|python3 ./tools/checks/ring3tonchi-check.py
retry|1|Patois lists the real courses live (Worker text from a stub), drills one with a score, bounds a hostile reply, and shows the Spanish deck for junk, a 500, an oversize body and no NIC|python3 ./tools/checks/ring3tonchi-live-check.py
retry|3|Fieldbook runs as a ring-3 process through the table-driven launcher: draws the ranked field list, moves the selection by keyboard and mouse through the real logic, closes both ways, crashes safely, desktop alive|python3 ./tools/checks/ring3fieldbook-check.py
retry|4|Clock runs as a ring-3 process through the table-driven launcher: draws the moving time, takes a timer through the real input logic, closes on Esc, desktop alive|python3 ./tools/checks/ring3clock-check.py
retry|7|Mines (Minesweeper) runs as a ring-3 process: a scripted game on the fixed first board is won by keyboard, closes on Esc, desktop alive|python3 ./tools/checks/ring3mines-check.py
retry|7|Solitaire runs as a ring-3 process: a solver wins the fixed first deal and the app plays the win move for move against a rules model, closes on Esc|python3 ./tools/checks/ring3-solitaire-check.py
retry|7|Paddle runs as a ring-3 process: the ball moves on the clock, Up moves your paddle, a point is scored, p pauses, closes on Esc|python3 ./tools/checks/ring3-paddle-check.py
retry|7|Bricks runs as a ring-3 process: the first serve breaks a brick, Left moves the paddle, p pauses, closes on Esc|python3 ./tools/checks/ring3-bricks-check.py
retry|7|Depths runs as a ring-3 process: draws walls, floor and ceiling, hits the creature ahead, walks the maze to the door and gets out, closes on Esc|python3 ./tools/checks/ring3-depths-check.py
retry|7|Snake runs as a ring-3 process: eats the seeded apple ahead, ignores a reverse, dies at the wall, plays twelve apples in step mode, closes on Esc|python3 ./tools/checks/ring3-snake-check.py
retry|8|Chess runs as a ring-3 process: e2e4 by cursor, six typed moves each answered by the computer, the whole game replays legally through the host rules, an illegal move refused, n and esc work|python3 ./tools/checks/ring3-chess-check.py
once|9|Chess rules and opponent on the host: perft counts from five standard positions exact, mate, stalemate and bare kings known, mate in one and a free queen taken, only legal replies|python3 ./tools/checks/chess-core-check.py
retry|4|Running-app dots in the dock on both desktops: Notes (i386), the Console's Terminal and Calendar (Pi) get one small dark dot under their tile while open, and it goes when they close|python3 ./tools/checks/dockdots-check.py
once|5|Every typeface loads at boot: 14 of 14 on i386 and 3 of 3 on the Pi before the desktop, and a broken face (fontbreak=inter) is named and drawn as DejaVu Sans while the desktop still comes up|python3 ./tools/checks/fonts-boot-check.py
retry|5|Backups to the Mac: the relay refuses hostile containers (traversal, absolute, not 8.3, too deep, oversize, cut short, repeats) and writes nothing, keeps a good one whole; the Pi's backup sends ann's home byte for byte, 40 KB file left out|python3 ./tools/checks/arm64-backup-check.py
retry|4|Nimble on the Pi: nimble QUESTION at the Terminal and Samantha's nimble plugin against a stub API, numbered sources, a 502, a 1 MB reply, not-JSON and a hostile reply in plain words, no network says so|python3 ./tools/checks/arm64-nimble-check.py
retry|3|Cmd+Tab and Ctrl+Tab on the Pi: Calendar and Notes open, each chord brings the window behind to the front, and a bare Tab stays with the app|python3 ./tools/checks/arm64-cmdtab-check.py
retry|2|The Pi desktop takes keys from its first moment: Cmd+Space sent the instant the desktop is up opens Spotlight, the first key is stamped and no main-loop step stalls the keys|python3 ./tools/checks/arm64-firstkey-check.py
once|3|Paddle finishes and can be won, and Bricks can be cleared: their physics played to the end on the host by a scripted player|python3 ./tools/checks/games-sim-check.py
retry|0|Movies plays a real AVI with sound at ring 3, audio-led: frame within one of the audio clock, drift under 100 ms, pause holds frame and sound (checked in the wav and on the framebuffer), seek by bar and keys, fullscreen, a damaged clip / non-AVI / over-cap file each show an error, closes on Esc, desktop alive|python3 ./tools/checks/movie-check.py
retry|0|Hamurapi (Hamurabi in the sources) is playable as a ring-3 program: draws its real title scene (sprite pixels equal the sheet), plays a classic reign by keyboard, a story reign with cards and choices and the robot's demo, and every year it logs equals a host replay of the same rules header, closes on Esc, desktop alive|python3 ./tools/checks/ring3hamurabi-check.py
retry|1|Airwell runs as a ring-3 process through the table-driven launcher: the circle grows on the in-breath and shrinks on the out-breath (measured off the framebuffer), all four presets by key and click log the web app's exact phases and seconds, pause holds the circle and the clock, closes on Esc, desktop alive|python3 ./tools/checks/ring3windgate-check.py
once |1|AVI reader, host harness (real JPEG frames, bad headers, truncation, mutation fuzz under ASan/UBSan)|./tools/checks/avi-host-check.sh
retry|5|Portfolio runs as a ring-3 process through the table-driven launcher: draws the fleet catalog, moves the selection by keyboard and mouse through the real logic, closes on Esc, desktop alive|python3 ./tools/checks/ring3portfolio-check.py
retry|2|Activity runs as a ring-3 process through the table-driven launcher: draws the live task list, refreshes it on its own, has the kernel refuse to kill the shell, closes on Esc, desktop alive|python3 ./tools/checks/ring3activity-check.py
retry|5|Contacts runs as a ring-3 process through the table-driven launcher: draws the list, adds and deletes a person through the real prompt, keeps CONTACTS.TXT across fresh runs, closes on Esc, desktop alive|python3 ./tools/checks/ring3contacts-check.py
retry|5|Thinkpond runs as a ring-3 process through the table-driven launcher: draws the ranked list, moves the selection, re-sorts when an upvote passes the leader, closes on Esc, desktop alive|python3 ./tools/checks/ring3hikko-check.py
retry|3|Thinkpond shows the real forum ideas live (Worker text from a stub), scrolls them, keeps votes local and forum order for ties, bounds a hostile reply, and shows the ten demo ideas for junk, a 500, an oversize body and no NIC|python3 ./tools/checks/ring3hikko-live-check.py
retry|2|Reminders runs as a ring-3 process through the table-driven launcher: adds, ticks and deletes items through the real prompt, keeps REMINDERS.TXT across fresh runs, closes on Esc, desktop alive|python3 ./tools/checks/ring3reminders-check.py
retry|5|Scrounge runs as a ring-3 process through the table-driven launcher: falls back to the samples when SYS_HTTP_GET finds no NIC, selects by key and click, the kernel refuses every bad path and pointer the probe hands the syscall, closes on Esc, desktop alive|python3 ./tools/checks/ring3curbfind-check.py
retry|4|Calendar runs as a ring-3 process through the table-driven launcher: gets today from SYS_TIME, draws the month grid, saves an event through the real editor, keeps EVENTS.TXT across fresh runs, feeds Samantha's calendar_today, closes on Esc, desktop alive|python3 ./tools/checks/ring3calendar-check.py
retry|4|Search runs as a ring-3 process through the table-driven launcher: SYS_READDIR refuses a kernel pointer, an over-long path and a missing folder, the list filters live, a file shows its real bytes, a FAT folder opens by relative path with the kernel's cwd untouched, closes on Esc, crashes safely, desktop alive|python3 ./tools/checks/ring3search-check.py
retry|5|Epiphany runs as a ring-3 process through the table-driven launcher: falls back to the offline prices when SYS_HTTP_GET finds no NIC, switches tabs by key and click, runs the command bar, closes on Esc, desktop alive|python3 ./tools/checks/ring3epiphany-check.py
retry|1|Weather runs as a ring-3 process through the table-driven launcher: reads the kernel's WEATHER.TXT and shows the offline face over labelled sample data, R refetches once and restarts it, closes on Esc, desktop alive|python3 ./tools/checks/ring3weather-check.py
retry|0|Burrow runs as a ring-3 process through the table-driven launcher: draws the folder grid, Enter opens a folder and Backspace goes up through SYS_READDIR, closes on Esc, desktop alive|python3 ./tools/checks/ring3burrow-check.py
retry|0|Every ring-3 app (parsed from RING3_APPS) crashes on purpose, is reaped, and the desktop keeps drawing and opens a different app after each one|python3 ./tools/checks/ring3crash-all-check.py
retry|7|A released window framebuffer is supervisor-only again: store faults, pointer into it or into the kernel is -EFAULT|python3 ./tools/checks/userfb-release-check.py
retry|0|SYS_BRK: a program grows 3MB of zeroed heap pages, bad tops are -EINVAL, and its crash gives every frame back (brk live=0, pmm free unchanged), desktop alive|python3 ./tools/checks/ring3brk-check.py
retry|2|Shell launches a ring-3 program by bare name, case-insensitively|./tools/checks/shellname-check.sh
retry|1|QEMU vmmouse absolute-pointer round trip|./tools/checks/vmmouse-check.sh
once |7|Calendar date math, host harness|./tools/checks/check-calendar.sh
retry|1|Settings click acts on the row actually clicked|./tools/checks/settingsclick-check.sh
retry|7|Settings Location geocodes, persists, and fails clean|python3 ./tools/checks/location-check.py
retry|0|Wallpaper defaults to Satellite on a fresh boot|./tools/checks/walldefault-check.sh
retry|0|Idle tour's Settings visit doesn't change the wallpaper theme|python3 ./tools/checks/walldemo-regression-check.py
retry|3|Wallpaper compose: Map and Satellite fetch distinct, byte-correct buffers (hermetic, fake tile server)|python3 ./tools/checks/wallcompose-check.py
retry|3|Notes editor chrome doesn't redraw on plain keystrokes|./tools/checks/editorflash-check.sh
retry|4|System-wide clipboard: Ctrl+C/X/V round-trips real text within Notes, across Notes->Terminal, truncates a too-long paste cleanly|python3 ./tools/checks/clipboard-check.py
retry|5|Reminders prompt redraws content, not chrome, per keystroke|./tools/checks/gui-prompt-keystroke-check.sh
retry|7|Notes typing, typography, pointer controls, persistence|python3 ./tools/checks/editor_qa.py
retry|2|Notes' runtime-TTF text is real antialiased rasterization at 12pt and 200pt, not a duplicated-block bitmap upscale|python3 ./tools/checks/notessharp-check.py
retry|3|Notes folders: legacy NOTES.TXT migrates intact, a new note lands in the current folder, both survive reboot, delete asks first|python3 ./tools/checks/notesfolders-check.py
retry|5|Notes autosave: a changed note saves itself about two seconds after the last key (never per keystroke), survives a red-dot close that kills the program, and is back on screen after reopening; with autosave off in a private build the same check fails|python3 ./tools/checks/notes-autosave-check.py
retry|1|Books (2.66): the ring-3 reader lists BOOKS/, refuses a book over 3 MB, turns two pages, saves the page to BOOKS/TEST.POS and reopens on it after a reboot|python3 ./tools/checks/books-check.py
retry|3|Terminal grid draws the mono face at its true advance|python3 ./tools/checks/termmono-check.py
retry|4|Terminal's runtime-TTF text is real antialiased rasterization with a driftless monospace grid|python3 ./tools/checks/termsharp-check.py
retry|2|Panes: independent scrollback per tab, tab rail, activity dot on a background tab, close, split panes with their own shells|python3 ./tools/checks/panes-check.py
retry|3|Claude app: asks Claude Code through the relay (stub claude on the host), draws the reply, resumes the session, red error line on a wrong token, a stopped relay and no relay set (-EACCES at once)|python3 ./tools/checks/ring3claude-check.py
once |3|Claude relay: token refused and accepted, session resume, exact read-only argv, prompt on stdin, timeout kills the process group, size cap, one request at a time, log hygiene, a mutant without the token check lets a tokenless request in, and --turing against a stub serve.py answers M Samantha with [[note]] intact, 502 when Turing is down, and --api-key-file refuses to start without --allow-api-spend|python3 ./tools/checks/claude-relay-check.py
once |7|Relay voice: POST /api/tts against a stub ElevenLabs returns its PCM (22050 Hz mono); text over the per-request cap and requests past the per-day cap are refused with no upstream call; the day count survives a relay restart and resets on a new date; failures give characters back; the key never appears in a response, header or log, even when the upstream echoes it; a tokenless request is 401; a key file others can read is refused (no network)|python3 ./tools/checks/relay-tts-check.py
once |7|Relay API mode: file tools refuse ../, absolute paths, dot files, symlinks and unlisted names and cap reads at 20000 characters; model routing; the prompt names both actions; the reply's M line names the model that answered (Claude Haiku 5.5, Sonnet, Opus) and ask.c's default prompt is Samantha, the relay's Turing name (no network)|python3 ./tools/checks/relay-api-check.py
retry|5|Apple-menu hover stays cheap, clock redraws on a minute change|./tools/checks/menuclock-check.sh
retry|2|Lock Screen: menu item locks, Esc cannot bypass, password unlocks|python3 ./tools/checks/lockscreen-check.py
retry|2|Multi-window chrome doesn't redraw on plain keystrokes|./tools/checks/mwkeyflash-check.sh
retry|7|Drawing lands offscreen, window_present puts it on screen|./tools/checks/backbuffer-check.sh
retry|3|Multi-window apps draw exactly one toolbar, not two|./tools/checks/mwdupetoolbar-check.sh
once |0|JPEG decoder, host harness (baseline and, since 6.46.0, progressive)|./tools/checks/jpeg-host-check.sh
once |1|Media decoders (WAV, MP3), host harness under ASan and UBSan|./tools/checks/media-host-check.sh
retry|7|Tasks keep their own x87 float state across switches|./tools/checks/fpu-check.sh
once |7|HTML entities decode to ASCII, host harness|./tools/checks/html-host-check.sh
retry|1|JPEG decoder, in-kernel|./tools/checks/jpeg-check.sh
once |4|PNG/JPEG decoder fuzz (ASan/UBSan, truncation+mutation+nasties)|./tools/checks/decoder-fuzz-check.sh
once |4|iCalendar and CalDAV parser fuzz (ASan/UBSan, truncation+mutation; 6.48.1)|./tools/checks/ics-fuzz-check.sh
once |4|Blackcomb page and Set-Cookie parser fuzz (ASan/UBSan, truncation+mutation; 6.48.2)|./tools/checks/browser-fuzz-check.sh
once |4|IMAP reply and MIME parser fuzz (ASan/UBSan, truncation+mutation; 6.48.3)|./tools/checks/mail-fuzz-check.sh
once |4|Wi-Fi scan result and frame parser fuzz (ASan/UBSan; 6.48.4)|./tools/checks/wifi-fuzz-check.sh
once |4|DNS reply reader fuzz, both kernels (ASan/UBSan; 6.48.5)|./tools/checks/dns-fuzz-check.sh
once |4|App image check fuzz (ASan/UBSan, truncation+mutation, relocations applied; 6.51.0)|./tools/checks/uimg-fuzz-check.sh
once |4|App signature check fuzz with the vendored BearSSL (ASan/UBSan; a throwaway key through tools/signapp.sh; cuts, mutations, a flipped image byte, another name; 6.54.0) and every store app in landing/apps signed by the shipped key (6.55.0)|./tools/checks/appsig-fuzz-check.sh
once |1|HTTP/JSON/FAT16 parser fuzz (ASan/UBSan, truncation+mutation+nasties)|./tools/checks/parser-fuzz-check.sh
once |7|Text-input bounds: json.c maxlen 0/1/truncation + auth const-time compare (ASan/UBSan)|./tools/checks/input-bounds-check.sh
once |2|Password hashing, host harness: PBKDF2-HMAC-SHA256 vectors (RFC 7914, RFC 6070 for SHA-256), legacy record upgrade, USERS.TXT parse|./tools/checks/auth-check.sh
retry|2|Dock apps open/close from the pointer alone|python3 ./tools/checks/appclose-check.py
retry|5|Esc on a bare desktop does not quit the GUI; Mail still opens from the dock|python3 ./tools/checks/esc-desktop-check.py
retry|2|Dock hover survives mid-animation|python3 ./tools/checks/dockhover-check.py
retry|4|Launchpad tile click launches, doesn't just close the folder|python3 ./tools/checks/launchpad-click-check.py
retry|1|Launchpad exits to the desktop: Esc, empty click, Apps tile again; web demo forwards Esc|python3 ./tools/checks/launchpad-exit-check.py
retry|4|Apps folder layout (no black band, no row spill, no ghost icons)|python3 ./tools/checks/appsfolder-layout-check.py
retry|5|Menu bar present after Launchpad, every app open and close, Esc, panels, drags; screen never black|python3 ./tools/checks/menubar-persist-check.py
retry|2|Launchpad centered, evenly padded, icons 56px or smaller, labels clear (pixels, five screen sizes)|python3 ./tools/checks/launchpad-centered-check.py
retry|0|Typography: baseline flatness, letter-gap variance, container padding|python3 ./tools/checks/baseline-check.py
retry|0|Multi-window (click-to-focus, real z-order compositing)|python3 ./tools/checks/multiwindow-check.py
retry|1|Ring-3 window: Reminders beside Notes, keys to the focused window only, a crash closes only its window|python3 ./tools/checks/ring3window-check.py
retry|7|Ring-3 stress: window task and desktop hammer heap and FAT at once, heap walk ok, file at root, desktop alive|python3 ./tools/checks/ring3stress-check.py
retry|2|Ring-3 PDE sync: kernel page table born after the window task is visible on its CR3, close clean, desktop alive|python3 ./tools/checks/pdesync-check.py
retry|3|Window snapping (title-bar drag to edge/corner, real pixel proof)|python3 ./tools/checks/windowsnap-check.py
retry|7|Ring-3 window resize: snap Notes to a quarter, JT_EV_RESIZE answered, new buffer mapped, app redrew at the quarter size (serial marker + far-corner pixels)|python3 ./tools/checks/ring3resize-check.py
retry|5|Windows drag live by their title bar (single-window Notes and multi-window Files)|python3 ./tools/checks/windowdrag-check.py
retry|3|Windowed apps start under the title bar, Calendar fits six weeks|python3 ./tools/checks/apptop-check.py
retry|0|Calendar Day, Week, Month and Year views (ring-3 program)|python3 ./tools/checks/calviews-check.py
retry|4|QA gallery: every app opens, screenshots, closes, no crash|python3 ./tools/checks/qa-gallery.py
retry|2|Every app's main action, headless|python3 ./tools/checks/feature-drive.py
retry|5|Dock icon edge quality (no staircased corners)|python3 ./tools/checks/iconedge-check.py
retry|7|Dock icon halo (clean clip to the tray, no glyph bleed)|python3 ./tools/checks/iconhalo-check.py
retry|3|Dock tiles draw the colour their authored SVG states (incl. Samantha)|python3 ./tools/checks/iconart-check.py
retry|0|Dock icon lighting (one soft top light, top highlight, no dark outline)|python3 ./tools/checks/iconlight-check.py
once |3|Every authored icon shares one tile silhouette, AA edges, glyph margin|python3 ./tools/checks/iconinset-check.py
retry|7|Calendar dock tile is a page-and-grid picture, no date or dash, same on every date|python3 ./tools/checks/calicon-check.py
retry|1|Clock icon is a live analog face: hands follow the RTC and redraw on the minute|python3 ./tools/checks/clockicon-check.py
retry|3|Shadow under the dock darkens the photo, no flat bands|python3 ./tools/checks/dockband-check.py
retry|5|Titlebar traffic-light AA (real coverage blend, not binary)|python3 ./tools/checks/titlebar-aa-check.py
retry|4|Dock tray corner AA (real coverage blend, not binary)|python3 ./tools/checks/traycorner-check.py
retry|0|Portfolio catalog opens, lists the fleet, and the list scrolls|python3 ./tools/checks/portfolio-check.py
retry|4|Asking Samantha for the weather fetches it, no "open Weather first"|python3 ./tools/checks/samweather-check.py
retry|5|Menu bar mark AA (no false interior seams) on its terracotta ground|python3 ./tools/checks/bootlogo-check.py
retry|4|Boot splash draws the real landing/logo.svg mark, not the old stick tree|python3 ./tools/checks/bootmark-check.py
retry|5|Boot splash draws the tree on stroke by stroke, never pops in whole, terracotta ground last|python3 ./tools/checks/bootanim-check.py
once |7|Landing eyebrow tracks roadmap Latest, H1 stays the brand line|./tools/checks/landing-headline-check.sh
once |7|Idle tour still cycles all 8 real dock apps|node ./tools/checks/tourappcount-check.mjs
once |7|Idle tour autoplay fix is in place (tourArmed reset)|node ./tools/checks/idletour-arm-reset-check.mjs
once |7|RTC local-time shift math (v86's CMOS answers in UTC)|node ./tools/checks/rtc-timezone-check.mjs
retry|5|Stocks opens without a supported network card|python3 ./tools/checks/stocks-dock-check.py
retry|1|Stocks chart line is antialiased (coverage blend, no stair-stepping)|python3 ./tools/checks/stocks-aa-check.py
retry|0|Stocks fills all five watchlist rows from a full-size stub Worker reply (fails if the app reads only part of STOCKS.TXT)|python3 ./tools/checks/ring3stocks-list-check.py
once |2|Stocks live quotes and kernel parsing|node ./tools/checks/stocks-live-check.mjs
retry|4|Epiphany command bar: AAPL GP draws the chart, an unknown code errors cleanly|python3 ./tools/checks/epiphany-cmdbar-check.py
once |7|Worker /api/proxy allowlist|node ./tools/checks/worker-proxy-check.mjs
once |7|Worker /api/waitlist store, validate, count|node ./tools/checks/waitlist-check.mjs
once |7|Mail Send: Worker /api/mail/send guards and Resend shape, kernel bearer wiring|python3 ./tools/checks/mailsend-check.py
once |1|Worker /api/proxy: a silent upstream cannot hang the guest (weather/chat freeze regression)|node ./tools/checks/weatherproxy-hang-check.mjs
once |7|Worker /api/listen: Whisper transcription, 503 without the AI binding, rejects oversize/empty, per-IP rate limit|node ./tools/checks/listen-worker-check.mjs
retry|4|Every app opens and closes by keyboard alone|python3 ./tools/checks/keyboard-only-check.py
once |5|Soak: every app opened and closed once each in one boot, no leak, no crash|python3 ./tools/checks/soak-check.py 1
retry|7|Accounts: legacy record upgrades to PBKDF2 on login, create, reboot, login, wrong and empty passwords rejected, change password|python3 ./tools/checks/auth-flow-check.py
retry|7|Entropy pool: serial names its sources, two boots draw different salts|python3 ./tools/checks/entropy-check.py
once |7|Panic screen: a ring-0 fault paints the reason, not a frozen desktop|python3 ./tools/checks/panic-check.py
once |7|Crash report names the faulting function, not just the exception kind|python3 ./tools/checks/panic-symbols-check.py
once |3|Frame time: idle, dock hover and window open stay within budget|python3 ./tools/checks/frametime-check.py
once |3|Every check in tools/checks/ is in this manifest or says why not|./tools/checks/suite-coverage-check.sh
retry|7|Text selection in Notes: Shift-arrow/Ctrl+A highlight, edsel/edcopy/edcut markers, selection-aware copy/cut/paste/delete|python3 ./tools/checks/textselect-check.py
retry|2|Window top edge and corner arc are one continuous AA shape|python3 ./tools/checks/windowedge-check.py
retry|2|Sound Blaster 16 detects, beep plays a real 440Hz tone, card-less boot is a no-op|python3 ./tools/checks/sb16-check.py
retry|2|Music runs as a ring-3 app: plays a fixture WAV off a FAT disk (position from the driver, pause holds, bar and arrow seeks land, next changes the song, oversize file refused) and the sound card hears the tones|python3 ./tools/checks/music-check.py
retry|7|Music QA: a 3:25 MP3 reads 3:25, a damaged MP3 frame no longer hangs the song, an unplayable song is skipped when one ends into the next and n moves on from it, the queue stays under half a second ahead, shuffle plays each song once, the wheel scrolls the list, no row sits under the key hint, a half-screen window keeps its pills, an empty library is quiet|python3 ./tools/checks/music-qa-check.py
retry|7|Sound Blaster 16 record path: `listen` reaches the driver and times out cleanly (QEMU has no ADC backend), card-less boot is a no-op|python3 ./tools/checks/sb16-record-check.py
retry|4|Chat speaks: say fetches /api/speak PCM from a stub and plays a real 1000Hz tone|python3 ./tools/checks/chat-speaks-check.py
retry|5|Chat face: idle frame before, talk frames while she speaks, idle after; no frames means no face|python3 ./tools/checks/chat-face-check.py
retry|0|HTTP stress: a ring-3 app makes 72 sequential SYS_HTTP_GET calls, all succeed (none -EIO), heap free stays at baseline|python3 ./tools/checks/httpstress-check.py
retry|0|Burrow view switcher: List/Icons choice is saved to BURROW.TXT and a fresh run reads it back|python3 ./tools/checks/filesview-check.py
retry|1|Demo canvas fills its frame, pixelated only at an exact 1:1 map|node ./tools/checks/democrisp-check.mjs
retry|5|Landing hero: a real portrait before any click (pixels, undistorted), default boot opens Samantha's face, Esc reaches the desktop, ?desktop opts out, phone and tablet hold; fails if the poster is removed|node ./tools/checks/hero-poster-check.mjs
once |0|Landing hero: Click or Tap to boot lifts the poster onto her face well before the 60 s fallback, even after the serial log is trimmed|node ./tools/checks/landing-poster-click-check.mjs
once |3|Landing page never overflows horizontally at phone widths|node ./tools/checks/mobile-overflow-check.mjs
once |3|Landing opens on screenshots, the live demo is at ?demo|node ./tools/checks/landing-shots-hero-check.mjs
once |1|Landing: benchmark labels clear, one app count, See the desktop link styled, sections visible on load|node ./tools/checks/landing-layout-check.mjs
once |7|Portfolio voice: on by default, mute button top right, remembered across a reload, absent outside portfolio|node ./tools/checks/portfolio-mute-check.mjs
retry|4|Portfolio tour starts even when his face frames are slow and the first Escape presses are lost: his chat exits and Epiphany opens|node ./tools/checks/portfolio-slowframes-check.mjs
once |0|Landing demo on a phone: the chat bar raises the keyboard, typed letters reach Samantha once each and Send is Enter|node ./tools/checks/mobile-type-check.mjs
once |7|Type system: every landing font size on the 9-step scale, line heights on the 4px grid, token block and ARCHITECTURE.md current, Tree Sans files load|python3 ./tools/checks/type-grid-check.py
once |4|Landing demo: no keyboard trap, one main landmark, Full screen top right on phones|node ./tools/checks/landing-demo-ui-check.mjs
once |3|Landing on a phone: exactly one visible "Message Samantha" input, focusable and typeable|node ./tools/checks/phone-one-input-check.mjs
once |1|Landing on a desktop: her input is one sharp page field over her face, focusable and typeable|node ./tools/checks/desktop-pagebar-check.mjs
retry|7|App switcher: Ctrl+Tab cycles open windows and focuses the highlighted one|python3 ./tools/checks/appswitcher-check.py
retry|2|Screenshot key: Ctrl+Shift+3 saves a real framebuffer BMP, numbered and visible in Files|python3 ./tools/checks/screenshot-check.py
retry|2|Drunk mode easter egg: horizontal sway applied to framebuffer rows|python3 ./tools/checks/drunk-mode-check.py
retry|7|Mac speech to text: stt.py turns a spoken sentence (macOS say, 16 kHz PCM) into words through whisper.cpp, and refuses a wrong token, an odd body and a GET (skips off a Mac)|python3 tools/checks/stt-check.py
retry|7|Brand tokens: docs/brand/tokens.css matches the kernel's desktop colours, the accent, the brand page links it and the map reads it|python3 tools/checks/brand-tokens-check.py
retry|10|ARM64: the aarch64 kernel boots under QEMU and prints over the UART (skips where the tools are missing)|python3 ./tools/checks/arm64-m0-check.py
retry|6|ARM64: a tiny local language model answers under QEMU (skips where the tools are missing)|python3 ./tools/checks/arm64-llm-check.py
retry|6|ARM64: with the relay out of reach, Samantha answers from the hosted Turing over HTTPS (skips where the tools are missing)|python3 ./tools/checks/arm64-sam-hosted-check.py
retry|6|ARM64 M1c: the aarch64 kernel draws a desktop into a ramfb framebuffer and QEMU screendump shows it (skips where the tools are missing)|python3 ./tools/checks/arm64-m1c-check.py
retry|9|ARM64 M2: the aarch64 kernel drives virtio disk, network, keyboard and mouse: a sector read back, a real ARP answer, key presses, moves and clicks (skips where the tools are missing)|python3 ./tools/checks/arm64-m2-check.py
retry|6|ARM64 console scrollback: Page Up, End and Home scroll the on-screen Console over the whole boot log, the title bar says which lines (skips where the tools are missing)|python3 ./tools/checks/arm64-console-scroll-check.py
retry|6|ARM64 net: the shared IP stack (drivers/net.c, drivers/http.c) on virtio-net leases 10.0.2.15 by DHCP and POSTs to a host server, 200 and the exact reply length (skips where the tools are missing)|python3 ./tools/checks/arm64-net-check.py
retry|8|ARM64 HTTPS: a TLS 1.2 page from a host server with a throwaway certificate built in as a trust anchor, refused (tls FAIL 62) when it is left out (skips where the tools are missing)|python3 ./tools/checks/arm64-tls-check.py
retry|10|ARM64 browser: browse URL typed at the Terminal's ask> row reads an HTTPS page through a redirect, prints readable text with numbered links, open N follows one (its connection resuming the TLS session, 6.45.0), a redirect loop stops (skips where the tools are missing)|python3 ./tools/checks/arm64-browser-check.py
retry|9|Real pages on the Pi: the browser reads a 2 MiB chunked gzip page and a 300 KiB plain one whole off a host server, asks for HTTP/1.1 and gzip, and refuses a chunk cut short, in headless QEMU|python3 ./tools/checks/arm64-realpages-check.py
retry|8|ARM64 browser, dead server: a host that accepts and never answers, and a closed port, each end in one timeout line (tls, reply, connect) within 12 s with dns ok and tcp ok printed first, and the prompt still answers (skips where the tools are missing)|python3 ./tools/checks/arm64-browser-deadserver-check.py
retry|6|ARM64 Browser window: Spotlight opens it, the address bar loads a page from a stub server, a click on the link line follows it, Alt+Left, Alt+Right and F5 navigate, a long page scrolls and stops at its end, a dead port says so in plain words, Esc closes; accent link colour and off-white page in a screendump (skips where the tools are missing)|python3 ./tools/checks/arm64-browser-app-check.py
retry|6|Page styles in Blackcomb: h1 and h2 drawn as big bold headings, h3 to h6 bold, <b> runs and font-weight rules (6.34.0) bold and only them, centred lines (6.35.0), right-aligned ones (6.44.0), descendant bold and align rules (6.47.0), taller ink for the big heading on a screendump, and the Terminal prints the page without style bytes, in headless QEMU|python3 ./tools/checks/arm64-pagestyle-check.py
retry|10|Inline images in Blackcomb: a page's PNG and baseline JPEG are fetched after its text, scaled to fit, drawn in place with their colours on a screendump, and a missing one says it failed, in headless QEMU|python3 ./tools/checks/arm64-images-check.py
retry|8|Forms and cookies on the Pi: a login form posts with its hidden token, the redirect comes back with the session cookie and not a Secure one over http, a GET form sends its query, fill never prints a value, and the Blackcomb window does it with Tab and Enter, in headless QEMU|python3 ./tools/checks/arm64-forms-check.py
retry|7|Form controls on the Pi: checkboxes tick, radios pick one of their name, a select steps or takes an option by value or label, the form sends exactly the ticked and picked values, and Blackcomb changes a control in place, in headless QEMU|python3 ./tools/checks/arm64-formctl-check.py
retry|6|What a page hides stays hidden: simple display:none rules from <style>, the hidden attribute and inline display:none skip an element whole, two-part descendant rules too (6.33.0) and child rules (6.49.0) and three-part rules (6.50.0), @media rules are left alone, and a hidden menu's link takes no number, in headless QEMU|python3 ./tools/checks/arm64-csshide-check.py
retry|8|Cookies kept per person: a persistent cookie survives a restart in its owner's keychain, a session cookie does not, another account gets none, cookies clear is kept, and no value is on the card in plain text or on the UART, over four boots of one card in headless QEMU|python3 ./tools/checks/arm64-cookiekeep-check.py
retry|10|History you can search: three pages visited (one twice) are listed newest first once each, found by every word in any case, ranked by visits, found from Blackcomb's address bar as you type (6.38.0), and cleared, in headless QEMU|python3 ./tools/checks/arm64-history-check.py
retry|9|Tabs on the Pi: four at most, each with its own page and history, from the Terminal's tab commands and Blackcomb's Cmd+T, Ctrl+Tab, Cmd+W and tab strip (6.39.0) (the last tab's Cmd+W closes the window), in headless QEMU|python3 ./tools/checks/arm64-tabs-check.py
retry|8|Downloads on the Pi: a link to a PDF is saved to JT/DOWNLOAD under an 8.3 name, never over a file (a hostile /wifi.txt leaves the card's own WIFI.TXT) with the page left in place, fetch URL saves any reply, a 404 is not saved, Blackcomb's status line says where, and the card image holds the exact bytes, in headless QEMU|python3 ./tools/checks/arm64-downloads-check.py
retry|9|Mail on the Pi: IMAP over TLS against a fixture server, the app password hidden and refused when wrong, the newest messages listed, plain, quoted-printable HTML and base64 bodies shown whole past a fake tagged ending, a mail's image never fetched, mail logout forgets, in headless QEMU|python3 ./tools/checks/arm64-mail-check.py
retry|10|Mail as pages in Blackcomb: mail: before a login says how to set it up, the inbox as links with a New message form, a message with a Reply form, Reply and Send through STARTTLS SMTP, multi-line bodies (6.40.0), and mail: typed in the window's address bar, in headless QEMU|python3 ./tools/checks/arm64-mailpages-check.py
retry|8|Sending mail on the Pi: SMTP submission only after STARTTLS (a server without it is refused before any AUTH), AUTH PLAIN with the app password, dot-stuffing, and mail reply to the listed sender, in headless QEMU|python3 ./tools/checks/arm64-smtp-check.py
retry|6|The keychain on the Pi: the mail sign-in sealed in ann's home under her login password, kept across a restart and a passwd, not bob's, and a flipped byte opens nothing, over five boots of one card in headless QEMU|python3 ./tools/checks/arm64-keychain-check.py
retry|10|Picking Wi-Fi on the Pi: wifi says what it joins and heard, wifi join NAME takes a passphrase with stars, refuses a short one, and saves JT/WIFI.TXT as the name and exactly Python's PMK, never the passphrase, in headless QEMU|python3 ./tools/checks/arm64-wifi-join-check.py
retry|9|Wi-Fi in Settings: the seventh section lists made-up heard networks (WIFI_FAKE build), Up and Down pick, the password field keeps key lines off the UART, and Enter saves JT/WIFI.TXT as Python's PMK, in headless QEMU|python3 ./tools/checks/arm64-wifi-pane-check.py
retry|10|Calendar subscriptions on the Pi: calendar subscribe reads a fixture iCalendar feed into SUBS.TXT (folded, escaped, same-day events joined, a non-feed refused), Calendar loads the days the person has no event on, and EVENTS.TXT is untouched, in headless QEMU|python3 ./tools/checks/arm64-ics-check.py
retry|9|Two-way CalDAV on the Pi: https only, the app password with stars, a wrong one refused, the person's day PUT as jt-YYYYMMDD.ics with its UID, the calendar REPORTed back into SUBS.TXT with XML and CDATA undone, deletes both ways (6.41.0), the calendar discovered from the server root (6.42.0), a sync on opening Calendar (6.43.0), a choice of calendars that never deletes across them (6.48.0), in headless QEMU against a fixture server|python3 ./tools/checks/arm64-caldav-check.py
retry|8|ARM64 Browser bookmarks and Top Sites (6.37.0): the star in the address bar (click and Ctrl+D) saves the open page as a line of JT/BOOKMARK.TXT, read back off the card image; a fresh boot lists the saved pages on the empty Browser, Tab then Enter opens one, a star on a saved page takes it off the card (skips where the tools are missing)|python3 ./tools/checks/arm64-bookmarks-check.py
once |6|YouTube in Blackcomb: yt.py (a saved yt-dlp reply) answers a video page only with the token and an id, and a youtu.be address in Blackcomb shows the video's title with the token kept off the UART, in headless QEMU|python3 ./tools/checks/arm64-youtube-check.py
once |9|ARM64 audio: `beep` fills the ring with a tone whose checksum matches Python, the PWM FIFO gets it in order on a fake Pi, stuck hardware gives up with one line, GPIO 42 and the boot path stay untouched (QEMU part skips where the tools are missing; the real jack is unverified)|python3 ./tools/checks/arm64-audio-check.py
retry|8|ARM64 volume: `vol 50` halves the ring sum of a beep against `vol 100`, `vol 0` is a silent ring, F11/F12 and the Mac media keys step the level before any pane with a "Volume N" menu bar notice and a tick, never past 0 or 100 (QEMU, skips where the tools are missing; the real speaker is unverified)|python3 ./tools/checks/arm64-volume-check.py
retry|10|ARM64 Samantha tile: a click on the Samantha dock tile opens the Terminal with `sam ` already typed on the ask> line, even with the Terminal already in front, and Enter runs `sam` (QEMU, skips where the tools are missing)|python3 ./tools/checks/arm64-samtile-check.py
retry|9|ARM64 boot splash bar: the bar under the logo glides with boot, dozens of distinct fills and no jump over 5 percent, and the pixels on screen match (QEMU, skips where the tools are missing)|python3 ./tools/checks/arm64-splashbar-check.py
once |7|ARM64 green light: the Pi's activity LED is blinked through the firmware mailbox (pin 42), never at boot, on a wait that ends (static, no hardware)|python3 tools/checks/arm64-led-check.py
once |4|Pi actions: an answer's [[note]], [[say]], [[led blink]], [[open]], [[browse]] (http/https only), [[calc]] and [[status]] lines are stripped and recorded by ask.c's real parser (compiled on the host), four per turn, unknown ones recorded to ignore, the rest stays as text, and the relay prompt names them all|python3 tools/checks/pi-actions-check.py
once |10|Pi release image carries no relay token: pi-ask.o built without JT_WIFI_DEV=1 has none, the dev build does (host only)|sh tools/checks/pi-release-notoken-check.sh
retry|10|ARM64 agent loop: against a fake relay, [[browse]] then a final answer with agent: step 1 and 2 logged, 4 actions per turn and 5 steps then step limit, Esc stops it, unknown actions logged and ignored (skips where the tools are missing)|python3 ./tools/checks/arm64-agent-check.py
retry|8|ARM64 /model and /effort: two F3 sessions preserve separate settings, conversation and typing, block switching while busy; slash commands run on the Pi, model and effort reach a fake relay only when not default, a bad value is refused in one line and never sent, /status /help /clear (skips where the tools are missing)|python3 ./tools/checks/arm64-model-check.py
retry|10|ARM64 Claude in the Terminal: the Console takes no input; `sam QUESTION` typed at the Terminal's joshua@tree % prompt reaches the real relay (stub claude, 127.0.0.1) over virtio-net and the answer prints as sam: in 53-column lines; after a stub Turing answers /status says Samantha; wrong token -401, no network and no token each say so (skips where the tools are missing)|python3 ./tools/checks/arm64-claude-console-check.py
retry|6|ARM64 zsh shell: the Terminal prompt is joshua@tree %, an unknown word prints zsh: command not found, `sam QUESTION` returns to the prompt at once while a stub relay delays its answer, a command typed meanwhile runs, a second sam says still thinking, then the delayed sam: answer prints above the prompt|python3 ./tools/checks/arm64-zsh-check.py
retry|6|ARM64 relay token gate: a 24-byte throwaway token is absent from kernel8.img with JT_WIFI_DEV unset and present with it set (skips where the tools are missing)|python3 ./tools/checks/arm64-token-gate-check.py
retry|9|ARM64 M3a and M3b: an unprivileged EL0 program prints through a write syscall, exits, and a direct access to a kernel-only page faults while the kernel survives; then user/hello.c built for aarch64 runs over svc with the Joshua Tree numbers (skips where the tools are missing)|python3 ./tools/checks/arm64-m3-check.py
retry|8|ARM64 M3c app loader: user/calendar.c (34 KB, relocated pointer tables, a data page, a zeroed pool) loads and runs at EL0 next to user/hello.c; its window call gets -ENODEV outside the scheduler and its own failure line, the probe finds the relocated pointer, a zero pool, writable data and TIME stored through (and refused into code), a store to its code and a branch into data, pool or stack each fault, a reload re-zeroes and re-relocates, TIME answers the fixed clock, `userrun calendar` opens its window and Esc closes it, `userrun hello` and the Terminal keep answering; M3d: the file calls refuse a kernel pointer, a read into kernel memory, a path with .. and huge lengths with errnos, and with the sd check's card as a virtio disk the unchanged hello.c reads JT/HELLO.TXT to exit 9 (skips where the tools are missing)|python3 ./tools/checks/arm64-m3c-check.py
retry|8|Apps from the card (6.51.0): JT/APPS/HI.APP runs at EL0 with `userrun hi`, and tools/mkapp.sh builds a C file from outside the tree into SUN.APP that runs too (6.52.0), and Spotlight lists and opens it while TERMINAL.APP and CALENDAR.APP are refused their taken names (6.53.0); a bad relocation, one that rewrites the next, an image over 256 KiB and a bad name are refused, and the kernel carries on (skips where the tools are missing)|python3 ./tools/checks/arm64-cardapp-check.py
retry|8|Signed apps (6.54.0): with a throwaway key built in and two accounts on the card, a signed app runs for a non-admin, an unsigned one only under an admin's sudo (and then in Spotlight), and one with a flipped byte, another key's signature or a copied name never, sudo or not (skips where the tools are missing)|python3 ./tools/checks/arm64-appsig-check.py
retry|8|The app store (6.55.0): with a throwaway key and a stub store, install fetches a signed app into JT/APPS (byte for byte) and userrun runs it; a changed image, a missing signature, an app the store lacks and a built-in name are refused and nothing is written (skips where the tools are missing)|python3 ./tools/checks/arm64-store-check.py
retry|10|ARM64 M3c Calendar window: the Calendar tile runs user/calendar.c at EL0 in its own window titled Calendar, March 2026 drawn with today in its accent (caltest clock), Right steps the month, Esc closes it and the wallpaper and the Terminal come back; a test program that yields forever leaves Spotlight and the Terminal answering and its red dot ends it (skips where the tools are missing)|python3 ./tools/checks/arm64-calendar-check.py
retry|6|ARM64 Weather: the Weather tile and Spotlight open the Pi window; the temperature, a sun glyph and five day cards are drawn from the labelled sample (no reading on the card), Esc and the red dot close it (skips where the tools are missing)|python3 ./tools/checks/arm64-weather-check.py
retry|7|ARM64 M4 Wi-Fi proto: wifi_proto.h packs and parses SDPCM, BCDC, escan and NVRAM on the host clang|sh ./tools/checks/wifi-host-check.sh
retry|7|ARM64 Wi-Fi WPA2: wpa.h SHA-1, HMAC, the pairwise-key PRF and AES key unwrap match the RFC vectors|cc -O1 -o "${TMPDIR:-/tmp}/wpa-test" tools/checks/wpa-test.c && "${TMPDIR:-/tmp}/wpa-test"
retry|10|ARM64 M4 Wi-Fi: the Pi image powers the chip, finds no SDIO card under QEMU, prints wifi FAIL cmd5 and the boot carries on, with and without the firmware files (skips where the tools are missing)|python3 ./tools/checks/arm64-wifi-check.py
retry|10|ARM64 Bluetooth BT1 and BT2: HCI over a second UART against a fake controller and mouse: the patch records in file order, version, baud, LE Meta on, a scan listing the mouse by name, then bt pair with Just Works checked by the check's own AES (c1, s1), encryption, ACL flow control, GATT discovery and four boot reports moving the pointer onto a dock tile and clicking it; a wrong confirm, a silent or out-of-order radio each end in one plain line with the kernel up, a saved pairing reconnects on its own at boot (after a first try nobody answers) and again after the link drops, with bt status, and no key reaches the UART (skips where the tools are missing)|python3 ./tools/checks/arm64-bt-check.py
once |7|Bluetooth saved pairing: btpair.h builds on the host and round-trips; bad magic, version, truncation, damage and a zero key are refused; no key in any printed string (host only)|python3 ./tools/checks/bt-pair-check.py
retry|10|ARM64 M4 SD card: the Pi image reads and writes a FAT32 SDHC card through EMMC2 at 0xFE340000 under QEMU: ls and cat through FAT32 chains with reads past the partition and the card refused; write, rm and mkdir in JT/ survive a reboot on the same card, config.txt, kernel8.img and the other boot files are refused and hash the same, a CMD25 lands, and mtools, a FAT pass and fsck.fat find the card clean; with no card it prints sd no card and boots on (skips where the tools are missing)|python3 ./tools/checks/arm64-sd-check.py
retry|6|ARM64 Finder on the Pi: on QEMU virt with the check card as a virtio disk holding folders, a document and a song under JT/, Spotlight opens user/finder.c at EL0, it lists JT/ (folders first, accent folder glyphs, the selected row shaded), Enter goes into a folder and a subfolder, Backspace comes back up, Enter on a .TXT previews it read-only with its text on screen, Left and Down move through the sidebar, Esc closes it; with no card the Burrow entry opens it on the built-in apps and says so (skips where the tools are missing)|python3 ./tools/checks/arm64-finder-check.py
retry|10|ARM64 M3d Notes on the Pi: on QEMU's raspi4b with the sd check's FAT32 card, made-up USB key reports walk the dock to Notes, the unchanged user/notes.c opens at EL0, a typed line is saved by Esc through open, write and close to JT/NOTES/NOTES/N0000001.TXT (mtools reads exactly the line, a FAT pass and fsck.fat find the card clean), and after a reboot on the same card Notes lists it pixel for pixel and opens it in the editor (skips where the tools are missing)|python3 ./tools/checks/arm64-notes-check.py
retry|9|ARM64 Reminders on the Pi: on QEMU's raspi4b with the sd check's FAT32 card, made-up USB key reports walk the dock to Reminders (no longer the Console's "not on ARM yet"), user/remindpi.c opens at EL0, two adds and a tick land in JT/REMINDER.TXT in i386's line format with the done item sunk (mtools reads it, the card is clean), the terracotta done circle is on screen, after a reboot on the same card the item is still there and ticked, and with no card the window says memory only (skips where the tools are missing)|python3 ./tools/checks/arm64-reminders-check.py
retry|10|ARM64 M3e Music on the Pi: on QEMU virt with the check card as a virtio disk holding JT/MUSIC/ALPHA.WAV and CHARLIE.MP3, Spotlight opens the unchanged user/music.c at EL0 and it lists both, Space plays the WAV with its clock following the frames the kernel fed and its ring sums matching the file exactly, n decodes the MP3 at EL0 with the FPU, F11 lowers the kernel volume and the WAV plays again with lower sums, Esc closes it and the Terminal answers; fpkeep keeps q0-q31, FPCR and FPSR across 40 parks and calls while the kernel wipes the FPU; on the host the Pi build's PWM1 DMA ring takes every queued byte in order (skips where the tools are missing; the real jack is unverified)|python3 ./tools/checks/arm64-music-check.py
retry|6|ARM64 mascot on the empty Music and Books screens: with no songs and no books, each app's window carries the mascot's ink and terracotta above a plain caption in a screendump (skips where the tools are missing)|python3 ./tools/checks/arm64-mascot-empty-check.py
retry|9|ARM64 M4 USB: the aarch64 kernel finds an xHCI controller behind a PCIe root port, enumerates a hub, a keyboard behind it and a mouse, and reads key presses, moves and clicks (skips where the tools are missing)|python3 ./tools/checks/arm64-usb-check.py
retry|10|ARM64 mouse: a USB mouse behind a hub moves the shared arrow over the ARM desktop, the dock label follows it and leaves no ghost, the Console's close button and a dock click work, a hot-plugged mouse moves it, and the scale-2 arrow at 1080p (skips where the tools are missing)|python3 ./tools/checks/arm64-mouse-check.py
retry|8|ARM64 window drag: the Console and the Browser follow a press on their title band (not the dots or the well) with their contents, leave flat wallpaper and the dock untouched, clamp whole between the menu bar and the dock, and the red dot still closes the moved window; the boot-time drag at 1080p (scale 2) (skips where the tools are missing)|python3 ./tools/checks/arm64-drag-check.py
retry|6|ARM64 app windows at 1080p: Calendar, Notes, Music and Books each open at boot on the Pi image at scale 2, their frame is within 15% of the Console's size and a screen fraction and their content is not blank (skips where the tools are missing)|python3 ./tools/checks/arm64-appsize-check.py
retry|10|ARM64 4.0 gate: Calendar, Toroid and Notes run at once on the Pi image, Toroid crashes on purpose, the kernel survives, its slot is freed and the other two keep presenting frames (skips where the tools are missing)|python3 ./tools/checks/arm64-windows-check.py
retry|6|ARM64 three apps opened at boot on QEMU virt: the boot self-tests stand aside instead of loading over them, no kernel crash|python3 tools/checks/arm64-appboot-check.py
retry|6|ARM64 watchdog: a frozen app is stopped after 3 s and the other apps keep running|python3 tools/checks/arm64-watchdog-check.py
retry|8|ARM64 windows are sharp at 2x: Calendar, Notes, Music and Books draw at device size at 1080p (hi-res buffer, Tree Sans text through JT_SYS_TEXT2), and with the kernel-drawn Calculator, Clock, Brick, Browser, Terminal and Console every narrow text stem in a screendump has at most two soft pixels and two or more full ones, where a 1x picture smoothed up has three or four soft (skips where the tools are missing)|python3 ./tools/checks/arm64-sharp2x-check.py
retry|9|ARM64 window drag and resize, every window: Console, Browser, Clock, Brick, Calculator and the ring-3 apps Calendar, Notes, Music and Books are dragged by the title band (dots, flat wallpaper behind, held under the menu bar), resized by the bottom-right corner and the right and bottom edges (12 px zone, grows by the pointer's move, minimum size, stops at the screen's edge and above the dock), lay their content out again, apps get JT_EV_RESIZE and answer with a second window open and a redrawn frame, and a Calendar dragged off a Terminal leaves it drawn whole (skips where the tools are missing)|python3 ./tools/checks/arm64-resize-check.py
retry|9|ARM64 Mail: Spotlight opens user/mailpi.c at EL0 in its own window on sample messages, Down moves the selection and repaints the list, Enter fills the reading pane and clears the unread dot, Right changes mailbox, Esc closes (skips where the tools are missing)|python3 ./tools/checks/arm64-mail-check.py
retry|10|ARM64 close glitch at 1080p: after the Terminal, Calendar, Notes, Browser, Brick and Calculator each open and close (red dot, Esc, dragged first, dragged to the bottom, and the Terminal and Calendar stacked both ways) the screen is pixel for pixel the empty desktop except the arrow, and a plain 800x600 boot has no white sliver by the tribute line (skips where the tools are missing)|python3 ./tools/checks/arm64-closeglitch-check.py
retry|10|ARM64 back buffer at 1080p: with Calendar, Toroid and Notes open, one pointer report drags Calendar and the screen changes only inside the union of its old and new rectangles, in one copy whose fb: flush line is that union, Calendar's pixels move whole, Toroid behind comes back where it was uncovered, then Toroid crashes on purpose and Calendar and Notes still answer clicks (skips where the tools are missing)|python3 ./tools/checks/arm64-dirtyrect-check.py
retry|6|ARM64 Browser flicker: the BR_TRACE build hashes the screen's window rectangle before a Browser paint and again just before the copy, and every paint (open, load, Tab, scroll, address typing) must log offscreen with the screen unchanged, never paint direct (skips where the tools are missing)|python3 ./tools/checks/arm64-browser-flicker-check.py
retry|6|ARM64 Browser pointer motion: 120 pointer reports across a long (hundreds of lines) Browser page on the BR_TRACE build must cause zero Browser paints and zero page layouts, leave the screen pixel for pixel as it was, and a page down must paint once without laying out again (skips where the tools are missing)|python3 ./tools/checks/arm64-browser-mouse-check.py
retry|9|ARM64 focus: with the Terminal and the Calculator both open, a click focuses one and typed keys land only in it (the sum in the Calculator, digits on the Terminal's prompt row), the clicked window comes to the front, and closing the Calculator puts the wallpaper back (skips where the tools are missing)|python3 ./tools/checks/arm64-focus-check.py
retry|9|ARM64 clipboard and Cmd keys: in Notes Cmd+A selects, Cmd+C copies into the one 4 KB kernel clipboard (SYS_CLIPBOARD 399); the Terminal's Cmd+V pastes it and Enter runs it, Cmd+X cuts a line, and Cmd+W closes the Terminal and then Notes (headless QEMU virt with a card, skips where the tools are missing)|python3 ./tools/checks/arm64-clipboard-check.py
retry|8|ARM64 Launchpad: the Apps tile opens a centred cream card, seven across, of all 29 apps Spotlight lists with icon art and names, the one with no ARM app (Trash) greyed, no Calculator; arrows (stopping at the edges), Enter, Esc and an outside click work and restore the pixels under the card, a click on the Calculator cell opens it, Enter on Calendar starts it, and every name is found by Spotlight (skips where the tools are missing)|python3 ./tools/checks/arm64-launchpad-check.py
retry|6|ARM64 Calculator: calc.c gets a table of 42 sums right on the host (sin(30deg)=0.5, 5!=120, 2^10=1024, 1/0 is an error), its keypad's memory and Deg keys work, and the calctest build types four sums at boot with the same answers on the UART and the window in a screendump (QEMU part skips where the tools are missing)|python3 ./tools/checks/arm64-calc-check.py
retry|9|ARM64 keyboard only: with no mouse, Cmd+Space and F1 open Spotlight and a bare Space does not (drawn in the house colours), typing term and Enter opens the Terminal with the keyboard, Esc hands the keys back, Left and Right move the dock's label and Enter opens that tile (Calendar, which Esc closes), F2 opens the Terminal at once, a held key repeats 8 to 14 times and stops on release, Caps Lock capitalises letters and Shift flips it, and a build with the repeat disabled fails the check (skips where the tools are missing)|python3 ./tools/checks/arm64-keys-check.py
retry|8|ARM64 Mac-style USB keyboard: over xHCI, Cmd+Space opens Spotlight, a plain letter on the bare desktop opens it with the letter typed, Esc with it closed does nothing, a 9-byte report with a report ID still gives Ctrl+T, and the key debug line shows only in the dev build (skips where the tools are missing)|python3 ./tools/checks/arm64-keydbg-check.py
retry|9|ARM64 Calendar month/day digits, December rollover, Clock time/date and unset state, Spotlight open/close and Samantha launch (skips where the tools are missing)|python3 ./tools/checks/arm64-calicon-check.py
retry|10|ARM64 crash screen: an unexpected EL1 fault prints class, ESR, FAR, ELR and the last console lines on the UART, draws them as a panel (QEMU screendump on virt and the Pi 4B model) and halts quietly (skips where the tools are missing)|python3 ./tools/checks/arm64-crash-check.py
retry|8|ARM64 FP state: q0-q31, FPCR and FPSR survive timer interrupts whose handler wipes them, and the build without the save fails the same test (skips where the tools are missing)|python3 ./tools/checks/arm64-fp-check.py
retry|9|ARM64 out of memory: a full heap prints oom fb or oom text and the kernel carries on (no screen, or the VGA fallback font) on virt and the Pi 4B model (skips where the tools are missing)|python3 ./tools/checks/arm64-oom-check.py
retry|8|ARM64 boot health: the Pi image boots on QEMU's raspi4b with no FAIL line beyond the listed expected ones, no oom, no crash, and the desktop up (skips where the tools are missing)|python3 ./tools/checks/arm64-boot-health-check.py
retry|8|ARM64 quiet boot: the Pi image's boot log window is never drawn, no dock label plays by itself (raspi4b at 1080p)|python3 ./tools/checks/arm64-boot-quiet-check.py
retry|9|ARM64 quiet boot and Wi-Fi fan: no Console window at boot (flat wallpaper where it was), a dock click opens it with the buffered log, the fan is dimmed grey when not joined and flat ink when joined, never blue|python3 ./tools/checks/arm64-quietboot-check.py
retry|8|ARM64 boot splash: neutral grey ground, no terracotta pixel, the tree mark crisp in ink with strong edge contrast, a grey bar|python3 ./tools/checks/arm64-splash-check.py
once |0|Pi card flasher: kernel, firmware and tools/pi-config.txt land on a stand-in card, and what was there is kept as .bak (skips where the tools are missing)|bash ./tools/checks/flash-pi-check.sh
once |7|ARM64 RNG200 full seeds, health faults, bounded waits and wiped failures (host harness)|python3 ./tools/checks/arm64-rng-check.py
once |5|ARM64 clock accepts bounded HTTPS dates only, never before the build or an accepted clock (host harness)|python3 ./tools/checks/arm64-clock-check.py
retry|10|ARM64 Terminal asks the real relay over TLS, with session resume and wrong-token refusal|python3 ./tools/checks/arm64-claude-console-check.py --tls
retry|9|ARM64 relay discovery: with a dead baked relay address the Pi finds the relay by a LAN broadcast probe, verifies its jt-relay certificate from the private CA, and refuses a right-CA certificate for the wrong name before the token is sent (skips where the tools are missing)|python3 ./tools/checks/arm64-relay-discovery-check.py
retry|9|ARM64 sam speed: sums answered on the Pi with no relay traffic, the relay remembered in memory and on the card (0 UDP probes after the first), a stalled relay stops at 30 seconds with the relay address, the timing line prints (skips where the tools are missing)|python3 ./tools/checks/arm64-samfast-check.py
retry|6|ARM64 sam chat view: bare `sam` opens a full-window Samantha in the Terminal with a header, scrolling history and `you: ` input row; a sum answers on the Pi, a question shows a thinking marker and her reply from the stub relay, Up/Page/Home/End scroll, and Esc or /quit return to a working zsh prompt with the busy flag clear (skips where the tools are missing)|python3 ./tools/checks/arm64-samtui-check.py
retry|10|ARM64 USB audio: a UAC1 speaker behind xHCI is configured and streamed 440 Hz over isochronous transfers, and the tone arrives intact|python3 tools/checks/arm64-usbaudio-check.py
retry|9|ARM64 voice mode: double-tap and hold Space listens with a mic mark in the menu bar and a live level bar, the release posts the samples and the words reach sam, the taps leave no spaces, a quick double tap stays two spaces|python3 tools/checks/arm64-voicemode-check.py
retry|8|ARM64 voice and face: [[say TEXT]] fetches Samantha's voice from the relay's /api/tts with the token and queues every sample into the Pi's sound ring, the chat view draws the mascot's face and its mouth moves with the clip, and a 413 leaves the caption with "voice: no audio"|python3 tools/checks/arm64-voice-check.py
EOF
}

# A wedged QEMU (hung boot, a script blocked forever on a socket that will
# never answer) must not eat the job's whole 20-minute budget and take
# every check after it down with a timeout instead of a result. Each check
# gets its own ceiling, so a hang is one named FAIL and the suite moves on.
PER_CHECK_TIMEOUT=${PER_CHECK_TIMEOUT:-600}
if command -v timeout >/dev/null 2>&1; then
    TIMEOUT_BIN=timeout
elif command -v gtimeout >/dev/null 2>&1; then
    TIMEOUT_BIN=gtimeout          # macOS with coreutils installed
else
    TIMEOUT_BIN=""                # macOS without it: no ceiling, results identical
fi
RUN_ONE() {
    if [ -n "$TIMEOUT_BIN" ]; then
        "$TIMEOUT_BIN" --signal=KILL "$PER_CHECK_TIMEOUT" bash -c "$1"
    else
        bash -c "$1"
    fi
}

pass=0; fail=0; flaky=0
summary=""
failed_names=""

while IFS='|' read -r mode shard name command; do
    [ -z "${name:-}" ] && continue
    mode=$(echo "$mode" | tr -d ' ')

    echo "::group::$name"
    start=$(date +%s)
    RUN_ONE "$command"
    status=$?

    if [ $status -ne 0 ] && [ "$mode" = "retry" ]; then
        echo "--- $name failed (exit $status), one automatic retry (timing-sensitive check) ---"
        RUN_ONE "$command"
        retry_status=$?
        if [ $retry_status -eq 0 ]; then
            elapsed=$(( $(date +%s) - start ))
            echo "::endgroup::"
            echo "FLAKY  $name (failed once, passed on retry, ${elapsed}s)"
            summary="${summary}FLAKY  ${name}"$'\n'
            flaky=$((flaky + 1)); pass=$((pass + 1))
            continue
        fi
        status=$retry_status
    fi

    elapsed=$(( $(date +%s) - start ))
    echo "::endgroup::"
    if [ $status -eq 0 ]; then
        echo "PASS   $name (${elapsed}s)"
        summary="${summary}PASS   ${name}"$'\n'
        pass=$((pass + 1))
    else
        echo "::error title=$name::check failed with exit $status"
        echo "FAIL   $name (exit $status, ${elapsed}s)"
        summary="${summary}FAIL   ${name} (exit ${status})"$'\n'
        failed_names="${failed_names}  - ${name}"$'\n'
        fail=$((fail + 1))
    fi
done < <(manifest | grep -v '^[[:space:]]*$' | awk -v i="${SHARD:-}" -F'|' 'i == "" || $2 == i')

echo
echo "================ regression suite summary ================"
printf '%s' "$summary"
echo "=========================================================="
echo "$pass passed, $fail failed, $flaky needed their one retry"

# The summary is also written to the job summary panel, so the full picture
# is readable without scrolling a 40-check log.
if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    {
        echo "### Regression suite"
        echo
        echo "\`$pass passed, $fail failed, $flaky flaky\`"
        echo
        echo '```'
        printf '%s' "$summary"
        echo '```'
    } >> "$GITHUB_STEP_SUMMARY"
fi

if [ $fail -gt 0 ]; then
    echo
    echo "FAIL: $fail check(s) failed:"
    printf '%s' "$failed_names"
    exit 1
fi

echo "PASS: every check in the suite passed"
