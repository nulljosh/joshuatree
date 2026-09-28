<img src="icon.svg" width="80">

# Joshua Tree

![version](https://img.shields.io/github/v/release/nulljosh/joshuatree?label=version&color=blue)
![ci](https://img.shields.io/github/actions/workflow/status/nulljosh/joshuatree/check.yml?event=pull_request&label=ci)
![platform](https://img.shields.io/badge/platform-i386-lightgrey)
![license](https://img.shields.io/badge/license-Apache_2.0-green)

A computer's brain, built from scratch.

Every computer runs an operating system. It boots when you press the power button, draws the screen, hears the keyboard, saves your files and runs your apps. Joshua Tree is a complete OS from the first chip instruction to the last pixel, written from zero, with its own windows, dock, fonts, sound, internet and 25 apps.

Try it now: [joshuatree.heyitsmejosh.com](https://joshuatree.heyitsmejosh.com)

You can talk to it. Ask Samantha to set a reminder, take a note or open an app. She does it, out loud, with a face that talks while she speaks. (Samantha is the assistant from [Turing](https://github.com/nulljosh/turing). Joshua Tree is the machine she runs.)

## How it fits together

Top to bottom: where it runs, what you see, the engine underneath, the parts that talk to hardware, the outside services it calls, and where your files live.

<img src="architecture.svg" width="600">

## Boot it

**Browser:** [joshuatree.heyitsmejosh.com](https://joshuatree.heyitsmejosh.com)

**USB stick:** Download from [Releases](https://github.com/nulljosh/joshuatree/releases):
```sh
# macOS
sudo dd if=joshuatree-X.Y.Z.iso of=/dev/rdiskN bs=4m
# Linux
sudo dd if=joshuatree-X.Y.Z.iso of=/dev/sdX bs=4M status=progress
```

Talk to her: `make talk`, hold **F2**, speak. (Real hardware and a real mic only -- QEMU's own Sound Blaster emulation has no recording path today, so a real "hearing" Samantha only exists on real hardware or a card QEMU emulates more fully; see `kernel/chat.h`.)

## Build it

```sh
brew install lld qemu
make run              # boot to shell; type gui for desktop
make samantha         # boot straight into Samantha, full screen, input focused
make talk             # boot with a real Sound Blaster on this Mac's mic/speakers
make iso              # build bootable ISO
./check.sh            # run tests
./tools/bench.sh      # run benchmarks
```

## How fast

Measured headless in QEMU on a Mac Mini M4 by `tools/bench.sh`. Run it yourself; the numbers move with the host.

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

- [docs/WHITEPAPER.md](docs/WHITEPAPER.md) - why and how
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) - every file
- [docs/VERSIONS.md](docs/VERSIONS.md) - release history
- [docs/PLAYLIST.md](docs/PLAYLIST.md) - the Building an OS video series, mapped to what we have
- [docs/TESTING.md](docs/TESTING.md) - every test
- [docs/roadmap.md](docs/roadmap.md) - what's next

Apache License 2.0, © 2026 Joshua Trommel
