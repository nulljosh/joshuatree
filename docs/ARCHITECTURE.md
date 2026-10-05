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
| `samantha` | Skips the desktop and opens the ring-3 Samantha program (`user/samantha.c`) first. Handled in `kernel/kernel.c`'s `kmain` as `boot_to_samantha`, which launches dock slot 6. |

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
| `kernel/brk.c` | `SYS_BRK` (1.9.27): a per-task heap at `JT_BRK_BASE` grown a frame at a time from the PMM, zeroed, mapped into that task's directory only, capped at 8 MB per task and a 4 MB free-frame floor, released from `syscall_release_task` on exit and crash with a `brk: released` serial line. |
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
| `kernel/memmap.h` | The one place the fixed ring-3 addresses live (`JT_USER_BASE`, `JT_DMABUF_BASE`, `JT_USER_FB`); the Makefile renders it into `boot/memmap.ld` for the linker scripts. See the memory map paragraph below. |
| `arch/arm64/start.S`, `arch/arm64/vectors.S`, `arch/arm64/user.S`, `arch/arm64/main.c`, `arch/arm64/text.c`, `arch/arm64/libc.c`, `arch/arm64/linker.ld`, `arch/arm64/Makefile` | The ARM64 port (docs/ARM64.md), milestones M0, M1a, M1b, M1c, the first M2 drivers and M3a, the first userland. `vectors.S` is the exception table (a fault prints a line instead of hanging); `main.c` also sets up the interrupt controller and a timer tick, turns the MMU and caches on over a flat identity map, has a small bump heap, and gets a framebuffer (ramfb through fw_cfg on QEMU's virt machine, the VideoCore mailbox on a Pi) draws a first desktop into it and mirrors the boot log in its window (lines printed before the screen existed are replayed). The text is the same DejaVu faces through `drivers/ttf.c` as the i386 desktop: `text.c` blends each rasterized glyph onto the screen, `libc.c` supplies the few libc calls the rasterizer makes, and only those files use floating point (`start.S` turns the FPU on at EL1). The old bitmap font is the fallback if the faces fail to load, then on virt finds virtio devices among the virtio-mmio slots: a disk (it reads sector 0), a network card (it asks the router for its address over ARP) and a keyboard and tablet, read with one input driver. Devices raise their interrupt through the GIC and the main loop sleeps in `wfi` between events. A separate build from the i386 kernel: a boot stub that parks all but core 0, a linker script for QEMU's virt machine, and `main` printing over the PL011 UART. `make -C arch/arm64 run` boots it, `make -C arch/arm64 pi` makes the Raspberry Pi 4 image (`kernel8.img`, enters at EL2 and drops to EL1); `tools/checks/arm64-m0-check.py` proves both builds boot, and `tools/checks/arm64-m1c-check.py` takes a QEMU screendump and checks the pixels, and `tools/checks/arm64-m2-check.py` presses keys, moves the pointer and clicks through QMP and reads them back. M3a: `user.S` is a small EL0 program the kernel copies into a user-only code page; `vectors.S` also takes `svc` and faults from EL0 and holds `enter_user` and `leave_user`; `main.c` splits the RAM map into 4 KiB pages for that one arena (code read-only, stack, and a page only the kernel may touch), answers `write` and `exit`, refuses to print memory the program does not own, and survives an EL0 fault as a printed line. `tools/checks/arm64-m3-check.py` asserts the exact serial lines. |
| `kernel/exec.c` | Loads a flat binary off the filesystem into a fixed window the linker script reserves, marks its pages user-accessible, and starts it as a ring-3 task with argv. |
| `user/hello.c` + `user/jtsys.h` | The reference user program and the one header a user program gets: inline `int 0x80` wrappers, nothing else. |
| `user/note.c` | The second user program, and the first one worth running: prints a file, appends a line, seeks. Exercises the v2 syscalls. |
| `user/libjt/` | A small C library for user programs: `string.c`, `stdlib.c` (a fixed-arena `malloc`), `stdio.c` (`printf` and friends over the write syscall), `text.c` (antialiased DejaVu text drawn into the app's window), plus `ctype.h` and `unistd.h`. Built into `libjt.a`. |
| `user/libjt/text.c`, `user/libjt/text.h` | Antialiased text for ring-3 apps (1.9.23). `jt_text_draw` blends each glyph's coverage over the pixels already in the window, in a body face, a real bold and a large digit face. `text.h` is the API: draw, width, height, ascent. |
| `user/libjt/mono.c`, `user/libjt/aamono.h` | The mono face for ring-3 apps: DejaVu Sans Mono, ASCII only, one size, every glyph an 8 px advance. `jt_mono_draw` uses its own generated atlas (`aamono.h`, baked by `tools/gen/gen_user_text.c` next to `aafont.h`) and lives in its own object so only the Terminal pays for it. |
| `user/libjt/jpeg.c` | One line: `#define JPEG_USER` then `#include "../../drivers/jpeg.c"`, so ring 3 runs the kernel face decoder itself. `jpeg_decode_scaled` decodes one MCU row at a time into a box-averaged RGB565 frame, which is how Samantha decodes a 320x320 JPEG frame to the 60x60 the kernel face shows; the JPEGs themselves live in her `SYS_BRK` heap. `jpeg_decode_scaled32` (2.9.0) is the same walk into packed 0RGB, up to 1024 wide, for her 736 pixel portrait. |
| `user/libjt/wav.c`, `wav.h` | The WAV reader for the Music app. `wav_open` checks a RIFF/WAVE file in memory (8 or 16-bit PCM, mono or stereo, 4000 to 44100 Hz) and refuses anything else; a data size that lies is clamped to the file. `wav_read8` returns mono 8-bit unsigned samples, the format `SYS_AUDIO` takes. It never reads outside the buffer it is given. |
| `user/libjt/avi.c`, `avi.h` | The AVI reader for the Movie app: motion-JPEG video and optional 8/16-bit PCM sound, parsed from a file held in memory. `avi_next` hands back video and audio chunks in file order, steps into `rec ` groups, ignores streams it does not play, and treats a cut-off last chunk as the end. `avi_audio8` makes audio chunks into the 8-bit mono `SYS_AUDIO` takes. |
| `tools/media/avigen.py`, `avi-host.c` | The fixtures and host test for `avi.c`: real JPEG frames through the kernel decoder, truncation, bad headers and a mutation fuzz under ASan and UBSan. Run by `tools/checks/avi-host-check.sh`. |
| `tools/media/demo-music.py`, `demo-disk.sh` | Three original demo songs for the Music app, synthesized from scratch (so nothing to license): 8-bit mono 11025 Hz WAV, about 40 s and 430KB each. `demo-disk.sh` puts them in `MUSIC/` on a FAT disk image. They are generated, never committed, and never embedded in the kernel image. |
| `tools/checks/fpu-check.sh` | Boots the kernel and runs `fputest`: two tasks keep different values on the x87 stack across task switches. Proves `schedule()` swaps per-task float state, which the float-heavy MP3 decoder needs. |
| `tools/media/wavgen.py`, `wav-host.c` | The fixtures and the host test for `wav.c`: golden samples, broken headers, then 20000 mutations under ASan and UBSan. Run by `tools/checks/media-host-check.sh`. |
| `tools/media/musicfix.py` | Builds the FAT16 disk `tools/checks/music-check.py` boots with: a 440Hz WAV in the root, a 16-bit stereo 660Hz WAV and a 3.3 MB over-the-cap WAV in `MUSIC/`. The songs live on the disk, never in the kernel image. |
| `third_party/minimp3/minimp3.h` | Vendored minimp3 (CC0 public domain, https://github.com/lieff/minimp3): a single-header MP3 decoder for Layer I, II and III files. Includes stubs `stdlib.h` and `string.h` that redirect to `user/libjt/`. |
| `user/libjt/mp3.c`, `mp3.h` | The MP3 reader for the Music app (2.2). `mp3_open` skips an ID3v2 tag, finds the first frame and adds up every frame's samples with a header-only scan for the length. `mp3_read8` hands back mono 8-bit unsigned samples and keeps the tail of a frame that straddles two calls. `mp3_seek` scans to the frame, then decodes about 2KB of earlier frames and drops them, because a frame leans on the bit reservoir of the ones before it. minimp3 keeps about 15KB of float tables on its stack and a ring-3 program has 4KB, so the decoder state, frame buffers and a 64KB stack of its own live in one heap block and the decode runs on that stack. |
| `tools/media/mp3-host.c`, `fixtures/` | The host test for `mp3.c`: golden length, the 440 Hz pitch and amplitude band, chunked reads equal one read, seek equals playing through, truncated and garbage input, a mutation fuzz, all under ASan and UBSan. Fixtures are three small committed MP3s (`fixtures/regen.sh` rebuilds them with ffmpeg), so CI needs no ffmpeg. |
| `user/libjt/avi.c`, `avi.h` | The AVI reader for Movies. `avi_open` checks a RIFF/AVI file in memory (motion-JPEG video, optional 8 or 16-bit PCM sound) and refuses anything else; `avi_next` hands back the video and audio chunks in file order, `avi_audio8` turns audio bytes into 8-bit mono. A chunk that runs past the end of the file is the end of the movie. It never reads outside the buffer it is given. |
| `tools/media/avigen.py`, `avi-host.c`, `tools/checks/avi-host-check.sh` | The fixtures and the host test for `avi.c`: real JPEG frames through the real decoder, odd chunk orders, broken headers, then mutations under ASan and UBSan. `avigen.py --movie` also builds the clips `movie-check.py` plays (a 2 s clip whose frame number is drawn as squares, a damaged clip, a non-AVI, one over the size cap). |
| `user/libjt/osk.c`, `user/libjt/osk.h` | The phone on-screen keyboard for ring-3 apps. Five rows of 40 px at the window bottom (digits, three letter rows with backspace, then space, enter and done), drawn with the antialiased text face. `jt_osk_draw` paints it, `jt_osk_hit` turns a click into a key code (done answers Esc), `jt_osk_height` is what an app subtracts from its text area. The launcher passes `phone` as argv[1] when the kernel booted in phone mode; Notes is the first user. |
| `user/libjt/aafont.h` | Generated glyph atlas behind `text.c`: 4-bit coverage bitmaps of DejaVu Sans, rendered from the kernel's own font data. Rebuilt by the Makefile when the font or the generator changes. |
| `tools/gen/gen_user_text.c` | Host program that bakes `user/libjt/aafont.h` through `drivers/ttf.c`, so ring-3 text and in-kernel text share one rasterizer and one font. |
| `tools/gen/ci-balance.py` | Rebalances the CI shards. Reads how long each check took in a finished run and rewrites the shard numbers in `tools/checks/ci-suite.sh` so every shard gets about the same work. |
| `tools/landing-shots.py`, `tools/landing_shots_manifest.py`, `tools/checks/landing-shots-fresh-check.py` | The landing app tiles in `landing/shots/`. The script retakes a tile headlessly (the fleet apps boot with `open=<app>` and no network, so they show their offline samples) and records a hash of the sources that draw it in `landing/shots/sources.json`. The check fails when an app changed and its tile was not retaken. |
| `user/wc.c` | Unix `wc`, counts lines, words and bytes. The first program linked against libjt instead of raw syscalls. |
| `user/brkpoke.c` | The `SYS_BRK` leak probe (1.9.27): grows 3 MB, touches every page, checks each came back zero, shrinks, then crashes on purpose so `tools/checks/ring3brk-check.py` can assert the PMM free count and brk live count return to baseline. Run by `kernel/ring3app.c` under the `brkpoke` boot flag. |
| `user/fbpoke.c` | The program that must not work (1.7.8): runs after a window closed, hands `write` a kernel pointer and a pointer into the released framebuffer (both must be `-EFAULT`), then stores into it and must page-fault. Run by `kernel/ring3app.c` under the `fbpoke` boot flag. |
| `user/keyrate.c` | Keyrate, the typing test, as a ring-3 program: the first app to leave the kernel (1.7.7). Gets its window from `SYS_WINDOW_OPEN`, its keys from `SYS_WINDOW_POLL`, draws its own 8x16 glyphs. The backquote key crashes it on purpose. |
| `user/toroid.c` | Toroid, Conway's Life on a torus, as a ring-3 program: the second app out of the kernel (1.7.11). Two 160x80 bit-packed boards in its own .data, generations paced off `SYS_TIME`, the backquote key crashes it on purpose. |
| `user/calculator.c` | Calculator, a recursive-descent parser over `+ - * / ()`, as a ring-3 program: the third app out of the kernel (1.7.12). Same grammar as the in-kernel version, evaluated straight into a `double` per rule instead of an `expr_node` tree, since a flat binary has no `.bss` and no `kmalloc`. Dividing by zero yields 0, unchanged. The backquote key crashes it on purpose. Tab switches to scientific: `^`, `!`, `%`, pi, e, ans and sin through exp, on the x87 with no libm. |
| `user/quotes.c` | Quotes, the film-quote guessing game, as a ring-3 program: the fourth app out of the kernel (1.7.14). Same fixed deck and answer-rotation as the in-kernel version, streak and best kept in its own `.data`. The backquote key crashes it on purpose. |
| `user/bookrank.c` | Bookrank, the ranked non-fiction shelf, as a ring-3 program: the fifth app out of the kernel (2.0). Two panes, up/down or a click selects, the list scrolls. The top twelve books come live from the real Bookrank API through `SYS_HTTP_GET` and the Worker (2.6.26); the reply is untrusted, so every byte is forced printable and every copy is bounded. Ten built-in samples show when the fetch or the parse fails. The backquote key crashes it on purpose. |
| `user/tonchi.c` | Tonchi (was Lexly), learn anything, as a ring-3 program: the seventh app out of the kernel (1.9.1). A course picker over the live Tonchi courses, then a four-choice drill with a score (2.6.27), through `SYS_HTTP_GET` and the Worker. The reply is untrusted: bytes forced printable, every copy bounded, answer index checked, bad rows dropped. With no network or a bad reply it opens the 30-word Spanish deck as before. Keys 1 to 4 or a click answer, Esc goes back to the picker. The backquote key crashes it on purpose for `tools/checks/ring3tonchi-check.py`; `tools/checks/ring3tonchi-live-check.py` proves the live and offline paths. |
| `user/fieldbook.c` | Fieldbook, every field of science and math explained plainly, as a ring-3 program: the ninth app out of the kernel (1.9.3). Same twelve fields and two-pane layout as the old in-kernel copy, up/down or a click selects, the explanation word-wraps by character count in the 8x16 font, and backquote is the deliberate crash `tools/checks/ring3fieldbook-check.py` presses. |
| `user/clock.c` | Clock, the time of day, a countdown timer and one alarm, as a ring-3 program: the tenth app out of the kernel (1.9.4). Same three jobs as the old in-kernel copy, the time comes from `SYS_TIME`, and the timer minutes and alarm are typed on the program's own line since a ring-3 program has no prompt box. Backquote is the deliberate crash `tools/checks/ring3clock-check.py` can press. |
| `user/music.c` | Music, a song player as a ring-3 program with its own window (2.2): the library lists every `.WAV` in the Files root and in a `MUSIC` folder (`SYS_READDIR`), one song at a time is read whole into the `SYS_BRK` heap (`SYS_READFILE`, refused over 3 MB with a message) and `wav.c` turns it into the 8-bit mono the card takes. Play, pause, previous, next, a seek bar (click or arrow keys), shuffle, repeat and a software volume. The elapsed time is the stream start plus the driver's `played` counter, never the wall clock, and pause or seek wait for the queue to drain first so that number stays true. MP3 is a marked hook that refuses with a message (the decoder does not fit a one page stack yet). Backquote is the deliberate crash for `tools/checks/ring3crash-all-check.py`; `tools/checks/music-check.py` reads its `music:` serial lines. |
| `user/movies.c` | Movies, a motion-JPEG AVI player with sound, as a ring-3 program (2.2). Lists `.AVI` files from Files, reads the whole clip into its `SYS_BRK` heap with `SYS_READ_FILE`, indexes it once with `avi.c`, then plays with the audio as the clock: the frame on screen is `played / rate / frame time`, only that frame is decoded, late frames are dropped and the picture never makes the sound wait. Space pauses, left and right seek five seconds, a click on the bar seeks, `f` hides the controls and letterboxes into the whole window. A clip too big, damaged or not an AVI shows a message instead. Backquote is the deliberate crash `ring3crash-all-check.py` presses; `tools/checks/movie-check.py` proves the rest. |
| `user/hamurabi.c` | Hamurapi (Hamurabi in the sources), the 1968 city game, as a ring-3 program with its own window (2.7), playable start to finish. The title screen (three buttons: story, Classic 1968, a demo the robot king plays by itself), a card at the gate of some story years (the opening, a turning point in years 5 and 10, or an omen with two choices, one drawn grey when the city cannot pay), the orders panel (Land, Feed, Plant, each with minus, plus and a bar you can click; Up and Down pick a row, Left and Right change it, Shift takes big steps, Enter ends the year; the orders are clamped like the web game's so a year can never be illegal), the year's report and the ending with the grade. The drawn city behind it (sky, hills, town around a ziggurat with the king on top, river and boat, fields, sheep, walking villagers) reacts as the web scene does: a villager for every four people, plots sown for what you order planted, sacks for the grain, the harvest and rats as floating numbers, ghosts for the starved, a green fog for the plague, rain on a ruined reign, fireworks on a great one. It draws at the largest whole-number pixel scale the window allows. The sprite sheet is unpacked once onto the `SYS_BRK` heap and every frame is painted into a back buffer, then copied over the window, so the compositor never catches half a frame. Each reign seeds its dice from the clock and logs its seed and every year's orders and result on the serial port; the backquote key crashes it on purpose. `tools/checks/ring3hamurabi-check.py` proves it, replaying every logged year on the host. |
| `user/hamurabi_sprites.h` | Hamurabi's 125 sprites on one 256x149 sheet: a 133-colour palette, the sheet as run-length coded palette indices (about 21 KB instead of 38 KB) and an enum of sprite names with their rectangles. Generated by `tools/gen/gen_hamurabi_sprites.py` from `art/hamurabi/sprites.png` and `sprites.json`, committed, and checked for staleness in the suite. |
| `user/windgate.c` | Windgate, guided breathing, as a ring-3 program (2.8). The web app's four presets (Box 4-4-4-4, 4-7-8, Relax, Coherent): a circle that grows on the in-breath, holds, and shrinks on the out-breath, the phase word, the seconds left, a round counter and a session timer. Each frame is painted into a back buffer on the `SYS_BRK` heap and copied over the window in one pass. Time comes from `SYS_TIME` (true wall seconds); the tick counter only places it inside the second and is re-learned at every boundary, because QEMU's runs about twice too fast. Exercise time stops while paused. It writes one serial line per phase start with the exact boundary time, which `tools/checks/ring3windgate-check.py` verifies. Backquote is the deliberate crash `ring3crash-all-check.py` presses. |
| `user/activity.c` | Activity, uptime, free memory and the six scheduler slots with a Kill button, as a ring-3 program: the twelfth app out of the kernel (1.9.6). Same screen as the old in-kernel copy, refreshed about once a second through the one new call, `SYS_TASKS`, which also does the kill and refuses the shell and the app itself. Backquote is the deliberate crash `tools/checks/ring3activity-check.py` can press. |
| `user/contacts.c` | Contacts, a list of up to 32 people with a name, phone and email kept in `CONTACTS.TXT`, as a ring-3 program: the thirteenth app out of the kernel (1.9.7). Same list, add prompt, person view and delete as the old in-kernel copy, and the file is rewritten whole after every change through the ordinary open, read, write and close calls, so it needed no new syscall. The list lives in the zeroed pages past the image, since a flat binary has no `.bss`. `tools/checks/ring3contacts-check.py` drives it. |
| `user/hikko.c` | Hikko, the idea forum, as a ring-3 program: the fourteenth app out of the kernel (1.9.8, as Sparkjar). Shows the real forum's top twelve ideas (title, votes, text, plan) from `/api/hikko` through `SYS_HTTP_GET` (2.6.29), and the ten demo ideas when the network, the reply or every row fails. Upvotes are local and never sent. `tools/checks/ring3hikko-check.py` drives the demo ideas, `tools/checks/ring3hikko-live-check.py` the live and fallback paths. |
| `user/reminders.c` | Reminders, a checklist of up to 24 items with a done flag kept in `REMINDERS.TXT`, as a ring-3 program: the fifteenth app out of the kernel (1.9.9). Same list, add prompt, tick and delete as the old in-kernel copy, and the file is rewritten whole after every change through the ordinary open, read, write and close calls, so it needed no new syscall. The list lives in the zeroed pages past the image, since a flat binary has no `.bss`. Samantha's reminder tools now read and write the same file fresh on every call from the ring-3 Samantha program. Driven by `tools/checks/ring3reminders-check.py`. |
| `user/curbfind.c` | Curbfind, Craigslist deals near the visitor ranked by deal score with the selected listing's price, neighbourhood, score bar and reason beside them, as a ring-3 program: the sixteenth app out of the kernel (1.9.11). Same list, detail pane and Vancouver samples as the old in-kernel copy. The live rows come through the one new call, `SYS_HTTP_GET`: the kernel fixes the host and port, the program names only the path, and a path that is not plain printable ASCII starting with `/`, or a buffer outside user memory, is refused before the network is touched. The `p` key is the probe `tools/checks/ring3curbfind-check.py` presses to prove those refusals. |
| `user/calendar.c` | Calendar, the Day, Week, Month and Year views with one event per day kept in `EVENTS.TXT`, as a ring-3 program: the seventeenth app out of the kernel (1.9.12). Same views, keys and layout as the old in-kernel copy, drawn in the 8x16 font (the Year view halves it). Today comes from `SYS_TIME`, so the epoch-to-date and weekday math run in the program; `tools/checks/check-calendar.sh` sweeps that math against libc on the host. The file is rewritten whole after every save through the ordinary open, read, write and close calls, so it needed no new syscall. The event list lives in the zeroed pages past the image, since a flat binary has no `.bss`. Driven by `tools/checks/ring3calendar-check.py`. |
| `user/mail.c` | Mail, the inbox list, reader and compose sheet over `MAIL.TXT`, as a ring-3 program: the twenty-second app out of the kernel. Its in-kernel UI is gone; `kernel/mail.h` is now just the message array and load/save. |
| `user/notes.c` | Notes, the folder and note browser plus the in-window editor (wrap, caret, scroll, line copy/cut/paste, Home/End/Delete, Ctrl+S), as a ring-3 program and a compositor window: the twenty-fourth app out of the kernel. Folders and notes are made and removed with `SYS_MKDIR` and `SYS_UNLINK`. |
| `user/terminal.c` | The Terminal, as a ring-3 program and a compositor window: the twenty-fifth app out of the kernel. Same look as before (dark page, scrollback, `> ` prompt pinned to the bottom, block cursor, hint line), drawn with libjt's antialiased DejaVu Sans Mono (`jt_mono_draw`, 8 px advance). It keeps its own working directory, shows it in the prompt, handles `cd` itself and sends the cwd with every `SYS_SHELL_RUN` call. Commands go through the one new call `SYS_SHELL_RUN`; copy, cut and paste use an in-app one-line clipboard. Backquote is the deliberate crash. |
| `kernel/shellsys.c` + `kernel/shellsys.h` | The allowlisted shell behind `SYS_SHELL_RUN` (help echo uptime mem ps ls cat, relative to the caller's cwd; plus a `crash` that exists only under the `panicdesk` boot flag, for the panic check). It is not `run()`: the syscall runs on the 4KB kernel stack with interrupts off, so it uses static buffers and refuses anything that blocks, waits on the network, opens a window or halts, and never moves the desktop's cwd. See docs/SYSCALL-ABI.md. |
| `kernel/stocks.h` | The data side of Stocks, now that the app is `user/stocks.c`: the quote table, the strict row parser, `stocks_fetch` (heap buffers, `/api/stocks?range=N`), `stocks_write_file` leaving `STOCKS.TXT` for the app, and `stocks_ring3_open`, the blocking path (phone grid, Apps folder) that fetches once and runs the app once. The window refetches through `SYS_REFRESH`. |
| `kernel/clockicon.h` | The Clock tile as a live analog face: white dial, ticks and hour, minute and centre hands drawn in physical pixels over the plain cached tile, redrawn when the minute changes. Also the one hook that lays Calendar's date and Clock's hands on a tile. |
| `user/search.c` | Search, the query box that filters the current directory as you type, as a ring-3 program: the eighteenth app out of the kernel (1.9.13). Same list, live filter, folder step-in and cat-style file view as the old in-kernel copy. It needed one new call, `SYS_READDIR`, which fills fixed-size records (name, size, is_dir) for a directory named by a path relative to the shell's directory; the kernel walks the path and walks back inside the call, so the app keeps its own cwd string and nothing outside it moves. `SYS_OPEN` takes the same relative paths, so a file inside a folder opens as `DOCS/NAME`. The records and the file buffer live in the zeroed pages past the image. Backquote is the deliberate crash. Driven by `tools/checks/ring3search-check.py`. |
| `user/burrow.c` | Burrow, the Files app, as a ring-3 program: a List and Icons toolbar, folders first, arrows to move, Enter to open a folder, Backspace to go up, Esc to close. It keeps its own folder path and lists it through `SYS_READDIR`. |
| `user/epiphany.c` | Epiphany, the markets terminal (a 40-ticker watchlist, portfolio, trading simulator, macro brief and the `/` command bar for `AAPL GP` and `AAPL DES`), as a ring-3 program: the nineteenth app out of the kernel (1.9.19). Prices come through `SYS_HTTP_GET` from `/api/quotes` (about 700 bytes, inside the 2 KB body), with the compiled-in prices when the fetch fails. The GP chart draws previous close to last price, because the intraday series lives in the kernel's Stocks and does not fit the syscall body. `tools/checks/ring3epiphany-check.py` drives it. |
| `user/weather.c` | Weather, as a ring-3 program: the twentieth app out of the kernel (1.9.22), made much richer in 2.10.0. The kernel's `weather_fetch` still does the network work, in one Open-Meteo request (current, 24 hours, 7 days with sunrise, sunset, UV and chance of rain), and writes `WEATHER.TXT` after every fetch (scalars, `d` lines per day, `hs/ht/hc/hp/hd` lines for the hours); the app reads it with plain file calls and shows the live reading, the last good one or labelled sample data in four sections (overview, hourly strip, 7-day bars, details), moved with Tab and the arrow keys. It is a real compositor window: R draws Fetching and calls `SYS_REFRESH` (398), the desktop loop refetches outside the gate, and the app reloads when `jt_sysinfo.data_stamp` moves. `tools/checks/ring3weather-check.py` drives it, `tools/checks/weather-app-check.sh` covers the live, stale, bad, timeout and offline faces. |
| `user/stocks.c` | Stocks, the watchlist sidebar with sparklines and a five-range line chart, as a ring-3 program: the twenty-first app out of the kernel. The kernel's `stocks_fetch` still does the network work and writes `STOCKS.TXT` after every fetch; the app reads the whole file with `jt_readfile` (one `jt_read` moves only 255 bytes). Arrow keys and clicks pick a symbol, a range change or R calls `SYS_REFRESH` (398) with `range | sel << 8`, the desktop loop refetches, and the window reloads when `data_stamp` moves. It is a real compositor window; Esc or a crash returns to the desktop. |
| `user/portfolio.c` | Portfolio, Joshua's About block and the fleet catalog grouped Life, Read, Make, Play and Dev, as a ring-3 program: the eleventh app out of the kernel (1.9.5). Same rows as the old in-kernel copy, up/down or the wheel skip the group headers, a click selects, and the bottom line shows the selected app's address. Backquote is the deliberate crash `tools/checks/ring3portfolio-check.py` can press. |
| `drivers/user_activity.h`, `user_calendar.h`, `user_clock.h`, `user_curbfind.h`, `user_epiphany.h`, `user_search.h`, `user_contacts.h`, `user_fieldbook.h`, `user_tonchi.h`, `user_portfolio.h`, `user_reminders.h`, `user_hikko.h` | The compressed bytes (raw deflate, with the raw length and an Adler-32) of those ring-3 apps, embedded in the kernel so it can start them; `ring3app_seed` unpacks and checks them at launch. Generated by `tools/gen/gen_user_bin.py` on every build, ignored by git, never edited by hand. |
| `drivers/user_brkpoke.h`, `user_burrow.h`, `user_calculator.h`, `user_keyrate.h`, `user_mail.h`, `user_hamurabi.h`, `user_windgate.h`, `user_movies.h`, `user_music.h`, `user_notes.h`, `user_samantha.h`, `user_stocks.h`, `user_terminal.h`, `user_weather.h` | The same, for the rest of the ring-3 apps and the break-on-purpose test program: compressed bytes embedded in the kernel, generated by `tools/gen/gen_user_bin.py` on every build, ignored by git, never edited by hand. |
| `kernel/ring3app.c` + `kernel/ring3app.h` | The table-driven launcher and supervisor for apps that run as ring-3 processes (`RING3_APPS`: name, embedded binary, VFS filename). Seeds the binary onto the VFS, runs it with `exec_user`, and when it exits or is reaped after a fault, logs what happened and hands the desktop back. Since 1.9.23 also `ring3app_launch_window`, the non-blocking launch behind a compositor window (see Ring-3 windows below). |
| `tools/checks/ring3window-check.py` | 1.9.23: Reminders as a ring-3 compositor window beside Notes. Both on screen, the crash key only reaches the focused window, and the crash closes only its own window. |
| `kernel/irqlock.h` | 1.9.23: `irq_save` / `irq_restore`, the one lock a uniprocessor kernel needs. A cli critical section that hands back the caller's own IF, used by kheap, vfs, the window event ring, task_kill and the window launcher. |
| `kernel/r3stress.c` | 1.9.23: the `stress=r3` boot flag. The desktop churns the heap and writes inside NOTES/ every frame while every SYS_WINDOW_POLL from a ring-3 window does the same under the gate; after 400 frames it walks the heap and reports to serial. Dead code unless armed. |
| `tools/checks/ring3stress-check.py` | 1.9.23: boots `open=remi stress=r3` on a real FAT disk, asserts both sides ran, no block was handed to two owners, `kheap: ok`, the syscall's file stayed at the root, the desktop finished. The lockless kernel hangs inside the first seconds. |
| `tools/checks/pdesync-check.py` | 1.9.24: boots `open=remi stress=pde`, grows the kernel heap into a 4 MB region that had no page table when the ring-3 task was created, has the task write and read it through SYS_WINDOW_POLL on its own CR3, then closes it with Esc. Without `paging_sync_task_dirs` it logs the BUG line and faults. |

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
| `/api/books` | The real Bookrank shelf (top twelve ranked books) as plain text for the Bookrank app. |
| `/api/hikko` | The real Hikko forum's top twelve ideas as plain text (`votes|category|title|text|plan`), ASCII only, read-only, for the Hikko app. |
| `/api/lexly` | The real Tonchi (was Lexly) course list as plain text, and with `?c=<id>` up to twenty four-choice questions from one course, ASCII only, for the Tonchi app. |
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
| `drivers/inflate.c` + `drivers/inflate.h` | The kernel's one DEFLATE inflate (plus Adler-32). The PNG decoder and the app unpacker in `kernel/ring3app.c` both use it, so there is a single copy to trust. |
| `tools/inflate-host/main.c` | Host harness for `drivers/inflate.c`. `tools/checks/user-compress-check.py` builds it with every app header and requires each embedded app to inflate to its exact binary and every damaged copy to be refused. |
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
| `kernel/ttf_render.h` | The shared glyph path for anything drawing real DejaVu text at physical resolution: a per-face cache, a glyph cache, the antialiased ink blend. Notes and the Terminal both draw through it. |
| `kernel/gui_prims.c` | Tiny pure helpers split out of `kernel.c`: blend two colours, square root for antialiased lines. |
| `kernel/dock_geom.c` | Dock geometry and hit-testing: where each icon sits at the current scale, and which slot a click landed on. |
| `kernel/dock_draw.c` | The dock's pixel drawing: the band cache behind hover/drag animation, the tray, the icons and the hover label. Split out of `kernel.c`, sits on top of `dock_geom.c`'s layout math. |
| `kernel/apps_geom.h` | Where the Launchpad (Apps folder) puts its glass panel, icons and labels. Pure integer math, centered on any screen, rows drop to fit a short one. |
| `kernel/hint.h` | The keyboard-hint line every app draws ("esc closes" and friends), skipped on phones where there is no keyboard. Split out of `kernel.c`. |
| `kernel/app.h` | The app interface. One `struct app` per app (name, tile color, glyph, `open`, and `draw`/`key` for apps that run in a desktop window), plus the few desktop services and helpers an app in its own file needs. |
| `kernel/app.c` | The helpers behind `app.h`, only ones two or more apps were writing by hand: start an app's window with its titlebar, print a number. |
| `kernel/bench.h` | Built-in benchmarks: boot time, heap, memcpy, context switch, disk read. `bench` in the shell or on the command line. Results in `docs/BENCHMARKS.md`. |
| `kernel/auth.h` | User accounts. PBKDF2-HMAC-SHA256 records in `USERS.TXT` (scheme tag and iteration count stored per line), verify-only support for the pre-1.7.9 chained-SHA-256 records with an in-place upgrade on the next successful login, the login and first-run screens. Built against `docs/THREAT-MODEL.md`. |
| `kernel/auth_kdf.c` + `kernel/auth_kdf.h` | PBKDF2-HMAC-SHA256 (RFC 8018) as one block loop over BearSSL's `br_hmac`; no primitive of its own. Compiled for the kernel and natively for `tools/auth-host`, which pins it to the RFC 7914 and RFC 6070 (SHA-256) vectors. |
| `kernel/wall_sat.h` | A real satellite photo, baked in, used as the wallpaper when there is no network to fetch map tiles. |
| `kernel/boot_mark.h` | The real landing brand mark (`landing/logo.svg`, the four-arm Joshua tree), rasterized by `tools/gen/gen_boot_mark.py` into 8-bit alpha coverage at the splash's real physical size and blended straight onto the boot screen by `gui_draw_boot_mark` (`kernel/kernel.c`), replacing the old `gui_draw_logo` stick-figure primitive there. The menu bar keeps drawing `gui_draw_logo` unchanged, since the engraved-style mark reads as a solid blob at 16px. |
| `kernel/phone_home.h` | The phone home screen for `boot_to_phone`: a 5-column, no-scroll grid of all 26 apps, a status bar with the real clock and weather, and a tappable back chevron in place of the desktop's traffic lights (drawn as two bold stepped diagonal strokes, injects a real Esc scancode through `kbd_inject()` so every app closes through the one `kbd_pop()==27` path a keyboard already drives). Desktop mode never calls into it, so it stays pixel-identical. |
| `kernel/settings_ui.h` | Settings app UI with a sidebar plus grouped detail pane (modeled on macOS System Settings). Pulled into its own file in the 1.7.x redesign pass to keep `kernel.c` under the godfile-check.sh ceiling; `#include`d directly into `kernel.c` at the exact spot the inline version used to sit. |

## Ring-3 windows (1.9.23)

Until 1.9.23 a ring-3 program was modal: `exec_user` blocked the desktop loop until the program exited, the program owned the one app viewport, and `SYS_WINDOW_POLL` pulled keys straight off the keyboard ring. Two programs could not even run at once, because every task directory shares the base page tables by value, so `JT_USER_BASE` and `JT_USER_FB` were the same physical frames in every task.

The window path changes four things. The desktop never falls back to the blocking path any more: a full window table or failed launch is refused with a notice and a `winrefuse: <App>` serial line (`gui_refuse_open`). The blocking path remains only for the phone home grid, the Apps folder and the text shell:

- `exec_user_window` (kernel/exec.c) loads the image and builds the stack in a kmalloc'd buffer of the window's own and returns the task id at once. `paging_task_map_private` (kernel/paging.c) gives that task's directory a private copy of the 4 MB page table that holds the user window, with the image, stack and framebuffer entries pointing at frames only this task maps. The low identity alias keeps the shared supervisor-only table, so no task reaches another's pages through it, and `paging_user_range_ok` now walks CR3, so a syscall pointer is judged by the caller's own tables.

**Kernel PDEs are shared by write-through, not by copy.** A task directory is a by-value copy of the kernel directory taken at `task_create`, which was a trap: the heap grows through `paging_map_region`, and a 4 MB region that got its page table after the task was born existed only in the shared directory, so the first syscall or IRQ on that task's CR3 to touch it page-faulted at ring 0 (Mail's close inside `r3win_release`). Preallocating every kernel table is the textbook fix but the wrong trade here: the identity map covers physical frames on demand and the LFB sits at 0xFD000000, so a full set is 4 MB of BSS against 64 KB today. Instead `kernel/paging.c` treats the shared directory as the one source of truth and `paging_sync_task_dirs` writes every kernel PDE create or remove through to every live task directory on the spot (zero extra memory, a six-slot loop per table change, and table changes happen once per 4 MB). The two slots a task owns privately, `PAGING_PRIVATE_PDE` and the user window table at `KERNEL_PDE_INDEX + 1`, are skipped, so `paging_task_map_private` is untouched. `paging_check_task_dirs` compares every live copy against the kernel entries and logs `BUG: paging: task directory drifted` if one differs; `stress=pde` (kernel/r3stress.c) and `tools/checks/pdesync-check.py` arm the whole path headlessly.
- `syscall_window_register` (kernel/syscall.c, `r3wins`) allocates the framebuffer, maps it at `JT_USER_FB` in that task only, and keeps a 16-deep event ring per window. `SYS_WINDOW_OPEN` hands back the same `jt_window_info` as before. `SYS_WINDOW_POLL` with `JT_POLL_PRESENT` marks the window dirty instead of copying anything, and drains the ring; `-EAGAIN` when it is empty.
- The compositor (kernel/kernel.c, `gui_window_t.task`) blits a dirty window's buffer through `window_pixel` at the window's position with the usual chrome, and feeds keys and content clicks only to the topmost window's ring (`syscall_window_push_event`). The red dot kills the task. Every frame it looks for a window whose task is gone (exit or a fault reaped by idt.c), logs the same crash line the blocking launcher does, and closes that one window.
- Resize is real (1.10): `ring3app_window_blit` compares the viewport with the window's buffer and `syscall_window_request_size` queues one `JT_EV_RESIZE`; the app re-opens (`jt_window_resized` in user/jtsys.h) and `r3win_resize` maps a fresh zeroed buffer with `paging_task_remap_private`, freeing the old one on the compositor's next look so a blit in flight never reads freed memory. Until then the old buffer is blitted clipped. `tools/checks/ring3resize-check.py` proves it.
- `gui_ring3_windowed` names the apps on this path. Reminders is the proof; adding an app is one row. Burrow, Notes and Mail can move to ring 3 on it without losing multi-window.

**The concurrency rule.** One CPU, so one rule: shared kernel state is only ever touched with interrupts off. A syscall already runs that way end to end (int 0x80 is an interrupt gate) and the only call that turns them back on, `SYS_HTTP_GET`, holds nothing while it waits: http.c refuses a second fetch while one is in flight, from the desktop or the gate. The desktop (task 0) runs with interrupts on and can be preempted anywhere, so every entry point it shares with a syscall is an `irq_save`/`irq_restore` section (kernel/irqlock.h): `kmalloc`/`kfree`, every `vfs_*` call (which covers FAT, ramfs and the disk), `syscall_window_push_event` into the per-window ring, `task_kill`'s frame patch, and the whole of `ring3app_launch_window` (a slot is marked used before its private mapping exists). The FAT cwd is one global cursor the desktop may have left inside NOTES/, so a syscall path resolves from the root and puts the cursor back (`vfs_cwd_get`/`vfs_cwd_set`). `kheap_check` walks the free list on demand and `tools/checks/ring3stress-check.py` hammers both sides at once to keep it that way.

**The memory map.** The kernel is linked at 0xC0100000 and its .text, .rodata, .data and .bss run upward from there; every ring-3 app binary is baked into .rodata as raw deflate (the generated `drivers/user_*.h`, about half the raw size since 2.7.1), so each new app pushes .bss higher, only more slowly. Above .bss sit three fixed, NOLOAD reservations that `pmm.c` never hands to the heap: the 132 KB program window at `JT_USER_BASE` (`JT_USER_IMAGE_PAGES`, 32 pages of image, plus 1 of stack), the 64 KB Sound Blaster DMA buffer at `JT_DMABUF_BASE`, and the ring-3 window framebuffer at `JT_USER_FB`. All three are written down once, in `kernel/memmap.h`; the Makefile turns that header into `boot/memmap.ld`, which `boot/linker.ld`, `user/hello.ld` and `user/note.ld` INCLUDE, so the loader, the reservations and the flat binaries' link address cannot drift. They have to stay inside the second 4 MB kernel page table (0xC0400000 to 0xC07FFFFF) because `paging_task_map_private` copies that one table for a task's private window; `boot/linker.ld` asserts that, plus the order and the DMA alignment. On 2026-10-01 the window moved up 512 KB (0xC0507000 to 0xC0587000) and `tools/checks/bss-margin-check.py` now reports about 544 KB between .bss and the window, with a 16 KB floor. Later that day the image window grew from 7 pages to 32 (28 KB to 128 KB, Samantha and Stocks had both closed to within 1 KB of the old ceiling; a brief detour to 128 pages for Samantha's face frames was undone once `SYS_BRK` landed, see below); `JT_USER_BASE` stayed put, `.dmabuf` and `.userfb` slid up 128 KB (0xC05B0000 and 0xC05C0000, ending at 0xC0730000, still inside the table; in 2.5.1 `.dmabuf` and `.userfb` slid back down into the gap under the program window, to 0xC05B0000 and 0xC05C0000, and `JT_USER_FB_BYTES` grew to 0x220000 so a 960x540 window fits, ending at 0xC07E0000; on 2026-10-04 all three moved up another 64 KB when Hamurabi's binary took the gap under 16 KB: `JT_USER_BASE` is now 0xC0597000, `.dmabuf` 0xC05C0000 and `.userfb` 0xC05D0000, ending at 0xC07F0000, so 64 KB of the table is left above it and about 62 KB between .bss and the window; playable Hamurapi then grew its binary from 54 KB to 81 KB, which left about 34 KB, against the 16 KB floor; 2.7.1 stored the app binaries compressed, which took the gap to about 438 KB), `kernel/exec.h`, both user link scripts and `drivers/ramfs.c` take the size from the header, and ramfs now allocates each file to its actual length instead of the cap.

**The task heap (1.9.27).** Anything larger than the image window goes through `SYS_BRK` (`kernel/brk.c`). Each task's heap starts at `JT_BRK_BASE` (0xFF000000, `kernel/memmap.h`) and grows up to `JT_BRK_MAX_PAGES` (8 MB), in PDEs 0x3FC and 0x3FD: above every identity alias `paging_map_region` can create (physical RAM and the 0xFD000000 framebuffer both sit lower), far from the private window PDE 19 and the user image, stack, `.dmabuf` and `.userfb` inside the second kernel table, so a task directory snapshot of the kernel's PDEs never collides with it and nothing the kernel maps later overwrites it. Frames come from the PMM one at a time, are zeroed, and are mapped user and writable in that task's directory only; the syscall switches to the kernel directory while it edits tables (a task directory misses pages the kernel mapped after it was made) and back, which also flushes the TLB. Exit and crash both reach `brk_release` from `syscall_release_task`, which frees every frame and both page tables and logs `brk: released N pages, live=M`. `user/libjt`'s `malloc` sits on it, and Samantha keeps all 72 face JPEGs there (about 1.5 MB), decoding the frame on screen on demand.

## Samantha at ring 3 (1.9.26, done)

Samantha (`user/samantha.c`) was the twenty-sixth and last app inside the kernel; her old in-kernel chat and face are deleted, and `kernel/phone_home.h` stays because it is the phone's home screen and app grid, the OS shell, not her UI. The reason she is last is that she is the only app that touches everything: the network, the sound card both ways, kernel-only state like the weather fields, and the launcher. The rule for the move is the same as every port before it: the fewest new syscalls that keep the host, the devices and the desktop's launch path on the kernel side, every user pointer through `paging_user_range_ok`, nothing that blocks with interrupts off except around a network wait, and nothing a program can point anywhere the kernel did not choose.

| Capability | In the kernel today | At ring 3 |
|---|---|---|
| Chat request and reply (`/api/chat`, `/api/pick`) | `http_post_timeout` to `llm_host:llm_port` with a 6 KB request and an 8 KB reply, 45s and 10s budgets | `SYS_HTTP_POST` (392): one struct argument, path only, host and port stay the Settings-owned `llm_host`/`llm_port`, body and reply bounce through kernel buffers, 6 KB in, 8 KB out, reply budget clamped to 45s. No streaming: the Worker already answers `stream:false`, and a streaming reply would need the gate to hand back partial bodies with IF on, which nothing else does. |
| Audio out (`/api/speak`) | `chat_face_speak` fetches the WAV and `sb16_play`s it, with a progress hook that drives the mouth frames | The program fetches the clip itself with `SYS_HTTP_POST` (the Worker returns 8-bit mono PCM under 8 KB per sentence; longer answers are sent one sentence at a time, the same split `speak.c` already does) and queues it with a new `SYS_AUDIO_PLAY` (393): user PCM copied into the kernel DMA buffer, returns at once, the program polls `SYS_AUDIO_STATUS` (393 with a flag) for the sample position to time the mouth. The kernel never fetches on the program's behalf: that would put a URL and a text body inside the gate for no gain. |
| Audio in (F2 push-to-talk, `/api/listen`) | `sb16_record` in 2s DMA chunks into a kmalloc'd 8s buffer, posted raw | `SYS_AUDIO_RECORD` (394): fills a user buffer with up to `n` samples at 16 kHz, blocks for one chunk with IF on like `http_get` does, returns the count; -ENODEV without a card. The program keeps the 8s cap and posts the samples with `SYS_HTTP_POST` (body cap rises to 128 KB only if the measured upload needs it; the first cut keeps 6 KB and trims the clip). F2 arrives as a normal key event through `SYS_WINDOW_POLL`. |
| Face | the old kernel face code drew frames from baked arrays with the `font`/`window` primitives | Pure app drawing into the window framebuffer with libjt; the frame arrays move into the user binary. Nothing kernel-side. |
| Reminders, notes, mail, calendar | `vfs_*` on REMINDERS.TXT, NOTES/NOTES, MAIL.TXT, EVENTS.TXT | Existing `open`/`read`/`write`/`readdir`/`mkdir` syscalls; these are files, and Reminders, Notes, Mail and Calendar already read and write them from ring 3. No new call. |
| Weather | `weather_text`/`weather_have` kernel fields | `SYS_SYSINFO` (395): fills a small read-only struct (weather text, model name, host name, time) from kernel state. Read only, one struct, one range check. |
| `open <app>` | `chat_launch_after` set, the desktop loop launches through `gui_apps_launch` | `SYS_LAUNCH_REQUEST` (396): the kernel checks the name against `APPS[]` and stores one pending index; the desktop loop (task 0, IF on) picks it up and runs the normal `gui_apps_launch` path. Nothing is launched inside the gate: `ring3app_launch_window` is an irqlock section the desktop owns, and the program only ever asks. |
| Phone mode (`phone`, `samantha` cmdline) | `phone_home.h` draws the home screen in kernel | The same binary with an argv flag (`samantha --phone`), launched by `kmain` through `ring3app` in place of the desktop; the phone home grid moves into the program and uses `SYS_LAUNCH_REQUEST` for its icons. |

Slices, each sized for one Sonnet agent in about ten minutes, in order:

1. `SYS_HTTP_POST` kernel side (done, this section). Sonnet.
2. (done) The samantha.c program skeleton under user/: window, conversation list, keyboard, `/api/chat` and `/api/pick` through `SYS_HTTP_POST`. Sonnet.
3. The tools: reminders, notes, mail, calendar through the file syscalls, moved line for line from `chat_run_tool`. Haiku.
4. `SYS_SYSINFO` + weather tool, `SYS_LAUNCH_REQUEST` + `open <app>`, desktop pickup in the main loop. Sonnet.
5. The face: frame arrays into the user binary, libjt drawing, idle and talking states. Haiku.
6. `SYS_AUDIO_PLAY`/`SYS_AUDIO_STATUS` plus the per-sentence speak path and mouth timing. Sonnet.
7. `SYS_AUDIO_RECORD` plus push-to-talk and `/api/listen`. Sonnet.
8. (done) Ring-3 Samantha is the default: dock slot 6, the `chat` and `samantha` shell commands, `boot_to_samantha` and phone mode (argv `phone`, as Notes gets it) all launch her; the in-kernel chat and face are deleted, `phone_home.h` stays and opens her through `gui_apps_launch`; 26 of 26. `facehost=` moved to `syscall.c` and now sets where her face and speech are fetched. Sonnet.

Checks that must keep passing (`tools/checks`): `samantha-fullscreen-check.py` (2.9.0: covers the screen, glass, fading captions, scrollback, the two typing bugs, the mouth follows the voice, Esc and the red dot; 2.9.1: a second machine watches her blink on her own clock, every other machine boots with `noblink`), `chat-face-check.py`, `chat-live-check.sh`, `chat-samantha-check.py`, `chat-speaks-check.py`, `chatapp-check.py`, `chattools-check.py`, `demochat-check.mjs`, `face-frames-check.py`, `facespeak-demo-check.mjs`, `phone-boot-check.py`, `phone-reboot-check.mjs`, `phone-samantha-back-check.py`, `samantha-boot-check.py`. They grep serial lines (`chattool=`, `chat=`, face frame counts); the ring-3 program must print the same lines through `SYS_WRITE` to the serial fd so the checks move over untouched, the way the Terminal and Notes ports did.

## The apps

Twenty-six apps live in the Apps folder; the dock pins the ones you reach
for most. Each is one header. They come in three shapes.

Renames (2.6.30): Lexly is Tonchi, Roost is Brick, Voxprint is Notate, Siftbox is Hagaki and Sparkjar is Hikko. Samantha still opens an app by its old name. Tonchi keeps `lexly.heyitsmejosh.com` and `/api/lexly` until its own address exists. Block Frame still answers at its old `wiretext` address.

**Apps with a file on disk.** Same pattern every time: a fixed-size array
in RAM, one plain text file on the FAT disk, written through on every
change. No Save button.

| App | File | On disk |
|---|---|---|
| Notes | `user/notes.c`, `kernel/editor.h` | Folders and many notes under `NOTES/`; an old `NOTES.TXT` moves in as the first note. Notes is the twenty-fourth ring-3 app; `kernel/editor.h` keeps only that migration, the shared pointer position and the DejaVu glyph tables. |
| Reminders | `user/reminders.c`, `kernel/ring3app.c` | `REMINDERS.TXT`, one line per item. The fifteenth ring-3 app (1.9.9); its in-kernel copy is gone. |
| Calendar | `user/calendar.c`, `kernel/ring3app.c` | `EVENTS.TXT`, one line per date. The grid itself is computed from the clock. The seventeenth ring-3 app (1.9.12); its in-kernel copy is gone. |
| Mail | `user/mail.c`, `kernel/mail.h` | `MAIL.TXT`, one line per message. Mail is the twenty-second ring-3 app (a compositor window, compose sheet inside it); `kernel/mail.h` keeps only the data layer Samantha's read_mail and send_mail use, reloaded from disk on every call. Two starter messages ship compiled in. |
| Contacts | `user/contacts.c`, `kernel/ring3app.c` | `CONTACTS.TXT`. The thirteenth ring-3 app (1.9.7); its in-kernel copy is gone. |
| Samantha | `user/samantha.c`, `kernel/ring3app.c` | `CHAT.TXT`. Talks to the Worker through `SYS_HTTP_POST`. Push-to-talk: holding F2 records through `SYS_AUDIO_RECORD` and posts the clip to the Worker's `/api/listen` (Cloudflare Workers AI Whisper); the recognized text runs through the same path a typed message takes. Full screen since 2.9.0: her 736 pixel portrait (`landing/face/hd.jpg`, one fetch of 59 KB through the Worker's `/face/` route) covers the window, the input line is a blurred glass panel, what is said shows as captions that fade, Tab opens the whole conversation, a red dot or Esc closes her, and her mouth is drawn by code from the loudness of the PCM she is playing. A host without the portrait leaves the old 320 pixel idle and talk frames, scaled up. The twenty-sixth ring-3 app (1.9.26); the in-kernel chat is gone. |

**Apps with nothing to save.**

| App | File | What it is |
|---|---|---|
| Stocks | `kernel/stocks.h` | Eight fixed symbols with live quotes and charts from the Worker at `/api/stocks`; the UI is `user/stocks.c`. |
| Epiphany | `user/epiphany.c`, `kernel/ring3app.c` | Watchlist, portfolio, simulator and a Bloomberg-style command bar (`/`, then `AAPL GP` or `AAPL DES`), live prices through `SYS_HTTP_GET`. The nineteenth ring-3 app (1.9.19); its in-kernel copy is gone. |
| Search | `user/search.c`, `kernel/ring3app.c` | Filters the current directory as you type. Enter opens a folder or shows a file. Scoped to what the VFS can list, no whole-disk index. The eighteenth ring-3 app (1.9.13); its in-kernel copy is gone. |
| Portfolio | `user/portfolio.c`, `kernel/ring3app.c` | A catalog of every app in the fleet with its URL. The eleventh ring-3 app (1.9.5); its in-kernel copy is gone. |
| Activity | `user/activity.c`, `kernel/ring3app.c` | Activity Monitor over the real scheduler and memory counters. Refreshes on a timer, can kill a task. The twelfth ring-3 app (1.9.6); its in-kernel copy is gone. |
| Clock | `user/clock.c`, `kernel/ring3app.c` | Current time from the RTC, a countdown timer you can start and pause, and an alarm. The tenth ring-3 app (1.9.4); its in-kernel copy is gone. |
| Music | `user/music.c`, `kernel/ring3app.c` | Plays the `.WAV` songs in Files and in a `MUSIC` folder. Keeps nothing on disk. The twenty-fifth ring-3 app (2.2), in the Apps folder. |
| Movies | `user/movies.c`, `kernel/ring3app.c` | Plays motion-JPEG AVI clips from Files with sound, pause, seek and fullscreen. In the Apps folder, opened with `open=movi`. A ring-3 app from the start (2.2). |

**Apps ported from the fleet.** Each is a native rewrite of one of the
sibling web apps, kept small on purpose.

| App | File | What it is |
|---|---|---|
| Curbfind | `user/curbfind.c`, `kernel/ring3app.c` | Craigslist deals near you, ranked by score, live through `SYS_HTTP_GET` with Vancouver samples when offline. The sixteenth ring-3 app (1.9.11); its in-kernel copy is gone. |
| Tonchi | `user/tonchi.c`, `kernel/ring3app.c` | Learn anything: pick a live course, then a four-choice drill with a score. Falls back to the Spanish deck when offline (2.6.27). The seventh ring-3 app (1.9.1); its in-kernel copy is gone. |
| Fieldbook | `user/fieldbook.c`, `kernel/ring3app.c` | Every field of science and math, explained plainly. The ninth ring-3 app (1.9.3); its in-kernel copy is gone. |
| Hikko | `user/hikko.c`, `kernel/ring3app.c` | The idea forum: the top ideas from the real forum, vote on them for the session only. Falls back to ten demo ideas when offline (2.6.29). The fourteenth ring-3 app (1.9.8); its in-kernel copy is gone. Renamed from Sparkjar (briefly Hotaru) in 2.6.29; Samantha still opens it by either old name. |
| Keyrate | `user/keyrate.c`, `kernel/ring3app.c` | Typing test with endless random words and a live words-per-minute count. The first app running outside the kernel as a ring-3 process (1.7.7); its in-kernel copy is gone. |
| Toroid | `user/toroid.c`, `kernel/ring3app.c` | Conway's Life on a torus. The second ring-3 app (1.7.11); its in-kernel copy is gone. |
| Calculator | `user/calculator.c`, `kernel/ring3app.c` | Recursive-descent parser over `+ - * / ()`. The third ring-3 app (1.7.12); its in-kernel copy is gone. |
| Quotes | `user/quotes.c`, `kernel/ring3app.c` | Name the film from the line. Streak and best for the session. The fourth ring-3 app (1.7.14); its in-kernel copy is gone. |
| Bookrank | `user/bookrank.c`, `kernel/ring3app.c` | Ranked non-fiction: a list on the left, the selected book's title, author, rating, review count and summary on the right. Live through `SYS_HTTP_GET` with samples when offline (2.6.26). The fifth ring-3 app (2.0); its in-kernel copy is gone. |
| Hamurabi (rules) | `user/hamurabi_rules.h`, `user/hamurabi_story.h`, `art/hamurabi/story.json` | The game's rules, robot king and story logic as plain C with no libc, no floats and no 64-bit division, so an app can include it as is. The story file is the one place for the words; the header is generated from it. Held to the other three ports by two golden numbers (`tools/checks/hamurabi-rules-check.sh`). |
| Hamurapi | `user/hamurabi.c`, `user/hamurabi_sprites.h`, `art/hamurabi/sprites.png` | The 1968 game of Hamurabi, rebuilt with a living city, shown as Hamurapi because the store name was taken (the files, `HAMURABI.BIN` and `open=hamurabi` keep the old spelling, and so does the credit to the 1968 game). In the Apps folder, opened with `open=hamu`. Playable (2.7): story with cards, Classic 1968, and the robot's demo, from the title to the graded ending. |
| Windgate | `user/windgate.c`, `kernel/ring3app.c` | Guided breathing: a circle grows on the in-breath, holds, shrinks on the out-breath. Four presets (Box, 4-7-8, Relax, Coherent), a countdown, a round counter and a session timer. Fully offline. In the Apps folder, opened with `open=windgate`. A ring-3 app from the start (2.8). |

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
the `kheap.h`, `libc.h` and `irq.h` shims next to each one are the fakes (`tools/parsers-host/irq.h` stands in for the kernel's tick counter so timeout loops stay finite on the host).
`tools/png-host/wallsat_check.c` decodes the baked satellite photo and
checks it comes out 960x540. `drivers/png_testdata.h` and
`drivers/jpeg_testdata.h` are generated test images.

**Fuzzers** in `tools/fuzz-host/` hammer the code that reads untrusted
bytes. `fuzz_parsers.c` covers HTTP, HTML and JSON; `fuzz_decoders.c`
covers PNG and JPEG. Both run under address and undefined-behaviour
sanitizers, using the shims in `tools/parsers-host/`.

**Generators** in `tools/gen/` produce the embedded data: the icon art
from hand-drawn SVGs, the editor fonts, the VGA font fallback, the
satellite wallpaper, the user program arrays, the ported app arrays, the
fact row and progress chart on the landing page, and Hamurabi's story data
(`gen_hamurabi_story.py` turns `art/hamurabi/story.json` into `user/hamurabi_story.h`; `gen_hamurabi_sprites.py` turns `art/hamurabi/sprites.png` into `user/hamurabi_sprites.h`).

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

The ad lives in `landing/ad/` and plays live from the same clock the video was captured from.

| File | What it does |
|---|---|
| `landing/ad/index.html` | The Joshua Tree 2.0 and Neo Kit promo, one self-contained page. A single clock function maps t to every camera and scene state: the real Neo STLs as an engraved-ink three.js case that explodes ring by ring, a 2D dive through the board, chip and die, a glyph field resolving into the OS's real 26 app icons, each in its own thin-walled box, one pixel growing into a real 1920x1080 capture of the desktop with Samantha's face frames (an eased nod and a blink from her frame set when the typed line lands), then the 2.0 mark and end card. The case is laid out from `docs/hardware/stl/manifest.json` and `neo_cad.py` numbers, not by eye. Exposes `window.__ad = {duration, render(t)}` and a `?t=` query for stills. |
| `landing/ad/vendor/three.module.min.js` | Vendored three.js r160 (pinned, MIT) so the ad has no runtime network dependency. |
| `landing/ad/vendor/STLLoader.js` | Vendored three.js r160 STLLoader (MIT), loads the real Neo parts. |
| `landing/ad/stl/*.stl` | Copies of the `docs/hardware/stl` parts, so the live ad page loads them from its own folder on the deployed site. Re-copy them if a part changes. |
| `tools/ad-capture.mjs` | Deterministic capture for the ad: serves the repo, steps `window.__ad.render(t)` frame by frame in headless Chromium across parallel workers, writes PNG frames or stills; ffmpeg encodes them to h264. |
| `landing/ad/desktop-a.jpg` | Real 1920x1080 framebuffer capture of today's desktop with Samantha snapped left and Notes snapped right, her input empty. Made by `tools/ad-desktop-capture.py`; the ad grows it from one pixel. |
| `landing/ad/desktop-typed.jpg` | The same desktop with "Hi Samantha" typed into her input and not yet sent, shown from 23.2 s so the nod has something to react to. |
| `landing/ad/desktop-b.jpg` | The same desktop one beat later: "Hi Samantha" sent and her reply showing, swapped in when the typed line lands. |
| `landing/ad/icons/*.png` | The 26 app icons as the OS draws them: 23 rendered from `art/icons/*.svg`, plus Portfolio, Activity and Clock cropped from the Apps grid capture and masked to the same tile shape. |
| `tools/ad-desktop-capture.py` | Headless QEMU run that drives the real kernel over QMP (Apps grid scroll, snap Samantha and Notes, send a line) and saves the 1920x1080 framebuffer for the ad. |
| `tools/ad-audio.py` | Narration and sound for the ad: Samantha's ElevenLabs voice per line, the `docs/hardware/ad/music2.py` bed cut to 30 s, ffmpeg places each line on its beat and ducks the bed under the voice. |
| `landing/ad.mp4` | The web cut of the ad: 1280x720 h264 with narration and music (aac), under 5 MB, faststart, played on the landing page. Encoded from the 1080p capture with ffmpeg. |
| `landing/ad-poster.jpg` | Poster frame for the ad video, a still of the exploded case. |

## Where to go next

- `docs/BENCHMARKS.md` for how fast it is, and `tools/bench.sh` to measure it yourself.
- `docs/WHITEPAPER.md` for why this exists and what it is not yet.
- `docs/SYSCALL-ABI.md` if you want to write a program for it.
- `docs/BLUEPRINT.md` for the plan after 1.0: apps as real processes and a window server.
- `docs/THREAT-MODEL.md` for what the login screen does and does not protect.
