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
5. **M4, a real Pi 4. Started: USB keyboard and mouse through xHCI work under QEMU (`arch/arm64/pci.c`, `arch/arm64/xhci.c`, `tools/checks/arm64-usb-check.py`), with the controller behind a PCIe root port and the keyboard behind a hub, as on the Pi. The Pi's own PCIe bring-up and the VL805 firmware load are written but untested on a board.** Firmware config, mailbox framebuffer, PL011, SD card through EMMC2, USB keyboard and mouse through xHCI, Ethernet through the Genet MAC. Sound last (HDMI or I2S, the hardest). Weeks.
6. **M5, the Pi 5.** RP1 over PCIe for every peripheral.

## Risks

- USB through xHCI is the largest single driver and the first thing a real board needs.
- Sound on a Pi has no Sound Blaster equivalent; budget real time for it.
- The browser demo is i386 under v86 and stays that way. Two architectures means two builds and a second CI lane. Keep the i386 path green the whole time.
- `ring3` and task switching are where x86 assumptions are deepest. Read `docs/ARCHITECTURE.md` before M3.

## First step

M0 is a half-day: new `arch/arm64/` with a boot stub, a linker script and a UART print, and a `make ARCH=arm64` target. Nothing in the i386 build changes.

## Errors and crashes

The tests for this section are `tools/checks/arm64-crash-check.py`, `arm64-fp-check.py`, `arm64-oom-check.py` and `arm64-boot-health-check.py`. Each drives a test build (`make -C arch/arm64 crashtest`, `fpsave`, `fpnosave`, `oomtest`) that the real kernel never contains.

- **A kernel fault.** An unexpected exception at EL1 prints `KERNEL CRASH: <class>`, the ESR, FAR and ELR and the last five console lines on the UART, draws the same text as a red panel on the screen, and sleeps in a `wfe` loop. It uses only the raw UART and the 8x16 VGA font, so it works if the fault came from the console or the text code, and the panel is cleaned out of the data cache so the real GPU shows it. A fault inside the crash code halts quietly. The deliberate EL0 faults of M3 are not crashes: they are printed and the kernel carries on.
- **Interrupts and floating point.** `irq_entry` saves q0-q31, FPCR and FPSR next to the general registers, so a handler can never corrupt the floating point work it interrupted. Today no handler uses floating point; the save makes that a promise nobody has to keep.
- **Out of memory.** `kmalloc` returns 0 when the bump heap is full and every caller checks. A failure prints `oom <where>` and ends only that step: no framebuffer means no screen but a working UART, and fonts that do not load mean the 8x16 VGA font. Open gap: one allocation in `xhci.c`.
- **A new failure cannot hide.** The boot health check fails on any `FAIL` or `oom` line the boot prints that is not on its short, commented list of things QEMU cannot do.
