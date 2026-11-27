# Joshua Tree architecture and reference

How it is built, the rules it keeps, how it is tested and secured.

## Architecture

Joshua Tree is a whole computer written from nothing.

<details><summary>Details (618 lines)</summary>


You turn it on and it draws a desktop. You use its apps: notes, mail, a calendar, weather, a chat that can set reminders for you.

Everything it does, it does itself. There is no Linux underneath and no borrowed library.

It runs in three places:
- in your browser, on the landing page
- in QEMU on a Mac
- from a USB stick on a real PC

It is all one file, `kernel.elf`, built from the `boot/`, `kernel/`, `drivers/` and `lib/` folders.

`docs/ROADMAP.md` (Roadmap) is the plan. This page is the map of what exists today.

### How it starts

1. **Something loads it.** QEMU, the browser demo or a real bootloader drops the kernel into memory and jumps to `boot/boot.S`.
2. **It sets up its own memory.** A few lines of assembly move the kernel to where it expects to live, then hand off to C.
3. **It wakes the hardware, one piece at a time.** `kmain` starts the processor tables, the clock, memory, the scheduler and the disk. Each one needs the last.
4. **You land in a shell.** Type `gui` and the desktop starts. The shell and the desktop are the same program. The Terminal is the shell in a window.

### Boot modes

Pass keywords on the multiboot command line to change how the kernel starts. Use QEMU's `-append` flag or the browser emulator's cmdline config.

| Flag | What it does |
|---|---|
| `phone` | Boots into portrait phone mode (430x932) instead of the desktop (960x540 retina). |
| `samantha` | Skips the desktop and opens the ring-3 Samantha program (`user/samantha.c`) first. |

### The layers

Each layer only leans on the ones above it on this page, so it reads top to bottom.

#### CPU setup and interrupts

| File | What it does |
|---|---|
| `boot/boot.S` | The first code that runs. Sets up just enough memory to reach C. |
| `kernel/gdt.c` | Tells the processor what is kernel and what is a program, so programs can safely call into the kernel. |
| `kernel/idt.c` + `kernel/isr.S` | The interrupt table and the 32 CPU exception handlers. A kernel-mode fault paints a panic screen and halts. |
| `kernel/symtab.h` + `kernel/backtrace.c` | Crash reports with real function names. |
| `kernel/pic.c` | Routes hardware signals, like a key press, so they never get confused with processor errors. |
| `kernel/irq.c` + `kernel/irq_stubs.S` | The hardware interrupt handlers. The timer tick drives the scheduler, the keyboard fills a ring buffer. |
| `kernel/entropy.c` + `kernel/entropy.h` | The kernel's only source of unpredictable bytes: an HMAC_DRBG (BearSSL SHA-256) seeded from RDRAND when the CPU has it, RDTSC jitter and interrupt timing. |
| `kernel/picker.c` + `kernel/picker.h`, `kernel/picker_weights.h`, `kernel/picker_golden.h`, `tools/checks/picker-check.sh` | 3.8.5: a small tool picker in freestanding C, Samantha's hands distilled (turing 10.0 slice). |
| `kernel/brk.c` | `SYS_BRK` (1.9.27): a per-task heap at `JT_BRK_BASE` grown a frame at a time from the PMM, zeroed, mapped into that task's. |
| `kernel/syscall.c` | The `int 0x80` dispatch table. |

#### Memory

| File | What it does |
|---|---|
| `kernel/pmm.c` | Physical memory as a bitmap of 4KB frames, sized from what the bootloader reports. |
| `kernel/paging.c` | Decides which memory each program can see, and keeps programs out of the kernel's. |
| `kernel/kheap.c` | `kmalloc` and `kfree`. A first-fit free list that grows one frame at a time. |
| `lib/text_ink.h` | The shared coverage curve: gives kernel and ring-3 text dense stems while keeping smooth edges; light text uses contrast only. |
| `lib/ease.h` | 6.4.0: the one motion curve, `ease_out`, 1 - (1 - p)^5 in thousandths (close to the web's `cubic-bezier(.23,1,.32,1)`), shared by the i386 boot mark. |
| `lib/libc.c` | `memcpy`, `memset`, `strlen` and the other handful of primitives a freestanding kernel cannot live without. |
| `third_party/bearssl/` | Vendored BearSSL subset (upstream 7bea48e, MIT): 90 flat `.c` files in `src/` for a minimal TLS 1.2 client (SSL client core and. |

#### Tasks and user programs

| File | What it does |
|---|---|
| `kernel/task.c` | Preemptive round-robin scheduling off the timer tick. Six slots. `yield()` reaches the same switch in software. |
| `kernel/ring3.c` + `kernel/ring3_asm.S` | Runs code at privilege level 3 with its own page tables, so a privileged instruction faults instead of taking the machine down. |
| `kernel/memmap.h` | The one place the fixed ring-3 addresses live (`JT_USER_BASE`, `JT_DMABUF_BASE`, `JT_USER_FB`); the Makefile renders it into `boot/memmap.ld` for the linker scripts. |
| `arch/arm64/wifi.c`, `arch/arm64/wifi_proto.h`, `arch/arm64/wifi_fw_gen.sh`, `tools/checks/arm64-wifi-join-check.py` | 6.22.0: the network is chosen at run time (`net_load`): `JT/WIFI.TXT`'s `ssid=` and `pmk=` hex lines, else the built-in. |
| `arch/arm64/wifi.c` (stage notes) | M4 Wi-Fi, stage 1 (Pi 4's CYW43455 over SDIO; shape from Plan 9's ether4330.c and Circle's wlan). |
| `arch/arm64/bt.c`, `arch/arm64/bt.h`, `arch/arm64/bt_fw_gen.sh` | Bluetooth, slice BT1 (2.51.0): the CYW43455's Bluetooth half over HCI in the H4 framing on the PL011 (GPIO 30-33, ALT3, hardware RTS/CTS), so the Pi. |
| `arch/arm64/btc.c`, `arch/arm64/btc.h` | Classic Bluetooth (12.1, BR/EDR) beside `bt.c`'s LE mouse, on the same UART and command slot: `bt.c` hands it every event and the answers to commands. |
| `arch/arm64/btpair.h` | The saved pairing's record, a 70 byte versioned file (magic `JTBP`, version, address type, address, LTK, Rand, EDIV, name, a 16 bit sum) as static. |
| `arch/arm64/emmc.c`, `arch/arm64/emmc.h` | M4 SD card (2.53.0 reads, 2.57.0 writes): the Pi 4 reads and writes the card it booted from. |
| `arch/x86_64/boot.S`, `arch/x86_64/main.c`, `arch/x86_64/linker.ld`, `arch/x86_64/Makefile`, `tools/checks/x86_64-m0-check.py`, `tools/checks/x86_64-irq-check.py`, `tools/checks/x86_64-mem-check.py` | 13.18.0, the third port's first slice: a 64-bit kernel for PCs. 13.22.0 (v15, slice 2): a 64-bit IDT with the 32 exception stubs, the 8259 PIC remapped to 32-47, the PIT at 100 Hz on IRQ0 and the PS/2 keyboard on IRQ1, each key printed on COM1. 13.23.0 (slice 3): the multiboot memory map, a 4 KiB frame bitmap with a small kmalloc on top, and the PS/2 mouse on IRQ12 moving a cursor clamped to the screen. |
| `arch/arm64/start.S`, `arch/arm64/vectors.S`, `arch/arm64/user.S`, `arch/arm64/main.c`, `arch/arm64/text.c`, `arch/arm64/libc.c`, `arch/arm64/linker.ld`, `arch/arm64/Makefile` | The ARM64 port (the ARM64 and the Raspberry Pi section), milestones M0, M1a, M1b, M1c, the first M2 drivers and M3a, the first userland. |
| `arch/arm64/ip.c`, `drivers/nic.h` | The IP stack on ARM. |
| `arch/arm64/rng.c`, `arch/arm64/rng.h` | Seeds TLS from the Pi 4 RNG200 hardware FIFO with health checks and a one-second deadline. |
| `arch/arm64/clock_ui.h` | Read-only Clock window on ARM, opened through Spotlight or Samantha. |
| `arch/arm64/settings.c` | The Pi's Settings choices and their file, 3.13.0. 13.21.0: Accent (Display) picks Blue, Purple, Terracotta, Orange, Green or Graphite. Kernel windows read `ui_accent`; apps read it with `jt_accent()` (syscall 415). Blue on the Pi, terracotta on QEMU so the checks read one colour. Saved as `accent=`. |
| `arch/arm64/accounts.c` | User accounts on the Pi, 4.8.0 (slice 1 of 6.0 Yours; the login screen is `login.c`). |
| `arch/arm64/login.c`, `arch/arm64/login.h`, `arch/arm64/login_ui.h` | The Pi's login screen, 4.9.0 (slice 2 of 6.0). 13.24.0: `login_ui.h` also draws the lock card on the scanout (the name fixed, one password field) and checks the password for the signed-in name only, as sudo does. |
| `arch/arm64/sudo.c`, `arch/arm64/sudo.h` | Admins and sudo on the Pi, 4.10.0 (slice 3 of 6.0). |
| `arch/arm64/limits.c`, `arch/arm64/limits.h` | Parental controls on the Pi, 12.0.0, as in Screen Time. |
| `tools/checks/fat-crash-check.py` | 4.13.0, slice 6 of 6.0: the real `fat.c` under sanitizers on FAT16 and FAT32 `mkfs.fat` images, power cut after every sector. |
| `arch/arm64/home.c`, `arch/arm64/home.h` | Per-user home folders on the Pi, 4.11.0 (slice 4 of 6.0). |
| `arch/arm64/snap.c`, `arch/arm64/snap.h` | Home snapshots (4.12.0) and backups (5.2.0). |
| `arch/arm64/wifi_ui.h`, `tools/checks/arm64-wifi-pane-check.py` | The Wi-Fi section of Settings (6.23.0), the seventh: the network the Pi joins, then a card of the networks the start-up scan. |
| `arch/arm64/users_ui.h` | The Users pane of Settings on ARM, 4.14.0, included by `settings_ui.h`. |
| `arch/arm64/bt_ui.h` | The Bluetooth pane of Settings on ARM, 7.9.0, included by `settings_ui.h`: radio state, the saved device, Look for devices (S), a. |
| `arch/arm64/lang.c`, `arch/arm64/lang.h`, `arch/arm64/lang_ui.h`, `arch/arm64/lang_hi.h`, `arch/arm64/world_font.h`, `tools/gen/gen_world_font.py` | Languages (9.2.0). |
| `arch/arm64/settings_ui.h` | The Settings window on ARM, drawn in the Console's rectangle like the Clock. |
| `arch/arm64/glyphs.h` | Flat anti-aliased glyphs drawn from circles, rounded boxes and capsules, 4x4 samples a pixel: the five Settings sidebar tiles and the menu bar's speaker and power symbol. |
| `arch/arm64/clock.c` | Gives the Pi a clock from a verified HTTPS Date header, never plain HTTP. |
| `arch/arm64/tls.c`, `arch/arm64/tls.h`, `arch/arm64/certs/` (Mozilla's list since 6.11.0, `mozilla-roots.pem` from curl's extract, beside the 37 roots picked in 3.0.0 that BearSSL still needs when a chain ends in one Mozilla has retired; 126 anchors, a root in both kept once), `tools/gen/tls-ta.py`, `tools/gen/tls-roots.py` | HTTPS on the Pi. |
| `arch/arm64/cardkeys.h`, `tools/gen/card-keys.py` | 13.19.0: the dev card keeps its relay token, relay CA and Wi-Fi key in `JT/` on the card, not in the kernel, so a signed release can replace the kernel with `update install`. Two pure parsers check every length. `tools/checks/arm64-cardkeys-check.py`. |
| `arch/arm64/cmd.c` | The one command line on ARM. |
| `arch/arm64/audio.c`, `arch/arm64/audio.h` | Sound out of the Pi 4's 3.5 mm jack, polled for `beep`, through DMA for an app (M3e). |
| `arch/arm64/sbc.c`, `arch/arm64/sbc.h` | SBC, the Bluetooth A2DP audio codec, encoder side, in fixed point with no libc. |
| `arch/arm64/ask.c` (relay discovery), `tools/checks/arm64-relay-discovery-check.py` | The Pi finds the Claude relay by itself (2.46.0). |
| `arch/arm64/keychain.c`, `tools/checks/arm64-keychain-check.py` | The keychain (6.21.0): an account's secrets sealed on the card in its home folder as `KEYCHAIN.BIN`. |
| `arch/arm64/ics.c`, `tools/checks/arm64-ics-check.py` | Calendar subscriptions (6.19.0). |
| `arch/arm64/mail.c`, `tools/checks/arm64-mail-check.py` | Mail over IMAP and TLS (6.18.0), at the Terminal. |
| `arch/arm64/browser.c` | A text browser behind the one command line (`cmd.c`). |
| `arch/arm64/gpu.c` | 13.2.0: the Pi 4's V3D GPU brought up by hand with `gpu` (`cmd.c`), never at boot. 13.20.0 (v14, slice 1): `gpu fill` runs the same job over a rectangle of tiles stored straight into the screen's back buffer, so the GPU paints a panel you can see. 13.22.0 also adds `gpu copy`: a LOAD_TILE_BUFFER_GENERAL per tile reads a source rectangle, so the GPU copies part of the screen, the building block for moving windows. |
| `arch/arm64/nimble.c` | Nimble on the Pi: one question to `nimble.jaybulb.com/api/answer` through the Browser's TLS client, one short answer back with up to three sources. |
| `arch/arm64/print.c` | Printing (13.11.0): `print FILE PRINTER[:PORT]` turns a text file into a PDF (Letter, Helvetica 11 pt, 60 lines a page) and sends it as one IPP. |
| `arch/arm64/mascot_art.h`, `tools/gen/gen_mascot_art.py` | The brand art on the Pi: `gen_mascot_art.py` scales the painted tree (`docs/brand/tree.png`, 8.0.0; the crayon mascot it replaced. |
| `arch/arm64/browser_ui.h`, `tools/checks/arm64-browser-app-check.py`, `tools/checks/arm64-pagestyle-check.py`, `tools/checks/arm64-forms-check.py`, `tools/checks/arm64-tabs-check.py`, `tools/checks/arm64-downloads-check.py` | The Browser window (2.66.0), opened from Spotlight, drawing h1 and h2 as 20 px bold headings two rows tall, h3 to h6 bold and. |
| `user/libjt/thread.h` | Threads, locks and condition variables for a Pi app, header only (8.1.0). |
| `user/libjt/hires.h` | Sharp 2x drawing for ring-3 apps, header-only (3.9.0). |
| `user/libjt/mascot.h`, `tools/checks/arm64-mascot-empty-check.py` | The mascot in a ring-3 app window, ARM64 build only (3.7.0). |
| `arch/arm64/ask.c` (sam speed), `arch/arm64/tls.c`, `tools/checks/arm64-samfast-check.py` | `sam` is fast on the board (3.7.6). |
| `arch/arm64/ask.c` (the bench), `tools/pi-bench.py`, `tools/checks/arm64-bench-check.py` | 13.16.0: `bench` lets the Mac run Terminal commands on the Pi, so a board test needs no second keyboard. |
| `arch/arm64/ask.c`, `arch/arm64/claude_cfg.sh` | Claude in the ARM Terminal. |
| `arch/arm64/main.c` (Console and Terminal) | The Console and two switchable Terminal sessions share one log renderer, each with its own scrollback: the Console (boot log. |
| `arch/arm64/ask.c` (agent loop), `docs/GUIDE.md` (Samantha's agent loop on the Pi), `tools/checks/arm64-agent-check.py`, `tools/checks/pi-release-notoken-check.sh` | Samantha as an agent on the Pi. |
| `arch/arm64/llm.c`, `arch/arm64/llm_model.S` | A small language model in the ARM kernel, no network. |
| `tools/checks/arm64-claude-console-check.py` | Boots the ARM build on QEMU virt with virtio-net, ramfb and a virtio keyboard, runs the real relay on 127.0.0.1 with a stub. |
| `arch/arm64/main.c` (dock) | The ARM desktop's dock (2.22.0) is the i386 dock: the same tray, shadows, icon artwork and layout from `kernel/gui_paint.c` and `kernel/dock_geom.c`. |
| `tools/checks/arm64-mouse-check.py` | Boots the ARM build on QEMU virt with ramfb and a USB keyboard and mouse behind a hub, moves the mouse through QMP and checks. |
| `tools/checks/arm64-focus-check.py` | Boots the ARM build on QEMU virt with a USB keyboard and mouse behind a hub, opens the Terminal and then the Calculator, and types into each after clicking it. |
| `tools/checks/arm64-clipboard-check.py` | 3.10.0: boots the ARM kernel headless on QEMU virt with a FAT32 card on virtio and the keyboard's Super key as Cmd. |
| `arch/arm64/main.c` (window drag) | 3.6.0: a press on the title band of the Console, Terminal, Clock, Brick or Browser (their one shared rectangle, `win_lx`. |
| `tools/checks/arm64-drag-check.py` | Boots the ARM build on QEMU virt with a USB keyboard and mouse behind a hub, presses and moves through QMP and checks. |
| `tools/checks/arm64-appsize-check.py`, `tools/checks/arm64-sharp2x-check.py` | `appsize` builds the `apptest` kernel for Calendar, Notes, Music and Books in turn (the app opened at boot), boots each on the Pi 4B model at 1080p. |
| `tools/checks/arm64-browser-flicker-check.py`, `arch/arm64/main.c` (off_begin, off_end) | 3.6.1: the Browser no longer flickers on the Pi, where the framebuffer is the live scanout. |
| `tools/checks/arm64-browser-mouse-check.py`, `arch/arm64/browser_ui.h` (br_wrap, browser_wheel) | 3.7.2: pointer motion over a long Browser page. |

| `tools/checks/arm64-splash-check.py`, `arch/arm64/crash.c` (splash_mark), `arch/arm64/splash_mark.h` | The Pi boot splash, Apple style: neutral grey `0xE5E5E7` with, since 8.0, the flat glyph in one ink `0x1C1C1E` above the bar. |

| `arch/arm64/main.c` (quiet boot, Wi-Fi fan), `tools/checks/arm64-quietboot-check.py` | 3.7.10: the splash never hides a failure. |
| `arch/arm64/calc.c` | The Calculator on ARM. |
| `tools/checks/arm64-keys-check.py` | Boots the ARM build on QEMU virt with a virtio keyboard and no pointer, and drives `main.c`'s keyboard-only desktop through QMP. |
| `tools/checks/arm64-keydbg-check.py` | Mac-style USB keyboards, over QEMU's xHCI and usb-kbd: boots `keydbg-kernel8.elf` (`make keydbgtest`), whose made-up HID reports. |
| `tools/checks/arm64-calicon-check.py` | Builds the `caltest` variant twice with the clock fixed at two dates, boots each headless and reads the Calendar tile out of a screendump: the accent. |
| `tools/checks/arm64-calc-check.py` | Compiles `arch/arm64/calc.c` on the host and checks a table of sums, including sin(30deg)=0.5, 5!=120, 2^10=1024 and 1/0 as an error. |
| `arch/arm64/wifi.c` (`wifi_hold`, `wifi_release`) | Bluetooth pairing holds the Wi-Fi radio (3.0.0): the chip has one antenna, and with Wi-Fi up a real mouse's first packets never reached the Pi (0x3e. |
| `arch/arm64/wall.c` | The ARM desktop's wallpaper: one flat colour, the brand terracotta `#b5502c` in 3.0.0 , graphite grey `#6E6E6E` from 3.8.3 and a calm blue `#3B6EA5`. |
| `arch/arm64/park_wall.h` | The Pi's default wallpaper, Joshua Tree at dusk (8.9.0; 13.9.3 redrawn at 960 x 540 with solid dithered bands, a sun halo. |
| `arch/arm64/userapps.S`, `arch/arm64/user_c.ld` | M3b (2.49.0) and M3c part 1 (2.50.0), C programs at EL0. |
| `arch/arm64/ufile.c` | M3d (2.58.0): the file calls of `user/jtsys.h` for the one EL0 app, with i386's semantics from the System call ABI section. |
| `arch/arm64/main.c` (EL0 apps in windows), `arch/arm64/vectors.S` (`resume_user`) | M3e (2.59.0): the EL0 arena is 32 MiB since 6.76.3 (16 MiB before), sixteen 2 MiB blocks of 4 KiB pages; from block two up to the. |
| `arch/arm64/pci.c`, `arch/arm64/xhci.c` | M4, USB keyboard and mouse on ARM. |
| `arch/arm64/virtio.c`, `arch/arm64/virtio.h` | QEMU's virtio-mmio devices for the ARM64 build, moved out of `main.c` in 4.23.2: the slot scan, the queues, input (evdev events into `main.c`'s. |
| `arch/arm64/boot.c`, `arch/arm64/kmain.h` | Cut out of `main.c` in 4.23.3: `boot.c` holds input from every driver (Linux evdev events), key repeat and `main()`, the boot sequence and the poll loop. |
| `arch/arm64/desk.c` | Cut out of `main.c`: the tree menu (About, Restart, Shut Down and the chords that ask first), the menu bar with its clock, Wi-Fi. |
| `arch/arm64/keys.c`, `windows.c`, `appwin.c`, `pointer.c`, `crash.c`, `console.c` | Cut out of `main.c` in 4.23.5, each part of `main.o` through `MAINSRC`: `keys.c` the keyboard-only paths (Spotlight, the. |
| `arch/arm64/utf8.h`, `tools/checks/utf8fold-host-check.sh` | 6.78.1: the Terminal draws one byte a cell as DejaVu Sans Mono's Latin-1 character, so `console_putc` folds text from UTF-8 on its way to the screen. |
| `arch/arm64/uimg.h` | The app image check (6.51.0), `uimg_ok`: the trailer and every relocation of a user image, before `user_load` in `main.c` touches a page. |
| `arch/arm64/appsig.c`, `appsig.h`, `app_key.h` | App signatures (6.54.0): `appsig_ok` checks `NAME.SIG`, an ECDSA P-256 signature in DER, over SHA-256 of `"JTAPP1\0"`, the lower-case name, `"\0"`. |
| `arch/arm64/eltests.c` | The EL0 self-tests that run at boot, out of `main.c` since 4.25.2 (`user_demo`): a data abort, a jump into kernel text, a clean exit, arena isolation. |
| `tools/checks/armcore.py` | The Pi core's source for checks that read it: `main.c`, every `MAINSRC` part and `kmain.h` as one text, and which part holds a. |
| `arch/arm64/xhci.c` (USB audio) | A USB microphone or speaker works through isochronous transfers, one a millisecond. |
| `tools/claude-relay/stt.py` | Speech to text on the Mac, a small HTTP service beside relay.py (which it never touches): raw 16 kHz mono PCM in with the relay's bearer token. |
| `tools/claude-relay/yt.py` | 6.9.0: YouTube for Blackcomb on the Mac, beside relay.py and stt.py. |
| `tools/claude-relay/spotify.py` | 13.3.0: Spotify as a remote, on the Mac beside yt.py (port 8769, same token, TLS on the LAN). |
| `arch/arm64/ask.c` (`hear_post`) | Speech into Samantha on the Pi: the samples voice mode recorded go raw (16 kHz) to the words service one port above the relay (the same token, and on. |
| `arch/arm64/ask.c` (voice) | Samantha's voice on the Pi: a `[[say TEXT]]` action prints the caption, posts the text to the relay's `/api/tts` (TLS on the Pi. |
| `arch/arm64/ask.c` (voice mode) | Double-tap Space and hold: `vm_key` watches the taps in the key handler, `vm_poll` confirms the hold 250 ms later from the main. |
| `arch/arm64/main.c` (the chat view's face) | `con_tui_begin` gives the chat view seven rows of face when the window has fourteen or more: Samantha's portrait from. |
| `arch/arm64/fptest.c` | Test support for the ARM64 floating point check, linked into the `fpsave`, `fpnosave` and (since M3e) `musictest` builds only, never the real kernel. |
| `tools/wifi-fw.sh` | Fetches the three CYW43455 firmware files (`brcmfmac43455-sdio.bin`, `.txt`, `.clm_blob`) plus the licence `copyright` into. |
| `tools/mkapp.sh` | The app SDK (6.52.0): one C file into `NAME.APP` for `JT/APPS/` on the card. |
| `arch/arm64/update.c`, `tools/update-sign.sh` | Self-update with a fallback kernel (9.3.0). |
| `arch/arm64/store.c`, `tools/store-add.sh`, `store/` | The app store (6.55.0). |
| `tools/plankapp.sh`, `user/pklibc/`, `store/slide.pk`, `store/code.pk` | Plank programs as apps (6.57.0). |
| `ask.c`'s `app_build`, `relay.py`'s `build_app` | Samantha writes apps (6.59.0). |
| `user/plankc/` | plankc, the Plank compiler written in Plank: the true bootstrap (8.0; its steps are in docs/ROADMAP.md). `lex.pk` and `parse.pk` give exactly plank.py's tokens and tree (6.60.0, 6.61.0). `gen.pk` writes aarch64 assembly from the tree as a stack machine: every value ends in x0, sp moves 16 bytes at a time, and anything it cannot do stops with "not supported yet". `rt.pk` is the runtime, written in Plank and built only by plankc, with built-ins plank.py lacks (`syscall`, `peek`, `poke`, `peek8`, `poke8`, `global`, `addr`, `as_str`, `as_float`, `as_int`, `bxor`, `shl`, `shr`, `fma`): strings, lists, dicts, printing, floats rounded as printf rounds them, regular expressions, JSON, files, dates and `random`, and a collector (6.70.0: conservative, non-moving mark and sweep; its roots are the stack and the runtime's cells, which holds because generated code never keeps a heap address only in a register across a call). `shake` (6.93.0) then keeps only the functions `main` reaches, by the `pk_` names written in each, so an app carries only the runtime it uses. `enc.pk` (6.71.0) turns that assembly into a JTU1 app image with no clang or linker. `plankc.pk` is the command: `plankc lex| parse FILE.pk`, `plankc asm FILE.pk RT.pk`, or on the Pi `plankc build NAME.PK` (6.95.0: the card's runtime, into. |
| `tools/signapp.sh` | App signing (6.54.0): `NAME.APP` into `NAME.SIG` with openssl, over the message `appsig.h` defines, with `~/.config/joshuatree/app-signing.pem` unless a key is named. |
| `tools/bt-fw.sh` | Fetches the Bluetooth patch `BCM4345C0.hcd` and `LICENSE.cypress` into `build/bt-fw/` (gitignored), pinned to one. |
| `arch/arm64/wifi_proto.h` | The pure half of the Wi-Fi driver: SDPCM and BCDC frame packing, escan result parsing and NVRAM packing, with no hardware access. |
| `drivers/lib_caveatbrush_font.h`, `drivers/lib_comicneue_font.h`, `drivers/lib_dancingscript_font.h`, `drivers/lib_ebgaramond_font.h`, `drivers/lib_jetbrainsmono_font.h`, `drivers/lib_sourcesans3_font.h`, `drivers/lib_sourceserif4_font.h` | The font library: seven open-licence typefaces (hand lettering, a comic face, a script, a book serif, a coding face, two reading. |
| `arch/arm64/wpa.h` | The WPA2 half of Wi-Fi: SHA-1, HMAC, `wpa_pmk` (PBKDF2-HMAC-SHA1, 4096 rounds, since 6.22.0, checked against 802.11i H.4), the pairwise-key PRF and. |
| `arch/arm64/wifi_cfg.sh` | Writes the build file `arch/arm64/wifi_cfg.h` (gitignored, never in git): the network name and its key (worked out at build time, never the. |
| `tools/checks/wpa-test.c` | Host test for `wpa.h` against the SHA-1, HMAC-SHA1, PRF and RFC 3394 key-unwrap test vectors; run by the suite. |
| `tools/gen/sample-song.py` | Writes `HELLO.WAV`, the sample song: 8 bars at 120 bpm in C major (bass, arpeggio pad and lead), 16 s, 16-bit mono 22050 Hz, about 690 KB, pure arithmetic so every run is byte-identical. `tools/flash-pi.sh` puts it in `JT/MUSIC/` only when the folder has no `.WAV` or `.MP3`; `tools/checks/arm64-music-check.py` plays it as a third song. |
| `tools/gen/map.py` | Draws `architecture.svg`, Joshua Tree at a glance in three panels: where it runs, the layers, talking to Samantha (13.21.3 cut it from seven, so the README stays short). Reads the app count from `progress.svg`, so it cannot drift. |
| `tools/gen/gen_stickers.py` | Draws the sticker sheet in `docs/brand/stickers/` (13.21.5): the park circle, the `joshua@tree %` prompt and "A computer, from scratch.", 3 inch SVGs (the die-cut tree is `docs/brand/tree.png` itself) with the fonts and art inside, ready for a print shop. |
| `tools/gen/svgkit.py` | The kit both pictures are drawn with (6.59.2): the brand tokens' colours, spacing and radii on an 8 point grid, Tree Sans subset to each picture's letters and embedded as woff2 (through uv's fonttools), and 8 by 8 pixel icons on terracotta tiles with a dither and scan lines. |
| `tools/gen/humans.py` | Draws `docs/apps-for-humans.svg`, the plain-words picture of how a new app reaches the Pi (the store, Samantha, or you) and what the seal means, for someone who has never read the code (6.59.1). Drawn with `svgkit.py`, like `map.py`. |
| `tools/gen/wallraw.py` | The baked tree photo in `drivers/wallpaper.h`: `write_header` up-filters it (each byte minus the one a row above) and deflates it, about 520 KB in place of 1.5 MB of raw RGB (4.23.0); `wallpaper_rgb()` gives the raw pixels back to the fixture generators and the wallpaper checks. The kernel unpacks it on first use in `kernel/wallmap.c` (`wall_photo_get`), and `png-check.sh` fails if the un-filter is wrong. |
| `tools/gen/park_to_photo.py` | The PC's picture wallpaper from the Pi's (13.22.0): decodes `arch/arm64/park_wall.h` (Joshua Tree at dusk) with the Pi's scanline and packs it into `drivers/wallpaper.h` through `wallraw.write_header`, so the PC and the Pi show one painting. |
| `tools/gen/breadboard.py` | Blender script that renders the breadboard prototype picture in `docs/hardware/`; the wiring it shows is written in `docs/GUIDE.md` (Breadboard prototype). |
| `docs/hardware/nimbus_cad.py` | Nimbus, a small square computer for a Pi 4 and the 7 inch touch display. build123d lofts the front outline from the icon squircle. |
| `docs/hardware/render_nimbus.py` | Nimbus product renders from the real CAD: builds the parts, then headless Cycles with one overhead soft key on a cream sweep, the. |
| `docs/hardware/nimbus_desktop.py` | The picture on Nimbus's screen: a saved 1920 x 1080 desktop screendump of the Pi build with Weather, Calendar and Notes composed. |
| `docs/hardware/nimbus_blueprint.py` | The Nimbus print blueprint: projects the tessellated parts into front, side, rear, top, underside and exploded views with the overall sizes, M3. |
| `docs/hardware/nimbus_box.py` | The Nimbus retail box render: a plain white two-piece box with the tree mark and the name on the lid, same light and sweep as the product views, to `nimbus-box.jpg`. |
| `docs/hardware/nimbus_pack.py` | The Nimbus print pack in `nimbus/pack/`: STLs checked against a 256 mm bed (`stl-check.json`), a 1:1 blueprint on A3 and tiled on A4 and Letter, a. |
| `docs/hardware/pi4b.py` | The Raspberry Pi 4B's numbers from its mechanical drawing: board, mounting holes, standoff, tallest port and the port windows. |
| `docs/hardware/samantha_cad.py` | The Samantha box, a desk unit for a 7 inch screen, a Pi 4B and a speaker. build123d writes the back shell, the bezel, a. |
| `docs/hardware/render_samantha.py`, `docs/hardware/render_kit.py` | The Samantha box previews: build the CAD into a temp folder, render the shell and bezel in Blender's Workbench with no window. |
| `tools/checks/samantha-box-check.py` | Runs `samantha_cad.py` into a scratch folder and fails if a fit assert stops it or one of its five files is missing. |
| `tools/checks/arm64-wifi-check.py` | Boots the Pi image on QEMU's raspi4b model, with and without the firmware files, and asserts `M1c fb ok`, `wifi power on`, `wifi FAIL cmd5` and then. |
| `tools/checks/arm64-bt-check.py` | Boots the virt build with a second serial port on a Unix socket to a fake H4 controller and a made-up `.hcd` (`BT_HCD=`). |
| `tools/checks/bt-pair-check.py` | Builds `btpair.h` on the host with clang and sanitizers and round-trips a record with an obviously fake key; refuses bad magic. |
| `kernel/exec.c` | Loads a flat binary off the filesystem into a fixed window the linker script reserves, marks its pages user-accessible, and starts it as a ring-3 task with argv. |
| `user/hello.c` + `user/jtsys.h` | The reference user program and the one header a user program gets: inline `int 0x80` wrappers, nothing else. |
| `user/note.c` | The second user program, and the first one worth running: prints a file, appends a line, seeks. Exercises the v2 syscalls. |
| `user/libjt/` | A small C library for user programs: `string.c`, `stdlib.c` (`malloc` over `SYS_BRK`, a fixed arena when brk is refused; its. |
| `user/libjt/text.c`, `user/libjt/text.h` | Antialiased text for ring-3 apps (1.9.23). |
| `user/libjt/mono.c`, `user/libjt/aamono.h` | The mono face for ring-3 apps: DejaVu Sans Mono, ASCII only, one size, every glyph an 8 px advance. |
| `user/libjt/jpeg.c` | One line: `#define JPEG_USER` then `#include "../../drivers/jpeg.c"`, so ring 3 runs the kernel face decoder itself. |
| `user/libjt/png.c` | Two lines: `#define PNG_USER` then the kernel's `drivers/inflate.c` and `drivers/png.c`, so ring 3 decodes PNG with the one. |
| `tools/gen/typetokens.py`, the Type system section | The type system, one source: nine sizes (11 to 64), line heights on a 4 px grid, tracking by size, weights with jobs, one spacing ladder. |
| `tools/fonts/treesans/build.py` | Builds Tree Sans from Inter 4.1 (OFL): Text and Display optical sizes, frozen alternates, vertical metrics on a 1/16 em grid, renamed, TTF and WOFF2. |
| `tools/checks/type-grid-check.py` | Fails when a landing font size is off the scale, the token block or the Type system section is stale, or a Tree Sans file does not load. |
| `user/libjt/wav.c`, `wav.h` | The WAV reader for the Music app. |
| `user/libjt/avi.c`, `avi.h` | The AVI reader for the Movie app: motion-JPEG video and optional 8/16-bit PCM sound, parsed from a file held in memory. |
| `tools/media/avigen.py`, `avi-host.c` | The fixtures and host test for `avi.c`: real JPEG frames through the kernel decoder, truncation, bad headers and a mutation fuzz under ASan and UBSan. |
| `tools/media/demo-music.py`, `demo-disk.sh` | Three original demo songs for the Music app, synthesized from scratch (so nothing to license): 8-bit mono 11025 Hz WAV, about 40 s and 430KB each. |
| `tools/checks/fpu-check.sh` | Boots the kernel and runs `fputest`: two tasks keep different values on the x87 stack across task switches. |
| `tools/media/wavgen.py`, `wav-host.c` | The fixtures and the host test for `wav.c`: golden samples, broken headers, then 20000 mutations under ASan and UBSan. |
| `tools/media/musicfix.py` | Builds the FAT16 disk `tools/checks/music-check.py` boots with: a 440Hz WAV in the root, a 16-bit stereo 660Hz WAV and a 3.3 MB over-the-cap WAV in `MUSIC/`. |
| `tools/checks/music-qa-check.py` | The Music QA pass (2.44.1), three headless boots with a Sound Blaster and the `musicfix.py` disks. |
| `third_party/minimp3/minimp3.h` | Vendored minimp3 (CC0 public domain, https://github.com/lieff/minimp3): a single-header MP3 decoder for Layer I, II and III files. |
| `user/libjt/mp3.c`, `mp3.h` | The MP3 reader for the Music app (2.2). |
| `tools/media/mp3-host.c`, `fixtures/` | The host test for `mp3.c`: golden length, the 440 Hz pitch and amplitude band, chunked reads equal one read, seek equals playing. |
| `user/libjt/avi.c`, `avi.h` | The AVI reader for Movies. |
| `tools/media/avigen.py`, `avi-host.c`, `tools/checks/avi-host-check.sh` | The fixtures and the host test for `avi.c`: real JPEG frames through the real decoder, odd chunk orders, broken headers, then mutations under ASan and UBSan. |
| `user/libjt/osk.c`, `user/libjt/osk.h` | The phone on-screen keyboard for ring-3 apps. |
| `user/libjt/aafont.h` | Generated glyph atlas behind `text.c`: 4-bit coverage bitmaps of DejaVu Sans, rendered from the kernel's own font data. |
| `tools/gen/gen_user_text.c` | Host program that bakes `user/libjt/aafont.h` through `drivers/ttf.c`, so ring-3 text and in-kernel text share one rasterizer and one font. |
| `arch/arm64/screen_idle.h` | Pi display idle: 15 s before the wait (Settings, Display, Screen saver after: 1 to 30 minutes or Never; five by default) the. Auto lock (13.24.0): with someone signed in, the saver or the dark locks; a wake shows the lock card, never fb, and only that person's password gives fb back. |
| `arch/arm64/trackpad.h` | The trackpad (10.2.0): multitouch slots in, mouse events out. |
| `arch/arm64/battery.h` | The battery (10.3.0): a small I2C driver on BSC1 (GPIO 2 and 3) reads a MAX17048 fuel gauge at 0x36 once a minute; the menu bar. |
| `arch/arm64/ptp.h` | USB precision touchpads (11.0.0): finds a Digitizer Touch Pad report descriptor (fingers, contact count, button, Input Mode), and xhci.c switches the. |
| `arch/arm64/notice.h` | One bounded menu message; a newer one replaces it and it expires after four seconds, including timer wrap. |
| `arch/arm64/brick.h`, `arch/arm64/brick_ui.h`, `tools/checks/arm64-brick-check.py` | Original brick-breaker rules and ARM window: 32 bricks, three lives, arrows or mouse, pause and restart. |
| `arch/arm64/ufile.c` (`ufile_sync`), `arch/arm64/power.h` (`power_halt_screen`), `arch/arm64/main.c` (`POWER_FILE_TEST`) | 4.16.0 power safety. |
| `arch/arm64/power.h`, `tools/checks/arm64-power-check.py` | Pi keyboard shutdown and restart, explicit confirmation and cancellation, VideoCore SD power-off and watchdog reset; host safety. |
| `user/finder.c`, `tools/checks/arm64-finder-check.py` | Finder on the ARM64 build (3.14.0), the file window of the Burrow tile and Spotlight (`SPOT_FINDER`), a ring-3 program built only for aarch64. |
| `tools/checks/debugger-check.py` | Boots both kernels paused under QEMU's gdb stub, lldb stops each at its first C function and reads the program counter. |
| `arch/arm64/heat.c` | 13.3.0: `temp` (`cmd.c`) prints the chip's temperature and the point the firmware slows it down, from mailbox tags 0x00030006 and. |
| `tools/checks/ring3seed-stale-check.py` | Boots twice on one FAT disk with the app's bytes changed between boots: the kernel replaces the stale copy, and leaves a matching one alone. |
| `tools/checks/arm64-notify-check.py` | A card app posts a notice with `jt_notify`: it shows under the app's name, Focus holds it, and turning Focus off says how many were held. |
| `tools/checks/ping-check.py` | `ping 10.0.2.2` in the Pi Terminal gets real ICMP replies from QEMU's gateway, and an address nobody holds gets none. |
| `arch/arm64/man.h` | The Terminal's `man NAME` pages, one per command, included once by `cmd.c`. |
| `tools/checks/arm64-man-check.py` | Fails when a command in `help` or `cmd_run` has no page, then runs `man cat` and `man zzz` in headless QEMU. |
| `tools/checks/arm64-spotlight-check.py` | Boots the ARM kernel headless in QEMU, presses F1: an empty query lists all 18 apps, typing calc shows exactly Calculator, Enter opens its window. |
| `tools/gen/ci-balance.py` | Rebalances the CI shards. |
| `tools/checks/ci-tooling-check.py` | Host fixtures prove the balancer calls a check ARM by what it runs (the ARM QEMU or an arch/arm64 build), packs those across the. |
| `tools/ci-local.sh`, `tools/hooks/pre-push` | Run the fixed-path guard before builds; local CI then runs the release suite with two workers. |
| `tools/ci-lock.sh` | One suite at a time on this Mac. |
| `tools/checks/scratch.py` | Gives each QEMU check its own temp directory, removed at exit. `JT_KEEP_TMP=1` keeps it for debugging. |
| `tools/checks/tmp-paths-check.py`, `tools/checks/tmp-paths-baseline.txt` | Fails when a check hard-codes a fixed `/tmp/jt-*` path or socket. The baseline lists the older offenders and only ever shrinks. |
| `tools/checks/relay-tts-check.py` | Proves the relay's voice endpoint (2.45.0) against a stub ElevenLabs server on loopback, with no network and no money: the stub's PCM comes back, text over the per-request cap and requests past the per-day cap are refused with no upstream call, the day count survives a relay restart, a new date starts fresh, a failed call gives its characters back, the key never shows in any response, header or log (even when the stub echoes it), a tokenless request is 401 and a key file others can read is refused. |
| `tools/claude-relay/relay.py`, `tools/checks/claude-relay-check.py` | The relay behind the Claude app (2.14.0), also Samantha's voice (2.45.0): `POST /api/tts` with the same token and TLS rules returns 22050 Hz mono 16-bit PCM from ElevenLabs, with caps and a private key file (`docs/GUIDE.md` (The Claude app)), Python standard library only. It takes `POST /api/claude` with a shared token and runs headless Claude Code (`claude -p`) with read-only tools in the repo, one request at a time, with a hard timeout. It is a command-capable agent behind an HTTP port: it binds loopback unless told `--lan`. The check proves the token, session, argv, timeout, size and logging rules against a stub `claude`. LAN binding requires a TLS certificate and key; loopback HTTP is retained for i386/QEMU. `docs/GUIDE.md` (Encrypted Pi relay) covers the card and certificate setup. API file tools open relative to a pinned directory with no symlink following, check the opened file is regular and refuse replacements without blocking; `relay-api-check.py` covers symlink, FIFO and deletion races. `docs/GUIDE.md` (The Claude app) explains it. |
| the Design rules section, `tools/checks/design-doc-check.py` | The design system as one page of written rules (2.12.2): what the OS never does, the shared icon shape and light, the fleet-icon rule, type, colour and day and night, window and dock numbers, and the caption timings. The check reads every `NAME = value` in the page and compares it with the file that defines it, so a rule cannot drift from the code; it also scans `art/icons` for text, purple, teal and gradient hue. |
| `tools/landing-shots.py`, `tools/landing_shots_manifest.py`, `tools/checks/landing-shots-fresh-check.py` | The landing app tiles in `landing/shots/`. The script retakes a tile headlessly (the fleet apps boot with `open=<app>` and no network, so they show their offline samples) and records a hash of the sources that draw it in `landing/shots/sources.json`. The check fails when an app changed and its tile was not retaken. `--clips` (13.20.0) also films each tile through a short scripted use (`PLAY`) into `landing/clips/app-*.mp4`, the still as its poster. |
| `tools/checks/landing-clip-still-check.py` | The app wall never flashes blank on scroll (13.22.0): each clip's still is its figure's background, a clip loads 600 px early, and it stays invisible until it plays. |
| `.github/workflows/landing-shots.yml` | Refilms landing stills on a runner for a Mac without QEMU or ld.lld (13.22.0): a pushed commit with `[shots] NAMES` (none means the hero) runs `tools/landing-shots.py` and uploads the webp files and `sources.json` as an artifact to commit by hand. |
| `tools/feature-clips.py` | The landing's Pi feature clips (13.20.0): boots the ARM kernel headless on QEMU virt with a USB keyboard and mouse, runs a short scripted use of Spaces, Mission Control, Spotlight or Launchpad, and records screendumps into `landing/clips/feature-*.mp4` plus a poster `.jpg`. `--cut FILMED START END NAME [SPEED]` trims a filmed boot into a clip (13.21.0). |
| `tools/film/qemu-system-aarch64` | Films any ARM check that draws a desktop (13.21.0): first on PATH with JT_FILM_DIR set, it execs the real QEMU with a second QMP socket and a recorder process beside it that screendumps about 10 times a second into `boot-N.mp4`. The check runs unchanged; the update, Bluetooth and Samantha landing clips came from it. |
| `landing/hero/samantha.webp`, `landing/hero/samantha-phone.webp`, `tools/checks/hero-poster-check.mjs` | The hero poster (2.11.1): real stills of Samantha's full-screen view, 1920x1080 for desktop and tablet and 860x1520 (430x760 at 2x) for phones, captured from the booted kernel's own canvas so lifting the poster on the first click shows the same picture, alive. `#hero-poster` in `landing/index.html` covers the stage; `landing/v86/embed.js` lifts it and takes the keyboard (the machine boots behind it from page load, with the `samantha` token on its command line unless `?desktop`). The check proves the poster is a real undistorted portrait by pixels, the default boot reaches her face, Esc closes her to the desktop, `?desktop` opts out, phone and tablet do not scroll sideways, and that it fails when the poster is removed. |
| `user/wc.c` | Unix `wc`, counts lines, words and bytes. The first program linked against libjt instead of raw syscalls; installed as WC.BIN at boot for the text shell. |
| `user/brkpoke.c` | The `SYS_BRK` leak probe (1.9.27): grows 3 MB, touches every page, checks each came back zero, shrinks, then crashes on purpose so `tools/checks/ring3brk-check.py` can assert the PMM free count and brk live count return to baseline. Run by `kernel/ring3app.c` under the `brkpoke` boot flag. |
| `user/fbpoke.c` | The program that must not work (1.7.8): runs after a window closed, hands `write` a kernel pointer and a pointer into the released framebuffer (both must be `-EFAULT`), then stores into it and must page-fault. Run by `kernel/ring3app.c` under the `fbpoke` boot flag. |
| `user/keyrate.c` | Keyrate, the typing test, as a ring-3 program: the first app to leave the kernel (1.7.7). Gets its window from `SYS_WINDOW_OPEN`, its keys from `SYS_WINDOW_POLL`, draws its own 8x16 glyphs. The backquote key crashes it on purpose. |
| `user/toroid.c` | Toroid, Conway's Life on a torus, as a ring-3 program: the second app out of the kernel (1.7.11). Two 160x80 bit-packed boards in its own .data, generations paced off `SYS_TIME`, the backquote key crashes it on purpose. |
| `user/calculator.c` | Calculator, a recursive-descent parser over `+ - * / ()`, as a ring-3 program: the third app out of the kernel (1.7.12). Same grammar as the in-kernel version, evaluated straight into a `double` per rule instead of an `expr_node` tree, since a flat binary has no `.bss` and no `kmalloc`. Dividing by zero yields 0, unchanged. The backquote key crashes it on purpose. Tab switches to scientific: `^`, `!`, `%`, pi, e, ans and sin through exp, on the x87 with no libm. |
| `user/quotes.c` | Quotes, the film-quote guessing game, as a ring-3 program: the fourth app out of the kernel (1.7.14). Same fixed deck and answer-rotation as the in-kernel version, streak and best kept in its own `.data`. The backquote key crashes it on purpose. |
| `user/bookrank.c` | Bookrank, the ranked non-fiction shelf, as a ring-3 program: the fifth app out of the kernel (2.0). Two panes, up/down or a click selects, the list scrolls. The top twelve books come live from the real Bookrank API through `SYS_HTTP_GET` and the Worker (2.6.26); the reply is untrusted, so every byte is forced printable and every copy is bounded. Ten built-in samples show when the fetch or the parse fails. The backquote key crashes it on purpose. |
| `user/tonchi.c` | Patois (was Patois, was Lexly), learn anything, as a ring-3 program: the seventh app out of the kernel (1.9.1). A course picker over the live Patois courses, then a four-choice drill with a score (2.6.27), through `SYS_HTTP_GET` and the Worker. The reply is untrusted: bytes forced printable, every copy bounded, answer index checked, bad rows dropped. With no network or a bad reply it opens the 30-word Spanish deck as before. Keys 1 to 4 or a click answer, Esc goes back to the picker. The backquote key crashes it on purpose for `tools/checks/ring3tonchi-check.py`; `tools/checks/ring3tonchi-live-check.py` proves the live and offline paths. |
| `user/fieldbook.c` | Fieldbook, every field of science and math explained plainly, as a ring-3 program: the ninth app out of the kernel (1.9.3). Same twelve fields and two-pane layout as the old in-kernel copy, up/down or a click selects, the explanation word-wraps by character count in the 8x16 font, and backquote is the deliberate crash `tools/checks/ring3fieldbook-check.py` presses. |
| `user/clock.c` | Clock, the time of day, a countdown timer and one alarm, as a ring-3 program: the tenth app out of the kernel (1.9.4). Same three jobs as the old in-kernel copy, the time comes from `SYS_TIME`, and the timer minutes and alarm are typed on the program's own line since a ring-3 program has no prompt box. Backquote is the deliberate crash `tools/checks/ring3clock-check.py` can press. |
| `user/music.c` | Music, a song player as a ring-3 program with its own window (2.2): the library lists every `.WAV` in the Files root and in a `MUSIC` folder (`SYS_READDIR`), one song at a time is read whole into the `SYS_BRK` heap (`SYS_READFILE`, refused over 3 MB with a message) and `wav.c` turns it into the 8-bit mono the card takes. Play, pause, previous, next, a seek bar (click or arrow keys), shuffle, repeat, a software volume and the wheel on the list. `.MP3` goes through `user/libjt/mp3.c`. The elapsed time is the stream start plus the driver's `played` counter, never the wall clock (32-bit math split into seconds and a remainder, so a song over 97 s at 44.1 kHz does not wrap), and pause or seek wait for the queue to drain first so that number stays true. About a third of a second is kept queued, not more, because the volume is applied before a sample is queued. Shuffle plays every song once a round (repeat off stops, repeat all starts another); when one song ends into the next a song that will not play is skipped, while n and p stop on it and say why. A narrow window puts the Shuffle and Repeat pills on their own row. Backquote is the deliberate crash for `tools/checks/ring3crash-all-check.py`; `tools/checks/music-check.py` and `music-qa-check.py` read its `music:` serial lines. Since 2.59.0 the same file runs on the ARM kernel (M3e): Spotlight opens it, its library is the card's `JT/MUSIC/`, and `tools/checks/arm64-music-check.py` plays a WAV and an MP3 with it on QEMU virt. |
| `user/movies.c` | Movies, a motion-JPEG AVI player with sound, as a ring-3 program (2.2). Lists `.AVI` files from Files, reads the whole clip into its `SYS_BRK` heap with `SYS_READ_FILE`, indexes it once with `avi.c`, then plays with the audio as the clock: the frame on screen is `played / rate / frame time`, only that frame is decoded, late frames are dropped and the picture never makes the sound wait. Space pauses, left and right seek five seconds, a click on the bar seeks, `f` hides the controls and letterboxes into the whole window. A clip too big, damaged or not an AVI shows a message instead. Backquote is the deliberate crash `ring3crash-all-check.py` presses; `tools/checks/movie-check.py` proves the rest. Since 6.1.0 the Pi runs it too, unchanged at EL0 with `avi.c` built as `ujt_avi.o`, last in Spotlight and the Launchpad (`arm64-movies-check.py`). Where `SYS_READAT` answers it streams: the headers from the first 64 KB, a chunk index from each chunk's 8 byte header, and each frame and stretch of sound read from the card as it plays, so the 6 MB cap is gone on the Pi. 6.2.0: in fullscreen the controls float over the picture (`draw_overlay`, the strip blended toward ink) and fade 3 s after the last input (`ov_want`). 6.10.0: at launch it reads PLAYNOW.TXT, removes it, and plays the clip it names (Blackcomb's Play). |
| `user/photos.c` | Photos (12.0), a ring-3 program on the Pi: the `.JPG` and `.PNG` files in the Files area's `PHOTOS` folder as a grid of small copies, one full size, Left and Right between them. Edits never touch the file, as in Apple's Photos: a quarter turn, black and white and light in steps of 20, kept in `PHOTOEDT.TXT` one line a photo and applied every time a photo is drawn. Decoding is libjt's copy of the kernel decoders. Its art is `art/icons/photos.svg`, slot 41 of `ICON_ART`, with no `APPS` row on i386. Check: `arm64-photos-check.py`. |
| `user/hamurabi.c` | Hamurapi (Hamurabi in the sources), the 1968 city game, as a ring-3 program with its own window (2.7), playable start to finish. The title screen (three buttons: story, Classic 1968, a demo the robot king plays by itself), a card at the gate of some story years (the opening, a turning point in years 5 and 10, or an omen with two choices, one drawn grey when the city cannot pay), the orders panel (Land, Feed, Plant, each with minus, plus and a bar you can click; Up and Down pick a row, Left and Right change it, Shift takes big steps, Enter ends the year; the orders are clamped like the web game's so a year can never be illegal), the year's report and the ending with the grade. The drawn city behind it (sky, hills, town around a ziggurat with the king on top, river and boat, fields, sheep, walking villagers) reacts as the web scene does: a villager for every four people, plots sown for what you order planted, sacks for the grain, the harvest and rats as floating numbers, ghosts for the starved, a green fog for the plague, rain on a ruined reign, fireworks on a great one. It draws at the largest whole-number pixel scale the window allows. The sprite sheet is unpacked once onto the `SYS_BRK` heap and every frame is painted into a back buffer, then copied over the window, so the compositor never catches half a frame. Each reign seeds its dice from the clock and logs its seed and every year's orders and result on the serial port; the backquote key crashes it on purpose. `tools/checks/ring3hamurabi-check.py` proves it, replaying every logged year on the host. |
| `user/hamurabi_sprites.h` | Hamurabi's 125 sprites on one 256x149 sheet: a 133-colour palette, the sheet as run-length coded palette indices (about 21 KB instead of 38 KB) and an enum of sprite names with their rectangles. Generated by `tools/gen/gen_hamurabi_sprites.py` from `art/hamurabi/sprites.png` and `sprites.json`, committed, and checked for staleness in the suite. |
| `user/windgate.c` | Airwell, guided breathing, as a ring-3 program (2.8). The web app's four presets (Box 4-4-4-4, 4-7-8, Relax, Coherent): a circle that grows on the in-breath, holds, and shrinks on the out-breath, the phase word, the seconds left, a round counter and a session timer. Each frame is painted into a back buffer on the `SYS_BRK` heap and copied over the window in one pass. Time comes from `SYS_TIME` (true wall seconds); the tick counter only places it inside the second and is re-learned at every boundary, because QEMU's runs about twice too fast. Exercise time stops while paused. It writes one serial line per phase start with the exact boundary time, which `tools/checks/ring3windgate-check.py` verifies. Backquote is the deliberate crash `ring3crash-all-check.py` presses. |
| `user/panes.c` | Panes, a cmux-style terminal multiplexer, as a ring-3 program (2.11.0), beside the Terminal rather than inside it. A sans tab rail on the left (up to nine tabs, named for the last command or folder, an accent dot on a tab that printed while you were elsewhere), each tab holding one or two panes side by side or stacked. Every pane is its own shell: its own 3 KB scrollback, input line, five-entry history and working directory, carved from one 47 KB arena past the image (12 panes in the pool). `cd`, `clear` and the shell commands come from `user/shellcore.h`; `sleep N [text]` is local, the one background timer, which prints when it ends and so lights the dot. Keys: Ctrl+T new tab, Ctrl+W close pane then tab (the last tab resets), Ctrl+1 to 9 and Ctrl+Left/Right switch, Ctrl+D split, Ctrl+E stack, Ctrl+O next pane; clicks on a tab, the New tab row or a pane work too. The kernel hands only this window those chords, as `KEY_CTL_*` (`kernel/app.h`, `key_ctl_code`, in `gui_multiwin_key_nonblock`); the Terminal and every other app keep plain keys. Apps folder, `open=pane`. `tools/checks/panes-check.py` proves it from framebuffer pixels and the `panes: tabs/panes/job done` serial markers. |
| `user/shellcore.h` | The shell engine the Terminal and Panes share (2.11.0): the echoed prompt line, `cd` against `jt_readdir`, `clear`, and the `SYS_SHELL_RUN` round trip with its `terminal: ran=N` serial line. Header-only and callback-driven (each app owns its scrollback and input), so a fix lands in both. |
| `user/claude.c` | Claude, a front end for Claude Code, as a ring-3 program (2.14.0). Claude Code itself runs on the relay machine (`tools/claude-relay/relay.py`), because this OS has no TLS and no Node. You type a question, the app posts it with `SYS_HTTP_POST` and `JT_POST_CLAUDE`, and the answer comes back as plain text: a transcript with You and Claude turns, word-wrapped and scrollable, a thinking line while it waits, and a red line saying why when it fails (no relay set, relay down, wrong token, too slow). It keeps the session id the relay hands back, so a conversation carries on; `/new` starts over. It never sees the relay's address or token: Settings owns both and the kernel adds the token. Apps folder, `open=claude`. `tools/checks/ring3claude-check.py` proves it in QEMU against the real relay and a stub `claude`. |
| `user/books.c` | Books, a plain-text reader as a ring-3 program (2.66), Apps folder and Spotlight, no dock tile. The library lists every `.TXT` in `BOOKS/` (the card's `JT/BOOKS/`), or the Files root when `BOOKS/` is missing. A book is read whole with `SYS_READFILE` into the heap (3 MB cap, refused with a message above it) and paged one wrapped page at a time: Right or Space next, Left back, Esc to the library. Each page turn saves the page to `NAME.POS`, read back on open. `tools/checks/books-check.py` proves the refusal, the page turns and the reopen on a FAT image. `open=books` opens it. |
| `user/mines.c` | Mines, Minesweeper as a ring-3 program: a 9x9 board with 10 mines. A click or space reveals, f flags, arrows move the cursor, r deals a new board, esc closes. The first board of every run comes from a fixed seed, so `tools/checks/ring3mines-check.py` can replay the same game blind and expect a win. Apps folder, `open=mine`. |
| `user/solitaire.c` | Solitaire, Klondike draw one, as a ring-3 program (4.23.0). Thirteen piles: stock, waste, four foundations by suit, seven columns. d or space draws and turns the waste over, a pick then a place moves (w, 1 to 7, f), a sends every card it can to the foundations, r deals again, esc closes; a click on a pile is its key. Suits are drawn from discs and rows, not a font. The first deal comes from a fixed seed so `tools/checks/ring3-solitaire-check.py` can replay it. Apps folder, `open=soli`. |
| `user/paddle.c` | Paddle, a bounce game against the computer (the Pong idea under our own name), as a ring-3 program (4.23.0). It steps once per kernel tick (100 a second, from `SYS_TASKS`) when poll has nothing, so the ball keeps its speed on any machine; a long stall steps at most five ticks. Only what moved is wiped and drawn again. Space serves the first ball. Up/Down or w/s move your paddle, the computer follows the ball once it crosses into its half, a little slower than it can move, a paddle hit sends the ball back steeper off the edges and a touch faster, first to 7 wins, p pauses, space plays again. Apps folder, `open=padd`. |
| `user/bricks.c` | Bricks, the brick-breaking game (the Breakout idea, under our own name), as a ring-3 program (4.23.0). Six rows of twelve bricks sized to the window, three balls, the score by row. The same tick clock and partial repaint as Paddle. A brick hit bounces the ball off the side with the smaller overlap; the paddle sends it back steeper off its ends, and never straight up. The ball rides the paddle until space launches it. Left/Right or a/d move the paddle, a click or drag puts it under the pointer, p pauses, space plays again. Apps folder, `open=bric`. |
| `user/depths.c` | Depths, a first-person maze shooter in the spirit of Doom, as a ring-3 program (4.23.0), all original. A grid raycaster in fixed point: positions in 1/65536 of a tile, a 256-step circle from a 64-entry quarter sine table, one DDA ray per two columns, walls shaded by distance and darker on y faces, the exit door pale. Creatures are billboards drawn far to near against a per-column depth buffer; they sleep until you first walk, turn or fire, then walk toward you inside seven tiles when no wall is on the straight line between, and bite when they reach you. No 64-bit division anywhere (i386 has no `__divdi3` here). Up/Down walk, Left/Right turn, space fires at the nearest creature in a cone ahead (wider up close, so one at your elbow can be hit) that no wall hides. It renders only when something changed, at most every third tick. Apps folder, `open=dept`. |
| `user/snake.c` | Snake as a ring-3 program (4.23.0). A grid sized to the window (up to 64 by 40 cells of 16 px), the body a ring of cell numbers head first, an apple from a seeded xorshift. It waits for the first arrow, then moves on the kernel tick clock, a cell every eight ticks and one tick faster every five apples down to three, and repaints only the cells that changed. A reverse onto the neck is ignored; a wall or the body ends the run. Paused, n moves one cell a press, and each new apple is logged, so a script can play it exactly. Apps folder, `open=snak`. |
| `user/chess.c` + `user/chess_core.h` | Chess against the computer, ring 3 (4.25.0), Apps folder and the Pi's Spotlight. `chess_core.h` holds the rules (a 64-square board, pseudo-legal generation, castling, en passant and promotion, a move that leaves the king in check refused) and the opponent (alpha-beta with captures played out, material and centre, deepening 2 to 4 plies while a depth stays under 40,000 positions), with no system calls so `tools/checks/chess-core-check.py` compiles it on the host and counts perft. The search keeps its positions and move lists in per-ply arrays: an i386 ring-3 program has one 4 KB stack page. `chess.c` draws the board and pixel-art pieces and takes the cursor, clicks or typed moves ("e2e4"). |
| `user/samcaps.h` | Samantha's captions (2.12.1), header-only and `#include`d into `user/samantha.c` to keep that file under the 2000 line ceiling. Up to three lines stacked from just above the glass input, newest at the bottom, each older one a step dimmer; a new line eases up into place over 0.28 s; the whole stack fades together 4 s after the talk goes quiet (0.6 s fade). In a wide window, where her mouth is low, it is one compact row above the input and none at all in a window too short to clear her lips. It prints `samface: line=`, `fade=` and `gone=` with her tick, which `samantha-fullscreen-check.py` reads. |
| `user/activity.c` | Activity, uptime, free memory and the six scheduler slots with a Kill button, as a ring-3 program: the twelfth app out of the kernel (1.9.6). Same screen as the old in-kernel copy, refreshed about once a second through the one new call, `SYS_TASKS`, which also does the kill and refuses the shell and the app itself. Backquote is the deliberate crash `tools/checks/ring3activity-check.py` can press. |
| `user/contacts.c` | Contacts, a list of up to 32 people with a name, phone and email kept in `CONTACTS.TXT`, as a ring-3 program: the thirteenth app out of the kernel (1.9.7). Same list, add prompt, person view and delete as the old in-kernel copy, and the file is rewritten whole after every change through the ordinary open, read, write and close calls, so it needed no new syscall. The list lives in the zeroed pages past the image, since a flat binary has no `.bss`. `tools/checks/ring3contacts-check.py` drives it. |
| `user/hikko.c` | Thinkpond, the idea forum, as a ring-3 program: the fourteenth app out of the kernel (1.9.8, as Sparkjar). Shows the real forum's top twelve ideas (title, votes, text, plan) from `/api/hikko` through `SYS_HTTP_GET` (2.6.29), and the ten demo ideas when the network, the reply or every row fails. Upvotes are local and never sent. `tools/checks/ring3hikko-check.py` drives the demo ideas, `tools/checks/ring3hikko-live-check.py` the live and fallback paths. |
| `user/reminders.c` | Reminders, a checklist of up to 24 items with a done flag kept in `REMINDERS.TXT`, as a ring-3 program: the fifteenth app out of the kernel (1.9.9). Same list, add prompt, tick and delete as the old in-kernel copy, and the file is rewritten whole after every change through the ordinary open, read, write and close calls, so it needed no new syscall. The list lives in the zeroed pages past the image, since a flat binary has no `.bss`. Samantha's reminder tools now read and write the same file fresh on every call from the ring-3 Samantha program. Driven by `tools/checks/ring3reminders-check.py`. |
| `user/curbfind.c` | Scrounge, Craigslist deals near the visitor ranked by deal score with the selected listing's price, neighbourhood, score bar and reason beside them, as a ring-3 program: the sixteenth app out of the kernel (1.9.11). Same list, detail pane and Vancouver samples as the old in-kernel copy. The live rows come through the one new call, `SYS_HTTP_GET`: the kernel fixes the host and port, the program names only the path, and a path that is not plain printable ASCII starting with `/`, or a buffer outside user memory, is refused before the network is touched. The `p` key is the probe `tools/checks/ring3curbfind-check.py` presses to prove those refusals. |
| `user/calendar.c` | Calendar, the Day, Week, Month and Year views with one event per day kept in `EVENTS.TXT` (and, since 6.19.0, a subscribed feed's days read from `SUBS.TXT`, never saved back, the person's own day winning), as a ring-3 program: the seventeenth app out of the kernel (1.9.12). Same views, keys and layout as the old in-kernel copy, drawn in the 8x16 font (the Year view halves it). Today comes from `SYS_TIME`, so the epoch-to-date and weekday math run in the program; `tools/checks/check-calendar.sh` sweeps that math against libc on the host. The file is rewritten whole after every save through the ordinary open, read, write and close calls, so it needed no new syscall. The event list lives in the zeroed pages past the image, since a flat binary has no `.bss`. Driven by `tools/checks/ring3calendar-check.py`. |
| `user/mail.c` | Mail, the inbox list, reader and compose sheet over `MAIL.TXT`, as a ring-3 program: the twenty-second app out of the kernel. Its in-kernel UI is gone; `kernel/mail.h` is now just the message array and load/save. |
| `user/mailpi.c` | Mail on the Pi before a sign-in, three panes at the Mac Mail scale (mailboxes, list, reading pane) over `JT/MAIL.TXT`. With no mail it says how to sign in (`mail login imap.mail.me.com NAME` with an app-specific password); 7.5.4 removed the made-up sample messages it used to show. Once mail is signed in, the Mail tile opens the real inbox in Blackcomb instead (6.32.0). |
| `user/notes.c` | Notes, the folder and note browser plus the in-window editor (wrap, caret, scroll, line copy/cut/paste, Home/End/Delete, Ctrl+S, and since 7.5.0 one level of Ctrl+Z or Cmd+Z undo that a second press redoes), as a ring-3 program and a compositor window: the twenty-fourth app out of the kernel. Folders and notes are made and removed with `SYS_MKDIR` and `SYS_UNLINK`. A note you have changed saves itself 1 to 2 seconds after your last key (`SYS_TIME`, whole seconds, read only while the note is dirty), because the window's red dot kills the program and Esc never runs; the title's star clears on each save. |
| `user/remindpi.c`, `tools/checks/arm64-reminders-check.py` | Reminders for the ARM build (3.17.0), the Pi's macOS-shaped list: a title and count, rows with round check circles (terracotta `#b5502c` when done, drawn with 4x4 sampling), and a text field along the bottom that is always there. Type and Enter adds; Up walks into the list, Space, Enter or a click on the circle ticks, Backspace or Delete removes the row; done items sink to the bottom. The Makefile builds it as the app named `reminders` (a rule that beats the generic per-app pattern, which would pick the i386 `user/reminders.c`), so the dock tile, Spotlight and `[[open Reminders]]` start it through `app_dock`. It keeps `JT/REMINDER.TXT` (8.3 on FAT) in i386's line format, `0` or `1`, a space, the text, rewritten whole after every change; with no card the open fails and the window says memory only. Check: boots `remw` and `remr` (`NOTES_SELFTEST` 3 and 4) on raspi4b, two adds and a tick on a card (mtools reads the file, fsck is clean), the item back after a second boot, and a no-card boot. |
| `tools/checks/notes-autosave-check.py` | Notes autosave on a headless QEMU guest. Types a line, wants no `notes: saved=` while keys are still coming and none in the first 0.95 s after the last, then exactly one save of the right length, closes through the red dot (reaper line on serial, no `notes: closed`), reads the note off the FAT image with mtools, reopens Notes from the dock and wants the text back on screen. Then builds a private copy with autosave off and wants this same check to fail on it. |
| `user/terminal.c` | The Terminal, as a ring-3 program and a compositor window: the twenty-fifth app out of the kernel. Same look as before (dark page, scrollback, `> ` prompt pinned to the bottom, block cursor, hint line), drawn with libjt's antialiased DejaVu Sans Mono (`jt_mono_draw`, 8 px advance). It keeps its own working directory, shows it in the prompt, handles `cd` itself and sends the cwd with every `SYS_SHELL_RUN` call. Commands go through the one new call `SYS_SHELL_RUN`; copy, cut and paste use an in-app one-line clipboard. Backquote is the deliberate crash. |
| `kernel/shellsys.c` + `kernel/shellsys.h` | The allowlisted shell behind `SYS_SHELL_RUN` (help echo uptime mem ps ls cat, relative to the caller's cwd; plus a `crash` that exists only under the `panicdesk` boot flag, for the panic check). It is not `run()`: the syscall runs on the 4KB kernel stack with interrupts off, so it uses static buffers and refuses anything that blocks, waits on the network, opens a window or halts, and never moves the desktop's cwd. See the System call ABI section. |
| `kernel/weather_extra.h` | The data side of the 2.10.0 Weather window, split out of `kernel.c` (its line ceiling only goes down): the 7-day and 24-hour statics, the JSON array readers (`json_section_array`, `json_array_clock` for sunrise, sunset and hour stamps), `wx_parse_extras` for everything past the temperature and condition, and `wx_write_extras`, which appends the `press`, `vis`, `isday`, `d` and `hs/ht/hc/hp/hd` lines to `WEATHER.TXT`. Included by `kernel/weather.c` just before `weather_fetch_inner`. |
| `kernel/stocks.c` + `kernel/stocks.h` | The data side of Stocks, now that the app is `user/stocks.c`: the quote table, the strict row parser, `stocks_fetch` (heap buffers, `/api/stocks?range=N`), `stocks_write_file` leaving `STOCKS.TXT` for the app, and `stocks_ring3_open`, the blocking path (phone grid, Apps folder) that fetches once and runs the app once. The window refetches through `SYS_REFRESH`. Its own unit since 4.20.13 (a header kernel.c included before). |
| `kernel/clockicon.c` + `kernel/clockicon.h` | The Clock tile as a live analog face (its own unit since 4.20.11, a header kernel.c included before that): white dial, ticks and hour, minute and centre hands drawn in physical pixels over the plain cached tile, redrawn when the minute changes. Also the one hook that lays Clock's hands on a tile; Calendar is plain art with nothing drawn over it. |
| `user/search.c` | Search, the query box that filters the current directory as you type, as a ring-3 program: the eighteenth app out of the kernel (1.9.13). Same list, live filter, folder step-in and cat-style file view as the old in-kernel copy. It needed one new call, `SYS_READDIR`, which fills fixed-size records (name, size, is_dir) for a directory named by a path relative to the shell's directory; the kernel walks the path and walks back inside the call, so the app keeps its own cwd string and nothing outside it moves. `SYS_OPEN` takes the same relative paths, so a file inside a folder opens as `DOCS/NAME`. The records and the file buffer live in the zeroed pages past the image. Backquote is the deliberate crash. Driven by `tools/checks/ring3search-check.py`. |
| `user/burrow.c` | Files (called Burrow from 2.26.0 to 8.9.0; the file keeps the name), as a ring-3 program: a List and Icons toolbar, folders first, arrows to move, Enter to open a folder, Backspace to go up, Esc to close. It keeps its own folder path and lists it through `SYS_READDIR`. |
| `user/epiphany.c` | Epiphany, the markets terminal (a 40-ticker watchlist, portfolio, trading simulator, macro brief and the `/` command bar for `AAPL GP` and `AAPL DES`), as a ring-3 program: the nineteenth app out of the kernel (1.9.19). Prices come through `SYS_HTTP_GET` from `/api/quotes` (about 700 bytes, inside the 2 KB body), with the compiled-in prices when the fetch fails. The GP chart draws previous close to last price, because the intraday series lives in the kernel's Stocks and does not fit the syscall body. `tools/checks/ring3epiphany-check.py` drives it. |
| `user/weather_pi.c`, `tools/checks/arm64-weather-check.py` | Weather on the ARM64 build (3.16.0), a small ring-3 program the Makefile builds as `weather` (the dock tile named Weather, Spotlight and Samantha's `[[open weather]]` find it by that name). City, the temperature large, a flat condition glyph from discs and capsules, high and low, and five day cards. It reads `WEATHER.TXT` (the format `user/weather.c` documents) through `jt_readfile`; since 7.6.0 opening it makes the kernel fetch the reading for where the Pi is (`arch/arm64/feeds.c`, the Worker's `/api/weather`); with no reading it says "No reading right now" and draws nothing made up; a failed fetch keeps the last good reading. Check (the `feedsfix` build reads a fixture from the check's own server, and a second boot with no network shows no reading): opens it from the tile and from Spotlight in headless QEMU, finds the temperature ink, the terracotta sun and the five cards in a screendump, and closes it with Esc and the red dot. |
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

#### Storage

| File | What it does |
|---|---|
| `drivers/ata.c` | Reads and writes the hard disk. |
| `drivers/blockdev.c` | One `read_sector` / `write_sector` table so the filesystem does not care whether the bytes come from a disk or from RAM. |
| `drivers/ramdisk.c` | The RAM-backed block device. |
| `drivers/fat.c` | FAT16 with real subdirectories, writes and 8.3 names. |
| `drivers/ramfs.c` | A tiny flat in-memory filesystem, eight files of 4KB. Exists so the VFS has a second backend to prove it abstracts anything. |
| `drivers/vfs.c` | The filesystem switch. Every app and every shell command goes through here instead of calling FAT directly. |
| `drivers/trash.c` | Recoverable delete. |

#### Network

| File | What it does |
|---|---|
| `drivers/pci.c` | Walks the PCI bus so the kernel can find its own network card. |
| `drivers/rtl8139.c` | Driver for the RTL8139 card, QEMU's default. |
| `drivers/ne2k.c` | Driver for the NE2000 card, which is what the browser emulator provides. |
| `drivers/net.c`, `drivers/dns.h` | Ethernet, ARP, IPv4, UDP, DNS and TCP, built up from raw frames on top of whichever card was found (on ARM, the card behind `drivers/nic.h`). |
| `drivers/http.c` | `http_get` and `http_post` over that TCP. Plain HTTP only. There is no TLS. |
| `drivers/html.c` | A deliberately tiny HTML-to-text converter, enough to read a page or a ported app. |
| `drivers/json.c` | A small JSON reader for the weather, geolocation and chat responses. |

The network stack is what fetches the weather in the menu bar, the map tiles for the wallpaper, stock quotes, and the replies from a local Ollama server in the Chat app.

#### Sound

| File | What it does |
|---|---|
| `drivers/sb16.c` | The Sound Blaster 16 driver: DSP reset/detect, IRQ 5, and ISA DMA channel 1 for both directions. |
| `drivers/speak.c` | Text to speech: posts to Turing's `/api/speak`, plays the PCM reply on `sb16.c`, and tracks its own playback position so Chat's face can move with it (`speak_level`). |

#### Worker endpoints

The browser landing page routes kernel HTTP requests to a Cloudflare Worker (`worker.js`) for external APIs:

| Endpoint | What it does |
|---|---|
| `/api/stocks` | Live stock quotes for the Stocks app. |
| `/api/books` | The real Bookrank shelf (top twelve ranked books) as plain text for the Bookrank app. |
| `/api/hikko` | The real Thinkpond forum's top twelve ideas as plain text (`votes|category|title|text| plan`), ASCII only, read-only, for the Thinkpond app. |
| `/api/lexly` | The real Patois (was Patois, was Lexly) course list as plain text, and with `?c=<id>` up to twenty four-choice questions from one course, ASCII only, for the Patois app. |
| `/api/chat` | LLM routing and chat responses for the Chat app. |
| `/api/pick` | LLM tool selection for Chat's command execution. |
| `/api/waitlist` | Dev-kit waitlist signup (POST) and count (GET). Stores one email per address with a timestamp. |
| `/api/listen` | Speech-to-text for Chat's push-to-talk: runs Workers AI Whisper over a posted audio clip. |

#### Graphics and input

| File | What it does |
|---|---|
| `drivers/vbe.c` | Switches the screen into 1920x1080 graphics. |
| `drivers/window.c` | The one drawing target everything renders through: pixels, rectangles, a viewport, and a partial-repaint band. |
| `drivers/font.c` | Reads the real 8x16 IBM font out of VGA hardware at boot. |
| `drivers/png.c` + `drivers/jpeg.c` | Small image decoders, 8-bit RGB and RGBA PNG and baseline JPEG (and progressive since 6.46.0: `prog_block` decodes each scan's DC, AC and refinement. |
| `drivers/inflate.c` + `drivers/inflate.h` | The kernel's one DEFLATE inflate (plus Adler-32). |
| `tools/inflate-host/main.c` | Host harness for `drivers/inflate.c`. |
| `drivers/ttf.c` | A runtime TrueType rasterizer, so text scales to any size. |
| `drivers/dejavu_font.h` and siblings | The six DejaVu faces, Sans, Serif and Mono in regular and bold, as ASCII plus Latin-1 subsets: `dejavu_bold_font.h`, `dejavu_serif_font.h`. |
| `drivers/lib_inter_font.h` and siblings | The font library: eight open faces (Inter, EB Garamond, Comic Neue, Caveat Brush, Dancing Script, JetBrains Mono, Source Serif, Source Sans) as ASCII. |
| `drivers/mouse.c` | The PS/2 mouse, including the wheel. |
| `drivers/vmmouse.c` | The VMware absolute-pointer interface. |
| `drivers/serial.c` | COM1 output for debugging. A boot trace you can read with `-serial stdio` even after the screen is dead. |
| `kernel/console.h` | The three text-console functions the kernel uses before the desktop exists. |

The desktop is laid out at 960x540 and drawn at 1920x1080.

#### The desktop

| File | What it does |
|---|---|
| `kernel/wallmap.c` + `kernel/wallmap.h` | The map and satellite wallpapers, out of kernel.c since 4.18.3: the tile fetch around the place `geo_*` names. |
| `kernel/settings.c` + `kernel/settings.h` | `SETTINGS.TXT`, out of kernel.c since 4.18.4: `settings_load` once at boot (every key bounds-checked, a missing or. |
| `kernel/selftest.c` + `kernel/selftest.h` | The shell's built-in self-tests that need no kernel.c state, out of `run()` since 4.18.5: `pngtest` and `jpegtest`. |
| `kernel/taskdemo.c` | The scheduler demos, out of `run()` since 4.18.6: `tasktest`, `fputest`, `preempttest`, `killtest`, `spawntest`. |
| `kernel/shellfs.c` | The shell's file commands, out of `run()` since 4.18.7: `ls`, `cat`, `write`, `rm` (into the trash first), `trash`. |
| `kernel/shellnet.c` | The shell's network commands, out of `run()` since 4.19.2: `lspci`, `ifconfig`, `netscan`, `web` (the text browser. |
| `kernel/wxtest.c` + `kernel/walltint.h` + `kernel/desktop.h` | The weather and wallpaper self-tests, out of `run()` since 4.19.2: `weathertest`, `weatherfxtest`, `weatherfxcliptest`. |
| `kernel/multiwin.c` + `kernel/multiwin.h` | The desktop's window manager, out of kernel.c since 4.19.2: the window table (`gui_windows`, up to `GUI_MULTIWIN_MAX`. |
| `kernel/progtest.c` | The ring-3 reference programs end to end, out of kernel.c since 4.20.1: `usertest` runs user/hello.c off the VFS and. |
| `kernel/guitest.c` | The desktop's drawing self-tests, out of kernel.c's `run()` since 4.20.2: `cursortest` (the cursor's save and restore. |
| `kernel/panic.c` | The panic screen, out of kernel.c since 4.20.4: when a ring-0 exception hits with the GUI up, `kernel/idt.c` calls. |
| `kernel/gui_shapes.c` + `kernel/gui_shapes.h` | Two i386 shape painters, out of kernel.c since 4.20.5: `gui_aa_line_phys`, an antialiased line drawn per physical pixel. |
| `kernel/wallpaint.c` + `kernel/wallpaint.h` | The i386 wallpaper painters, out of kernel.c since 4.23.6: the day/night tint by the real hour (`daynight_calc` is pure. |
| `kernel/uitext.c` + `kernel/uitext.h` | The i386 antialiased UI text, out of kernel.c since 4.23.7: `uitext_fonts_preload` (4.27.0: every face parsed and tried. |
| `kernel/menubar.c` + `kernel/menubar.h` | The i386 menu bar, out of kernel.c since 4.23.8: the mark, the clock (CMOS weekday, date and 12-hour time), the. |
| `kernel/appview.c` + `kernel/appview.h` | The i386 app viewport, out of kernel.c since 4.23.8: `gui_app_windowed` and the content shift `gui_app_dy`, the live. |
| `kernel/desktop.c` | The i386 desktop's event loop, out of kernel.c since 4.23.9: `gui_run` opens the screen at the boot line's size (or the. |
| `kernel/kmain.c` | `kmain`, the i386 kernel's entry from `boot/boot.S`, out of kernel.c since 4.25.1: multiboot and memory, paging, the. |
| `kernel/klog.c` + `kernel/klog.h` | The kernel log, out of kernel.c since 4.20.6: a 32-line ring that boot steps write with `klog()`, each line tagged with. |
| `kernel/icons.c` + `kernel/icons.h` | The i386 app icons, out of kernel.c since 4.20.7: one painter per app (`gui_icon_*`, named by the APPS table in. |
| `kernel/appsfolder.c` + `kernel/appsfolder.h` | The Apps folder (Launchpad), out of kernel.c since 4.20.8: every app as a grid on glass over the wallpaper, sized by. |
| `kernel/bootscreen.c` + `kernel/bootscreen.h` | The brand mark, the boot splash and About, out of kernel.c since 4.20.9: `gui_draw_mark_sized` blends the baked. |
| `kernel/menu.c` + `kernel/menu.h` | The Apple menu off the tree mark and the menu-bar weather dropdown, out of kernel.c since 4.20.10: the rows and their. |
| `kernel/weather.c` + `kernel/weather.h` | The menu-bar weather, out of kernel.c since 4.12.3: ip-api (or `loc=` through Open-Meteo's geocoder) for the place. |
| `kernel/kernel.c` | The big one. |
| `kernel/ttf_render.h` | The shared glyph path for anything drawing real DejaVu text at physical resolution: a per-face cache, a glyph cache. |
| `kernel/gui_prims.c` | The x87 square root the antialiased lines use. i386 only. |
| `kernel/gui_paint.c`, `kernel/gui_paint.h` | The desktop painters both builds share (2.22.0): colour blend and lerp, the rounded rect on the wallpaper, the dock. |
| `kernel/dock_geom.c` | Dock geometry and hit-testing: where each icon sits at the current scale, and which slot a click landed on. |
| `kernel/dock_draw.c` | The i386 dock's animation: the band cache behind hover and drag, and the icons. |
| `kernel/apps_geom.h` | Where the Launchpad (Apps folder) puts its glass panel, icons and labels. |
| `kernel/hint.c` + `kernel/hint.h` | The keyboard-hint line every app draws ("esc closes" and friends), skipped on phones where there is no keyboard. |
| `kernel/app.h` | The app interface. |
| `kernel/app.c` | The helpers behind `app.h`, only ones two or more apps were writing by hand: start an app's window with its titlebar. |
| `kernel/bench.c` + `kernel/bench.h` | Built-in benchmarks: boot time, heap, memcpy, context switch, disk read. `bench` in the shell or on the command line. |
| `kernel/auth.c` + `kernel/auth.h` + `kernel/auth_env.h` | User accounts (its own unit since 4.20.15; `auth_env.h` is the kernel surface it calls, swapped for stand-ins when. |
| `kernel/auth_kdf.c` + `kernel/auth_kdf.h` | PBKDF2-HMAC-SHA256 (RFC 8018) as one block loop over BearSSL's `br_hmac`; no primitive of its own. |
| `kernel/wall_sat.h` | A real satellite photo, baked in, used as the wallpaper when there is no network to fetch map tiles. |
| `user/stocks_pi.c`, `tools/checks/arm64-stocks-check.py` | Stocks on the ARM64 build (3.18.0), a small ring-3 program the Makefile builds as `stocks` (the dock tile named Stocks. |
| `arch/arm64/feeds.c` | 7.6.0: live data held in RAM. |
| `kernel/boot_mark.h` | The brand mark as 8-bit coverage, written by `tools/gen/gen_boot_mark.py` from the painted tree's flat glyph. |
| `kernel/phone_home.c` + `kernel/phone_home.h` | The phone home screen for `boot_to_phone`: a 5-column, no-scroll grid of all 26 apps, a status bar with the real clock. |
| `kernel/shell.c` + `kernel/shell.h` | The shell's dispatcher, out of kernel.c since 4.20.21: `split_argv` and `run()`, where a typed line becomes a command. |
| `kernel/settings_ui.c` + `kernel/settings_ui.h` | Settings app UI with a sidebar plus grouped detail pane (modeled on macOS System Settings). |

### The apps

Twenty-six apps live in the Apps folder. The dock pins the ones you reach for most. Each is one header. They come in three shapes.

Renames (2.6.30, and the fleet since):
- Lexly is Tonchi, now Patois.
- Roost is Brick, now Atlin.
- Voxprint is Notate.
- Siftbox is Hagaki, now Mailbag.
- Sparkjar is Hikko, now Thinkpond.
- Windgate is Airwell.
- Healstack is Intake.

Samantha still opens an app by its old name. Patois keeps `lexly.jaybulb.com` and `/api/lexly`, and Thinkpond keeps `hikko.jaybulb.com`, until their own addresses exist. Block Frame still answers at its old `wiretext` address.

**Apps with a file on disk.** It is the same pattern every time:
- a fixed-size array in RAM
- one plain text file on the FAT disk
- written through on every change

There is no Save button.

| App | File | On disk |
|---|---|---|
| Notes | `user/notes.c`, `kernel/editor.h` | Folders and many notes under `NOTES/`; an old `NOTES.TXT` moves in as the first note. |
| Reminders | `user/reminders.c`, `kernel/ring3app.c` | `REMINDERS.TXT`, one line per item. The fifteenth ring-3 app (1.9.9); its in-kernel copy is gone. |
| Calendar | `user/calendar.c`, `kernel/ring3app.c` | `EVENTS.TXT`, one line per date. |
| Mail | `user/mail.c`, `kernel/mail.h` | `MAIL.TXT`, one line per message. |
| Contacts | `user/contacts.c`, `kernel/ring3app.c` | `CONTACTS.TXT`. The thirteenth ring-3 app (1.9.7); its in-kernel copy is gone. |
| Samantha | `user/samantha.c`, `kernel/ring3app.c` | `CHAT.TXT`. |

**Samantha, in detail.**

Push-to-talk:
- Holding F2 records through `SYS_AUDIO_RECORD`.
- It posts the clip to the Worker's `/api/listen` (Cloudflare Workers AI Whisper).
- The recognized text runs through the same path a typed message takes.

Full screen since 2.9.0:
- Her 736 pixel portrait fills the window. It is `landing/face/hd.jpg`, one fetch of 59 KB through the Worker's `/face/` route.
- A tall window, the phone, shows all of her edge to edge.
- Since 2.12.1 a wide window gives her room.
- The input line is a blurred glass panel.
- What is said shows as captions that rise from just above the input.
- In a wide window, her mouth is low and her chin travels down toward the panel. So there is one compact row only, and never over her lips or eyes.
- Tab opens the whole conversation.
- A red dot or Esc closes her.
- Her mouth is drawn by code from the loudness of the PCM she is playing.
- A host without the portrait leaves the old 320 pixel idle and talk frames, scaled up.

**Apps with nothing to save.**

| App | File | What it is |
|---|---|---|
| Stocks | `kernel/stocks.c` | Eight fixed symbols with live quotes and charts from the Worker at `/api/stocks`; the UI is `user/stocks.c`. |
| Epiphany | `user/epiphany.c`, `kernel/ring3app.c` | Watchlist, portfolio, simulator and a Bloomberg-style command bar (`/`, then `AAPL GP` or `AAPL DES`), live prices. |
| Search | `user/search.c`, `kernel/ring3app.c` | Filters the current directory as you type. |
| Portfolio | `user/portfolio.c`, `kernel/ring3app.c` | A catalog of every app in the fleet with its URL. The eleventh ring-3 app (1.9.5); its in-kernel copy is gone. |
| Activity | `user/activity.c`, `kernel/ring3app.c` | Activity Monitor over the real scheduler and memory counters. |
| Clock | `user/clock.c`, `kernel/ring3app.c` | Current time from the RTC, a countdown timer you can start and pause, and an alarm. |
| Music | `user/music.c`, `kernel/ring3app.c` | Plays the `.WAV` and `.MP3` songs in Files and in a `MUSIC` folder. |
| Movies | `user/movies.c`, `kernel/ring3app.c` | Plays motion-JPEG AVI clips from Files with sound, pause, seek and fullscreen. |

**Apps ported from the fleet.** Each is a native rewrite of one of the sibling web apps, kept small on purpose.

| App | File | What it is |
|---|---|---|
| Scrounge | `user/curbfind.c`, `kernel/ring3app.c` | Craigslist deals near you, ranked by score, live through `SYS_HTTP_GET` with Vancouver samples when offline. |
| Patois | `user/tonchi.c`, `kernel/ring3app.c` | Learn anything: pick a live course, then a four-choice drill with a score. |
| Fieldbook | `user/fieldbook.c`, `kernel/ring3app.c` | Every field of science and math, explained plainly. The ninth ring-3 app (1.9.3); its in-kernel copy is gone. |
| Thinkpond | `user/hikko.c`, `kernel/ring3app.c` | The idea forum: the top ideas from the real forum, vote on them for the session only. |
| Keyrate | `user/keyrate.c`, `kernel/ring3app.c` | Typing test with endless random words and a live words-per-minute count. |
| Toroid | `user/toroid.c`, `kernel/ring3app.c` | Conway's Life on a torus. The second ring-3 app (1.7.11); its in-kernel copy is gone. |
| Calculator | `user/calculator.c`, `kernel/ring3app.c` | Recursive-descent parser over `+ - * / ()`. The third ring-3 app (1.7.12); its in-kernel copy is gone. |
| Quotes | `user/quotes.c`, `kernel/ring3app.c` | Name the film from the line. |
| Bookrank | `user/bookrank.c`, `kernel/ring3app.c` | Ranked non-fiction: a list on the left, the selected book's title, author, rating, review count and summary on the. |
| Hamurabi (rules) | `user/hamurabi_rules.h`, `user/hamurabi_story.h`, `art/hamurabi/story.json` | The game's rules, robot king and story logic as plain C with no libc, no floats and no 64-bit division, so an app can. |
| Hamurapi | `user/hamurabi.c`, `user/hamurabi_sprites.h`, `art/hamurabi/sprites.png` | The 1968 game of Hamurabi, rebuilt with a living city, shown as Hamurapi because the store name was taken (the files. |
| Airwell | `user/windgate.c`, `kernel/ring3app.c` | Guided breathing: a circle grows on the in-breath, holds, shrinks on the out-breath. |
| Panes | `user/panes.c`, `user/shellcore.h`, `kernel/ring3app.c` | Terminal tabs and split panes, cmux-style: a tab rail, a shell per pane, an activity dot on a tab that printed in the. |
| Claude | `user/claude.c`, `tools/claude-relay/relay.py`, `kernel/ring3app.c` | Ask Claude Code a question from inside Joshua Tree. |
| Mines | `user/mines.c`, `kernel/ring3app.c` | Minesweeper, the 9x9 beginner board. In the Apps folder, opened with `open=mine`. A ring-3 app from the start. |
| Solitaire | `user/solitaire.c`, `kernel/ring3app.c` | Klondike, draw one. In the Apps folder, opened with `open=soli`. A ring-3 app from the start. |
| Paddle | `user/paddle.c`, `kernel/ring3app.c` | A bounce game against the computer. In the Apps folder, opened with `open=padd`. A ring-3 app from the start. |
| Bricks | `user/bricks.c`, `kernel/ring3app.c` | The brick-breaking game. In the Apps folder, opened with `open=bric`. A ring-3 app from the start. |
| Depths | `user/depths.c`, `kernel/ring3app.c` | A first-person maze shooter. In the Apps folder, opened with `open=dept`. A ring-3 app from the start. |
| Snake | `user/snake.c`, `kernel/ring3app.c` | Snake. In the Apps folder, opened with `open=snak`. A ring-3 app from the start. |
| Books | `user/books.c`, `kernel/ring3app.c` | Plain-text reader. |

**Adding an app.** Every app is one row in `APPS[]` in `kernel/kernel.c`. Nothing else dispatches on an app's index. The dock, the Apps folder, Chat's `open_app` and the multiwindow desktop all read that table.

The steps:
1. Write the app as a ring-3 program in `user/<app>.c`, the way Keyrate and Toroid do.
2. Add the row.
3. Add the `.c` to the Makefile.
4. Bump `GUI_APP_COUNT`.

Give it `draw` and `key` only if it should also run as a desktop window. Reach for `app.h`'s helpers before writing your own.

The `drivers/app_*.h` files are the original single-file web builds of those apps as C byte arrays, generated by `tools/gen/gen_app.sh`. The `serveapp` shell command serves one over the kernel's own TCP, so you can `curl` it from the host.

The `drivers/user_hello.h` and `drivers/user_note.h` arrays are the two user programs. They are embedded the same way, so a fresh disk has something to `exec`.

### Host-side tools

None of this is kernel code. It builds and runs on the Mac.

**Native harnesses** compile one kernel file against a tiny fake of `kmalloc` and `memcpy`. So it can be tested without booting QEMU.
- `tools/png-host/main.c`, `tools/jpeg-host/main.c`, `tools/ttf-host/` and `tools/auth-host/main.c` each do this for a decoder, the font rasterizer or `auth.c`.
- `tools/libjt-host/main.c` does the same for the user-space C library.
- The `kheap.h`, `libc.h` and `irq.h` shims next to each one are the fakes.
- `tools/png-host/wallsat_check.c` decodes the baked satellite photo and checks it comes out 960x540.
- `drivers/png_testdata.h` and `drivers/jpeg_testdata.h` are generated test images, and so is `tools/jpeg-host/jpeg_progdata.h` (6.46.0's progressive crops).

**Fuzzers** in `tools/fuzz-host/` hammer the code that reads untrusted bytes.
- `fuzz_parsers.c` covers HTTP, HTML and JSON. `kmalloc_host.c` is the one heap call `drivers/fat.c` makes, as malloc, for every host build of it.
- `fuzz_decoders.c` covers PNG and JPEG.
- `fuzz_browser.c` (6.48.2) covers `arch/arm64/browser.c`'s `to_text` (tags, entities, `<style>` rules, forms, then `fill` and `open` on every field) and `take_cookies`.
- `fuzz_appsig.c` (6.54.0) covers `arch/arm64/appsig.c` with the vendored BearSSL built for the host: a throwaway key signs an image through `tools/signapp.sh`, then every cut and mutation of the.
- `fuzz_uimg.c` (6.51.0) covers `arch/arm64/uimg.h`'s `uimg_ok`, the app image check, and applies every relocation it passes into a buffer of the image's own size; `tools/checks/uimg-fuzz-check.sh` runs it.
- `fuzz_dns.c` (6.48.5) covers `drivers/dns.h`'s `dns_answer`, the DNS reply reader `drivers/net.c` calls in both kernels; `tools/checks/dns-fuzz-check.sh` runs it.
- `fuzz_wifi.c` (6.48.4) covers `arch/arm64/wifi_proto.h`'s `escan_walk` (scan results, chosen by any access point in range), `sdpcm_parse` and `bcdc_reply`, with every short scan-entry length and a.
- `fuzz_mail.c` (6.48.3) covers `arch/arm64/mail.c`'s IMAP reply readers (`exists`, `next_fetch`, `field`) and the MIME ones (`pick_part`, `unqp`, `unb64`); `tools/checks/mail-fuzz-check.sh` runs it.
- `fuzz_ics.c` (6.48.1) covers `arch/arm64/ics.c`: the iCalendar feed reader and the CalDAV REPORT and PROPFIND walks.
- Both run under address and undefined-behaviour sanitizers, using the shims in `tools/parsers-host/`.

**Generators** in `tools/gen/` produce the embedded data:
- the icon art, from hand-drawn SVGs
- the editor fonts
- the VGA font fallback
- the satellite wallpaper
- the user program arrays
- the ported app arrays
- the fact row and progress chart on the landing page
- Hamurabi's story data: `gen_hamurabi_story.py` turns `art/hamurabi/story.json` into `user/hamurabi_story.h`, and `gen_hamurabi_sprites.py` turns `art/hamurabi/sprites.png` into `user/hamurabi_sprites.h`

**Checks** in `tools/checks/` are the regression suite.
- `check.sh` proves the kernel boots.
- `qa-gallery.py` opens every app in a headless QEMU and looks at the pixels.
- `feature-drive.py` performs each app's main action.
- `soak-check.py` opens and closes everything many times and checks nothing leaked.
- `frametime-check.py` fails if a repaint gets slow.
- `usertest-check.sh` runs the ring-3 programs and asserts their output.
- The rest each pin one specific thing that once broke: dock shadows, icon lighting, the lock screen, the panic screen, the Apps folder layout, the landing page facts.
- CI runs them all on every pull request.

**The dev loop.**
- `tools/watch.sh` keeps a QEMU window open and reboots it when the built kernel actually changes.
- `tools/qa-demo.sh` records a headless tour of the desktop to video.
- `tools/voice-control.sh` lets you speak a shell command into a running instance through Whisper and Ollama.

**The ad.** It lives in `landing/ad/` and plays live from the same clock the video was captured from.

| File | What it does |
|---|---|
| `landing/ad/index.html` | The Joshua Tree 2.0 and Neo Kit promo, one self-contained page. |
| `landing/ad/vendor/three.module.min.js` | Vendored three.js r160 (pinned, MIT) so the ad has no runtime network dependency. |
| `landing/ad/vendor/STLLoader.js` | Vendored three.js r160 STLLoader (MIT), loads the real Neo parts. |
| `landing/ad/stl/*.stl` | The retired Neo case's parts, kept here only for the promo page; the case's CAD was removed in 4.11.6. |
| `tools/ad-capture.mjs` | Deterministic capture for the ad: serves the repo, steps `window.__ad.render(t)` frame by frame in headless Chromium. |
| `landing/ad/desktop-a.jpg` | Real 1920x1080 framebuffer capture of today's desktop with Samantha snapped left and Notes snapped right, her input. |
| `landing/ad/desktop-typed.jpg` | The same desktop with "Hi Samantha" typed into her input and not yet sent, shown from 23.2 s so the nod has something. |
| `landing/ad/desktop-b.jpg` | The same desktop one beat later: "Hi Samantha" sent and her reply showing, swapped in when the typed line lands. |
| `landing/ad/icons/*.png` | The 26 app icons as the OS draws them: 23 rendered from `art/icons/*.svg`, plus Portfolio, Activity and Clock cropped. |
| `tools/ad-desktop-capture.py` | Headless QEMU run that drives the real kernel over QMP (Apps grid scroll, snap Samantha and Notes, send a line) and. |
| `tools/ad-audio.py` | Narration and sound for the ad: Samantha's ElevenLabs voice per line, the `docs/hardware/ad/music2.py` bed cut to 30 s. |
| `landing/joshua-tree-ad-v13-nimbus.mp4` | The film on the landing (13.x), in the calm Fable style: 29 s, 1920x1080 h264, 16 steady shots under one curved horizon. |
| `landing/joshua-tree-ad-v13-nimbus-poster.jpg` | The film's poster: the end card, the mark and Joshua Tree on the desert horizon. |

### Where to go next

- The Benchmarks section: how fast it is. Use `tools/bench.sh` to measure it yourself.
- `docs/ROADMAP.md` (Whitepaper): why this exists and what it is not yet.
- The System call ABI section: if you want to write a program for it.
- `docs/ROADMAP.md` (Blueprint: where the OS goes after 1.0): the plan after 1.0, with apps as real processes and a window server.
- The Threat model section: what the login screen does and does not protect.

### More source files

Generated headers, vendored libraries and the Plank C library, which the layers above do not name one by one.

| File | What it does |
|---|---|
| `arch/arm64/face_art.h` | Samantha's face, two frames (mouth closed and open), written by `tools/gen/gen_face_art.py` from `landing/face`. |
| `drivers/dejavu_mono_font.h`, `drivers/dejavu_mono_bold_font.h`, `drivers/dejavu_serif_bold_font.h` | DejaVu fonts embedded for `ttf_load()`, generated by `tools/gen/gen_ttf_font.py` (public domain). |
| `drivers/stb_truetype.h` | Vendored stb_truetype v1.26 (public domain), the font parser behind the TrueType text. |
| `kernel/symtab_stub.c` | An empty symbol table, linked only into the first-pass kernel so `backtrace.c` has something to resolve. |
| `store/fortune.c`, `store/dice.c` | The first two store apps, built with the SDK and installed with `install NAME`. |
| `user/libjt/stdio.c`, `user/libjt/unistd.h` | Unix-shaped names over `jtsys.h` for code that expects `read()`, `write()` and `close()`. |
| `user/pklibc/pklibc.c`, `user/pklibc/jtw.c`, `user/pklibc/setjmp.S` | The C library Plank programs need on the box, its window calls for `extern fn`, and `setjmp` and `longjmp` for aarch64. |
| `user/pklibc/include/stdio.h`, `math.h`, `time.h`, `unistd.h`, `dirent.h`, `regex.h`, `setjmp.h`, `sys/stat.h`, `sys/wait.h` | Headers for that library. `regex.h` reports no support yet. |

</details>

## ARM64 and the Raspberry Pi

Status (2026-10-10): the desktop, USB keyboard, Wi-Fi and HTTPS fetches have run on Joshua's Raspberry Pi 4.
- The 2.34.0 development card boots.
- The 2.35.0 Calendar icon, Clock and Samantha dock launch are in PR #492. Focused QEMU checks are done. A full local suite is pending.
- USB mouse, hardware RNG output and a real Samantha answer still need a board check.

<details><summary>Details (80 lines)</summary>


If this is accepted, it replaces the x86 board in `docs/GUIDE.md` (Hardware plan) as the 3.0 reference. The OS stays free. The board is what we sell around it.

For the current app list, see [Apps currently implemented on ARM](GUIDE.md#raspberry-pi). The dock artwork is shared with i386. Most of its apps are not ported yet.

### Why a Pi and not the Mac mini

The kernel is 32-bit x86. The Mac mini M4 is Apple Silicon.
- It has no UEFI and a custom boot chain.
- USB, storage and sound sit behind Apple-only controllers.
- Bare-metal there means an Asahi-sized effort.

A Raspberry Pi boots a plain `kernel8.img` from its firmware. It has a documented framebuffer, UART and interrupt controller. QEMU models parts of it.

Start with a Pi 4B. The Pi 5 hangs every peripheral off the RP1 chip over PCIe, which makes first bring-up harder. Move to it second.

### What the port touches

The kernel, drivers and lib are C, plus the user programs.

- Arch-specific and must be rewritten for AArch64: `boot/boot.S`, the linker script, GDT, IDT, PIC, ISR and IRQ stubs, paging, task switch, ring 3 entry and exit, and the 105 inline-asm blocks.
- PC-only drivers that go away and get Pi equivalents: ATA, PS/2 keyboard and mouse, VBE, vmmouse, PCI, RTL8139 and NE2000, Sound Blaster 16.
- Port I/O (`inb`, `outb`) shows up in 13 files and becomes memory-mapped I/O.
- Portable as is, in theory: the UI, fonts, JPEG and PNG decoders, FAT, the network stack above the NIC, the HTTP client, BearSSL, and the apps' C code.

### Console scrollback

The on-screen Console is the only debug channel on a Pi with no serial cable. The boot prints more lines than the window holds.

Since 2.19.0 it keeps the last 16 KB of the log, and a keyboard scrolls it:
- Page Up and Page Down move by half a page.
- Home goes to the first line. End goes to the newest.
- The title bar reads "lines 12-27 of 61".
- The bottom row of the window pins the newest `wifi` line and the newest `usb` line (key echoes excluded), cut to fit.
- New output while scrolled back is logged but does not move the view.

`tools/checks/arm64-console-scroll-check.py` proves it with QEMU screendumps.

### Back buffer and dirty rectangles (4.3.0)

On the board, dragging or resizing a window flickered and smeared: every painter drew straight onto the scanout, so the screen showed the wallpaper going down and the window going back up, a step at a time.

A moved or resized Calculator or app window repaints only the union of its old and new rectangles.

The back buffer and the scratch screen are 16 MiB each, in the 2 MiB blocks right after the 16 MiB kernel heap.

### Network stack

The ARM build runs the same IP stack as the i386 kernel. It is not a copy. `drivers/net.c` (ARP, IPv4, UDP, DNS, TCP, DHCP) and `drivers/http.c` compile for both.

The only seam is `drivers/nic.h`. It has four calls: bring the card up, read its MAC, send a frame, poll for one.

On QEMU, the virtio-net driver in `arch/arm64/main.c` answers them. `arch/arm64/ip.c` adds a 100 Hz clock.

Next, in order:
1. Wi-Fi joins a network below the same four calls (the CYW43455 driver).
2. TCP that survives a lost segment, which a real radio needs and QEMU never tests.
3. TLS, later, through the BearSSL the i386 build already has.

### Risks

- USB through xHCI is the largest single driver and the first thing a real board needs.
- Sound on a Pi has no Sound Blaster equivalent. Budget real time for it.
- The browser demo is i386 under v86 and stays that way. Two architectures means two builds and a second CI lane. Keep the i386 path green the whole time.
- `ring3` and task switching are where x86 assumptions are deepest. Read the Architecture section before M3.

### Errors and crashes

The tests for this section are `tools/checks/arm64-crash-check.py`, `arm64-fp-check.py`, `arm64-oom-check.py` and `arm64-boot-health-check.py`.

- **A kernel fault.**
  - An unexpected exception at EL1 prints `KERNEL CRASH: <class>`, the ESR, FAR and ELR and the last five console lines on the UART.
  - It draws the same text as a red panel on the screen, and sleeps in a `wfe` loop.
  - It uses only the raw UART and the 8x16 VGA font. So it works if the fault came from the console or the text code.
  - The panel is cleaned out of the data cache so the real GPU shows it.
  - A fault inside the crash code halts quietly.
  - The deliberate EL0 faults of M3 are not crashes. They are printed and the kernel carries on.
- **Interrupts and floating point.** `irq_entry` saves q0-q31, FPCR and FPSR next to the general registers.
- **Out of memory.** `kmalloc` returns 0 when the bump heap is full, and every caller checks.
- **A new failure cannot hide.** The boot health check fails on any `FAIL` or `oom` line the boot prints that is not on its short, commented list of things QEMU cannot do.

</details>

## Design rules

This is the one page of rules for how Joshua Tree looks and what it never does. The brand kit, every mark, colour, type size and icon on one page you can open in a browser, is `docs/brand/index.html`. Every number in a code span below is read back from the source by `tools/checks/design-doc-check.py`, so when a rule drifts the suite says so.

<details><summary>Details (141 lines)</summary>


### What the OS never does

It never shows a pixel. The desktop is laid out at `DESKTOP = 960x540` and drawn at `PIXEL_SCALE = 2` times that. Text, icons and the wallpaper are written on the real pixels and anti-aliased. Plain rectangles are 2x2 blocks, which is fine because they have no diagonal edges.

It never draws an emoji. UI text covers `UI_GLYPH_RANGE = 32-126`, plain ASCII, plus the degree sign. It never shows purple or teal in an icon or in the window chrome, and never puts a border, stripe or card edge on something just to decorate it. Super simple wins: one window frame, one dock, one light.

### Brand mark

8.0.0: the mark is one Joshua tree, painted in gouache and watercolour in the manner of a wildlife album cover: a shaggy bark trunk leaning left, three arms, a spiky rosette at each tip, and a wash of terracotta soil with a few stones. The source is `docs/brand/tree.png` (1024 px on nothing, cut from its paper once); it keeps its own colours on light and dark pages. Below 48 px the brushwork turns to mud, so `tools/gen/gen_brand_art.py` traces it flat into `docs/brand/glyph.png`, bark ink with terracotta soil in the same pose, for the favicon (`landing/favicon-32.png`) and anything 32 px or under. The same script writes every other copy: `landing/tree.png` (hero and footer seal), `landing/mark.png`, `landing/badge.png`, the share card, the README marks and the X avatar. The page colours come from the painting: paper `#f4f3ec`, ink `#261a13` (its darkest bark), and the terracotta accent, which is its soil. Its other colours (bark, sienna, ochre, sage, soil) are in `docs/brand/tokens.css` for brand art only. Nimbus is painted to match (`docs/hardware/nimbus-painted.png`, on the landing's Want one and footer), and the build section shows the Nimbus plans as a brown print, the CAD's own sheets recoloured.

The kernels draw the flat glyph in one ink, Apple style: the Pi boot splash, the i386 splash, where it grows up from its foot, and both menu bars. `tools/gen/logo.py` and the crayon SVGs remain only for the landing's boot placeholder mask.

The crayon mark it replaced (3.0 to 7.9) is one Joshua tree drawn in crayon: steady ink lines for a leaning trunk, three arms ending in dense looping tufts, and one terracotta stroke for the ground.

#### Mark history

Every minor version refreshes the mark above the landing footer, one step at a time. File: `landing/logo.svg`, the seal over the footer, inverted for a dark page with the hue turned back so the ground stays terracotta. Built with the `logo-refresh` skill and `tools/gen/logo.py`.

- 1.3: the engraved badge traced to clean vector lines, sky hatching and small text dropped, window buttons, power-symbol sun and prompt added.
- 1.4: new mark. The engraved Joshua tree on its own with a ground line at its base, one color, ink only. Poster: `docs/mark-1.4.png`.
- 2.0: the brand mark (favicon, boot splash, landing seal) gets the engraved look. Tapered spike tufts in two rings, a flared trunk and a ground line, still one ink color. Built by `tools/gen/logo.py`, boot splash rebuilt by `tools/gen/gen_boot_mark.py`. Preview: `mark-2.0-preview.png`.
- 2.12: the same tree, finished more carefully. The corners where the limbs meet the trunk and each other are rounded off, the arms run straight into their crowns with no knob, the blades alternate long and short and the two inner crowns lean apart so all four crowns stay separate at 32 px, the trunk stands on a short tapered ground line instead of a flat cut, and thin engraved grooves follow the trunk and each limb, lit from the upper left. Still one ink, the grooves are real holes so it works as a mask on light and dark. Built by `tools/gen/logo.py`, boot splash by `tools/gen/gen_boot_mark.py`, PNG copies by `tools/gen/gen_brand_art.py`. Preview: `docs/mark-2.12.png`.
- 2.25: a new mark, much simpler. A crayon scribble of one Joshua tree in the style of a hand-drawn album cover: thin, wobbly lines for the trunk, three arms and the ground, a dense looping scribble for each tuft with a few blades flicking out, and a light crayon grain. Black ink on off-white, no fill. A clean, heavier copy drawn down the middle of each limb (`landing/mark-bold.svg`) keeps it a tree at 16 px. Built by `tools/gen/logo.py`. Preview: `landing/badge.png`.
- 2.60 (the 3.0 mark): the same crayon tree, tightened. Steadier, heavier lines, a trunk that swells where the marker pressed, denser tufts with paper showing through, three or four short blades over each instead of a fan, a lighter grain, and the ground drawn in terracotta, the mark's first and only colour. The small copy leans with it and keeps the terracotta ground, which lands on one pixel row at 16 px. The boot placeholder's mask (`landing/mark-tree.svg`) stays one colour by nature; the hero and the footer seal keep the terracotta in both themes. Built by `tools/gen/logo.py`. Preview: `landing/badge.png`.
- 8.0: a painting replaces the crayon. One Joshua tree in gouache, like a wildlife album cover, with a flat traced glyph for small sizes. `docs/brand/tree.png` is the source, `tools/gen/gen_brand_art.py` makes every copy.

### Mascot

Retired in 8.0.0. The crayon friend (a chubby tree with dot eyes and pink cheeks) was a child's drawing, and the painted mark replaced that style everywhere. Empty screens show the painted tree instead.

### Icons

Every icon is one picture on one tile. The tile is a superellipse, `SQUIRCLE_N = 4.8`, over the whole 128 unit canvas, and an icon's edge must land within `TOL_SQUIRCLE = 1.0` unit of that curve at every angle, with a soft anti-aliased edge. A plain rounded rectangle fails.

The glyph is bold and sits in the middle. It keeps out of the top `GLYPH_TOP_MARGIN = 16` and bottom `GLYPH_BOTTOM_MARGIN = 20` units, because that band belongs to the tile's own light.

There is one light, from the top. A dock tile gets darker from top to bottom by `DEPTH_MIN = 6` to `DEPTH_MAX = 45` luminance steps, never a dark rim (`EDGE_MAX = 6`), and a white lip along the top edge, stroked `HL_WIDTH = 5.0` units wide, `HL_ALPHA = 0.5` strong, fading out over `HL_FADE = 22` units. Gradients exist only to carry that light, so they never change hue: none runs more than `GRADIENT_HUE_MAX = 15` degrees of hue from end to end.

No text, letters or wordmarks, so `ICON_TEXT_ELEMENTS = 0`. That includes Calendar: its tile is a page with a terracotta binding bar and a grid of day squares, with no date, because the Pi has no clock and a blank page with a dash read as half drawn. No emblem and no speech bubble drawn around a picture. No purple or teal in any icon's colours, so `ICON_PURPLE_TEAL = 0`.

The house accent is terracotta, `ACCENT = #b5502c`, the landing page's accent and the colour of the Music, Movies and Hamurapi tiles. The one deliberate exception is Patois, which keeps its own sky blue, `LEXLY_BLUE = #2E86DE`, because that is the colour of its store icon.

Fleet icons are the `FLEET_ICONS = 10` apps imported from their own repos (Samantha's became an authored symbol, her glasses, in 7.7.0). They keep their own tile colour and their own picture. They must still share the squircle, the anti-aliased edge, the top lip and the glyph margin. They do not get the tile falloff, so a fleet tile is flat. The importer clips each one to the squircle and lays the lip on it. The checks measure the squircle and the edge on all `ICON_FILES = 43` icons, but the glyph margin only on the `MARGIN_TILES = 29` that carry the shared tile header (`restyle_icons.py` writes it, and the importer writes Samantha's, whose portrait sits in an 88 unit disc so it keeps the margins). A fleet icon's margin is held by eye, and the roadmap lists the fleet icons that still fall short.

### Type

The registry holds `DEJAVU_FACES = 14` faces.
- Two are Tree Sans (Regular and Bold). It is our own face, built from Inter. The headers keep their `dejavu_` names.
- Four are DejaVu: Serif, Serif Bold, Mono and Mono Bold.
- Eight are open look-alikes for documents and user choice: Inter, EB Garamond, Comic Neue, Caveat Brush, Dancing Script, JetBrains Mono, Source Serif and Source Sans (`fonts` lists them).

Sans only in the UI chrome. The menu bar, dock and window titles are Tree Sans (3.0.0; DejaVu Sans before). Sans is the interface.

The sizes:
- The desktop draws it at `GUI_TEXT_PX = 24` physical pixels, 12 on the 960 wide grid.
- Apps draw it at `BODY_PX = 14.0`.
- Bold is at the same size.
- The big display digits are at `DISP_PX = 56.0`.

On the desktop, Mono is for a shell grid and nothing else: `MONO_APPS = panes, terminal`, at `MONO_PX = 13.5` with every glyph `JT_MONO_ADV = 8` pixels wide. Serif ships with the fonts and nothing draws with it, so `SERIF_USES = 0`. Sans-serif only is the house rule, so serif has to earn its way in.

The retina rule holds for type as it does for icons: stems are inked dense but stay anti-aliased. The 8x16 bitmap font is only the fallback for VGA text mode and for off-screen targets.

### Colour and light

The wallpaper is plain grey, `DEFAULT_WALLPAPER = WALL_GREY`, the Pi's default colour. Terracotta, Clay and Ink are the other plain walls; none of them fetch anything or get tinted. The satellite photo (live tiles with a network, the baked capture without), the map themes and the tree photo are choices in Settings, never the boot default.

Day and night are a blend on the wallpaper. They are read from the same clock as the menu bar, and they never go toward black or blue.
- From `NIGHT_HOURS = 21-5` the wallpaper sits `NIGHT_PCT = 65` percent of the way to the espresso brown `NIGHT_FLOOR = 0x00201009`. It eases in over the evening and out over the morning.
- In `DAY_HOURS = 8-18` it lifts `DAY_PCT = 10` percent toward silver, `DAY_TINT = 0x00DDDDDD`.

The chrome colours:
- The menu bar is `GUI_MENUBAR_H = 26` pixels tall, half wallpaper and half white (`MENUBAR_WHITE = 50` percent). It is closed by a one pixel rule, `MENUBAR_RULE = 0x00BDB8B0`.
- The dock tray is warm cream, `DOCK_TRAY_COLOR = 0x00EFEBE4`.
- A window body is `WINDOW_BODY = 0x00F5F0EB` with ink `WINDOW_INK = 0x001C1C1E`.

### Windows and dock

A window is one frame:
- a body with a `WINDOW_RADIUS = 18` pixel corner
- a one pixel rule `WINDOW_RULE = 0x00D9D3CB` under the title band
- the name centred in ink
- a red close dot, `CLOSE_DOT = 0x00FF5F57`, at the left

The few full-screen panels (Settings, Trash, the lock screen) use a plainer bar with the same dots. The edge and the corner are one anti-aliased curve, with no notch where they meet.

Drag a window by its title bar. It follows the pointer live. Let go and it may snap:
- within `SNAP_EDGE = 12` pixels of a side, it takes that half of the strip between the menu bar and the dock
- within `SNAP_CORNER = 40` pixels of a corner, it takes that quarter
- at the top edge, it takes the whole strip

On the Pi's desktop every window drags and resizes the same way.
- Press on the title band and the window follows the pointer, held whole between the menu bar and the dock.
- Press within `RESIZE_ZONE = 12` pixels of the left, right or bottom edge, below the title band. That edge follows the pointer while the opposite one stays. The bottom-right corner takes both.
- A window shrinks no further than one of these, or the size it opened at when that is smaller:
  - `MIN_WINDOW = 320x220` (Console, Terminal, Browser, Clock, Settings)
  - `MIN_CALC = 220x260` (Calculator)
  - `MIN_APP = 400x300` (every ring-3 app window: Calendar, Notes, Music, Books, Finder, Mail and the rest)
- Brick keeps its first size.
- A window grows no further than the sides of the screen and the strip above the dock.
- The frame is the same one throughout, with its 18 pixel corner and nothing added. The content lays itself out again for the new size.

The dock holds `DOCK_SLOTS = 12` tiles in this order: `DOCK_ORDER = Apps, Files, Mail, Calendar, Notes, Reminders, Terminal, Samantha, Weather, Stocks, Settings, Trash`. Settings (7.8.0) is a tile like Apps and Trash, not an app: `GUI_SETTINGS` 40 in `APPS[]`, opening the i386 Settings window and, on the Pi, `settings_open` (`DOCK_SETTINGS`, the same slot Spotlight and the menu bar use, so it is listed once). Its art is a gear on graphite (`restyle_icons.py`). Portfolio mode swaps in the fleet.

The dock's size and edges:
- A tile is `dock_scale_pct = 7` percent of the screen height, so `DOCK_ICON = 37` pixels on the desktop.
- There is `DOCK_GAP = 6` between tiles, `DOCK_PAD = 10` inside the tray and `DOCK_MARGIN_BOT = 24` below it.
- The tray has a `TRAY_RADIUS = 18` corner.
- It has a hairline `TRAY_EDGE = 0x00D6D0C6` top and bottom.
- It has a soft shadow that fades out over `TRAY_SHADOW_REACH = 14` pixels and follows the wallpaper.
- Hover shows the app's name in a pale capsule, `DOCK_LABEL_BG = 0x00F4F1EC` with a `DOCK_LABEL_EDGE = 0x00BDB4A8` hairline.
- Nothing magnifies or bounces.

The Launchpad is `APPS_COLS = 5` columns of icons at most `APPS_TILE = 56` pixels. Its glass panel is centred, at least `LAUNCHPAD_MIN_TOP = 24` pixels from the menu bar and the dock and `LAUNCHPAD_MIN_SIDE = 40` from the sides, the padding is even, and no two labels are closer than `LABEL_MIN_GAP = 6` pixels.

### Motion

The shell barely moves.

The one exception is the boot splash. The tree is drawn on, trunk first and the terracotta ground last, over `BOOT_DRAW = 0.8 s`, with the pen easing out. The progress bar fills after it.

Windows do not animate open, close or snap. The dock only shows its label.

What moves is Samantha's captions, on a clock of `TICK_HZ = 100` ticks a second.
- A new line rises into place over `CAP_IN = 0.28 s`, easing out as a square (`CAP_EASE = quadratic out`).
- Up to `CAP_SHOW = 3` lines show at once, each older one dimmer.
- When the talk has been quiet for `CAP_QUIET = 4 s`, the whole stack fades together over `CAP_OUT = 0.6 s`.
- Her caption backdrop is black at `CAP_ALPHA = 150` out of 256.

Apps move only when the movement is the app: Airwell's breathing circle, Samantha's mouth. The tree photo wallpaper sways in the wind; the satellite default stands still.

### How it is enforced

The numbers above are checked by `design-doc-check.py` against the file that defines each one. It also reads every icon in `art/icons` for text, purple, teal and gradient hue.

Run in the suite on every pull request: `iconinset-check.py` (the squircle and the edges on every icon, the glyph margin on the tiles with the shared header), `iconart-check.py` (the dock draws the colour each SVG states), `iconlight-check.py`, `iconhalo-check.py`, `iconedge-check.py` (its only hard assert is the Trash rim, the rest is a score to read), `calicon-check.py`, `clockicon-check.py`, `dockslots-check.py`, `dockband-check.py`, `dockhover-check.py`, `windowedge-check.py`, `windowsnap-check.py`, `windowdrag-check.py`, `arm64-resize-check.py` (every Pi window drags and resizes), `launchpad-centered-check.py`, `textsharp-check.py`, `textspacing-check.sh` and `termsharp-check.py`.

Run by hand, not in CI yet: `iconsync-check.sh` and `dockshadow-check.py`.

Not measured, held by eye: a bold glyph, one picture per icon, no emblem or speech bubble, no stripes or decorative borders, and the glyph margin of the icons without the shared tile header (the other fleet icons, Movies, Panes and Portfolio).

</details>

## Type system

Generated by `tools/gen/typetokens.py`; edit the rules there. The landing page, the OS and the docs all read this one table.

<!-- type:start -->
The face is **Tree Sans**, built by `tools/fonts/treesans/build.py` from Inter 4.1 (SIL OFL 1.1) and renamed as the licence asks.
Two optical sizes: **Text** (opsz 14, open and generously fitted) below 22 px, **Display** (opsz 32, tighter) from 22 px up.
Proportional by default, kerned, with tabular digits one feature away (`font-variant-numeric: tabular-nums`).

### Constraints

1. A **4 px baseline grid**. Line heights, gaps and padding are all multiples of 4.
2. **Nine sizes**, a 1.25 ratio rounded to whole pixels. No tenth size.
3. **Line height** is 1.3 times the size, rounded up to the grid.
4. **Tracking** tightens as type grows: +0.01em at 11, 0 at 13 to 15, -0.01em at 17 to 22, -0.02em above.
5. **Weights have jobs**: 400 reads, 500 labels, 600 leads, 700 headlines.
6. **Space** comes from one ladder: 4 8 12 16 24 32 48 64 96.

### Scale

| Role | Size px | Line px | Tracking | Cut |
|---|---|---|---|---|
| caption | 11 | 16 | 0.01em | Text |
| footnote | 13 | 20 | 0 | Text |
| callout | 15 | 20 | 0 | Text |
| body | 17 | 24 | -0.01em | Text |
| title3 | 22 | 32 | -0.01em | Display |
| title2 | 28 | 40 | -0.02em | Display |
| title1 | 36 | 48 | -0.02em | Display |
| large | 48 | 64 | -0.02em | Display |
| hero | 64 | 84 | -0.02em | Display |

### Font metrics

Every vertical metric is a multiple of 1/16 em: ascent 15, descent 4, line gap 0, a 19/16 em line box. Glyph spacing is the font's own, with
its kerning (GPOS `kern`) on. Alternates frozen into the default letters: the tailed `l`, the serifed capital `I` and round quotes and commas, so
`I`, `l` and `1` never read alike on a small screen.
<!-- type:end -->

#### Font sources

Every library face is SIL Open Font License 1.1. Its licence sits next to it in `tools/fonts/` as `<Name>-OFL.txt`. None of them is Helvetica, Garamond, Comic Sans or Papyrus. They are open look-alikes.

| Face | Stands in for | Subset bytes | Fetched from |
|---|---|---|---|
| Inter | Helvetica | 9,760 | https://raw.githubusercontent.com/google/fonts/main/ofl/inter/Inter%5Bopsz,wght%5D.ttf |
| EB Garamond | Garamond | 19,856 | https://raw.githubusercontent.com/google/fonts/main/ofl/ebgaramond/EBGaramond%5Bwght%5D.ttf |
| Comic Neue | Comic Sans | 11,644 | https://raw.githubusercontent.com/google/fonts/main/ofl/comicneue/ComicNeue-Regular.ttf |
| Caveat Brush | Papyrus (nearest open brush face; nothing open is close) | 29,480 | https://raw.githubusercontent.com/google/fonts/main/ofl/caveatbrush/CaveatBrush-Regular.ttf |
| Dancing Script | cursive | 16,424 | https://raw.githubusercontent.com/google/fonts/main/ofl/dancingscript/DancingScript%5Bwght%5D.ttf |
| JetBrains Mono | monospace | 8,748 | https://raw.githubusercontent.com/google/fonts/main/ofl/jetbrainsmono/JetBrainsMono%5Bwght%5D.ttf |
| Source Serif 4 | serif | 12,828 | https://raw.githubusercontent.com/google/fonts/main/ofl/sourceserif4/SourceSerif4%5Bopsz,wght%5D.ttf |
| Source Sans 3 | sans | 9,112 | https://raw.githubusercontent.com/google/fonts/main/ofl/sourcesans3/SourceSans3%5Bwght%5D.ttf |

Each licence came from the same folder's `OFL.txt`. Total added: 117,852 bytes.

Recipe: variable fonts are pinned at weight 400 with `fontTools.varLib.instancer`, then subset to printable ASCII (U+0020 to U+007E) with no hinting and no layout tables. Only the subset files are kept. `python3 tools/gen/gen_ttf_font.py` turns them into `drivers/lib_*_font.h`.

The faces are embedded in the kernel for now. Loading them from disk is a later step.

#### Building Tree Sans

Tree Sans is the Joshua Tree typeface. It is derived from Inter 4.1 by Rasmus Andersson and The Inter Project Authors (https://github.com/rsms/inter), licensed under the SIL Open Font License 1.1 (`OFL.txt`), and renamed as the OFL asks of a modified font. It is made the way Nimble Sans was.

Rebuild: `python3 tools/fonts/treesans/build.py InterVariable.ttf OUT_DIR` (the variable font is `Inter[opsz,wght].ttf` from google/fonts).

### What changed from Inter

- Two optical sizes, pinned from Inter's variable axes: **Text** (opsz 14) for Regular, Medium, SemiBold, Bold, and **Display** (opsz 32) for SemiBold and Bold. Text is used below 22 px, Display from 22 px up.
- Alternates frozen into the default letters: `cv05` the tailed lowercase l, `cv08` the serifed capital I, `ss03` round quotes and commas. `I`, `l` and `1` never read alike on a small screen.
- Vertical metrics on a 1/16 em grid: ascent 15, descent 4, line gap 0 (a 19/16 em line box), one underline for the family. The Windows clip box still covers every glyph.
- Subset to Basic Latin, Latin-1, general punctuation, arrows and the math signs the apps use. Kerning (`kern`), ligatures, `case`, `tnum`, `pnum`, `frac` and `zero` are kept. Tabular digits are one feature away, proportional by default.
- Names: family "Tree Sans" and "Tree Sans Display", PostScript `TreeSans-*` and `TreeSansDisplay-*`; Inter's copyright and the OFL text stay.
- Glyph shapes and kerning are otherwise Inter's.

The type system around it (sizes, line heights, tracking, spacing) is the Type system section, generated by `tools/gen/typetokens.py`.

## System call ABI

This is the contract a ring-3 program written against this kernel can rely on.

<details><summary>Details (470 lines)</summary>


Three versions are shipped. **v1 is frozen**: every number, argument meaning and error below in the v1 section works exactly as written and will not change without a MAJOR bump.

### v1, frozen

The whole v1 set is now implemented, and `user/hello.c` is the real external caller. It is compiled separately, with no kernel include path at all, against `user/jtsys.h` and nothing else.

#### Calling convention

Borrowed from x86 Linux rather than invented, so anything written against that convention lines up here without translation:

- `int $0x80` is the gate. IDT vector 0x80, DPL 3, 32-bit interrupt gate, so interrupts are off for the duration of the call.
- `eax` holds the syscall number on entry and the result on return.
- `ebx`, `ecx`, `edx` hold arguments 1 through 3. There is no fourth argument in v1, and adding one later does not break this contract.
- A negative return value is `-errno`. Errno values are Linux's own.
- Every unassigned number returns `-ENOSYS` (38), including every number at or above the dispatch table's size. Dispatch never jumps into an empty table slot.
- The call runs on the calling task's own kernel stack, repointed by the TSS `esp0` that `task.c`'s scheduler updates on every switch.

Any pointer a program hands the kernel is checked against the page tables before the kernel reads or writes it, the job Linux's `access_ok` does.

#### The v1 call set

Numbers are Linux i386's own, so `__NR_exit` is 1 and `__NR_write` is 4. The gaps are deliberate, not slots waiting to be filled in order.

| # | Call | Arguments | Returns | Status |
|---|------|-----------|---------|--------|
| 1 | `exit` | ebx = exit code | never returns | shipped |
| 3 | `read` | ebx = fd, ecx = buf, edx = len | bytes read, 0 at end of file | shipped |
| 4 | `write` | ebx = fd, ecx = buf, edx = len | bytes written | shipped |
| 5 | `open` | ebx = path, ecx = flags | fd, or -errno | shipped |
| 6 | `close` | ebx = fd | 0, or -errno | shipped |
| 13 | `time` | ebx = 0, or an `unsigned *` | seconds since the epoch | shipped |
| 20 | `getpid` | none | this task's id | shipped |
| 158 | `sched_yield` | none | 0 | shipped |

File descriptors 0, 1 and 2 are standard input, output and error. There is one console, so 1 and 2 both reach it. `read` on fd 0 reads the keyboard. Descriptors from `open` start at 3 and are per task.

#### The limits, exactly

Every number here is part of the contract. They are the values the implementation actually uses, not round numbers chosen for the document.

- `write` copies at most **255** bytes per call, so the kernel-side copy has a fixed on-stack buffer.
- `read` copies at most **255** bytes per call, the same bound for the same reason.
- `open` accepts a path of at most **63** bytes plus the terminating NUL. A longer one, or one with no NUL in range, is `-EINVAL` (22).
- `open` reads the whole file eagerly into a kernel buffer and `read` serves out of that snapshot, because `vfs_read_file()` is the only read primitive the VFS layer has.
- Each task has **8** descriptor slots. Three are the standard streams, so **5 files can be open at once** per task, numbered 3 through 7. A sixth is `-EMFILE` (24).
- `open`'s flags argument must be **0**. v1 is read-only; any other value is `-EINVAL`. There is no create, no write, no append.
- Descriptors are per task slot and are closed for you when the task exits, including when it exits by faulting.
- `read` on fd 0 is **non-blocking**, and this is forced, not chosen: `int $0x80` is an interrupt gate, so interrupts are off inside the call and the keyboard IRQ that would deliver the awaited byte cannot fire.
- `read` on fd 1 or 2 is `-EBADF` (9). The console is write-only.
- `time` returns seconds since 1970-01-01T00:00:00Z, read from the CMOS RTC.
- `getpid` returns the task's scheduler slot id. The shell is 0, so a ring-3 program started from it is never 0. There are **6** slots in total, so ids are 0 through 5.
- The dispatch table has **160** entries (the highest v1 number, 158, rounded up to the next multiple of 32).

#### How a program is built and loaded

Also part of the contract, because a flat binary has no headers and no relocations, so its load address is not negotiable:

- A user program is a flat binary. Entry is offset 0, not an ELF entry point; `exec_user()` jumps straight at the load address.
- It is linked at **0xC059F000** and gets **33 pages**: thirty-two for the image (128KB, `JT_USER_IMAGE_PAGES` in kernel/memmap.h; a larger one fails to link), one for its.
- It gets no `.bss`: with a flat image there is nothing to zero it and no crt0 to do the zeroing, so `user/hello.ld` refuses to link one.
- One program runs at a time. There is one image window, and the shell waits for the program it started.
- It runs with IOPL 0 in its own page directory, so any `in`/`out` it tries is a `#GP` and it cannot see another task's private pages.

#### What this contract does NOT cover

v1 had no memory-mapping call (a program got the eight pages it was loaded with), no `fork` or `exec`, no signals, IPC or sockets, no directory enumeration, seek or stat, no writing to files (`open` was read-only), no command-line.

### v2, shipped

v1 shipped an interface a program could look through and not change anything with: `open` demanded `flags == 0` and `write` only reached the console. v2 is the other half.

#### The v2 call set

| # | Call | Arguments | Returns | Status |
|---|------|-----------|---------|--------|
| 4 | `write` | ebx = fd **3..7**, ecx = buf, edx = len | bytes written | shipped |
| 5 | `open` | ebx = path, ecx = **flags** | fd, or -errno | shipped |
| 19 | `lseek` | ebx = fd, ecx = offset, edx = whence | the new position, or -errno | shipped |

`write` and `open` are the same numbers with the same arguments as v1. What changed is only that values v1 rejected are now accepted: an fd from `open`, and a non-zero flag word. Every v1 call still returns what v1 said it returns.

#### open flags

Linux i386's own values, the same borrow the call numbers are. The access mode is the low two bits; the rest are independent.

| Flag | Value | Meaning |
|---|---|---|
| `O_RDONLY` | 0 | read only. This is v1's `flags == 0`, unchanged |
| `O_WRONLY` | 0x001 | write only; `read` on this descriptor is `-EBADF` |
| `O_RDWR` | 0x002 | both |
| `O_CREAT` | 0x040 | create the file if it does not exist |
| `O_TRUNC` | 0x200 | start from an empty file |
| `O_APPEND` | 0x400 | every `write` goes to the end, whatever the offset is |

- Access mode `3` is not a mode and is `-EINVAL`.
- Any bit outside the table is `-EINVAL`, not ignored. A program is never told a request succeeded when part of it was dropped.
- `O_CREAT`, `O_TRUNC` and `O_APPEND` require `O_WRONLY` or `O_RDWR`.
- `O_CREAT` and `O_TRUNC` take effect **at open**: the file exists, and is empty, the moment `open` returns.
- `O_CREAT` on a backend with no room left is `-ENOSPC` (28).

#### How writing actually works, and what that costs

This is the part to read before writing a program against it, because the model is not POSIX's and the difference is visible.

The VFS layer underneath has exactly two write primitives, `vfs_write_file()` and `vfs_replace_file()`, and **both take a whole file at once**. There is no partial write, no truncate-to-length, no per-backend cursor.

The consequences, all real and all contractual:

- **The file on the filesystem does not change until `close`.** A program that writes and then exits without closing is still fine: task exit closes every descriptor, and.
- **Two descriptors open on the same file do not see each other**, and the last one closed wins the whole file. There is no locking.
- **`close` can fail.** It returns `-EIO` (5) if the bytes did not reach the backend intact, and that is a real check, not a formality: `close` reads the file straight back and compares it to what it just wrote.
- **The descriptor is released even when `close` returns `-EIO`.** A failed close has still closed.

#### The limits, exactly

Again, the values the implementation uses, not round numbers.

- `write` to a file copies at most **255** bytes per call, the same bound as v1's console `write` and for the same reason.
- `write` to a file is **binary clean**: unlike `write` to fd 1 or 2, it does not stop at a NUL byte.
- A file cannot grow past **8192** bytes, `OPEN_MAX_FILE`, the same ceiling v1 put on reading.
- `write` on a descriptor opened `O_RDONLY` is `-EBADF` (9). `read` on one opened `O_WRONLY` is `-EBADF`.
- `write` to fd 0 is `-EBADF`, as in v1.
- `lseek` whence is `SEEK_SET` (0), `SEEK_CUR` (1), `SEEK_END` (2). Anything else is `-EINVAL`.
- `lseek` to exactly the file's current size is legal: that is end of file, where an append lands.
- `lseek` on fd 0, 1 or 2 is `-ESPIPE` (29). Those are streams, not files.
- `lseek` offsets are signed 32-bit, which is not a limit worth worrying about when a file caps at 8192 bytes.
- A **zero-length file and a missing file are the same thing** through this interface.

#### Command-line arguments

A program is entered with the i386 cdecl frame a plain C function already expects, so this is a valid entry point with no assembly anywhere:

```c
void _start(int argc, char **argv) { ... jt_exit(0); }
```

Exactly, at the moment the CPU lands on the entry point:

```
esp+0   a return address, always 0
esp+4   argc
esp+8   argv  ->  [ argv[0], ..., argv[argc-1], NULL ]
...     the argv pointer array
...     the argument strings, NUL-terminated
top     0xC05B8000, the top of the program's stack page
```

Why this layout and not Linux's: real Linux puts `argc` at `0(%esp)` with no return address above it, which is exactly why a real i386 crt0 has to be written in assembly.

The rest of the contract around it:

- `argv[0]` is the program's own name by convention. The shell's `exec` puts the filename there. Nothing in the kernel invents one.
- `argv[argc]` is `NULL`, so argv can be walked without argc.
- The strings and the pointer array live at the top of the program's own stack page, which the loader zeroes and marks user-accessible before the program starts.
- At most **8** arguments including `argv[0]`, and at most **256** bytes of argument text including the NULs.
- A program that wants nothing from its arguments can still declare `void _start(void)`. `user/hello.c` does, which is why it did not have to change for v2.
- **`_start` must never return.** There is no caller above it and the return address is 0, so returning faults at an unmapped address.

#### One thing v1 said that v2 makes less than literally true

Flagged rather than buried, because v1 is frozen and this is the one place the frozen text and the running kernel no longer read the same.

v1's "How a program is built and loaded" says the stack page's top, 0xC05B8000, is the initial `esp`.

Why this is not treated as a MAJOR break:

- The stack page itself, its address, its top and its size are all unchanged, and it is still the only stack a program gets.
- No v1 program can observe the difference through the v1 interface.
- `user/hello.c` is unmodified and `tools/checks/usertest-check.sh` passes unmodified, which is the practical version of the same claim.

If that is not good enough for some future caller, the fix is a MAJOR bump, not a quiet edit to the v1 section above.

#### The v2 reference program

`user/note.c`, built like `user/hello.c` against `user/jtsys.h` and no kernel header: `note FILE` prints it, `note FILE text...` appends a line (creating the file), `note FILE @N text...` overwrites the bytes at offset N, which is what.

#### What v2 still does NOT cover

Still missing at v2: more than one user program alive at once (one image window and one user stack page, and the shell blocks on the program it started), `fork`, `wait`, job control, `dup`, redirection and pipes (fd 1 is the console).

#### What a MAJOR version means, from here on

Changing the meaning of a number in the table above, removing a call, changing an argument's meaning, or tightening one of the limits in "The limits, exactly" is a breaking change and requires a MAJOR bump.

Before this, the file described an interface nothing depended on.

#### Writing a program with libjt

`user/hello.c` and `user/note.c` are written straight against `jtsys.h`'s `int $0x80` wrappers, on purpose: they are the interface's own self-test and should look exactly like the ABI they exercise.

What it gets: `string.h` (`strlen`, `strcmp`, `strcpy`, `memcpy`, and the rest of the usual set), `ctype.h`, `stdlib.h` (`atoi`, `strtol`, `abs`, `exit`, and `malloc`/`calloc`/`realloc`/`free` over a fixed 16KB static arena, since there is.

To build one: write a `_start` the way `user/note.c` does (`void _start(int argc, char **argv)`, or `void _start(void)` if you don't need argv), `#include` whichever libjt headers you need, and link `user/libjt.a` in after your own object.

### v3, shipped (1.7.7)

Two calls, so a ring-3 program can be an app on the desktop rather than a line of text in the shell. They are Joshua Tree's own numbers, **384 and 385**, starting past anything Linux i386 assigns so the two sets can never collide.

#### The v3 call set

| # | Name | ebx | ecx | Returns |
|---|---|---|---|---|
| 384 | `window_open` | `struct jt_window_info *` | 0 | 0, or -errno |
| 385 | `window_poll` | `struct jt_event *` | flags | 1 with an event written, -EAGAIN with none, or -errno. |

```c
struct jt_window_info { unsigned int width, height, pitch; unsigned int *pixels; };
struct jt_event { unsigned int kind; int a, b; };
```

**window_open** hands the program the app window the desktop already opened for it.

**window_poll** is how pixels reach the screen and how input reaches the program, and it never blocks: `int 0x80` runs with interrupts off, so the kernel cannot sleep a program inside it.

| kind | meaning | a | b |
|---|---|---|---|
| 1 `JT_EV_KEY` | a key | ASCII, or 256 and up for up/down/enter/esc/left/right, the same values `kernel/app.h` gives in-kernel apps | 0 |
| 2 `JT_EV_CLICK` | a left click | x in window coordinates | y |
| 3 `JT_EV_WHEEL` | a wheel tick | +1 up, -1 down | 0 |
| 4 `JT_EV_RESIZE` | the window rect changed (compositor windows only) | new width | new height |
| 5 `JT_EV_DRAG` | the pointer moved with the left button held inside the window's content (2.59.1): the press's own spot first, then one per new pixel, dropped if the ring is half full; the release still sends its `JT_EV_CLICK` | x in window coordinates | y |

There is no `window_close`.

#### What v3 guarantees, and what it does not

The point of v3 is isolation, and this is the part `tools/checks/ ring3app-check.py` proves rather than states: a program with a window is still an ordinary ring-3 task.

Limits, exactly: one program window at a time (`-EBUSY` otherwise), the same one-program-at-a-time rule as `exec_user`. The framebuffer is `0x110000` bytes, enough for the dock's 804x345 viewport with a little room.

Release is complete, as of 1.7.8. When the owning task ends, by `exit` or by a fault, the buffer is zeroed and its pages are flipped back to supervisor-only (`paging_clear_user`), on every teardown path.

#### The v3 reference program

`user/keyrate.c`, the typing test, launched from the dock by `kernel/ring3app.c` (which since 1.7.11 also launches `user/toroid.c` from one table) (which is kept for now).

#### Windows as compositor windows (1.9.23)

The calls did not change. What changed is what they mean for a program the desktop launched on the window path (`kernel/ring3app.c` `ring3app_launch_window`):

- `window_open` fills the same `jt_window_info`; `pixels` is still `JT_USER_FB`, but the frames behind it belong to this task alone, mapped into no other directory.
- `window_poll` with `JT_POLL_PRESENT` no longer copies the buffer to the screen inside the call.
- Events come from a per-window queue of 16.
- **Resize (1.10).** When the window rect changes (snap, resize), the desktop queues `JT_EV_RESIZE` (a = new content width, b = new height) once per change.
- A program on the blocking path (every app not named in `gui_ring3_windowed`) sees the old behaviour exactly.

#### tasks (1.9.6)

| # | Name | ebx | ecx | Returns |
|---|---|---|---|---|
| 386 | `tasks` | `struct jt_tasks *` | slot to kill, or -1 | 0, or -errno |

```c
struct jt_tasks { unsigned int ticks, free_kb, total_kb, current, used; };
```

**tasks** is what the Activity app shows: uptime in 100Hz ticks, free and total memory in KB, the caller's own slot, and a bitmask of the live scheduler slots.

#### http_get (1.9.11)

| # | Name | ebx | ecx | edx | Returns |
|---|---|---|---|---|---|
| 387 | `http_get` | `const char *path` | `void *buf` | `len` (at most 65536, `JT_HTTP_BIG_MAX`) | body bytes, -status, or -errno. |

**http_get** is what the Scrounge app fetches its live rows with, and the first call that puts the kernel's network stack behind a ring-3 program.

On an HTTP 200 the body is copied out, at most `len` bytes, and the count is returned.
#### readfile (2.2)

| # | Name | ebx | ecx | edx | Returns |
|---|---|---|---|---|---|
| 401 | `readfile` | `const char *path` | `void *buf` | capacity in bytes (1 to 6MB) | bytes read, or -errno |

**readfile** loads a whole file into one caller buffer. `open` stops at 8KB and `read` moves 255 bytes a call, which is no use for a song.

#### readdir (1.9.13)

| # | Name | ebx | ecx | edx | Returns |
|---|---|---|---|---|---|
| 388 | `readdir` | `const char *path` | `struct jt_dirent *` | records the array holds | entries in the directory, or -errno |

```c
struct jt_dirent { char name[32]; unsigned int size, is_dir; };
```

**readdir** is what the Search app lists (and what Files will). `path` is relative to the shell's current directory: `""` or `"."` is that directory, `"DOCS"` a folder inside it, `"DOCS/SUB"` two levels down.

Since 1.9.13 **open** takes the same relative paths, so the file a listing named inside `DOCS` opens as `"DOCS/NAME"`. The walk in and back out happens at open and again at close, when the buffer is written back. An empty path is -EINVAL.

#### mkdir and unlink (1.9.24)

| # | Name | ebx | ecx | edx | Returns |
|---|---|---|---|---|---|
| 389 | `mkdir` | `const char *path` | 0 | 0 | 0, or -errno |
| 390 | `unlink` | `const char *path` | 0 | 0 | 0, or -errno |

**mkdir** makes a folder and **unlink** deletes a file, both for the Notes app.

#### shell_run (1.9.24)

| # | Name | ebx | ecx | edx | Returns |
|---|---|---|---|---|---|
| 391 | `shell_run` | `const char *line` (`"<cwd>\n<command>"`) | `char *out` | `unsigned outlen` | bytes written to `out`, or -errno |

**shell_run** is the one call behind the window Terminal (`user/terminal.c`). It runs a single shell line and writes the text the shell would have printed into `out`, NUL terminated and cut to `outlen - 1`.
4096. Errors: -EFAULT (either pointer not user memory), -EINVAL (`outlen` out of range, line too long). A refused command is not an error: it returns a one-line message.

It does not call the text shell's `run()`. The call executes inside the int 0x80 gate on the 4KB kernel stack with interrupts off, and `run()` has 4KB buffers in its frame and commands that sleep, wait on the network, open windows or halt.

| Command | Does |
|---|---|
| `help` | lists the allowlist |
| `echo <text>` | prints the text |
| `uptime` | seconds since boot |
| `mem` | free and total memory in K |
| `ps` | each task slot, used or free |
| `ls [dir]` | the cwd (or `dir` under it), name and size |
| `cat <file>` | a file under the cwd, up to 2047 bytes, non-printable bytes shown as `.` |

Everything else is refused with `<name>: not available in the window terminal (allowed: ...)`.

The working directory belongs to the caller. `line` is `<cwd>\n<command>`: the Terminal keeps its own cwd as a relative path from the root (empty is the root, at most 63 bytes, up to `JT_PATH_DEPTH` components) and sends it with every call.

Since 1.9.24 the ring-3 key paths also deliver Home, End, Delete (0xE0 0x47, 0x4F, 0x53) as `JT_KEY_HOME` 305, `JT_KEY_END` 306, `JT_KEY_DELETE` 307, and Ctrl+S as `JT_KEY_SAVE` 308, both to a blocking app's `gui_poll_event` and to a.

#### http_post (1.9.26)

| # | Name | ebx | ecx | edx | Returns |
|---|---|---|---|---|---|
| 392 | `http_post` | `struct jt_http_post *` | flags (0, or `JT_POST_WORKER` 1, `JT_POST_BIG` 2, `JT_POST_CLAUDE` 4) | 0 | reply body bytes, -status, or -errno |

```c
struct jt_http_post {
    const char *path;                 /* same rules as http_get */
    const char *body; unsigned int body_len;   /* at most 6144 bytes */
    char *out;        unsigned int out_len;    /* clamped to 8192 */
    unsigned int reply_ticks;         /* 0 = 1500 (15s), clamped to 4500 (45s) */
};
```

**http_post** is the first call of the Samantha port (the Architecture section, "Samantha at ring 3"): one `POST <path> HTTP/1.0` of `application/json` to the chat host the Settings app keeps (`llm_host:llm_port`, turing.jaybulb.com port 80 by default).

On HTTP 200 the reply body is copied out, at most `out_len` bytes, and the count is returned.

`ecx` is a flags word; 0 is the behaviour above, so old callers are untouched.

One path is special. A `JT_POST_WORKER` post to exactly `/api/mail/send` (the Mail app's Send) gets an `Authorization: Bearer <token>` header added by the kernel, from the Mail token in Settings (Assistant, `mailtoken=` in `SETTINGS.TXT`).

`JT_POST_CLAUDE` (4, 2.14.0) is the Claude app's selector.

#### audio (1.9.26)

| # | Name | ebx | ecx | edx | Returns |
|---|---|---|---|---|---|
| 393 | `audio` | op: 1 play, 2 status, 3 stop | `struct jt_audio_play *` / `struct jt_audio_status *` / 0 | `sizeof` that struct | play: bytes taken; status: bytes written; stop: 0; or -errno |

One number, three ops; recording has its own number, 394, below.

```c
struct jt_audio_play { const void *pcm; unsigned len; unsigned rate; unsigned flags; }; /* flags: JT_AUDIO_END = 1 */
struct jt_audio_status { unsigned version, size, playing, queued, space, rate, played; };
```

`play` copies at most 8192 bytes per call into a 32KB kernel ring and returns how many it took. 0 means the ring is full (or the card is busy with the kernel's own chat playback or a recording), so retry on a later frame; the call never waits.

`status` fills the struct (`version` first, `size` is what the kernel wrote) and returns the byte count.

##### audio_record (394)

| # | Name | ebx | ecx | edx | Returns |
|---|---|---|---|---|---|
| 394 | `audio_record` | op: 1 start, 2 read, 3 stop | start: rate in Hz (a value) / read: user buffer / 0 | 0 / max bytes / 0 | start: 0; read: bytes copied; stop: 0; or -errno |

Push-to-talk capture on the same card and in the same format as `audio` (8-bit unsigned mono, 4000 to 44100 Hz, 16000 for Samantha).

Capture and playback are exclusive in both directions.

#### sysinfo and launch_request (1.9.26)

| # | Name | ebx | ecx | edx | Returns |
|---|---|---|---|---|---|
| 395 | `sysinfo` | `struct jt_sysinfo *` | caller's `sizeof` | 0 | bytes written, or -errno |
| 396 | `launch_request` | `const char *name` | the Pi (7.7.0): `const char *arg`, or 0 | 0 | 0, or -errno |

```c
struct jt_sysinfo {
    unsigned int version;      /* 1; always first so the struct can grow */
    unsigned int size;         /* sizeof in the kernel that filled it */
    unsigned int phone;        /* 1 in phone mode */
    unsigned int epoch;        /* seconds since 1970, same clock as time */
    unsigned int wx_have;      /* 1 when a good weather reading exists */
    unsigned int wx_state;     /* 0 none, 1 ok, 2 offline, 3 timeout, 4 failed, 5 bad */
    int wx_temp_c, wx_code10;  /* whole degrees C, WMO code times 10 */
    unsigned int llm_port;
    char wx_text[24];          /* the menu bar text, empty when none */
    char llm_host[40];         /* the chat host http_post talks to */
};
```

**sysinfo** hands a program the read-only state Samantha reports.

**launch_request** is `open <app>`.

#### clipboard (1.9.28)

| # | Name | ebx | ecx | edx | Returns |
|---|---|---|---|---|---|
| 399 | `clipboard` | op: 1 SET, 2 GET | user buffer | length | bytes stored or copied, or -errno |

One system clipboard, one 4 KB kernel buffer, shared by every ring-3 app. It came back when the apps left the kernel: Notes, Terminal, Mail and Samantha each used to keep their own copy.

#### brk (1.9.27)

| # | Name | ebx | ecx | edx | Returns |
|---|---|---|---|---|---|
| 397 | `brk` | new top, or 0 | 0 | 0 | the heap top after the call, or -errno |
| 398 | `refresh` | kind (0 weather, 1 stocks) | stocks: range \| sel << 8 | 0 | 0, -EINVAL (bad kind, range, selection) or -EBUSY (one request already pending). |

A program's image window (`JT_USER_IMAGE_PAGES`, 128 KB) is fixed and kmalloc'd per running window, so it is the wrong place for anything big.

#### text (400)

| # | Name | ebx | ecx | edx | Returns |
|---|---|---|---|---|---|
| 400 | `text` | `struct jt_text *` (x, y, fg, s) | op: 0 draw, 1 measure, 2 clear | unused | the string's advance in window pixels, or -errno |

**text** gives a ring-3 window the same anti-aliased face every kernel string uses. The window's pixel buffer is logical resolution and is doubled on the way to the screen, so glyphs a program stamps in itself are blocky.

#### text2 (402), ARM64 only, and the hi-res window flag (3.9.0)

| # | Name | ebx | ecx | edx | Returns |
|---|---|---|---|---|---|
| 402 | `text2` | `struct jt_text2 *` (LP64: x, y, rgb, face, pct, len, s, adv) | unused | unused | the pen's device x after the run, or -errno |

`window_open` (384) takes a flag in ecx (it ignored it before): 1 asks for a device-size buffer.

Threads (8.1.0) and processes (8.4.0), the Pi only; i386 answers -ENOSYS. `user/libjt/thread.h` wraps 407 to 409, with the locks; `jt_spawn`, `jt_wait` and `jt_connect` in `user/jtsys.h` wrap 410 to 412.

| # | Name | x0 | x1 | x2 | Returns |
|---|---|---|---|---|---|
| 407 | `thread_spawn` | entry, inside the app's code | stack top, 16-byte aligned, in the app's own writable memory | arg, the thread's x0 | the thread's number (1 to 3), -EFAULT for a bad entry or stack, -EAGAIN with all four taken |
| 408 | `thread_exit` | status | 0 | 0 | never; the last thread to exit is the app's exit with that status |
| 409 | `futex` | 0 wait, 1 wake | `int *` word, 4-byte aligned, writable | wait: the value it must still hold; wake: how many | wait: 0 once woken, -EAGAIN when the word had already changed; wake: how many woke |
| 410 | `spawn` | `const char *name` | `const char *words`, or 0 | 0 | the child's pid, or -ENOENT, -EAGAIN, -ENOEXEC, -EFAULT |
| 411 | `wait` | pid | 0 | 0 | the child's exit status (-1 for a fault or a kill), or -ECHILD |
| 412 | `connect` | `const char *host`, a name or a dotted address | port | 0 | fd 100, or -EBUSY, -EHOSTUNREACH, -ECONNREFUSED, -EFAULT, -EINVAL |
| 413 | `notify` (13.7.0, the Pi only) | `const char *text`, up to 63 bytes | 0 | 0 | 0, or -EFAULT, -EINVAL |

An app has up to four threads, each with its own registers, pc, stack and FP block (`struct thread` in `arch/arm64/kmain.h`).

Four cores (8.2.0).

App threads on four cores (8.11.0). Core 0 still owns the kernel; cores 1 to 3 run only app code at EL0.

Pipes (8.3.0). `userrun A [WORDS] | userrun B [WORDS]` in the Terminal (`cmd.c`) calls `userrun_pipe`, which starts B with `pin` set, then A with `pout` (one at a time, since a card app is read into one shared buffer).

Spawn and wait (8.4.0, the Pi only; i386 answers -ENOSYS).

Sockets (8.5.0, the Pi only).

Demand paging for the heap (8.7.0). `JT_SYS_BRK` only moves the heap's top.

Swap (8.12.0).

The kit's screens (8.13.0). `fb_pick_size` (main.c) asks the firmware for the panel's size and takes it as the buffer when it is at least 720x480; narrower and the dock does not fit, so anything smaller or odd keeps 800x600.

Touch screens (9.0.0). The configuration walk in `xhci.c` takes a HID interface that is neither a boot keyboard nor a boot mouse as a maybe (kind 4) and keeps its report descriptor's length from the HID descriptor.

The block cache (8.8.0). `blockdev_read_sector` and `blockdev_write_sector` in `emmc.c`, fat.c's only way to the card, keep 128 sectors (64 KiB), the least recently used one replaced.

</details>

## Threat model

Written before the auth code. A login prompt on top of no real privilege boundary would protect nothing. So this page is honest about what "protects" means here.

Condensed 2026-10-12. The full prose is in git history.

### What an account protects against, and what it does not

A Joshua Tree account is a desktop lock for the person physically at the keyboard.

It stops:
- a casual second person sitting down while the first is away
- an accidental open of the desktop (a stray click, a demo left running) changing another account's files

It also gives the kernel a real password hash:
- PBKDF2-HMAC-SHA256 over BearSSL
- 100,000 iterations
- a 16-byte salt
- the iteration count stored per record

This is the 1.7.9 replacement for the older home-grown chained SHA-256. The old hash is still verified, so old accounts log in and are then rewritten as PBKDF2.

It does not protect against anything past the keyboard, and must not be oversold.
- There is no network-facing multi-tenant use and no remote caller for the login to guard.
- There is no disk encryption.
- Apps now run at ring 3 in their own address spaces. But code already running in the kernel can read a session's memory or the raw disk.
- No claim anywhere in the UI or comments may say this "secures your data" or "encrypts your files".

The engineering bar that follows:
- Do the hashing for real: real salt, real iteration, constant-time comparison. A weak or timing-leaky hash is a cheap avoidable mistake.
- Imply no stronger boundary than exists.

The gate is opt-in. The login screen engages only once at least one account exists in `USERS.TXT`.
- An unconfigured system boots straight to the desktop, deliberately.
- A gate with no account behind it would be a lock on a door with no one behind it.
- The live v86 demo on `landing/index.html` boots this exact `kernel.elf` for anonymous visitors, who cannot create an account first.
- Creating the first account is a deliberate in-desktop action (Settings' "Add user" row).

### The Claude relay (2.14.0)

The Claude app is the first thing here with a remote caller. The relay (`tools/claude-relay/relay.py`) runs Claude Code on the host for anyone who presents its token.

- The token is a shared secret kept in plain text in `SETTINGS.TXT`, like the Mail token.
- The kernel adds it to the request. It is never handed to ring 3.
- The relay listens on loopback by default. A LAN relay now requires TLS (see the Encrypted Pi relay section of GUIDE.md).
- Claude's tools are read-only and scoped to the relay's working directory. So the worst case for a stolen token is reading that directory and spending the owner's plan.

The Claude app section of GUIDE.md has the full list.

### The 10.0 audit (2026-11-02)

10.0 is gated on an audit against this model: what is new since the 2026-10-08 network review, read against what an account, the card and the network may and may not be trusted with. Every surface below takes input from outside the kernel.

- **Self-update (9.3.0, `update.c`).** The image and its signature come over HTTPS from GitHub (host name and chain checked as for any page), and nothing touches the card until `appsig_ok` passes.
- **Languages (9.2.0).** `utf8_next` stops at the first byte that is not a continuation, so it never reads past a string's end.
- **USB touch (9.0.0, `hid_layout`).** A report descriptor is the device's to write.
- **The screen reader (9.1.0).** It speaks the dock's names through the relay's `/api/tts`, with the same token and escaping as Samantha's own answers.
- **Swap (8.12.0).** App memory that is paged out sits in plain text in `JT/SWAP.BIN` until the next boot recreates the file as zeros.
- **Pipes, sockets, spawn, threads (8.1.0 to 8.6.0).** These were reviewed when built.

Out of scope, as before: someone who can rewrite the card can replace `KERNEL8.IMG`, `TRYBOOT.TXT` or `config.txt` themselves, and no signature can stop that without a secure boot chain the Pi 4 does not have.

## Security on the Pi

Apps from the card (6.51.0 to 6.54.0): an app image and its signature are files anyone with the card can change, so both are fuzzed (`uimg.h`, `appsig.c`), the signature is checked on the exact bytes that are loaded, unsigned apps need an.

<details><summary>Details (107 lines)</summary>


This is a read-only review of what the ARM64 build exposes to the network. It was reviewed 2026-10-08 against the branch head.

What it covered:
- the text browser (`arch/arm64/browser.c`)
- HTTPS (`arch/arm64/tls.c`, the roots in `arch/arm64/certs/`, `tools/gen/tls-ta.py`, `tools/gen/tls-roots.py`)
- the DNS path (`drivers/net.c`, `drivers/http.c`)
- the relay client (`arch/arm64/ask.c`, `claude_cfg.sh`)
- the relay (`tools/claude-relay/relay.py`)

Condensed 2026-10-12. The line numbers and per-finding write-ups are in git history. The status below says which findings have since moved.

### What holds up

- Every HTTPS server certificate is checked against the embedded roots and the requested host name (`br_ssl_client_reset(&sc, host, 0)`).
- Response size is bounded.
  - The raw reply stops at `RAW_MAX` (4 MiB on the heap since 6.12.0, was 64 KiB), a gzip body at `PAGE_MAX` (8 MiB inflated), text at `TEXT_MAX`, links at `LINK_MAX`, and the title at 60 bytes.
  - 6.28.0: form controls.
  - 6.27.0: searchable history.
  - 6.31.0: each field keeps `at` and `len`, the span of the page text that shows it; `field_show` splices in its new state and moves the fields after it, and Blackcomb rewraps (`br_wk_w = -1`).
  - 6.30.0: an Expires date behind the clock (`http_date_past` in clock.c) deletes the cookie like Max-Age=0; Max-Age wins when a line has both.
  - 6.26.0: the cookie jar is the signed-in person's (`jar_switch` before every fetch); a cookie with Max-Age or Expires is `keep` and `jar_save` seals the kept ones in the.
  - 6.40.0: field kind 6, the page's first `<textarea>`, keeps its text in `area` (AREA_MAX 1024) through `fval`; `fill` turns a typed `\n` into a break and `field_show` draws breaks as " / "..
  - 6.39.0: each tab keeps a `label` (its title when it went behind) for `browser_tab_label`; browser_ui.h draws the strip at BR_PAGE_Y with two tabs or more and `br_page_y` moves the well down by BR_STRIP.
  - 6.38.0: `browser_history_match` (shares `hw_words` with `history WORDS`) feeds browser_ui.h's `hq_update` on each keystroke in the address bar; the matches cover the well and Tab steps `hq_sel`.
  - 6.37.0: `hw_nth_top` ranks HISTWORD.TXT lines by visits (ties newest first) for `history top` and `browser_top_site`; browser_ui.h's `tp_load` fills Top Sites, and.
  - 6.36.0: IMG_MAX is 8 (was 4); `img_px` is BSS, 480 KiB an image. 6.86.0: 16, the most the text's image mark (`0x10` + its number) can name.
  - 6.86.0: `show` no longer fetches images.
  - 6.86.0: a hiding rule may be `a + b` or `a ~ b` (two one-part selectors).
  - 6.50.0: a hiding rule may have three parts.
  - 6.49.0: a `>` between the two parts is a child rule: `gdep` counts open elements as to_text reads (a script's or style's close is counted in the skip branch), `anc_g` keeps the count around the first.
  - 6.47.0: `hide_act` lets the hide table carry two-part bold (2) and align (3, 4) rules too; `desc_rule` asks it when an element opens.
  - 6.44.0: BR_RIGHT, the same as BR_CENTER for text-align right or end and align=right (`center_mode` keeps which a rule asks for); the window flags BR_ST_RIGHT and starts the line all the spare width in.
  - 6.35.0: BR_CENTER and BR_LEFT marks around a centred element (`<center>`, align=center, inline or `center_sel` text-align:center); browser_ui.h carries the state across.
  - 6.34.0: `css_rules` also keeps one-part selectors whose block `says_bold` (`bold_sel`); to_text puts BR_BOLD when one opens and BR_PLAIN when its own tag's nesting closes, one at a time.
  - 6.33.0: a two-part descendant rule (`hide_sel` with one space) hides its second part (`sel_match`) while `anc_track` has its first part open, counting that tag's own nesting in `anc_d`.
  - 6.25.0: what the page hides stays hidden.
  - 6.17.0: downloads.
  - 6.16.0: tabs.
  - 6.15.0: forms and a cookie jar.
  - It asks for HTTP/1.1 with `Accept-Encoding: gzip` and `Connection: close`; `page_body` joins a chunked reply and inflates gzip through `drivers/inflate.c`.
  - The request buffer is sized from `URL_MAX`.
  - Redirects stop after three hops (`arm64-browser-check.py` proves a self-loop stops).
  - The HTML walker stays inside its input. `put` refuses to write past `TEXT_MAX`.
- The relay token is never printed.
  - It is one static buffer and one Authorization header, zeroed after the request.
  - Browser requests carry no token, cookie or session id. A browsed page is never sent to Claude.
  - The relay compares the token in constant time and does not log prompts or replies.
- The Wi-Fi passphrase never reaches the kernel. `wifi_cfg.sh` derives the PMK on the Mac. Only the PMK is embedded, and only when `JT_WIFI_DEV=1`.

### Findings, most severe first, with status

1. **High: the relay token was embedded in every Pi build made on the dev Mac.**
   - `claude_cfg.sh` read `~/.claude-relay-token` whenever it existed. Only the Wi-Fi key was gated.
   - Fixed: only `JT_WIFI_DEV=1` or an explicitly supplied token file embeds a token.
   - Check idea: build with the flag unset and fail if the token bytes appear in `kernel8.img`.
2. **High: the token crossed the Wi-Fi in clear text** over plain TCP with `--lan`.
   - Fixed 2026-10-09: Pi relay requests use verified HTTPS with no fallback. A LAN relay needs a certificate and key.
3. **High: the certificate clock came from an unauthenticated HTTP Date header.**
   - Reduced, not closed. The clock now takes only a verified HTTPS Date. It refuses dates before the build or a previously accepted time.
   - But the first handshake still validates at build time. So a certificate valid at build and expired now can pass that bootstrap check.
   - Closing it needs persistent trusted time or a signed-time protocol.
4. **Medium: an HTTPS page can redirect to plain HTTP silently.**
   - Fix: refuse an `http://` redirect from an `https://` page (`browser: redirect to http refused`), and mark plain links in the link list.
   - Check by a `/down` route in `arm64-browser-check.py`.
5. **Medium: DNS answers are easy to forge on the LAN.**
   - The transaction id came from the 100 Hz `ticks()`. The source port is fixed at 53000. Answers are matched on ports only.
   - HTTPS still protects page content. Plain HTTP pages and the clock are not protected.
   - Fix: take the id and port from the entropy source, drop answers from the wrong source IP, and compare the question section.
   - Check in `dns-txid-check.sh`.
6. **Medium: a crafted link can split the request line.**
   - `parse_url` accepts any byte, so `\r\n` in an `href` injects a header.
   - Fix: reject any byte below `!` or above `~` in host or path.
   - Check with a hostile page.
7. **Medium: the trust anchor set is small and hand-refreshed, with no expiry check or revocation.**
   - There are seven roots in `tools/gen/tls-roots.py`. The earliest, GlobalSign Root CA, expires January 2028.
   - There is no CRL, OCSP or CT. That is normal for a BearSSL minimal client, but it should be written down.
   - Fix: a CI check that fails when any root expires within a year or `TAS_NUM` differs from the PEM count.
8. **Low: the TLS random seed came from the timer, and the suite list includes plain RSA key exchange and CBC with SHA-1 (no forward secrecy).**
   - The BCM2711 RNG200 hardware FIFO now seeds TLS on the Pi. A health fault or one-second timeout refuses the connection.
   - `arm64-rng-check.py` checks the driver with fake registers. Hardware output still needs a board test.
   - Open: drop the `BR_TLS_RSA_WITH_*` and `*_CBC_SHA` suites unless a needed site requires them.
9. **Low: the dev card holds the Wi-Fi PMK and SSID in the image.**
   - Accepted. The dev card never leaves the house. `tools/flash-pi.sh` is the only path that sets the dev flag.
   - The log must never gain a line that prints the key.

### Dev card versus release card

| | Dev card (`tools/flash-pi.sh`) | Release card |
|---|---|---|
| Wi-Fi SSID and PMK | Embedded, from `~/.config/joshuatree/wifi.conf` | Absent: the Pi only scans |
| Relay host, port, token | Embedded, from `~/.claude-relay-token` | Absent |
| Trust anchors | The seven roots, plus any `TLS_TA=` extra (the relay CA) | The seven roots |

### What was not verified

- No run on the real board. QEMU does not model the Wi-Fi chip or the hardware RNG.
- BearSSL's minimal X.509 engine is known not to enforce name constraints in intermediate certificates. This is untested, and the roots are all public CAs.
- `drivers/net.c`'s TCP (one connection, no retransmission) and the WPA2 handshake were not reviewed.
- Whether `www.google.com` keeps answering plain `HEAD /` with a Date header.
- The relay's tool sandbox (`~/pi-files`, read only) was read in its header comment only.

### Relay follow-up, 2026-10-09

The API relay's shared-folder reader had a high-severity check/open race. A local process with write access to `~/pi-files` could replace an approved file with a symlink and make the model read outside the folder.

The reader now pins the folder, opens with `O_NOFOLLOW` and checks the opened descriptor.

</details>

## Lessons from the loop

What keeps going wrong when agents work this repo, and the rule that stops it.

### Splitting a file

**A macro defined in one part is invisible to the others.** Problem: `main.c` defined `APP_BOOT` for the windows and hang builds, and once `main()` moved to `boot.c` its `#if defined(APP_BOOT)` was false there, so the poll loop never ran.

**A cut can copy state or drop a guard without a build error.** Problem: in 4.23.5 `windows.c` kept its own `static int br_frame`, so a Browser resize set a copy the painter never read, and `HEAP_SIZE` reached `kmain.h` without its.

**A cut file must name the generated headers it includes.** Problem: 4.25.1 moved `kmain` to `kernel/kmain.c`, which includes `drivers/user_wc.h` and `drivers/version.h`; the Makefile tied those generated headers to `kernel.o` only, so a.

### Merges

**Generated files conflict on every merge.** Problem: `VERSION`, `landing/index.html`, `landing/version.txt` and the Testing section change in almost every PR, so merging main always conflicts there, and hand edits got lost (5ff21afa.

**A Makefile conflict is never "pick one side".** Problem: in the arm64 Makefile one side added the Calculator (`calc.o` on the link lines, built by a rule that uses `FPCC`, the floating-point compiler flags) and the other changed the.

**Conflict markers got committed and pushed.** Problem: `<<<<<<<` lines were committed and pushed in `arch/arm64/Makefile` (6f4ce57e) and `docs/ROADMAP.md` (Loop handoff) (40a4140e).

**Two open PRs both bump the version.** Problem: whichever merges second conflicts on `VERSION` and needs another bump. Rule: merge them one at a time, in the order the handoff names, and rebump the second after the first lands.

### Checks

**Fixed sleeps make CI flaky.** Problem: a check that sleeps a set time and then looks passes on a fast Mac and fails on a busy runner (fb6adbe3 and 3dff87ce swapped sleeps for polling).

**A new arm64 check that is not in the suite never runs.** Problem: a check file in `tools/checks/` does nothing unless `tools/checks/ci-suite.sh` lists it.

**the Testing section drifts from the suite.** Problem: adding or renaming a check leaves the testing page stale, and its check fails in CI (1a68634c, 583c747f were catch-up refreshes).

**Draft PRs never run CI.** Problem: the loop opens PRs as drafts so pushes do not burn runner time, which also means a draft shows no red even when it is broken.

**A fuzz harness passed on the Mac and leaked on Linux.** Problem: 6.54.0's `fuzz_appsig.c` never freed the two files it read. macOS ASan does not run LeakSanitizer, so it passed locally; CI's Linux runner failed it, a full CI cycle lost.

**Readers of network bytes shipped without a fuzzer.** Problem: the calendar, page, cookie, mail, Wi-Fi scan and DNS readers all passed their QEMU checks with good input, and a first fuzz run on 2026-10-27 (6.48.1 to 6.48.5) found sixteen.

**A new instruction in plankc passed on the Mac and broke on the Pi.** Problem: 6.92.0's `bxor`, `shl` and `shr` write `eor`, `lsl` and `lsr` with a register; `arm64-plank-check.py` passed, since clang assembles those on the Mac, but.

**plankc compiling itself on the box ran out of room as the runtime grew.** Problem: plankc compiles all of `rt.pk` with every program and only drops what is not reached at the end, so each runtime feature lengthens its own self-compile.

### The real Pi

**`ticks` stops counting on the Pi.** Problem: the timer handler stops advancing `ticks` early in boot, so any wait on it spins forever; the boot-time LED blink waited on it and left the USB keyboard dead (fa6995f4).

**The board finds bugs QEMU cannot.** Problem: QEMU has no Wi-Fi chip and a tidy network, so the WPA2 stall (the router wanted its exact security element repeated back), lost DNS replies and "no such host" (NXDOMAIN) answers only showed up.

### Process

**A debug QEMU run with no timeout took the Mac down.** Problem: QEMU with `-d int -D file` ran for two days and filled the disk. Rule: every traced QEMU run gets `timeout` in front, and the log is deleted after.

**Pulling the SD card mid-flash froze the Mac's disk.** Problem: a cut-off `tools/flash-pi.sh` run left the disk stuck until a restart. Rule: never pull the card while the flash script runs; wait for it to finish.

**Merging with `--admin` skips the checks the loop relies on.** Problem: an admin merge lands code nobody saw go green. Rule: never use `--admin`; `gh pr merge --auto --squash` once the PR is ready and green.

## Decisions

Standing answers, so nobody has to ask again. Locked 2026-10-06 night by Joshua. Change one by editing it here, with the date.

1. **Who can push.** The Claude relay needs a token and listens on the local network only. Claude opens pull requests and never merges. CI merges on green, like everything else.
2. **Power cuts.** Keep a spare card with the last good build. A fallback kernel boots the old build if a new one fails. A journaling filesystem comes later.
3. **Who owns what.** The OS stays free forever, under Apache 2.0. The hardware designs are CC BY-NC-SA. The Wi-Fi firmware is not ours: it downloads at flash time and never enters the repo.
4. **When 3.0 is done.** The desktop boots on a real Raspberry Pi 4, you can type and click on it, mouse included, and one app from the shared `user/` tree runs on the Pi in user mode (Joshua, 2026-10-11: "Focus on user mode by v3").
5. **Who it is for.** Joshua first. Then the people who build computers for fun and would buy the kit. Kids come later; that is the version 100 idea.
6. **Backups.** GitHub holds the code, the LaCie holds the rest. Push every day.
7. **Usage.** Stop starting new work at 90 percent of a weekly limit. Haiku for small jobs. At most three helpers, two if any is Opus or Fable.

## Testing

Every check the suite runs, what it proves, and which apps still have none. Generated by `tools/gen/testing-doc.py` from `tools/checks/ci-suite.sh`; do not edit between the markers.

<details><summary>Details (474 lines)</summary>


<!-- testing:start -->
Run it all locally with `tools/ci-local.sh`. Run one with the command in the table.

### Running suites safely

Local CI and pre-push reject new fixed test paths before building.

The balancer packs ARM checks across shards 6, 8, 9 and 10 (the raspi4b runner) and the rest across the others.

Only one suite runs at a time on a Mac.
- `tools/ci-local.sh` and `tools/checks/ci-suite.sh` take a lock (`tools/ci-lock.sh`).
- A second run waits its turn. It prints `waiting for ci-local pid N` every two minutes, for up to an hour.
- A lock left by a dead run clears itself.
- `CI_LOCAL_NO_LOCK=1` skips it.

Every QEMU check writes into its own temp directory (`tools/checks/scratch.py`).
- `JT_KEEP_TMP=1` keeps it for debugging.
- `tools/checks/tmp-paths-check.py` fails any new fixed `/tmp/jt-*` path.

### The suite (405 checks)

| What it proves | File | Mode |
|---|---|---|
| ARM menu notices: bounded messages, expiry, pixels and input | `tools/checks/arm64-notice-check.py` | once |
| Pi screen sleep and deep idle (10.1.0): idle deadline, wrap, active apps, refused mailbox requests, keyboard/mouse wake, and 50 loop passes a second asleep | `tools/checks/arm64-screen-idle-check.py` | once |
| Auto lock on the Pi (13.24.0): the saver locks, a key wakes the card and is not typed, only the signed-in password opens it, and the desktop comes back exactly | `tools/checks/arm64-lock-check.py` | retry |
| Trackpad (10.2.0): one finger moves the pointer, two scroll, a tap clicks left and a two-finger tap right, on the host and through QEMU virtio-multitouch | `tools/checks/arm64-trackpad-check.py` | once |
| USB precision touchpads (11.0.0): a PTP descriptor is found and a mouse's or touch screen's is not; its reports drag, scroll, tap, click and lift through trackpad.h | `tools/checks/arm64-ptp-check.py` | once |
| Heat (13.3.0): `temp` reads the chip's temperature from the firmware on raspi4b to a tenth of a degree, virt says it has no sensor, a refused tag says so | `tools/checks/arm64-heat-check.py` | once |
| GPU (13.2.0): gpu.c brings the V3D up in Linux's order on a fake Pi 4 and stops before any V3D read at a refused tag, a missing bridge or a stuck ACK; virt has none, raspi4b has no bridge and boots on | `tools/checks/arm64-gpu-check.py` | once |
| Battery (10.3.0): a MAX17048 gauge read over the Pi's own I2C driver (an EEPROM stands in for it on QEMU raspi4b), none without one, and the menu bar shows it | `tools/checks/arm64-battery-check.py` | once |
| Brick on ARM: collision rules, lives and win under sanitizers; keyboard/mouse play, timed animation, pause, restart, Spotlight and close in QEMU | `tools/checks/arm64-brick-check.py` | once |
| Spotlight on ARM lists every app including the Calculator and Settings on an empty query, typing calc shows it and Enter opens the window, in headless QEMU | `tools/checks/arm64-spotlight-check.py` | once |
| Accounts on the Pi: accounts.c under sanitizers (add, check, refusals, corrupt files, failed writes, PBKDF2 vectors); in headless QEMU adduser, users and deluser work, the password never shows or reaches the UART, and JT/USERS.TXT is a real PBKDF2 record that survives a reboot | `tools/checks/arm64-accounts-check.py` | once |
| Login on the Pi: login.c on the host under sanitizers; in headless QEMU a card with an account boots to the login screen, a wrong password costs a second and stays, the right one brings the desktop and the name, logout returns, a card with no account has no login, and a login that accepts any password is caught | `tools/checks/arm64-login-check.py` | once |
| Admins and sudo on the Pi: sudo.c under sanitizers (first admin, five minute memory, makeadmin, unadmin, damaged SUDOERS.TXT, passwd, same-time unknown name); in headless QEMU a normal user is refused every privileged command, sudo asks a starred password and remembers it, passwd changes it, no password or key code reaches the UART, the quiet boot shows the login card first, and a sudo that skips the check is caught | `tools/checks/arm64-sudo-check.py` | once |
| Home folders on the Pi: home.c and ufile.c under sanitizers over an in-memory FAT (an app's root is JT/HOME/<name>, flush after sign-out, another's folder refused by any path, cat/ls/write/rm walker, Samantha's save, folder-name collisions); in headless QEMU a normal user works in ~ without sudo, cannot touch an admin's folder, an admin needs sudo for his, adduser makes the folder and deluser leaves it, and two mutations are caught | `tools/checks/arm64-home-check.py` | once |
| Home snapshots: snap.c and the real fat.c on a mkfs.fat image under sanitizers (nested copy, SNAP never copied into itself, newest three kept, a snapshot without _DONE ignored and cleaned, restore puts files back and removes later ones, fat_rmdir, fsck.fat clean) | `tools/checks/arm64-snap-check.py` | once |
| Settings on the Pi: settings.c parsing and file text under sanitizers; in headless QEMU Spotlight and the menu bar icons open the window, six sections with their own glyphs, a wallpaper change repaints the whole desktop and a window closed afterwards leaves the new colour, volume and dock size step, and JT/SETTINGS.TXT survives a reboot | `tools/checks/arm64-settings-check.py` | once |
| Crash-safe card: the real fat.c on FAT16 and FAT32 mkfs.fat images under sanitizers, power cut after every sector write of replace (more and less data), delete, mkdir and rmdir; fsck.fat -n reports at most unreachable clusters or unequal FAT copies, the mount scan repairs them and fsck.fat is then clean, the file is exactly old or new, and the entry-before-data mutation fails | `tools/checks/fat-crash-check.py` | once |
| Settings Users pane on the Pi: in headless QEMU the sixth section lists the accounts, adds one (a mismatched second password and a wrong admin password are refused, passwords are stars and never reach the UART), makes an admin on sudo's remembered window, resets a password, removes with a confirm step, keeps it across a reboot, refuses a normal user every admin action while he changes his own password with the old one, and a pane that skips the admin check is caught | `tools/checks/arm64-users-check.py` | once |
| Stocks on ARM: the tile and Spotlight open the Pi window, the watchlist pills and the line chart are drawn from quotes fetched off a fixture server, Down changes the chart, Esc and the red dot close it, and with no network it shows no prices, in headless QEMU | `tools/checks/arm64-stocks-check.py` | retry |
| Games on the Pi: Spotlight opens Mines, Toroid, Hamurapi, Keyrate, Solitaire, Paddle, Bricks, Depths and Snake at EL0, a reveal (key and click), a generation, a year and a typed word each change the pixels, Esc closes each, in headless QEMU | `tools/checks/arm64-games-check.py` | once |
| Movies on the Pi: Spotlight opens the unchanged user/movies.c at EL0, a motion-JPEG AVI plays all 40 frames in order and its frame code is on the screen, a file that is not a clip shows an error, and the desktop survives, in headless QEMU | `tools/checks/arm64-movies-check.py` | once |
| Magnifying dock on the Pi: the tile under the pointer grows into the room above the tray and follows the pointer, a click still opens it, and leaving puts every pixel back (the plain hover label too), in headless QEMU | `tools/checks/arm64-dockmag-check.py` | once |
| Queued mouse reports on the Pi are drawn once per pass, at the latest position: 20 made-up reports in one go run pointer_moved once (the dock magnify and a dragged window keep up with the hand) | `tools/checks/arm64-ptrburst-check.py` | once |
| Browser on a full heap: with 3 to 4 MiB of heap free, a small page still loads (the reply buffer shrinks to fit, 4 MiB down to 256 KiB) | `tools/checks/arm64-browser-lowmem-check.py` | once |
| Genie on the Pi: the yellow dot minimizes Calendar's window into its dock tile over five frames or more, the desktop comes back exactly, the app keeps running, and its tile brings the window back pixel for pixel, in headless QEMU | `tools/checks/arm64-genie-check.py` | once |
| Spaces on the Pi: Ctrl+Right and Ctrl+Left step between four desktops, an app's window stays on its own (not drawn, not clickable, still running), Cmd+Tab keeps to the desktop, and a dock tile goes to its window's desktop, in headless QEMU | `tools/checks/arm64-spaces-check.py` | once |
| Mission Control on the Pi: Ctrl+Up shows the four desktops and this desktop's app windows as thumbnails, Enter raises one, a chip goes to its desktop, Esc or a click outside closes it and puts the screen back, in headless QEMU | `tools/checks/arm64-mission-check.py` | once |
| Shortcuts on the Pi: shortcut NAME = A; B saves a line of Terminal commands to JT/SHORTCUT.TXT, shortcut NAME runs each one shown first, loops and bad names are refused, Samantha's shortcut plugin saves one only under a name the person typed, rm drops it, against a fake relay in headless QEMU | `tools/checks/arm64-shortcut-check.py` | once |
| Parental controls on the Pi: an admin limits another account's apps, hours and web with sudo limits, kept in JT/LIMITS.TXT; the limited account is refused the rest, cannot lift a limit or write the file, and admins are never limited, in headless QEMU | `tools/checks/arm64-limits-check.py` | once |
| YouTube clips are the whole video: yt.py fits a video into the Pi's bytes by bitrate across its full length, 320 wide when generous, cut only below a watchable floor, on generated videos with no network (static) | `tools/checks/yt-fit-check.py` | once |
| Spotify remote on the Mac: spotify.py renders now playing, Play/Pause links and search hits from a saved Web API reply (HTML escaped), and refuses a wrong token, a bad command or URI, a GET and another path, with no network or account | `tools/checks/spotify-check.py` | once |
| Spotify in Blackcomb: a spotify: address sends one command to spotify.py (a saved Web API reply) and shows the remote as a page whose Prev link sends the next command, with the token kept off the UART, in headless QEMU | `tools/checks/arm64-spotify-check.py` | once |
| SBC encoder: A2DP's codec in fixed point, its frames read back by ffmpeg's own SBC decoder as the 1000 and 440 Hz tones that went in, above 20 dB SNR, every frame a valid CRC-8, on a generated tone with no network (static) | `tools/checks/sbc-encode-check.py` | once |
| Tab groups on the Pi: tab group save NAME keeps the open tabs in JT/TABGROUP.TXT, saving again rewrites the group, tab group NAME swaps them in, rm drops one, bad names and unknown groups are refused, and a second boot brings a group back from the card, in headless QEMU | `tools/checks/arm64-tabgroups-check.py` | once |
| Photos on the Pi: the PHOTOS folder as a grid, one full size, a quarter turn, black and white and light kept in PHOTOEDT.TXT across a restart and reverted with 0, the photo itself never rewritten, in headless QEMU | `tools/checks/arm64-photos-check.py` | once |
| Traffic lights on the Pi: the front window's dots are plain until the pointer is over them, a window behind another goes grey, and focus moves the colour, in headless QEMU | `tools/checks/arm64-traffic-check.py` | once |
| Pi keyboard shutdown and restart: confirmed HID chords halt or reboot raspi4b, Escape cancels, busy and hidden prompts refuse (host and QEMU) | `tools/checks/arm64-power-check.py` | once |
| ARM tree menu: click or F10 opens About, Restart, Shut Down and Log Out (pixels, keys, outside click), power rows confirm and refuse mid-answer, Log Out closes windows, raspi4b restarts and halts | `tools/checks/arm64-treemenu-check.py` | once |
| The whole web's roots: the Pi's trust anchors hold every root in Mozilla's list, each once, ISRG Root X1 among them, from a bundle under 18 months old (host only) | `tools/checks/tls-roots-check.py` | once |
| CI tooling packs ARM checks across the ARM shards only and rejects fixed test paths before builds (host only) | `tools/checks/ci-tooling-check.py` | once |
| Dock slot constants agree with kernel.c (static drift guard) | `tools/checks/dockslots-check.py` | once |
| Keyrate (13.24.0): the page `keyrate` serves is one offline file of about 42 KB, loading nothing by src, href, import, CSS url() or @import, fetch() or a Worker | `tools/checks/keyrate-offline-check.py` | once |
| No check hard-codes a fixed temp path or socket, so two suites cannot corrupt each other (static, baseline only shrinks) | `tools/checks/tmp-paths-check.py` | once |
| docs/ARCHITECTURE.md states only what the source says: icon shape and light, fonts, colours, dock and window numbers, caption timings (static) | `tools/checks/design-doc-check.py` | once |
| every source file the progress graph counts has a row in docs/ARCHITECTURE.md (docs at 100 percent, 6.59.1) (static) | `tools/checks/doc-coverage-check.py` | once |
| plankc, the Plank compiler written in Plank (user/plankc), tokenizes (6.60.0) and parses (6.61.0) all of Plank's examples, its own files and its edge cases exactly as plank.py does, token for token and tree for tree, steps 1 and 2 of the true bootstrap (skips without Plank and uv; CI pins Plank on this shard) | `tools/checks/plankc-check.py` | once |
| Kernel memory keeps 16KB clear of the program window (toolchain drift guard) | `tools/checks/bss-margin-check.py` | once |
| Samantha's face loops wrap without a seam | `tools/checks/face-frames-check.py` | once |
| docs/ARCHITECTURE.md lists every check in this suite | `tools/checks/testing-doc-check.sh` | once |
| The landing logo rebuilds byte for byte from tools/gen/logo.py | `tools/checks/logo-check.sh` | once |
| Landing app tiles are not older than the app code that draws them (source hashes, no QEMU) | `tools/checks/landing-shots-fresh-check.py` | once |
| The landing demo downloads the kernel once, gzipped | `tools/checks/kernel-gz-check.sh` | once |
| Samantha box CAD: every fit assert holds (bosses clear the Pi, ports and screen, one-piece shell, the box drops into its stand) and all five files are written; the old stand slot stops it (skips without uv and build123d) | `tools/checks/samantha-box-check.py` | once |
| Boot check | `check.sh` | retry |
| DHCP client leases real SLIRP config, DNS+HTTP still work, nodhcp keeps the old fixed path | `tools/checks/dhcp-check.sh` | retry |
| Benchmarks run and report every number | `tools/checks/bench-check.sh` | retry |
| ISO boot (CD-ROM and USB/raw-disk paths) | `tools/checks/iso-boot-check.sh` | retry |
| PNG decoder, host harness | `tools/checks/png-host-check.sh` | once |
| Vendored BearSSL TLS 1.2 client subset compiles for aarch64 freestanding (static) | `tools/checks/bearssl-tls-check.py` | once |
| Embedded apps are stored compressed: every one inflates to its exact binary, damaged copies are refused (host harness) | `tools/checks/user-compress-check.py` | once |
| Hamurabi game rules: SplitMix64, golden checksum, invariants, i386-freestanding | `tools/checks/hamurabi-rules-check.sh` | once |
| Hamurabi sprite sheet header is current with art/hamurabi/sprites.png and sprites.json (static) | `python3 ./tools/gen/gen_hamurabi_sprites.py --check` | once |
| Burrow: Files renamed, Samantha still opens it as files / file browser, icon art is the new fox | `tools/checks/burrow-rename-check.py` | once |
| Burrow icon view wraps a long file name onto a second line instead of cutting it | `tools/checks/burrow-labels-check.py` | retry |
| Mail, Notes and Weather read past byte 255 of their data file (SYS_READ moves 255 bytes a call) | `tools/checks/read-long-files-check.py` | retry |
| Clock opens and its countdown timer updates live | `tools/checks/clock-check.py` | retry |
| TTF rasterizer, host harness | `tools/checks/ttf-host-check.sh` | once |
| libjt string/stdlib, host harness | `tools/checks/libjt-host-check.sh` | once |
| God-file guard (no hand-written .c/.h over its line ceiling) | `tools/checks/godfile-check.sh` | once |
| PNG decoder, in-kernel | `tools/checks/png-check.sh` | retry |
| AA text spacing, in-kernel | `tools/checks/textspacing-check.sh` | retry |
| AA text stems are dense but still antialiased | `tools/checks/textsharp-check.py` | retry |
| FAT filesystem cycle hang (regression test) | `tools/checks/fatcyclehang-check.sh` | retry |
| File robustness: empty, oversized, corrupt-FAT and full-disk cases | `tools/checks/filerobust-check.py` | retry |
| Chat defaults to Samantha (Turing) and surfaces an HTTPS-redirect host clearly | `tools/checks/chat-samantha-check.py` | retry |
| GUI Chat app asks Samantha and renders the reply on screen | `tools/checks/chatapp-check.py` | retry |
| Chat's tools (reminder, note, open app) work locally via /api/pick, ordinary questions still reach Samantha | `tools/checks/chattools-check.py` | retry |
| Chat mail tools: "read my email" lists a real message, "email <someone> <text>" lands one in Mail | `tools/checks/mailtools-check.py` | retry |
| Chat notes/reminders tools: list_reminders and read_notes work via the local keyword fallback when the picker doesn't know them | `tools/checks/notestools-check.py` | retry |
| "samantha" boot flag opens Chat's full-screen avatar view, input focused, before the desktop | `tools/checks/samantha-boot-check.py` | retry |
| Samantha tells the time and the date from her own clock; pagebar hides only her bar | `tools/checks/samantha-clock-check.py` | retry |
| "phone" boot flag opens a real 430x932 portrait frame straight into Samantha's view | `tools/checks/phone-boot-check.py` | retry |
| Phone Samantha back chevron exits her view and F2/Esc hints are hidden on phones | `tools/checks/phone-samantha-back-check.py` | retry |
| Samantha is full screen: room around her (her wall in the side bands, eyes 40 percent down), glass input, fading captions that stay off her lips and eyes, scrollback, the typing bugs, a mouth drawn from her voice, Esc and the red dot | `tools/checks/samantha-fullscreen-check.py` | retry |
| Touch: a tap opens the on-screen keyboard on phone and a tapped key reaches the Notes editor | `tools/checks/touch-osk-check.py` | retry |
| No-disk boot falls back to ramfs with seeded demo files | `tools/checks/ramfs-demo-check.sh` | retry |
| Shell regression suite (heap, task, preempt, kill, ring3, ps) | `tools/checks/shellregress-check.sh` | retry |
| Small tool picker in C agrees with its Python reference (pickcheck, no network) | `tools/checks/picker-check.sh` | retry |
| Ring-3 reference program against the v1 syscall ABI | `tools/checks/usertest-check.sh` | retry |
| Ring-3 program writing a real file against the v2 syscall ABI | `tools/checks/notetest-check.sh` | retry |
| Keyrate runs as a ring-3 process with its own window; crashing it leaves the desktop alive (v3 syscall ABI) | `tools/checks/ring3app-check.py` | retry |
| Toroid runs as a ring-3 process through the table-driven launcher: draws generations, closes both ways, crashes safely, desktop alive | `tools/checks/ring3toroid-check.py` | retry |
| Calculator runs as a ring-3 process through the table-driven launcher: evaluates 12*3=36 and 5/0=0 through the real parser, closes both ways, crashes safely, desktop alive | `tools/checks/ring3calc-check.py` | retry |
| Quotes runs as a ring-3 process through the table-driven launcher: draws the option grid, answers right and wrong through the real logic, closes both ways, crashes safely, desktop alive | `tools/checks/ring3quotes-check.py` | retry |
| Bookrank runs as a ring-3 process through the table-driven launcher: draws the ranked list, moves the selection by keyboard and mouse through the real logic, closes both ways, crashes safely, desktop alive | `tools/checks/ring3bookrank-check.py` | retry |
| Bookrank pulls the real shelf live (Worker text from a stub), scrolls it, bounds a hostile reply, and shows the ten samples for junk, a 500, an oversize body and no NIC | `tools/checks/ring3bookrank-live-check.py` | retry |
| Patois runs as a ring-3 process through the table-driven launcher: draws the word and choices, answers right and wrong through the real drill, closes both ways, crashes safely, desktop alive | `tools/checks/ring3tonchi-check.py` | retry |
| Patois lists the real courses live (Worker text from a stub), drills one with a score, bounds a hostile reply, and shows the Spanish deck for junk, a 500, an oversize body and no NIC | `tools/checks/ring3tonchi-live-check.py` | retry |
| Fieldbook runs as a ring-3 process through the table-driven launcher: draws the ranked field list, moves the selection by keyboard and mouse through the real logic, closes both ways, crashes safely, desktop alive | `tools/checks/ring3fieldbook-check.py` | retry |
| Clock runs as a ring-3 process through the table-driven launcher: draws the moving time, takes a timer through the real input logic, closes on Esc, desktop alive | `tools/checks/ring3clock-check.py` | retry |
| Mines (Minesweeper) runs as a ring-3 process: a scripted game on the fixed first board is won by keyboard, closes on Esc, desktop alive | `tools/checks/ring3mines-check.py` | retry |
| Solitaire runs as a ring-3 process: a solver wins the fixed first deal and the app plays the win move for move against a rules model, closes on Esc | `tools/checks/ring3-solitaire-check.py` | retry |
| Paddle runs as a ring-3 process: the ball moves on the clock, Up moves your paddle, a point is scored, p pauses, closes on Esc | `tools/checks/ring3-paddle-check.py` | retry |
| Bricks runs as a ring-3 process: the first serve breaks a brick, Left moves the paddle, p pauses, closes on Esc | `tools/checks/ring3-bricks-check.py` | retry |
| Depths runs as a ring-3 process: draws walls, floor and ceiling, hits the creature ahead, walks the maze to the door and gets out, closes on Esc | `tools/checks/ring3-depths-check.py` | retry |
| Snake runs as a ring-3 process: eats the seeded apple ahead, ignores a reverse, dies at the wall, plays twelve apples in step mode, closes on Esc | `tools/checks/ring3-snake-check.py` | retry |
| Chess runs as a ring-3 process: e2e4 by cursor, six typed moves each answered by the computer, the whole game replays legally through the host rules, an illegal move refused, n and esc work | `tools/checks/ring3-chess-check.py` | retry |
| Chess rules and opponent on the host: perft counts from five standard positions exact, mate, stalemate and bare kings known, mate in one and a free queen taken, only legal replies | `tools/checks/chess-core-check.py` | once |
| Running-app dots in the dock on both desktops: Notes (i386), the Console's Terminal and Calendar (Pi) get one small dark dot under their tile while open, and it goes when they close | `tools/checks/dockdots-check.py` | retry |
| Every typeface loads at boot: 14 of 14 on i386 and 3 of 3 on the Pi before the desktop, and a broken face (fontbreak=inter) is named and drawn as DejaVu Sans while the desktop still comes up | `tools/checks/fonts-boot-check.py` | once |
| Backups to the Mac: the relay refuses hostile containers (traversal, absolute, not 8.3, too deep, oversize, cut short, repeats) and writes nothing, keeps a good one whole; the Pi's backup sends ann's home byte for byte, 40 KB file left out | `tools/checks/arm64-backup-check.py` | retry |
| The bench (13.16.0): the relay hands a queued line only to a Pi with the token, the Pi runs it only after `bench` was typed there, and the output comes back to the Mac through tools/pi-bench.py; 13.17.0: keys, typing, clicks, drags and a PNG of the screen | `tools/checks/arm64-bench-check.py` | retry |
| The card's keys (13.19.0): the relay token and CA on the card connect a kernel built with neither (a release image), and short, long, cut or forged key files are refused | `tools/checks/arm64-cardkeys-check.py` | retry |
| Nimble on the Pi: nimble QUESTION at the Terminal and Samantha's nimble plugin against a stub API, numbered sources, a 502, a 1 MB reply, not-JSON and a hostile reply in plain words, no network says so | `tools/checks/arm64-nimble-check.py` | retry |
| Printing (13.11.0, 13.12.0): print FILE PRINTER sends a well-formed PDF as an IPP Print-Job to a stub printer, printers finds it over mDNS; refusals, no printer and hostile replies said plainly | `tools/checks/arm64-print-check.py` | retry |
| Scanning (13.14.0): scan [SCANNER] sends eSCL ScanSettings to a stub scanner, waits out a 503 warm-up, saves the JPEG into PHOTOS where Photos lists it, finds the scanner over mDNS; refusals said plainly | `tools/checks/arm64-scan-check.py` | retry |
| Cmd+Tab and Ctrl+Tab on the Pi: Calendar and Notes open, each chord brings the window behind to the front, and a bare Tab stays with the app | `tools/checks/arm64-cmdtab-check.py` | retry |
| The Pi desktop takes keys from its first moment: Cmd+Space sent the instant the desktop is up opens Spotlight, the first key is stamped and no main-loop step stalls the keys | `tools/checks/arm64-firstkey-check.py` | retry |
| Paddle finishes and can be won, and Bricks can be cleared: their physics played to the end on the host by a scripted player | `tools/checks/games-sim-check.py` | once |
| Movies plays a real AVI with sound at ring 3, audio-led: frame within one of the audio clock, drift under 100 ms, pause holds frame and sound (checked in the wav and on the framebuffer), seek by bar and keys, fullscreen, a damaged clip / non-AVI / over-cap file each show an error, closes on Esc, desktop alive | `tools/checks/movie-check.py` | retry |
| Hamurapi (Hamurabi in the sources) is playable as a ring-3 program: draws its real title scene (sprite pixels equal the sheet), plays a classic reign by keyboard, a story reign with cards and choices and the robot's demo, and every year it logs equals a host replay of the same rules header, closes on Esc, desktop alive | `tools/checks/ring3hamurabi-check.py` | retry |
| Airwell runs as a ring-3 process through the table-driven launcher: the circle grows on the in-breath and shrinks on the out-breath (measured off the framebuffer), all four presets by key and click log the web app's exact phases and seconds, pause holds the circle and the clock, closes on Esc, desktop alive | `tools/checks/ring3windgate-check.py` | retry |
| AVI reader, host harness (real JPEG frames, bad headers, truncation, mutation fuzz under ASan/UBSan) | `tools/checks/avi-host-check.sh` | once |
| Portfolio runs as a ring-3 process through the table-driven launcher: draws the fleet catalog, moves the selection by keyboard and mouse through the real logic, closes on Esc, desktop alive | `tools/checks/ring3portfolio-check.py` | retry |
| Activity runs as a ring-3 process through the table-driven launcher: draws the live task list, refreshes it on its own, has the kernel refuse to kill the shell, closes on Esc, desktop alive | `tools/checks/ring3activity-check.py` | retry |
| Contacts runs as a ring-3 process through the table-driven launcher: draws the list, adds and deletes a person through the real prompt, keeps CONTACTS.TXT across fresh runs, closes on Esc, desktop alive | `tools/checks/ring3contacts-check.py` | retry |
| Thinkpond runs as a ring-3 process through the table-driven launcher: draws the ranked list, moves the selection, re-sorts when an upvote passes the leader, closes on Esc, desktop alive | `tools/checks/ring3hikko-check.py` | retry |
| Thinkpond shows the real forum ideas live (Worker text from a stub), scrolls them, keeps votes local and forum order for ties, bounds a hostile reply, and shows the ten demo ideas for junk, a 500, an oversize body and no NIC | `tools/checks/ring3hikko-live-check.py` | retry |
| Reminders runs as a ring-3 process through the table-driven launcher: adds, ticks and deletes items through the real prompt, keeps REMINDERS.TXT across fresh runs, closes on Esc, desktop alive | `tools/checks/ring3reminders-check.py` | retry |
| Scrounge runs as a ring-3 process through the table-driven launcher: falls back to the samples when SYS_HTTP_GET finds no NIC, selects by key and click, the kernel refuses every bad path and pointer the probe hands the syscall, closes on Esc, desktop alive | `tools/checks/ring3curbfind-check.py` | retry |
| Calendar runs as a ring-3 process through the table-driven launcher: gets today from SYS_TIME, draws the month grid, saves an event through the real editor, keeps EVENTS.TXT across fresh runs, feeds Samantha's calendar_today, closes on Esc, desktop alive | `tools/checks/ring3calendar-check.py` | retry |
| Search runs as a ring-3 process through the table-driven launcher: SYS_READDIR refuses a kernel pointer, an over-long path and a missing folder, the list filters live, a file shows its real bytes, a FAT folder opens by relative path with the kernel's cwd untouched, closes on Esc, crashes safely, desktop alive | `tools/checks/ring3search-check.py` | retry |
| Epiphany runs as a ring-3 process through the table-driven launcher: falls back to the offline prices when SYS_HTTP_GET finds no NIC, switches tabs by key and click, runs the command bar, closes on Esc, desktop alive | `tools/checks/ring3epiphany-check.py` | retry |
| Weather runs as a ring-3 process through the table-driven launcher: reads the kernel's WEATHER.TXT and shows the offline face over labelled sample data, R refetches once and restarts it, closes on Esc, desktop alive | `tools/checks/ring3weather-check.py` | retry |
| Burrow runs as a ring-3 process through the table-driven launcher: draws the folder grid, Enter opens a folder and Backspace goes up through SYS_READDIR, closes on Esc, desktop alive | `tools/checks/ring3burrow-check.py` | retry |
| Every ring-3 app (parsed from RING3_APPS) crashes on purpose, is reaped, and the desktop keeps drawing and opens a different app after each one | `tools/checks/ring3crash-all-check.py` | retry |
| A released window framebuffer is supervisor-only again: store faults, pointer into it or into the kernel is -EFAULT | `tools/checks/userfb-release-check.py` | retry |
| SYS_BRK: a program grows 3MB of zeroed heap pages, bad tops are -EINVAL, and its crash gives every frame back (brk live=0, pmm free unchanged), desktop alive | `tools/checks/ring3brk-check.py` | retry |
| Shell launches a ring-3 program by bare name, case-insensitively | `tools/checks/shellname-check.sh` | retry |
| QEMU vmmouse absolute-pointer round trip | `tools/checks/vmmouse-check.sh` | retry |
| Calendar date math, host harness | `tools/checks/check-calendar.sh` | once |
| Settings click acts on the row actually clicked | `tools/checks/settingsclick-check.sh` | retry |
| Settings Location geocodes, persists, and fails clean | `tools/checks/location-check.py` | retry |
| Wallpaper defaults to Satellite on a fresh boot | `tools/checks/walldefault-check.sh` | retry |
| Idle tour's Settings visit doesn't change the wallpaper theme | `tools/checks/walldemo-regression-check.py` | retry |
| Wallpaper compose: Map and Satellite fetch distinct, byte-correct buffers (hermetic, fake tile server) | `tools/checks/wallcompose-check.py` | retry |
| Notes editor chrome doesn't redraw on plain keystrokes | `tools/checks/editorflash-check.sh` | retry |
| System-wide clipboard: Ctrl+C/X/V round-trips real text within Notes, across Notes->Terminal, truncates a too-long paste cleanly | `tools/checks/clipboard-check.py` | retry |
| Reminders prompt redraws content, not chrome, per keystroke | `tools/checks/gui-prompt-keystroke-check.sh` | retry |
| Notes typing, typography, pointer controls, persistence | `tools/checks/editor_qa.py` | retry |
| Notes' runtime-TTF text is real antialiased rasterization at 12pt and 200pt, not a duplicated-block bitmap upscale | `tools/checks/notessharp-check.py` | retry |
| Notes folders: legacy NOTES.TXT migrates intact, a new note lands in the current folder, both survive reboot, delete asks first | `tools/checks/notesfolders-check.py` | retry |
| Notes autosave: a changed note saves itself about two seconds after the last key (never per keystroke), survives a red-dot close that kills the program, and is back on screen after reopening; with autosave off in a private build the same check fails | `tools/checks/notes-autosave-check.py` | retry |
| Books (2.66): the ring-3 reader lists BOOKS/, refuses a book over 3 MB, turns two pages, saves the page to BOOKS/TEST.POS and reopens on it after a reboot | `tools/checks/books-check.py` | retry |
| Terminal grid draws the mono face at its true advance | `tools/checks/termmono-check.py` | retry |
| Terminal's runtime-TTF text is real antialiased rasterization with a driftless monospace grid | `tools/checks/termsharp-check.py` | retry |
| Panes: independent scrollback per tab, tab rail, activity dot on a background tab, close, split panes with their own shells | `tools/checks/panes-check.py` | retry |
| Claude app: asks Claude Code through the relay (stub claude on the host), draws the reply, resumes the session, red error line on a wrong token, a stopped relay and no relay set (-EACCES at once) | `tools/checks/ring3claude-check.py` | retry |
| Claude relay: token refused and accepted, session resume, exact read-only argv, prompt on stdin, timeout kills the process group, size cap, one request at a time, log hygiene, a mutant without the token check lets a tokenless request in, and --turing against a stub serve.py answers M Samantha with [[note]] intact, 502 when Turing is down, and --api-key-file refuses to start without --allow-api-spend | `tools/checks/claude-relay-check.py` | once |
| Relay voice: POST /api/tts against a stub ElevenLabs returns its PCM (22050 Hz mono); text over the per-request cap and requests past the per-day cap are refused with no upstream call; the day count survives a relay restart and resets on a new date; failures give characters back; the key never appears in a response, header or log, even when the upstream echoes it; a tokenless request is 401; a key file others can read is refused (no network) | `tools/checks/relay-tts-check.py` | once |
| Relay API mode: file tools refuse ../, absolute paths, dot files, symlinks and unlisted names and cap reads at 20000 characters; model routing; the prompt names both actions; the reply's M line names the model that answered (Claude Haiku 5.5, Sonnet, Opus) and ask.c's default prompt is Samantha, the relay's Turing name (no network) | `tools/checks/relay-api-check.py` | once |
| Apple-menu hover stays cheap, clock redraws on a minute change | `tools/checks/menuclock-check.sh` | retry |
| Lock Screen: menu item locks, Esc cannot bypass, password unlocks | `tools/checks/lockscreen-check.py` | retry |
| Multi-window chrome doesn't redraw on plain keystrokes | `tools/checks/mwkeyflash-check.sh` | retry |
| Drawing lands offscreen, window_present puts it on screen | `tools/checks/backbuffer-check.sh` | retry |
| Multi-window apps draw exactly one toolbar, not two | `tools/checks/mwdupetoolbar-check.sh` | retry |
| Nimbus guide (9.0.0): the Build a Nimbus chapter has its seven steps, names only real files and scripts, covers every part in bom.csv, and promises only screens arm64-screens-check boots | `tools/checks/guide-nimbus-check.py` | once |
| World fonts (9.2.0): every character past Latin-1 in lang.c's plain rows is in world_font.h, and lang_hi.h was shaped from the Hindi row lang.c has | `tools/checks/lang-fonts-check.py` | once |
| CI manifest: every ci-suite row splits into mode, shard, name and a runnable command, so a pipe inside a name (8.3.0) cannot hand bash half a sentence | `tools/checks/ci-manifest-check.py` | once |
| JPEG decoder, host harness (baseline and, since 6.46.0, progressive) | `tools/checks/jpeg-host-check.sh` | once |
| Media decoders (WAV, MP3), host harness under ASan and UBSan | `tools/checks/media-host-check.sh` | once |
| Tasks keep their own x87 float state across switches | `tools/checks/fpu-check.sh` | retry |
| HTML entities decode to ASCII, host harness | `tools/checks/html-host-check.sh` | once |
| The Pi Terminal folds UTF-8 to Latin-1 cells: é stays é, past Latin-1 is ?, host harness | `tools/checks/utf8fold-host-check.sh` | once |
| JPEG decoder, in-kernel | `tools/checks/jpeg-check.sh` | retry |
| PNG/JPEG decoder fuzz (ASan/UBSan, truncation+mutation+nasties) | `tools/checks/decoder-fuzz-check.sh` | once |
| iCalendar and CalDAV parser fuzz (ASan/UBSan, truncation+mutation; 6.48.1) | `tools/checks/ics-fuzz-check.sh` | once |
| Blackcomb page and Set-Cookie parser fuzz (ASan/UBSan, truncation+mutation; 6.48.2) | `tools/checks/browser-fuzz-check.sh` | once |
| IMAP reply and MIME parser fuzz (ASan/UBSan, truncation+mutation; 6.48.3) | `tools/checks/mail-fuzz-check.sh` | once |
| Wi-Fi scan result and frame parser fuzz (ASan/UBSan; 6.48.4) | `tools/checks/wifi-fuzz-check.sh` | once |
| DNS reply reader fuzz, both kernels (ASan/UBSan; 6.48.5) | `tools/checks/dns-fuzz-check.sh` | once |
| App image check fuzz (ASan/UBSan, truncation+mutation, relocations applied; 6.51.0) | `tools/checks/uimg-fuzz-check.sh` | once |
| App signature check fuzz with the vendored BearSSL (ASan/UBSan; a throwaway key through tools/signapp.sh; cuts, mutations, a flipped image byte, another name; 6.54.0) and every store app in landing/apps signed by the shipped key (6.55.0) | `tools/checks/appsig-fuzz-check.sh` | once |
| HTTP/JSON/FAT16 parser fuzz (ASan/UBSan, truncation+mutation+nasties) | `tools/checks/parser-fuzz-check.sh` | once |
| Text-input bounds: json.c maxlen 0/1/truncation + auth const-time compare (ASan/UBSan) | `tools/checks/input-bounds-check.sh` | once |
| Password hashing, host harness: PBKDF2-HMAC-SHA256 vectors (RFC 7914, RFC 6070 for SHA-256), legacy record upgrade, USERS.TXT parse | `tools/checks/auth-check.sh` | once |
| Dock apps open/close from the pointer alone | `tools/checks/appclose-check.py` | retry |
| Esc on a bare desktop does not quit the GUI; Mail still opens from the dock | `tools/checks/esc-desktop-check.py` | retry |
| Dock hover survives mid-animation | `tools/checks/dockhover-check.py` | retry |
| Launchpad tile click launches, doesn't just close the folder | `tools/checks/launchpad-click-check.py` | retry |
| Launchpad exits to the desktop: Esc, empty click, Apps tile again; web demo forwards Esc | `tools/checks/launchpad-exit-check.py` | retry |
| Apps folder layout (no black band, no row spill, no ghost icons) | `tools/checks/appsfolder-layout-check.py` | retry |
| Menu bar present after Launchpad, every app open and close, Esc, panels, drags; screen never black | `tools/checks/menubar-persist-check.py` | retry |
| Launchpad centered, evenly padded, icons 56px or smaller, labels clear (pixels, five screen sizes) | `tools/checks/launchpad-centered-check.py` | retry |
| Typography: baseline flatness, letter-gap variance, container padding | `tools/checks/baseline-check.py` | retry |
| Multi-window (click-to-focus, real z-order compositing) | `tools/checks/multiwindow-check.py` | retry |
| Ring-3 window: Reminders beside Notes, keys to the focused window only, a crash closes only its window | `tools/checks/ring3window-check.py` | retry |
| Ring-3 stress: window task and desktop hammer heap and FAT at once, heap walk ok, file at root, desktop alive | `tools/checks/ring3stress-check.py` | retry |
| Ring-3 PDE sync: kernel page table born after the window task is visible on its CR3, close clean, desktop alive | `tools/checks/pdesync-check.py` | retry |
| Window snapping (title-bar drag to edge/corner, real pixel proof) | `tools/checks/windowsnap-check.py` | retry |
| ping on the Pi (13.8.0): the gateway answers three echoes, an address nobody holds gets no reply | `tools/checks/ping-check.py` | once |
| Notices from apps (13.7.0): one shows under its app's name, Focus holds it and says how many, in headless QEMU | `tools/checks/arm64-notify-check.py` | once |
| A disk kept from an older kernel gets this kernel's app: a changed KEYRATE.BIN is replaced at launch and an unchanged one is left alone | `tools/checks/ring3seed-stale-check.py` | retry |
| man NAME (13.6.0): every Pi Terminal command has a page, man cat prints it, an unknown name is refused, in headless QEMU | `tools/checks/arm64-man-check.py` | once |
| lldb stops both kernels at their entry through QEMU's gdb stub (skips with no lldb) | `tools/checks/debugger-check.py` | once |
| Ring-3 window resize: snap Notes to a quarter, JT_EV_RESIZE answered, new buffer mapped, app redrew at the quarter size (serial marker + far-corner pixels) | `tools/checks/ring3resize-check.py` | retry |
| Windows drag live by their title bar (single-window Notes and multi-window Files) | `tools/checks/windowdrag-check.py` | retry |
| Windowed apps start under the title bar, Calendar fits six weeks | `tools/checks/apptop-check.py` | retry |
| Calendar Day, Week, Month and Year views (ring-3 program) | `tools/checks/calviews-check.py` | retry |
| QA gallery: every app opens, screenshots, closes, no crash | `tools/checks/qa-gallery.py` | retry |
| Every app's main action, headless | `tools/checks/feature-drive.py` | retry |
| Dock icon edge quality (no staircased corners) | `tools/checks/iconedge-check.py` | retry |
| Dock icon halo (clean clip to the tray, no glyph bleed) | `tools/checks/iconhalo-check.py` | retry |
| Dock tiles draw the colour their authored SVG states (incl. Samantha) | `tools/checks/iconart-check.py` | retry |
| Dock icon lighting (one soft top light, top highlight, no dark outline) | `tools/checks/iconlight-check.py` | retry |
| Every authored icon shares one tile silhouette, AA edges, glyph margin | `tools/checks/iconinset-check.py` | once |
| Calendar dock tile is a page-and-grid picture, no date or dash, same on every date | `tools/checks/calicon-check.py` | retry |
| Clock icon is a live analog face: hands follow the RTC and redraw on the minute | `tools/checks/clockicon-check.py` | retry |
| Shadow under the dock darkens the photo, no flat bands | `tools/checks/dockband-check.py` | retry |
| Titlebar traffic-light AA (real coverage blend, not binary) | `tools/checks/titlebar-aa-check.py` | retry |
| Dock tray corner AA (real coverage blend, not binary) | `tools/checks/traycorner-check.py` | retry |
| Portfolio catalog opens, lists the fleet, and the list scrolls | `tools/checks/portfolio-check.py` | retry |
| Asking Samantha for the weather fetches it, no "open Weather first" | `tools/checks/samweather-check.py` | retry |
| Menu bar mark AA (no false interior seams) on its terracotta ground | `tools/checks/bootlogo-check.py` | retry |
| Boot splash draws the brand glyph, not the old crayon or stick tree | `tools/checks/bootmark-check.py` | retry |
| Boot splash draws the tree on stroke by stroke, never pops in whole, terracotta ground last | `tools/checks/bootanim-check.py` | retry |
| Landing eyebrow tracks roadmap Latest, H1 stays the brand line | `tools/checks/landing-headline-check.sh` | once |
| Landing app wall: clips load early, sit over their stills and fade in only when playing (no flash on scroll) | `tools/checks/landing-clip-still-check.py` | once |
| Idle tour still cycles all 8 real dock apps | `tools/checks/tourappcount-check.mjs` | once |
| Idle tour autoplay fix is in place (tourArmed reset) | `tools/checks/idletour-arm-reset-check.mjs` | once |
| RTC local-time shift math (v86's CMOS answers in UTC) | `tools/checks/rtc-timezone-check.mjs` | once |
| Stocks opens without a supported network card | `tools/checks/stocks-dock-check.py` | retry |
| Stocks chart line is antialiased (coverage blend, no stair-stepping) | `tools/checks/stocks-aa-check.py` | retry |
| Stocks fills all five watchlist rows from a full-size stub Worker reply (fails if the app reads only part of STOCKS.TXT) | `tools/checks/ring3stocks-list-check.py` | retry |
| Stocks live quotes and kernel parsing | `tools/checks/stocks-live-check.mjs` | once |
| Epiphany command bar: AAPL GP draws the chart, an unknown code errors cleanly | `tools/checks/epiphany-cmdbar-check.py` | retry |
| Worker /api/proxy allowlist | `tools/checks/worker-proxy-check.mjs` | once |
| Worker /api/weather: WEATHER.TXT's lines for the caller's place, an ASCII city, have 0 when it cannot | `tools/checks/worker-weather-check.mjs` | once |
| Worker /api/waitlist store, validate, count | `tools/checks/waitlist-check.mjs` | once |
| Mail Send: Worker /api/mail/send guards and Resend shape, kernel bearer wiring | `tools/checks/mailsend-check.py` | once |
| Worker /api/proxy: a silent upstream cannot hang the guest (weather/chat freeze regression) | `tools/checks/weatherproxy-hang-check.mjs` | once |
| Worker /api/listen: Whisper transcription, 503 without the AI binding, rejects oversize/empty, per-IP rate limit | `tools/checks/listen-worker-check.mjs` | once |
| Every app opens and closes by keyboard alone | `tools/checks/keyboard-only-check.py` | retry |
| Soak: every app opened and closed once each in one boot, no leak, no crash | `tools/checks/soak-check.py` | once |
| Accounts: legacy record upgrades to PBKDF2 on login, create, reboot, login, wrong and empty passwords rejected, change password | `tools/checks/auth-flow-check.py` | retry |
| Entropy pool: serial names its sources, two boots draw different salts | `tools/checks/entropy-check.py` | retry |
| Panic screen: a ring-0 fault paints the reason, not a frozen desktop | `tools/checks/panic-check.py` | once |
| Crash report names the faulting function, not just the exception kind | `tools/checks/panic-symbols-check.py` | once |
| Frame time: idle, dock hover and window open stay within budget | `tools/checks/frametime-check.py` | once |
| Every check in tools/checks/ is in this manifest or says why not | `tools/checks/suite-coverage-check.sh` | once |
| Text selection in Notes: Shift-arrow/Ctrl+A highlight, edsel/edcopy/edcut markers, selection-aware copy/cut/paste/delete | `tools/checks/textselect-check.py` | retry |
| Window top edge and corner arc are one continuous AA shape | `tools/checks/windowedge-check.py` | retry |
| Sound Blaster 16 detects, beep plays a real 440Hz tone, card-less boot is a no-op | `tools/checks/sb16-check.py` | retry |
| Music runs as a ring-3 app: plays a fixture WAV off a FAT disk (position from the driver, pause holds, bar and arrow seeks land, next changes the song, oversize file refused) and the sound card hears the tones | `tools/checks/music-check.py` | retry |
| Music QA: a 3:25 MP3 reads 3:25, a damaged MP3 frame no longer hangs the song, an unplayable song is skipped when one ends into the next and n moves on from it, the queue stays under half a second ahead, shuffle plays each song once, the wheel scrolls the list, no row sits under the key hint, a half-screen window keeps its pills, an empty library is quiet | `tools/checks/music-qa-check.py` | retry |
| Sound Blaster 16 record path: `listen` reaches the driver and times out cleanly (QEMU has no ADC backend), card-less boot is a no-op | `tools/checks/sb16-record-check.py` | retry |
| Chat speaks: say fetches /api/speak PCM from a stub and plays a real 1000Hz tone | `tools/checks/chat-speaks-check.py` | retry |
| Chat face: idle frame before, talk frames while she speaks, idle after; no frames means no face | `tools/checks/chat-face-check.py` | retry |
| HTTP stress: a ring-3 app makes 72 sequential SYS_HTTP_GET calls, all succeed (none -EIO), heap free stays at baseline | `tools/checks/httpstress-check.py` | retry |
| Burrow view switcher: List/Icons choice is saved to BURROW.TXT and a fresh run reads it back | `tools/checks/filesview-check.py` | retry |
| Demo canvas fills its frame, pixelated only at an exact 1:1 map | `tools/checks/democrisp-check.mjs` | retry |
| Landing hero: a real portrait before any click (pixels, undistorted), default boot opens Samantha's face, Esc reaches the desktop, ?desktop opts out, phone and tablet hold; fails if the poster is removed | `tools/checks/hero-poster-check.mjs` | retry |
| Landing hero: Click or Tap to boot lifts the poster onto her face well before the 60 s fallback, even after the serial log is trimmed | `tools/checks/landing-poster-click-check.mjs` | once |
| Landing page never overflows horizontally at phone widths | `tools/checks/mobile-overflow-check.mjs` | once |
| Landing opens on screenshots, the live demo is at ?demo | `tools/checks/landing-shots-hero-check.mjs` | once |
| Landing: benchmark labels clear, one app count, See the desktop link styled, sections visible on load | `tools/checks/landing-layout-check.mjs` | once |
| Portfolio voice: on by default, mute button top right, remembered across a reload, absent outside portfolio | `tools/checks/portfolio-mute-check.mjs` | once |
| Portfolio tour starts even when his face frames are slow and the first Escape presses are lost: his chat exits and Epiphany opens | `tools/checks/portfolio-slowframes-check.mjs` | retry |
| Landing demo on a phone: the chat bar raises the keyboard, typed letters reach Samantha once each and Send is Enter | `tools/checks/mobile-type-check.mjs` | once |
| Type system: every landing font size on the 9-step scale, line heights on the 4px grid, token block and ARCHITECTURE.md current, Tree Sans files load | `tools/checks/type-grid-check.py` | once |
| Landing demo: no keyboard trap, one main landmark, Full screen top right on phones | `tools/checks/landing-demo-ui-check.mjs` | once |
| Landing on a phone: exactly one visible "Message Samantha" input, focusable and typeable | `tools/checks/phone-one-input-check.mjs` | once |
| Landing on a desktop: her input is one sharp page field over her face, focusable and typeable | `tools/checks/desktop-pagebar-check.mjs` | once |
| App switcher: Ctrl+Tab cycles open windows and focuses the highlighted one | `tools/checks/appswitcher-check.py` | retry |
| Screenshot key: Ctrl+Shift+3 saves a real framebuffer BMP, numbered and visible in Files | `tools/checks/screenshot-check.py` | retry |
| Drunk mode easter egg: horizontal sway applied to framebuffer rows | `tools/checks/drunk-mode-check.py` | retry |
| Mac speech to text: stt.py turns a spoken sentence (macOS say, 16 kHz PCM) into words through whisper.cpp, and refuses a wrong token, an odd body and a GET (skips off a Mac) | `tools/checks/stt-check.py` | retry |
| Brand tokens: docs/brand/tokens.css matches the kernel's desktop colours, the accent, the brand page links it and the map reads it | `tools/checks/brand-tokens-check.py` | retry |
| ARM64: the aarch64 kernel boots under QEMU and prints over the UART (skips where the tools are missing) | `tools/checks/arm64-m0-check.py` | retry |
| x86-64 slice 1 (13.18.0): the 64-bit kernel boots in qemu-system-x86_64 into long mode, maps 4 GiB, prints on the serial port and a 1024x768 screen; a CPU without long mode is refused in words | `tools/checks/x86_64-m0-check.py` | retry |
| ARM64: a tiny local language model answers under QEMU (skips where the tools are missing) | `tools/checks/arm64-llm-check.py` | retry |
| x86-64 slice 2 (13.22.0): the IDT loads, the PIC and PIT tick at 100 Hz for 20 ticks, and IRQ1 reads the keyboard (a is scancode 0x1e, b is 0x30) on the serial port | `tools/checks/x86_64-irq-check.py` | retry |
| x86-64 slice 3 (13.23.0): the multiboot memory map gives 200 to 256 MB under -m 256, the frame allocator and kmalloc pass a self-test, and QMP mouse moves draw a white cursor, print its position on the serial port and clamp at the screen edge | `tools/checks/x86_64-mem-check.py` | retry |
| ARM64: with the relay out of reach, Samantha answers from the hosted Turing over HTTPS (skips where the tools are missing) | `tools/checks/arm64-sam-hosted-check.py` | retry |
| ARM64 M1c: the aarch64 kernel draws a desktop into a ramfb framebuffer and QEMU screendump shows it (skips where the tools are missing) | `tools/checks/arm64-m1c-check.py` | retry |
| ARM64 M2: the aarch64 kernel drives virtio disk, network, keyboard and mouse: a sector read back, a real ARP answer, key presses, moves and clicks (skips where the tools are missing) | `tools/checks/arm64-m2-check.py` | retry |
| ARM64 console scrollback: Page Up, End and Home scroll the on-screen Console over the whole boot log, the title bar says which lines (skips where the tools are missing) | `tools/checks/arm64-console-scroll-check.py` | retry |
| ARM64 net: the shared IP stack (drivers/net.c, drivers/http.c) on virtio-net leases 10.0.2.15 by DHCP and POSTs to a host server, 200 and the exact reply length (skips where the tools are missing) | `tools/checks/arm64-net-check.py` | retry |
| ARM64 HTTPS: a TLS 1.2 page from a host server with a throwaway certificate built in as a trust anchor, refused (tls FAIL 62) when it is left out (skips where the tools are missing) | `tools/checks/arm64-tls-check.py` | retry |
| ARM64 browser: browse URL typed at the Terminal's ask> row reads an HTTPS page through a redirect, prints readable text with numbered links, open N follows one (its connection resuming the TLS session, 6.45.0), a redirect loop stops (skips where the tools are missing) | `tools/checks/arm64-browser-check.py` | retry |
| Real pages on the Pi: the browser reads a 2 MiB chunked gzip page and a 300 KiB plain one whole off a host server, asks for HTTP/1.1 and gzip, and refuses a chunk cut short, in headless QEMU | `tools/checks/arm64-realpages-check.py` | retry |
| ARM64 browser, dead server: a host that accepts and never answers, and a closed port, each end in one timeout line (tls, reply, connect) within 12 s with dns ok and tcp ok printed first, and the prompt still answers (skips where the tools are missing) | `tools/checks/arm64-browser-deadserver-check.py` | retry |
| ARM64 Browser window: Spotlight opens it, the address bar loads a page from a stub server, a click on the link line follows it, Alt+Left, Alt+Right and F5 navigate, a long page scrolls and stops at its end, a dead port says so in plain words, Esc closes; accent link colour and off-white page in a screendump (skips where the tools are missing) | `tools/checks/arm64-browser-app-check.py` | retry |
| Page styles in Blackcomb: h1 and h2 drawn as big bold headings, h3 to h6 bold, <b> runs and font-weight rules (6.34.0) bold and only them, centred lines (6.35.0), right-aligned ones (6.44.0), descendant bold and align rules (6.47.0), taller ink for the big heading on a screendump, and the Terminal prints the page without style bytes, in headless QEMU | `tools/checks/arm64-pagestyle-check.py` | retry |
| Inline images in Blackcomb: a page's PNG and baseline JPEG are fetched after its text, scaled to fit, drawn in place with their colours on a screendump, and a missing one says it failed, in headless QEMU | `tools/checks/arm64-images-check.py` | retry |
| Forms and cookies on the Pi: a login form posts with its hidden token, the redirect comes back with the session cookie and not a Secure one over http, a GET form sends its query, fill never prints a value, and the Blackcomb window does it with Tab and Enter, in headless QEMU | `tools/checks/arm64-forms-check.py` | retry |
| Form controls on the Pi: checkboxes tick, radios pick one of their name, a select steps or takes an option by value or label, the form sends exactly the ticked and picked values, and Blackcomb changes a control in place, in headless QEMU | `tools/checks/arm64-formctl-check.py` | retry |
| What a page hides stays hidden: simple display:none rules from <style>, the hidden attribute and inline display:none skip an element whole, two-part descendant rules too (6.33.0) and child rules (6.49.0) and three-part rules (6.50.0), @media rules are left alone, and a hidden menu's link takes no number, in headless QEMU | `tools/checks/arm64-csshide-check.py` | retry |
| Cookies kept per person: a persistent cookie survives a restart in its owner's keychain, a session cookie does not, another account gets none, cookies clear is kept, and no value is on the card in plain text or on the UART, over four boots of one card in headless QEMU | `tools/checks/arm64-cookiekeep-check.py` | retry |
| History you can search: three pages visited (one twice) are listed newest first once each, found by every word in any case, ranked by visits, found from Blackcomb's address bar as you type (6.38.0), and cleared, in headless QEMU | `tools/checks/arm64-history-check.py` | retry |
| Tabs on the Pi: four at most, each with its own page and history, from the Terminal's tab commands and Blackcomb's Cmd+T, Ctrl+Tab, Cmd+W and tab strip (6.39.0) (the last tab's Cmd+W closes the window), in headless QEMU | `tools/checks/arm64-tabs-check.py` | retry |
| Downloads on the Pi: a link to a PDF is saved to JT/DOWNLOAD under an 8.3 name, never over a file (a hostile /wifi.txt leaves the card's own WIFI.TXT) with the page left in place, fetch URL saves any reply, a 404 is not saved, Blackcomb's status line says where, and the card image holds the exact bytes, in headless QEMU | `tools/checks/arm64-downloads-check.py` | retry |
| Mail on the Pi: IMAP over TLS against a fixture server, the app password hidden and refused when wrong, the newest messages listed, plain, quoted-printable HTML and base64 bodies shown whole past a fake tagged ending, a mail's image never fetched, mail logout forgets, in headless QEMU | `tools/checks/arm64-mail-check.py` | retry |
| Mail as pages in Blackcomb: mail: before a login says how to set it up, the inbox as links with a New message form, a message with a Reply form, Reply and Send through STARTTLS SMTP, multi-line bodies (6.40.0), and mail: typed in the window's address bar, in headless QEMU | `tools/checks/arm64-mailpages-check.py` | retry |
| Sending mail on the Pi: SMTP submission only after STARTTLS (a server without it is refused before any AUTH), AUTH PLAIN with the app password, dot-stuffing, and mail reply to the listed sender, in headless QEMU | `tools/checks/arm64-smtp-check.py` | retry |
| The keychain on the Pi: the mail sign-in sealed in ann's home under her login password, kept across a restart and a passwd, not bob's, and a flipped byte opens nothing, over five boots of one card in headless QEMU | `tools/checks/arm64-keychain-check.py` | retry |
| Picking Wi-Fi on the Pi: wifi says what it joins and heard, wifi join NAME takes a passphrase with stars, refuses a short one, and saves JT/WIFI.TXT as the name and exactly Python's PMK, never the passphrase, in headless QEMU | `tools/checks/arm64-wifi-join-check.py` | retry |
| Wi-Fi in Settings: the seventh section lists made-up heard networks (WIFI_FAKE build), Up and Down pick, the password field keeps key lines off the UART, and Enter saves JT/WIFI.TXT as Python's PMK, in headless QEMU | `tools/checks/arm64-wifi-pane-check.py` | retry |
| Calendar subscriptions on the Pi: calendar subscribe reads a fixture iCalendar feed into SUBS.TXT (folded, escaped, same-day events joined, a non-feed refused), Calendar loads the days the person has no event on, and EVENTS.TXT is untouched, in headless QEMU | `tools/checks/arm64-ics-check.py` | retry |
| Two-way CalDAV on the Pi: https only, the app password with stars, a wrong one refused, the person's day PUT as jt-YYYYMMDD.ics with its UID, the calendar REPORTed back into SUBS.TXT with XML and CDATA undone, deletes both ways (6.41.0), the calendar discovered from the server root (6.42.0), a sync on opening Calendar (6.43.0), a choice of calendars that never deletes across them (6.48.0), in headless QEMU against a fixture server | `tools/checks/arm64-caldav-check.py` | retry |
| ARM64 Browser bookmarks and Top Sites (6.37.0): the star in the address bar (click and Ctrl+D) saves the open page as a line of JT/BOOKMARK.TXT, read back off the card image; a fresh boot lists the saved pages on the empty Browser, Tab then Enter opens one, a star on a saved page takes it off the card (skips where the tools are missing) | `tools/checks/arm64-bookmarks-check.py` | retry |
| YouTube in Blackcomb: yt.py (a saved yt-dlp reply) answers a video page only with the token and an id, and a youtu.be address in Blackcomb shows the video's title with the token kept off the UART, in headless QEMU | `tools/checks/arm64-youtube-check.py` | once |
| ARM64 audio: `beep` fills the ring with a tone whose checksum matches Python, the PWM FIFO gets it in order on a fake Pi, stuck hardware gives up with one line, GPIO 42 and the boot path stay untouched (QEMU part skips where the tools are missing; the real jack is unverified) | `tools/checks/arm64-audio-check.py` | once |
| ARM64 volume: `vol 50` halves the ring sum of a beep against `vol 100`, `vol 0` is a silent ring, F11/F12 and the Mac media keys step the level before any pane with a "Volume N" menu bar notice and a tick, never past 0 or 100 (QEMU, skips where the tools are missing; the real speaker is unverified) | `tools/checks/arm64-volume-check.py` | retry |
| ARM64 Samantha tile: a click on the Samantha dock tile opens the Terminal with `sam ` already typed on the ask> line, even with the Terminal already in front, and Enter runs `sam` (QEMU, skips where the tools are missing) | `tools/checks/arm64-samtile-check.py` | retry |
| ARM64 boot splash bar: the bar under the logo glides with boot, dozens of distinct fills and no jump over 5 percent, and the pixels on screen match (QEMU, skips where the tools are missing) | `tools/checks/arm64-splashbar-check.py` | retry |
| ARM64 green light: the Pi's activity LED is blinked through the firmware mailbox (pin 42), never at boot, on a wait that ends (static, no hardware) | `tools/checks/arm64-led-check.py` | once |
| Pi actions: an answer's [[note]], [[say]], [[led blink]], [[open]], [[browse]] (http/https only), [[calc]] and [[status]] lines are stripped and recorded by ask.c's real parser (compiled on the host), four per turn, unknown ones recorded to ignore, the rest stays as text, and the relay prompt names them all | `tools/checks/pi-actions-check.py` | once |
| Pi release image carries no relay token: pi-ask.o built without JT_WIFI_DEV=1 has none, the dev build does (host only) | `tools/checks/pi-release-notoken-check.sh` | once |
| ARM64 agent loop: against a fake relay, [[browse]] then a final answer with agent: step 1 and 2 logged, 4 actions per turn and 5 steps then step limit, Esc stops it, unknown actions logged and ignored (skips where the tools are missing) | `tools/checks/arm64-agent-check.py` | retry |
| ARM64 /model and /effort: two F3 sessions preserve separate settings, conversation and typing, block switching while busy; slash commands run on the Pi, model and effort reach a fake relay only when not default, a bad value is refused in one line and never sent, /status /help /clear (skips where the tools are missing) | `tools/checks/arm64-model-check.py` | retry |
| ARM64 Claude in the Terminal: the Console takes no input; `sam QUESTION` typed at the Terminal's joshua@tree % prompt reaches the real relay (stub claude, 127.0.0.1) over virtio-net and the answer prints as sam: in 53-column lines; after a stub Turing answers /status says Samantha; wrong token -401, no network and no token each say so (skips where the tools are missing) | `tools/checks/arm64-claude-console-check.py` | retry |
| ARM64 zsh shell: the Terminal prompt is joshua@tree %, an unknown word prints zsh: command not found, `sam QUESTION` returns to the prompt at once while a stub relay delays its answer, a command typed meanwhile runs, a second sam says still thinking, then the delayed sam: answer prints above the prompt | `tools/checks/arm64-zsh-check.py` | retry |
| ARM64 relay token gate: a 24-byte throwaway token is absent from kernel8.img with JT_WIFI_DEV unset and present with it set (skips where the tools are missing) | `tools/checks/arm64-token-gate-check.py` | retry |
| ARM64 M3a and M3b: an unprivileged EL0 program prints through a write syscall, exits, and a direct access to a kernel-only page faults while the kernel survives; then user/hello.c built for aarch64 runs over svc with the Joshua Tree numbers (skips where the tools are missing) | `tools/checks/arm64-m3-check.py` | retry |
| ARM64 M3c app loader: user/calendar.c (34 KB, relocated pointer tables, a data page, a zeroed pool) loads and runs at EL0 next to user/hello.c; its window call gets -ENODEV outside the scheduler and its own failure line, the probe finds the relocated pointer, a zero pool, writable data and TIME stored through (and refused into code), a store to its code and a branch into data, pool or stack each fault, a reload re-zeroes and re-relocates, TIME answers the fixed clock, `userrun calendar` opens its window and Esc closes it, `userrun hello` and the Terminal keep answering; M3d: the file calls refuse a kernel pointer, a read into kernel memory, a path with .. and huge lengths with errnos, and with the sd check's card as a virtio disk the unchanged hello.c reads JT/HELLO.TXT to exit 9 (skips where the tools are missing) | `tools/checks/arm64-m3c-check.py` | retry |
| Apps from the card (6.51.0): JT/APPS/HI.APP runs at EL0 with `userrun hi`, and tools/mkapp.sh builds a C file from outside the tree into SUN.APP that runs too (6.52.0), and Spotlight lists and opens it while TERMINAL.APP and CALENDAR.APP are refused their taken names (6.53.0), and the store's fortune and dice run as landing/apps serves them, dice's window rolling on Space (6.56.0); a bad relocation, one that rewrites the next, an image over 384 KiB and a bad name are refused, and the kernel carries on (skips where the tools are missing) | `tools/checks/arm64-cardapp-check.py` | retry |
| The debugger (7.2.0): `debug hi 6` in the Terminal steps a card app six instructions, each step with where it is, the instruction and the registers that changed, then its registers and the bytes at sp, and it runs on to its exit; an app that points sp and pc at the kernel's own page sees only ?? and faults while the kernel carries on; an app that exits with steps to spare is said to have ended, and the next userrun is not stepped (skips where the tools are missing) | `tools/checks/arm64-debug-check.py` | retry |
| App threads on four cores (8.11.0): on QEMU virt with -smp 4 three threads of a card app add to a shared count under a jt_mutex, the total is exact, the app ran on a core above 0 and `cores` counts app slices on cores 1 to 3; built with SMP_APPS_OFF the same app stays on core 0 (skips where the tools are missing) | `tools/checks/arm64-smp-threads-check.py` | retry |
| Threads (8.1.0): a card app's two threads add to a shared count under a jt_mutex and the total is exact, a count with no lock comes out short, the threads took turns in 10 ms slices, a thread outside the app's code or on a crooked stack is refused, and the same app with the lock taken out comes out short (skips where the tools are missing) | `tools/checks/arm64-threads-check.py` | retry |
| Four cores (8.2.0): on QEMU virt with -smp 4 cores 1 to 3 come up through PSCI, each with its own MPIDR, and `cores` in the Terminal shows each one's wake-up count climbing; with one CPU the boot says they are not there and carries on; the Pi image on raspi4b brings all three up through the spin table (skips where the tools are missing) | `tools/checks/arm64-smp-check.py` | retry |
| Pipes (8.3.0): `userrun gen` piped into `userrun wc` in the Terminal carries 600 lines, more than the 4 KiB pipe, into wc in order (count and checksum match), both exit 0; `userrun gen` piped into `userrun head` ends with gen getting -EPIPE and exiting 32 (skips where the tools are missing) | `tools/checks/arm64-pipe-check.py` | retry |
| Spawn and wait (8.4.0): a card app spawns kids with words and waits for each exit status, two at once in the other order, a fault reported as -1, -ENOENT for no such app and -ECHILD for a stranger's pid, every kid's line in the Terminal before the parent's summary (skips where the tools are missing) | `tools/checks/arm64-spawn-check.py` | retry |
| Sockets (8.5.0): a card app connects to a TCP server on the host, writes a line and reads the server's capitalised answer through fd 100, a write after close is -EBADF, a dead port is -ECONNREFUSED and a bad name -EHOSTUNREACH (skips where the tools are missing) | `tools/checks/arm64-sock-check.py` | retry |
| The desktop comes first (8.6.0): a single-threaded card app that computes for seconds without yielding is cut off at least ten times, finishes with the right sum, and `sched` says no app held the core longer than 60 ms in a turn (skips where the tools are missing) | `tools/checks/arm64-sched-check.py` | retry |
| Self-update (9.3.0): with a throwaway key and a stub release, update opens Software Update, a bad signature stages nothing, a good one stages KERNEL8.NEW, TRYBOOT.TXT and UPDATE.VER, the old kernel drops an update that never started and a kernel built as the new version commits it after 20 s (skips where the tools are missing) | `tools/checks/arm64-update-check.py` | retry |
| Languages (9.2.0): every word of twelve rows has a glyph and is read as characters, Settings' Language page draws all twelve names, Español is kept on the card across a reboot and the screen reader speaks Spanish, then Japanese (skips where the tools are missing) | `tools/checks/arm64-lang-check.py` | retry |
| Screen reader (9.1.0): Cmd+F5 turns it on, three quick arrows along the dock give three caption lines and only the resting one is spoken, and `reader off` in the Terminal stops it (skips where the tools are missing) | `tools/checks/arm64-reader-check.py` | retry |
| Touch screens (9.0.0): on QEMU virt a usb-tablet on qemu-xhci is read from its report descriptor as an absolute pointer, and taps open the Terminal and Samantha's tiles they land on (skips where the tools are missing) | `tools/checks/arm64-touch-check.py` | retry |
| The kit's screens (8.13.0): on QEMU's raspi4b the Pi image fills 800x480, 1024x600, 1280x800, 720x1280 and 1080p panels at their own pixels with the menu bar, a window and all 12 dock tiles (skips where the tools are missing) | `tools/checks/arm64-screens-check.py` | retry |
| Swap (8.12.0): a card app grows a 4 MiB heap through a 64-page pool, every page reads back its own stamp after going to JT/SWAP.BIN and back, a 128 KiB heap buffer round-trips through a file, and `mem` counts the pages out and in (skips where the tools are missing) | `tools/checks/arm64-swap-check.py` | retry |
| Demand paging (8.7.0): a card app asks for an 8 MiB heap and touches six pages, one first touched by the kernel storing through a pointer for it; what it wrote reads back, untouched and re-grown pages read 0, and the kernel counts exactly 6 heap pages, not 2048 (skips where the tools are missing) | `tools/checks/arm64-heap-check.py` | retry |
| The block cache (8.8.0): five repeated cats read nothing more from the card and the cache answers them; a file written just before QEMU is killed is on the card with its bytes; a second boot reads both files (skips where the tools are missing) | `tools/checks/arm64-diskcache-check.py` | retry |
| USB sticks (8.9.0): a FAT32 stick on QEMU's xHCI is found at boot, `stick ls` lists it, `stick cat` reads a file over USB and `stick copy` puts it in JT/ byte for byte | `tools/checks/arm64-stick-check.py` | retry |
| Signed apps (6.54.0): with a throwaway key built in and two accounts on the card, a signed app runs for a non-admin, an unsigned one only under an admin's sudo (and then in Spotlight), and one with a flipped byte, another key's signature or a copied name never, sudo or not (skips where the tools are missing) | `tools/checks/arm64-appsig-check.py` | retry |
| The app store (6.55.0): with a throwaway key and a stub store, install fetches a signed app into JT/APPS (byte for byte) and userrun runs it; a changed image, a missing signature, an app the store lacks and a built-in name are refused and nothing is written (skips where the tools are missing) | `tools/checks/arm64-store-check.py` | retry |
| Download-to-install (13.4.0): Blackcomb follows a link to a signed .app and installs it, refuses a changed or unsigned one, still downloads other files, and with accounts refuses a guest who is no admin, in headless QEMU | `tools/checks/arm64-browser-install-check.py` | once |
| Plank on the box (6.57.0): twelve of Plank's own examples (printing, floats, strings, lists, dicts, structs, enums, closures, optionals, try and catch) built for the Pi by tools/plankapp.sh over user/pklibc each print exactly their .out file when run from the card, and the store's Slide, written in Plank, opens its window, slides tiles and closes (6.58.0), and fib and fizzbuzz built by plankc, the compiler written in Plank, with no LLVM IR, print theirs too (6.62.0), as do primes and plankc's string, list, struct, optional, dict and error tests on its runtime written in Plank, against plank.py's output (6.63.0 to 6.68.0) (skips without Plank and uv; CI pins Plank on this shard) | `tools/checks/arm64-plank-check.py` | retry |
| plankc built by itself (stage 2) runs on the box through userrun's words and whole-file reads and writes, and compiles fib, then its own source, into exactly the assembly plank.py's build of plankc writes on the Mac: the fixed point (6.69.0, 6.70.0); then, with no clang or linker, plankc writes fib's app image (it runs) and its own, which compiles plankc again and writes the next image byte for byte the same (6.71.0) (skips without Plank and uv; CI pins Plank on this shard) | `tools/checks/arm64-plankc-self-check.py` | retry |
| Samantha writes an app (6.59.0): against a fake relay whose /api/build is relay.py's own build_app, her broken Plank fails and the compiler's words reach her next turn, her fix builds, lands in JT/APPS unsigned, opens and runs; an app nobody asked for is refused (skips without Plank and uv) | `tools/checks/arm64-samapp-check.py` | retry |
| Samantha's apps built on the Pi (6.94.0): plankc and its runtime on the card, the relay's /api/build answering 500, her broken Plank fails on the Pi and plankc's words reach her next turn, her fix builds on the Pi, opens and runs, and the Mac is never asked (skips without Plank and uv) | `tools/checks/arm64-sambuild-check.py` | retry |
| Code, the editor (6.98.0): on the Pi alone, plankc builds the editor from its source on the card, a program typed into it (indent, Backspace) is saved by Esc, plankc builds that and it runs (skips without Plank and uv) | `tools/checks/arm64-code-check.py` | retry |
| ARM64 M3c Calendar window: the Calendar tile runs user/calendar.c at EL0 in its own window titled Calendar, March 2026 drawn with today in its accent (caltest clock), Right steps the month, Esc closes it and the wallpaper and the Terminal come back; a test program that yields forever leaves Spotlight and the Terminal answering and its red dot ends it (skips where the tools are missing) | `tools/checks/arm64-calendar-check.py` | retry |
| ARM64 Weather: the Weather tile and Spotlight open the Pi window; the temperature, a sun glyph and five day cards are drawn from a reading fetched off a fixture server, and with no network it shows no reading, Esc and the red dot close it (skips where the tools are missing) | `tools/checks/arm64-weather-check.py` | retry |
| ARM64 M4 Wi-Fi proto: wifi_proto.h packs and parses SDPCM, BCDC, escan and NVRAM on the host clang | `tools/checks/wifi-host-check.sh` | retry |
| ARM64 Wi-Fi WPA2: wpa.h SHA-1, HMAC, the pairwise-key PRF and AES key unwrap match the RFC vectors | `tools/checks/wpa-test.c` | retry |
| ARM64 M4 Wi-Fi: the Pi image powers the chip, finds no SDIO card under QEMU, prints wifi FAIL cmd5 and the boot carries on, with and without the firmware files (skips where the tools are missing) | `tools/checks/arm64-wifi-check.py` | retry |
| ARM64 Bluetooth BT1 and BT2: HCI over a second UART against a fake controller and mouse: the patch records in file order, version, baud, LE Meta on, a scan listing the mouse by name, then bt pair with Just Works checked by the check's own AES (c1, s1), encryption, ACL flow control, GATT discovery and four boot reports moving the pointer onto a dock tile and clicking it; a wrong confirm, a silent or out-of-order radio each end in one plain line with the kernel up, a saved pairing reconnects on its own at boot (after a first try nobody answers) and again after the link drops, with bt status, and no key reaches the UART (skips where the tools are missing) | `tools/checks/arm64-bt-check.py` | retry |
| Classic Bluetooth (12.1): bt find lists headphones and keyboards from Inquiry by name and Class of Device, bt connect pairs with Secure Simple Pairing (Just Works) and saves the link key to JT/BTLINK.BIN, a later bt connect answers the Link Key Request with it, a key the device forgot is erased, the key never reaches the UART, and a classic keyboard opens HID control and interrupt over L2CAP in boot protocol and types (12.2), and headphones set up A2DP over AVDTP and play Music's song as SBC in RTP that ffmpeg decodes to its 440 Hz (13.0), against a fake radio whose opcodes come from Linux's hci.h (skips where the tools are missing) | `tools/checks/arm64-btclassic-check.py` | retry |
| Bluetooth saved pairing: btpair.h builds on the host and round-trips; bad magic, version, truncation, damage and a zero key are refused; no key in any printed string (host only) | `tools/checks/bt-pair-check.py` | once |
| ARM64 M4 SD card: the Pi image reads and writes a FAT32 SDHC card through EMMC2 at 0xFE340000 under QEMU: ls and cat through FAT32 chains with reads past the partition and the card refused; write, rm and mkdir in JT/ survive a reboot on the same card, config.txt, kernel8.img and the other boot files are refused and hash the same, a CMD25 lands, and mtools, a FAT pass and fsck.fat find the card clean; with no card it prints sd no card and boots on (skips where the tools are missing) | `tools/checks/arm64-sd-check.py` | retry |
| ARM64 Finder on the Pi: on QEMU virt with the check card as a virtio disk holding folders, a document and a song under JT/, Spotlight opens user/finder.c at EL0, it lists JT/ (folders first, accent folder glyphs, the selected row shaded), Enter goes into a folder and a subfolder, Backspace comes back up, Enter on a .TXT previews it read-only with its text on screen, Left and Down move through the sidebar, Esc closes it; with no card the Burrow entry opens it on the built-in apps and says so (skips where the tools are missing) | `tools/checks/arm64-finder-check.py` | retry |
| ARM64 M3d Notes on the Pi: on QEMU's raspi4b with the sd check's FAT32 card, made-up USB key reports walk the dock to Notes, the unchanged user/notes.c opens at EL0, a typed line is saved by Esc through open, write and close to JT/NOTES/NOTES/N0000001.TXT (mtools reads exactly the line, a FAT pass and fsck.fat find the card clean), and after a reboot on the same card Notes lists it pixel for pixel and opens it in the editor (skips where the tools are missing) | `tools/checks/arm64-notes-check.py` | retry |
| ARM64 Reminders on the Pi: on QEMU's raspi4b with the sd check's FAT32 card, made-up USB key reports walk the dock to Reminders (no longer the Console's "not on ARM yet"), user/remindpi.c opens at EL0, two adds and a tick land in JT/REMINDER.TXT in i386's line format with the done item sunk (mtools reads it, the card is clean), the terracotta done circle is on screen, after a reboot on the same card the item is still there and ticked, and with no card the window says memory only (skips where the tools are missing) | `tools/checks/arm64-reminders-check.py` | retry |
| ARM64 M3e Music on the Pi: on QEMU virt with the check card as a virtio disk holding JT/MUSIC/ALPHA.WAV and CHARLIE.MP3, Spotlight opens the unchanged user/music.c at EL0 and it lists both, Space plays the WAV with its clock following the frames the kernel fed and its ring sums matching the file exactly, n decodes the MP3 at EL0 with the FPU, F11 lowers the kernel volume and the WAV plays again with lower sums, Esc closes it and the Terminal answers; fpkeep keeps q0-q31, FPCR and FPSR across 40 parks and calls while the kernel wipes the FPU; on the host the Pi build's PWM1 DMA ring takes every queued byte in order (skips where the tools are missing; the real jack is unverified) | `tools/checks/arm64-music-check.py` | retry |
| ARM64 mascot on the empty Music and Books screens: with no songs and no books, each app's window carries the mascot's ink and terracotta above a plain caption in a screendump (skips where the tools are missing) | `tools/checks/arm64-mascot-empty-check.py` | retry |
| ARM64 M4 USB: the aarch64 kernel finds an xHCI controller behind a PCIe root port, enumerates a hub, a keyboard behind it and a mouse, and reads key presses, moves and clicks (skips where the tools are missing) | `tools/checks/arm64-usb-check.py` | retry |
| ARM64 mouse: a USB mouse behind a hub moves the shared arrow over the ARM desktop, the dock label follows it and leaves no ghost, the Console's close button and a dock click work, a hot-plugged mouse moves it, and the scale-2 arrow at 1080p (skips where the tools are missing) | `tools/checks/arm64-mouse-check.py` | retry |
| ARM64 window drag: the Console and the Browser follow a press on their title band (not the dots or the well) with their contents, leave flat wallpaper and the dock untouched, clamp whole between the menu bar and the dock, and the red dot still closes the moved window; the boot-time drag at 1080p (scale 2) (skips where the tools are missing) | `tools/checks/arm64-drag-check.py` | retry |
| ARM64 app windows at 1080p: Calendar, Notes, Music and Books each open at boot on the Pi image at scale 2, their frame is within 15% of the Console's size and a screen fraction and their content is not blank (skips where the tools are missing) | `tools/checks/arm64-appsize-check.py` | retry |
| ARM64 4.0 gate: Calendar, Toroid and Notes run at once on the Pi image, Toroid crashes on purpose, the kernel survives, its slot is freed and the other two keep presenting frames (skips where the tools are missing) | `tools/checks/arm64-windows-check.py` | retry |
| ARM64 three apps opened at boot on QEMU virt: the boot self-tests stand aside instead of loading over them, no kernel crash | `tools/checks/arm64-appboot-check.py` | retry |
| ARM64 watchdog: a frozen app is stopped after 3 s and the other apps keep running | `tools/checks/arm64-watchdog-check.py` | retry |
| ARM64 windows are sharp at 2x: Calendar, Notes, Music and Books draw at device size at 1080p (hi-res buffer, Tree Sans text through JT_SYS_TEXT2), and with the kernel-drawn Calculator, Clock, Brick, Browser, Terminal and Console every narrow text stem in a screendump has at most two soft pixels and two or more full ones, where a 1x picture smoothed up has three or four soft (skips where the tools are missing) | `tools/checks/arm64-sharp2x-check.py` | retry |
| ARM64 window drag and resize, every window: Console, Browser, Clock, Brick, Calculator and the ring-3 apps Calendar, Notes, Music and Books are dragged by the title band (dots, flat wallpaper behind, held under the menu bar), resized by the bottom-right corner and the right and bottom edges (12 px zone, grows by the pointer's move, minimum size, stops at the screen's edge and above the dock), lay their content out again, apps get JT_EV_RESIZE and answer with a second window open and a redrawn frame, and a Calendar dragged off a Terminal leaves it drawn whole (skips where the tools are missing) | `tools/checks/arm64-resize-check.py` | retry |
| ARM64 Mail over IMAP and TLS against a fixture server: login with a starred app password, list, plain, HTML and base64 messages, logout (skips where the tools are missing) | `tools/checks/arm64-mail-check.py` | retry |
| ARM64 close glitch at 1080p: after the Terminal, Calendar, Notes, Browser, Brick and Calculator each open and close (red dot, Esc, dragged first, dragged to the bottom, and the Terminal and Calendar stacked both ways) the screen is pixel for pixel the empty desktop except the arrow, and a plain 800x600 boot has no white sliver by the tribute line (skips where the tools are missing) | `tools/checks/arm64-closeglitch-check.py` | retry |
| ARM64 back buffer at 1080p: with Calendar, Toroid and Notes open, one pointer report drags Calendar and the screen changes only inside the union of its old and new rectangles, in one copy whose fb: flush line is that union, Calendar's pixels move whole, Toroid behind comes back where it was uncovered, then Toroid crashes on purpose and Calendar and Notes still answer clicks (skips where the tools are missing) | `tools/checks/arm64-dirtyrect-check.py` | retry |
| ARM64 presents at 1080p (13.9.3): Paddle's arrows move it and each present flushes only the pixels that changed, not the whole window, so a game no longer starves the keyboard (skips where the tools are missing) | `tools/checks/arm64-present-check.py` | retry |
| ARM64 Browser flicker: the BR_TRACE build hashes the screen's window rectangle before a Browser paint and again just before the copy, and every paint (open, load, Tab, scroll, address typing) must log offscreen with the screen unchanged, never paint direct (skips where the tools are missing) | `tools/checks/arm64-browser-flicker-check.py` | retry |
| ARM64 Browser pointer motion: 120 pointer reports across a long (hundreds of lines) Browser page on the BR_TRACE build must cause zero Browser paints and zero page layouts, leave the screen pixel for pixel as it was, and a page down must paint once without laying out again (skips where the tools are missing) | `tools/checks/arm64-browser-mouse-check.py` | retry |
| ARM64 focus: with the Terminal and the Calculator both open, a click focuses one and typed keys land only in it (the sum in the Calculator, digits on the Terminal's prompt row), the clicked window comes to the front, and closing the Calculator puts the wallpaper back (skips where the tools are missing) | `tools/checks/arm64-focus-check.py` | retry |
| ARM64 clipboard and Cmd keys: in Notes Cmd+A selects, Cmd+C copies into the one 4 KB kernel clipboard (SYS_CLIPBOARD 399); the Terminal's Cmd+V pastes it and Enter runs it, Cmd+X cuts a line, and Cmd+W closes the Terminal and then Notes (headless QEMU virt with a card, skips where the tools are missing) | `tools/checks/arm64-clipboard-check.py` | retry |
| ARM64 Launchpad: the Apps tile opens a centred cream card, seven across, of all 29 apps Spotlight lists with icon art and names, the one with no ARM app (Trash) greyed, no Calculator; arrows (stopping at the edges), Enter, Esc and an outside click work and restore the pixels under the card, a click on the Calculator cell opens it, Enter on Calendar starts it, and every name is found by Spotlight (skips where the tools are missing) | `tools/checks/arm64-launchpad-check.py` | retry |
| ARM64 Calculator: calc.c gets a table of 42 sums right on the host (sin(30deg)=0.5, 5!=120, 2^10=1024, 1/0 is an error), its keypad's memory and Deg keys work, and the calctest build types four sums at boot with the same answers on the UART and the window in a screendump (QEMU part skips where the tools are missing) | `tools/checks/arm64-calc-check.py` | retry |
| ARM64 keyboard only: with no mouse, Cmd+Space and F1 open Spotlight and a bare Space does not (drawn in the house colours), typing term and Enter opens the Terminal with the keyboard, Esc hands the keys back, Left and Right move the dock's label and Enter opens that tile (Calendar, which Esc closes), F2 opens the Terminal at once, a held key repeats 8 to 14 times and stops on release, Caps Lock capitalises letters and Shift flips it, and a build with the repeat disabled fails the check (skips where the tools are missing) | `tools/checks/arm64-keys-check.py` | retry |
| ARM64 Mac-style USB keyboard: over xHCI, Cmd+Space opens Spotlight, a plain letter on the bare desktop opens it with the letter typed, Esc with it closed does nothing, a 9-byte report with a report ID still gives Ctrl+T, and the key debug line shows only in the dev build (skips where the tools are missing) | `tools/checks/arm64-keydbg-check.py` | retry |
| ARM64 Calendar month/day digits, December rollover, Clock time/date and unset state, Spotlight open/close and Samantha launch (skips where the tools are missing) | `tools/checks/arm64-calicon-check.py` | retry |
| ARM64 crash screen: an unexpected EL1 fault prints class, ESR, FAR, ELR and the last console lines on the UART, draws them as a panel (QEMU screendump on virt and the Pi 4B model) and halts quietly (skips where the tools are missing) | `tools/checks/arm64-crash-check.py` | retry |
| ARM64 FP state: q0-q31, FPCR and FPSR survive timer interrupts whose handler wipes them, and the build without the save fails the same test (skips where the tools are missing) | `tools/checks/arm64-fp-check.py` | retry |
| ARM64 out of memory: a full heap prints oom fb or oom text and the kernel carries on (no screen, or the VGA fallback font) on virt and the Pi 4B model (skips where the tools are missing) | `tools/checks/arm64-oom-check.py` | retry |
| ARM64 boot health: the Pi image boots on QEMU's raspi4b with no FAIL line beyond the listed expected ones, no oom, no crash, and the desktop up (skips where the tools are missing) | `tools/checks/arm64-boot-health-check.py` | retry |
| ARM64 quiet boot: the Pi image's boot log window is never drawn, no dock label plays by itself (raspi4b at 1080p) | `tools/checks/arm64-boot-quiet-check.py` | retry |
| ARM64 quiet boot and Wi-Fi fan: no Console window at boot (flat wallpaper where it was), a dock click opens it with the buffered log, the fan is dimmed grey when not joined and flat ink when joined, never blue | `tools/checks/arm64-quietboot-check.py` | retry |
| ARM64 boot splash: neutral grey ground, no terracotta pixel, the tree mark crisp in ink with strong edge contrast, a grey bar | `tools/checks/arm64-splash-check.py` | retry |
| Pi card flasher: kernel, firmware and tools/pi-config.txt land on a stand-in card, and what was there is kept as .bak (skips where the tools are missing) | `tools/checks/flash-pi-check.sh` | once |
| ARM64 RNG200 full seeds, health faults, bounded waits and wiped failures (host harness) | `tools/checks/arm64-rng-check.py` | once |
| ARM64 clock accepts bounded HTTPS dates only, never before the build or an accepted clock (host harness) | `tools/checks/arm64-clock-check.py` | once |
| ARM64 Terminal asks the real relay over TLS, with session resume and wrong-token refusal | `tools/checks/arm64-claude-console-check.py` | retry |
| ARM64 relay discovery: with a dead baked relay address the Pi finds the relay by a LAN broadcast probe, verifies its jt-relay certificate from the private CA, and refuses a right-CA certificate for the wrong name before the token is sent (skips where the tools are missing) | `tools/checks/arm64-relay-discovery-check.py` | retry |
| ARM64 sam speed: sums answered on the Pi with no relay traffic, the relay remembered in memory and on the card (0 UDP probes after the first), a stalled relay stops at 30 seconds with the relay address, the timing line prints (skips where the tools are missing) | `tools/checks/arm64-samfast-check.py` | retry |
| ARM64 sam chat view: bare `sam` opens a full-window Samantha in the Terminal with a header, scrolling history and `you: ` input row; a sum answers on the Pi, a question shows a thinking marker and her reply from the stub relay, Up/Page/Home/End scroll, and Esc or /quit return to a working zsh prompt with the busy flag clear (skips where the tools are missing) | `tools/checks/arm64-samtui-check.py` | retry |
| ARM64 USB audio: a UAC1 speaker behind xHCI is configured and streamed 440 Hz over isochronous transfers, and the tone arrives intact | `tools/checks/arm64-usbaudio-check.py` | retry |
| ARM64 voice mode: double-tap and hold Space listens with a mic mark in the menu bar and a live level bar, the release posts the samples and the words reach sam, the taps leave no spaces, a quick double tap stays two spaces | `tools/checks/arm64-voicemode-check.py` | retry |
| ARM64 voice and face: [[say TEXT]] fetches Samantha's voice from the relay's /api/tts with the token and queues every sample into the Pi's sound ring, the chat view draws the mascot's face and its mouth moves with the clip, and a 413 leaves the caption with "voice: no audio" | `tools/checks/arm64-voice-check.py` | retry |

`retry` checks boot a VM and get one retry for host timing noise; `once` checks are pure and never retried.

### Apps

| App | Checks that name it |
|---|---|
| Files | `tools/checks/arm64-accounts-check.py`, `tools/checks/arm64-browser-install-check.py`, `tools/checks/arm64-cardkeys-check.py`, `tools/checks/arm64-diskcache-check.py`, `tools/checks/arm64-sd-check.py`, `tools/checks/arm64-snap-check.py`, `tools/checks/arm64-wifi-check.py`, `tools/checks/burrow-rename-check.py`, `tools/checks/fatcyclehang-check.sh`, `tools/checks/filesview-check.py`, `tools/checks/guide-nimbus-check.py`, `tools/checks/plankc-check.py`, `tools/checks/ramfs-demo-check.sh`, `tools/checks/read-long-files-check.py`, `tools/checks/relay-api-check.py`, `tools/checks/samantha-box-check.py`, `tools/checks/screenshot-check.py`, `tools/checks/type-grid-check.py`, `tools/checks/windowdrag-check.py` |
| Mail | `tools/checks/arm64-keychain-check.py`, `tools/checks/arm64-led-check.py`, `tools/checks/arm64-mail-check.py`, `tools/checks/arm64-mailpages-check.py`, `tools/checks/arm64-screen-idle-check.py`, `tools/checks/arm64-smtp-check.py`, `tools/checks/esc-desktop-check.py`, `tools/checks/mail-fuzz-check.sh`, `tools/checks/mailsend-check.py`, `tools/checks/mailtools-check.py`, `tools/checks/read-long-files-check.py` |
| Calendar | `tools/checks/apptop-check.py`, `tools/checks/arm64-appsize-check.py`, `tools/checks/arm64-caldav-check.py`, `tools/checks/arm64-calendar-check.py`, `tools/checks/arm64-calicon-check.py`, `tools/checks/arm64-cardapp-check.py`, `tools/checks/arm64-closeglitch-check.py`, `tools/checks/arm64-cmdtab-check.py`, `tools/checks/arm64-dirtyrect-check.py`, `tools/checks/arm64-genie-check.py`, `tools/checks/arm64-ics-check.py`, `tools/checks/arm64-keys-check.py`, `tools/checks/arm64-launchpad-check.py`, `tools/checks/arm64-m3c-check.py`, `tools/checks/arm64-resize-check.py`, `tools/checks/arm64-sharp2x-check.py`, `tools/checks/arm64-windows-check.py`, `tools/checks/calicon-check.py`, `tools/checks/calviews-check.py`, `tools/checks/check-calendar.sh`, `tools/checks/dockdots-check.py`, `tools/checks/ics-fuzz-check.sh`, `tools/checks/ring3calendar-check.py` |
| Notes | `tools/checks/arm64-appsize-check.py`, `tools/checks/arm64-clipboard-check.py`, `tools/checks/arm64-closeglitch-check.py`, `tools/checks/arm64-cmdtab-check.py`, `tools/checks/arm64-dirtyrect-check.py`, `tools/checks/arm64-notes-check.py`, `tools/checks/arm64-resize-check.py`, `tools/checks/arm64-sharp2x-check.py`, `tools/checks/arm64-windows-check.py`, `tools/checks/clipboard-check.py`, `tools/checks/dockdots-check.py`, `tools/checks/editor_qa.py`, `tools/checks/editorflash-check.sh`, `tools/checks/notes-autosave-check.py`, `tools/checks/notesfolders-check.py`, `tools/checks/notessharp-check.py`, `tools/checks/notestools-check.py`, `tools/checks/read-long-files-check.py`, `tools/checks/ring3resize-check.py`, `tools/checks/ring3window-check.py`, `tools/checks/textselect-check.py`, `tools/checks/touch-osk-check.py`, `tools/checks/windowdrag-check.py` |
| Reminders | `tools/checks/arm64-reminders-check.py`, `tools/checks/gui-prompt-keystroke-check.sh`, `tools/checks/notestools-check.py`, `tools/checks/ring3reminders-check.py`, `tools/checks/ring3window-check.py` |
| Terminal | `tools/checks/arm64-browser-check.py`, `tools/checks/arm64-calendar-check.py`, `tools/checks/arm64-cardapp-check.py`, `tools/checks/arm64-claude-console-check.py`, `tools/checks/arm64-clipboard-check.py`, `tools/checks/arm64-closeglitch-check.py`, `tools/checks/arm64-debug-check.py`, `tools/checks/arm64-focus-check.py`, `tools/checks/arm64-keys-check.py`, `tools/checks/arm64-m3c-check.py`, `tools/checks/arm64-man-check.py`, `tools/checks/arm64-music-check.py`, `tools/checks/arm64-nimble-check.py`, `tools/checks/arm64-pagestyle-check.py`, `tools/checks/arm64-pipe-check.py`, `tools/checks/arm64-reader-check.py`, `tools/checks/arm64-resize-check.py`, `tools/checks/arm64-samtile-check.py`, `tools/checks/arm64-samtui-check.py`, `tools/checks/arm64-sharp2x-check.py`, `tools/checks/arm64-shortcut-check.py`, `tools/checks/arm64-smp-check.py`, `tools/checks/arm64-spawn-check.py`, `tools/checks/arm64-tabs-check.py`, `tools/checks/arm64-touch-check.py`, `tools/checks/arm64-zsh-check.py`, `tools/checks/clipboard-check.py`, `tools/checks/dockdots-check.py`, `tools/checks/termmono-check.py`, `tools/checks/termsharp-check.py`, `tools/checks/utf8fold-host-check.sh` |
| Samantha | `tools/checks/arm64-calicon-check.py`, `tools/checks/arm64-claude-console-check.py`, `tools/checks/arm64-home-check.py`, `tools/checks/arm64-nimble-check.py`, `tools/checks/arm64-sam-hosted-check.py`, `tools/checks/arm64-samapp-check.py`, `tools/checks/arm64-sambuild-check.py`, `tools/checks/arm64-samtile-check.py`, `tools/checks/arm64-samtui-check.py`, `tools/checks/arm64-shortcut-check.py`, `tools/checks/arm64-touch-check.py`, `tools/checks/arm64-voice-check.py`, `tools/checks/burrow-rename-check.py`, `tools/checks/chat-samantha-check.py`, `tools/checks/chatapp-check.py`, `tools/checks/chattools-check.py`, `tools/checks/claude-relay-check.py`, `tools/checks/face-frames-check.py`, `tools/checks/hero-poster-check.mjs`, `tools/checks/iconart-check.py`, `tools/checks/mobile-type-check.mjs`, `tools/checks/phone-boot-check.py`, `tools/checks/phone-one-input-check.mjs`, `tools/checks/phone-samantha-back-check.py`, `tools/checks/relay-api-check.py`, `tools/checks/ring3calendar-check.py`, `tools/checks/samantha-boot-check.py`, `tools/checks/samantha-box-check.py`, `tools/checks/samantha-clock-check.py`, `tools/checks/samantha-fullscreen-check.py`, `tools/checks/samweather-check.py` |
| Weather | `tools/checks/arm64-weather-check.py`, `tools/checks/read-long-files-check.py`, `tools/checks/ring3weather-check.py`, `tools/checks/samweather-check.py`, `tools/checks/weatherproxy-hang-check.mjs`, `tools/checks/worker-weather-check.mjs` |
| Scrounge | `tools/checks/ring3curbfind-check.py` |
| Keyrate | `tools/checks/arm64-games-check.py`, `tools/checks/keyrate-offline-check.py`, `tools/checks/ring3app-check.py`, `tools/checks/ring3seed-stale-check.py` |
| Bookrank | `tools/checks/ring3bookrank-check.py`, `tools/checks/ring3bookrank-live-check.py` |
| Quotes | `tools/checks/arm64-stocks-check.py`, `tools/checks/ring3quotes-check.py`, `tools/checks/stocks-live-check.mjs` |
| Patois | `tools/checks/ring3tonchi-check.py`, `tools/checks/ring3tonchi-live-check.py` |
| Toroid | `tools/checks/arm64-dirtyrect-check.py`, `tools/checks/arm64-games-check.py`, `tools/checks/arm64-windows-check.py`, `tools/checks/ring3toroid-check.py` |
| Thinkpond | `tools/checks/ring3hikko-check.py`, `tools/checks/ring3hikko-live-check.py` |
| Fieldbook | `tools/checks/ring3fieldbook-check.py` |
| Contacts | `tools/checks/ring3contacts-check.py` |
| Calculator | `tools/checks/arm64-calc-check.py`, `tools/checks/arm64-closeglitch-check.py`, `tools/checks/arm64-focus-check.py`, `tools/checks/arm64-launchpad-check.py`, `tools/checks/arm64-resize-check.py`, `tools/checks/arm64-sharp2x-check.py`, `tools/checks/arm64-spotlight-check.py`, `tools/checks/ring3calc-check.py` |
| Stocks | `tools/checks/arm64-stocks-check.py`, `tools/checks/ring3stocks-list-check.py`, `tools/checks/stocks-aa-check.py`, `tools/checks/stocks-dock-check.py`, `tools/checks/stocks-live-check.mjs` |
| Search | `tools/checks/arm64-history-check.py`, `tools/checks/ring3search-check.py`, `tools/checks/spotify-check.py` |
| Epiphany | `tools/checks/epiphany-cmdbar-check.py`, `tools/checks/portfolio-slowframes-check.mjs`, `tools/checks/ring3epiphany-check.py` |
| Portfolio | `tools/checks/portfolio-check.py`, `tools/checks/portfolio-mute-check.mjs`, `tools/checks/portfolio-slowframes-check.mjs`, `tools/checks/ring3portfolio-check.py` |
| Activity | `tools/checks/arm64-led-check.py`, `tools/checks/panes-check.py`, `tools/checks/ring3activity-check.py` |
| Clock | `tools/checks/arm64-calendar-check.py`, `tools/checks/arm64-calicon-check.py`, `tools/checks/arm64-clock-check.py`, `tools/checks/arm64-m3c-check.py`, `tools/checks/arm64-music-check.py`, `tools/checks/arm64-resize-check.py`, `tools/checks/arm64-sharp2x-check.py`, `tools/checks/clock-check.py`, `tools/checks/clockicon-check.py`, `tools/checks/menuclock-check.sh`, `tools/checks/movie-check.py`, `tools/checks/ring3-paddle-check.py`, `tools/checks/ring3clock-check.py`, `tools/checks/ring3windgate-check.py`, `tools/checks/samantha-clock-check.py` |
| Music | `tools/checks/arm64-appsize-check.py`, `tools/checks/arm64-btclassic-check.py`, `tools/checks/arm64-mascot-empty-check.py`, `tools/checks/arm64-music-check.py`, `tools/checks/arm64-resize-check.py`, `tools/checks/arm64-sharp2x-check.py`, `tools/checks/music-check.py`, `tools/checks/music-qa-check.py` |
| Movies | `tools/checks/arm64-movies-check.py`, `tools/checks/movie-check.py` |
| Hamurapi | `tools/checks/arm64-games-check.py`, `tools/checks/ring3hamurabi-check.py` |
| Airwell | `tools/checks/ring3windgate-check.py` |
| Panes | `tools/checks/arm64-lang-check.py`, `tools/checks/panes-check.py` |
| Claude | `tools/checks/arm64-claude-console-check.py`, `tools/checks/claude-relay-check.py`, `tools/checks/relay-api-check.py`, `tools/checks/ring3claude-check.py` |
| Mines | `tools/checks/arm64-games-check.py`, `tools/checks/ring3mines-check.py` |
| Books | `tools/checks/arm64-appsize-check.py`, `tools/checks/arm64-mascot-empty-check.py`, `tools/checks/arm64-resize-check.py`, `tools/checks/arm64-sharp2x-check.py`, `tools/checks/books-check.py` |
| Solitaire | `tools/checks/arm64-games-check.py`, `tools/checks/ring3-solitaire-check.py` |
| Paddle | `tools/checks/arm64-games-check.py`, `tools/checks/arm64-present-check.py`, `tools/checks/games-sim-check.py`, `tools/checks/ring3-bricks-check.py`, `tools/checks/ring3-paddle-check.py` |
| Bricks | `tools/checks/arm64-games-check.py`, `tools/checks/games-sim-check.py`, `tools/checks/ring3-bricks-check.py` |
| Depths | `tools/checks/arm64-games-check.py`, `tools/checks/ring3-depths-check.py` |
| Snake | `tools/checks/arm64-games-check.py`, `tools/checks/ring3-snake-check.py` |
| Chess | `tools/checks/chess-core-check.py`, `tools/checks/ring3-chess-check.py` |
| Apps | `tools/checks/appclose-check.py`, `tools/checks/appsfolder-layout-check.py`, `tools/checks/appsig-fuzz-check.sh`, `tools/checks/appswitcher-check.py`, `tools/checks/apptop-check.py`, `tools/checks/arm64-appboot-check.py`, `tools/checks/arm64-appsig-check.py`, `tools/checks/arm64-appsize-check.py`, `tools/checks/arm64-cardapp-check.py`, `tools/checks/arm64-finder-check.py`, `tools/checks/arm64-launchpad-check.py`, `tools/checks/arm64-limits-check.py`, `tools/checks/arm64-notify-check.py`, `tools/checks/arm64-resize-check.py`, `tools/checks/arm64-samapp-check.py`, `tools/checks/arm64-sambuild-check.py`, `tools/checks/arm64-screen-idle-check.py`, `tools/checks/arm64-smp-threads-check.py`, `tools/checks/arm64-store-check.py`, `tools/checks/arm64-watchdog-check.py`, `tools/checks/launchpad-exit-check.py`, `tools/checks/mwdupetoolbar-check.sh`, `tools/checks/ring3crash-all-check.py`, `tools/checks/tourappcount-check.mjs`, `tools/checks/user-compress-check.py` |
| Trash | `tools/checks/arm64-launchpad-check.py` |
| Settings | `tools/checks/arm64-lang-check.py`, `tools/checks/arm64-model-check.py`, `tools/checks/arm64-scan-check.py`, `tools/checks/arm64-settings-check.py`, `tools/checks/arm64-spotlight-check.py`, `tools/checks/arm64-users-check.py`, `tools/checks/arm64-wifi-pane-check.py`, `tools/checks/location-check.py`, `tools/checks/settingsclick-check.sh`, `tools/checks/walldemo-regression-check.py` |

Apps with no check yet: none.
<!-- testing:end -->

</details>

## CI and deploy

How a change gets from a branch to joshuatree.jaybulb.com, on one page.

### The pipeline today

1. **Verify locally.** Every PR opens as a draft, and a draft runs no CI. You run `tools/ci-local.sh` on the Mac instead.
   - It builds `kernel.elf`.
   - It runs the eleven shards of `tools/checks/ci-suite.sh` in parallel QEMUs (the four ARM shards in turn).
   - Then it runs the check-refs scripts and the browser demo checks.
   - It mirrors `.github/workflows/check.yml` job for job, except the `network` job.
   - Only one suite runs at a time (`tools/ci-lock.sh`).
2. **Flip the PR ready.** `gh pr ready <N>` starts `.github/workflows/check.yml`. It runs on pull requests only, never on push. Its jobs:
   - `changes` decides what the PR touches. A prose-only PR skips the QEMU suite.
   - `suite` runs the eleven shards of `ci-suite.sh`. `demo` runs the Playwright landing checks.
   - `check` passes only if `changes`, `suite` and `demo` passed or were rightly skipped.
   - `check-refs` checks doc paths, the version sync and the VERSION bump.
   - `network` talks to real internet hosts. It is `continue-on-error`, so it reports but never blocks.
   - `ci` passes only if `check` and `check-refs` both passed.
3. **The gate.**
   - Branch protection on `main` requires `check`, `check-refs` and `network`. The branch must be strictly up to date, and no reviews are needed.
   - `network` can never fail. So in practice the gate is `check` plus `check-refs`.
   - The `ci` job says the same thing in one place, but it is not on the required list.
4. **Deploy.** Merging pushes to `main`.
   - `.github/workflows/deploy.yml` runs on any push to `main` that touches `landing/**`, `wrangler.toml`, `worker.js`, the kernel sources or a few generators.
   - It builds `kernel.elf` fresh, refreshes the landing facts, headline and roadmap card, and runs `wrangler deploy`.
   - Deploys queue and never cancel each other.
5. **Release.** A push that changes `VERSION` cuts a GitHub release through `.github/workflows/release.yml`.
6. **Smoke the live site.** `.github/workflows/live-smoke.yml` loads the live page. It checks that `/api/proxy` refuses a host off its allowlist.
   - It is manual (`workflow_dispatch`). Run it after a deploy finishes.
   - It does not run on push, because nothing guarantees the deploy is finished first.

## Benchmarks

Measured by `tools/bench.sh`; `--write` refreshes the block below, the README table and the landing page.

<!-- benchdoc:start -->
The kernel boots headless in QEMU on this Mac, runs `kernel/bench.c` and prints the numbers over serial. Run `tools/bench.sh` yourself; the numbers move with the host. Version 9.0.0, 2026-11-02.

| Benchmark | Result |
|---|---|
| boot_to_shell | 270 ms |
| heap_alloc_free | 225 ns/op |
| memcpy | 769 MB/s |
| context_switch | 3236 ns/switch |
| disk_read | 10253 KB/s |

boot_to_shell is timer ticks from the first interrupt to the shell prompt.
heap_alloc_free is one kmalloc plus one kfree, averaged over 20,000.
memcpy moves 256 KB blocks, 16 MB in all. context_switch is one direction
of a round trip to a task that only yields. disk_read is 128 KB of PIO
sectors from a fresh FAT16 image.
<!-- benchdoc:end -->
