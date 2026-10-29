<picture>
  <source media="(prefers-color-scheme: dark)" srcset="docs/brand/mark-paper.png">
  <img src="docs/brand/mark-ink.png" width="80" alt="Joshua Tree">
</picture>

# Joshua Tree

![version](https://img.shields.io/github/v/release/nulljosh/joshuatree?label=version&color=blue)
![release](https://img.shields.io/github/actions/workflow/status/nulljosh/joshuatree/release.yml?branch=main&label=release)
![license](https://img.shields.io/badge/license-Apache_2.0-green)

A small operating system, written from scratch, that runs on a Raspberry Pi 4, in an emulator on a Mac and in your browser. It has its own windows, dock, sound, internet and 38 apps, from Notes and a Terminal to a web browser, mail and live Weather, plus a few games. Its compiler is its own too: Plank's, written in Plank, rebuilds itself on the Pi with nothing from outside. Samantha, the assistant from [Turing](https://github.com/nulljosh/turing), runs on it: she answers, works the machine and writes apps. The internet work for 7.0 and talking to her through a USB mic are next on the real board.

<img src="landing/shots/desktop-apps.webp" width="100%" alt="The Joshua Tree desktop with the Terminal and live Weather open, and running-app dots in the dock">

<p>
  <img src="landing/shots/samantha-chat.webp" width="32%" alt="Samantha">
  <img src="landing/shots/app-files.webp" width="32%" alt="Burrow, the file browser">
  <img src="landing/shots/app-notes.webp" width="32%" alt="Notes">
  <img src="landing/shots/app-terminal.webp" width="32%" alt="Terminal running uptime, mem and ls">
  <img src="landing/shots/app-weather.webp" width="32%" alt="Weather, live">
  <img src="landing/shots/app-stocks.webp" width="32%" alt="Stocks, live quotes and a chart">
</p>

## Try it

- In a browser: open [joshuatree.jaybulb.com/?demo](https://joshuatree.jaybulb.com/?demo).
- On a PC: boot the [latest ISO](https://github.com/nulljosh/joshuatree/releases/latest/download/joshuatree.iso) ([checksums](https://github.com/nulljosh/joshuatree/releases/latest/download/SHA256SUMS)). BIOS boot is checked in QEMU. UEFI and real PCs are not.
- On a Pi 4: flash a card with `tools/flash-pi.sh` ([steps](docs/GUIDE.md#raspberry-pi)).
- To build it: `brew install lld qemu`. Then `make run` to boot and `./check.sh` to test.

## The box

<img src="docs/hardware/nimbus-hero.jpg" width="60%" alt="Nimbus, a small square computer running Joshua Tree">

Nimbus: a Raspberry Pi 4 and a 7 inch screen in a printed shell. A concept, with the CAD, renders and print pack in the [Guide](docs/GUIDE.md#nimbus).

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

<img src="architecture.svg" width="60%" alt="Joshua Tree at a glance: where it runs, the layers, Samantha, apps, the compiler building itself, and the Pi">

- [Guide](docs/GUIDE.md): using it, the Raspberry Pi, building the hardware.
- [Architecture](docs/ARCHITECTURE.md): how it is built, design rules, system calls, security, tests.
- [Roadmap](docs/ROADMAP.md): what is next, what shipped, the story.
- [Testing](docs/TESTING.md): the short list to try on a real Pi.

## License

[Apache License 2.0](LICENSE)
