# Joshua Tree Technical Whitepaper

**0.69.0** | September 2026

An operating system, written from nothing. Not a Linux distribution. Not a
layer on top of something else. Every part of it, from the first
instruction the CPU runs to the pixels of the desktop, is in this
repository. It boots in about two seconds, runs at 1920x1080, has a dock, a
terminal, a text editor, a file browser, Mail, Calendar, Contacts,
Calculator, Stocks, Reminders, twenty-one apps in all, live weather in the
menu bar, and a tree that sways in the wind over a real map of your location.
It also runs in a browser tab.

## Why

Every computer I have ever used was someone else's decisions, stacked a
thousand deep, none of which I could see. I wanted one machine where I
understand the whole thing. Not "trust the abstraction." Actually know why
that bit is set in that page table.

The longer goal is a computer I built end to end: this software, on
hardware I solder myself, talking to a language model I trained, with no
cloud in the loop. This is the software half.

## What it is

A kernel for a 32-bit Intel machine. It sets up its own memory management,
its own interrupts, its own scheduler with real process isolation (each
task gets its own page tables), and its own filesystem layer with two
backends (a real FAT16 disk and a RAM disk). It found the network card by
reading the PCI bus itself and built Ethernet, ARP, IPv4, UDP, DNS, TCP and
HTTP from raw bytes on the wire.

On top of that sits a desktop. Twenty-one apps live in an Apps folder; the
dock pins ten you reach for most, Apps and Trash bookending them. Deleted
files go to a Trash you can restore from. Clicking the clock shows the
system's own log and any live warnings. The terminal is the same shell the
machine boots into, just in a window. Text everywhere is a real antialiased
typeface, not a bitmap. Icons are drawn as geometry at the panel's true
resolution, never stored as images. The wallpaper is a map of your real
location, tinted warm or cool or left raw, pulled live from OpenTopoMap
with its own PNG decoder built into the kernel.

## How the pieces work

**Scaling.** The desktop is laid out at 960x540 and drawn at 1920x1080.
Every ordinary pixel write fills a 2x2 block, so no app has to know. Things
that want real sharpness (icons, the wallpaper, text) write physical
pixels directly. That is why the icons are crisp and nothing else had to
change.

**Drawing only what changed.** There is no double buffer, so a full
repaint is visible. Moving the cursor repaints about 340 pixels. Hovering
a dock icon repaints the dock strip from cached tiles. Only opening an app
repaints the screen.

**Pointer input.** A tap lands the cursor exactly where the finger is, not
wherever a relative walk from the last position happens to land. The
kernel probes the VMware absolute-pointer backdoor at boot; where a host
answers (QEMU's default machine, v86 in the browser) the GUI takes
positions straight from it. On real hardware, with no backdoor to answer,
it falls back to the plain relative PS/2 protocol.

**Chat.** `chat <message>` and the Chat app talk to a local Ollama server
over this kernel's own TCP/HTTP stack, plain HTTP only, no TLS here yet.
Someone else's model for now, but already running with no cloud in the
loop, the one piece of the longer goal that already works.

**Map wallpaper.** The desktop background is a real topographic map, pulled
live from OpenTopoMap by IP geolocation. A real PNG decoder, built into the
kernel, renders map tiles at boot and when you change themes. Four
wallpaper styles are selectable: the original photo, the map tinted warm,
tinted cool for higher contrast, or raw from the tiles themselves. The same
TCP/HTTP stack fetches both the tiles and the geographic coordinates.

**The browser demo.** The landing page runs this exact kernel in a
JavaScript x86 emulator. Four things a real BIOS normally sets up had to be
done by the kernel itself first: text mode, keyboard scanning, the colour
palette, the font. The demo tours itself if you leave it alone. The same
plain-HTTP TCP stack that talks to Ollama also pulls live weather from
Open-Meteo into the menu bar.

## What it is not, yet

There is a real `int 0x80` gate now, but only two calls behind it, exit and
write, no libc shim, no ELF loading, no wider surface yet. A real,
general syscall interface for programs to target is still the definition
of 1.0. Apps are full-screen; there is no windowing. The Trash
is in RAM and empties on reboot. The weather fetch blocks the desktop for
its duration on a machine with no route out. The wind measures its own
first frame and turns itself off on slow machines, including the browser.

## How it is verified

Every version ships only after it is shown working, not just compiling.
`check.sh` proves it boots. `apptest.sh` opens all fifteen apps in a real
emulator and checks the pixels. `mobiletest.mjs` drives the browser demo
with a real iPhone emulation and taps an icon. `tourtest.mjs` loads the
demo, touches nothing, and confirms it demonstrates itself. The bugs that
mattered most were found by instruments, not by reasoning: a serial probe
found the missing font, a pixel dump found corners drawn from the wrong
centre, a macro photo found a bezel nobody could see at normal size.

## License

Apache License 2.0. Copyright 2026 Joshua Trommel.
