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
| `user/libjt/` | A small C library for user programs: `string.c`, `stdlib.c` (a fixed-arena `malloc`), `stdio.c` (`printf` and friends over the write syscall), `text.c` (antialiased DejaVu text drawn into the app's window), plus `ctype.h` and `unistd.h`. Built into `libjt.a`. |
| `user/libjt/text.c`, `user/libjt/text.h` | Antialiased text for ring-3 apps (1.9.23). `jt_text_draw` blends each glyph's coverage over the pixels already in the window, in a body face, a real bold and a large digit face. `text.h` is the API: draw, width, height, ascent. |
| `user/libjt/aafont.h` | Generated glyph atlas behind `text.c`: 4-bit coverage bitmaps of DejaVu Sans, rendered from the kernel's own font data. Rebuilt by the Makefile when the font or the generator changes. |
| `tools/gen/gen_user_text.c` | Host program that bakes `user/libjt/aafont.h` through `drivers/ttf.c`, so ring-3 text and in-kernel text share one rasterizer and one font. |
| `user/wc.c` | Unix `wc`, counts lines, words and bytes. The first program linked against libjt instead of raw syscalls. |
| `user/fbpoke.c` | The program that must not work (1.7.8): runs after a window closed, hands `write` a kernel pointer and a pointer into the released framebuffer (both must be `-EFAULT`), then stores into it and must page-fault. Run by `kernel/ring3app.c` under the `fbpoke` boot flag. |
| `user/keyrate.c` | Keyrate, the typing test, as a ring-3 program: the first app to leave the kernel (1.7.7). Gets its window from `SYS_WINDOW_OPEN`, its keys from `SYS_WINDOW_POLL`, draws its own 8x16 glyphs. The backquote key crashes it on purpose. |
| `user/toroid.c` | Toroid, Conway's Life on a torus, as a ring-3 program: the second app out of the kernel (1.7.11). Two 160x80 bit-packed boards in its own .data, generations paced off `SYS_TIME`, the backquote key crashes it on purpose. |
| `user/calculator.c` | Calculator, a recursive-descent parser over `+ - * / ()`, as a ring-3 program: the third app out of the kernel (1.7.12). Same grammar as the in-kernel version, evaluated straight into a `double` per rule instead of an `expr_node` tree, since a flat binary has no `.bss` and no `kmalloc`. Dividing by zero yields 0, unchanged. The backquote key crashes it on purpose. |
| `user/quotes.c` | Quotes, the film-quote guessing game, as a ring-3 program: the fourth app out of the kernel (1.7.14). Same fixed deck and answer-rotation as the in-kernel version, streak and best kept in its own `.data`. The backquote key crashes it on purpose. |
| `user/bookrank.c` | Bookrank, the ranked non-fiction shelf, as a ring-3 program: the fifth app out of the kernel (2.0). Same fixed book list and two-pane layout as the in-kernel version, up/down or a click selects, the summary word-wraps by character count in its own `.data`. The backquote key crashes it on purpose. |
| `user/homeqi.c` | Homeqi, eight yes/no feng shui questions about your home and a score, as a ring-3 program: the sixth app out of the kernel (1.8.22). Same questions and scoring as the old in-kernel copy, which had gone dead: APPS[] was opening Homeqi through the generic static-page viewer. The backquote key crashes it on purpose. |
| `user/lexly.c` | Lexly, a Spanish word and four English choices with a streak, as a ring-3 program: the seventh app out of the kernel (1.9.1). Same 30-word deck and drill as the old in-kernel copy, keys 1 to 4 or a click answer, streak and best kept in its own `.data`. The backquote key crashes it on purpose for `tools/checks/ring3lexly-check.py`. |
| `user/plan.c` | Plan, Joshua's ten-year education and career roadmap, as a ring-3 program: the eighth app out of the kernel (1.9.2). Same five milestones and two-pane layout as the old in-kernel copy, up/down or a click selects, the detail word-wraps by character count. The backquote key crashes it on purpose for `tools/checks/ring3plan-check.py`. |
| `user/fieldbook.c` | Fieldbook, every field of science and math explained plainly, as a ring-3 program: the ninth app out of the kernel (1.9.3). Same twelve fields and two-pane layout as the old in-kernel copy, up/down or a click selects, the explanation word-wraps by character count in the 8x16 font, and backquote is the deliberate crash `tools/checks/ring3fieldbook-check.py` presses. |
| `user/clock.c` | Clock, the time of day, a countdown timer and one alarm, as a ring-3 program: the tenth app out of the kernel (1.9.4). Same three jobs as the old in-kernel copy, the time comes from `SYS_TIME`, and the timer minutes and alarm are typed on the program's own line since a ring-3 program has no prompt box. Backquote is the deliberate crash `tools/checks/ring3clock-check.py` can press. |
| `user/activity.c` | Activity, uptime, free memory and the six scheduler slots with a Kill button, as a ring-3 program: the twelfth app out of the kernel (1.9.6). Same screen as the old in-kernel copy, refreshed about once a second through the one new call, `SYS_TASKS`, which also does the kill and refuses the shell and the app itself. Backquote is the deliberate crash `tools/checks/ring3activity-check.py` can press. |
| `user/contacts.c` | Contacts, a list of up to 32 people with a name, phone and email kept in `CONTACTS.TXT`, as a ring-3 program: the thirteenth app out of the kernel (1.9.7). Same list, add prompt, person view and delete as the old in-kernel copy, and the file is rewritten whole after every change through the ordinary open, read, write and close calls, so it needed no new syscall. The list lives in the zeroed pages past the image, since a flat binary has no `.bss`. `tools/checks/ring3contacts-check.py` drives it. |
| `user/sparkjar.c` | Sparkjar, ten ideas ranked by votes with the selected idea's pitch and plan beside them, as a ring-3 program: the fourteenth app out of the kernel (1.9.8). Same list, upvote and re-sort as the old in-kernel copy, votes kept for the run only, so it touches no file and needed no new syscall. `tools/checks/ring3sparkjar-check.py` drives it. |
| `user/reminders.c` | Reminders, a checklist of up to 24 items with a done flag kept in `REMINDERS.TXT`, as a ring-3 program: the fifteenth app out of the kernel (1.9.9). Same list, add prompt, tick and delete as the old in-kernel copy, and the file is rewritten whole after every change through the ordinary open, read, write and close calls, so it needed no new syscall. The list lives in the zeroed pages past the image, since a flat binary has no `.bss`. Samantha's reminder tools now read and write the same file fresh on every call from `kernel/chat.h`. Driven by `tools/checks/ring3reminders-check.py`. |
| `user/curbfind.c` | Curbfind, Craigslist deals near the visitor ranked by deal score with the selected listing's price, neighbourhood, score bar and reason beside them, as a ring-3 program: the sixteenth app out of the kernel (1.9.11). Same list, detail pane and Vancouver samples as the old in-kernel copy. The live rows come through the one new call, `SYS_HTTP_GET`: the kernel fixes the host and port, the program names only the path, and a path that is not plain printable ASCII starting with `/`, or a buffer outside user memory, is refused before the network is touched. The `p` key is the probe `tools/checks/ring3curbfind-check.py` presses to prove those refusals. |
| `user/calendar.c` | Calendar, the Day, Week, Month and Year views with one event per day kept in `EVENTS.TXT`, as a ring-3 program: the seventeenth app out of the kernel (1.9.12). Same views, keys and layout as the old in-kernel copy, drawn in the 8x16 font (the Year view halves it). Today comes from `SYS_TIME`, so the epoch-to-date and weekday math run in the program; `tools/checks/check-calendar.sh` sweeps that math against libc on the host. The file is rewritten whole after every save through the ordinary open, read, write and close calls, so it needed no new syscall. The event list lives in the zeroed pages past the image, since a flat binary has no `.bss`. Driven by `tools/checks/ring3calendar-check.py`. |
| `kernel/caldate.h` | The date math the kernel still needs with Calendar gone: leap years, days in a month, the `YYYY-MM-DD` key, today off the RTC, and a fresh read of `EVENTS.TXT` for Samantha's calendar tool in `chat.h`; `stocks.h` uses the same key for its quote timestamps. |
| `kernel/clockicon.h` | The Clock tile as a live analog face: white dial, ticks and hour, minute and centre hands drawn in physical pixels over the plain cached tile, redrawn when the minute changes. Also the one hook that lays Calendar's date and Clock's hands on a tile. |
| `user/search.c` | Search, the query box that filters the current directory as you type, as a ring-3 program: the eighteenth app out of the kernel (1.9.13). Same list, live filter, folder step-in and cat-style file view as the old in-kernel copy. It needed one new call, `SYS_READDIR`, which fills fixed-size records (name, size, is_dir) for a directory named by a path relative to the shell's directory; the kernel walks the path and walks back inside the call, so the app keeps its own cwd string and nothing outside it moves. `SYS_OPEN` takes the same relative paths, so a file inside a folder opens as `DOCS/NAME`. The records and the file buffer live in the zeroed pages past the image. Backquote is the deliberate crash. Driven by `tools/checks/ring3search-check.py`. |
| `user/epiphany.c` | Epiphany, the markets terminal (a 40-ticker watchlist, portfolio, trading simulator, macro brief and the `/` command bar for `AAPL GP` and `AAPL DES`), as a ring-3 program: the nineteenth app out of the kernel (1.9.19). Prices come through `SYS_HTTP_GET` from `/api/quotes` (about 700 bytes, inside the 2 KB body), with the compiled-in prices when the fetch fails. The GP chart draws previous close to last price, because the intraday series lives in the kernel's Stocks and does not fit the syscall body. `tools/checks/ring3epiphany-check.py` drives it. |
| `user/weather.c` | Weather, the current reading and a five-day forecast, as a ring-3 program: the twentieth app out of the kernel (1.9.22). The kernel's `weather_fetch` still does the network work and writes `WEATHER.TXT` after every fetch; the app reads it with plain file calls and shows the live reading, the last good one or labelled sample data. R draws Fetching and exits 7 so the kernel refetches and starts it again. `tools/checks/ring3weather-check.py` drives it, `tools/checks/weather-app-check.sh` covers the live, stale, bad, timeout and offline faces. |
| `user/portfolio.c` | Portfolio, Joshua's About block and the fleet catalog grouped Life, Read, Make, Play and Dev, as a ring-3 program: the eleventh app out of the kernel (1.9.5). Same rows as the old in-kernel copy, up/down or the wheel skip the group headers, a click selects, and the bottom line shows the selected app's address. Backquote is the deliberate crash `tools/checks/ring3portfolio-check.py` can press. |
| `drivers/user_activity.h`, `user_clock.h`, `user_contacts.h`, `user_fieldbook.h`, `user_homeqi.h`, `user_lexly.h`, `user_plan.h`, `user_portfolio.h`, `user_reminders.h`, `user_sparkjar.h` | The compiled bytes of those ring-3 apps, embedded in the kernel so it can start them. Generated by `tools/gen/gen_user_bin.py`, never edited by hand. |
| `kernel/ring3app.c` + `kernel/ring3app.h` | The table-driven launcher and supervisor for apps that run as ring-3 processes (`RING3_APPS`: name, embedded binary, VFS filename). Seeds the binary onto the VFS, runs it with `exec_user`, and when it exits or is reaped after a fault, logs what happened and hands the desktop back. Since 1.9.23 also `ring3app_launch_window`, the non-blocking launch behind a compositor window (see Ring-3 windows below). |
| `tools/checks/ring3window-check.py` | 1.9.23: Reminders as a ring-3 compositor window beside Notes. Both on screen, the crash key only reaches the focused window, and the crash closes only its own window. |
| `kernel/irqlock.h` | 1.9.23: `irq_save` / `irq_restore`, the one lock a uniprocessor kernel needs. A cli critical section that hands back the caller's own IF, used by kheap, vfs, the window event ring, task_kill and the window launcher. |
| `kernel/r3stress.c` | 1.9.23: the `stress=r3` boot flag. The desktop churns the heap and writes inside NOTES/ every frame while every SYS_WINDOW_POLL from a ring-3 window does the same under the gate; after 400 frames it walks the heap and reports to serial. Dead code unless armed. |
| `tools/checks/ring3stress-check.py` | 1.9.23: boots `open=remi stress=r3` on a real FAT disk, asserts both sides ran, no block was handed to two owners, `kheap: ok`, the syscall's file stayed at the root, the desktop finished. The lockless kernel hangs inside the first seconds. |

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
| `kernel/kernel.c` | The big one. The text console, the keyboard scancode table, the clock, the shell, and the whole desktop: menu bar, dock, windows, Apps folder, Burrow (the file browser), Settings, Weather, Lock Screen, the wind-swayed tree. Most apps are still drawn from here. |
| `kernel/files.h` | Burrow, the file browser app: browse the disk, open, rename, delete, restore from Trash. Split out of `kernel.c` and included straight back in. |
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
| `kernel/osk.h` | The on-screen keyboard for `boot_to_phone` (roadmap 1.9): a five-row key grid drawn at the bottom of the Notes editor, and `osk_tap()`, which turns a tap on a key into make and break scancodes through `kbd_inject()` so it lands in the same queue a PS/2 key does. A tap is already a left click at that point through the absolute pointer (`drivers/vmmouse.c`), so touch adds no second input path. Desktop mode never calls into it. |
| `kernel/settings_ui.h` | Settings app UI with a sidebar plus grouped detail pane (modeled on macOS System Settings). Pulled into its own file in the 1.7.x redesign pass to keep `kernel.c` under the godfile-check.sh ceiling; `#include`d directly into `kernel.c` at the exact spot the inline version used to sit. |

## Ring-3 windows (1.9.23)

Until 1.9.23 a ring-3 program was modal: `exec_user` blocked the desktop loop until the program exited, the program owned the one app viewport, and `SYS_WINDOW_POLL` pulled keys straight off the keyboard ring. Two programs could not even run at once, because every task directory shares the base page tables by value, so `JT_USER_BASE` and `JT_USER_FB` were the same physical frames in every task.

The window path changes four things and leaves the blocking path alone for every app not on it:

- `exec_user_window` (kernel/exec.c) loads the image and builds the stack in a kmalloc'd buffer of the window's own and returns the task id at once. `paging_task_map_private` (kernel/paging.c) gives that task's directory a private copy of the 4 MB page table that holds the user window, with the image, stack and framebuffer entries pointing at frames only this task maps. The low identity alias keeps the shared supervisor-only table, so no task reaches another's pages through it, and `paging_user_range_ok` now walks CR3, so a syscall pointer is judged by the caller's own tables.
- `syscall_window_register` (kernel/syscall.c, `r3wins`) allocates the framebuffer, maps it at `JT_USER_FB` in that task only, and keeps a 16-deep event ring per window. `SYS_WINDOW_OPEN` hands back the same `jt_window_info` as before. `SYS_WINDOW_POLL` with `JT_POLL_PRESENT` marks the window dirty instead of copying anything, and drains the ring; `-EAGAIN` when it is empty.
- The compositor (kernel/kernel.c, `gui_window_t.task`) blits a dirty window's buffer through `window_pixel` at the window's position with the usual chrome, and feeds keys and content clicks only to the topmost window's ring (`syscall_window_push_event`). The red dot kills the task. Every frame it looks for a window whose task is gone (exit or a fault reaped by idt.c), logs the same crash line the blocking launcher does, and closes that one window.
- `gui_ring3_windowed` names the apps on this path. Reminders is the proof; adding an app is one row. Burrow, Notes and Mail can move to ring 3 on it without losing multi-window.

**The concurrency rule.** One CPU, so one rule: shared kernel state is only ever touched with interrupts off. A syscall already runs that way end to end (int 0x80 is an interrupt gate) and the only call that turns them back on, `SYS_HTTP_GET`, holds nothing while it waits: http.c refuses a second fetch while one is in flight, from the desktop or the gate. The desktop (task 0) runs with interrupts on and can be preempted anywhere, so every entry point it shares with a syscall is an `irq_save`/`irq_restore` section (kernel/irqlock.h): `kmalloc`/`kfree`, every `vfs_*` call (which covers FAT, ramfs and the disk), `syscall_window_push_event` into the per-window ring, `task_kill`'s frame patch, and the whole of `ring3app_launch_window` (a slot is marked used before its private mapping exists). The FAT cwd is one global cursor the desktop may have left inside NOTES/, so a syscall path resolves from the root and puts the cursor back (`vfs_cwd_get`/`vfs_cwd_set`). `kheap_check` walks the free list on demand and `tools/checks/ring3stress-check.py` hammers both sides at once to keep it that way.

## The apps

Twenty-five apps live in the Apps folder; the dock pins the ones you reach
for most. Each is one header. They come in three shapes.

**Apps with a file on disk.** Same pattern every time: a fixed-size array
in RAM, one plain text file on the FAT disk, written through on every
change. No Save button.

| App | File | On disk |
|---|---|---|
| Notes | `kernel/editor.h` | Folders and many notes under `NOTES/`; an old `NOTES.TXT` moves in as the first note. The one app with real typography: an embedded DejaVu family with six faces and any size from 12 to 200 points. |
| Reminders | `user/reminders.c`, `kernel/ring3app.c` | `REMINDERS.TXT`, one line per item. The fifteenth ring-3 app (1.9.9); its in-kernel copy is gone. |
| Calendar | `user/calendar.c`, `kernel/ring3app.c` | `EVENTS.TXT`, one line per date. The grid itself is computed from the clock. The seventeenth ring-3 app (1.9.12); its in-kernel copy is gone. |
| Mail | `kernel/mail.h` | `MAIL.TXT`. Two starter messages ship compiled in. |
| Contacts | `user/contacts.c`, `kernel/ring3app.c` | `CONTACTS.TXT`. The thirteenth ring-3 app (1.9.7); its in-kernel copy is gone. |
| Chat | `kernel/chat.h`, `kernel/chat_face.h` | `CHAT.TXT`. Talks to a local Ollama server over the kernel's own HTTP. Push-to-talk: holding F2 records on `drivers/sb16.c` and posts the clip to the Worker's `/api/listen` (Cloudflare Workers AI Whisper); the recognized text runs through the same path a typed message takes. `chat_face.h` renders Samantha's animated face above the conversation, fetching idle and talk frame sequences on first boot. |

**Apps with nothing to save.**

| App | File | What it is |
|---|---|---|
| Stocks | `kernel/stocks.h` | Eight fixed symbols with live quotes and charts from the Worker at `/api/stocks`. |
| Epiphany | `user/epiphany.c`, `kernel/ring3app.c` | Watchlist, portfolio, simulator and a Bloomberg-style command bar (`/`, then `AAPL GP` or `AAPL DES`), live prices through `SYS_HTTP_GET`. The nineteenth ring-3 app (1.9.19); its in-kernel copy is gone. |
| Search | `user/search.c`, `kernel/ring3app.c` | Filters the current directory as you type. Enter opens a folder or shows a file. Scoped to what the VFS can list, no whole-disk index. The eighteenth ring-3 app (1.9.13); its in-kernel copy is gone. |
| Portfolio | `user/portfolio.c`, `kernel/ring3app.c` | A catalog of every app in the fleet with its URL. The eleventh ring-3 app (1.9.5); its in-kernel copy is gone. |
| Activity | `user/activity.c`, `kernel/ring3app.c` | Activity Monitor over the real scheduler and memory counters. Refreshes on a timer, can kill a task. The twelfth ring-3 app (1.9.6); its in-kernel copy is gone. |
| Clock | `user/clock.c`, `kernel/ring3app.c` | Current time from the RTC, a countdown timer you can start and pause, and an alarm. The tenth ring-3 app (1.9.4); its in-kernel copy is gone. |

**Apps ported from the fleet.** Each is a native rewrite of one of the
sibling web apps, kept small on purpose.

| App | File | What it is |
|---|---|---|
| Curbfind | `user/curbfind.c`, `kernel/ring3app.c` | Craigslist deals near you, ranked by score, live through `SYS_HTTP_GET` with Vancouver samples when offline. The sixteenth ring-3 app (1.9.11); its in-kernel copy is gone. |
| Lexly | `user/lexly.c`, `kernel/ring3app.c` | Spanish vocabulary drill, four choices. The seventh ring-3 app (1.9.1); its in-kernel copy is gone. |
| Fieldbook | `user/fieldbook.c`, `kernel/ring3app.c` | Every field of science and math, explained plainly. The ninth ring-3 app (1.9.3); its in-kernel copy is gone. |
| Plan | `user/plan.c`, `kernel/ring3app.c` | A ten-year timeline with a detail panel. The eighth ring-3 app (1.9.2); its in-kernel copy is gone. |
| Sparkjar | `user/sparkjar.c`, `kernel/ring3app.c` | Vote on ideas, session only. The fourteenth ring-3 app (1.9.8); its in-kernel copy is gone. |
| Keyrate | `user/keyrate.c`, `kernel/ring3app.c` | Typing test with endless random words and a live words-per-minute count. The first app running outside the kernel as a ring-3 process (1.7.7); its in-kernel copy is gone. |
| Toroid | `user/toroid.c`, `kernel/ring3app.c` | Conway's Life on a torus. The second ring-3 app (1.7.11); its in-kernel copy is gone. |
| Calculator | `user/calculator.c`, `kernel/ring3app.c` | Recursive-descent parser over `+ - * / ()`. The third ring-3 app (1.7.12); its in-kernel copy is gone. |
| Quotes | `user/quotes.c`, `kernel/ring3app.c` | Name the film from the line. Streak and best for the session. The fourth ring-3 app (1.7.14); its in-kernel copy is gone. |
| Bookrank | `user/bookrank.c`, `kernel/ring3app.c` | Ranked non-fiction: a list on the left, the selected book's title, author and summary on the right. The fifth ring-3 app (2.0); its in-kernel copy is gone. |
| Homeqi | `user/homeqi.c`, `kernel/ring3app.c` | Eight feng shui questions about your home and a score. The sixth ring-3 app (1.8.22); its in-kernel copy is gone. |

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
