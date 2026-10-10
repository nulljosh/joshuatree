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
export TMPDIR=$(mktemp -d "${TMPDIR:-/tmp}/jt-suite-XXXXXX")
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
# of each other instead of one shard alone gating the job. 8 shards since
# 1.6.10, about 300s each (was 4 at about 600s): GitHub gives a public repo
# the extra runners for free, so the wall time roughly halves. Re-run the packing (any LPT/bin
# packing script works) if the suite's shape changes enough to matter --
# a new slow check, or several checks added/removed.
cd "$(dirname "$0")/../.."

# ARM build checks share shard 6: local shards share arch/arm64 and each check cleans it.
# retry?  shard  name                                                   command
manifest() {
cat <<'EOF'
once |6|Pi keyboard shutdown and restart: confirmed HID chords halt or reboot raspi4b, Escape cancels, busy and hidden prompts refuse (host and QEMU)|python3 ./tools/checks/arm64-power-check.py
once |7|Dock slot constants agree with kernel.c (static drift guard)|python3 ./tools/checks/dockslots-check.py
once |7|No check hard-codes a fixed temp path or socket, so two suites cannot corrupt each other (static, baseline only shrinks)|python3 ./tools/checks/tmp-paths-check.py
once |5|docs/DESIGN.md states only what the source says: icon shape and light, fonts, colours, dock and window numbers, caption timings (static)|python3 ./tools/checks/design-doc-check.py
once |7|Kernel memory keeps 16KB clear of the program window (toolchain drift guard)|python3 ./tools/checks/bss-margin-check.py
once |6|Samantha's face loops wrap without a seam|python3 ./tools/checks/face-frames-check.py
once |7|docs/TESTING.md lists every check in this suite|./tools/checks/testing-doc-check.sh
once |7|The landing logo rebuilds byte for byte from tools/gen/logo.py|./tools/checks/logo-check.sh
once |7|Landing app tiles are not older than the app code that draws them (source hashes, no QEMU)|python3 ./tools/checks/landing-shots-fresh-check.py
once |7|The landing demo downloads the kernel once, gzipped|./tools/checks/kernel-gz-check.sh
retry|5|Boot check|./check.sh
retry|2|DHCP client leases real SLIRP config, DNS+HTTP still work, nodhcp keeps the old fixed path|./tools/checks/dhcp-check.sh
retry|7|Benchmarks run and report every number|./tools/checks/bench-check.sh
retry|2|ISO boot (CD-ROM and USB/raw-disk paths)|./tools/checks/iso-boot-check.sh
once |6|PNG decoder, host harness|./tools/checks/png-host-check.sh
once |6|Vendored BearSSL TLS 1.2 client subset compiles for aarch64 freestanding (static)|python3 ./tools/checks/bearssl-tls-check.py
once |3|Embedded apps are stored compressed: every one inflates to its exact binary, damaged copies are refused (host harness)|python3 ./tools/checks/user-compress-check.py
once |0|Hamurabi game rules: SplitMix64, golden checksum, invariants, i386-freestanding|./tools/checks/hamurabi-rules-check.sh
once |7|Hamurabi sprite sheet header is current with art/hamurabi/sprites.png and sprites.json (static)|python3 ./tools/gen/gen_hamurabi_sprites.py --check
once |1|Burrow: Files renamed, Samantha still opens it as files / file browser, icon art is the new fox|python3 ./tools/checks/burrow-rename-check.py
retry|0|Burrow icon view wraps a long file name onto a second line instead of cutting it|python3 ./tools/checks/burrow-labels-check.py
retry|6|Mail, Notes and Weather read past byte 255 of their data file (SYS_READ moves 255 bytes a call)|python3 ./tools/checks/read-long-files-check.py
retry|7|Clock opens and its countdown timer updates live|python3 ./tools/checks/clock-check.py
once |4|TTF rasterizer, host harness|./tools/checks/ttf-host-check.sh
once |7|libjt string/stdlib, host harness|./tools/checks/libjt-host-check.sh
once |2|God-file guard (no hand-written .c/.h over its line ceiling)|./tools/checks/godfile-check.sh
retry|5|PNG decoder, in-kernel|./tools/checks/png-check.sh
retry|1|AA text spacing, in-kernel|./tools/checks/textspacing-check.sh
retry|3|AA text stems are dense but still antialiased|python3 ./tools/checks/textsharp-check.py
retry|2|FAT filesystem cycle hang (regression test)|./tools/checks/fatcyclehang-check.sh
retry|5|File robustness: empty, oversized, corrupt-FAT and full-disk cases|python3 ./tools/checks/filerobust-check.py
retry|1|Chat defaults to Samantha (Turing) and surfaces an HTTPS-redirect host clearly|python3 ./tools/checks/chat-samantha-check.py
retry|2|GUI Chat app asks Samantha and renders the reply on screen|python3 ./tools/checks/chatapp-check.py
retry|1|Chat's tools (reminder, note, open app) work locally via /api/pick, ordinary questions still reach Samantha|python3 ./tools/checks/chattools-check.py
retry|4|Chat mail tools: "read my email" lists a real message, "email <someone> <text>" lands one in Mail|python3 ./tools/checks/mailtools-check.py
retry|5|Chat notes/reminders tools: list_reminders and read_notes work via the local keyword fallback when the picker doesn't know them|python3 ./tools/checks/notestools-check.py
retry|4|"samantha" boot flag opens Chat's full-screen avatar view, input focused, before the desktop|python3 ./tools/checks/samantha-boot-check.py
retry|3|"phone" boot flag opens a real 430x932 portrait frame straight into Samantha's view|python3 ./tools/checks/phone-boot-check.py
retry|7|Phone Samantha back chevron exits her view and F2/Esc hints are hidden on phones|python3 ./tools/checks/phone-samantha-back-check.py
retry|6|Samantha is full screen: room around her (her wall in the side bands, eyes 40 percent down), glass input, fading captions that stay off her lips and eyes, scrollback, the typing bugs, a mouth drawn from her voice, Esc and the red dot|python3 ./tools/checks/samantha-fullscreen-check.py
retry|2|Touch: a tap opens the on-screen keyboard on phone and a tapped key reaches the Notes editor|python3 ./tools/checks/touch-osk-check.py
retry|5|No-disk boot falls back to ramfs with seeded demo files|./tools/checks/ramfs-demo-check.sh
retry|2|Shell regression suite (heap, task, preempt, kill, ring3, ps)|./tools/checks/shellregress-check.sh
retry|6|Ring-3 reference program against the v1 syscall ABI|./tools/checks/usertest-check.sh
retry|3|Ring-3 program writing a real file against the v2 syscall ABI|./tools/checks/notetest-check.sh
retry|4|Keyrate runs as a ring-3 process with its own window; crashing it leaves the desktop alive (v3 syscall ABI)|python3 ./tools/checks/ring3app-check.py
retry|1|Toroid runs as a ring-3 process through the table-driven launcher: draws generations, closes both ways, crashes safely, desktop alive|python3 ./tools/checks/ring3toroid-check.py
retry|2|Calculator runs as a ring-3 process through the table-driven launcher: evaluates 12*3=36 and 5/0=0 through the real parser, closes both ways, crashes safely, desktop alive|python3 ./tools/checks/ring3calc-check.py
retry|3|Quotes runs as a ring-3 process through the table-driven launcher: draws the option grid, answers right and wrong through the real logic, closes both ways, crashes safely, desktop alive|python3 ./tools/checks/ring3quotes-check.py
retry|1|Bookrank runs as a ring-3 process through the table-driven launcher: draws the ranked list, moves the selection by keyboard and mouse through the real logic, closes both ways, crashes safely, desktop alive|python3 ./tools/checks/ring3bookrank-check.py
retry|7|Bookrank pulls the real shelf live (Worker text from a stub), scrolls it, bounds a hostile reply, and shows the ten samples for junk, a 500, an oversize body and no NIC|python3 ./tools/checks/ring3bookrank-live-check.py
retry|6|Tonchi runs as a ring-3 process through the table-driven launcher: draws the word and choices, answers right and wrong through the real drill, closes both ways, crashes safely, desktop alive|python3 ./tools/checks/ring3tonchi-check.py
retry|0|Tonchi lists the real courses live (Worker text from a stub), drills one with a score, bounds a hostile reply, and shows the Spanish deck for junk, a 500, an oversize body and no NIC|python3 ./tools/checks/ring3tonchi-live-check.py
retry|5|Fieldbook runs as a ring-3 process through the table-driven launcher: draws the ranked field list, moves the selection by keyboard and mouse through the real logic, closes both ways, crashes safely, desktop alive|python3 ./tools/checks/ring3fieldbook-check.py
retry|7|Clock runs as a ring-3 process through the table-driven launcher: draws the moving time, takes a timer through the real input logic, closes on Esc, desktop alive|python3 ./tools/checks/ring3clock-check.py
retry|1|Movies plays a real AVI with sound at ring 3, audio-led: frame within one of the audio clock, drift under 100 ms, pause holds frame and sound (checked in the wav and on the framebuffer), seek by bar and keys, fullscreen, a damaged clip / non-AVI / over-cap file each show an error, closes on Esc, desktop alive|python3 ./tools/checks/movie-check.py
retry|5|Hamurapi (Hamurabi in the sources) is playable as a ring-3 program: draws its real title scene (sprite pixels equal the sheet), plays a classic reign by keyboard, a story reign with cards and choices and the robot's demo, and every year it logs equals a host replay of the same rules header, closes on Esc, desktop alive|python3 ./tools/checks/ring3hamurabi-check.py
retry|4|Windgate runs as a ring-3 process through the table-driven launcher: the circle grows on the in-breath and shrinks on the out-breath (measured off the framebuffer), all four presets by key and click log the web app's exact phases and seconds, pause holds the circle and the clock, closes on Esc, desktop alive|python3 ./tools/checks/ring3windgate-check.py
once |5|AVI reader, host harness (real JPEG frames, bad headers, truncation, mutation fuzz under ASan/UBSan)|./tools/checks/avi-host-check.sh
retry|0|Portfolio runs as a ring-3 process through the table-driven launcher: draws the fleet catalog, moves the selection by keyboard and mouse through the real logic, closes on Esc, desktop alive|python3 ./tools/checks/ring3portfolio-check.py
retry|4|Activity runs as a ring-3 process through the table-driven launcher: draws the live task list, refreshes it on its own, has the kernel refuse to kill the shell, closes on Esc, desktop alive|python3 ./tools/checks/ring3activity-check.py
retry|4|Contacts runs as a ring-3 process through the table-driven launcher: draws the list, adds and deletes a person through the real prompt, keeps CONTACTS.TXT across fresh runs, closes on Esc, desktop alive|python3 ./tools/checks/ring3contacts-check.py
retry|7|Hikko runs as a ring-3 process through the table-driven launcher: draws the ranked list, moves the selection, re-sorts when an upvote passes the leader, closes on Esc, desktop alive|python3 ./tools/checks/ring3hikko-check.py
retry|1|Hikko shows the real forum ideas live (Worker text from a stub), scrolls them, keeps votes local and forum order for ties, bounds a hostile reply, and shows the ten demo ideas for junk, a 500, an oversize body and no NIC|python3 ./tools/checks/ring3hikko-live-check.py
retry|7|Reminders runs as a ring-3 process through the table-driven launcher: adds, ticks and deletes items through the real prompt, keeps REMINDERS.TXT across fresh runs, closes on Esc, desktop alive|python3 ./tools/checks/ring3reminders-check.py
retry|5|Curbfind runs as a ring-3 process through the table-driven launcher: falls back to the samples when SYS_HTTP_GET finds no NIC, selects by key and click, the kernel refuses every bad path and pointer the probe hands the syscall, closes on Esc, desktop alive|python3 ./tools/checks/ring3curbfind-check.py
retry|4|Calendar runs as a ring-3 process through the table-driven launcher: gets today from SYS_TIME, draws the month grid, saves an event through the real editor, keeps EVENTS.TXT across fresh runs, feeds Samantha's calendar_today, closes on Esc, desktop alive|python3 ./tools/checks/ring3calendar-check.py
retry|6|Search runs as a ring-3 process through the table-driven launcher: SYS_READDIR refuses a kernel pointer, an over-long path and a missing folder, the list filters live, a file shows its real bytes, a FAT folder opens by relative path with the kernel's cwd untouched, closes on Esc, crashes safely, desktop alive|python3 ./tools/checks/ring3search-check.py
retry|0|Epiphany runs as a ring-3 process through the table-driven launcher: falls back to the offline prices when SYS_HTTP_GET finds no NIC, switches tabs by key and click, runs the command bar, closes on Esc, desktop alive|python3 ./tools/checks/ring3epiphany-check.py
retry|3|Weather runs as a ring-3 process through the table-driven launcher: reads the kernel's WEATHER.TXT and shows the offline face over labelled sample data, R refetches once and restarts it, closes on Esc, desktop alive|python3 ./tools/checks/ring3weather-check.py
retry|4|Burrow runs as a ring-3 process through the table-driven launcher: draws the folder grid, Enter opens a folder and Backspace goes up through SYS_READDIR, closes on Esc, desktop alive|python3 ./tools/checks/ring3burrow-check.py
retry|3|Every ring-3 app (parsed from RING3_APPS) crashes on purpose, is reaped, and the desktop keeps drawing and opens a different app after each one|python3 ./tools/checks/ring3crash-all-check.py
retry|1|A released window framebuffer is supervisor-only again: store faults, pointer into it or into the kernel is -EFAULT|python3 ./tools/checks/userfb-release-check.py
retry|7|SYS_BRK: a program grows 3MB of zeroed heap pages, bad tops are -EINVAL, and its crash gives every frame back (brk live=0, pmm free unchanged), desktop alive|python3 ./tools/checks/ring3brk-check.py
retry|6|Shell launches a ring-3 program by bare name, case-insensitively|./tools/checks/shellname-check.sh
retry|4|QEMU vmmouse absolute-pointer round trip|./tools/checks/vmmouse-check.sh
once |7|Calendar date math, host harness|./tools/checks/check-calendar.sh
retry|0|Settings click acts on the row actually clicked|./tools/checks/settingsclick-check.sh
retry|6|Settings Location geocodes, persists, and fails clean|python3 ./tools/checks/location-check.py
retry|5|Wallpaper defaults to Satellite on a fresh boot|./tools/checks/walldefault-check.sh
retry|5|Idle tour's Settings visit doesn't change the wallpaper theme|python3 ./tools/checks/walldemo-regression-check.py
retry|0|Wallpaper compose: Map and Satellite fetch distinct, byte-correct buffers (hermetic, fake tile server)|python3 ./tools/checks/wallcompose-check.py
retry|4|Notes editor chrome doesn't redraw on plain keystrokes|./tools/checks/editorflash-check.sh
retry|5|System-wide clipboard: Ctrl+C/X/V round-trips real text within Notes, across Notes->Terminal, truncates a too-long paste cleanly|python3 ./tools/checks/clipboard-check.py
retry|0|Reminders prompt redraws content, not chrome, per keystroke|./tools/checks/gui-prompt-keystroke-check.sh
retry|3|Notes typing, typography, pointer controls, persistence|python3 ./tools/checks/editor_qa.py
retry|1|Notes' runtime-TTF text is real antialiased rasterization at 12pt and 200pt, not a duplicated-block bitmap upscale|python3 ./tools/checks/notessharp-check.py
retry|7|Notes folders: legacy NOTES.TXT migrates intact, a new note lands in the current folder, both survive reboot, delete asks first|python3 ./tools/checks/notesfolders-check.py
retry|2|Terminal grid draws the mono face at its true advance|python3 ./tools/checks/termmono-check.py
retry|6|Terminal's runtime-TTF text is real antialiased rasterization with a driftless monospace grid|python3 ./tools/checks/termsharp-check.py
retry|1|Panes: independent scrollback per tab, tab rail, activity dot on a background tab, close, split panes with their own shells|python3 ./tools/checks/panes-check.py
retry|7|Claude app: asks Claude Code through the relay (stub claude on the host), draws the reply, resumes the session, red error line on a wrong token, a stopped relay and no relay set (-EACCES at once)|python3 ./tools/checks/ring3claude-check.py
once |6|Claude relay: token refused and accepted, session resume, exact read-only argv, prompt on stdin, timeout kills the process group, size cap, one request at a time, log hygiene, and a mutant without the token check lets a tokenless request in|python3 ./tools/checks/claude-relay-check.py
once |6|Relay API mode: file tools refuse ../, absolute paths, dot files, symlinks and unlisted names and cap reads at 20000 characters; model routing; the prompt names both actions; the reply's M line names the model that answered (Claude Haiku 5.5, Sonnet, Opus) and ask.c's default prompt matches the relay's default model (no network)|python3 ./tools/checks/relay-api-check.py
retry|1|Apple-menu hover stays cheap, clock redraws on a minute change|./tools/checks/menuclock-check.sh
retry|6|Lock Screen: menu item locks, Esc cannot bypass, password unlocks|python3 ./tools/checks/lockscreen-check.py
retry|4|Multi-window chrome doesn't redraw on plain keystrokes|./tools/checks/mwkeyflash-check.sh
retry|6|Drawing lands offscreen, window_present puts it on screen|./tools/checks/backbuffer-check.sh
retry|5|Multi-window apps draw exactly one toolbar, not two|./tools/checks/mwdupetoolbar-check.sh
once |7|JPEG decoder, host harness|./tools/checks/jpeg-host-check.sh
once |0|Media decoders (WAV, MP3), host harness under ASan and UBSan|./tools/checks/media-host-check.sh
retry|7|Tasks keep their own x87 float state across switches|./tools/checks/fpu-check.sh
once |7|HTML entities decode to ASCII, host harness|./tools/checks/html-host-check.sh
retry|0|JPEG decoder, in-kernel|./tools/checks/jpeg-check.sh
once |7|PNG/JPEG decoder fuzz (ASan/UBSan, truncation+mutation+nasties)|./tools/checks/decoder-fuzz-check.sh
once |0|HTTP/JSON/FAT16 parser fuzz (ASan/UBSan, truncation+mutation+nasties)|./tools/checks/parser-fuzz-check.sh
once |7|Text-input bounds: json.c maxlen 0/1/truncation + auth const-time compare (ASan/UBSan)|./tools/checks/input-bounds-check.sh
once |1|Password hashing, host harness: PBKDF2-HMAC-SHA256 vectors (RFC 7914, RFC 6070 for SHA-256), legacy record upgrade, USERS.TXT parse|./tools/checks/auth-check.sh
retry|4|Dock apps open/close from the pointer alone|python3 ./tools/checks/appclose-check.py
retry|3|Esc on a bare desktop does not quit the GUI; Mail still opens from the dock|python3 ./tools/checks/esc-desktop-check.py
retry|1|Dock hover survives mid-animation|python3 ./tools/checks/dockhover-check.py
retry|1|Launchpad tile click launches, doesn't just close the folder|python3 ./tools/checks/launchpad-click-check.py
retry|5|Apps folder layout (no black band, no row spill, no ghost icons)|python3 ./tools/checks/appsfolder-layout-check.py
retry|7|Menu bar present after Launchpad, every app open and close, Esc, panels, drags; screen never black|python3 ./tools/checks/menubar-persist-check.py
retry|1|Launchpad centered, evenly padded, icons 56px or smaller, labels clear (pixels, five screen sizes)|python3 ./tools/checks/launchpad-centered-check.py
retry|3|Typography: baseline flatness, letter-gap variance, container padding|python3 ./tools/checks/baseline-check.py
retry|0|Multi-window (click-to-focus, real z-order compositing)|python3 ./tools/checks/multiwindow-check.py
retry|2|Ring-3 window: Reminders beside Notes, keys to the focused window only, a crash closes only its window|python3 ./tools/checks/ring3window-check.py
retry|3|Ring-3 stress: window task and desktop hammer heap and FAT at once, heap walk ok, file at root, desktop alive|python3 ./tools/checks/ring3stress-check.py
retry|0|Ring-3 PDE sync: kernel page table born after the window task is visible on its CR3, close clean, desktop alive|python3 ./tools/checks/pdesync-check.py
retry|2|Window snapping (title-bar drag to edge/corner, real pixel proof)|python3 ./tools/checks/windowsnap-check.py
retry|2|Ring-3 window resize: snap Notes to a quarter, JT_EV_RESIZE answered, new buffer mapped, app redrew at the quarter size (serial marker + far-corner pixels)|python3 ./tools/checks/ring3resize-check.py
retry|6|Windows drag live by their title bar (single-window Notes and multi-window Files)|python3 ./tools/checks/windowdrag-check.py
retry|7|Windowed apps start under the title bar, Calendar fits six weeks|python3 ./tools/checks/apptop-check.py
retry|5|Calendar Day, Week, Month and Year views (ring-3 program)|python3 ./tools/checks/calviews-check.py
retry|4|QA gallery: every app opens, screenshots, closes, no crash|python3 ./tools/checks/qa-gallery.py
retry|2|Every app's main action, headless|python3 ./tools/checks/feature-drive.py
retry|3|Dock icon edge quality (no staircased corners)|python3 ./tools/checks/iconedge-check.py
retry|4|Dock icon halo (clean clip to the tray, no glyph bleed)|python3 ./tools/checks/iconhalo-check.py
retry|5|Dock icon lighting (one soft top light, top highlight, no dark outline)|python3 ./tools/checks/iconlight-check.py
once |1|Every authored icon shares one tile silhouette, AA edges, glyph margin|python3 ./tools/checks/iconinset-check.py
retry|5|Calendar dock tile is a page-and-grid picture, no date or dash, same on every date|python3 ./tools/checks/calicon-check.py
retry|0|Clock icon is a live analog face: hands follow the RTC and redraw on the minute|python3 ./tools/checks/clockicon-check.py
retry|2|Shadow under the dock darkens the photo, no flat bands|python3 ./tools/checks/dockband-check.py
retry|2|Titlebar traffic-light AA (real coverage blend, not binary)|python3 ./tools/checks/titlebar-aa-check.py
retry|6|Dock tray corner AA (real coverage blend, not binary)|python3 ./tools/checks/traycorner-check.py
retry|6|Portfolio catalog opens, lists the fleet, and the list scrolls|python3 ./tools/checks/portfolio-check.py
retry|3|Asking Samantha for the weather fetches it, no "open Weather first"|python3 ./tools/checks/samweather-check.py
retry|1|Boot logo AA (no false interior seams at overlapping capsule joints)|python3 ./tools/checks/bootlogo-check.py
retry|4|Boot splash draws the real landing/logo.svg mark, not the old stick tree|python3 ./tools/checks/bootmark-check.py
once |2|Landing eyebrow tracks roadmap Latest, H1 stays the brand line|./tools/checks/landing-headline-check.sh
once |7|Idle tour still cycles all 8 real dock apps|node ./tools/checks/tourappcount-check.mjs
once |7|Idle tour autoplay fix is in place (tourArmed reset)|node ./tools/checks/idletour-arm-reset-check.mjs
once |7|RTC local-time shift math (v86's CMOS answers in UTC)|node ./tools/checks/rtc-timezone-check.mjs
retry|4|Stocks opens without a supported network card|python3 ./tools/checks/stocks-dock-check.py
retry|2|Stocks chart line is antialiased (coverage blend, no stair-stepping)|python3 ./tools/checks/stocks-aa-check.py
retry|4|Stocks fills all five watchlist rows from a full-size stub Worker reply (fails if the app reads only part of STOCKS.TXT)|python3 ./tools/checks/ring3stocks-list-check.py
once |3|Stocks live quotes and kernel parsing|node ./tools/checks/stocks-live-check.mjs
retry|0|Epiphany command bar: AAPL GP draws the chart, an unknown code errors cleanly|python3 ./tools/checks/epiphany-cmdbar-check.py
once |7|Worker /api/proxy allowlist|node ./tools/checks/worker-proxy-check.mjs
once |7|Worker /api/waitlist store, validate, count|node ./tools/checks/waitlist-check.mjs
once |7|Mail Send: Worker /api/mail/send guards and Resend shape, kernel bearer wiring|python3 ./tools/checks/mailsend-check.py
once |5|Worker /api/proxy: a silent upstream cannot hang the guest (weather/chat freeze regression)|node ./tools/checks/weatherproxy-hang-check.mjs
once |7|Worker /api/listen: Whisper transcription, 503 without the AI binding, rejects oversize/empty, per-IP rate limit|node ./tools/checks/listen-worker-check.mjs
retry|7|Every app opens and closes by keyboard alone|python3 ./tools/checks/keyboard-only-check.py
once |5|Soak: every app opened and closed once each in one boot, no leak, no crash|python3 ./tools/checks/soak-check.py 1
retry|6|Accounts: legacy record upgrades to PBKDF2 on login, create, reboot, login, wrong and empty passwords rejected, change password|python3 ./tools/checks/auth-flow-check.py
retry|3|Entropy pool: serial names its sources, two boots draw different salts|python3 ./tools/checks/entropy-check.py
once |5|Panic screen: a ring-0 fault paints the reason, not a frozen desktop|python3 ./tools/checks/panic-check.py
once |7|Crash report names the faulting function, not just the exception kind|python3 ./tools/checks/panic-symbols-check.py
once |2|Frame time: idle, dock hover and window open stay within budget|python3 ./tools/checks/frametime-check.py
once |4|Every check in tools/checks/ is in this manifest or says why not|./tools/checks/suite-coverage-check.sh
retry|5|Text selection in Notes: Shift-arrow/Ctrl+A highlight, edsel/edcopy/edcut markers, selection-aware copy/cut/paste/delete|python3 ./tools/checks/textselect-check.py
retry|7|Window top edge and corner arc are one continuous AA shape|python3 ./tools/checks/windowedge-check.py
retry|3|Sound Blaster 16 detects, beep plays a real 440Hz tone, card-less boot is a no-op|python3 ./tools/checks/sb16-check.py
retry|5|Music runs as a ring-3 app: plays a fixture WAV off a FAT disk (position from the driver, pause holds, bar and arrow seeks land, next changes the song, oversize file refused) and the sound card hears the tones|python3 ./tools/checks/music-check.py
retry|0|Sound Blaster 16 record path: `listen` reaches the driver and times out cleanly (QEMU has no ADC backend), card-less boot is a no-op|python3 ./tools/checks/sb16-record-check.py
retry|1|Chat speaks: say fetches /api/speak PCM from a stub and plays a real 1000Hz tone|python3 ./tools/checks/chat-speaks-check.py
retry|2|Chat face: idle frame before, talk frames while she speaks, idle after; no frames means no face|python3 ./tools/checks/chat-face-check.py
retry|6|HTTP stress: a ring-3 app makes 72 sequential SYS_HTTP_GET calls, all succeed (none -EIO), heap free stays at baseline|python3 ./tools/checks/httpstress-check.py
retry|6|Burrow view switcher: List/Icons choice is saved to BURROW.TXT and a fresh run reads it back|python3 ./tools/checks/filesview-check.py
retry|7|Demo canvas fills its frame, pixelated only at an exact 1:1 map|node ./tools/checks/democrisp-check.mjs
retry|1|Landing hero: a real portrait before any click (pixels, undistorted), default boot opens Samantha's face, Esc reaches the desktop, ?desktop opts out, phone and tablet hold; fails if the poster is removed|node ./tools/checks/hero-poster-check.mjs
once |2|Landing hero: Click or Tap to boot lifts the poster onto her face well before the 60 s fallback, even after the serial log is trimmed|node ./tools/checks/landing-poster-click-check.mjs
once |4|Landing page never overflows horizontally at phone widths|node ./tools/checks/mobile-overflow-check.mjs
once |0|Landing: benchmark labels clear, one app count, See the desktop link styled, sections visible on load|node ./tools/checks/landing-layout-check.mjs
once |2|Portfolio voice: on by default, mute button top right, remembered across a reload, absent outside portfolio|node ./tools/checks/portfolio-mute-check.mjs
retry|2|Portfolio tour starts even when his face frames are slow and the first Escape presses are lost: his chat exits and Epiphany opens|node ./tools/checks/portfolio-slowframes-check.mjs
once |0|Landing demo on a phone: the chat bar raises the keyboard, typed letters reach Samantha once each and Send is Enter|node ./tools/checks/mobile-type-check.mjs
once |0|Landing: Tech specs accordions hold every developer number collapsed, and the footer is a four-column directory of real links|node ./tools/checks/landing-specs-footer-check.mjs
once |7|Landing demo: no keyboard trap, one main landmark, Full screen top right on phones|node ./tools/checks/landing-demo-ui-check.mjs
once |5|Landing on a phone: exactly one visible "Message Samantha" input, focusable and typeable|node ./tools/checks/phone-one-input-check.mjs
retry|3|App switcher: Ctrl+Tab cycles open windows and focuses the highlighted one|python3 ./tools/checks/appswitcher-check.py
retry|6|Screenshot key: Ctrl+Shift+3 saves a real framebuffer BMP, numbered and visible in Files|python3 ./tools/checks/screenshot-check.py
retry|1|Drunk mode easter egg: horizontal sway applied to framebuffer rows|python3 ./tools/checks/drunk-mode-check.py
retry|6|ARM64: the aarch64 kernel boots under QEMU and prints over the UART (skips where the tools are missing)|python3 ./tools/checks/arm64-m0-check.py
retry|6|ARM64: a tiny local language model answers under QEMU (skips where the tools are missing)|python3 ./tools/checks/arm64-llm-check.py
retry|6|ARM64 M1c: the aarch64 kernel draws a desktop into a ramfb framebuffer and QEMU screendump shows it (skips where the tools are missing)|python3 ./tools/checks/arm64-m1c-check.py
retry|6|ARM64 M2: the aarch64 kernel drives virtio disk, network, keyboard and mouse: a sector read back, a real ARP answer, key presses, moves and clicks (skips where the tools are missing)|python3 ./tools/checks/arm64-m2-check.py
retry|6|ARM64 console scrollback: Page Up, End and Home scroll the on-screen Console over the whole boot log, the title bar says which lines (skips where the tools are missing)|python3 ./tools/checks/arm64-console-scroll-check.py
retry|6|ARM64 net: the shared IP stack (drivers/net.c, drivers/http.c) on virtio-net leases 10.0.2.15 by DHCP and POSTs to a host server, 200 and the exact reply length (skips where the tools are missing)|python3 ./tools/checks/arm64-net-check.py
retry|6|ARM64 HTTPS: a TLS 1.2 page from a host server with a throwaway certificate built in as a trust anchor, refused (tls FAIL 62) when it is left out (skips where the tools are missing)|python3 ./tools/checks/arm64-tls-check.py
retry|6|ARM64 browser: browse URL typed at the Terminal's ask> row reads an HTTPS page through a redirect, prints readable text with numbered links, open N follows one, a redirect loop stops (skips where the tools are missing)|python3 ./tools/checks/arm64-browser-check.py
retry|6|ARM64 browser, dead server: a host that accepts and never answers, and a closed port, each end in one timeout line (tls, reply, connect) within 12 s with dns ok and tcp ok printed first, and the prompt still answers (skips where the tools are missing)|python3 ./tools/checks/arm64-browser-deadserver-check.py
once |6|ARM64 green light: the Pi's activity LED is blinked through the firmware mailbox (pin 42), never at boot, on a wait that ends (static, no hardware)|python3 tools/checks/arm64-led-check.py
once |7|Pi actions: an answer's [[note]], [[say]], [[led blink]], [[open]], [[browse]] (http/https only), [[calc]] and [[status]] lines are stripped and recorded by ask.c's real parser (compiled on the host), four per turn, unknown ones recorded to ignore, the rest stays as text, and the relay prompt names them all|python3 tools/checks/pi-actions-check.py
once |6|Pi release image carries no relay token: pi-ask.o built without JT_WIFI_DEV=1 has none, the dev build does (host only)|sh tools/checks/pi-release-notoken-check.sh
retry|6|ARM64 agent loop: against a fake relay, [[browse]] then a final answer with agent: step 1 and 2 logged, 4 actions per turn and 5 steps then step limit, Esc stops it, unknown actions logged and ignored (skips where the tools are missing)|python3 ./tools/checks/arm64-agent-check.py
retry|6|ARM64 /model and /effort: two F3 sessions preserve separate settings, conversation and typing, block switching while busy; slash commands run on the Pi, model and effort reach a fake relay only when not default, a bad value is refused in one line and never sent, /status /help /clear (skips where the tools are missing)|python3 ./tools/checks/arm64-model-check.py
retry|6|ARM64 Claude in the Terminal: the Console takes no input; typed at the Terminal's ask> row, a question reaches the real relay (stub claude, 127.0.0.1) over virtio-net and the answer prints in 53-column lines; wrong token -401, no network and no token each say so (skips where the tools are missing)|python3 ./tools/checks/arm64-claude-console-check.py
retry|6|ARM64 relay token gate: a 24-byte throwaway token is absent from kernel8.img with JT_WIFI_DEV unset and present with it set (skips where the tools are missing)|python3 ./tools/checks/arm64-token-gate-check.py
retry|6|ARM64 M3a: an unprivileged EL0 program prints through a write syscall, exits, and a direct access to a kernel-only page faults while the kernel survives (skips where the tools are missing)|python3 ./tools/checks/arm64-m3-check.py
retry|6|ARM64 M4 Wi-Fi proto: wifi_proto.h packs and parses SDPCM, BCDC, escan and NVRAM on the host clang|sh ./tools/checks/wifi-host-check.sh
retry|7|ARM64 Wi-Fi WPA2: wpa.h SHA-1, HMAC, the pairwise-key PRF and AES key unwrap match the RFC vectors|cc -O1 -o "${TMPDIR:-/tmp}/wpa-test" tools/checks/wpa-test.c && "${TMPDIR:-/tmp}/wpa-test"
retry|6|ARM64 M4 Wi-Fi: the Pi image powers the chip, finds no SDIO card under QEMU, prints wifi FAIL cmd5 and the boot carries on, with and without the firmware files (skips where the tools are missing)|python3 ./tools/checks/arm64-wifi-check.py
retry|6|ARM64 M4 USB: the aarch64 kernel finds an xHCI controller behind a PCIe root port, enumerates a hub, a keyboard behind it and a mouse, and reads key presses, moves and clicks (skips where the tools are missing)|python3 ./tools/checks/arm64-usb-check.py
retry|6|ARM64 mouse: a USB mouse behind a hub moves the shared arrow over the ARM desktop, the dock label follows it and leaves no ghost, the Console's close button and a dock click work, a hot-plugged mouse moves it, and the scale-2 arrow at 1080p (skips where the tools are missing)|python3 ./tools/checks/arm64-mouse-check.py
retry|6|ARM64 Calculator: calc.c gets a table of 42 sums right on the host (sin(30deg)=0.5, 5!=120, 2^10=1024, 1/0 is an error), its keypad's memory and Deg keys work, and the calctest build types four sums at boot with the same answers on the UART and the window in a screendump (QEMU part skips where the tools are missing)|python3 ./tools/checks/arm64-calc-check.py
retry|6|ARM64 keyboard only: with no mouse, Ctrl+Space and F1 open Spotlight (drawn in the house colours), typing term and Enter opens the Terminal with the keyboard, Esc hands the keys back, Left and Right move the dock's label and Enter opens that tile, F2 opens the Terminal at once (skips where the tools are missing)|python3 ./tools/checks/arm64-keys-check.py
retry|6|ARM64 Mac-style USB keyboard: over xHCI, Cmd+Space opens Spotlight, a plain letter on the bare desktop opens it with the letter typed, Esc with it closed does nothing, a 9-byte report with a report ID still gives Ctrl+T, and the key debug line shows only in the dev build (skips where the tools are missing)|python3 ./tools/checks/arm64-keydbg-check.py
retry|6|ARM64 Calendar month/day digits, December rollover, Clock time/date and unset state, Spotlight open/close and Samantha launch (skips where the tools are missing)|python3 ./tools/checks/arm64-calicon-check.py
retry|6|ARM64 crash screen: an unexpected EL1 fault prints class, ESR, FAR, ELR and the last console lines on the UART, draws them as a panel (QEMU screendump on virt and the Pi 4B model) and halts quietly (skips where the tools are missing)|python3 ./tools/checks/arm64-crash-check.py
retry|6|ARM64 FP state: q0-q31, FPCR and FPSR survive timer interrupts whose handler wipes them, and the build without the save fails the same test (skips where the tools are missing)|python3 ./tools/checks/arm64-fp-check.py
retry|6|ARM64 out of memory: a full heap prints oom fb or oom text and the kernel carries on (no screen, or the VGA fallback font) on virt and the Pi 4B model (skips where the tools are missing)|python3 ./tools/checks/arm64-oom-check.py
retry|6|ARM64 boot health: the Pi image boots on QEMU's raspi4b with no FAIL line beyond the listed expected ones, no oom, no crash, and the desktop up (skips where the tools are missing)|python3 ./tools/checks/arm64-boot-health-check.py
once |4|Pi card flasher: kernel, firmware and tools/pi-config.txt land on a stand-in card, and what was there is kept as .bak (skips where the tools are missing)|bash ./tools/checks/flash-pi-check.sh
once |6|ARM64 RNG200 full seeds, health faults, bounded waits and wiped failures (host harness)|python3 ./tools/checks/arm64-rng-check.py
once |6|ARM64 clock accepts bounded HTTPS dates only, never before the build or an accepted clock (host harness)|python3 ./tools/checks/arm64-clock-check.py
retry|6|ARM64 Terminal asks the real relay over TLS, with session resume and wrong-token refusal|python3 ./tools/checks/arm64-claude-console-check.py --tls
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
