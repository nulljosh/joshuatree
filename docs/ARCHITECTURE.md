# Architecture

Joshua Tree is a whole computer written from nothing. You turn it on, it
draws a desktop, and you use its apps: notes, mail, a calendar, weather,
a chat that can set reminders for you. Everything it does, it does
itself. There is no Linux underneath and no borrowed library.

It runs in your browser on the landing page, in QEMU on a Mac, or from a
USB stick on a real PC. It is all one file, `kernel.elf`, built from the
`boot/`, `kernel/`, `drivers/` and `lib/` folders. `docs/roadmap.md` is
the plan; this page is the map of what exists today.

## How it starts

1. **Something loads it.** QEMU, the browser demo or a real bootloader
   drops the kernel into memory and jumps to `boot/boot.S`.
2. **It sets up its own memory.** A few lines of assembly move the kernel
   to where it expects to live, then hand off to C.
3. **It wakes the hardware, one piece at a time.** `kmain` starts the
   processor tables, the clock, memory, the scheduler and the disk, each
   one needing the last.
4. **You land in a shell.** Type `gui` and the desktop starts. The shell
   and the desktop are the same program; the Terminal is the shell in a
   window.

## Boot modes

Pass keywords on the multiboot command line (QEMU's `-append` flag or the
browser emulator's cmdline config) to change how the kernel starts:

| Flag | What it does |
|---|---|
| `phone` | Boots into portrait phone mode (430x932) instead of the desktop (960x540 retina). Handled in `kernel/kernel.c`'s `kmain` as `boot_to_phone`, used by `gui_run`. |
| `samantha` | Skips the desktop and opens Chat's full-screen Samantha avatar view directly. Handled in `kernel/kernel.c`'s `kmain` as `boot_to_samantha`, passed to `chat_boot_samantha_open`. |

## The layers

Each layer only leans on the ones above it on this page, so it reads top to bottom.

### CPU setup and interrupts

| File | What it does |
|---|---|
| `boot/boot.S` | The first code that runs. Sets up just enough memory to reach C. |
| `kernel/gdt.c` | Tells the processor what is kernel and what is a program, so programs can safely call into the kernel. |
| `kernel/idt.c` + `kernel/isr.S` | The interrupt table and the 32 CPU exception handlers. A kernel-mode fault paints a panic screen and halts. A user-mode fault kills that one program and the kernel keeps going. Its crash report now names the function that faulted, via `kernel/symtab.h`. |
| `kernel/symtab.h` + `kernel/backtrace.c` | Crash reports with real function names. `kernel/symtab.c` (generated, never committed) is a sorted address-to-name table read from `nm -n` on a first-pass link of the kernel, done twice by the Makefile so the table can live inside the very kernel it describes. `backtrace.c` binary-searches that table for a fault's EIP and walks a few EBP stack frames above it, printing `at <func>+0x<off>` lines over serial and on the panic screen. `kernel/symtab_stub.c` is the empty placeholder table pass one links against, since the real table doesn't exist yet at that point. |
| `kernel/pic.c` | Routes hardware signals, like a key press, so they never get confused with processor errors. |
| `kernel/irq.c` + `kernel/irq_stubs.S` | The hardware interrupt handlers. The timer tick drives the scheduler, the keyboard fills a ring buffer. |
| `kernel/entropy.c` + `kernel/entropy.h` | The kernel's only source of unpredictable bytes: an HMAC_DRBG (BearSSL SHA-256) seeded from RDRAND when the CPU has it, RDTSC jitter and interrupt timing. Password salts draw from it. Boot logs `entropy: sources=` over serial. |
| `kernel/syscall.c` | The `int 0x80` dispatch table. Numbers and calling convention are Linux's, so the ABI needs no translation. The contract is written down in `docs/SYSCALL-ABI.md`. |

### Memory

| File | What it does |
|---|---|
| `kernel/pmm.c` | Physical memory as a bitmap of 4KB frames, sized from what the bootloader reports. |
| `kernel/paging.c` | Decides which memory each program can see, and keeps programs out of the kernel's. |
| `kernel/kheap.c` | `kmalloc` and `kfree`. A first-fit free list that grows one frame at a time. |
| `lib/libc.c` | `memcpy`, `memset`, `strlen` and the other handful of primitives a freestanding kernel cannot live without. |
| `third_party/bearssl/` | Vendored BearSSL 0.6 subset (MIT): implementation files (`src/sha2small.c`, `src/hmac.c`, `src/hmac_drbg.c`, `src/dec32be.c`, `src/enc32be.c`), public API headers (`inc/bearssl.h`, `bearssl_aead.h`, `bearssl_block.h`, `bearssl_ec.h`, `bearssl_hash.h`, `bearssl_hmac.h`, `bearssl_kdf.h`, `bearssl_pem.h`, `bearssl_prf.h`, `bearssl_rand.h`, `bearssl_rsa.h`, `bearssl_ssl.h`, `bearssl_x509.h`), an internal header (`src/inner.h`), a config header (`src/config.h`), and a one-line `string.h` shim onto `lib/libc.h`. Nothing modified; see its README.md. |

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
| `user/fbpoke.c` | The program that must not work (1.7.8): runs after a window closed, hands `write` a kernel pointer and a pointer into the released framebuffer (both must be `-EFAULT`), then stores into it and must page-fault. Run by `kernel/ring3app.c` under the `fbpoke` boot flag. |
| `user/keyrate.c` | Keyrate, the typing test, as a ring-3 program: the first app to leave the kernel (1.7.7). Gets its window from `SYS_WINDOW_OPEN`, its keys from `SYS_WINDOW_POLL`, draws its own 8x16 glyphs. The backquote key crashes it on purpose. |
| `user/toroid.c` | Toroid, Conway's Life on a torus, as a ring-3 program: the second app out of the kernel (1.7.11). Two 160x80 bit-packed boards in its own .data, generations paced off `SYS_TIME`, the backquote key crashes it on purpose. |
| `user/calculator.c` | Calculator, a recursive-descent parser over `+ - * / ()`, as a ring-3 program: the third app out of the kernel (1.7.12). Same grammar as the in-kernel version, evaluated straight into a `double` per rule instead of an `expr_node` tree, since a flat binary has no `.bss` and no `kmalloc`. Dividing by zero yields 0, unchanged. The backquote key crashes it on purpose. |
| `user/quotes.c` | Quotes, the film-quote guessing game, as a ring-3 program: the fourth app out of the kernel (1.7.14). Same fixed deck and answer-rotation as the in-kernel version, streak and best kept in its own `.data`. The backquote key crashes it on purpose. |
| `user/bookrank.c` | Bookrank, the ranked non-fiction shelf, as a ring-3 program: the fifth app out of the kernel (2.0). Same fixed book list and two-pane layout as the in-kernel version, up/down or a click selects, the summary word-wraps by character count in its own `.data`. The backquote key crashes it on purpose. |
| `kernel/ring3app.c` + `kernel/ring3app.h` | The table-driven launcher and supervisor for apps that run as ring-3 processes (`RING3_APPS`: name, embedded binary, VFS filename). Seeds the binary onto the VFS, runs it with `exec_user`, and when it exits or is reaped after a fault, logs what happened and hands the desktop back. |

### Storage

| File | What it does |
|---|---|
| `drivers/ata.c` | Reads and writes the hard disk. |
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
| `drivers/net.c` | Ethernet, ARP, IPv4, UDP, DNS and TCP, built up from raw frames on top of whichever card was found. `net_init` leases a real address by DHCP (DISCOVER/OFFER/REQUEST/ACK) first, falling back to the old fixed config if nothing answers; `nodhcp` on the command line skips it. |
| `drivers/http.c` | `http_get` and `http_post` over that TCP. Plain HTTP only. There is no TLS. |
| `drivers/html.c` | A deliberately tiny HTML-to-text converter, enough to read a page or a ported app. |
| `drivers/json.c` | A small JSON reader for the weather, geolocation and chat responses. |

The network stack is what fetches the weather in the menu bar, the map
tiles for the wallpaper, stock quotes, and the replies from a local
Ollama server in the Chat app.

### Sound

| File | What it does |
|---|---|
| `drivers/sb16.c` | The Sound Blaster 16 driver: DSP reset/detect, IRQ 5, and ISA DMA channel 1 for both directions. `sb16_play`/`sb16_beep` push 8-bit unsigned mono PCM out through the SB16-only high-speed command pair (`0x41` set rate, `0xC0` transfer). `sb16_record` captures the same format in, through the older DSP-2.xx-compatible ADC pair (`0x40` set time constant, `0x24` transfer) every real SB16 answers -- QEMU's own `-device sb16` has no ADC path today (confirmed against its source), so recording only produces real audio on hardware, or under `make talk`'s coreaudio backend. |
| `drivers/speak.c` | Text to speech: posts to Turing's `/api/speak`, plays the PCM reply on `sb16.c`, and tracks its own playback position so Chat's face can move with it (`speak_level`). |

### Worker endpoints

The browser landing page routes kernel HTTP requests to a Cloudflare Worker
(`worker.js`) for external APIs:

| Endpoint | What it does |
|---|---|
| `/api/stocks` | Live stock quotes for the Stocks app. |
| `/api/chat` | LLM routing and chat responses for the Chat app. |
| `/api/pick` | LLM tool selection for Chat's command execution. |
| `/api/waitlist` | Dev-kit waitlist signup (POST) and count (GET). Stores one email per address with a timestamp. |
| `/api/listen` | Speech-to-text for Chat's push-to-talk: runs Workers AI Whisper over a posted audio clip. |

### Graphics and input

| File | What it does |
|---|---|
| `drivers/vbe.c` | Switches the screen into 1920x1080 graphics. |
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
| `kernel/dock_draw.c` | The dock's pixel drawing: the band cache behind hover/drag animation, the tray, the icons and the hover label. Split out of `kernel.c`, sits on top of `dock_geom.c`'s layout math. |
| `kernel/hint.h` | The keyboard-hint line every app draws ("esc closes" and friends), skipped on phones where there is no keyboard. Split out of `kernel.c`. |
| `kernel/app.h` | The app interface. One `struct app` per app (name, tile color, glyph, `open`, and `draw`/`key` for apps that run in a desktop window), plus the few desktop services and helpers an app in its own file needs. |
| `kernel/app.c` | The helpers behind `app.h`, only ones two or more apps were writing by hand: start an app's window with its titlebar, print a number. |
| `kernel/bench.h` | Built-in benchmarks: boot time, heap, memcpy, context switch, disk read. `bench` in the shell or on the command line. Results in `docs/BENCHMARKS.md`. |
| `kernel/gui_prompt.h` | The shared one-line prompt and chrome-versus-content split that the newer apps use, so a keystroke redraws only what changed. |
| `kernel/auth.h` | User accounts. PBKDF2-HMAC-SHA256 records in `USERS.TXT` (scheme tag and iteration count stored per line), verify-only support for the pre-1.7.9 chained-SHA-256 records with an in-place upgrade on the next successful login, the login and first-run screens. Built against `docs/THREAT-MODEL.md`. |
| `kernel/auth_kdf.c` + `kernel/auth_kdf.h` | PBKDF2-HMAC-SHA256 (RFC 8018) as one block loop over BearSSL's `br_hmac`; no primitive of its own. Compiled for the kernel and natively for `tools/auth-host`, which pins it to the RFC 7914 and RFC 6070 (SHA-256) vectors. |
| `kernel/wall_sat.h` | A real satellite photo, baked in, used as the wallpaper when there is no network to fetch map tiles. |
| `kernel/boot_mark.h` | The real landing brand mark (`landing/logo.svg`, the four-arm Joshua tree), rasterized by `tools/gen/gen_boot_mark.py` into 8-bit alpha coverage at the splash's real physical size and blended straight onto the boot screen by `gui_draw_boot_mark` (`kernel/kernel.c`), replacing the old `gui_draw_logo` stick-figure primitive there. The menu bar keeps drawing `gui_draw_logo` unchanged, since the engraved-style mark reads as a solid blob at 16px. |
| `kernel/phone_home.h` | The phone home screen for `boot_to_phone`: a 5-column, no-scroll grid of all 26 apps, a status bar with the real clock and weather, and a tappable back chevron in place of the desktop's traffic lights (drawn as two bold stepped diagonal strokes, injects a real Esc scancode through `kbd_inject()` so every app closes through the one `kbd_pop()==27` path a keyboard already drives). Desktop mode never calls into it, so it stays pixel-identical. |
| `kernel/settings_ui.h` | Settings app UI with a sidebar plus grouped detail pane (modeled on macOS System Settings). Pulled into its own file in the 1.7.x redesign pass to keep `kernel.c` under the godfile-check.sh ceiling; `#include`d directly into `kernel.c` at the exact spot the inline version used to sit. |

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
| Chat | `kernel/chat.h`, `kernel/chat_face.h` | `CHAT.TXT`. Talks to a local Ollama server over the kernel's own HTTP. Push-to-talk: holding F2 records on `drivers/sb16.c` and posts the clip to the Worker's `/api/listen` (Cloudflare Workers AI Whisper); the recognized text runs through the same path a typed message takes. `chat_face.h` renders Samantha's animated face above the conversation, fetching idle and talk frame sequences on first boot. |

**Apps with nothing to save.**

| App | File | What it is |
|---|---|---|
| Stocks | `kernel/stocks.h` | Eight fixed symbols with live quotes and charts from the Worker at `/api/stocks`. |
| Epiphany | `kernel/epiphany.h` | The offline slice of the Epiphany portfolio app: watchlist, portfolio, crypto, plus a Bloomberg-style command bar (`/`, then `AAPL GP` or `AAPL DES`). |
| Search | `kernel/search.h` | Filters the current directory as you type. Enter opens a folder or shows a file. Scoped to what the VFS can list, no whole-disk index. |
| Portfolio | `kernel/portfolio.h` | A catalog of every app in the fleet with its URL. |
| Activity | `kernel/activity.h` | Activity Monitor over the real scheduler and memory counters. Refreshes on a timer, can kill a task. |
| Clock | `kernel/clock.h` | Current time from the RTC, a countdown timer you can start and pause, and an alarm. |

**Apps ported from the fleet.** Each is a native rewrite of one of the
sibling web apps, kept small on purpose.

| App | File | What it is |
|---|---|---|
| Curbfind | `kernel/curbfind.h` | Craigslist deals for Vancouver, ranked by score. |
| Lexly | `kernel/lexly.h` | Spanish vocabulary drill, four choices. |
| Fieldbook | `kernel/fieldbook.h` | Every field of science and math, explained plainly. |
| Plan | `kernel/plan.h` | A ten-year timeline with a detail panel. |
| Sparkjar | `kernel/sparkjar.h` | Post an idea, vote on ideas. |
| Homeqi | `kernel/homeqi.h` | Eight feng shui questions about your home and a score. |
| Keyrate | `user/keyrate.c`, `kernel/ring3app.c` | Typing test with endless random words and a live words-per-minute count. The first app running outside the kernel as a ring-3 process (1.7.7); its in-kernel copy is gone. |
| Toroid | `user/toroid.c`, `kernel/ring3app.c` | Conway's Life on a torus. The second ring-3 app (1.7.11); its in-kernel copy is gone. |
| Calculator | `user/calculator.c`, `kernel/ring3app.c` | Recursive-descent parser over `+ - * / ()`. The third ring-3 app (1.7.12); its in-kernel copy is gone. |
| Quotes | `user/quotes.c`, `kernel/ring3app.c` | Name the film from the line. Streak and best for the session. The fourth ring-3 app (1.7.14); its in-kernel copy is gone. |
| Bookrank | `user/bookrank.c`, `kernel/ring3app.c` | Ranked non-fiction: a list on the left, the selected book's title, author and summary on the right. The fifth ring-3 app (2.0); its in-kernel copy is gone. |

**Adding an app.** Every app is one row in `APPS[]` in `kernel/kernel.c`,
and nothing else dispatches on an app's index: the dock, the Apps folder,
Chat's `open_app`, and the multiwindow desktop all read that table. Write
the app as a ring-3 program in `user/<app>.c` the way Keyrate and Toroid
do and add a row to `RING3_APPS` in `kernel/ring3app.c`, or (for now) in
its own kernel `.c` with its `open` declared in a tiny header. Then add
the row, add the `.c` to the Makefile, and bump `GUI_APP_COUNT`. Give it `draw` and `key` only if it should also run as a
desktop window. Reach for `app.h`'s helpers before writing your own.

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
