<picture>
  <source media="(prefers-color-scheme: dark)" srcset="https://github.com/nulljosh/joshuatree/releases/download/2.23.0/jt-mark-paper.png">
  <img src="https://github.com/nulljosh/joshuatree/releases/download/2.23.0/jt-mark-ink.png" width="80" alt="Joshua Tree">
</picture>

# Joshua Tree

![version](https://img.shields.io/github/v/release/nulljosh/joshuatree?label=version&color=blue)
![ci](https://img.shields.io/github/actions/workflow/status/nulljosh/joshuatree/check.yml?event=pull_request&label=ci)
![license](https://img.shields.io/badge/license-Apache_2.0-green)

A whole computer, built from scratch: its own windows, dock, fonts, sound, internet and 30 apps, from the first chip instruction to the last pixel. No libc, no dependencies.

Where it is going, and the picture of how it all fits: [Vision](docs/VISION.md).

**Try it in your browser:** [joshuatree.heyitsmejosh.com](https://joshuatree.heyitsmejosh.com)

You can talk to it. Ask Samantha to set a reminder, take a note or open an app, and she answers out loud. (She is the assistant from [Turing](https://github.com/nulljosh/turing). Joshua Tree is the machine she runs.)

## In plain words

A computer is a pile of chips and a pile of software on top. Most people never see the bottom of that pile. This project is the whole pile, written by one person: the code that wakes the chip, draws the screen, reads the keyboard, and runs the apps. You can read all of it.

It runs in your browser, in an emulator on a Mac, and on a real Raspberry Pi 4.

## On a Raspberry Pi

It boots on a Pi 4 with a USB keyboard and an HDMI screen: the desktop, the dock, the console and the Claude prompt. It joins Wi-Fi, sets its clock from the network, and answers questions at the console through Samantha. The steps to flash a card are in [the Pi guide](docs/RASPBERRY-PI.md); one script, `tools/flash-pi.sh`, does the work.

## Boot it

- **Browser:** [joshuatree.heyitsmejosh.com](https://joshuatree.heyitsmejosh.com)
- **USB stick:** download the ISO from [Releases](https://github.com/nulljosh/joshuatree/releases). The release notes have the copy steps.
- **Talk to her:** `make talk`, then hold **F2**. Needs real hardware and a mic (see `user/samantha.c`).
- **Ask Claude Code:** run the relay on your Mac and open Claude from the Launchpad. See [the Claude app](docs/CLAUDE-APP.md).

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

[Our story](docs/STORY.md): who we are and how it went. [The docs](docs/README.md): whitepaper, architecture, the design rules, hardware, the Raspberry Pi guide and the roadmap.

## License

[Apache License 2.0](LICENSE)
