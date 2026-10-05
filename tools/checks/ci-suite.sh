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
export TMPDIR=$(mktemp -d "${TMPDIR:-/tmp}/jt-suite-XXXXXX")
trap 'rm -rf "$TMPDIR"' EXIT
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

# retry?  shard  name                                                   command
manifest() {
cat <<'EOF'
once |6|Dock slot constants agree with kernel.c (static drift guard)|python3 ./tools/checks/dockslots-check.py
once |6|Kernel memory keeps 16KB clear of the program window (toolchain drift guard)|python3 ./tools/checks/bss-margin-check.py
once |6|Samantha's face loops wrap without a seam|python3 ./tools/checks/face-frames-check.py
once |6|docs/TESTING.md lists every check in this suite|./tools/checks/testing-doc-check.sh
once |6|The landing logo rebuilds byte for byte from tools/gen/logo.py|./tools/checks/logo-check.sh
once |6|Landing app tiles are not older than the app code that draws them (source hashes, no QEMU)|python3 ./tools/checks/landing-shots-fresh-check.py
once |6|The landing demo downloads the kernel once, gzipped|./tools/checks/kernel-gz-check.sh
retry|1|Boot check|./check.sh
retry|4|DHCP client leases real SLIRP config, DNS+HTTP still work, nodhcp keeps the old fixed path|./tools/checks/dhcp-check.sh
retry|3|Benchmarks run and report every number|./tools/checks/bench-check.sh
retry|1|ISO boot (CD-ROM and USB/raw-disk paths)|./tools/checks/iso-boot-check.sh
once |0|PNG decoder, host harness|./tools/checks/png-host-check.sh
once |4|Burrow: Files renamed, Samantha still opens it as files / file browser, icon art is the new fox|python3 ./tools/checks/burrow-rename-check.py
retry|4|Burrow icon view wraps a long file name onto a second line instead of cutting it|python3 ./tools/checks/burrow-labels-check.py
retry|5|Mail, Notes and Weather read past byte 255 of their data file (SYS_READ moves 255 bytes a call)|python3 ./tools/checks/read-long-files-check.py
retry|6|Clock opens and its countdown timer updates live|python3 ./tools/checks/clock-check.py
once |0|TTF rasterizer, host harness|./tools/checks/ttf-host-check.sh
once |5|libjt string/stdlib, host harness|./tools/checks/libjt-host-check.sh
once |6|God-file guard (no hand-written .c/.h over its line ceiling)|./tools/checks/godfile-check.sh
retry|4|PNG decoder, in-kernel|./tools/checks/png-check.sh
retry|6|AA text spacing, in-kernel|./tools/checks/textspacing-check.sh
retry|0|AA text stems are dense but still antialiased|python3 ./tools/checks/textsharp-check.py
retry|3|FAT filesystem cycle hang (regression test)|./tools/checks/fatcyclehang-check.sh
retry|1|File robustness: empty, oversized, corrupt-FAT and full-disk cases|python3 ./tools/checks/filerobust-check.py
retry|4|Chat defaults to Samantha (Turing) and surfaces an HTTPS-redirect host clearly|python3 ./tools/checks/chat-samantha-check.py
retry|5|GUI Chat app asks Samantha and renders the reply on screen|python3 ./tools/checks/chatapp-check.py
retry|4|Chat's tools (reminder, note, open app) work locally via /api/pick, ordinary questions still reach Samantha|python3 ./tools/checks/chattools-check.py
retry|7|Chat mail tools: "read my email" lists a real message, "email <someone> <text>" lands one in Mail|python3 ./tools/checks/mailtools-check.py
retry|1|Chat notes/reminders tools: list_reminders and read_notes work via the local keyword fallback when the picker doesn't know them|python3 ./tools/checks/notestools-check.py
retry|2|"samantha" boot flag opens Chat's full-screen avatar view, input focused, before the desktop|python3 ./tools/checks/samantha-boot-check.py
retry|2|"phone" boot flag opens a real 430x932 portrait frame straight into Samantha's view|python3 ./tools/checks/phone-boot-check.py
retry|0|Phone Samantha back chevron exits her view and F2/Esc hints are hidden on phones|python3 ./tools/checks/phone-samantha-back-check.py
retry|2|Touch: a tap opens the on-screen keyboard on phone and a tapped key reaches the Notes editor|python3 ./tools/checks/touch-osk-check.py
retry|4|No-disk boot falls back to ramfs with seeded demo files|./tools/checks/ramfs-demo-check.sh
retry|3|Shell regression suite (heap, task, preempt, kill, ring3, ps)|./tools/checks/shellregress-check.sh
retry|5|Ring-3 reference program against the v1 syscall ABI|./tools/checks/usertest-check.sh
retry|3|Ring-3 program writing a real file against the v2 syscall ABI|./tools/checks/notetest-check.sh
retry|3|Keyrate runs as a ring-3 process with its own window; crashing it leaves the desktop alive (v3 syscall ABI)|python3 ./tools/checks/ring3app-check.py
retry|2|Toroid runs as a ring-3 process through the table-driven launcher: draws generations, closes both ways, crashes safely, desktop alive|python3 ./tools/checks/ring3toroid-check.py
retry|5|Calculator runs as a ring-3 process through the table-driven launcher: evaluates 12*3=36 and 5/0=0 through the real parser, closes both ways, crashes safely, desktop alive|python3 ./tools/checks/ring3calc-check.py
retry|2|Quotes runs as a ring-3 process through the table-driven launcher: draws the option grid, answers right and wrong through the real logic, closes both ways, crashes safely, desktop alive|python3 ./tools/checks/ring3quotes-check.py
retry|1|Bookrank runs as a ring-3 process through the table-driven launcher: draws the ranked list, moves the selection by keyboard and mouse through the real logic, closes both ways, crashes safely, desktop alive|python3 ./tools/checks/ring3bookrank-check.py
retry|5|Bookrank pulls the real shelf live (Worker text from a stub), scrolls it, bounds a hostile reply, and shows the ten samples for junk, a 500, an oversize body and no NIC|python3 ./tools/checks/ring3bookrank-live-check.py
retry|5|Tonchi runs as a ring-3 process through the table-driven launcher: draws the word and choices, answers right and wrong through the real drill, closes both ways, crashes safely, desktop alive|python3 ./tools/checks/ring3tonchi-check.py
retry|5|Tonchi lists the real courses live (Worker text from a stub), drills one with a score, bounds a hostile reply, and shows the Spanish deck for junk, a 500, an oversize body and no NIC|python3 ./tools/checks/ring3tonchi-live-check.py
retry|6|Fieldbook runs as a ring-3 process through the table-driven launcher: draws the ranked field list, moves the selection by keyboard and mouse through the real logic, closes both ways, crashes safely, desktop alive|python3 ./tools/checks/ring3fieldbook-check.py
retry|3|Clock runs as a ring-3 process through the table-driven launcher: draws the moving time, takes a timer through the real input logic, closes on Esc, desktop alive|python3 ./tools/checks/ring3clock-check.py
retry|6|Movies plays a real AVI with sound at ring 3, audio-led: frame within one of the audio clock, drift under 100 ms, pause holds frame and sound (checked in the wav and on the framebuffer), seek by bar and keys, fullscreen, a damaged clip / non-AVI / over-cap file each show an error, closes on Esc, desktop alive|python3 ./tools/checks/movie-check.py
once |6|AVI reader, host harness (real JPEG frames, bad headers, truncation, mutation fuzz under ASan/UBSan)|./tools/checks/avi-host-check.sh
retry|5|Portfolio runs as a ring-3 process through the table-driven launcher: draws the fleet catalog, moves the selection by keyboard and mouse through the real logic, closes on Esc, desktop alive|python3 ./tools/checks/ring3portfolio-check.py
retry|7|Activity runs as a ring-3 process through the table-driven launcher: draws the live task list, refreshes it on its own, has the kernel refuse to kill the shell, closes on Esc, desktop alive|python3 ./tools/checks/ring3activity-check.py
retry|5|Contacts runs as a ring-3 process through the table-driven launcher: draws the list, adds and deletes a person through the real prompt, keeps CONTACTS.TXT across fresh runs, closes on Esc, desktop alive|python3 ./tools/checks/ring3contacts-check.py
retry|3|Hikko runs as a ring-3 process through the table-driven launcher: draws the ranked list, moves the selection, re-sorts when an upvote passes the leader, closes on Esc, desktop alive|python3 ./tools/checks/ring3hikko-check.py
retry|5|Hikko shows the real forum ideas live (Worker text from a stub), scrolls them, keeps votes local and forum order for ties, bounds a hostile reply, and shows the ten demo ideas for junk, a 500, an oversize body and no NIC|python3 ./tools/checks/ring3hikko-live-check.py
retry|3|Reminders runs as a ring-3 process through the table-driven launcher: adds, ticks and deletes items through the real prompt, keeps REMINDERS.TXT across fresh runs, closes on Esc, desktop alive|python3 ./tools/checks/ring3reminders-check.py
retry|6|Curbfind runs as a ring-3 process through the table-driven launcher: falls back to the samples when SYS_HTTP_GET finds no NIC, selects by key and click, the kernel refuses every bad path and pointer the probe hands the syscall, closes on Esc, desktop alive|python3 ./tools/checks/ring3curbfind-check.py
retry|7|Calendar runs as a ring-3 process through the table-driven launcher: gets today from SYS_TIME, draws the month grid, saves an event through the real editor, keeps EVENTS.TXT across fresh runs, feeds Samantha's calendar_today, closes on Esc, desktop alive|python3 ./tools/checks/ring3calendar-check.py
retry|4|Search runs as a ring-3 process through the table-driven launcher: SYS_READDIR refuses a kernel pointer, an over-long path and a missing folder, the list filters live, a file shows its real bytes, a FAT folder opens by relative path with the kernel's cwd untouched, closes on Esc, crashes safely, desktop alive|python3 ./tools/checks/ring3search-check.py
retry|7|Epiphany runs as a ring-3 process through the table-driven launcher: falls back to the offline prices when SYS_HTTP_GET finds no NIC, switches tabs by key and click, runs the command bar, closes on Esc, desktop alive|python3 ./tools/checks/ring3epiphany-check.py
retry|6|Weather runs as a ring-3 process through the table-driven launcher: reads the kernel's WEATHER.TXT and shows the offline face over labelled sample data, R refetches once and restarts it, closes on Esc, desktop alive|python3 ./tools/checks/ring3weather-check.py
retry|7|Burrow runs as a ring-3 process through the table-driven launcher: draws the folder grid, Enter opens a folder and Backspace goes up through SYS_READDIR, closes on Esc, desktop alive|python3 ./tools/checks/ring3burrow-check.py
retry|3|Every ring-3 app (parsed from RING3_APPS) crashes on purpose, is reaped, and the desktop keeps drawing and opens a different app after each one|python3 ./tools/checks/ring3crash-all-check.py
retry|7|A released window framebuffer is supervisor-only again: store faults, pointer into it or into the kernel is -EFAULT|python3 ./tools/checks/userfb-release-check.py
retry|1|SYS_BRK: a program grows 3MB of zeroed heap pages, bad tops are -EINVAL, and its crash gives every frame back (brk live=0, pmm free unchanged), desktop alive|python3 ./tools/checks/ring3brk-check.py
retry|0|Shell launches a ring-3 program by bare name, case-insensitively|./tools/checks/shellname-check.sh
retry|4|QEMU vmmouse absolute-pointer round trip|./tools/checks/vmmouse-check.sh
once |6|Calendar date math, host harness|./tools/checks/check-calendar.sh
retry|0|Settings click acts on the row actually clicked|./tools/checks/settingsclick-check.sh
retry|4|Settings Location geocodes, persists, and fails clean|python3 ./tools/checks/location-check.py
retry|5|Wallpaper defaults to Satellite on a fresh boot|./tools/checks/walldefault-check.sh
retry|7|Idle tour's Settings visit doesn't change the wallpaper theme|python3 ./tools/checks/walldemo-regression-check.py
retry|4|Wallpaper compose: Map and Satellite fetch distinct, byte-correct buffers (hermetic, fake tile server)|python3 ./tools/checks/wallcompose-check.py
retry|0|Notes editor chrome doesn't redraw on plain keystrokes|./tools/checks/editorflash-check.sh
retry|5|System-wide clipboard: Ctrl+C/X/V round-trips real text within Notes, across Notes->Terminal, truncates a too-long paste cleanly|python3 ./tools/checks/clipboard-check.py
retry|2|Reminders prompt redraws content, not chrome, per keystroke|./tools/checks/gui-prompt-keystroke-check.sh
retry|7|Notes typing, typography, pointer controls, persistence|python3 ./tools/checks/editor_qa.py
retry|4|Notes' runtime-TTF text is real antialiased rasterization at 12pt and 200pt, not a duplicated-block bitmap upscale|python3 ./tools/checks/notessharp-check.py
retry|6|Notes folders: legacy NOTES.TXT migrates intact, a new note lands in the current folder, both survive reboot, delete asks first|python3 ./tools/checks/notesfolders-check.py
retry|4|Terminal grid draws the mono face at its true advance|python3 ./tools/checks/termmono-check.py
retry|5|Terminal's runtime-TTF text is real antialiased rasterization with a driftless monospace grid|python3 ./tools/checks/termsharp-check.py
retry|4|Apple-menu hover stays cheap, clock redraws on a minute change|./tools/checks/menuclock-check.sh
retry|6|Lock Screen: menu item locks, Esc cannot bypass, password unlocks|python3 ./tools/checks/lockscreen-check.py
retry|5|Multi-window chrome doesn't redraw on plain keystrokes|./tools/checks/mwkeyflash-check.sh
retry|2|Drawing lands offscreen, window_present puts it on screen|./tools/checks/backbuffer-check.sh
retry|6|Multi-window apps draw exactly one toolbar, not two|./tools/checks/mwdupetoolbar-check.sh
once |7|JPEG decoder, host harness|./tools/checks/jpeg-host-check.sh
once |6|Media decoders (WAV, MP3), host harness under ASan and UBSan|./tools/checks/media-host-check.sh
retry|5|Tasks keep their own x87 float state across switches|./tools/checks/fpu-check.sh
once |1|HTML entities decode to ASCII, host harness|./tools/checks/html-host-check.sh
retry|6|JPEG decoder, in-kernel|./tools/checks/jpeg-check.sh
once |7|PNG/JPEG decoder fuzz (ASan/UBSan, truncation+mutation+nasties)|./tools/checks/decoder-fuzz-check.sh
once |1|HTTP/JSON/FAT16 parser fuzz (ASan/UBSan, truncation+mutation+nasties)|./tools/checks/parser-fuzz-check.sh
once |2|Text-input bounds: json.c maxlen 0/1/truncation + auth const-time compare (ASan/UBSan)|./tools/checks/input-bounds-check.sh
once |0|Password hashing, host harness: PBKDF2-HMAC-SHA256 vectors (RFC 7914, RFC 6070 for SHA-256), legacy record upgrade, USERS.TXT parse|./tools/checks/auth-check.sh
retry|6|Dock apps open/close from the pointer alone|python3 ./tools/checks/appclose-check.py
retry|7|Esc on a bare desktop does not quit the GUI; Mail still opens from the dock|python3 ./tools/checks/esc-desktop-check.py
retry|1|Dock hover survives mid-animation|python3 ./tools/checks/dockhover-check.py
retry|1|Launchpad tile click launches, doesn't just close the folder|python3 ./tools/checks/launchpad-click-check.py
retry|6|Apps folder layout (no black band, no row spill, no ghost icons)|python3 ./tools/checks/appsfolder-layout-check.py
retry|6|Menu bar present after Launchpad, every app open and close, Esc, panels, drags; screen never black|python3 ./tools/checks/menubar-persist-check.py
retry|6|Launchpad centered, evenly padded, icons 56px or smaller, labels clear (pixels, five screen sizes)|python3 ./tools/checks/launchpad-centered-check.py
retry|0|Typography: baseline flatness, letter-gap variance, container padding|python3 ./tools/checks/baseline-check.py
retry|7|Multi-window (click-to-focus, real z-order compositing)|python3 ./tools/checks/multiwindow-check.py
retry|4|Ring-3 window: Reminders beside Notes, keys to the focused window only, a crash closes only its window|python3 ./tools/checks/ring3window-check.py
retry|7|Ring-3 stress: window task and desktop hammer heap and FAT at once, heap walk ok, file at root, desktop alive|python3 ./tools/checks/ring3stress-check.py
retry|2|Ring-3 PDE sync: kernel page table born after the window task is visible on its CR3, close clean, desktop alive|python3 ./tools/checks/pdesync-check.py
retry|4|Window snapping (title-bar drag to edge/corner, real pixel proof)|python3 ./tools/checks/windowsnap-check.py
retry|3|Ring-3 window resize: snap Notes to a quarter, JT_EV_RESIZE answered, new buffer mapped, app redrew at the quarter size (serial marker + far-corner pixels)|python3 ./tools/checks/ring3resize-check.py
retry|5|Windows drag live by their title bar (single-window Notes and multi-window Files)|python3 ./tools/checks/windowdrag-check.py
retry|6|Windowed apps start under the title bar, Calendar fits six weeks|python3 ./tools/checks/apptop-check.py
retry|6|Calendar Day, Week, Month and Year views (ring-3 program)|python3 ./tools/checks/calviews-check.py
retry|2|QA gallery: every app opens, screenshots, closes, no crash|python3 ./tools/checks/qa-gallery.py /tmp/jt-gallery
retry|1|Every app's main action, headless|python3 ./tools/checks/feature-drive.py
retry|6|Dock icon edge quality (no staircased corners)|python3 ./tools/checks/iconedge-check.py
retry|7|Dock icon halo (clean clip to the tray, no glyph bleed)|python3 ./tools/checks/iconhalo-check.py
retry|1|Dock icon lighting (one soft top light, top highlight, no dark outline)|python3 ./tools/checks/iconlight-check.py
once |4|Every authored icon shares one tile silhouette, AA edges, glyph margin|python3 ./tools/checks/iconinset-check.py
retry|5|Calendar dock tile shows today's date, not fixed art|python3 ./tools/checks/calicon-check.py
retry|3|Clock icon is a live analog face: hands follow the RTC and redraw on the minute|python3 ./tools/checks/clockicon-check.py
retry|5|Shadow under the dock darkens the photo, no flat bands|python3 ./tools/checks/dockband-check.py
retry|0|Titlebar traffic-light AA (real coverage blend, not binary)|python3 ./tools/checks/titlebar-aa-check.py
retry|2|Dock tray corner AA (real coverage blend, not binary)|python3 ./tools/checks/traycorner-check.py
retry|1|Portfolio catalog opens, lists the fleet, and the list scrolls|python3 ./tools/checks/portfolio-check.py
retry|5|Asking Samantha for the weather fetches it, no "open Weather first"|python3 ./tools/checks/samweather-check.py
retry|6|Boot logo AA (no false interior seams at overlapping capsule joints)|python3 ./tools/checks/bootlogo-check.py
retry|4|Boot splash draws the real landing/logo.svg mark, not the old stick tree|python3 ./tools/checks/bootmark-check.py
once |6|Landing eyebrow tracks roadmap Latest, H1 stays the brand line|./tools/checks/landing-headline-check.sh
once |6|Idle tour still cycles all 8 real dock apps|node ./tools/checks/tourappcount-check.mjs
once |6|Idle tour autoplay fix is in place (tourArmed reset)|node ./tools/checks/idletour-arm-reset-check.mjs
once |6|RTC local-time shift math (v86's CMOS answers in UTC)|node ./tools/checks/rtc-timezone-check.mjs
retry|0|Stocks opens without a supported network card|python3 ./tools/checks/stocks-dock-check.py
retry|0|Stocks chart line is antialiased (coverage blend, no stair-stepping)|python3 ./tools/checks/stocks-aa-check.py
retry|0|Stocks fills all five watchlist rows from a full-size stub Worker reply (fails if the app reads only part of STOCKS.TXT)|python3 ./tools/checks/ring3stocks-list-check.py
once |6|Stocks live quotes and kernel parsing|node ./tools/checks/stocks-live-check.mjs
retry|7|Epiphany command bar: AAPL GP draws the chart, an unknown code errors cleanly|python3 ./tools/checks/epiphany-cmdbar-check.py
once |6|Worker /api/proxy allowlist|node ./tools/checks/worker-proxy-check.mjs
once |6|Worker /api/waitlist store, validate, count|node ./tools/checks/waitlist-check.mjs
once |6|Mail Send: Worker /api/mail/send guards and Resend shape, kernel bearer wiring|python3 ./tools/checks/mailsend-check.py
once |7|Worker /api/proxy: a silent upstream cannot hang the guest (weather/chat freeze regression)|node ./tools/checks/weatherproxy-hang-check.mjs
once |6|Worker /api/listen: Whisper transcription, 503 without the AI binding, rejects oversize/empty, per-IP rate limit|node ./tools/checks/listen-worker-check.mjs
retry|5|Every app opens and closes by keyboard alone|python3 ./tools/checks/keyboard-only-check.py
once |0|Soak: every app opened and closed once each in one boot, no leak, no crash|python3 ./tools/checks/soak-check.py 1
retry|4|Accounts: legacy record upgrades to PBKDF2 on login, create, reboot, login, wrong and empty passwords rejected, change password|python3 ./tools/checks/auth-flow-check.py
retry|7|Entropy pool: serial names its sources, two boots draw different salts|python3 ./tools/checks/entropy-check.py
once |2|Panic screen: a ring-0 fault paints the reason, not a frozen desktop|python3 ./tools/checks/panic-check.py
once |6|Crash report names the faulting function, not just the exception kind|python3 ./tools/checks/panic-symbols-check.py
once |3|Frame time: idle, dock hover and window open stay within budget|python3 ./tools/checks/frametime-check.py
once |5|Every check in tools/checks/ is in this manifest or says why not|./tools/checks/suite-coverage-check.sh
retry|2|Text selection in Notes: Shift-arrow/Ctrl+A highlight, edsel/edcopy/edcut markers, selection-aware copy/cut/paste/delete|python3 ./tools/checks/textselect-check.py
retry|7|Window top edge and corner arc are one continuous AA shape|python3 ./tools/checks/windowedge-check.py
retry|0|Sound Blaster 16 detects, beep plays a real 440Hz tone, card-less boot is a no-op|python3 ./tools/checks/sb16-check.py
retry|6|Music runs as a ring-3 app: plays a fixture WAV off a FAT disk (position from the driver, pause holds, bar and arrow seeks land, next changes the song, oversize file refused) and the sound card hears the tones|python3 ./tools/checks/music-check.py
retry|1|Sound Blaster 16 record path: `listen` reaches the driver and times out cleanly (QEMU has no ADC backend), card-less boot is a no-op|python3 ./tools/checks/sb16-record-check.py
retry|2|Chat speaks: say fetches /api/speak PCM from a stub and plays a real 1000Hz tone|python3 ./tools/checks/chat-speaks-check.py
retry|4|Chat face: idle frame before, talk frames while she speaks, idle after; no frames means no face|python3 ./tools/checks/chat-face-check.py
retry|5|HTTP stress: a ring-3 app makes 72 sequential SYS_HTTP_GET calls, all succeed (none -EIO), heap free stays at baseline|python3 ./tools/checks/httpstress-check.py
retry|1|Burrow view switcher: List/Icons choice is saved to BURROW.TXT and a fresh run reads it back|python3 ./tools/checks/filesview-check.py
retry|3|Demo canvas fills its frame, pixelated only at an exact 1:1 map|node ./tools/checks/democrisp-check.mjs
once |3|Landing page never overflows horizontally at phone widths|node ./tools/checks/mobile-overflow-check.mjs
once |2|Landing: benchmark labels clear, one app count, Samantha link styled, sections visible on load|node ./tools/checks/landing-layout-check.mjs
once |3|Portfolio voice: on by default, mute button top right, remembered across a reload, absent outside portfolio|node ./tools/checks/portfolio-mute-check.mjs
retry|2|Portfolio tour starts even when his face frames are slow and the first Escape presses are lost: his chat exits and Epiphany opens|node ./tools/checks/portfolio-slowframes-check.mjs
once |7|Landing demo on a phone: the chat bar raises the keyboard, typed letters reach Samantha once each and Send is Enter|node ./tools/checks/mobile-type-check.mjs
once |1|Landing: Tech specs accordions hold every developer number collapsed, and the footer is a four-column directory of real links|node ./tools/checks/landing-specs-footer-check.mjs
once |3|Landing demo: no keyboard trap, one main landmark, Full screen top right on phones|node ./tools/checks/landing-demo-ui-check.mjs
retry|1|App switcher: Ctrl+Tab cycles open windows and focuses the highlighted one|python3 ./tools/checks/appswitcher-check.py
retry|2|Screenshot key: Ctrl+Shift+3 saves a real framebuffer BMP, numbered and visible in Files|python3 ./tools/checks/screenshot-check.py
retry|3|Drunk mode easter egg: horizontal sway applied to framebuffer rows|python3 ./tools/checks/drunk-mode-check.py
retry|6|ARM64: the aarch64 kernel boots under QEMU and prints over the UART (skips where the tools are missing)|python3 ./tools/checks/arm64-m0-check.py
retry|6|ARM64 M1c: the aarch64 kernel draws a desktop into a ramfb framebuffer and QEMU screendump shows it (skips where the tools are missing)|python3 ./tools/checks/arm64-m1c-check.py
retry|6|ARM64 M2: the aarch64 kernel drives virtio disk, network, keyboard and mouse: a sector read back, a real ARP answer, key presses, moves and clicks (skips where the tools are missing)|python3 ./tools/checks/arm64-m2-check.py
retry|6|ARM64 M3a: an unprivileged EL0 program prints through a write syscall, exits, and a direct access to a kernel-only page faults while the kernel survives (skips where the tools are missing)|python3 ./tools/checks/arm64-m3-check.py
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
