<img src="icon.svg" width="80">

# Joshua Tree

![version](https://img.shields.io/badge/version-0.82.0-blue)
![platform](https://img.shields.io/badge/platform-i386-lightgrey)
![license](https://img.shields.io/badge/license-Apache_2.0-green)

A freestanding operating system, written from scratch.

It boots, reads a real disk, talks to the network, and runs a mouse-driven
desktop with 21 applications, a terminal, and live weather. No libc, no
bootloader, no external dependencies beyond Clang, lld and QEMU.

Live demo, running the real kernel in your browser: [joshuatree.heyitsmejosh.com](https://joshuatree.heyitsmejosh.com)

<img src="progress.svg" width="460">

## Building

```sh
brew install lld qemu
make run      # boots to the shell; type gui for the desktop
./check.sh    # boot regression check
```

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
Current plan: [`roadmap.md`](roadmap.md).

## License

Apache License 2.0, © 2026 Joshua Trommel
