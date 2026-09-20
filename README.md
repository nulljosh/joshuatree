<img src="icon.svg" width="80">

# Joshua Tree

![version](https://img.shields.io/badge/version-0.79.1-blue)
![platform](https://img.shields.io/badge/platform-i386-lightgrey)
![license](https://img.shields.io/badge/license-Apache_2.0-green)

An operating system written from nothing.

It boots, reads a real disk, talks to the network, and opens a desktop you
can click around with a mouse. Eighteen apps, a terminal, live weather.
No libc, no bootloader, nothing borrowed. Clang, lld and QEMU are the only
things it needs.

Try it in your browser, running for real: [joshuatree.heyitsmejosh.com](https://joshuatree.heyitsmejosh.com)

<img src="progress.svg" width="460">

## Build it

```sh
brew install lld qemu
make run      # boots to the shell, type gui for the desktop
./check.sh    # does it still boot
```

## Poke at it

`gui` opens the desktop. `web example.com` does a real DNS lookup and a real
TCP connection over the network stack in this repo, with no operating system
underneath it. `ls`, `cat`, `write` and `exec` work on a real FAT disk.

The rest, mostly there to exercise one subsystem at a time: `help` `dmesg`
`mem` `ps` `kill` `sleep` `heaptest` `heapgrow` `tasktest` `preempttest`
`ring3test` `isotest` `reaptest` `killtest` `disktest` `diskuse` `fsuse`
`mkdir` `rm` `cd` `browse` `lspci` `gfxtest` `fonttest` `mousetest`
`nettest` `ifconfig` `netscan` `serve` `serveapp` `chat` `weathertest`
`geotest` `wind` `testapps` `crash` `pagefault` `reboot`.

## How it fits together

<img src="architecture.svg" width="600">

Every file, what it does, and what is deliberately missing:
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).
The contract a program can rely on: [`docs/SYSCALL-ABI.md`](docs/SYSCALL-ABI.md).
What is next: [`roadmap.md`](roadmap.md).

## License

Apache License 2.0, © 2026 Joshua Trommel
