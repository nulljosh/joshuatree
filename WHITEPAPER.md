# os Technical Whitepaper

**v0** | September 2026

A kernel. A small one, from nothing. It boots in QEMU and drops you at a prompt.
It exists to answer one question by building the thing rather than reading about
it: what is actually true about a computer before any operating system has run,
and how little code does it take to turn that bare machine into something that
takes a command and responds.

## Scope

The point is the smallest thing that is honestly a kernel: it owns the
machine, talks to hardware directly, and takes commands. Everything that a
real OS adds for concurrency is deliberately absent, because concurrency is a
different, much larger problem than "does this thing boot and talk back," and
mixing the two would bury the part worth learning under scheduler code.

| Piece | File |
|---|---|
| Boot | `boot.S`: multiboot1 header, stack, jump to `kmain` |
| Kernel | `kernel.c`: VGA text, keyboard, clock, shell |
| Link | `linker.ld`: flat ELF32 at 1 MB |
| Check | `check.sh`: boots it and asserts the banner reached VGA memory |

## Design decisions

- **No bootloader, no ISO.** QEMU's `-kernel` loads a multiboot ELF directly,
  because writing a bootloader is its own separate problem and this project is
  about the kernel, not the boot chain in front of it.
- **32-bit protected mode, no paging.** Flat segments are enough for a shell;
  virtual memory only earns its complexity once something needs isolation
  between processes, and there's only one thing running here.
- **No interrupt table.** The keyboard is polled on port `0x60`; `time` reads
  the CMOS clock instead of counting timer ticks. That trades a background
  tick for about a hundred fewer lines, and a polling loop is easier to reason
  about honestly than an interrupt handler is. An IDT, paging and long mode
  come in when something has to run while the shell waits for a key.
- **VGA text mode.** Writes go straight to `0xB8000`, the simplest possible
  output a machine offers, so there's no driver stack between "kernel wants to
  print" and "text appears."

Commands: `help` `clear` `echo` `time` `reboot`.

## Build

```sh
brew install lld qemu
make run
./check.sh
```

## License

MIT 2026, Joshua Trommel
