# Joshua Tree Technical Whitepaper

**0.45.0** | September 2026

An operating system, written from nothing. Not a Linux distribution. Not a
layer on top of something else. Every part of it, from the first
instruction the CPU runs to the pixels of the desktop, is in this
repository. It boots in about two seconds, runs at 1920x1080, has a dock, a
terminal, a text editor, a file browser, fifteen apps, live weather in the
menu bar, and a tree that sways in the wind. It also runs in a browser tab.

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

On top of that sits a desktop. Fifteen apps live in an Apps folder; the
dock shows the seven you reach for. Deleted files go to a Trash you can
restore from. Clicking the clock shows the system's own log and any live
warnings. The terminal is the same shell the machine boots into, just in a
window. Text everywhere is a real antialiased typeface, not a bitmap. Icons
are drawn as geometry at the panel's true resolution, never stored as
images.

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

**The browser demo.** The landing page runs this exact kernel in a
JavaScript x86 emulator. Four things a real BIOS normally sets up had to be
done by the kernel itself before that worked: the text mode, keyboard
scanning, the colour palette, and the font. The demo tours itself if you
leave it alone.

**Weather.** Fetched by the kernel's own TCP stack from Open-Meteo, which
still answers plain HTTP. There is no TLS here, so that detail is the whole
reason the feature exists.

## What it is not, yet

There is no stable syscall interface for programs to target. That is the
definition of 1.0. Apps are full-screen; there is no windowing. The Trash
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

## Licence

Apache License 2.0. Copyright 2026 Joshua Trommel.
