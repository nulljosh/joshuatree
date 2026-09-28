# Building an OS, mapped to Joshua Tree

A reference series: [Building an OS](https://www.youtube.com/playlist?list=PLFjM7v6KGMpiH2G-kT781ByCNC_0pKpPN) (nanobyte). Each topic, and where Joshua Tree stands on it. Update this table in the same PR that changes a row.

| Topic | Joshua Tree | Where |
|---|---|---|
| 1. Hello world, booting | Done. GRUB loads our kernel; the ISO boots from a USB stick. | boot/boot.S |
| 2. Reading from the disk | Done. | drivers/ata.c |
| 3, 6. FAT, subdirectories; Live: FAT32 | Open. Our disk format is our own, so a normal USB stick or SD card won't open. | roadmap: FAT32 |
| 4, 7. Writing a bootloader in C | Skipped on purpose. GRUB already does it. | |
| 5. printf | Done. | kernel/ |
| Protected mode | Done. Apps are moving to ring 3 for 2.0. | kernel/gdt.c, kernel/ring3app.c |
| Dev environment, GCC toolchain | Done, with clang and lld. | Makefile |
| 8, 9, 10. GDT, IDT, interrupts, PIC | Done. | kernel/gdt.c, kernel/irq.c, kernel/pic.c |
| Writing logs from your OS | Done. Everything logs over serial, and the checks read it. | tools/checks/ |
| Booting from hard drives, MBR | Open. Install to disk from the USB stick. | roadmap: install to disk |
| Better debugging with ELF | Done: a panic names the function (1.7.0). | kernel/ |
| Loading ELF programs from disk | Partial: Keyrate and Toroid are saved as files and launched from them by one table-driven launcher (flat binaries, not ELF yet). | kernel/ring3app.c |
| C++ | Skipped on purpose. C only. | |
| 11. Memory detection | Done, from the boot memory map. | kernel/pmm.c |
| Memory management 1 and 2 | Done: physical pages and paging. One address space per app is part of 2.0. | kernel/pmm.c, kernel/paging.c |
