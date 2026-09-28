<img src="icon.svg" width="80">

# Joshua Tree

![version](https://img.shields.io/github/v/release/nulljosh/joshuatree?label=version&color=blue)
![ci](https://img.shields.io/github/actions/workflow/status/nulljosh/joshuatree/check.yml?event=pull_request&label=ci)
![platform](https://img.shields.io/badge/platform-i386-lightgrey)
![license](https://img.shields.io/badge/license-Apache_2.0-green)

A whole computer, built from nothing. Its own kernel, its own desktop, its own network, and 25 apps. Written in C, no libc.

Try it live in your browser: [joshuatree.heyitsmejosh.com](https://joshuatree.heyitsmejosh.com)

Ask Samantha to set a reminder, take a note or open an app. She does it, and she answers out loud: her voice plays through Joshua Tree's own sound driver, with a face that talks while she speaks. Samantha is the assistant from [Turing](https://github.com/nulljosh/turing), a separate project; Joshua Tree's Samantha app is how the OS talks to her. Everything you save lands on a real disk, and every feature has a check behind it.

## Boot it

**Browser:** [joshuatree.heyitsmejosh.com](https://joshuatree.heyitsmejosh.com)

**USB stick:** Download from [Releases](https://github.com/nulljosh/joshuatree/releases), then:
```sh
# macOS: find the disk with `diskutil list`, then
sudo dd if=joshuatree-X.Y.Z.iso of=/dev/rdiskN bs=4m
# Linux: find the device with `lsblk`, then
sudo dd if=joshuatree-X.Y.Z.iso of=/dev/sdX bs=4M status=progress conv=fsync
```
Boot from USB (F12/F10/Esc/Del at power-on).

Talk to her: `make talk`, hold **F2**, speak. (Real hardware and a real mic only -- QEMU's own Sound Blaster emulation has no recording path today, so a real "hearing" Samantha only exists on real hardware or a card QEMU emulates more fully; see `kernel/chat.h`.)

## Build it

```sh
brew install lld qemu
make run              # boot to shell; type gui for desktop
make talk             # boot with a real Sound Blaster on this Mac's mic/speakers
make iso              # build bootable ISO
./check.sh            # run regression tests
./tools/bench.sh      # boot time, heap, memcpy, context switch, disk
```

## How fast

Measured headless in QEMU on a Mac Mini M4 by `tools/bench.sh`. Run it yourself; the numbers move with the host.

<!-- bench:start -->
| Benchmark | Result |
|---|---|
| Boot to shell | 260 ms |
| Alloc + free | 134 ns/op |
| memcpy | 475 MB/s |
| Context switch | 4348 ns/switch |
| Disk read | 6375 KB/s |
<!-- bench:end -->

## Read more

- [docs/WHITEPAPER.md](docs/WHITEPAPER.md) - why and how
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) - every file
- [docs/BENCHMARKS.md](docs/BENCHMARKS.md) - how fast it is
- [SECURITY.md](SECURITY.md) - security model
- [docs/THREAT-MODEL.md](docs/THREAT-MODEL.md) - threat model
- [docs/roadmap.md](docs/roadmap.md) - what's next

Apache License 2.0, © 2026 Joshua Trommel
