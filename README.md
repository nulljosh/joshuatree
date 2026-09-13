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

Renamed from `os`. Same tree survives the Mojave on almost nothing, I'm
building this the same way, piece by piece, nothing borrowed. Also a nod
to the U2 album, and yeah, my own name.

| Piece | Where |
|-------|-------|
| Boot | `boot.S`: multiboot1 header, stack, jump to `kmain` |
| Segments | `gdt.c`: flat GDT (ring-0 code + data, 4GB) |
| Interrupts | `idt.c` + `isr.S`: IDT and the 32 CPU-exception handlers |
| IRQs | `pic.c`, `irq.c` + `irq_stubs.S`: PIC remap, IRQ-driven keyboard, PIT timer |
| Memory | `pmm.c` (physical frames), `paging.c` (identity-mapped paging), `kheap.c` (`kmalloc`/`kfree`) |
| Tasks | `task.c` + `task_switch.S`: cooperative round-robin scheduling |
| Storage | `ata.c` (disk driver), `fat.c` (FAT16 read), `exec.c` (load+run a flat binary) |
| Console | `kernel.c`: VGA text, keyboard, clock, the shell |
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
`heaptest` `tasktest` `sleep` `disktest` `ls` `cat` `exec`, most exist to
manually exercise a subsystem (see `docs/ARCHITECTURE.md`), not just to be useful.

## Architecture

<img src="architecture.svg" width="600">

QEMU's `-kernel` loads it directly. No bootloader, no ISO. Full subsystem
breakdown, boot sequence, and what's deliberately not built yet (and why):
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md). Plan and ETAs: `roadmap.md`.

## License

MIT 2026, Joshua Trommel
