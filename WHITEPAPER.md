# os Technical Whitepaper

**v4** | September 2026

A kernel. A small one, from nothing. It boots in QEMU, manages its own
memory, cooperatively runs more than one task, reads a real FAT16
filesystem off a real disk, and executes code loaded from it. It exists to
answer one question by building the thing rather than reading about it:
what does it actually take, piece by piece, to turn a bare i386 machine
into something usable — no macOS, no Linux, nothing borrowed underneath.

## Scope

Every piece here is real, not a simulation of one: the memory manager
tracks actual physical frames from the multiboot memory map, the disk
driver talks to a real ATA controller, the filesystem reads bytes a real
copy of macOS's own `newfs_msdos` wrote. Nothing is stubbed to look done.

Two pieces were scoped out on purpose rather than rushed, and stay that
way until they can be verified properly instead of just "it still boots":
a higher-half kernel layout, and ring-3 user mode. Both are documented in
`roadmap.md` with the specific reason each is risky to rush.

## Design decisions

- **No bootloader, no ISO.** QEMU's `-kernel` loads a multiboot ELF
  directly — the boot chain is a separate problem from the kernel itself.
- **Identity-mapped paging, not higher-half.** The first 4MB is
  identity-mapped and paging is on, but the kernel still lives at its
  physical load address. Higher-half needs a boot-time page directory
  split between physical and virtual addresses before `kmain` can even
  run — real complexity, deliberately deferred, not skipped by accident.
- **Cooperative multitasking, not preemptive.** `yield()` swaps stacks
  correctly (verified: two tasks printing `A`/`B` interleave exactly as
  expected). Preemption off the PIT tick needs every task's stack to
  mimic a full interrupt frame, not just `yield()`'s simpler layout —
  a much easier place to introduce a subtle, hard-to-catch bug.
- **FAT16, root directory only.** No subdirectories, no long filenames,
  no FAT32. FAT16 is the best-documented format with the most existing
  tooling to cross-check against, and root-only is enough until something
  actually needs a folder.
- **Ring 0 for everything, including loaded code.** `exec.c` runs a flat
  binary with full kernel privileges. There's no isolation yet — ring-3
  user mode is deferred for the same reason higher-half is: a subtle bug
  in a privilege transition can silently boot "fine" while the isolation
  itself is broken, which the project's only automated check (`check.sh`'s
  boot-banner assertion) cannot detect.

## Verification standard

`check.sh` catches "does it boot at all." Everything else — does the
memory manager actually track free frames correctly, does the disk driver
actually round-trip real data, does the context switch actually swap
stacks — gets verified against real artifacts before shipping: a real
disk image made with `newfs_msdos`, a real flat binary assembled outside
the kernel and placed on that disk, a boot-time printout of the actual
result. See `docs/ARCHITECTURE.md` and the commit history for what was
checked and how, subsystem by subsystem.

Commands: `help` `clear` `echo` `time` `uptime` `mem` `reboot` `crash`
`pagefault` `heaptest` `tasktest` `sleep` `disktest` `ls` `cat` `exec`.

## Build

```sh
brew install lld qemu
make run
./check.sh
```

## License

MIT 2026, Joshua Trommel
