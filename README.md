<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/brand/mark-paper.png">
  <img src="docs/brand/mark-ink.png" width="80" alt="Joshua Tree">
</picture>

# Joshua Tree

![version](https://img.shields.io/github/v/release/nulljosh/joshuatree?label=version&color=blue)
![release](https://img.shields.io/github/actions/workflow/status/nulljosh/joshuatree/release.yml?branch=main&label=release)
![license](https://img.shields.io/badge/license-Apache_2.0-green)

Joshua Tree is an operating system written from scratch. No Linux, no libc. It runs on PCs and on the Raspberry Pi 4.

It has windows, a dock, sound, the internet and 38 apps, from Notes and a Terminal to a web browser, mail and live weather. Samantha, the built-in assistant, opens apps and does things for you when you talk to her. The compiler, Plank, is written in Plank and rebuilds itself on the Pi.

<img src="landing/shots/desktop-apps.webp" width="100%" alt="The Joshua Tree desktop with the Terminal and live Weather open">

## Try it

- In a browser: [joshuatree.jaybulb.com/?demo](https://joshuatree.jaybulb.com/?demo).
- On a PC: download the [latest ISO](https://github.com/nulljosh/joshuatree/releases/latest/download/joshuatree.iso) and check it against the [checksums](https://github.com/nulljosh/joshuatree/releases/latest/download/SHA256SUMS). PC boot is tested in QEMU only so far.
- On a Pi 4: `tools/flash-pi.sh` writes the card. The [Guide](docs/GUIDE.md#raspberry-pi) has the steps.

## Build and run

You need clang, lld and QEMU. On a Mac: `brew install lld qemu`. On Linux: `sudo apt install clang lld qemu-system-x86 qemu-system-arm`.

- `make run` builds the PC version and boots it in QEMU.
- `./check.sh` runs the boot test.
- `make -C arch/arm64 run` boots the Pi version in QEMU.

## Docs

- [Guide](docs/GUIDE.md): using it, the Raspberry Pi, the hardware.
- [Architecture](docs/ARCHITECTURE.md): how it is built, the design rules, system calls, tests.
- [Roadmap](docs/ROADMAP.md): what is next and what shipped.
- [Testing](docs/TESTING.md): the short list to try on a real Pi.

## How fast

Measured headless in QEMU by `tools/bench.sh`. The numbers move with the host.

<!-- bench:start -->
| Benchmark | Result |
|---|---|
| Boot to shell | 270 ms |
| Alloc + free | 225 ns/op |
| memcpy | 769 MB/s |
| Context switch | 3236 ns/switch |
| Disk read | 10253 KB/s |
<!-- bench:end -->

## License

[Apache License 2.0](LICENSE)
