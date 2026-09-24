<img src="icon.svg" width="80">

# Joshua Tree

![version](https://img.shields.io/github/v/release/nulljosh/joshuatree?label=version&color=blue)
![ci](https://img.shields.io/github/actions/workflow/status/nulljosh/joshuatree/check.yml?branch=main&label=ci)
![platform](https://img.shields.io/badge/platform-i386-lightgrey)
![license](https://img.shields.io/badge/license-Apache_2.0-green)

A desktop built from nothing in C: bootloader, kernel, memory manager, filesystem, network stack, window system, typeface renderer, and 25 native apps, with real text selection in Notes. No libc, no external dependencies beyond Clang, lld, and QEMU. Run live in the browser: [joshuatree.heyitsmejosh.com](https://joshuatree.heyitsmejosh.com)

## Boot it

**Browser:** [joshuatree.heyitsmejosh.com](https://joshuatree.heyitsmejosh.com)

**USB stick:** Download from [Releases](https://github.com/nulljosh/joshuatree/releases), then:
```sh
# macOS: find the disk with `diskutil list`, then
sudo dd if=joshuatree-vX.Y.Z.iso of=/dev/rdiskN bs=4m
# Linux: find the device with `lsblk`, then
sudo dd if=joshuatree-vX.Y.Z.iso of=/dev/sdX bs=4M status=progress conv=fsync
```
Boot from USB (F12/F10/Esc/Del at power-on).

## Build it

```sh
brew install lld qemu
make run              # boot to shell; type gui for desktop
make iso              # build bootable ISO
./check.sh            # run regression tests
```

## Read more

- [docs/WHITEPAPER.md](docs/WHITEPAPER.md) - why and how
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) - every file
- [SECURITY.md](SECURITY.md) - security model
- [docs/THREAT-MODEL.md](docs/THREAT-MODEL.md) - threat model
- [docs/roadmap.md](docs/roadmap.md) - what's next

Apache License 2.0, © 2026 Joshua Trommel
