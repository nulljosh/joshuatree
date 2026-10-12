# Joshua Tree roadmap: done

## 3.7.3 development branch (2026-10-12)

The Bluetooth mouse comes back by itself. Before, a saved pairing got one 20 second try at boot and nothing after, so a mouse that was asleep, or that slept later, stayed gone until `bt reconnect`. Now the reconnect is a run of tries (12 seconds each, then a pause of 2, 4, 8, 15 and 30 seconds, up to 30), it starts at boot, when the link drops and on `bt reconnect`, and it re-encrypts with the saved key and turns the mouse's reports on again each time. `bt status` shows searching, connected or idle, the saved address with only its last byte, the tries and the last error. A mouse that ignores boot protocol now has its report map read and its button, X, Y and wheel fields placed from it (12 bit packed X and Y checked), and `bt status` shows the last report's bytes. Proven in QEMU only against a fake mouse, not yet on the Pi. Check: `tools/checks/arm64-bt-check.py`.

## 3.6.3 development branch (2026-10-12)

The user guide gains a chapter for each Pi app and tool: Terminal and the shell, Browser, Books, Music, Notes, Calendar, Clock, Samantha and Bluetooth. The Pi row of `architecture.svg` now lists them, solid only where the board has seen it. Browser bookmarks are on the roadmap. Check: `tools/checks/check-refs.sh`.

## 2.63.1 development branch (2026-10-12)

The first board pass fixes. The Pi boots to a quiet desktop: the boot log steps aside once the desktop is up (a FAIL or oom line keeps it, Wi-Fi lines do not), and the dock-label lap that played by itself at boot is gone. One app at a time swaps instead of ignoring you: a dock tile for another app closes the one in front and opens it (Notes did nothing while Calendar was open). `bt scan` speaks English: each device as what it says it is (Mouse, Keyboard, Phone, Apple device, or its own name) and how close it is, mice first, numbered, so `bt pair 1` works; it holds 64 devices where 24 dropped the mouse in a busy room, and a mouse or keyboard takes the place of the weakest other device when the table is full. The Terminal says `samantha:` where it said `claude:`. The dev card build bakes our relay's CA (`tools/flash-pi.sh`), the missing piece behind "TLS failed". Pi text uses the shared coverage curve. Checks: `tools/checks/arm64-boot-quiet-check.py`, `arm64-bt-check.py`.
## 2.63.2 development branch (2026-10-12)

Icons synced with their projects. Samantha's dock tile is her mascot, Turing's app icon since v5.12.4 (the flat portrait with copper hair and tortoise-shell glasses, `web/mascot.svg`), in place of the voice-orb stand-in; `tools/gen/import_fleet_icons.py` can now name a file inside a repo and lay a ground colour under art that is a bare shape. Tonchi's icon comes from its renamed repo. Every other fleet icon was re-imported and is unchanged.
## 2.66.0 development branch (2026-10-12)

Browser app on the Pi. A Browser window opens from Spotlight: address bar, page as text in Tree Sans with clickable accent link lines, Back, Forward, Reload, scroll and a status line, wrapped around `browser.c`. Protocol-relative links (`//host/x`) keep the page's scheme. Check: `arm64-browser-app-check.py`.
## 2.66.0 development branch (2026-10-11)

Books, a plain-text reader as a ring-3 app (`user/books.c`) in the Apps folder and Spotlight. It lists the `.TXT` files in `BOOKS/` (the card's `JT/BOOKS/`), turns one page at a time with Right, Space and Left, and saves the page per book in `NAME.POS`. Books over 3 MB are refused; the card ships with one public domain sample. Check: `tools/checks/books-check.py`.

## 2.61.3 development branch (2026-10-11)

Music artwork. A cover comes from the picture in an MP3's ID3 tag (JPEG or PNG), else a `COVER`, `FOLDER` or `ALBUM` file (`.JPG` or `.PNG`) beside the song, which also covers WAVs; anything else keeps the note tile. A non-square picture is cropped to its centre square and scaled to the 96 px tile. PNG needed the kernel's decoder in ring 3 (`user/libjt/png.c`). The transport row is Apple Music's: Shuffle at the left, Previous Play Next centred under the bar, Repeat at the right, and the right-hand time counts down. Checks: `tools/checks/music-qa-check.py` boot E reads a pixel off each kind of cover.

## 3.0.0 "Hold it" (2026-10-12)

Version 3: the desktop on a real Raspberry Pi 4 with apps in user mode. The landing page's Pi section says what works on the board and what is still only proven in QEMU, the ten minute board pass is `docs/BOARD-TEST.md`, and `docs/LOOP-HANDOFF.md` says where everything stands. This release carries no code of its own: it is the twenty PRs above, cut as one number. The board pass decides whether it holds.

## 2.62.0 development branch (2026-10-11)

Tree Sans, the Joshua Tree typeface, on the landing page. Derived from Inter 4.1 (OFL) the way Nimble Sans was, with a Text and a Display optical size, alternates chosen once so I, l and 1 never read alike, and vertical metrics on a 1/16 em grid. Around it a type system with constraints: nine sizes, line heights on a 4 px grid, tracking that tightens as type grows, one spacing ladder (`docs/TYPE.md`, generated by `tools/gen/typetokens.py`). All 25 ad hoc landing font sizes now sit on the nine; headings use Display and wrap balanced. The OS still draws DejaVu; moving its atlas to Tree Sans with fractional advances and kerning is next. Check: `tools/checks/type-grid-check.py`.

## 2.61.0 development branch (2026-10-11)

Music, the iTunes pass. A song shows its real name: the library reads the first 2 KB of each file and takes the ID3 title and artist from an MP3 (v2.2, 2.3, 2.4; Latin-1, UTF-8, UTF-16), and the cover from its picture frame through the JPEG decoder Movies already uses, so `NEVERG~1.MP3` reads "Never Gonna Give You Up" with Rick Astley's cover. Rows carry each song's length from its header (WAV fmt and data, MP3 Xing frame or bit rate) and sort by title. The seek and volume knobs drag: the kernel sends `JT_EV_DRAG` to a ring-3 window while the button is held inside it, the seek lands once on release. The ARM build links the decoder too, and `arm64-music-check.py` reads the `ahead=` position lines. Under it the desktop reads the wheel without pumping the mouse queue a second time, which had swallowed fast clicks in the browser demo. Checks: `tools/checks/music-qa-check.py` boot D and the drag asserts, `tools/media/mp3-host.c` for the tag parser.

## 2.44.1 development branch (2026-10-11)

The Music QA pass: every key and click, a WAV and an MP3, screenshots of each state. A 3:25 MP3 read 0:10 (frames times 1000 wrapped past 32 bits), one damaged MP3 frame hung the song for good, a song that would not play stopped the queue and trapped n, shuffle never ended, no app window ever got a wheel step, a volume change landed a second late, a half-screen window cut off Repeat, and the list ran under the key hint. All fixed (the wheel fix is the kernel's: every ring-3 window now gets wheel steps, and Music is the one checked). Check: `tools/checks/music-qa-check.py`, each fix's assertion failing when the fix is reverted, and the damaged-frame case in `tools/media/mp3-host.c`.
## 2.48.1 development branch (2026-10-11)

On the landing page, Samantha tells the time and the date from the machine's own clock ("It's 3:07 PM."): the server cannot see a visitor's time zone, so she used to say she couldn't find anything. Her input on a desktop is now the page's own field over her face, sharp at any size; the kernel's bar was a small bitmap font scaled up with the screen and read pixelated. `pagebar` on the boot line tells her to draw none. Checks: `tools/checks/samantha-clock-check.py`, `tools/checks/desktop-pagebar-check.mjs`.

## 2.42.1 development branch (2026-10-11)

The Samantha box now goes together: the bezel screws moved to a 9 mm band outside the screen and clear of the Pi and its port windows, the grille stopped cutting its own centre loose, the stand became two cradles the box really drops into, and the fit test is a pocket the screen drops into. Every assert now runs before a file is written, and the Pi 4B numbers live in `docs/hardware/pi4b.py` for both cases. Checked by `tools/checks/samantha-box-check.py`, which builds the CAD and wants the old stand slot to stop it.
## 2.60.0 development branch (2026-10-11)

The 3.0 mark, option A from Joshua's contact sheet: the crayon tree with steadier, heavier lines, denser tufts and a terracotta ground, the one accent. `tools/gen/logo.py` writes every SVG, the PNGs and the README copies are regenerated, and the kernel blends the ground in terracotta on the splash (drawn last), the menu bar and About. Checks: `logo-check.sh`, `bootanim-check.py` (ground last) and `bootlogo-check.py` (ground in the menu bar).
## 2.59.0 development branch (2026-10-11)

M3e, Music on the Pi: the unchanged `user/music.c` opens from Spotlight and plays the WAV and MP3 songs in the card's `JT/MUSIC/` out of the 3.5 mm jack. Under it, a 16 MiB EL0 arena with a `JT_SYS_BRK` heap for whole songs, a `JT_SYS_AUDIO` that never waits (a 32 KB ring the desktop's loop drains into a DMA-fed PWM ring, through the 2.48.0 master volume, merged in here), and an app's FP registers kept across every call and park so minimp3 runs at EL0. Check: `tools/checks/arm64-music-check.py`; the jack is unverified.

## 2.55.0 development branch (2026-10-11)

Notes saves itself. A changed note writes to disk 1 to 2 seconds after your last key and the title's star clears, so closing the window with the red dot (which kills the program, Esc never runs) no longer loses the edit, and a crash costs at most two seconds. The kernel has no close hook, so the timer is the whole fix. Checked by `tools/checks/notes-autosave-check.py`: no save per keystroke, one save by itself, a red-dot close, the text on the disk image and back on screen after reopening.
## 2.58.0 development branch (2026-10-11)

M3d, files for user mode on ARM: `arch/arm64/ufile.c` answers open, read, write, close, lseek, readdir, readfile, mkdir and unlink with i386's errnos and limits over the shared FAT code, rooted in the card's `JT/` (a virtio disk on QEMU virt). Notes is on the Pi's dock: a typed note is saved to `JT/NOTES/NOTES/` and reads back after a reboot. One app at a time, since fat.c has one current directory. Checks: `tools/checks/arm64-notes-check.py`, and the abuse pass and hello.c to exit 9 in `tools/checks/arm64-m3c-check.py`.

## 2.57.0 development branch (2026-10-11)

Files that stay: the Pi writes its SD card (CMD24 and CMD25 with busy waits in `arch/arm64/emmc.c`, FAT32 writes in `drivers/fat.c`), and the Terminal's `write`, `rm` and `mkdir` keep the OS's own files in `JT/` across a reboot. Every name the firmware boots from is refused, and so is a write onto the boot sectors. Checked by `tools/checks/arm64-sd-check.py`, which reboots QEMU on the same card and checks it from the host with mtools and `fsck.fat`.

## 2.53.0 development branch (2026-10-11)

The Pi reads its own SD card: `arch/arm64/emmc.c` brings the card up on EMMC2 and reads blocks, and the i386 kernel's `drivers/fat.c`, now able to read FAT32, mounts the boot partition read only for the Terminal's new `ls` and `cat NAME`. Files that stay are a third of the 3.0 gate, and reading comes before writing. Checked by `tools/checks/arm64-sd-check.py`, which plugs a FAT32 card into QEMU's EMMC2 at the board's own address.

## 2.52.0 development branch (2026-10-11)

Caps Lock and key repeat on ARM. Caps Lock capitalises letters and Shift flips it, in the Terminal, the Calculator and Spotlight (Shift now reaches Spotlight too). A held typed key or Backspace types again after 400 ms and every 80 ms, where the first press went, until the release or another key. Before, Caps Lock did nothing and holding Backspace deleted one character: the USB keyboard sends a held key once. Check: `tools/checks/arm64-keys-check.py` holds keys with QMP and counts the repeats; `arm64-keydbg-check.py` does it over USB. Needs a real keyboard on the board; the Caps light stays off for now.
## 2.54.0 development branch (2026-10-11)

Bluetooth, slice BT2: `bt pair` connects Joshua's LE mouse, pairs it with Just Works, turns encryption on, finds its boot mouse report over GATT and feeds every report to the pointer, so it moves the arrow and clicks the dock. The key stays in RAM, so a reboot means pairing again. Not yet seen on the board. Check: `tools/checks/arm64-bt-check.py`, against a fake mouse that checks the pairing math with its own AES.

## 2.51.0 development branch (2026-10-11)

Bluetooth, slice BT1: the Pi talks HCI to its radio over the PL011, loads the BCM4345C0 patch, prints the version, and `bt scan` lists what it hears, so Joshua's Bluetooth mouse can be found before BT2 connects it. The Pi console moved to the mini UART to free the PL011, and `disable-bt` left `config.txt`. Not yet seen on the board. Check: `tools/checks/arm64-bt-check.py`, against a fake controller.
## 2.56.0 development branch (2026-10-11)

M3c part 2, Calendar in user mode on the Pi's desktop. The dock tile runs the unchanged `user/calendar.c` at EL0 in its own window, under a small cooperative scheduler: yield parks the app and the desktop's loop resumes it, so its forever loop no longer freezes the screen. Window open, poll and present follow i386; Esc or the red dot closes it. Its file calls are still -38, so the month is empty. Check: `tools/checks/arm64-calendar-check.py`.

## 2.50.0 development branch (2026-10-11)

M3c part 1, a real app loader on ARM. A `{name, start, end}` table holds `user/hello.c` and `user/calendar.c`, built unchanged for aarch64; `user_load` copies one in, relocates it, maps code read-only and executable and data, a zeroed pool and the stack never executable, and `userrun NAME` runs it. `JT_SYS_TIME` and `JT_SYS_GETPID` answer; calendar's `JT_SYS_WINDOW_OPEN` gets -38 and it exits, which is where part 2 starts. Check: `tools/checks/arm64-m3c-check.py`.

## 2.49.0 development branch (2026-10-11)

M3b, the first C program in user mode on ARM. `user/jtsys.h` gets an aarch64 `svc #0` path with the same `JT_SYS_*` numbers, `user/hello.c` builds unchanged as a flat position independent image baked into the ARM kernel, and the EL0 handler answers `JT_SYS_WRITE` and `JT_SYS_EXIT`. Unported calls return -ENOSYS, so hello prints its line, fails its `open` and exits 1. Check: `tools/checks/arm64-m3-check.py` (the old M3a lines and the three faults still pass).
## 2.48.0 development branch (2026-10-11)

Volume controls for the Pi. A master volume, 0 to 100 and 70 by default, scales every sample as it enters the audio ring, so the PWM range is never exceeded and 0 is silence. `vol`, `vol N`, `vol up` and `vol down` in the Terminal; F11, F12 and a Mac keyboard's volume and mute keys step it by 10 before any pane, show "Volume N" in the menu bar and click through `beep`. Checked by `tools/checks/arm64-volume-check.py`: `vol 50` is half the ring sum of `vol 100`, `vol 0` is silent, scripted keys print the notice. The real speaker still needs tuning.

## 2.43.0 development branch (2026-10-11)

The Pi can make sound. `beep` (or `beep 880 500`) in the Terminal plays a quiet tone out of the 3.5 mm jack: GPIO 40 and 41 go to PWM1, the PWM clock is set through the clock manager, and a ring buffer feeds the PWM FIFO by hand, with no DMA yet. Every hardware wait gives up and prints one line; nothing touches the jack or the green light at boot. `tools/checks/arm64-audio-check.py` proves the tone reaches the ring (checksum matches a tone computed in Python), the FIFO order and scaling on a fake PWM, and that stuck hardware cannot hang it. QEMU has no PWM, so the jack itself is unverified until Joshua tests the real board.
## 2.47.0 development branch (2026-10-11)

Samantha answers the Pi from Joshua's own model now. The credit ran out and the prompt still said Claude. The relay's new `--turing` backend posts to Turing's `app/serve.py` and answers "M Samantha". `--api-key-file` will not start without `--allow-api-spend`. The Pi's prompt reads `Samantha $ ` before any answer, and `ask.c` now accepts the Samantha name, which it used to drop. Checks: `tools/checks/claude-relay-check.py` (stub Turing) and `tools/checks/arm64-claude-console-check.py`.
## 2.46.0 development branch (2026-10-11)

The Pi finds the Claude relay by itself when the router moves the Mac: it tries the last good address, else the baked one, then broadcasts a small UDP probe that `relay.py` answers. TLS now checks the fixed name `jt-relay` against the private CA alone, so the address no longer matters and a probe reply can never get the token. Check: `tools/checks/arm64-relay-discovery-check.py`.
## 2.45.0 development branch (2026-10-11)

The Mac relay can speak for Samantha. `POST /api/tts` takes text and returns raw 16-bit mono PCM at 22050 Hz from ElevenLabs. The key sits in a private file on the Mac and never leaves it. A per-request cap (400) and a per-day cap (5000), kept in a file across restarts, refuse before ElevenLabs is called. Pi playback is a separate open line. Check: `tools/checks/relay-tts-check.py`.
## 2.44.0 development branch (2026-10-11)

Spotlight opens only on F1 or Cmd+Space, like a Mac. A bare Space, Enter or Tab on the desktop and Ctrl or Alt+Space do nothing now (Joshua hit Space on the board). A letter still opens it with that letter typed. Checks: `tools/checks/arm64-keys-check.py` and `arm64-keydbg-check.py` press Space and Ctrl+Space and expect nothing.

Typing into apps on ARM, the last slice of M1d part two. The Calculator is now its own window over the right of the Terminal or Console, so both show. A click on one brings it to the front, and only that one gets the keys, the same rule the i386 desktop uses. Before, any open Calculator took every key. Check: `tools/checks/arm64-focus-check.py`, which fails with the old routing. Framebuffer write-combining is split out as its own roadmap line. Needs a USB mouse and keyboard test on the board.
## 2.43.0 development branch (2026-10-11)

The Pi can make sound. `beep` (or `beep 880 500`) in the Terminal plays a quiet tone out of the 3.5 mm jack: GPIO 40 and 41 go to PWM1, the PWM clock is set through the clock manager, and a ring buffer feeds the PWM FIFO by hand, with no DMA yet. Every hardware wait gives up and prints one line; nothing touches the jack or the green light at boot. `tools/checks/arm64-audio-check.py` proves the tone reaches the ring (checksum matches a tone computed in Python), the FIFO order and scaling on a fake PWM, and that stuck hardware cannot hang it. QEMU has no PWM, so the jack itself is unverified until Joshua tests the real board.

## 2.42.0 development branch (2026-10-11)

The Samantha box: a printable desk case for a 7 inch HDMI screen, a Pi 4B, a round speaker and a mic hole, built by `docs/hardware/samantha_cad.py` with a stand that leans it back and a fit-test frame to print first. The screen, speaker and board numbers are stand-ins until the parts are in hand, and nothing has been printed. Checked by the script's own asserts (the tallest port clears the screen, the Pi stays inside the walls, the speaker leaves the card a channel).

## 2.41.0 development branch (2026-10-11)

The boot splash draws the tree on, trunk first and spikes last, over 0.8 s before the bar fills, instead of showing it whole. `tools/gen/gen_boot_mark.py` re-renders `landing/logo.svg` as the pen's path grows and ships each pixel's first frame beside the coverage, so the kernel needs no animation data beyond one byte a pixel. Checked by `tools/checks/bootanim-check.py`, which samples the real framebuffer and wants a growing, never shrinking tree.

## 2.40.0 development branch (2026-10-10)
## 2.41.0 development branch (2026-10-10)

Pi HDMI blanks after five idle minutes and wakes on keyboard or mouse while apps keep running. Brick and active assistant requests prevent sleep. Refused requests preserve the display state and avoid tight retries. Host boundary, wrap and negative-control tests plus real raspi4b mailbox/input routing pass in `tools/checks/arm64-screen-idle-check.py`. QEMU does not render mailbox blanking; the physical HDMI check remains.

## 2.40.0 released (2026-10-10)

The PC/browser text shell can run `wc FILE` immediately after boot. Counts now use the number format libjt actually supports, fixing literal `%ld` output. It installs the existing protected user program into either the mounted filesystem or the RAM fallback, preserving an existing copy. The shell regression checks real keyboard input, exact counts and a missing-file error. Check: `tools/checks/shellname-check.sh`. PR #502 merged; local and GitHub gates, deployment, live browser QA and release ISO checksum passed. Board testing remains.

## 2.39.1 development branch (2026-10-10)

The landing footer has a wider directory, a larger closing statement and original desert line art built from our existing tree mark. Links now have separate 44px tap targets instead of overlapping invisible hit areas. Chromium and WebKit checks cover 320/390/768/1280px in both themes, keyboard focus, contrast and real links. Release status is recorded on PR #502. Check: `tools/checks/landing-specs-footer-check.mjs`.

## 2.39.0 release candidate (2026-10-10)

Combines the current review branches, including the older Mines app. Review caught Mines closing when its help panel was clicked. A QEMU regression reproduces that failure; off-board content clicks now stay in the game. The full combined gate and release approval remain.

## 2.38.1 development branch (2026-10-10)

The README and site link directly to the latest PC ISO and checksums. Releases now upload a stable `joshuatree.iso` name as well as the versioned image, and checksum both. The alias becomes available when this release workflow runs after approval/merge; the current 2.33.5 release predates it. Check: `tools/checks/iso-boot-check.sh` and the workflow packaging step.

## 2.38.0 development branch (2026-10-10)

The Pi has a short menu-bar notification strip. Wi-Fi joined and Brick win/game-over messages clear after four seconds; later messages replace earlier ones. Bounded-message sanitizer tests and real QEMU pixels/input/idle-expiry pass. Not merged, released or tested on the board. Check: `tools/checks/arm64-notice-check.py`.
## 2.37.1 development branch (2026-10-10)

Ring-3 app text now uses the kernel ink curve, including the Terminal mono face. Mail and Notes stem coverage rises from 23–33% to 44–52%, with six intermediate edge levels preserved. The stricter pixel check fails on the old renderer. Not merged or released. Check: `tools/checks/textsharp-check.py`.

## 2.37.0 development branch (2026-10-10)

Brick is implemented as an original native ARM game: 32 bricks, three lives, keyboard or mouse paddle, pause, restart, win and game over. Spotlight opens it. Sanitized host physics and QEMU input/pixel checks pass; not merged, flashed or physically verified. Check: `tools/checks/arm64-brick-check.py`.

## 2.36.0 development branch (2026-10-10)

Keyboard shutdown and restart are implemented for the Pi. Both require an explicit Enter after the chord; Escape cancels, a busy Samantha request refuses, and a hidden prompt cannot confirm. The host safety harness and real HID-to-watchdog shutdown/reboot pass on QEMU raspi4b. This is not merged or flashed; physical board verification remains. Check: `tools/checks/arm64-power-check.py`.

Items ticked off in `docs/roadmap.md`, moved here on 2026-10-06 so the roadmap only shows open work. Nothing was deleted; each item sits under the heading it had. The full history is in `git log` and the GitHub releases.

### Landing and demo, to A+
- [x] [Haiku] App tiles on the landing go stale. Done 2.6.28: tiles for Bookrank, Lexly, Curbfind and Epiphany, and `tools/checks/landing-shots-fresh-check.py` fails when an app changed and its tile was not retaken. Earlier: `tools/landing-shots.py` pins the QEMU clock to 2026-10-03 and the Calendar tile is retaken; the other tiles only redraw when their app changes. Original note: `python3 tools/landing-shots.py calendar` draws today's date, so the Calendar tile ages by the day. Pin the QEMU clock (`-rtc base=...`) in `tools/landing-shots.py` so every tile is the same on every run, then retake all of them.
- [x] [Haiku] Done 2026-10-04: Lexly's `icon.svg` is now the sky blue one and the `GLYPH` recolor is gone. Original note: Lexly's own repo disagrees with itself: `icon.svg` is a black tile with blue dots, `assets/icon.svg` and the store icon are sky blue. Make `icon.svg` the blue one so the import needs no recolor, then drop the `GLYPH` workaround.

### From the notebook (Joshua, 2026-10-04)
- [x] [Sonnet] Rename the Strata enclosure to Neo, done 2026-10-04: the hardware docs, CAD scripts and their output names, the landing and the ad text all say Neo now, and `docs/HARDWARE.md` notes the old name. Still [Joshua]: what mobility means for a Pi box (battery, portable monitor, carry case), since the notebook pitches Neo on mobility and security.
- [x] [Joshua] Constraints and design system as written rules, done 2.12.2: `docs/DESIGN.md` is the one page (what the OS never does, icons and the fleet rule, type, colour and the day and night blend, windows and dock, motion), and `tools/checks/design-doc-check.py` fails when a number in it stops matching the source or an icon picks up text, purple, teal or a hue-changing gradient. The icon half is the item under Landing, still open.

### Fleet apps on their real backends (Joshua, 2026-10-04)
- [x] [Sonnet] Bookrank, done 2.6.26: the ranked shelf (top twelve, rating, review count, badge, one-line summary) comes from the live Bookrank API through the Worker's `/api/books` and `SYS_HTTP_GET`; the ten samples stay as the offline fallback. Per-account chapter summaries are private in the real app, so they are not here. Check: `tools/checks/ring3bookrank-live-check.py`.
- [x] [Sonnet] Lexly, done 2.6.27: a course picker with 72 live courses and a four-choice drill with a score, from the real Lexly course packs through the Worker's `/api/lexly` and `SYS_HTTP_GET`; the Spanish deck stays as the offline fallback. Courses in scripts the font cannot draw (Japanese, Chinese, Korean, Russian, Arabic and more) and the short math courses are left out. Check: `tools/checks/ring3tonchi-live-check.py`.
- [x] [Sonnet] Hikko, done 2.6.29: Sparkjar is renamed Hikko inside the OS (files, window, dock, Apps folder, checks; Samantha still opens it as "sparkjar" or "hotaru"). It shows the real forum's top twelve ideas (title, votes, text, plan) from the public post feed through the Worker's `/api/hikko` and `SYS_HTTP_GET`; the ten demo ideas stay as the offline fallback. Voting stays local: the real forum needs an account to vote. Check: `tools/checks/ring3hikko-live-check.py`.
- [x] [Sonnet] Names, done 2.6.30: Lexly is Tonchi inside the OS (files, window, dock, Apps folder, checks, landing, Worker comments), the Portfolio catalog says Brick, Notate and Hagaki and adds Madobe and Plank. Samantha still opens Tonchi as "lexly". Tonchi keeps `lexly.heyitsmejosh.com` until `tonchi.` exists. Check: `tools/checks/burrow-rename-check.py` fails if an old name returns to the catalog or an alias stops working.
- [x] [Sonnet] Stocks showed a price for AAPL and "--" for the other four rows, done 2.6.31. The app read its data file with one `jt_read`, which moves 255 bytes a call, so it saw only the first row. It now reads the whole file, and the status line says Retrying or Offline instead of stale text. Check: `tools/checks/ring3stocks-list-check.py` (fails on the old app, passes now).
- [x] [Sonnet] Burrow labels, done 2.6.32: a long file name wraps onto a second line in icon view instead of being cut. Check: `tools/checks/burrow-labels-check.py` fails if the label goes back to one cut line.
- [x] [Sonnet] Read audit, done 2.6.32: one read moves 255 bytes, so Mail (inbox past 255 bytes) and Weather (readings past 255 bytes) lost data. Both now use `jt_readfile`. Notes, Note, wc, Search, Calendar, Contacts and Reminders already loop. Check: `tools/checks/read-long-files-check.py` fails on the old Mail and Weather.
- [x] [Sonnet] Menu bar missing or dark, done 2.6.34: Joshua saw the demo lose its menu bar sometimes. Cause: the full-screen Launchpad clears the whole screen, and the bar only repaints when the minute changes, so it stayed gone after Esc. Now every full desktop repaint redraws the bar and the Launchpad draws it too. Check: `tools/checks/menubar-persist-check.py` (fails on 2.6.32, passes now).
- [x] [Sonnet] Launchpad, done 2.6.35: Joshua saw it on a real monitor, left of center with big icons packed edge to edge. The folder window was parked at a fixed spot, so it was only centered on a 960 pixel screen. Now the glass panel is centered across the screen and between the menu bar and the dock, icons are 56 pixels (were 74), padding is even, and a short screen shows two rows. Check: `tools/checks/launchpad-centered-check.py` (fails on the old layout, passes now).

#### Every fleet app in the Launchpad (Joshua, 2026-10-04)
- [x] [Fable] Room for apps, done 2.7.1: apps are stored compressed in the kernel image and unpacked (and checksummed) when launched. 857 KB of embedded programs became 444 KB; `tools/checks/bss-margin-check.py` went from 35 KB to 438 KB free, enough for about ten more apps this size. A damaged image is refused ("ring3app: bad image") and the desktop keeps running. Next biggest things in the image if room runs short again: `wallpaper_rgb` 1.5 MB, `editor_pixels` 500 KB, `wall_sat_png` 277 KB.
- [x] [Sonnet] Hamurapi, done 2.7.0: the 1968 game with a drawn city, story cards, Classic, the orders panel, reports, endings and a robot demo. Its rules in C match the other three ports exactly; `tools/checks/ring3hamurabi-check.py` replays every reign on the host. Left for later: music and sound (the app has none yet), a difficulty setting, and a permanent check for the greyed-out choice.

### Voice chat, lag and sharing (Joshua, 2026-10-04)
- [x] [Sonnet] OS side, 2.6.22: one `voicetime:` serial line per message (pick, chat, first sound, in ms from Enter), asserted by `tools/checks/chat-face-check.py`.

### CI and speed
- [x] [Sonnet] Clock icon check flake fixed, 2026-10-04: on a slow runner the face sits still, white and handless for seconds, and the check took that as settled. Reproduced locally under CPU load with the exact CI numbers; it now waits for the hands to have ink (`tools/checks/clockicon-check.py`).
- [x] [Haiku] Found 2026-10-04: the repo's `core.hooksPath` was an absolute path into the main checkout, so every worktree ran whatever hook that checkout's branch had. Now the relative `tools/hooks` (what `make hooks` sets), so each worktree runs its own. Original note: Three red runs today were things the pre-push hook already covers (unlisted check, kernel.c over its line ceiling). The hook did not run on those pushes. Find out why (hook not installed in the worktree, or `--no-verify`) and make it hard to skip. It runs the god-file guard now.

### Raspberry Pi and ARM64
- [x] [Joshua] Bought the board, 2026-10-06: Pi 4B 4 GB, 32 GB microSD, heat sinks, case, fan. No serial cable yet; the screen shows the boot log so it can wait.
- [x] [Sonnet] Card flashed without Raspberry Pi OS, 2026-10-06: `tools/flash-pi.sh` builds `kernel8.img`, fetches the five firmware files and writes the config file (`docs/RASPBERRY-PI.md`).
- [x] [Joshua] First real boot over HDMI, 2026-10-06: the desktop came up on a Samsung monitor on the first clean power-on (`docs/RASPBERRY-PI.md` has the photo and log).
- [x] [Sonnet] Full screen on the real Pi, 2.12.5: the kernel asks the firmware how big the monitor is and draws the desktop at that size, laid out to fit. A 4K monitor gets exactly half each way, so the 2x scale stays sharp. Check: `tools/checks/arm64-m1c-check.py` fakes a 1080p and a 4K monitor on QEMU's Pi model.
- [x] [Sonnet] A quiet Steve Jobs line on the Pi desktop, 2.12.5: "Steve Jobs, 1955 to 2011. Thank you. Make something wonderful." sits just above the dock (the last sentence added 2026-10-06, the title of the Steve Jobs Archive book), added on the 15th anniversary. `tools/checks/arm64-m1c-check.py` looks for it.
- [x] [Fable] M1c part one, 2.6.21: ramfb framebuffer on QEMU virt through fw_cfg, a first desktop drawn into it, proven by a QEMU screendump (`tools/checks/arm64-m1c-check.py`).
- [x] [Fable] M1c part two, 2.6.22: the Pi build asks the GPU for a framebuffer through the VideoCore mailbox and draws the same desktop, proven on QEMU's Pi 4B model by screendump (`tools/checks/arm64-m1c-check.py`). Real board still to try.
- [x] [Sonnet] M1c boot log on screen, 2.6.22: every line the ARM kernel prints over serial is also drawn in the window, so a first boot with a bad serial cable still shows how far it got (`tools/checks/arm64-m1c-check.py` counts the text pixels).
- [x] [Sonnet] M1d part one, 2.6.23: the ARM screen draws smooth DejaVu text (the i386 desktop's own rasterizer, `drivers/ttf.c`, built for aarch64 with the FPU on): a menu bar title, a titled window and an anti-aliased boot log. `tools/checks/arm64-m1c-check.py` counts the soft edge pixels.
- [x] [Fable] M2 keyboard and mouse, 2.6.22: one modern virtio-mmio input driver reads key down and up, pointer position and clicks, polled (`tools/checks/arm64-m2-check.py`).
- [x] [Fable] M2 disk, 2.6.22: a virtio-blk driver reads a known sector off a disk image (`tools/checks/arm64-m2-check.py`). Writes and a FAT reader on top are next.
- [x] [Fable] M2 network, 2.6.22: a virtio-net driver sends an ARP request to the router and prints its real answer (`tools/checks/arm64-m2-check.py`).
- [x] [Fable] M3a, 2.6.24: the first EL0 program (`arch/arm64/user.S`) runs unprivileged, prints and exits through `svc` (write and exit), and a deliberate load from a kernel-only page faults while the kernel prints it and survives (`tools/checks/arm64-m3-check.py`). Next: more than one program, per-program address spaces, then the apps.
- [x] [Fable] M4 USB keyboard and mouse, 2.13.0: a polled xHCI driver (`arch/arm64/xhci.c`) behind a small PCIe enumerator (`arch/arm64/pci.c`). It walks a hub, addresses the keyboard behind it and turns boot-protocol reports into key codes and pointer moves, each key echoed on screen. Proven under QEMU with the controller behind a PCIe root port and the keyboard behind a hub, the Pi's own shape (`tools/checks/arm64-usb-check.py`).

### Known limits to recheck
- [x] [Haiku] PR 387 (the portfolio demo starts at once and tours the launchpad) landed in 2.6.15, and no branch for it is left.

## Toward 2.0: apps leave the kernel
- [x] [Fable] Step one, 1.7.7: Keyrate is the first app running as a real ring-3 process (`user/keyrate.c`, launched by `kernel/ring3app.c`) with its own window through two new syscalls (`SYS_WINDOW_OPEN`, `SYS_WINDOW_POLL`) and real crash isolation: a null write inside it is reaped by the kernel, the window is torn down, the desktop comes back. Proven by `tools/checks/ring3app-check.py`. Not 2.0 yet.
- [x] [Fable] Step two, 1.7.11: Toroid runs at ring 3 (`user/toroid.c`, bit-packed grids in its own .data), `kernel/ring3app.c` is one table-driven launcher (`RING3_APPS`), and the in-kernel copies of both Keyrate and Toroid are deleted. `tools/checks/ring3toroid-check.py` proves it draws, closes both ways, crashes safely.
- [x] [Opus] Step three, 1.7.12: Calculator runs at ring 3 (`user/calculator.c`, the same recursive-descent grammar evaluated straight into a double instead of an expr_node tree, since a flat binary has no .bss and no kmalloc), still one row in `RING3_APPS`, and its in-kernel copy is deleted. `tools/checks/ring3calc-check.py` proves it evaluates through the real parser (`12*3 = 36`, `5/0 = 0`), draws, closes both ways, crashes safely.
- [x] [Opus] Step four, 1.7.14: Quotes runs at ring 3 (`user/quotes.c`, the same fixed deck and answer-rotation, streak and best kept in its own `.data`), still one row in `RING3_APPS`, and its in-kernel copy is deleted. `tools/checks/ring3quotes-check.py` proves it draws the option grid, answers right and wrong through the real logic, closes both ways, crashes safely.
- [x] [Opus] Step six, 1.9.1: Lexly runs at ring 3 (`user/tonchi.c`, the same 30-word deck and drill), one more row in `RING3_APPS`, and its in-kernel copy is deleted. `tools/checks/ring3tonchi-check.py` proves it opens, draws, scores keys, crashes safely and leaves the desktop alive.
- [x] [Opus] Step seven, 1.9.2: Plan runs at ring 3 (user/plan.c, removed in 2.0.0 with the app, the same five milestones and two-pane layout), one more row in `RING3_APPS`, and its in-kernel copy is deleted. tools/checks/ring3plan-check.py (removed with it) proved it opens, draws, selects by key and click, crashes safely and leaves the desktop alive.
- [x] [Opus] Step eight, 1.9.3: Fieldbook runs at ring 3 (`user/fieldbook.c`, the same twelve fields and two-pane layout), one more row in `RING3_APPS`, and its in-kernel copy is deleted. `tools/checks/ring3fieldbook-check.py` proves it opens, draws, selects by key and click, crashes safely and leaves the desktop alive.
- [x] [Opus] Step nine, 1.9.4: Clock runs at ring 3 (`user/clock.c`, the same time, timer and alarm, reading `SYS_TIME`), one more row in `RING3_APPS`, and its in-kernel copy is deleted. `tools/checks/ring3clock-check.py` proves it opens, draws the moving time, takes a timer, closes on Esc and leaves the desktop alive.
- [x] [Opus] Step ten, 1.9.5: Portfolio runs at ring 3 (`user/portfolio.c`, the same About block and fleet catalog), one more row in `RING3_APPS`, and its in-kernel copy is deleted. `tools/checks/ring3portfolio-check.py` proves it opens, draws, selects by key and click, closes on Esc and leaves the desktop alive.
- [x] [Opus] Step eleven, 1.9.6: Activity runs at ring 3 (`user/activity.c`, the same task list, memory line and Kill button), one more row in `RING3_APPS`, one new syscall (`tasks`, 386) for the scheduler and memory numbers, and its in-kernel copy is deleted. `tools/checks/ring3activity-check.py` proves it opens, draws, refreshes on its own, selects, has the kernel refuse to kill the shell, closes on Esc and leaves the desktop alive.
- [x] [Opus] Step twelve, 1.9.7: Contacts runs at ring 3 (`user/contacts.c`, the same list, add prompt, person view and delete, `CONTACTS.TXT` rewritten through the existing file calls), one more row in `RING3_APPS`, and its in-kernel copy is deleted. `tools/checks/ring3contacts-check.py` proves it opens, draws, adds and deletes through the real prompt, keeps the file across fresh runs, closes on Esc and leaves the desktop alive.
- [x] [Opus] Step thirteen, 1.9.8: Sparkjar runs at ring 3 (user/sparkjar.c (now `user/hikko.c`), the same ranked idea list with upvote and re-sort, votes kept for the run only, no new syscall), one more row in `RING3_APPS`, and its in-kernel copy is deleted. tools/checks/ring3sparkjar-check.py (now `tools/checks/ring3hikko-check.py`) proves it opens, draws, moves the selection, re-sorts on the upvote that passes the leader, closes on Esc and leaves the desktop alive.
- [x] [Opus] Step fourteen, 1.9.9: Reminders runs at ring 3 (`user/reminders.c`, the same checklist with add prompt, tick and delete, `REMINDERS.TXT` rewritten through the existing file calls), one more row in `RING3_APPS`, and its in-kernel copy is deleted; Samantha's reminder tools keep working by reading the file fresh on each call. `tools/checks/ring3reminders-check.py` proves it opens, draws, adds, ticks and deletes through the real keys, keeps the file across fresh runs, closes on Esc and leaves the desktop alive.
- [x] [Fable] Step fifteen, 1.9.11: Curbfind runs at ring 3 (`user/curbfind.c`, the same ranked deal list, detail pane and Vancouver samples), one more row in `RING3_APPS`, one new syscall (`http_get`, 387) for the live rows with the host fixed kernel-side and the path checked before the network is touched, and its in-kernel copy is deleted. `tools/checks/ring3curbfind-check.py` proves it opens, draws, falls back to the samples with no NIC, selects by key and click, has every bad path and pointer refused, closes on Esc and leaves the desktop alive.
- [x] [Opus] Step sixteen, 1.9.12: Calendar runs at ring 3 (`user/calendar.c`, the same Day, Week, Month and Year views and one event per day, today from `SYS_TIME` with the date math in the program, `EVENTS.TXT` rewritten through the existing file calls), one more row in `RING3_APPS`, and its in-kernel copy is deleted; Samantha's calendar tool keeps working by reading the file fresh on each call. `tools/checks/ring3calendar-check.py` proves it opens, draws the grid, saves an event through the real editor, keeps the file across a fresh run, answers Samantha, closes on Esc and leaves the desktop alive.
- [x] [Fable] Step seventeen, 1.9.13: Search runs at ring 3 (`user/search.c`, the same query box, live filter, folder step-in and file view), one more row in `RING3_APPS`, one new syscall (`readdir`, 388) that lists a directory into fixed-size records by a path relative to the shell's directory, and its in-kernel copy is deleted. The app keeps its own cwd string, so stepping into a folder no longer moves the kernel's cwd; `open` takes the same relative paths. `tools/checks/ring3search-check.py` proves it opens, filters as you type, shows a file's real bytes, steps into a FAT folder and back out with the kernel's cwd untouched, has the kernel refuse a bad pointer, an over-long path and a missing folder, closes on Esc, crashes safely and leaves the desktop alive.
- [x] [Sonnet] Phone visitors can type to the landing demo: phones get a chat bar and a Send button under the demo, and what is typed shows live in the kernel's own input line. Before, Samantha, Notes and Terminal took typed keys and a phone had no way to send any. `tools/checks/mobile-type-check.mjs` types on an iPhone profile and proves the letters reach Samantha's screen once each, a correction sends Backspace and Send sends Enter.
- [x] [Sonnet] Landing QA fixes from the desktop pass: the demo no longer traps a keyboard user (Tab walks past it, Enter takes it over, Shift+Escape gives it back, and it shows a focus ring), the page has one `<main>` landmark, the version eyebrow is a paragraph not a heading, canonical and social meta are set, a stray sentence under the Strata render is gone, and on phones the Full screen button moved top right with an icon. `tools/checks/landing-demo-ui-check.mjs` proves each one and fails on the old page.
- [x] [Sonnet] Landing QA fixes from the phone pass: Full screen and Send are icons, tapping the chat bar puts the demo in full screen and the screen shrinks to fit above the keyboard so she stays in view, a phone on its side gets a real demo instead of a 128x72 strip, an iPad gets the chat bar, and the waitlist fields are 44px. Covered by `tools/checks/landing-demo-ui-check.mjs` and `tools/checks/mobile-type-check.mjs`.
- [x] [Sonnet] Landing reads in plain words, and the developer specs moved, not went away: benchmarks, the growth chart, the documented percent, architecture and build facts sit in collapsed Tech specs accordions near the bottom. The footer is now a four-column directory like apple.com. Covered by `tools/checks/landing-specs-footer-check.mjs`, and `tools/checks/landing-layout-check.mjs` opens every accordion before it measures.
- [x] [Sonnet] Crash check: one check crashes every ring-3 app on purpose and proves the desktop survives each one. Contacts, Sparkjar, Reminders, Curbfind and Calendar gained the backquote crash key the other twelve already had. `tools/checks/ring3crash-all-check.py` reads `RING3_APPS`, so a new app is covered automatically, and checks that the task is reaped, the window is gone, the dock is drawn and Mail still opens (all 18 apps).

## Architecture to A+ (refreshed 2026-09-27)
- [x] [Sonnet] An Activity Monitor app over the shell's `ps`/`kill`/`mem`. Shipped in 1.9.6, at ring 3.

## Desktop and apps
- [x] [Sonnet] Files is now Burrow, with a kit fox peeking out of its burrow for an icon. Samantha still opens it for "files" and "file browser". `tools/checks/burrow-rename-check.py` proves the name, the aliases and the new icon art.

## Top of the queue after 2.0.0
- [x] Restore the full-bleed Joshua face in ring-3 portfolio mode (2.5.1, user/samantha.c draws the 320 px frame full screen with a glass bar; the kernel gives the portfolio chat a frameless full-screen window).

## Moved from main during the 2026-10-06 merge
- [x] [Sonnet] A quiet Steve Jobs line on the Pi desktop, 2.12.5: "Steve Jobs, 1955 to 2011. Thank you." sits just above the dock, added on the 15th anniversary. `tools/checks/arm64-m1c-check.py` looks for it.
- [x] [Joshua] M4 USB on the real Pi 4, 2026-10-06: booted the 2.13.0 card with a NuPhy keyboard on a data cable; PCIe, the VL805 firmware load, xHCI, the hub and the keyboard all came up first try and keys printed on screen (`docs/RASPBERRY-PI.md`).
- [x] [Joshua] M4 USB on the real Pi 4, second try (2.13.2): the first boot got through PCIe, the VL805 firmware and the hub but saw nothing behind it. This build prints one status line per hub port and rescans every second, so plug the keyboard in (wired mode, lit up) before or after boot and photograph the screen: `usb hp N st .. ch ..` says whether any port saw a connection. Superseded: the first boot with a real data cable already found the keyboard.
- [x] [Fable] USB scan fix, 2.13.2: hub ports are polled for up to 1.5 s after power, then reset and enumerated, and every root and hub port is rescanned once a second (hot-plug and unplug). Any boot-protocol keyboard or mouse interface of a composite device is used, a stalled request no longer wedges endpoint 0, and the Pi build polls without sleeping. `tools/checks/arm64-usb-check.py` plugs keyboards in after boot and fails without the rescan.
- [x] [5.0] Claude app, phase 1, 2.14.0: type a question in the Claude app and Claude Code answers. It runs on the Mac behind a small token-gated relay with read-only tools; Joshua Tree is the front end and never holds the token. How to run it and the security model: `docs/CLAUDE-APP.md`. Checks: `tools/checks/claude-relay-check.py`, `tools/checks/ring3claude-check.py`.
- [x] [Fable] USB on the real Pi, 2.13.0: xHCI with hub support, proven in QEMU, then the BCM2711 PCIe and VL805 setup on the board. 2026-10-06: a NuPhy keyboard enumerated behind the VL805 hub and keys arrived on screen on the first boot with a data cable (`docs/RASPBERRY-PI.md`). Mouse untested.

## Moved from main, 2026-10-07
- [x] [Fable] Console scrollback, 2.19.0: Page Up, Page Down, Home and End scroll the Console over the whole boot log, the title bar says which lines are shown, and a pinned row keeps the latest `wifi` and `usb` line in every photo. `tools/checks/arm64-console-scroll-check.py`.

## Moved from main, 2026-10-07 (IP stack and others)
- [x] [Fable] ARM IP stack, 2.20.0: the i386 kernel's own `drivers/net.c` and `drivers/http.c` now build for ARM on a four-call card interface (`drivers/nic.h`), so Wi-Fi only has to fill it in. On QEMU's virtio-net it leases 10.0.2.15 by DHCP and POSTs to the host. Check: `tools/checks/arm64-net-check.py`.

## Moved from the errors-tests branch, 2026-10-07
- [x] [Fable] Crash screen, 2.17.0: an unexpected exception at EL1 now prints the class, ESR, FAR, ELR and the last console lines on the UART, draws them as a red panel on the screen (integer 8x16 font, cleaned out of the cache for the GPU) and halts in a `wfe` loop, instead of one line and a silent hang. `tools/checks/arm64-crash-check.py` crashes a test build on purpose and reads the UART and a screendump, on virt and the Pi 4B model.
- [x] [Fable] FP state across interrupts, 2.17.0: `vectors.S` saves and restores q0-q31, FPCR and FPSR on every IRQ. `tools/checks/arm64-fp-check.py` holds known values across timer interrupts whose handler wipes them, and shows the same test failing on a build without the save.
- [x] [Sonnet] Out of memory on ARM, 2.17.0: the ARM framebuffer allocation had no null check; it does now, and a full heap prints `oom fb`, `oom text`, `oom vq`, `oom net` or `oom input` and fails only that step (the 8x16 font stands in for a font that did not load). `tools/checks/arm64-oom-check.py` shrinks the heap on purpose. Still open: `arch/arm64/xhci.c` has one unchecked `kmalloc`, left for the USB PRs that own that file.
- [x] [Sonnet] Boot health, 2.17.0: `tools/checks/arm64-boot-health-check.py` boots the Pi image on QEMU and fails on any `FAIL` line not in its short expected list, any `oom`, a crash report, or a missing desktop, so a new failing step cannot slip in unseen.
## Moved from the desktop branch, 2026-10-07
- [x] [Sonnet] Two suites at once corrupted each other, found 2026-10-06 (feature-drive, menuclock, ring3crash-all, qa-gallery and soak failed at random on truncated dumps). `tools/ci-local.sh` now takes a one-at-a-time lock, those checks use private temp dirs, `tools/checks/tmp-paths-check.py` stops new fixed paths, and the Samantha caption-fade check steps her clock instead of polling the host's (`tools/checks/samantha-fullscreen-check.py`). The 122 older checks still on fixed paths are listed in `tools/checks/tmp-paths-baseline.txt`; the lock covers them until each is converted.

## Reviewed against main, 2026-10-09

The 2.33.0 core integration merged the Terminal prompt, bounded Samantha agent loop with Esc/stop, browser navigation, release-token gate and Calendar icon. The latest keyboard shortcuts are in 2.33.2. Checks and hardware limits are in `docs/AGENT.md`, `docs/TERMINAL.md` and `docs/RASPBERRY-PI.md`. The older status notes below are historical, not the current queue.

The short list. Everything below is the long list; done items moved to [roadmap-done.md](roadmap-done.md).

**Done on the Pi.** Wi-Fi joins with our own WPA2, the clock sets itself from the net, the menu bar shows the three bars and the clock, the console is quiet, the boot screen shows, the green light blinks once at boot, Samantha answers and acts ([[note TEXT]] prints a note, [[led blink]] blinks the light). First benchmark on the board: memcpy 1109 MB/s, alloc 30 ns, Wi-Fi joined in 6.4 s, clock set in 8.7 s.

**Shipped 2026-10-08 (main is 2.32.1).** HTTPS through BearSSL (TLS 1.2) and a text browser at the `ask>` row (`browse URL`, `open N`, PR 476); Joshua confirmed `browse https://heyitsmejosh.com` on the board with `dns ok`, `tcp ok`, `tls ok`, `http 200`. The Calculator on ARM (PR 475, `tools/checks/arm64-calc-check.py`). A tiny local model in the kernel, `llm PROMPT`, with no weights baked into release builds (PR 477, `tools/checks/arm64-llm-check.py`). A browse that fails fast with progress lines instead of sticking on fetching, and the DNS stub so the fuzz harness links (PR 479, `tools/checks/arm64-browser-deadserver-check.py`). Not merged: the prompt naming the model it used (no branch yet), and the relay token kept to dev builds only (branch `token-gate`, 2.32.2).

**Mac-style USB keyboard (2026-10-09).** On the board, F1, F2 and Cmd or Ctrl+Space did nothing. Cmd was never tracked, report-ID reports lost their modifier byte, and a Mac-mode top row sends media keys on an interface we never opened. Now all three are handled, any letter on the bare desktop opens Spotlight, and the dev card shows raw HID bytes in the menu bar. Check: `tools/checks/arm64-keydbg-check.py`.

**Keyboard fix, 2026-10-09.** Spotlight shortcuts close it when pressed again, and F2 or Ctrl+T opens Terminal even while search is open. Search debug messages stay off the Console so they cannot repaint through the overlay. The keyboard-only check verifies these paths and that closing search restores the screen.


- [x] [Haiku] Calendar icon fix, if not yet merged. The Pi tile got its face in 2.26.0; confirm the other places the icon shows. Check: `tools/checks/check-calendar.sh` and the icon margin check.
- [x] [5.0] Claude on the real Pi: the same Console prompt, once Wi-Fi stage 2 joins a network. Today it says `claude: no network`.
- [x] [Fable] Wi-Fi stage 1: the CYW43455 wakes up over SDIO, loads its firmware and lists the networks on screen. In progress, 2.16.0.
- [x] [Fable] `browse URL` on the real Pi stuck on "fetching". Two causes, both in the code path, neither a `ticks` wait: `drivers/http.c` sent every DNS query to SLIRP's 10.0.2.3, which is nobody on a real LAN (now the DHCP lease's DNS server), and each stage waited its own 20 s (DNS, SYN-ACK, 15 s per TLS read), so a dead hop read as a hang. The browser now gives DNS 3 s, the connect 4 s and each read 8 s, and prints `dns ok`, `tcp ok`, `tls ok`, `http NNN` as it goes, so the next board test shows where it stops. Check: `tools/checks/arm64-browser-deadserver-check.py`. Still to prove on the board: that BearSSL finishes a handshake against a real site within the budget.
- [x] [Fable] The Pi browser is usable: `back`, `forward` (8 pages), `reload`, `links`, `more`/`up`/`down`/`top`/`bottom`, `find WORD`, `help`, a status line (URL, line X of Y), `&#NN;` entities, blank lines before headings, `text [3]` links, one clear line per failure. All commands go through one `cmd_run(line, sink)` in `arch/arm64/cmd.c`, so a Terminal app can take the input later; the Console keeps the ask> row for now. Check: `tools/checks/arm64-browser-check.py`.
- [x] [Fable] Wi-Fi stage 2: join works on the real board (our own WPA2, `arch/arm64/wpa.h`; name and key from `~/.config/joshuatree/wifi.conf`, never the repo). DHCP and the clock code landed in c14ac464, untested on the board. The IP stack ported from `drivers/net.c`.
- [x] [Sonnet] A clock that is right: the Pi has no battery clock, so take the time from the network once Wi-Fi joins (SNTP), and show it in the menu bar.
- [x] [Sonnet] A crash screen: when the kernel faults, draw the fault address, the last console lines and the register dump on screen instead of freezing.
- [x] [Fable] Console text on the real Pi, fixed in 2.12.5, waiting on a photo: the boot lines showed as one thin mark each. Cause: the screen is cached memory and the GPU only sees what the kernel cleans out of the cache. The console wiped its full page after the one clean at startup, and nothing after that left the cache, so only two pixel columns of old text survived. Now every glyph and every wipe is cleaned, the console scrolls, and a diagnostic line ends the log. Tick it when a photo shows the text. Check: `tools/checks/arm64-m1c-check.py` fails if a draw or a wipe stops cleaning.
- [x] [Joshua] M4 USB on the real Pi 4: boot the 2.13.0 SD card with a keyboard in, photograph the screen. The Pi's PCIe bring-up and the VL805 firmware load are written from Circle and Linux but have never run on a board; every step prints a short `usb ...` line, so the photo shows how far it got.
- [x] [Fable] Wi-Fi on the Pi 4 (CYW43455 over SDIO). Stage 1 (list networks) is built, waiting on a photo from the real board; stage 2 join works on the board; IP is next. Ethernet first for everything else.
- [x] [Sonnet] Boot straight into Samantha: a kernel command-line flag (`samantha`) opens her full screen after boot.
- [x] [Haiku] Clock app with timer and alarm.
- [x] [Sonnet] The Terminal hosts the agent and commands; the Console is logs only. Merged in 2.33.0. Per-pane model settings are still open in the roadmap.
- [x] [Fable] Samantha works as an agent in Joshua Tree: actions open, browse, calc, say, read, list, status; up to 5 steps, 4 actions per turn, with a kill switch.
- [x] [Sonnet] Browser v2 on the Pi: a working browser controllable from Console and Terminal. Pages with heavy scripts still show little text, because the Pi does not run JavaScript.

## 2026-10-09: relay file safety, CI balance and first guide chapter

The relay now opens shared files relative to a pinned directory, refuses symlinks at open time and refuses non-regular files without blocking. Regression checks replace a listed file with a symlink, FIFO or missing file. CI was rebalanced using all 215 timings from green run 37983553844, with no checks removed and the two-job local limit unchanged. The Wi-Fi chapter in `docs/GUIDE.md` explains setup, commands, failures and the files behind them; the remaining guide chapters and coverage guard stay open.

### Evening improvements (2.34.0)
- [x] [Sonnet] ARM Terminal has two sessions, switched with F3, with separate model, effort, conversation, typed line and scrollback. A running request cannot move sessions. Check: `tools/checks/arm64-model-check.py` uses a stub relay to prove separation and the busy gate. Side-by-side layout remains open.
- [x] [Sonnet] Calendar Year view uses small antialiased date numbers instead of dots at the normal window size. Check: `tools/checks/calviews-check.py` verifies distinct two-digit shapes in all twelve months and keeps the Today highlight and other views checked.

## 2.35.0: Pi date and Clock

Calendar now shows a month and large day number, including December. Clock opens from Spotlight and shows Vancouver time and date, or waits for network time. Samantha’s dock tile opens the assistant Terminal. Check: `tools/checks/arm64-calicon-check.py`; real-board verification remains.
## October 9 keyboard repair

**Mac-style USB keyboard (2026-10-09).** On the board, F1, F2 and Cmd or Ctrl+Space did nothing. Cmd was never tracked, report-ID reports lost their modifier byte, and a Mac-mode top row sends media keys on an interface we never opened. Now all three are handled, any letter on the bare desktop opens Spotlight, and the dev card shows raw HID bytes in the menu bar. Check: `tools/checks/arm64-keydbg-check.py`.
**Keyboard fix, 2026-10-09.** Spotlight shortcuts close it when pressed again, and F2 or Ctrl+T opens Terminal even while search is open. Search debug messages stay off the Console so they cannot repaint through the overlay. The keyboard-only check verifies these paths and that closing search restores the screen.

## 3.5.0: the Bluetooth mouse remembers

The pairing key goes to `JT/BTPAIR.BIN` and a reboot reconnects the mouse without scanning or pairing again (`bt forget` clears it, `bt reconnect` retries). Wi-Fi is held for the reconnect as for a pairing.
Check: `tools/checks/bt-pair-check.py` and `tools/checks/arm64-bt-check.py`; the radio side needs the real board.

## 3.6.0: drag a window by its title

A press on the title band (not the dots) moves the Console, the Terminal, Clock, Brick or the Browser: `win_move_to` in `arch/arm64/main.c` copies the pixels to the new place, fills what it uncovered with the flat wallpaper and keeps the window between the menu bar and the dock. `con_under` is gone, closing fills the wallpaper where the window now is.
Check: `tools/checks/arm64-drag-check.py`. Left: the Calculator and the app windows (Calendar, Notes, Music, Books) keep their places, and the Console is not dragged while one of them is open.

## 3.6.1: the Browser stops flickering on the Pi

The Pi's framebuffer is the live scanout, so clearing the well and drawing the text back in place showed every half-drawn state. `browser_paint` now draws the whole window into an offscreen buffer (`off_begin`, `off_end` in `arch/arm64/main.c`) and copies it to the screen in one top-to-bottom pass.
Check: `tools/checks/arm64-browser-flicker-check.py` (the `brtrace` build proves the screen is untouched until the copy). Left: the Clock, Brick, Console and Calculator still repaint in place.

## 3.6.2: reboot works after Samantha answers

On the Pi, `reboot` and `shutdown` answered "Wait for Samantha to finish, then try again." every time. Root cause: the refusal asked `ask_pending()`, which is also true while the typed line holding the command runs, so the command refused itself. They now refuse only while a `sam` request is live (`ask_busy()`), in both the Pi and test builds, so an error reply, a timeout or Esc leaves them working.
Check: `tools/checks/arm64-zsh-check.py` refuses reboot mid-question, then after a relay error runs `reboot` and `shutdown` and answers a second `sam`; it fails if the fix is reverted.

## 3.7.0: the mascot on empty Music and Books

On the Pi build, an empty Music library and an empty Books shelf draw the mascot (`arch/arm64/mascot_art.h`, the same art as the Browser's empty well) above a plain caption: "No songs yet" and "No books yet". The i386 apps are unchanged.
Check: `tools/checks/arm64-mascot-empty-check.py` boots both apps on a card with no songs or books and reads the mascot's pixels out of a screendump; it fails if the draw is reverted.

## 3.7.2: pointer motion over a long Browser page

Nothing in the pointer path paints the Browser, so the board glitch on a Wikipedia page is not a repaint per motion event; what can repaint it is a wheel event, and a Bluetooth mouse report with extra bytes reads as wheel notches. `input_event` now sums wheel notches per report, `browser_wheel` skips the paint when the page is already at that end, and `br_wrap` keeps its lines while text and width are unchanged.
Check: `tools/checks/arm64-browser-mouse-check.py` (120 pointer reports over a 300-row page: zero Browser paints, zero layouts, screen identical). Left: the real board needs another try.

## 3.7.4: a clean desktop at boot

## 3.7.4: a splash instead of the Console at boot

The Pi boots behind a terracotta screen with the tree mark and a thin line that fills as the screen, card, Wi-Fi, clock and Bluetooth finish (`splash_step` in `arch/arm64/main.c`); the desktop paints once after `boot done`, and the Console never shows at boot. The log is still kept and a dock click opens it. Wi-Fi fan: flat ink when joined, dimmed grey when not.
Check: `tools/checks/arm64-quietboot-check.py` (a screendump per stage, the desktop only after `boot done`). Was: the Pi no longer draws the Console at boot. It showed for the whole Wi-Fi bring-up and stayed on any FAIL line, over the terracotta desktop. Now the log is only kept (`QUIET_BOOT`) and a dock click opens it. The Wi-Fi fan is flat ink when joined and dimmed grey when not, where it was a blue-grey.
Check: `tools/checks/arm64-quietboot-check.py` (and `arm64-boot-quiet-check.py` on the Pi image).
