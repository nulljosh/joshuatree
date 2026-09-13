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
- [x] Paging: identity-map the first 4MB, enable it (`paging.c`); page faults now report the faulting address from CR2 (`pagefault` shell command exercises it)
- [x] Kernel heap: `kmalloc`/`kfree`, first-fit free list grown a frame at a time via `pmm_alloc_frame` (`kheap.c`, `heaptest` shell command)
- [ ] Higher-half kernel (map kernel to 0xC0000000+) — **deliberately deferred, not blocked**: touches boot.S (needs a boot-time PSE page directory + physical/virtual split before `kmain` can even be called), linker.ld (dual VMA/LMA per section), and paging.c + pmm.c (every physical-address computation there currently assumes virtual==physical and has to subtract 0xC0000000). Real risk of a subtly-broken kernel that still passes the crude banner check. Wants a dedicated pass with more careful verification than this loop's `check.sh`, not to be squeezed in at the tail of a long session.

## v3 — multitasking (more than one thing running)
- [x] Kernel stacks + context switch: cooperative round-robin, `yield()` swaps ESP + callee-saved registers (`task.c`, `task_switch.S`). Verified `ABABAB...` interleaving at boot before shipping. `tasktest` shell command exists but its Enter-key output can't be confirmed through the QEMU-monitor `sendkey` test harness (known limitation, see below) — the boot-time verification is the real evidence
- [ ] Preemptive scheduling off the PIT tick — deferred: needs the timer IRQ handler itself to call the switch, which means every task's initial stack must exactly mimic the IRQ frame layout (`pusha`+vector+CPU frame), not just `yield()`'s simpler callee-saved layout. Real risk of a subtly wrong stack frame. Do this as its own focused pass once cooperative switching has been exercised more (real second/third tasks, not just the two-letter demo)
- [x] Wait/sleep primitive: `sleep_ticks(n)` yields until n PIT ticks pass (`task.c`, `sleep` shell command). Verified it actually returns via a temporary boot-time check before shipping
- [ ] User mode: ring 3, TSS, syscall via `int 0x80` — **deliberately deferred, not blocked**: needs new GDT entries (ring-3 code/data + a TSS descriptor), a TSS with a valid ss0/esp0, `paging.c`'s page tables switched from supervisor-only to user-accessible, and a correct `iret`-based privilege transition. Same failure mode as higher-half: a subtle bug here (wrong RPL, wrong TSS field) can leave ring-3 code silently running with ring-0 privileges while `check.sh`'s boot-banner check still passes — it can't detect a privilege-isolation bug, only a crash. Wants real verification (does ring-3 code actually fault on a privileged instruction) before shipping, not a rushed pass.

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

## v9 — running real apps (the browser can now load something)
- [ ] Serve the codebase's own static web apps (weather, numen, fieldbook, etc. — one-file, no-backend apps from `~/Documents/Code`) over the v7 network stack to the v8 browser
- [ ] A tiny local HTTP server on the kernel itself, so apps run without needing an external host
- [ ] Pick 2-3 of the simplest static apps as the first real test load, not all 30+ at once

## v10 — talking to it like gato does (this is the actual point)
- [ ] Wire a voice or text command channel into the kernel shell that can reach an LLM (needs v7's network stack to call out, or an on-device model if that's ever feasible on bare metal)
- [ ] "build stuff" loop: a command that takes a request, edits/generates a file, serves it as a v9 app — the kernel-native version of what gato already does on macOS
- [ ] Decide then whether voice I/O belongs in the kernel itself or stays a gato-style layer that talks to this OS over the network (real design call, not a default — flag it when v10 is reached instead of guessing)

## Explicitly parked / non-goals
- SMP (multi-core) — one CPU is plenty until everything above works
- A real filesystem journal / crash-consistency — FAT read support is enough for v4-v5
- Wi-Fi — wired NIC only, QEMU doesn't emulate Wi-Fi hardware anyway
- Anything GUI-toolkit-shaped (widgets, themes) before v6's raw framebuffer works
- gato (the macOS voice kiosk, `~/Documents/Code/gato`) — different project, different repo, on purpose. Voice control of this kernel is a real future idea but nowhere near the front of the queue.
