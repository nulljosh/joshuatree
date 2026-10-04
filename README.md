<img src="icon.svg" width="80">

# Joshua Tree

![version](https://img.shields.io/github/v/release/nulljosh/joshuatree?label=version&color=blue)
![ci](https://img.shields.io/github/actions/workflow/status/nulljosh/joshuatree/check.yml?event=pull_request&label=ci)
![license](https://img.shields.io/badge/license-Apache_2.0-green)

A whole computer, built from scratch: its own windows, dock, fonts, sound, internet and 26 apps, from the first chip instruction to the last pixel. No libc, no dependencies.

**Try it in your browser:** [joshuatree.heyitsmejosh.com](https://joshuatree.heyitsmejosh.com)

You can talk to it. Ask Samantha to set a reminder, take a note or open an app, and she answers out loud. (She is the assistant from [Turing](https://github.com/nulljosh/turing). Joshua Tree is the machine she runs.)

## Boot it

- **Browser:** [joshuatree.heyitsmejosh.com](https://joshuatree.heyitsmejosh.com)
- **USB stick:** download the ISO from [Releases](https://github.com/nulljosh/joshuatree/releases). The release notes have the copy steps.
- **Talk to her:** `make talk`, then hold **F2**. Needs real hardware and a mic (see `user/samantha.c`).

## Build it

```sh
brew install lld qemu
make run       # boot it (type gui for the desktop)
./check.sh     # test it
```

More in the Makefile: `make samantha`, `make talk`, `make iso`.

## How fast

Measured headless in QEMU by `tools/bench.sh`. The numbers move with the host.

<!-- bench:start -->
| Benchmark | Result |
|---|---|
| Boot to shell | 250 ms |
| Alloc + free | 65 ns/op |
| memcpy | 777 MB/s |
| Context switch | 2797 ns/switch |
| Disk read | 11443 KB/s |
<!-- bench:end -->

## Read more

- [Whitepaper](docs/WHITEPAPER.md): why and how
- [Architecture](docs/ARCHITECTURE.md): how it fits together, every file
- [Hardware](docs/HARDWARE.md): the real board and the 3.0 plan
- [Portfolio mode](docs/PORTFOLIO.md): how heyitsmejosh.com is a mode of the OS, with Joshua in place of Samantha
- [Raspberry Pi guide](docs/RASPBERRY-PI.md): what to buy, the serial cable, and how to boot it on a real Pi 4. [Printable Pi case](docs/hardware/PI-CASE.md)
- [ARM64 and the Raspberry Pi](docs/ARM64.md): the second CPU target. It boots under QEMU today with a drawn desktop, a keyboard, mouse, disk and network, and its first program in user mode (`make -C arch/arm64 run`); the Pi 4B is the first real board
- [Roadmap](docs/roadmap.md): what is next
- [All docs](docs/)

## License

Software: Apache License 2.0, © 2026 Joshua Trommel. Hardware designs: CC BY-NC-SA 4.0, free to build for yourself, see [docs/hardware/LICENSE-NOTICE.md](docs/hardware/LICENSE-NOTICE.md). Joshua Tree™ and Neo Kit™ are trademarks, see [TRADEMARKS.md](TRADEMARKS.md).
