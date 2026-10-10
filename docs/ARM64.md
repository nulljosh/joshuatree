# Joshua Tree on ARM64 (Raspberry Pi)

Status (2026-10-10): the desktop, USB keyboard, Wi-Fi and HTTPS fetches have run on Joshua's Raspberry Pi 4. The 2.34.0 development card boots; the 2.35.0 Calendar icon, Clock and Samantha dock launch are in PR #492, with focused QEMU checks and a full local suite pending. USB mouse, hardware RNG output and a real Samantha answer still need a board check.

If this is accepted it replaces the x86 board in `docs/HARDWARE.md` as the 3.0 reference. The OS stays free; the board is what we sell around it.

For the current app list, see [Apps currently implemented on ARM](RASPBERRY-PI.md#apps-currently-implemented-on-arm). The dock artwork is shared with i386; most of its apps are not ported yet.

## Why a Pi and not the Mac mini

The kernel is 32-bit x86. The Mac mini M4 is Apple Silicon: no UEFI, a custom boot chain, and USB, storage and sound behind Apple-only controllers. Bare-metal there means an Asahi-sized effort. A Raspberry Pi boots a plain `kernel8.img` from its firmware, has a documented framebuffer, UART and interrupt controller, and QEMU models parts of it. Start with a Pi 4B. The Pi 5 hangs every peripheral off the RP1 chip over PCIe, which makes first bring-up harder; move to it second.

## What the port touches

The kernel, drivers and lib are C, plus the user programs.

- Arch-specific and must be rewritten for AArch64: `boot/boot.S`, the linker script, GDT, IDT, PIC, ISR and IRQ stubs, paging, task switch, ring 3 entry and exit, and the 105 inline-asm blocks.
- PC-only drivers that go away and get Pi equivalents: ATA, PS/2 keyboard and mouse, VBE, vmmouse, PCI, RTL8139 and NE2000, Sound Blaster 16.
- Port I/O (`inb`, `outb`) shows up in 13 files and becomes memory-mapped I/O.
- Portable as is, in theory: the UI, fonts, JPEG and PNG decoders, FAT, the network stack above the NIC, the HTTP client, BearSSL, and the apps' C code. The user programs rebuild with an `svc` syscall ABI instead of `int 0x80`.

## Console scrollback

The on-screen Console is the only debug channel on a Pi with no serial cable, and the boot prints more lines than the window holds. Since 2.19.0 it keeps the last 16 KB of the log and a keyboard scrolls it: Page Up and Page Down by half a page, Home to the first line, End to the newest. The title bar reads "lines 12-27 of 61", and the bottom row of the window pins the newest `wifi` line and the newest `usb` line (key echoes excluded), cut to fit. New output while scrolled back is logged but does not move the view. `tools/checks/arm64-console-scroll-check.py` proves it with QEMU screendumps.

## Milestones, each one runs

1. **M0, serial boot: done.** `arch/arm64/start.S` reaches C and prints through the UART in QEMU and on the Pi. Check: `tools/checks/arm64-m0-check.py`.
2. **M1, desktop: implemented.** Memory mapping, timers, fonts, colour Satellite wallpaper, dock, cursor and Console scrollback are present. Clock and the readable Calendar icon are in PR #492; board verification remains. Checks: `tools/checks/arm64-m1c-check.py`, `tools/checks/arm64-calicon-check.py`.
3. **M2, input and network in QEMU: implemented.** Virtio keyboard, mouse, sector reads and the shared IP stack work. These are emulator drivers; they do not prove the Pi's physical devices. Checks: `tools/checks/arm64-m2-check.py`, `tools/checks/arm64-net-check.py`.
4. **M3, userland: started.** One EL0 test program can write and exit; touching a kernel-only page faults while the kernel survives. Separate app address spaces, scheduling and native app ports remain. Check: `tools/checks/arm64-m3-check.py`.
5. **M4, real Pi 4: started.** HDMI desktop, USB keyboard and Wi-Fi are working on the board. The xHCI mouse path is checked in QEMU and still needs a physical mouse test. SD writes and audio remain. Checks: `tools/checks/arm64-usb-check.py`, `tools/checks/arm64-wifi-check.py`; physical checks are recorded in [RASPBERRY-PI.md](RASPBERRY-PI.md).
6. **M5, Pi 5: future work.** Its RP1 peripheral controller needs a separate bring-up.

## Network stack

The ARM build runs the same IP stack as the i386 kernel. Not a copy: `drivers/net.c` (ARP, IPv4, UDP, DNS, TCP, DHCP) and `drivers/http.c` compile for both.

The only seam is `drivers/nic.h`, four calls: bring the card up, read its MAC, send a frame, poll for one. On QEMU the virtio-net driver in `arch/arm64/main.c` answers them. `arch/arm64/ip.c` adds a 100 Hz clock and, at boot, prints `net dhcp 10.0.2.15 gw 10.0.2.2`. Built with `make -C arch/arm64 NETPORT=8080` it also POSTs to the host and prints `net http 200 N bytes`. `tools/checks/arm64-net-check.py` proves both.

Next, in order: Wi-Fi joins a network below the same four calls (the CYW43455 driver). Then TCP that survives a lost segment, which a real radio needs and QEMU never tests. TLS comes later, through the BearSSL the i386 build already has.

## Claude in the Console

Since 2.21.0 there is a prompt. It lives on the bottom row of the Terminal window (its dock tile, or F1); the Console is logs only ([TERMINAL.md](TERMINAL.md)). The prompt names the model, shell style: `Claude Sonnet 5.5 $ `. The relay says which model answered (Haiku, Sonnet or Opus) and the prompt shows the last one. Before any answer, or when the relay cannot be reached, it shows the relay's default, `Claude Haiku 5.5 $ `. A build with no relay token shows `Claude $ `. Type a question, Backspace to fix it, Enter to send. The kernel posts it to the Claude relay on the Mac (`tools/claude-relay/relay.py`, see [CLAUDE-APP.md](CLAUDE-APP.md)) through the IP stack above, and prints the answer in the Terminal in lines of at most 53 columns. A follow-up keeps the conversation. Each outcome is one short line: `claude: thinking`, `claude: error -401` when the relay refuses the token, `claude: timeout`, `claude: no network` with no DHCP lease, `claude: no token` when the build had none. Typed keys show on the prompt row and their echo lines go to the serial port only, so the log stays readable.

The relay's address and token are set at build time, never in git: `CLAUDE_RELAY_HOST` (default 10.0.2.2), `CLAUDE_RELAY_PORT` (default 8765) and the token file named by `CLAUDE_RELAY_TOKEN_FILE` (default the same token file the Claude app uses). `arch/arm64/claude_cfg.sh` writes them into a gitignored header on every build. The token is never printed.

It works end to end on QEMU's virt machine with virtio-net: `tools/checks/arm64-claude-console-check.py` types a question with QMP keys and reads the answer back from a stub Claude. A real Pi has no network until Wi-Fi joins (stage 2 below); if joining fails, it says `claude: no network, Wi-Fi has not joined yet`. Once Wi-Fi joins, run the relay with `--lan` and build with `CLAUDE_RELAY_HOST` set to the Mac's LAN address.

## Risks

- USB through xHCI is the largest single driver and the first thing a real board needs.
- Sound on a Pi has no Sound Blaster equivalent; budget real time for it.
- The browser demo is i386 under v86 and stays that way. Two architectures means two builds and a second CI lane. Keep the i386 path green the whole time.
- `ring3` and task switching are where x86 assumptions are deepest. Read `docs/ARCHITECTURE.md` before M3.

## Errors and crashes

The tests for this section are `tools/checks/arm64-crash-check.py`, `arm64-fp-check.py`, `arm64-oom-check.py` and `arm64-boot-health-check.py`. Each drives a test build (`make -C arch/arm64 crashtest`, `fpsave`, `fpnosave`, `oomtest`) that the real kernel never contains.

- **A kernel fault.** An unexpected exception at EL1 prints `KERNEL CRASH: <class>`, the ESR, FAR and ELR and the last five console lines on the UART, draws the same text as a red panel on the screen, and sleeps in a `wfe` loop. It uses only the raw UART and the 8x16 VGA font, so it works if the fault came from the console or the text code, and the panel is cleaned out of the data cache so the real GPU shows it. A fault inside the crash code halts quietly. The deliberate EL0 faults of M3 are not crashes: they are printed and the kernel carries on.
- **Interrupts and floating point.** `irq_entry` saves q0-q31, FPCR and FPSR next to the general registers, so a handler can never corrupt the floating point work it interrupted. Today no handler uses floating point; the save makes that a promise nobody has to keep.
- **Out of memory.** `kmalloc` returns 0 when the bump heap is full and every caller checks. A failure prints `oom <where>` and ends only that step: no framebuffer means no screen but a working UART, and fonts that do not load mean the 8x16 VGA font. Open gap: one allocation in `xhci.c`.
- **A new failure cannot hide.** The boot health check fails on any `FAIL` or `oom` line the boot prints that is not on its short, commented list of things QEMU cannot do.
## M4 Wi-Fi
Stage 1 (2.16.0): the CYW43455 is alive and lists the networks on screen. `arch/arm64/wifi.c` brings up the SDIO host, loads the Cypress firmware (`brcmfmac43455-sdio.bin`, `.txt`, `.clm_blob` from RPi-Distro/firmware-nonfree at the commit pinned in `tools/wifi-fw.sh`, downloaded into `build/wifi-fw/`, never committed, `copyright` alongside), then prints `wifi ver`, `wifi mac` and one `wifi ap <rssi> ch<n> <ssid>` line per network. Lines in order on a real Pi: `wifi power on` (or `wifi power on (was on)`), `wifi sdio card rca 1`, `wifi f1 f2 up`, `wifi fw NNNk loaded`, `wifi fw ready`, `wifi ver`, `wifi mac`, `wifi ap ...`, `wifi scan done`. Any `wifi FAIL <step>` names the step that timed out and the desktop still comes up. Without the firmware files the build still links and prints `wifi no firmware`; QEMU has the SD host but no SDIO card, so it prints `wifi power on` then `wifi FAIL cmd5` and the boot carries on (`tools/checks/arm64-wifi-check.py`). A real board that prints `wifi FAIL cmd5` instead has a powered chip the host cannot see: check the GPIO 34-39 pin setup first.
Stage 2 (implemented, real-board join confirmed): join. Read `~/.config/joshuatree/wifi.conf` (fallback `arch/arm64/wifi.conf`, both gitignored, lines `ssid=`, `psk=`, `country=`) at build time into a generated object; never print the psk. Set `wsec` 4, `wpa_auth` 0x80, `wsec_pmk`, then `join` with the SSID (the firmware does the WPA2 handshake). SDPCM data frames carry the shared IP stack (`drivers/net.c`: ARP, DHCP, TCP), with `wifi ip 192.168.x.y` on screen. QEMU has no matching SDIO Wi-Fi chip; its missing-card path cannot validate a real WPA2 join.
