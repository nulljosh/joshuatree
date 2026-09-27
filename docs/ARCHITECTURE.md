# Architecture

This is the map of the kernel as it exists today. If you want to know what
a file is for, or where to start reading, start here. `docs/roadmap.md` is
the plan and the history; this page stays in the present tense.

The whole thing is one static binary, `kernel.elf`, built from the
`boot/`, `kernel/`, `drivers/` and `lib/` folders with Clang and lld.
There is no libc. Everything the kernel needs, it carries.

## From power-on to the desktop

1. **The bootloader hands over.** QEMU's `-kernel` loader, or Limine on a
   real disk, loads the ELF at physical address 1MB and jumps to
   `_start` in `boot/boot.S`.
2. **Paging comes on.** The kernel is linked to run at 0xC0000000 (a
   "higher-half" kernel) but it is sitting at 1MB. `boot.S` builds a
   temporary page directory that maps the first 4MB at both addresses,
   switches paging on, and jumps to the high address. From here on every
   line of C runs in the upper half.
3. **`kmain` brings the hardware up in dependency order.** Segments, then
   interrupt tables, then the interrupt controller, then physical memory,
   then the permanent page tables, then the scheduler, then the disk. Each
   step needs the one before it, so the order is not negotiable.
4. **You land in a shell.** Type `gui` and the desktop starts. The shell
   and the desktop are the same program; the terminal window is just the
   shell in a window.

## The layers

Read the kernel bottom-up and each file makes sense from the ones below it.

### CPU setup and interrupts

| File | What it does |
|---|---|
| `boot/boot.S` | Multiboot header, temporary page tables, the jump into the higher half, the first stack. |
| `kernel/gdt.c` | The segment table. Ring-0 and ring-3 code and data segments, plus the TSS that lets a user program trap back into the kernel on its own kernel stack. |
| `kernel/idt.c` + `kernel/isr.S` | The interrupt table and the 32 CPU exception handlers. A kernel-mode fault paints a panic screen and halts. A user-mode fault kills that one program and the kernel keeps going. |
| `kernel/pic.c` | Reprograms the 8259 so hardware interrupts land on vectors 32 to 47 instead of colliding with CPU exceptions. |
| `kernel/irq.c` + `kernel/irq_stubs.S` | The hardware interrupt handlers. The timer tick drives the scheduler, the keyboard fills a ring buffer. |
| `kernel/syscall.c` | The `int 0x80` dispatch table. Numbers and calling convention are Linux's, so the ABI needs no translation. The contract is written down in `docs/SYSCALL-ABI.md`. |

### Memory

| File | What it does |
|---|---|
| `kernel/pmm.c` | Physical memory as a bitmap of 4KB frames, sized from what the bootloader reports. |
| `kernel/paging.c` | The permanent page tables. Identity-maps the first 4MB and maps it again at 0xC0000000 for the kernel. Also flips individual pages user-accessible for ring-3 programs. |
| `kernel/kheap.c` | `kmalloc` and `kfree`. A first-fit free list that grows one frame at a time. |
| `lib/libc.c` | `memcpy`, `memset`, `strlen` and the other handful of primitives a freestanding kernel cannot live without. |

### Tasks and user programs

| File | What it does |
|---|---|
| `kernel/task.c` | Preemptive round-robin scheduling off the timer tick. Six slots. `yield()` reaches the same switch in software. |
| `kernel/ring3.c` + `kernel/ring3_asm.S` | Runs code at privilege level 3 with its own page tables, so a privileged instruction faults instead of taking the machine down. |
| `kernel/exec.c` | Loads a flat binary off the filesystem into a fixed window the linker script reserves, marks its pages user-accessible, and starts it as a ring-3 task with argv. |
| `user/hello.c` + `user/jtsys.h` | The reference user program and the one header a user program gets: inline `int 0x80` wrappers, nothing else. |
| `user/note.c` | The second user program, and the first one worth running: prints a file, appends a line, seeks. Exercises the v2 syscalls. |
| `user/libjt/` | A small C library for user programs: `string.c`, `stdlib.c` (a fixed-arena `malloc`), `stdio.c` (`printf` and friends over the write syscall), plus `ctype.h` and `unistd.h`. Built into `libjt.a`. |
| `user/wc.c` | Unix `wc`, counts lines, words and bytes. The first program linked against libjt instead of raw syscalls. |

### Storage

| File | What it does |
|---|---|
| `drivers/ata.c` | ATA PIO disk reads and writes, primary master, 28-bit addressing. |
| `drivers/blockdev.c` | One `read_sector` / `write_sector` table so the filesystem does not care whether the bytes come from a disk or from RAM. |
| `drivers/ramdisk.c` | The RAM-backed block device. |
| `drivers/fat.c` | FAT16 with real subdirectories, writes and 8.3 names. |
| `drivers/ramfs.c` | A tiny flat in-memory filesystem, eight files of 4KB. Exists so the VFS has a second backend to prove it abstracts anything. |
| `drivers/vfs.c` | The filesystem switch. Every app and every shell command goes through here instead of calling FAT directly. |
| `drivers/trash.c` | Recoverable delete. `rm` moves a file here; you can restore it from the Trash on the desktop. Lives in RAM, so it empties on reboot. |

### Network

| File | What it does |
|---|---|
| `drivers/pci.c` | Walks the PCI bus so the kernel can find its own network card. |
| `drivers/rtl8139.c` | Driver for the RTL8139 card, QEMU's default. |
| `drivers/ne2k.c` | Driver for the NE2000 card, which is what the browser emulator provides. The kernel probes for RTL8139 first and falls back to this. |
| `drivers/net.c` | Ethernet, ARP, IPv4, UDP, DNS and TCP, built up from raw frames on top of whichever card was found. |
| `drivers/http.c` | `http_get` and `http_post` over that TCP. Plain HTTP only. There is no TLS. |
| `drivers/html.c` | A deliberately tiny HTML-to-text converter, enough to read a page or a ported app. |
| `drivers/json.c` | A small JSON reader for the weather, geolocation and chat responses. |

The network stack is what fetches the weather in the menu bar, the map
tiles for the wallpaper, stock quotes, and the replies from a local
Ollama server in the Chat app.

### Graphics and input

| File | What it does |
|---|---|
| `drivers/vbe.c` | Switches the Bochs VGA adapter into 1920x1080 linear framebuffer mode and back. |
| `drivers/window.c` | The one drawing target everything renders through: pixels, rectangles, a viewport, and a partial-repaint band. |
| `drivers/font.c` | Reads the real 8x16 IBM font out of VGA hardware at boot. Falls back to an embedded copy when the read comes back empty, as it does in the browser. |
| `drivers/png.c` + `drivers/jpeg.c` | Small image decoders, 8-bit RGB and RGBA PNG and baseline JPEG. The wallpaper map tiles go through PNG. |
| `drivers/ttf.c` | A runtime TrueType rasterizer, so text scales to any size. Wraps `drivers/stb_truetype.h`, vendored unmodified from Sean Barrett's stb, with the heap and string shims it needs. |
| `drivers/dejavu_font.h` and siblings | The six DejaVu faces, Sans, Serif and Mono in regular and bold, as ASCII plus Latin-1 subsets: `dejavu_bold_font.h`, `dejavu_serif_font.h`, `dejavu_serif_bold_font.h`, `dejavu_mono_font.h`, `dejavu_mono_bold_font.h`. Generated from the TTFs in `tools/fonts/`. |
| `drivers/mouse.c` | The PS/2 mouse, including the wheel. |
| `drivers/vmmouse.c` | The VMware absolute-pointer interface. When the host answers, a tap lands exactly where the finger is. Real hardware falls back to `mouse.c`. |
| `drivers/serial.c` | COM1 output for debugging. A boot trace you can read with `-serial stdio` even after the screen is dead. |
| `kernel/console.h` | The three text-console functions the kernel uses before the desktop exists. |

The desktop is laid out at 960x540 and drawn at 1920x1080. Every ordinary
pixel write fills a 2x2 block, so apps never think about scaling. Icons,
text and the wallpaper write physical pixels, which is why they are sharp.
There is no double buffer; the kernel repaints only the region that
changed.

### The desktop

| File | What it does |
|---|---|
| `kernel/kernel.c` | The big one. The text console, the keyboard scancode table, the clock, the shell, and the whole desktop: menu bar, dock, windows, Apps folder, Files, Settings, Weather, Lock Screen, the wind-swayed tree. Most apps are still drawn from here. |
| `kernel/files.h` | The Files app: browse the disk, open, rename, delete, restore from Trash. Split out of `kernel.c` and included straight back in. |
| `kernel/ttf_render.h` | The shared glyph path for anything drawing real DejaVu text at physical resolution: a per-face cache, a glyph cache, the antialiased ink blend. Notes and the Terminal both draw through it. |
| `kernel/gui_prims.c` | Tiny pure helpers split out of `kernel.c`: blend two colours, square root for antialiased lines. |
| `kernel/dock_geom.c` | Dock geometry and hit-testing: where each icon sits at the current scale, and which slot a click landed on. |
| `kernel/bench.h` | Built-in benchmarks: boot time, heap, memcpy, context switch, disk read. `bench` in the shell or on the command line. Results in `docs/BENCHMARKS.md`. |
| `kernel/gui_prompt.h` | The shared one-line prompt and chrome-versus-content split that the newer apps use, so a keystroke redraws only what changed. |
| `kernel/auth.h` | User accounts. A from-scratch SHA-256, a salted `USERS.TXT` on disk, the login and first-run screens. Built against `docs/THREAT-MODEL.md`. |
| `kernel/wall_sat.h` | A real satellite photo, baked in, used as the wallpaper when there is no network to fetch map tiles. |

## The apps

Twenty-five apps live in the Apps folder; the dock pins the ones you reach
for most. Each is one header. They come in three shapes.

**Apps with a file on disk.** Same pattern every time: a fixed-size array
in RAM, one plain text file on the FAT disk, written through on every
change. No Save button.

| App | File | On disk |
|---|---|---|
| Notes | `kernel/editor.h` | `NOTES.TXT`. The one app with real typography: an embedded DejaVu family with six faces and any size from 12 to 200 points. |
| Reminders | `kernel/reminders.h` | `REMINDERS.TXT`, one line per item. |
| Calendar | `kernel/calendar.h` | `EVENTS.TXT`. The grid itself is computed from the clock. |
| Mail | `kernel/mail.h` | `MAIL.TXT`. Two starter messages ship compiled in. |
| Contacts | `kernel/contacts.h` | `CONTACTS.TXT`. |
| Chat | `kernel/chat.h` | `CHAT.TXT`. Talks to a local Ollama server over the kernel's own HTTP. |

**Apps with nothing to save.**

| App | File | What it is |
|---|---|---|
| Calculator | `kernel/calculator.h` | A recursive-descent parser over `+ - * / ()`. |
| Stocks | `kernel/stocks.h` | Eight fixed symbols with live quotes and charts from the Worker at `/api/stocks`. |
| Epiphany | `kernel/epiphany.h` | The offline slice of the Epiphany portfolio app: watchlist, portfolio, crypto. |
| Search | `kernel/search.h` | Filters the current directory as you type. Enter opens a folder or shows a file. Scoped to what the VFS can list, no whole-disk index. |
| Portfolio | `kernel/portfolio.h` | A catalog of every app in the fleet with its URL. |
| Activity | `kernel/activity.h` | Activity Monitor over the real scheduler and memory counters. Refreshes on a timer, can kill a task. |

**Apps ported from the fleet.** Each is a native rewrite of one of the
sibling web apps, kept small on purpose.

| App | File | What it is |
|---|---|---|
| Quotes | `kernel/quotes.h` | Name the film from the line. Streak and best for the session. |
| Toroid | `kernel/toroid.h` | Conway's Life on a torus. |
| Bookrank | `kernel/bookrank.h` | Ranked non-fiction with a summary panel. |
| Curbfind | `kernel/curbfind.h` | Craigslist deals for Vancouver, ranked by score. |
| Lexly | `kernel/lexly.h` | Spanish vocabulary drill, four choices. |
| Fieldbook | `kernel/fieldbook.h` | Every field of science and math, explained plainly. |
| Plan | `kernel/plan.h` | A ten-year timeline with a detail panel. |
| Sparkjar | `kernel/sparkjar.h` | Post an idea, vote on ideas. |
| Homeqi | `kernel/homeqi.h` | Eight feng shui questions about your home and a score. |

The `drivers/app_*.h` files are the original single-file web builds of
those apps as C byte arrays, generated by `tools/gen/gen_app.sh`. The
`serveapp` shell command serves one over the kernel's own TCP so you can
`curl` it from the host. The `drivers/user_hello.h` and
`drivers/user_note.h` arrays are the two user programs, embedded the same
way so a fresh disk has something to `exec`.

## Host-side tools

None of this is kernel code. It builds and runs on the Mac.

**Native harnesses** compile one kernel file against a tiny fake of
`kmalloc` and `memcpy` so it can be tested without booting QEMU.
`tools/png-host/main.c`, `tools/jpeg-host/main.c`, `tools/ttf-host/` and
`tools/auth-host/main.c` each do this for a decoder, the font rasterizer or `auth.h`;
`tools/libjt-host/main.c` does the same for the user-space C library;
the `kheap.h` and `libc.h` shims next to each one are the fakes.
`tools/png-host/wallsat_check.c` decodes the baked satellite photo and
checks it comes out 960x540. `drivers/png_testdata.h` and
`drivers/jpeg_testdata.h` are generated test images.

**Fuzzers** in `tools/fuzz-host/` hammer the code that reads untrusted
bytes. `fuzz_parsers.c` covers HTTP, HTML and JSON; `fuzz_decoders.c`
covers PNG and JPEG. Both run under address and undefined-behaviour
sanitizers, using the shims in `tools/parsers-host/`.

**Generators** in `tools/gen/` produce the embedded data: the icon art
from hand-drawn SVGs, the editor fonts, the VGA font fallback, the
satellite wallpaper, the user program arrays, the ported app arrays, and
the fact row and progress chart on the landing page.

**Checks** in `tools/checks/` are the regression suite. `check.sh` proves
the kernel boots. `qa-gallery.py` opens every app in a
headless QEMU and looks at the pixels. `feature-drive.py` performs each
app's main action. `soak-check.py` opens and closes everything many times
and checks nothing leaked. `frametime-check.py` fails if a repaint gets
slow. `usertest-check.sh` runs the ring-3 programs and asserts their
output. The rest each pin one specific thing that once broke: dock
shadows, icon lighting, the lock screen, the panic screen, the Apps folder
layout, the landing page facts. CI runs them all on every pull request.

**The dev loop.** `tools/watch.sh` keeps a QEMU window open and reboots it
when the built kernel actually changes. `tools/qa-demo.sh` records a
headless tour of the desktop to video. `tools/voice-control.sh` lets you
speak a shell command into a running instance through Whisper and Ollama.

## Where to go next

- `docs/BENCHMARKS.md` for how fast it is, and `tools/bench.sh` to measure it yourself.
- `docs/WHITEPAPER.md` for why this exists and what it is not yet.
- `docs/SYSCALL-ABI.md` if you want to write a program for it.
- `docs/BLUEPRINT.md` for the plan after 1.0: apps as real processes and a window server.
- `docs/THREAT-MODEL.md` for what the login screen does and does not protect.
