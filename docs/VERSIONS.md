# Joshua Tree, version by version

Where this goes, one major version at a time. Up to 10 it's a plan: each version is one promise kept, and the roadmap holds the steps. Past 10 it's a direction, not a schedule. The loop works top down and moves this file as versions ship.

## 1.x: the base (now)

- 1.7: Samantha on your phone. The app interface. Mail, notes and reminders through Samantha. The Epiphany command bar.
- 1.8: A phone home screen: an app grid, one app full screen at a time, a back button.
- 1.9: Touch, an on-screen keyboard, and every app readable at phone size.

## The plan to 10

- 2.0: Apps leave the kernel. Each app is its own protected program, so one crash can't take the machine down.
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
  - 1.9.11: sixteen apps in ring 3 now. Calendar followed Keyrate, Toroid, Calculator, Quotes, Bookrank, Homeqi, Lexly, Plan, Fieldbook, Clock, Portfolio, Activity, Contacts, Sparkjar and Reminders out, the in-kernel copy is gone, today comes through `SYS_TIME` with the date math done in the program, and it kept `EVENTS.TXT` through the file calls it already had, no new syscall. Samantha's calendar tool reads the file fresh on every call.
  - 1.9.5: eleven apps in ring 3 now. Portfolio followed Keyrate, Toroid, Calculator, Quotes, Bookrank, Homeqi, Lexly, Plan, Fieldbook and Clock out, the in-kernel copy is gone, and the same table-driven launcher runs it.
- 2.1: Music. A player app in its own protected space: WAV first, then MP3 through a small public-domain decoder, a library from Files, play and pause through the sound driver it already has.
- 2.2: Video. A player app: motion-JPEG first (reusing the JPEG decoder the kernel already has) with sound in sync, then a real codec when the hardware allows.
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
