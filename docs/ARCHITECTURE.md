# Architecture

What each file does and how boot actually proceeds. `roadmap.md` is the plan;
this is the map of what exists right now.

## Boot sequence

1. `boot/boot.S`, multiboot1 header, sets up a stack, pushes the multiboot info
   pointer GRUB/QEMU leaves in `%ebx`, calls `kmain`.
2. `kmain` (`kernel/kernel.c`) brings subsystems up in dependency order:
   `gdt_install` → `idt_install` → `irq_install` → `pmm_init` →
   `paging_install` → `tasks_init` → `fat_mount` → the shell loop.

## Subsystems

| File | What it owns |
|---|---|
| `gdt.c` | Flat GDT: ring-0 and ring-3 code/data segments (both spanning 4GB) plus a TSS for ring-3-to-ring-0 stack switches |
| `idt.c` + `isr.S` | IDT and the 32 CPU-exception handlers. An exception prints and halts, no recovery |
| `pic.c` | Remaps the 8259 PIC so IRQs land on vectors 32-47 instead of overlapping CPU exceptions |
| `irq.c` + `irq_stubs.S` | IRQ0 (PIT tick counter) and IRQ1 (keyboard ring buffer) |
| `pmm.c` | Physical memory: a bitmap over `mem_upper` from the multiboot info struct |
| `paging.c` | Identity-maps the first 4MB and turns paging on |
| `kheap.c` | `kmalloc`/`kfree`, a first-fit free list grown a frame at a time from `pmm.c` |
| `task.c` + `irq_stubs.S`'s irq0 | Preemptive round-robin off the PIT tick. `yield()` reaches the same switch in software via `int $32`, same IDT gate as the hardware timer |
| `ata.c` | ATA PIO disk driver, LBA28, primary master only |
| `fat.c` | Read-only FAT16, root directory only, 8.3 names |
| `exec.c` | Loads a flat binary via `fat.c` and calls into it, ring 0, no isolation |
| `kernel/kernel.c` | VGA text console, PS/2 scancode table, RTC clock, the shell |

## Why things are ordered this way

Each subsystem depends on the ones above it: paging needs `pmm.c`'s frames,
the heap needs paging to be sane, tasks need the heap for their stacks, `fat.c`
needs `ata.c`'s sectors, `exec.c` needs `fat.c`'s files. Bringing them up
out of order is the fastest way to a silent, hard-to-diagnose bug.

## What's deliberately not here yet

One piece is still scoped out on purpose rather than rushed, see
`roadmap.md` for the full reasoning:

- **Higher-half kernel** (v2): needs a boot-time page directory split between
  physical and virtual addresses before `kmain` can even run. Every
  physical-address computation in `paging.c`/`pmm.c` currently assumes
  virtual == physical.

Ring-3 user mode (v3) shipped: `gdt.c` adds ring-3 code/data segments and a
TSS, `paging.c`'s `paging_set_user()` marks specific pages user-accessible
while everything else stays supervisor-only, and `ring3.c`/`ring3_asm.S`
run a one-shot demo payload in ring 3 that writes a proof-of-execution
marker, then attempts a privileged instruction and faults, verified by an
independent QEMU-monitor memory read of the marker and the kernel's own
correctly-named general-protection fault report, not just "didn't crash".
`ring3test` halts the kernel by design (no process kill/reap exists yet),
reboot after running it. No `int 0x80` syscall gate yet, nothing calls into
the kernel from ring 3 today to need one.

## Verification

`check.sh` is the only automated check: it boots the kernel in QEMU and
asserts the startup banner reached VGA memory. That catches "does it boot
at all." Everything beyond that, does paging actually work, does the disk
driver actually read what it wrote, does the context switch actually swap
stacks correctly, gets verified manually per change (see commit messages
and `roadmap.md`'s per-item notes) against real QEMU-attached disks and
boot-time output, not just "it compiled."
