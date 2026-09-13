<img src="icon.svg" width="80">

# Joshua Tree

![version](https://img.shields.io/badge/version-v5-blue)
![license](https://img.shields.io/badge/license-MIT-green) [![GitHub](https://img.shields.io/badge/GitHub-nulljosh%2Fjoshuatree-black?logo=github)](https://github.com/nulljosh/joshuatree)
![language](https://img.shields.io/badge/language-C%20%2F%20x86%20asm-blue)
![platform](https://img.shields.io/badge/platform-i386-lightgrey)
[![repo size](https://img.shields.io/github/repo-size/nulljosh/joshuatree)](https://github.com/nulljosh/joshuatree)
[![last commit](https://img.shields.io/github/last-commit/nulljosh/joshuatree)](https://github.com/nulljosh/joshuatree/commits/main)

A kernel. A small one, from nothing. It boots in QEMU, reads a real disk,
runs code loaded off it. Full breakdown: [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

Renamed from `os`. The tree survives the Mojave on almost nothing, built
the same way this kernel is. Also a nod to the U2 album, and my own name.

| Piece | Where |
|-------|-------|
| Boot | `boot/boot.S`: multiboot1 header, temporary page tables to get into the higher half, jump to `kmain` (kernel runs at 0xC0000000+, loaded at 1MB) |
| Segments | `gdt.c`: flat GDT (ring-0 + ring-3 code/data, 4GB, plus a TSS) |
| Interrupts | `idt.c` + `isr.S`: IDT and the 32 CPU-exception handlers |
| IRQs | `pic.c`, `irq.c` + `irq_stubs.S`: PIC remap, IRQ-driven keyboard, PIT timer |
| Memory | `pmm.c` (physical frames), `paging.c` (identity + higher-half double-mapped paging), `kheap.c` (`kmalloc`/`kfree`) |
| Tasks | `task.c` + `irq_stubs.S`'s irq0: preemptive round-robin off the PIT tick, `yield()` reaches the same path in software via `int $32` |
| User mode | `ring3.c` + `ring3_asm.S`: real ring-3 privilege isolation, `ring3test` proves a privileged instruction faults instead of silently succeeding |
| Storage | `ata.c` (disk driver), `fat.c` (FAT16 read), `exec.c` (load+run a flat binary) |
| Console | `kernel/kernel.c`: VGA text, keyboard, clock, the shell |
| Link | `linker.ld`: flat ELF32 at 1 MB |
| Check | `check.sh`: boots it and checks the banner reached VGA memory |

## Progress

<img src="progress.svg" width="460">

Regenerate after checking off `roadmap.md`: `./progress.sh`

## Build

```sh
brew install lld qemu
make run      # boots to the shell
./check.sh    # boot check
```

Commands: `help` `clear` `echo` `time` `uptime` `mem` `reboot` `crash` `pagefault`
`heaptest` `tasktest` `sleep` `disktest` `ls` `cat` `exec` `rm` `browse` `lspci`
`gfxtest` `mousetest` `nettest` `web <host> [path]`, most exist to manually
exercise a subsystem (see `docs/ARCHITECTURE.md`), not just to be useful.
`web` is the one worth trying: it does a real DNS lookup and TCP connection
over the network stack in this repo, no libc, no OS underneath.

## Architecture

<img src="architecture.svg" width="600">

QEMU's `-kernel` loads it directly. No bootloader, no ISO. Full subsystem
breakdown, boot sequence, and what's deliberately not built yet (and why):
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md). Plan and ETAs: `roadmap.md`.

## License

MIT 2026, Joshua Trommel
