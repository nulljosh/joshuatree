<img src="icon.svg" width="80">

# Joshua Tree

![version](https://img.shields.io/badge/version-0.76.19-blue)
![platform](https://img.shields.io/badge/platform-i386-lightgrey)
![license](https://img.shields.io/badge/license-Apache_2.0-green)

Live: [joshuatree.heyitsmejosh.com](https://joshuatree.heyitsmejosh.com)

A kernel. A small one, from nothing. It boots in QEMU, reads a real disk,
runs code loaded off it, drives a real mouse-driven GUI desktop with real
apps. No libc, no bootloader beyond multiboot, no external dependencies
beyond clang/lld/qemu. Full subsystem breakdown, every file mapped to what
it does: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md). Plan and what's
next: [`roadmap.md`](roadmap.md).

## Progress

<img src="progress.svg" width="460">

Regenerate after checking off `roadmap.md`: `./progress.sh`

## Build

```sh
brew install lld qemu
make run      # boots to the shell
./check.sh    # boot check
```

The Mac launcher and `make run` attach `dotfiles.img`. Run `./sync_dotfiles.sh`
to refresh its allowlisted, readable copies from `../dotfiles`. Fish and macOS
apps do not execute inside Joshua Tree yet.

Commands: `help` `clear` `echo` `time` `uptime` `dmesg` `mem` `reboot` `crash`
`pagefault` `heaptest` `heapgrow` `tasktest` `preempttest` `weathertest`
`weatherfxcliptest` `geotest` `wind`
`isotest` `reaptest` `ring3test` `ps` `kill` `killtest` `sleep` `disktest`
`diskuse` `fsuse` `ls` `cat` `exec` `rm` `cd` `mkdir` `write <file> <content>`
`browse` `lspci` `gfxtest` `fonttest` `mousetest` `nettest` `ifconfig` `netscan`
`web <host> [path]` `serve` `serveapp` `chat <message>` `build <what>` `gui`
`testapps`, most exist to manually exercise a subsystem (see
`docs/ARCHITECTURE.md`), not just to be useful. `web` and `gui` are worth
trying: `web` does a real DNS lookup and TCP connection over the network
stack in this repo, no libc, no OS underneath; `gui` opens a real
mouse-driven desktop with working apps.

## Voice control

`tools/voice-control.sh` is a host-side script (not part of the kernel, see
`roadmap.md`'s v10 note on why voice stays outside it): speak a request, a
local Ollama model maps it onto one of the kernel's real commands, and it
gets typed into a running QEMU instance through its monitor socket. Start
the kernel with a monitor socket first:

```sh
qemu-system-i386 -kernel kernel.elf -display none \
  -monitor unix:/tmp/jt-monitor.sock,server,nowait \
  -netdev user,id=n0 -device rtl8139,netdev=n0
tools/voice-control.sh          # records 4s, or: voice-control.sh 6
```

Needs `sox` (`rec`), `whisper-cpp` (`whisper-cli` plus a downloaded
`ggml-*.bin` model), and Ollama running locally with `llama3.1:8b`.

## Architecture

<img src="architecture.svg" width="600">

QEMU's `-kernel` loads it directly. No bootloader, no ISO. Full subsystem
breakdown, boot sequence, and what's deliberately not built yet (and why):
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md). Plan and ETAs: `roadmap.md`.

## License

Apache License 2.0, © 2026 Joshua Trommel
