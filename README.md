<img src="icon.svg" width="80">

# os

![version](https://img.shields.io/badge/version-v3-blue)
![license](https://img.shields.io/badge/license-MIT-green) [![GitHub](https://img.shields.io/badge/GitHub-nulljosh%2Fos-black?logo=github)](https://github.com/nulljosh/os)

A kernel. A small one, from nothing. It boots in QEMU and drops you at a prompt.

| Piece | Where |
|-------|-------|
| Boot | `boot.S`: multiboot1 header, stack, jump to `kmain` |
| Kernel | `kernel.c`: VGA text, keyboard, clock, shell |
| Segments | `gdt.c`: flat GDT (ring-0 code + data, 4GB) |
| Interrupts | `idt.c` + `isr.S`: IDT and the 32 CPU-exception handlers |
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

Commands: `help` `clear` `echo` `time` `reboot` `crash` (triggers a breakpoint exception, exercises the IDT)

## Architecture

<img src="architecture.svg" width="600">

QEMU's `-kernel` loads it directly. No bootloader, no ISO. It installs a flat
GDT and an IDT so CPU exceptions report and halt instead of triple-faulting,
but still has no paging and no IRQ-driven anything yet: the keyboard is polled
on port `0x60` and `time` reads the CMOS clock instead of counting PIT ticks.
See `roadmap.md` for what's next (PIC remap, IRQ-driven keyboard/timer, then
paging, multitasking, a filesystem, graphics, and eventually a browser).

## License

MIT 2026, Joshua Trommel
