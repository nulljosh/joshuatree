<img src="icon.svg" width="80">

# os

![version](https://img.shields.io/badge/version-v4-blue)
![license](https://img.shields.io/badge/license-MIT-green) [![GitHub](https://img.shields.io/badge/GitHub-nulljosh%2Fos-black?logo=github)](https://github.com/nulljosh/os)

A kernel. A small one, from nothing. It boots in QEMU, reads a real
filesystem off a real disk, and runs code loaded from it. Full breakdown:
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

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
`heaptest` `tasktest` `sleep` `disktest` `ls` `cat` `exec` — most exist to
manually exercise a subsystem (see `docs/ARCHITECTURE.md`), not just to be useful.

## Architecture

<img src="architecture.svg" width="600">

QEMU's `-kernel` loads it directly. No bootloader, no ISO. Full subsystem
breakdown, boot sequence, and what's deliberately not built yet (and why):
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md). Plan and ETAs: `roadmap.md`.

## License

MIT 2026, Joshua Trommel
