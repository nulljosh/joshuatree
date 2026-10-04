# Joshua Tree on ARM64 (Raspberry Pi)

Status: M0, M1a, M1b and M1c (a framebuffer with a drawn desktop: ramfb on QEMU virt, the VideoCore mailbox on QEMU's Pi 4B model) run under QEMU, and M2 has its first drivers, virtio keyboard, mouse, network and disk (updated 2026-10-04). The first boot on a real Pi 4 is the next step.

If this is accepted it replaces the x86 board in `docs/HARDWARE.md` as the 3.0 reference. The OS stays free; the board is what we sell around it.

## Why a Pi and not the Mac mini

The kernel is 32-bit x86. The Mac mini M4 is Apple Silicon: no UEFI, a custom boot chain, and USB, storage and sound behind Apple-only controllers. Bare-metal there means an Asahi-sized effort. A Raspberry Pi boots a plain `kernel8.img` from its firmware, has a documented framebuffer, UART and interrupt controller, and QEMU models parts of it. Start with a Pi 4B. The Pi 5 hangs every peripheral off the RP1 chip over PCIe, which makes first bring-up harder; move to it second.

## What the port touches

About 163,000 lines of C and headers across boot, kernel, drivers and lib, plus 46 user programs.

- Arch-specific and must be rewritten for AArch64: `boot/boot.S`, the linker script, GDT, IDT, PIC, ISR and IRQ stubs, paging, task switch, ring 3 entry and exit, and the 105 inline-asm blocks.
- PC-only drivers that go away and get Pi equivalents: ATA, PS/2 keyboard and mouse, VBE, vmmouse, PCI, RTL8139 and NE2000, Sound Blaster 16.
- Port I/O (`inb`, `outb`) shows up in 13 files and becomes memory-mapped I/O.
- Portable as is, in theory: the UI, fonts, JPEG and PNG decoders, FAT, the network stack above the NIC, the HTTP client, BearSSL, and the apps' C code. The 46 user programs rebuild with an `svc` syscall ABI instead of `int 0x80`.

## Milestones, each one runs

1. **M0, serial hello. Done (`arch/arm64`, `tools/checks/arm64-m0-check.py`). Also builds for a real Pi 4 (`make -C arch/arm64 pi`), tried on QEMU's raspi4b model; the first boot on a real board is still to do, see [RASPBERRY-PI.md](RASPBERRY-PI.md).** `clang -target aarch64-none-elf` plus `ld.lld` (both installed here) build a kernel that prints on the PL011 UART under `qemu-system-aarch64 -machine virt`. Days.
2. **M1, a machine. Started: M1a and M1b are done** (exception vectors that print faults, the interrupt controller and a timer tick) and M1b (a flat identity map with the MMU and both caches on, a bump heap, `.bss` zeroed on boot), on QEMU's virt machine and its Pi 4 model; `tools/checks/arm64-m0-check.py`. Still to do: the desktop on a framebuffer. Exception vectors, MMU, the generic timer, the GIC, a heap and the memory manager. Draw the existing desktop to a `ramfb` framebuffer. Days to a couple of weeks.
3. **M2, input and net in QEMU. Started: keyboard, mouse, a disk that reads a real sector and a network card that gets a real ARP answer are done (`tools/checks/arm64-m2-check.py`), woken by GIC interrupts.** `virtio` keyboard, mouse, network and block drivers. With the HVF accelerator on the Mac mini this runs at native speed, far faster than today's i386 emulation, so the ARM build helps the browser demo too once v86 is not the only target. Weeks.
4. **M3, userland.** EL0 programs, the syscall layer, per-window address spaces, and all 46 apps rebuilt. Weeks.
5. **M4, a real Pi 4.** Firmware config, mailbox framebuffer, PL011, SD card through EMMC2, USB keyboard and mouse through xHCI, Ethernet through the Genet MAC. Sound last (HDMI or I2S, the hardest). Weeks.
6. **M5, the Pi 5.** RP1 over PCIe for every peripheral.

## Risks

- USB through xHCI is the largest single driver and the first thing a real board needs.
- Sound on a Pi has no Sound Blaster equivalent; budget real time for it.
- The browser demo is i386 under v86 and stays that way. Two architectures means two builds and a second CI lane. Keep the i386 path green the whole time.
- `ring3` and task switching are where x86 assumptions are deepest. Read `docs/ARCHITECTURE.md` before M3.

## First step

M0 is a half-day: new `arch/arm64/` with a boot stub, a linker script and a UART print, and a `make ARCH=arm64` target. Nothing in the i386 build changes.
