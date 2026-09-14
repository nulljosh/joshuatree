# Architecture

What each file does and how boot actually proceeds. `roadmap.md` is the plan;
this is the map of what exists right now.

## Boot sequence

Higher-half kernel: everything from `kmain` on runs at 0xC0000000+, loaded
physically at 1MB. `boot/boot.S`'s `_start` (deliberately unrelocated, it
runs both before and after paging turns on) builds a temporary page
directory mapping the first 4MB both identity and at the high base, turns
on paging, jumps into the real kernel. `paging_install()` then replaces
that with the permanent, same-shaped tables.

1. `boot/boot.S`: multiboot1 header, temporary page tables, paging on,
   jump to the higher-half-linked `higher_half_entry`, which sets the real
   stack, pushes the multiboot info pointer GRUB/QEMU left in `%ebx`, calls
   `kmain`.
2. `kmain` (`kernel/kernel.c`) brings subsystems up in dependency order:
   `gdt_install` → `idt_install` → `irq_install` → `pmm_init` →
   `paging_install` → `tasks_init` → `fat_mount` → the shell loop.

## Subsystems

| File | What it owns |
|---|---|
| `gdt.c` | Flat GDT: ring-0 and ring-3 code/data segments (both spanning 4GB) plus a TSS for ring-3-to-ring-0 stack switches |
| `idt.c` + `isr.S` | IDT, the 32 CPU-exception handlers, and the `int 0x80` entry. A ring-0 exception prints and halts (a kernel bug); a ring-3 one names itself, reaps the task, and the kernel keeps running (v64) |
| `syscall.c` | `int 0x80` dispatch table, x86 Linux convention and numbers: `SYS_EXIT` (1), `SYS_WRITE` (4). User pointers are checked against the page tables before the kernel reads them (v64) |
| `pic.c` | Remaps the 8259 PIC so IRQs land on vectors 32-47 instead of overlapping CPU exceptions |
| `irq.c` + `irq_stubs.S` | IRQ0 (PIT tick counter) and IRQ1 (keyboard ring buffer) |
| `pmm.c` | Physical memory: a bitmap over `mem_upper` from the multiboot info struct |
| `paging.c` | Permanent page tables: identity-maps the first 4MB and double-maps it at 0xC0000000 for the kernel's own higher-half code/data |
| `kheap.c` | `kmalloc`/`kfree`, a first-fit free list grown a frame at a time from `pmm.c` |
| `task.c` + `irq_stubs.S`'s irq0 | Preemptive round-robin off the PIT tick. `yield()` reaches the same switch in software via `int $32`, same IDT gate as the hardware timer. Ring-3 tasks sit in the same table since v64 (`task_create_user`), each with its own kernel stack that the TSS `esp0` is repointed to on every switch; the saved frame carries DS/ES/FS/GS so a resume into ring 3 keeps its own selectors |
| `ata.c` | ATA PIO disk driver, LBA28, primary master only |
| `fat.c` | FAT16, real subdirectories and file writes, 8.3 names |
| `exec.c` | Loads a flat binary via `fat.c` and calls into it, ring 0, no isolation |
| `drivers/mouse.c` + `drivers/vmmouse.c` | Pointer input. `mouse.c` is the PS/2 mouse on the 8042's second port (IRQ12, 3-byte relative packets). `vmmouse.c` (v62) probes the VMware absolute-pointer backdoor on I/O port 0x5658 at boot; where a host answers (QEMU's default pc machine, v86 in the browser) it switches the host to absolute mode and the GUI takes positions from it, PS/2 bytes still drained for phase but ignored. No backdoor (bare hardware, `-machine vmport=off`): PS/2 relative stays the only mouse |
| `kernel/kernel.c` | VGA text console, PS/2 scancode table, RTC clock, the shell, and a mouse-driven GUI desktop (`gui`) built on v6's graphics/font/mouse primitives. The GUI runs at 960x540 logical, scaled 2x to 1920x1080 physical; dock geometry and app layout live here too |

## Why things are ordered this way

Each subsystem depends on the ones above it: paging needs `pmm.c`'s frames,
the heap needs paging to be sane, tasks need the heap for their stacks, `fat.c`
needs `ata.c`'s sectors, `exec.c` needs `fat.c`'s files. Bringing them up
out of order is the fastest way to a silent, hard-to-diagnose bug.

## What's here now that used to be deferred

Higher-half kernel (v2) shipped: see the Boot sequence section above.
`paging.c`/`pmm.c` are the two files that needed physical-vs-virtual care;
`rtl8139.c` needed the same care for a different reason, the NIC does raw
physical-memory DMA and doesn't know what a page table is.

Ring-3 user mode (v3) shipped: `gdt.c` adds ring-3 code/data segments and a
TSS, `paging.c`'s `paging_set_user()` marks specific pages user-accessible
while everything else stays supervisor-only, and `ring3.c`/`ring3_asm.S`
copy a hand-written payload onto a user page and run it at CPL 3. As of
v64 that payload is a real scheduled task (`task_create_user`) with a
real `int 0x80` gate to call: `ring3test` writes a line through
`SYS_WRITE`, stores the kernel's return value in a marker from ring 3, and
ends with `SYS_EXIT`; `ring3test fault` keeps the v3 privileged-instruction
payload and is reaped by `idt.c`'s ring-3 exception path instead of halting
the machine; `ring3test spin` proves preemption and `task_kill` on a ring-3
task. All three come back to the shell. Verified by independent
QEMU-monitor reads of the marker plus the kernel's own correctly-named
reports, not just "didn't crash"; see `roadmap.md`'s v3 and v64 entries.

## Verification

`check.sh` is the only automated check: it boots the kernel in QEMU and
asserts the startup banner reached VGA memory. That catches "does it boot
at all." Everything beyond that, does paging actually work, does the disk
driver actually read what it wrote, does the context switch actually swap
stacks correctly, gets verified manually per change (see commit messages
and `roadmap.md`'s per-item notes) against real QEMU-attached disks and
boot-time output, not just "it compiled."
