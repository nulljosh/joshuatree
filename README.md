<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://github.com/nulljosh/joshuatree/releases/download/3.0.0/jt-mark-paper.png">
  <img src="https://github.com/nulljosh/joshuatree/releases/download/3.0.0/jt-mark-ink.png" width="80" alt="Joshua Tree">
</picture>

# Joshua Tree

![version](https://img.shields.io/github/v/release/nulljosh/joshuatree?label=version&color=blue)
![release](https://img.shields.io/github/actions/workflow/status/nulljosh/joshuatree/release.yml?branch=main&label=release)
![license](https://img.shields.io/badge/license-Apache_2.0-green)

An operating system with its own windows, dock, fonts, sound, internet and 32 apps. It runs in your browser, in an emulator on a Mac, and on a Raspberry Pi 4. Samantha, the assistant from [Turing](https://github.com/nulljosh/turing), runs on it and answers out loud. Where it is going: [Vision](docs/VISION.md).

## Try it

Open [joshuatree.heyitsmejosh.com](https://joshuatree.heyitsmejosh.com). For a PC USB stick, grab the [latest ISO](https://github.com/nulljosh/joshuatree/releases/latest/download/joshuatree.iso) and its [checksums](https://github.com/nulljosh/joshuatree/releases/latest/download/SHA256SUMS). BIOS boot is checked in QEMU. UEFI and real PC hardware are not verified.

To build it: `brew install lld qemu`, then `make run` to boot and `./check.sh` to test.

## On a Raspberry Pi

It boots a Pi 4 to the desktop with a USB keyboard and an HDMI screen. Samantha answers through a relay on your Mac that uses your own Claude API key, which never goes on the Pi. Flash a card with `tools/flash-pi.sh`; the steps, and what is still unchecked on the board, are in [docs/RASPBERRY-PI.md](docs/RASPBERRY-PI.md).

## How fast

Measured headless in QEMU by `tools/bench.sh`. The numbers move with the host.

<!-- bench:start -->
| Benchmark | Result |
|---|---|
| Boot to shell | 260 ms |
| Alloc + free | 172 ns/op |
| memcpy | 756 MB/s |
| Context switch | 3233 ns/switch |
| Disk read | 9674 KB/s |
<!-- bench:end -->

## Docs

[Architecture](docs/ARCHITECTURE.md), [design rules](docs/DESIGN.md), [roadmap](docs/roadmap.md) and the [user guide](docs/GUIDE.md). The rest is indexed in [docs/README.md](docs/README.md).

## License

[Apache License 2.0](LICENSE)
