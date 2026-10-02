<img src="icon.svg" width="80">

# Joshua Tree

![version](https://img.shields.io/github/v/release/nulljosh/joshuatree?label=version&color=blue)
![ci](https://img.shields.io/github/actions/workflow/status/nulljosh/joshuatree/check.yml?event=pull_request&label=ci)
![license](https://img.shields.io/badge/license-Apache_2.0-green)

A whole computer, built from scratch: its own windows, dock, fonts, sound, internet and 26 apps, from the first chip instruction to the last pixel. No libc, no dependencies.

**Try it in your browser:** [joshuatree.heyitsmejosh.com](https://joshuatree.heyitsmejosh.com)

You can talk to it. Ask Samantha to set a reminder, take a note or open an app, and she answers out loud. (She is the assistant from [Turing](https://github.com/nulljosh/turing). Joshua Tree is the machine she runs.)

[![Joshua Tree ad, 36 seconds. Click to play.](docs/hardware/ad-poster.jpg)](https://github.com/nulljosh/joshuatree/releases/download/1.8.8/joshua-tree-ad-v6.mp4)

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
- [Roadmap](docs/roadmap.md): what is next
- [All docs](docs/)

Apache License 2.0, © 2026 Joshua Trommel
