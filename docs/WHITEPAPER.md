# Joshua Tree Technical Whitepaper

Updated 2026-10-08. The software is at the version in the `VERSION` file.
This paper is a snapshot of that day, not a living page: the first draft
described 1.5.18 (September 2026), and the section "What changed since
1.5.18" at the end says what moved. When this paper and the code disagree,
the code and `docs/ARCHITECTURE.md` win.

An operating system, written from nothing. Not a Linux distribution. Not a
layer on top of something else. Every part of it, from the first
instruction the CPU runs to the pixels of the desktop, is in this
repository. On a PC it boots to a 1920x1080 desktop with a dock, a
terminal, a text editor, a file browser, Mail, Calendar, Contacts,
Calculator, Stocks, Reminders, twenty-six apps in all, live weather in
the menu bar, and an assistant, Samantha, who talks. It also runs in a
browser tab. Since October 2026 a second build of the same code boots on
a Raspberry Pi 4 with nothing underneath it.

## Why

Every computer I have ever used was someone else's decisions, stacked a
thousand deep, none of which I could see. I wanted one machine where I
understand the whole thing. Not "trust the abstraction." Actually know why
that bit is set in that page table.

The longer goal is a computer I built end to end: this software, on
hardware I can hold, talking to a model with as little cloud in the loop
as possible. This is the software half. The ideas behind each call are in
`docs/SOUL.md` and `docs/VISION.md`.

## What it is

Two builds of one code base.

**The i386 kernel.** A kernel for a 32-bit Intel machine. It sets up its
own memory management, its own interrupts, its own scheduler with real
process isolation (each task gets its own page tables), and its own
filesystem layer with two backends (a real FAT16 disk and a RAM disk). It
finds the network card by reading the PCI bus itself and built Ethernet,
ARP, IPv4, UDP, DNS, DHCP, TCP and HTTP from raw bytes on the wire. Every
app is a ring-3 program behind an `int 0x80` gate (`docs/SYSCALL-ABI.md`),
in its own address space with its own heap; the kernel keeps the devices,
the desktop's launch path and the network.

**The ARM64 build for the Raspberry Pi 4.** `arch/arm64/` is a separate
build of the same kernel, drivers and library (plan and milestones in
`docs/ARM64.md`, the board story in `docs/RASPBERRY-PI.md`). It boots as a
bare `kernel8.img` from the Pi's firmware, no Raspberry Pi OS, and draws
the same desktop over HDMI at the monitor's own size. The IP stack and the
HTTP client are the same C files as the i386 build, under a four-call
network card seam. It first booted on a real Pi 4 on 2026-10-06.

**The desktop.** Twenty-six apps live in an Apps folder; the dock pins the
ones you reach for most, Apps and Trash bookending them. Deleted files go
to a Trash you can restore from. Clicking the clock shows the system's own
log and any live warnings. The terminal is the same shell the machine
boots into, just in a window. Text everywhere is a real antialiased
typeface, not a bitmap. Icons are drawn as geometry at the panel's true
resolution, never stored as images. The Pi draws the same dock, window
frame and cursor from the shared paint code.

**Samantha.** The assistant is a ring-3 program with a face and a voice.
She talks to a Claude relay on the Mac (`tools/claude-relay/relay.py`,
`docs/CLAUDE-APP.md`): the OS posts a question over plain HTTP, the relay
answers from the Claude API, picking a cheap model for short questions and
a stronger one for long or hard ones, with read-only tools over one shared
folder of files on the Mac. On the Pi she answers at the Console's `ask>`
row. Each question carries the Pi's live status (IP address, clock, Wi-Fi
signal), and an answer can end with an action the Pi carries out: print a
note, or blink the board's green light.

**Wi-Fi on the Pi.** The Pi's CYW43455 chip has no supplicant, so the
WPA2 4-way handshake is done in software in the kernel (`arch/arm64/wpa.h`:
SHA-1, HMAC, the pairwise-key PRF and AES key unwrap, checked against the
RFC vectors by `tools/checks/wpa-test.c`). Then DHCP and the shared IP
stack on top.

**The network clock.** The Pi has no battery clock, so it does not know
the date until the network tells it. Once Wi-Fi joins, one plain HTTP HEAD
request gives a `Date:` line in UTC and the kernel counts forward from
there. Until then the menu bar reads `--:--` and the Calendar tile shows a
dash, never a made-up date.

**The quiet console.** The Pi's on-screen Console is the only debug
channel on a board with no serial cable. It shows only what matters:
failures, odd status lines and summaries. Chatter and key echoes go to
the serial line. It keeps the last 16 KB of the log, and Page Up, Page
Down, Home and End scroll it.

## How the pieces work

**Scaling.** The desktop is laid out at 960x540 and drawn at 1920x1080.
Every ordinary pixel write fills a 2x2 block, so no app has to know. Things
that want real sharpness (icons, the wallpaper, text) write physical
pixels directly. That is why the icons are crisp and nothing else had to
change. The Pi asks the firmware how big the monitor is and draws at that
size; a 4K monitor gets exactly half each way.

**Drawing only what changed.** There is no double buffer, so a full
repaint is visible. Moving the cursor repaints a few hundred pixels.
Hovering a dock icon repaints the dock strip from cached tiles. Only
opening an app repaints the screen. On the Pi the screen lives in cached
memory and the GPU sees only what the kernel cleans out of the cache, so
every glyph and every wipe is cleaned as it is drawn. The first boot
showed one thin mark per line until that was understood.

**Pointer input.** A tap lands the cursor exactly where the finger is.
The i386 kernel probes the VMware absolute-pointer backdoor at boot; where
a host answers (QEMU's default machine, v86 in the browser) the GUI takes
positions straight from it. On real hardware it falls back to the plain
relative PS/2 protocol. The Pi reads a USB keyboard and mouse through its
own PCIe and xHCI driver; the keyboard is proven on the board, the mouse
only in QEMU so far.

**Map wallpaper.** The desktop background can be a real topographic map,
pulled live from OpenTopoMap by IP geolocation. A real PNG decoder, built
into the kernel, renders map tiles at boot and when you change themes.
The default is the Satellite wallpaper in colour.

**The browser demo.** The landing page runs this exact i386 kernel in a
JavaScript x86 emulator. Four things a real BIOS normally sets up had to be
done by the kernel itself first: text mode, keyboard scanning, the colour
palette, the font. The demo tours itself if you leave it alone. The same
plain-HTTP TCP stack pulls live weather from Open-Meteo into the menu bar.

## Measured

Numbers from `docs/BENCHMARKS.md`, taken by `tools/bench.sh` in QEMU on
this Mac at 1.6.21 (2026-09-27). They move with the host; run it yourself.

| Benchmark | QEMU, i386 |
|---|---|
| boot_to_shell | 250 ms |
| heap_alloc_free | 65 ns/op |
| memcpy | 777 MB/s |
| context_switch | 2797 ns/switch |
| disk_read | 11443 KB/s |

On the real Pi 4, one run on one board, read off the screen:

| Measure | Pi 4 |
|---|---|
| memcpy | 1109 MB/s |
| alloc | 30 ns/op |
| desktop up | 0.2 s |
| Wi-Fi joined | 6.4 s |
| clock set | 8.7 s |

Treat the Pi row as a first reading, not a benchmark. There is no
automated bench on the Pi yet and no second board to compare with.

## How to boot

**In QEMU:** The kernel runs directly under QEMU's `-kernel` loader
without needing a bootloader at all. `make run` boots it to the shell;
type `gui` for the desktop.

**On a Raspberry Pi 4:** `tools/flash-pi.sh` builds `kernel8.img`,
fetches the Pi's own firmware files and writes a FAT32 card. Put the card
in and power on; the desktop comes up over HDMI 0. `make -C arch/arm64
run-pi` tries the same image on QEMU's Pi 4B model first.

**ISO/USB on other machines:** `make iso` builds a bootable hybrid disk
image (works as both CD-ROM and raw USB). It fetches Limine, a small
BIOS/UEFI bootloader, and packs it with `kernel.elf`.

**Real PC limitations:** UEFI-only machines without legacy BIOS/CSM are
untested. The bootloader can initialize graphics on Bochs VGA, VMware SVGA
and QEMU virtio; any other GPU gets text only. No USB storage or AHCI/NVMe
driver exists, so a real PC only sees FAT16 disks wired as IDE/ATA. USB
keyboards work only through BIOS legacy PS/2 emulation. A real PC has not
been tried; the Pi is the real-hardware target.

**Shell usage:** After booting to the shell, type `gui` to open the
desktop, `web example.com` for DNS/TCP, `ls`/`cat`/`write` for the
filesystem, and `help` for a full command list.

## Security

The parts that cross a trust boundary, and what each one does today. The
full list is `docs/THREAT-MODEL.md`.

**The relay.** The Claude relay is a command-capable agent behind an HTTP
port. It binds loopback unless started with `--lan`. Every request needs
a shared token of 16 to 63 characters, checked in constant time; a wrong
or missing token gets 401 and Claude never starts. The model gets
read-only tools only, scoped to one folder; anything else is denied, not
asked. Logs hold one line per request and never the token or a prompt.

**Keys outside the repo.** The relay token lives in a file in the home
folder, never on the command line and never in git. The i386 app never
holds it: Settings keeps it and the kernel adds the header, only for the
relay host. The ARM build compiles the token in at build time from the
same file, so a built Pi image is as secret as the token.

**The Wi-Fi key.** The network name and key come from
`~/.config/joshuatree/wifi.conf` at build time, written into a gitignored
header, and only when `JT_WIFI_DEV=1`, which `tools/flash-pi.sh` sets for
a development card. Release and CI builds carry no network. The passphrase
itself is never printed and never stored; only the derived key is.

**What is not wired yet.** There is no TLS anywhere in the OS. `--lan`
is plain HTTP, so on a LAN the token crosses the wire in the clear and a
request can be replayed; use it only on a network you trust. The repo's
BearSSL is a five-file subset (hashes, HMAC, the DRBG) used for entropy
and password hashing, not a TLS client. The plan is to vendor the rest of
BearSSL's client (handshake, certificates, records) and prove it with one
secure fetch; until then nothing in the kernel can speak HTTPS, and the
weather, map and Claude paths all go over port 80. There is no admin tier:
every program that runs is trusted the same.

## What it is not, yet

- **No mouse on the Pi.** The pointer, the dock label and clicks work in
  QEMU. The board has not had a wired USB mouse plugged in yet.
- **No HTTPS.** See above. A browser cannot load a secure page and the
  Pi cannot pull from GitHub.
- **No SD card writes.** The Pi boots from the card and nothing it does
  is saved to it. MBR and FAT32 writes are on the list.
- **No apps on the Pi.** The ARM build runs one EL0 test program with a
  write and exit syscall. The twenty-six ring-3 apps have not been rebuilt
  for ARM; Samantha on the Pi is the Console's `ask>` row.
- **No self-update.** A new build still means a card swap. The serial
  loader and the fallback kernel are not written.
- **No admin tier.** No accounts, no sudo, no privilege boundary between
  programs beyond the ring-3 gate.
- On the i386 side: the Trash is in RAM and empties on reboot, and the
  weather fetch blocks the desktop for its duration on a machine with no
  route out.

## How it is verified

Every version ships only after it is shown working, not just compiling.
`check.sh` proves the i386 kernel boots. Each shipped feature has a check
under `tools/checks/` that fails when the feature is removed; the ARM
build has its own set, from `arm64-m0-check.py` (serial hello) through
`arm64-wifi-check.py` and `arm64-claude-console-check.py`, all headless
in QEMU. The relay has `claude-relay-check.py` against a stub `claude`.
What QEMU cannot model (the Wi-Fi chip, the real cache, the GPU) is
verified by booting the board and reading the screen; the log of each
real boot is at the bottom of `docs/RASPBERRY-PI.md`. The bugs that
mattered most were found by instruments, not by reasoning: a serial probe
found a missing font, a pixel dump found corners drawn from the wrong
centre, a photo of the Pi's console found a cache that was never cleaned.

## What changed since 1.5.18

- Every app left the kernel. At 1.5.18 five apps opened in their own
  window and the rest were full-screen kernel code. Now all twenty-six
  are ring-3 programs, Samantha last, each with its own heap.
- The assistant moved from a local Ollama server to Samantha with a face
  and a voice, and the Claude relay with model routing and read-only
  file tools replaced the plain chat.
- The ARM64 build went from nothing to a desktop on a real Pi 4: USB
  keyboard, Wi-Fi with a software WPA2 handshake, DHCP, the network
  clock, the quiet console, and Samantha acting on the board.
- The map wallpaper is still there; the default is the colour Satellite
  image.
- The browser demo, the i386 memory model and the verification rule did
  not change.

## License

Apache License 2.0. Copyright 2026 Joshua Trommel.
