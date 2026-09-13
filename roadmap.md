# Joshua Tree roadmap

Freestanding i386 kernel, no libc. v0 boots in QEMU with VGA text, PS/2
keyboard, RTC clock, and a shell that only knows `help clear echo time
reboot`. Everything below is the standard bare-metal-to-usable-OS path
(same order every OSDev-wiki "Bare Bones" -> "Meaty Skeleton" walkthrough
takes, and roughly what xv6/ToaruOS/Linux 0.01 did in their first months).

## Done
<!-- progress.sh: done-items 10/10 -->
- **v0** (Aug-Sep 2026): boots under QEMU/GRUB, VGA text, polled PS/2 keyboard, RTC clock, shell (`help clear echo time reboot`)
- **v1** (Sep 2026): flat GDT, IDT + CPU exception handlers (`crash` command exercises it), PIC remap, IRQ-driven keyboard, PIT timer (`uptime`)

Full breakdown of what shipped in each: `git log --oneline` or the commit history, not here, this file is the queue, not the changelog.

**Total remaining, roughly: 10-14 sessions (~30-45 active hours)** to v10, per-version ETAs below. These are active-work estimates, not calendar time, usage caps and check.sh-quality verification pace it across days, not one continuous run. Revised after each version actually ships, not predicted once and left stale.

**Model tag on each open item**: `[Haiku]` is mechanical work with a known-correct shape, glue between pieces that already exist, or scoped review/polish, cheap to run and low risk if it's slightly off since check.sh plus a quick look catches it. `[Fable]` is anything where a subtly wrong answer looks fine and boots fine (privilege isolation, a stack frame layout, a wire protocol's exact byte layout, memory model changes), needs the deeper reasoning pass and the fuller verification this session already uses for those. `[Joshua]` isn't code at all, a real design or scope call. Tags are a starting guess for whoever or whatever picks up an item next, not a lock, re-tag if an item turns out easier or harder once actually opened.

## v2, memory (from "one flat blob" to real address space)
- [x] Physical memory manager: bitmap over `mem_upper` from the multiboot info struct, kernel image frames pre-reserved (`pmm.c`, `mem` shell command reports free/total)
- [x] Paging: identity-map the first 4MB, enable it (`paging.c`); page faults now report the faulting address from CR2 (`pagefault` shell command exercises it)
- [x] Kernel heap: `kmalloc`/`kfree`, first-fit free list grown a frame at a time via `pmm_alloc_frame` (`kheap.c`, `heaptest` shell command)
- [ ] [Fable] Higher-half kernel (map kernel to 0xC0000000+), **deliberately deferred, not blocked**: touches boot.S (needs a boot-time PSE page directory + physical/virtual split before `kmain` can even be called), linker.ld (dual VMA/LMA per section), and paging.c + pmm.c (every physical-address computation there currently assumes virtual==physical and has to subtract 0xC0000000). Real risk of a subtly-broken kernel that still passes the crude banner check. Wants a dedicated pass with more careful verification than this loop's `check.sh`, not to be squeezed in at the tail of a long session.

## v3, multitasking (more than one thing running)
- [x] Kernel stacks + context switch: cooperative round-robin, `yield()` swaps ESP + callee-saved registers (`task.c`, `task_switch.S`). Verified `ABABAB...` interleaving at boot before shipping. `tasktest` shell command exists but its Enter-key output can't be confirmed through the QEMU-monitor `sendkey` test harness (known limitation, see below), the boot-time verification is the real evidence
- [ ] [Fable] Preemptive scheduling off the PIT tick, deferred: needs the timer IRQ handler itself to call the switch, which means every task's initial stack must exactly mimic the IRQ frame layout (`pusha`+vector+CPU frame), not just `yield()`'s simpler callee-saved layout. Real risk of a subtly wrong stack frame. Do this as its own focused pass once cooperative switching has been exercised more (real second/third tasks, not just the two-letter demo)
- [x] Wait/sleep primitive: `sleep_ticks(n)` yields until n PIT ticks pass (`task.c`, `sleep` shell command). Verified it actually returns via a temporary boot-time check before shipping
- [ ] [Fable] User mode: ring 3, TSS, syscall via `int 0x80`, **deliberately deferred, not blocked**: needs new GDT entries (ring-3 code/data + a TSS descriptor), a TSS with a valid ss0/esp0, `paging.c`'s page tables switched from supervisor-only to user-accessible, and a correct `iret`-based privilege transition. Same failure mode as higher-half: a subtle bug here (wrong RPL, wrong TSS field) can leave ring-3 code silently running with ring-0 privileges while `check.sh`'s boot-banner check still passes, it can't detect a privilege-isolation bug, only a crash. Wants real verification (does ring-3 code actually fault on a privileged instruction) before shipping, not a rushed pass.

## v4, storage (data survives reboot), done (Sep 2026)
- [x] ATA PIO driver: LBA28 read/write on the primary master (`ata.c`, `disktest` shell command). Verified with a real attached disk image (`write:ok read:ok match:ok`), not just the graceful-no-drive path
- [x] FAT16 read support: root directory only, 8.3 names (`fat.c`, `ls`/`cat` shell commands). Verified against a real FAT16 image made with macOS `newfs_msdos` containing an actual file, not a hand-rolled test fixture (`mount:ok`, real directory listing, `cat:ok -> hello from fat16, real filesystem test`)
- [ ] [Haiku] VFS layer so the shell's `open`/`read` don't care which fs backs them, lower priority now: FAT is the only filesystem that exists, so there's nothing to abstract over yet
- [x] Load and exec a flat binary from disk (`exec.c`, `exec` shell command; runs in ring 0, no isolation, v3's ring-3 work is deferred, documented on the command itself). Verified end to end: a real flat binary assembled outside the kernel, placed on a real FAT image, loaded via `fat_read_file`, and actually executed (`exec:ok`, and its own VGA write appeared exactly where expected)

## v5, the "file explorer", done (Sep 2026)
Mechanical once v4's VFS exists, mostly shell commands and a UI loop.
- [x] `ls`/`cat` already exist (v4). `rm`: real delete, marks the directory entry `0xE5` and rewrites that sector (`fat.c`'s `fat_delete`, `rm` shell command). Verified against a real FAT image: file present → `rm` → gone from `ls`, unrelated files untouched
- [ ] [Fable] `cd`/`mkdir` need actual subdirectories, which don't exist yet, root-directory-only was v4's deliberate scope call (see `fat.c`'s header comment). Adding real subdirectory traversal is its own small chunk of work, not a one-liner alongside `rm`
- [x] Text-mode file browser: arrow keys, Enter to view, Esc/q to quit (`browse` shell command, `get_key()` handles extended scancodes for arrows). Data-collection path verified against a real FAT image with real files (correct names/sizes); the interactive navigation itself can't be confirmed through the QEMU `sendkey` harness (same known limitation as Enter elsewhere), so it rests on the verified collection logic plus already-proven `puts`/`putn` rendering
- [x] Basic libc subset: `memcpy`/`memset`/`memcmp`/`strlen`/`strcmp` (`libc.c`), not speculative, actually replaced real duplicate loops (`kernel.c`'s 19 `streq` call sites, `fat.c`'s `names_eq`)

## v6, graphics (text mode won't carry a browser), mostly done (Sep 2026), font renderer deferred
A real fork discovered mid-implementation: requesting a video mode via the multiboot header (the obvious first approach) makes QEMU boot straight into graphics mode with no way back to VGA text, breaking the working shell until the font renderer exists too, since text and framebuffer output can't coexist that way. The better path is switching graphics on and off at runtime via QEMU's Bochs VBE register interface (ports 0x1CE/0x1CF), which needs the framebuffer's real physical address first, only PCI config space knows that, not a fixed constant.
- [x] PCI enumeration: scans config space via ports 0xCF8/0xCFC, finds QEMU's VGA device and reads its BAR0 (`pci.c`, `lspci` shell command). Verified against the real device: boot-time check printed `pci:ok VGA BAR0=0xfd000000`, the actual address QEMU's std VGA framebuffer lives at
- [x] VESA/VBE linear framebuffer mode via the Bochs VBE register interface (`vbe.c`, `gfxtest` shell command, `paging_map_region` extends identity-mapping to wherever the framebuffer's real physical address lands). Verified with a real screenshot of a real QEMU window, three colored bands rendered exactly as coded, the first real graphics this kernel has ever produced. `vbe_disable()` returns cleanly to the text shell
- [x] Software framebuffer primitives (pixel, rect, blit): `pixels[y*800+x]=color` fills already proven correct in `gfxtest`, worth factoring into named helpers when the next thing that needs them shows up, not speculatively now
- [ ] [Fable] Bitmap font renderer, **deliberately deferred, not blocked**: hand-authoring bitmap glyph data for 95+ ASCII characters at real quality in one sitting is exactly the "garbled but technically renders" trap this item already warns against, a single bit-order or glyph-index mistake produces wrong-looking text that still "works." Needs real, carefully-sourced font data (a known public-domain bitmap font, verified glyph by glyph, not authored blind) as its own focused pass. Still support more than one embedded font and let the user pick once it happens, that requirement doesn't go away just because it's deferred
- [x] PS/2 mouse driver: 8042 enable sequence, IRQ12 packet parsing, `mousetest` draws a tracking cursor square in graphics mode (`mouse.c`, PIC cascade + IRQ12 unmasked in `pic.c`). Verified the real handshake: boot-time check read back `ack1=0xfa ack2=0xfa`, the genuine PS/2 ACK byte, twice, from QEMU's actual mouse emulation, not a timeout artifact. Full cursor-movement screenshot verification was blocked by a macOS Spaces/window-focus issue tonight (screencapture kept grabbing an unrelated window on another Space), not a kernel problem, worth re-confirming visually next time a display is easy to reach
- [x] Minimal windowing surface: `window.c` wraps the full-screen framebuffer (`window_open`/`clear`/`pixel`/`rect`/`close`) so callers stop reimplementing the same pixel loop. `gfxtest`/`mousetest` refactored onto it, re-verified with a fresh screenshot after the refactor, same three bands rendered correctly through the new abstraction
- [ ] [Haiku] Memory efficiency pass: this is a real standing constraint from here on, not a one-time task. Watch static allocations (paging.c's extra page tables, kheap's growth), avoid needless copies, keep the kernel's own footprint small before it starts hosting real app logic in v9. Revisit whenever a subsystem's memory use looks bigger than it needs to be, not just once
- [ ] [Haiku] Product design pass: also a standing concern, not one task. The warm palette/copy discipline that landed on the landing page belongs on the kernel side too once there's a UI worth looking at, consistent colors, deliberate layout, not just "does the pixel show up." Revisit once the font renderer and windowing surface have real content to arrange, not before there's anything to design

Once graphics exist, a lighter early win becomes possible without waiting for v7/v8's full network stack and browser: pure-logic apps from the codebase (no DOM, no network dependency, e.g. numen's calculator parser, keyrate's typing-test scoring, weather's forecast math minus the live fetch) can be ported natively in C and rendered straight to the framebuffer, no HTTP client or HTML parser needed. This is real groundwork for v9, not a replacement for it: the full vision (real web apps served and rendered by the real browser) still needs v7 and v8. Worth a small side-track once the framebuffer and font renderer above are solid, picking one simple app's core logic (not its whole UI) as the first native port.

## v7, networking (the "browser" part needs a network stack), done (Sep 2026)
The hardest version in the plan: a NIC driver plus a real TCP stack, both easy to get subtly wrong in ways that "sort of work."
- [x] PCI extended for I/O-space BARs (RTL8139's BAR0 is an I/O port range, not a physical memory address like the VGA framebuffer was) plus `pci_enable_device` to set the I/O/memory/bus-master bits ourselves, since a kernel entered directly via multiboot has no BIOS pass that already did it. Verified against a real `-device rtl8139` QEMU NIC: boot-time check read back `vga:ok nic:ok io=0xc000`, correctly distinguishing the two BAR types
- [x] RTL8139 driver: reset, RX/TX buffer setup, accept-all-packets mode, raw frame send and receive (`rtl8139.c`, `nettest` shell command). Polled, no IRQ wiring yet, hlt-between-checks is enough now that a real protocol polls it. Verified against real hardware, not fixtures: read back MAC `52:54:00:12:34:56`, QEMU's actual documented default NIC MAC, and a real frame transmit that the card genuinely acknowledged (`send:ok`, TOK bit set)
- [x] Ethernet/ARP/IP/UDP minimal stack (`net.c`: `net_init`, `arp_resolve`, `udp_send`), no routing table, single-link only, correct for QEMU's SLIRP network. Found and fixed a real bug this way, not just "boots": the first version reused TX descriptor 0 for every send, which works exactly once and then stalls forever with OWN/TOK both stuck at 0 (the card expects the driver to cycle its 4 TX descriptors in order). Verified with a real `tcpdump` capture on QEMU's netdev filter-dump, not just in-kernel self-report: watched the actual ARP request leave the wire and a real reply come back from `10.0.2.2` as `52:55:0a:00:02:02`, matching what the shell printed
- [x] TCP: full handshake (SYN/SYN-ACK/ACK), one request, reading the response across as many segments as arrive, then closing (`tcp_get` in `net.c`, extends `nettest`). Found and fixed two real bugs along the way, not just "boots": (1) ARP was being sent straight to the destination IP even for real internet hosts, which nothing on the LAN can ever answer, since ARP only resolves same-link addresses, fixed with the one routing rule that matters here, same-subnet gets ARP'd directly, anything else goes to the default gateway's MAC while the IP header keeps the real destination; (2) a fixed-iteration poll "timeout" that got lucky once against a 14ms reply and then lost outright against a real 300ms one, since a real DNS/TCP round trip over the actual internet has no fixed bound, fixed by giving WAN-crossing waits a far larger budget than the LAN-only ARP wait. Verified with a real `tcpdump` capture, not just the shell's own printed answer: a full connection to example.com over the actual internet, `tcpdump` independently confirmed every checksum correct, a real 827-byte HTTP/1.1 200 response came back with the real page body, and both sides closed cleanly. v7 is done.
- [x] DNS: single-question A-record query over `udp_send`/`arp_resolve`, name compression handled on the receive side only (`dns_resolve` in `net.c`, extends `nettest`). Verified with a real `tcpdump` capture, not just the shell's own printed answer: watched a real query for `example.com` leave the wire to QEMU SLIRP's built-in resolver at 10.0.2.3, and a real reply come back forwarded from the host's actual DNS with two A records, `172.66.147.243` and `104.20.23.154`, matching the first one the kernel printed

## v8, the actual browser (the point of all of this), ETA: 1-2 sessions (~4-6h)
Mostly glue over v6+v7 once both exist; the HTML parser is deliberately tiny.
- [ ] [Haiku] HTTP client good enough to fetch a page
- [ ] [Haiku] A tag-soup HTML subset parser (headings, paragraphs, links, not CSS, not JS, v1 is Lynx-level)
- [ ] [Haiku] Render parsed text to the framebuffer with the v6 font renderer
- [ ] [Haiku] Link navigation via keyboard/mouse, back button, that's a browser

## v9, running real apps (the browser can now load something), ETA: 1 session (~2-3h)
- [ ] [Haiku] Serve the codebase's own static web apps (weather, numen, fieldbook, etc., one-file, no-backend apps from `~/Documents/Code`) over the v7 network stack to the v8 browser
- [ ] [Fable] A tiny local HTTP server on the kernel itself, so apps run without needing an external host
- [ ] [Haiku] Pick 2-3 of the simplest static apps as the first real test load, not all 30+ at once

## v10, talking to it like gato does (this is the actual point), ETA: 1-2 sessions (~3-5h), plus a real design decision along the way
- [ ] [Fable] Wire a voice or text command channel into the kernel shell that can reach an LLM (needs v7's network stack to call out, or an on-device model if that's ever feasible on bare metal)
- [ ] [Fable] "build stuff" loop: a command that takes a request, edits/generates a file, serves it as a v9 app, the kernel-native version of what gato already does on macOS
- [ ] [Joshua] Decide then whether voice I/O belongs in the kernel itself or stays a gato-style layer that talks to this OS over the network (real design call, not a default, flag it when v10 is reached instead of guessing)

## Explicitly parked / non-goals
- SMP (multi-core), one CPU is plenty until everything above works
- A real filesystem journal / crash-consistency, FAT read support is enough for v4-v5
- Wi-Fi, wired NIC only, QEMU doesn't emulate Wi-Fi hardware anyway
- Anything GUI-toolkit-shaped (widgets, themes) before v6's raw framebuffer works
- gato (the macOS voice kiosk, `~/Documents/Code/gato`), different project, different repo, on purpose. Voice control of this kernel is a real future idea but nowhere near the front of the queue.
