# Joshua Tree, version by version

Where this goes, one major version at a time. Up to 10 it's a plan: each version is one promise kept, and the roadmap holds the steps. Past 10 it's a direction, not a schedule. The loop works top down and moves this file as versions ship.

## 1.x: the base (now)

- 1.7: Samantha on your phone. The app interface. Mail, notes and reminders through Samantha. The Epiphany command bar.
- 1.8: A phone home screen: an app grid, one app full screen at a time, a back button.
- 1.9: Touch, an on-screen keyboard, and every app readable at phone size.

## The plan to 10

- [x] 2.0: Apps leave the kernel. Each app is its own protected program, so one crash can't take the machine down. Done, shipped as 2.0.0.
  - 2.0.0: every app is its own protected program, all twenty-six of them, and each one is a real window. Keys and clicks go to the focused window only, a crash closes that window and nothing else, and you can copy and paste between any of them. Windows resize, apps can grow their own memory, and type is smooth everywhere. Mail really sends now. There is a new mark.
  - 1.7.7 took the first step: Keyrate runs as a real ring-3 process with its own window, and crashing it on purpose leaves the desktop standing.
  - 1.7.11: two apps in ring 3 now. Toroid followed Keyrate out, the in-kernel copies of both are gone, and one table-driven launcher runs them. Still to do before this line is checked: the other apps, ported the same way, one PR at a time.
  - 1.7.12: three apps in ring 3 now. Calculator followed Keyrate and Toroid out, the in-kernel copy is gone, and the same table-driven launcher runs it. Still to do before this line is checked: the other apps, ported the same way, one PR at a time.
  - 1.7.14: four apps in ring 3 now. Quotes followed Keyrate, Toroid and Calculator out, the in-kernel copy is gone, and the same table-driven launcher runs it. Still to do before this line is checked: the other apps, ported the same way, one PR at a time.
  - 1.7.15: fixed a Linux-CI-only feature-drive.py bug that Quotes exposed, not caused: the check's own window-close retry could double-click into the Apps folder behind a self-closing app and quit the whole GUI to the text shell, failing every app tested after it.
  - 1.8.22: six apps in ring 3 now. Homeqi followed Keyrate, Toroid, Calculator, Quotes and Bookrank out, the in-kernel copy is gone, and the same table-driven launcher runs it.
  - 1.9.1: seven apps in ring 3 now. Lexly followed Keyrate, Toroid, Calculator, Quotes, Bookrank and Homeqi out, the in-kernel copy is gone, and the same table-driven launcher runs it.
  - 1.9.2: eight apps in ring 3 now. Plan followed Keyrate, Toroid, Calculator, Quotes, Bookrank, Homeqi and Lexly out, the in-kernel copy is gone, and the same table-driven launcher runs it.
  - 1.9.3: nine apps in ring 3 now. Fieldbook followed Keyrate, Toroid, Calculator, Quotes, Bookrank, Homeqi, Lexly and Plan out, the in-kernel copy is gone, and the same table-driven launcher runs it.
  - 1.9.4: ten apps in ring 3 now. Clock followed Keyrate, Toroid, Calculator, Quotes, Bookrank, Homeqi, Lexly, Plan and Fieldbook out, the in-kernel copy is gone, and the same table-driven launcher runs it.
  - 1.9.6: twelve apps in ring 3 now. Activity followed Keyrate, Toroid, Calculator, Quotes, Bookrank, Homeqi, Lexly, Plan, Fieldbook, Clock and Portfolio out, the in-kernel copy is gone, and it needed one new syscall, `tasks`, for the scheduler and memory numbers.
  - 1.9.7: thirteen apps in ring 3 now. Contacts followed Keyrate, Toroid, Calculator, Quotes, Bookrank, Homeqi, Lexly, Plan, Fieldbook, Clock, Portfolio and Activity out, the in-kernel copy is gone, and it kept `CONTACTS.TXT` through the file calls it already had, no new syscall.
  - 1.9.8: fourteen apps in ring 3 now. Sparkjar followed Keyrate, Toroid, Calculator, Quotes, Bookrank, Homeqi, Lexly, Plan, Fieldbook, Clock, Portfolio, Activity and Contacts out, the in-kernel copy is gone, and its votes stay session-only with no new syscall.
  - 1.9.9: fifteen apps in ring 3 now. Reminders followed Keyrate, Toroid, Calculator, Quotes, Bookrank, Homeqi, Lexly, Plan, Fieldbook, Clock, Portfolio, Activity, Contacts and Sparkjar out, the in-kernel copy is gone, and it kept `REMINDERS.TXT` through the file calls it already had, no new syscall. Samantha's reminder tools read the file fresh on every call.
  - 1.9.11: sixteen apps in ring 3 now. Curbfind followed Keyrate, Toroid, Calculator, Quotes, Bookrank, Homeqi, Lexly, Plan, Fieldbook, Clock, Portfolio, Activity, Contacts, Sparkjar and Reminders out, the in-kernel copy is gone, and it needed one new syscall, `http_get`, for its live deal rows: the host is fixed in the kernel, a program names only the path, and a bad path or pointer is refused before the network is touched.
  - 1.9.12: seventeen apps in ring 3 now. Calendar followed Keyrate, Toroid, Calculator, Quotes, Bookrank, Homeqi, Lexly, Plan, Fieldbook, Clock, Portfolio, Activity, Contacts, Sparkjar and Reminders out, the in-kernel copy is gone, today comes through `SYS_TIME` with the date math done in the program, and it kept `EVENTS.TXT` through the file calls it already had, no new syscall. Samantha's calendar tool reads the file fresh on every call.
  - 1.9.13: eighteen apps in ring 3 now. Search followed Keyrate, Toroid, Calculator, Quotes, Bookrank, Homeqi, Lexly, Plan, Fieldbook, Clock, Portfolio, Activity, Contacts, Sparkjar and Reminders out, the in-kernel copy is gone, and it needed one new syscall, `readdir` (388), which lists a directory into fixed-size records by a path relative to the shell's directory, walked and walked back inside the call, so a ring-3 app can step into folders without moving the kernel's own cwd. `open` takes the same relative paths now, so a file inside a folder opens as `DOCS/NAME`.
  - 1.9.14: the landing page got a QA pass (keyboard focus, phone chat bar, icon buttons), plain-words copy with the developer numbers in collapsed Tech specs, and a four-column footer. One check now crashes every ring-3 app on purpose (18 of 18) and proves the desktop survives each one; Contacts, Sparkjar, Reminders, Curbfind and Calendar gained the backquote crash key. The Files app is now Burrow, with a kit-fox icon; Samantha still answers to "files".
  - 1.9.19: nineteen apps in ring 3 now. Epiphany followed Search out, the in-kernel copy (`kernel/epiphany.h`) is gone, and it reuses `http_get` (387) for its `/api/quotes` prices with the offline sample prices when the fetch fails. The GP chart can no longer read the kernel's Stocks series, so it draws previous close to last price for any of the 40 tickers and says so under the chart.
  - 1.9.26: Samantha is a ring-3 program now, the last app out of the kernel: twenty-six of twenty-six. Her voice, face, tools and listening work through small syscalls for audio, a bigger HTTP call, system info and asking the desktop to open an app, and apps can grow their own memory. The dock, the menu, the `chat` and `samantha` shell commands and phone mode all open the one ring-3 program, and the in-kernel chat is gone.
  - 1.9.25: twenty-five of twenty-six apps run in ring 3. Notes and Terminal left the kernel; only Samantha is still in it. Notes keeps its folders and its phone keyboard. Terminal runs an allowlisted set of shell commands through one new syscall and keeps its own folder for cd. Closing a ring-3 window no longer crashes the desktop.
  - 1.9.24: twenty-three of twenty-six apps run in ring 3. Burrow, Stocks and Mail left the kernel. Every ring-3 app now opens as a real window beside the others, except Weather and Stocks, which still close and relaunch to fetch fresh data. The kernel's user window moved up 512 KB so apps have room to grow, and Mail's compose is now an inline sheet inside the window. Keyrate, Clock, Calculator and Homeqi no longer close when you click inside them.
  - 1.9.23: ring-3 programs can be real windows. Reminders opens as a compositor window beside Notes without blocking the desktop: it gets its own image and framebuffer, mapped only into its own page directory, presents by marking the window dirty, and takes keys and clicks from a per-window queue the desktop fills only for the focused window. A crash closes that window and nothing else. The blocking launch is still there for the other nineteen, so nothing moved that was not asked to. Concurrency closed behind it: cli sections around kheap, vfs, the window event ring and the launcher (kernel/irqlock.h), syscall paths resolve from the root, one HTTP fetch at a time, and ring3stress-check.py proves the heap survives both sides hammering it. Ring-3 apps also get smooth antialiased type now (a small DejaVu renderer in libjt), starting with Weather.
  - 1.9.22: twenty apps in ring 3 now. Weather left the kernel as `user/weather.c`. The kernel still does the fetching (menu bar, wind sway, phone home and Samantha read the same fields) and leaves a small `WEATHER.TXT` after every fetch, which the app reads through plain file calls. R shows Fetching, the app exits 7 and the kernel fetches and starts it again, so there is no new syscall. The window shows the live reading, the last good one, or labelled sample data, same as before, drawn in the 8x16 system font.
  - 1.9.21: Weather no longer holds up any window check. Multiwindow, window snap, the app switcher, title bar AA and text sharpness all open Notes as the second window now, which clears the way for Weather to leave the kernel. Notes browse stopped drawing its labels on blue boxes, and the Clock icon in the Dock is a live analog face.
  - 1.9.5: eleven apps in ring 3 now. Portfolio followed Keyrate, Toroid, Calculator, Quotes, Bookrank, Homeqi, Lexly, Plan, Fieldbook and Clock out, the in-kernel copy is gone, and the same table-driven launcher runs it.
- [x] 2.1: Music. A player app in its own protected space: WAV first, then MP3 through a small public-domain decoder, a library from Files, play and pause through the sound driver it already has.
- [x] 2.2: Video. A player app: motion-JPEG first (reusing the JPEG decoder the kernel already has) with sound in sync, then a real codec when the hardware allows.
- [x] 2.5: Joshua on the web. Numbered 2.5 on Joshua's call, so the plan's 2.1 Music and 2.2 Video are still open. The portfolio at heyitsmejosh.com is Joshua's own: the chat is titled Joshua, answers in the first person and speaks in his cloned voice, and the page opens on a 28 second lip-synced intro of him before the live OS takes over. Done, shipped as 2.5.0.
  - 2.5.0: the ring-3 Chat app learned portfolio mode again after 2.0 moved it out of the kernel (title, persona and voice follow the portfolio flag); the landing plays the recorded intro; the phone demo waits out a spoken reply before it asks the next question. Still open: the full-bleed face after the intro.
  - 2.5.1: Joshua's face fills the screen again after the intro (the 2.0 ring-3 chat had shrunk it to a 60 px band): the video fades into his live face with a glass chat bar, then the desktop tour runs. The ring-3 window buffer grew to 960x540, so `.dmabuf` and `.userfb` slid down into the gap under the program window.
  - 2.6.0: Music and Video, the plan's 2.1 and 2.2, shipped together. Play WAV and MP3 songs from disk through a small decoder. Play motion-JPEG AVI movies with sound in sync through the JPEG decoder. Both are protected apps that cannot crash the desktop. On Joshua's portfolio: the speaker button now starts the intro video with sound (it only moved the emulator volume before, so a first press on a phone heard nothing), the phone chat bar is a glass overlay on his face instead of a black strip, and the phone title says Joshua.
  - 2.6.1: the ARM64 port starts. `arch/arm64` builds a separate aarch64 kernel that boots under QEMU and prints over the UART (milestone M0 of docs/ARM64.md), and the docs name the Raspberry Pi 4B as the first real board. The i386 kernel is untouched.
  - 2.6.2: the ARM64 kernel now builds for a real Raspberry Pi 4 (`make -C arch/arm64 pi` makes `kernel8.img`): it enters at EL2 like the Pi's firmware does, drops to EL1, sets up the Pi's UART pins and speed, and prints. It boots on QEMU's Pi 4B model; the first real-board boot is waiting on the hardware. New: the Pi setup guide (`docs/RASPBERRY-PI.md`) and a printable Strata Pi case with a fit-test plate (`docs/hardware/PI-CASE.md`).
  - 2.6.3: the Movies check samples the screen again when a slow CI runner catches it between repaints, so it stops failing runs that are fine.
  - 2.6.4: phone fixes. The "Tap to hear" pill is gone: a speaker button in the top right starts the sound and mutes it (it shows sound-off until the first tap), on the landing demo and the portfolio. The landing demo no longer types a stray "n" before each line, and Samantha's face shows as soon as her idle frames load instead of after all 72.
  - 2.6.5: the phone demo pokes around the OS instead of typing into his chat. After the intro video it opens the apps from the home grid, one by one, and ends on him. His face draws off screen and is copied in one pass, so the bubbles no longer tear. His reply floats above the bar while he talks and for six seconds after, then the face is clear. Frame downloads stop while he talks and slow down once his idle loop is in, so the face keeps moving.
  - 2.6.6: a better landing demo. On a phone it starts at once (no wait for a tap) and plays four real beats instead of repeating three questions: her itinerary (a sample trip in the browser demo's calendar, dated from the clock), a reminder, the weather and a note, each followed by opening the app her tool just used. Samantha is a full-screen face on the phone, like Joshua is. Asking for an itinerary, agenda or schedule now reaches the calendar, and a note no longer says note: twice. New docs/PORTFOLIO.md. The HTTP heap check now flags only a fall in free memory, not a rise.
  - 2.6.7: phone QA. The intro video goes straight into the tour (no live face with its eyes shut in between), and while his frames download he holds a still, eyes-open portrait and nothing downloads while he talks, so the blink never freezes. Epiphany lays out for a phone (one full-width watchlist, scaled Portfolio and Simulator columns), and the chat bar stays out of the way while an app is open.
  - 2.6.8: ARM64 M1a. The aarch64 kernel sets its exception table (a fault now prints its cause and address instead of hanging), answers a deliberate system call, sets up the interrupt controller and takes three timer interrupts. Runs on QEMU's generic ARM machine and its Raspberry Pi 4 model; `tools/checks/arm64-m0-check.py` proves both.
  - 2.6.9: the portfolio tour no longer stalls when his face frames are slow. The page used to send Escape once at the end of the intro video; if the chat app was busy downloading frames it missed the key and the visitor sat on a bare desktop. Now the page watches the kernel log for the app to exit and sends Escape again until it does, at every place the tour closes an app. New check: `portfolio-slowframes-check.mjs` drops two Escape presses and holds the frames back.
  - 2.6.10: ARM64 M1b. The aarch64 kernel zeroes its `.bss` on boot (a real board does not clear RAM for it), turns the MMU and both caches on over a flat identity map (device memory for the peripherals, write-back for RAM, different on the Pi than on QEMU's virt machine), and has a small heap. The exception table and the timer keep working with the caches on. Runs on QEMU's generic ARM machine and its Pi 4 model.
  - 2.6.14: Lexly's icon is no longer blue. Its dots were sky blue on the sage tile, so the Apps folder, the dock and Joshua's portfolio all showed a blue icon. The dots now take the tile color. The landing ad's copy is redrawn too.
- 3.0: It boots a real computer. One reference mini PC: UEFI, USB keyboard, mouse and stick, a real disk, a real network card, real sound.
- 3.1: The Strata Kit ships. The $199 case and OS stick for the 3.0 board, bring your own parts (MONEY.md has the math).
- 4.0: True multitasking. A compositor, many windows, resize and minimize, apps that never freeze each other.
- 5.0: The real internet. TLS built in, no proxy, and a web browser on top.
- 6.0: The terminal. Epiphany streams prices, draws candlesticks, runs four panels, a news wire, alerts and P&L.
- 7.0: Modern chips. 64-bit, every core.
- 8.0: Samantha lives inside. Her model runs on the machine, no cloud needed.
- 9.0: It builds itself. A compiler on Joshua Tree, and Samantha fixes her own bugs.
- 10.0: Strata Complete ships. A finished machine with Joshua Tree on it, install to disk, updates.

## 11 to 20: a family of machines

- 11: A laptop: battery, suspend, trackpad.
- 12: Wi-Fi.
- 13: Bluetooth: headphones, keyboards.
- 14: Graphics acceleration on the reference board.
- 15: An ARM port, the second chip family.
- 16: A phone board: cellular modem, cameras.
- 17: External displays and HiDPI everywhere.
- 18: Printers and scanners.
- 19: Power management that beats the laptop's stock OS on battery life.
- 20: Three shipping devices: dev kit, laptop, phone.

## 21 to 30: an ecosystem

- 21: An SDK anyone can build apps with.
- 22: A package manager and updates for every app.
- 23: An app store.
- 24: Accounts and more than one person per machine.
- 25: Backups and restore.
- 26: Sync between your Joshua Tree devices.
- 27: Plugins for the desktop and for Samantha.
- 28: The first hundred apps by other people.
- 29: Localization into the top 25 languages.
- 30: Accessibility complete: screen reader, voice control, high contrast.

## 31 to 40: Samantha grows up

- 31: You speak, she hears. Voice in, not just voice out.
- 32: She sees the screen and understands it.
- 33: She runs every app for you, end to end.
- 34: She remembers you, on the device, private.
- 35: She plans multi-step tasks and checks her own work.
- 36: Her face is a real-time, fully lip-synced video, on the device.
- 37: Custom characters: pick her face and voice, or make your own.
- 38: She learns from your corrections without the cloud.
- 39: Samantha for developers: she writes and ships apps with you.
- 40: Most people use Joshua Tree by talking.

## 41 to 50: trust

- 41: A security audit by outsiders.
- 42: The kernel core formally verified.
- 43: Every app sandboxed with explicit permissions.
- 44: Encrypted disk by default.
- 45: Reproducible builds: anyone can prove the image matches the source.
- 46: A bug bounty.
- 47: Messaging, end to end encrypted.
- 48: Federated services you can host yourself.
- 49: Ten years of updates promised per device.
- 50: Trusted enough for banks and hospitals.

## 51 to 60: pro tools

- 51: The terminal trades, through real brokers.
- 52: Research: fundamentals, filings, screeners.
- 53: A photo editor.
- 54: A music studio.
- 55: A video editor.
- 56: A full IDE.
- 57: A spreadsheet that runs a business.
- 58: A word processor that runs a publisher.
- 59: Design and 3D tools.
- 60: Professionals switch for the tools, not the novelty.

## 61 to 70: scale

- 61: 10,000 devices shipped.
- 62: An education edition for schools.
- 63: Fleet management for companies.
- 64: 100 languages.
- 65: 100,000 devices.
- 66: Manufacturing partners build their own Joshua Tree machines.
- 67: Joshua Tree certified hardware program.
- 68: A foundation to steward the OS.
- 69: A million devices.
- 70: Profitable on hardware alone.

## 71 to 80: new shapes

- 71: The kitchen display: Samantha on the counter.
- 72: The car.
- 73: The watch.
- 74: Glasses.
- 75: One OS across all your devices, handing tasks between them.
- 76: Tiny embedded Joshua Tree for appliances.
- 77: Joshua Tree in space (a satellite or a rover payload).
- 78: A home server edition.
- 79: Accessibility devices built on it.
- 80: Ten form factors, one kernel.

## 81 to 90: it improves itself

- 81: Samantha writes most new code, reviewed.
- 82: Updates proven safe before they ship.
- 83: The OS tunes itself to your hardware.
- 84: Drivers written by Samantha from a chip's datasheet.
- 85: Every bug report fixed by Samantha within a day.
- 86: A verified compiler.
- 87: New apps generated from a sentence.
- 88: The OS explains every decision it makes.
- 89: People customize the OS by asking.
- 90: It keeps getting better on its own.

## 91 to 100: the computer after the phone

- 91: 10 million devices.
- 92: Talking replaces tapping for most people who use it.
- 93: 100 million people.
- 94: Joshua Tree in every classroom that wants it.
- 95: The default OS on a major hardware brand.
- 96: A billion people.
- 97: The trillion dollar plan in MONEY.md, reached.
- 98: Fully open, fully verified, fully yours.
- 99: The next person builds their own OS on it.
- 100: A computer you talk to, that anyone can trust, that anyone can read.
