# Joshua Tree, version by version

Where this goes, one major version at a time. Up to 10 it's a plan: each version is one promise kept, and the roadmap holds the steps. Past 10 it's a direction, not a schedule. The loop works top down and moves this file as versions ship.

## 1.x: the base (shipped, 2026)

- 1.7: Samantha on your phone. The app interface. Mail, notes and reminders through Samantha. The Epiphany command bar.
- 1.8: A phone home screen: an app grid, one app full screen at a time, a back button.
- 1.9: Touch, an on-screen keyboard, and every app readable at phone size.

## The plan to 10

- [x] 2.0: Apps leave the kernel. Each app is its own protected program, so one crash can't take the machine down. Shipped as 2.0.0; every 2.x step since is in `roadmap-done.md` and the GitHub releases, newest first.
  - 1.7.7 took the first step: Keyrate runs as a real ring-3 process with its own window, and crashing it on purpose leaves the desktop standing.
- [x] 2.1: Music. A player app in its own protected space: WAV first, then MP3 through a small public-domain decoder, a library from Files, play and pause through the sound driver it already has.
- [x] 2.2: Video. A player app: motion-JPEG first (reusing the JPEG decoder the kernel already has) with sound in sync, then a real codec when the hardware allows.
- [x] 2.5: Joshua on the web. Numbered 2.5 on Joshua's call, so the plan's 2.1 Music and 2.2 Video are still open. The portfolio at heyitsmejosh.com is Joshua's own: the chat is titled Joshua, answers in the first person and speaks in his cloned voice, and the page opens on a 28 second lip-synced intro of him before the live OS takes over. Done, shipped as 2.5.0.
- 3.0: Hold it. Joshua Tree runs on a real Raspberry Pi 4: the desktop boots on the board and you can type and click on it, mouse included. Sound from the 3.5 mm jack, Samantha over Wi-Fi through the Mac relay, a new mark. The reference mini PC (UEFI, a real disk, a real network card) moves to a later 3.x.
- 3.1: The Samantha box ships as a kit. The printed case from `docs/hardware/`, a Pi 4, a 7 inch screen, a speaker and the card, bring your own parts (MONEY.md has the math).
- 4.0: Many windows. Real multitasking: a window server, real processes, pipes, a Terminal with tabs and splits. Three apps run at once, one crashes, the other two do not notice. Doom is the proof. May go mobile first: one app full screen, a home grid, gestures.
- 5.0: Talk to it. Samantha is the interface: a full-screen face, speech in and out, a small model on the box. A ten-minute spoken conversation that opens and changes things on the machine. The phone shape, if 4.0 started it, is finished here.
- 6.0: Yours. Accounts, a locked disk, snapshots and backups. Delete a file, go back in time, get it back.
- 7.0: Online. TLS 1.3, Wi-Fi, a real browser, mail and calendar that sync. A normal day of web, mail and calendar on it.
- 8.0: Build things. The Plank compiler on the box, an SDK, apps as signed bundles, a store. Someone who is not Joshua ships an app without touching the kernel.
- 9.0: The kit. The hardware is a product: Neo revisions, two displays, GPU, USB audio, storage and camera. A stranger builds one from the guide and it boots first try.
- 10.0: Daily driver. A security audit against the threat model, accessibility, languages, long-term support. Joshua uses it as his only computer for a month.

The one-line ideas and gates are the roadmap's long-road table (`roadmap.md`); this list stays in step with it. Order of work inside every version: usable, then stable, then polished.

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
