<img src="icon.svg" width="80">

# Joshua Tree

![version](https://img.shields.io/badge/version-0.85.3-blue)
![platform](https://img.shields.io/badge/platform-i386-lightgrey)
![license](https://img.shields.io/badge/license-Apache_2.0-green)

A freestanding operating system, written from scratch.

It boots, reads a real disk, talks to the network, and runs a mouse-driven
desktop with 23 applications, a terminal, and live weather. No libc, no
external dependencies beyond Clang, lld and QEMU. The kernel itself needs
no bootloader at all when QEMU loads it directly (`-kernel`); an ISO/USB
image for booting on other machines uses a small third-party bootloader
just to get the CPU into protected mode and hand off, see below.

Live demo, running the real kernel in your browser: [joshuatree.heyitsmejosh.com](https://joshuatree.heyitsmejosh.com)

<img src="progress.svg" width="460">

## Building

```sh
brew install lld qemu
make run      # boots to the shell; type gui for the desktop
./check.sh    # boot regression check
```

## Booting from a CD, USB stick, or another VM

`make iso` builds `joshuatree.iso`, a real bootable disk image, not just
a QEMU convenience. It fetches [Limine](https://github.com/limine-bootloader/limine)
(a small BIOS/UEFI bootloader, pulled into a gitignored `build/` folder
at build time, never committed) and packs it with `kernel.elf` using
`xorriso`. The result is a hybrid image: it works as a CD-ROM (booted
with `-cdrom`) and, because the same file also carries a valid MBR/GPT
partition table, as a raw USB disk image too.

To try it in QEMU:

```sh
make iso
qemu-system-i386 -cdrom joshuatree.iso          # CD-ROM boot
qemu-system-i386 -drive file=joshuatree.iso,format=raw   # USB-stick-style boot
```

To try it in VirtualBox, create a new machine (type Other/Unknown,
32-bit), attach `joshuatree.iso` as the optical drive, and boot it.

To put it on a real USB stick and boot real hardware (this will erase
the stick, double check the device path first):

```sh
# macOS: find the disk with `diskutil list`, then
sudo dd if=joshuatree.iso of=/dev/rdiskN bs=4m
# Linux: find the disk with `lsblk`, then
sudo dd if=joshuatree.iso of=/dev/sdX bs=4M status=progress conv=fsync
```

Then boot the machine from that USB stick (most PCs: hold down the boot
menu key at power-on, F12/F10/Esc/Del depending on the maker, and pick
the USB drive). You can also drag the ISO straight into balenaEtcher or
Rufus if you would rather not use `dd` by hand.

The kernel boots straight to the desktop with no disk attached at all
(it falls back to an in-memory filesystem seeded with a couple of demo
files, the same fallback the browser demo above uses). To also mount a
real FAT16 disk with your own files, attach `dotfiles.img` (built by
`make dotfiles.img`) as a second drive alongside the ISO, for example
`qemu-system-i386 -cdrom joshuatree.iso -drive file=dotfiles.img,format=raw,if=ide,index=0`.

What this does not do yet, and where it is expected to fall short on
real machines:

- UEFI-only machines with no legacy BIOS/CSM support are unverified.
  The ISO carries UEFI boot stubs and should work, but nobody has
  tried it on a real UEFI-only board yet.
- No USB storage or AHCI/NVMe disk driver exists, so real hardware only
  ever sees the FAT disk if it is wired up the old IDE/ATA way (real,
  modern PCs mostly don't have that bus anymore). This is a real gap,
  not a soft one, and it's on the roadmap.
- Graphics needs a Bochs/QEMU-compatible virtual VGA adapter (the
  `1234:1111` PCI ID QEMU's `-vga std` and most VMs expose). Real
  hardware GPUs, and other virtual adapters like Cirrus or VMware SVGA,
  don't implement that interface, so the kernel currently falls back to
  no graphics rather than guessing at a driver it doesn't have. It
  still boots and logs over serial either way, it just can't paint a
  screen.
- USB keyboards only work through a real PC's BIOS/CSM legacy PS/2
  emulation; nothing here talks to USB HID directly.
- Real hardware hasn't actually been tried yet. Everything above is
  verified headless in QEMU (CD-ROM boot, raw-disk/USB-style boot, and
  the no-NIC/no-disk/non-Bochs-VGA fallback paths), not on a physical
  machine.

## Usage

`gui` opens the desktop. `web example.com` performs a real DNS lookup and
TCP connection over this repo's own network stack, with no host operating
system underneath it. `ls`, `cat` and `write` operate on a real FAT disk.

`usertest` runs the reference ring-3 program (`user/hello.c`), compiled
against [`docs/SYSCALL-ABI.md`](docs/SYSCALL-ABI.md) and nothing else,
loaded from the filesystem and executed at CPL 3. `notetest` runs a second
program (`user/note.c`), a small file utility that creates, appends to and
seek-patches a file from ring 3 with real command-line arguments.
`exec <file> [args...]` runs any flat binary the same way, passing the
remaining words as `argv`.

The remaining commands mostly exercise one subsystem at a time: `help`
`dmesg` `mem` `ps` `kill` `sleep` `heaptest` `heapgrow` `tasktest`
`preempttest` `ring3test` `usertest` `notetest` `isotest` `reaptest`
`killtest` `disktest` `diskuse` `fsuse` `mkdir` `rm` `cd` `browse` `lspci`
`gfxtest` `fonttest` `mousetest` `nettest` `ifconfig` `netscan` `serve`
`serveapp` `chat` `weathertest` `geotest` `wind` `testapps` `crash`
`pagefault` `reboot`.

## Architecture

<img src="architecture.svg" width="600">

Full subsystem breakdown: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).
The syscall contract: [`docs/SYSCALL-ABI.md`](docs/SYSCALL-ABI.md).
Current plan: [`docs/roadmap.md`](docs/roadmap.md).

## License

Apache License 2.0, © 2026 Joshua Trommel
