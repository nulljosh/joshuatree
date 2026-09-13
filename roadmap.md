# os roadmap

Freestanding i386 kernel, no libc. v0 boots in QEMU with VGA text, PS/2
keyboard, RTC clock, and a shell that only knows `help clear echo time
reboot`. Everything below is the standard bare-metal-to-usable-OS path
(same order every OSDev-wiki "Bare Bones" -> "Meaty Skeleton" walkthrough
takes, and roughly what xv6/ToaruOS/Linux 0.01 did in their first months).

## Done
<!-- progress.sh: done-items 10/10 -->
- **v0** (Aug-Sep 2026): boots under QEMU/GRUB, VGA text, polled PS/2 keyboard, RTC clock, shell (`help clear echo time reboot`)
- **v1** (Sep 2026): flat GDT, IDT + CPU exception handlers (`crash` command exercises it), PIC remap, IRQ-driven keyboard, PIT timer (`uptime`)

Full breakdown of what shipped in each: `git log --oneline` or the commit history, not here — this file is the queue, not the changelog.

## v2 — memory (from "one flat blob" to real address space)
- [x] Physical memory manager: bitmap over `mem_upper` from the multiboot info struct, kernel image frames pre-reserved (`pmm.c`, `mem` shell command reports free/total)
- [ ] Paging: identity-map the kernel, enable it, handle page faults
- [ ] Kernel heap: `kmalloc`/`kfree` over the physical allocator
- [ ] Higher-half kernel (map kernel to 0xC0000000+, standard OSDev move)

## v3 — multitasking (more than one thing running)
- [ ] Kernel stacks + context switch (save/restore registers, switch %esp)
- [ ] Round-robin scheduler driven by the PIT tick
- [ ] Basic IPC or at least a wait/sleep primitive
- [ ] User mode: ring 3, TSS, syscall via `int 0x80`

## v4 — storage (data survives reboot)
- [ ] ATA PIO driver (read/write sectors, the simplest disk interface)
- [ ] A real filesystem: FAT16/32 (read support first, most-documented, most tooling) or a small custom one if FAT is too much
- [ ] VFS layer so the shell's `open`/`read` don't care which fs backs them
- [ ] Load and exec a flat binary or minimal ELF from disk

## v5 — the "file explorer" (this is why the project exists)
- [ ] Shell commands: `ls`, `cd`, `cat`, `rm`, `mkdir` over the VFS
- [ ] Simple text-mode file browser (arrow keys, VGA text UI, not just a shell)
- [ ] Basic libc subset: `malloc`, `memcpy`, `strcmp`, etc. for anything above the kernel

## v6 — graphics (text mode won't carry a browser)
- [ ] VESA/VBE linear framebuffer mode instead of VGA text
- [ ] Software framebuffer primitives: pixel, rect, blit, a bitmap font renderer
- [ ] Mouse: PS/2 mouse driver (IRQ12), cursor sprite
- [ ] A minimal windowing surface — even one full-screen buffer counts for v6

## v7 — networking (the "browser" part needs a network stack)
- [ ] NIC driver (RTL8139 or virtio-net — both are the standard QEMU-emulatable choices with tons of reference code)
- [ ] Ethernet/ARP/IP/UDP minimal stack
- [ ] TCP: enough to open one connection and do a raw HTTP GET
- [ ] DNS: enough to resolve a hostname before the GET

## v8 — the actual browser (the point of all of this)
- [ ] HTTP client good enough to fetch a page
- [ ] A tag-soup HTML subset parser (headings, paragraphs, links — not CSS, not JS, v1 is Lynx-level)
- [ ] Render parsed text to the framebuffer with the v6 font renderer
- [ ] Link navigation via keyboard/mouse, back button, that's a browser

## Explicitly parked / non-goals
- SMP (multi-core) — one CPU is plenty until everything above works
- A real filesystem journal / crash-consistency — FAT read support is enough for v4-v5
- Wi-Fi — wired NIC only, QEMU doesn't emulate Wi-Fi hardware anyway
- Anything GUI-toolkit-shaped (widgets, themes) before v6's raw framebuffer works
- gato (the macOS voice kiosk, `~/Documents/Code/gato`) — different project, different repo, on purpose. Voice control of this kernel is a real future idea but nowhere near the front of the queue.
