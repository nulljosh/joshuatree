# Joshua Tree on ARM64 (Raspberry Pi)

Status: M0, M1a, M1b and M1c (a framebuffer with a drawn desktop and the boot log written on it: ramfb on QEMU virt, the VideoCore mailbox on QEMU's Pi 4B model) run under QEMU with smooth DejaVu text on the screen, M2 has its first drivers, virtio keyboard, mouse, network and disk, and M3a runs the first program at EL0 with a write/exit syscall layer and a kernel-only page it cannot touch (updated 2026-10-04). The desktop booted on a real Pi 4 over HDMI on 2026-10-06. M4 has its USB keyboard and mouse driver, proven in QEMU; the next step is trying it on the board.

If this is accepted it replaces the x86 board in `docs/HARDWARE.md` as the 3.0 reference. The OS stays free; the board is what we sell around it.

## Why a Pi and not the Mac mini

The kernel is 32-bit x86. The Mac mini M4 is Apple Silicon: no UEFI, a custom boot chain, and USB, storage and sound behind Apple-only controllers. Bare-metal there means an Asahi-sized effort. A Raspberry Pi boots a plain `kernel8.img` from its firmware, has a documented framebuffer, UART and interrupt controller, and QEMU models parts of it. Start with a Pi 4B. The Pi 5 hangs every peripheral off the RP1 chip over PCIe, which makes first bring-up harder; move to it second.

## What the port touches

The kernel, drivers and lib are C, plus the user programs.

- Arch-specific and must be rewritten for AArch64: `boot/boot.S`, the linker script, GDT, IDT, PIC, ISR and IRQ stubs, paging, task switch, ring 3 entry and exit, and the 105 inline-asm blocks.
- PC-only drivers that go away and get Pi equivalents: ATA, PS/2 keyboard and mouse, VBE, vmmouse, PCI, RTL8139 and NE2000, Sound Blaster 16.
- Port I/O (`inb`, `outb`) shows up in 13 files and becomes memory-mapped I/O.
- Portable as is, in theory: the UI, fonts, JPEG and PNG decoders, FAT, the network stack above the NIC, the HTTP client, BearSSL, and the apps' C code. The user programs rebuild with an `svc` syscall ABI instead of `int 0x80`.

## Milestones, each one runs

1. **M0, serial hello. Done (`arch/arm64`, `tools/checks/arm64-m0-check.py`). Also builds for a real Pi 4 (`make -C arch/arm64 pi`), tried on QEMU's raspi4b model; the first boot on a real board is still to do, see [RASPBERRY-PI.md](RASPBERRY-PI.md).** `clang -target aarch64-none-elf` plus `ld.lld` (both installed here) build a kernel that prints on the PL011 UART under `qemu-system-aarch64 -machine virt`. Days.
2. **M1, a machine. M1a to M1c are done, M1d is half done** (exception vectors that print faults, the interrupt controller and a timer tick; a flat identity map with the MMU and both caches on, a bump heap; a framebuffer with a drawn desktop and boot log, plus smooth DejaVu text), on QEMU's virt machine and its Pi 4 model; `tools/checks/arm64-m0-check.py`, `tools/checks/arm64-m1c-check.py`. Still to do: the real window and dock drawing code on ARM instead of rectangles. Days to a couple of weeks.
3. **M2, input and net in QEMU. Started: keyboard, mouse, a disk that reads a real sector and a network card that gets a real ARP answer are done (`tools/checks/arm64-m2-check.py`), woken by GIC interrupts.** `virtio` keyboard, mouse, network and block drivers. With the HVF accelerator on the Mac mini this runs at native speed, far faster than today's i386 emulation, so the ARM build helps the browser demo too once v86 is not the only target. Weeks.
4. **M3, userland. Started: M3a is done (`arch/arm64/user.S`, `tools/checks/arm64-m3-check.py`): one EL0 program, `svc` with write and exit, user-only page permissions in the identity map, and a deliberate access to a kernel-only page that faults while the kernel survives. Still to do: per-window address spaces, scheduling more than one program, and the apps.** EL0 programs, the syscall layer, per-window address spaces, and all 46 apps rebuilt. Weeks.
5. **M4, a real Pi 4. Started: USB keyboard and mouse through xHCI work under QEMU, and PCIe, the VL805 firmware and its hub work on a real Pi 4 (hot-plug rescan and per-port status lines added in 2.13.2) (`arch/arm64/pci.c`, `arch/arm64/xhci.c`, `tools/checks/arm64-usb-check.py`), with the controller behind a PCIe root port and the keyboard behind a hub, as on the Pi. The Pi's own PCIe bring-up and the VL805 firmware load are written but untested on a board.** Firmware config, mailbox framebuffer, PL011, SD card through EMMC2, USB keyboard and mouse through xHCI, Ethernet through the Genet MAC. Sound last (HDMI or I2S, the hardest). Weeks.
6. **M5, the Pi 5.** RP1 over PCIe for every peripheral.

## Risks

- USB through xHCI is the largest single driver and the first thing a real board needs.
- Sound on a Pi has no Sound Blaster equivalent; budget real time for it.
- The browser demo is i386 under v86 and stays that way. Two architectures means two builds and a second CI lane. Keep the i386 path green the whole time.
- `ring3` and task switching are where x86 assumptions are deepest. Read `docs/ARCHITECTURE.md` before M3.

## First step

M0 is a half-day: new `arch/arm64/` with a boot stub, a linker script and a UART print, and a `make ARCH=arm64` target. Nothing in the i386 build changes.

## M4 Wi-Fi

Stage 1 (2.16.0): the CYW43455 is alive and lists the networks on screen. `arch/arm64/wifi.c` brings up the SDIO host, loads the Cypress firmware (`brcmfmac43455-sdio.bin`, `.txt`, `.clm_blob` from RPi-Distro/firmware-nonfree at the commit pinned in `tools/wifi-fw.sh`, downloaded into `build/wifi-fw/`, never committed, `copyright` alongside), then prints `wifi ver`, `wifi mac` and one `wifi ap <rssi> ch<n> <ssid>` line per network. Lines in order on a real Pi: `wifi power on` (or `wifi power on (was on)`), `wifi sdio card rca 1`, `wifi f1 f2 up`, `wifi fw NNNk loaded`, `wifi fw ready`, `wifi ver`, `wifi mac`, `wifi ap ...`, `wifi scan done`. Any `wifi FAIL <step>` names the step that timed out and the desktop still comes up. Without the firmware files the build still links and prints `wifi no firmware`; QEMU has the SD host but no SDIO card, so it prints `wifi power on` then `wifi FAIL cmd5` and the boot carries on (`tools/checks/arm64-wifi-check.py`). A real board that prints `wifi FAIL cmd5` instead has a powered chip the host cannot see: check the GPIO 34-39 pin setup first.

Stage 2 (next): join. Read `~/.config/joshuatree/wifi.conf` (fallback `arch/arm64/wifi.conf`, both gitignored, lines `ssid=`, `psk=`, `country=`) at build time into a generated object; never print the psk. Set `wsec` 4, `wpa_auth` 0x80, `wsec_pmk`, then `join` with the SSID (the firmware does the WPA2 handshake). Then SDPCM data frames in and out, the i386 IP stack (`drivers/net.c`: ARP, DHCP, TCP) on top, and `wifi ip 192.168.x.y` on screen.
