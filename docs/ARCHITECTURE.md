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
| `ring3.c` + `ring3_asm.S` | Ring-3 privilege isolation and user-mode task execution. Copies a payload onto a user page and runs it at CPL 3 with restricted instruction set (privileged instructions fault). Since v64, user tasks sit in the same task table as kernel tasks (`task_create_user`), with `int 0x80` as the syscall gate to write/exit. `ring3test` / `ring3test fault` / `ring3test spin` verify the three cases: normal syscall, privileged-instruction fault, and preemption on a ring-3 task (v3 / v64) |
| `ata.c` | ATA PIO disk driver, LBA28, primary master only |
| `fat.c` | FAT16, real subdirectories and file writes, 8.3 names |
| `exec.c` | Loads a flat binary via `fat.c` and calls into it, ring 0, no isolation |
| `drivers/vfs.c` | v29's real VFS: a `vfs_ops` table (`read_file`/`list`/`delete_`/`chdir`/`mkdir`/`write_file`/`replace_file`) so callers stop dialing `fat_*` directly, correctly deferred at v4 until a second backend actually existed to abstract over. `vfs_switch()` (the `fsuse` shell command) flips the active backend; `fat.c` and `ramfs.c` each register one at boot, first-registered wins by default |
| `drivers/blockdev.c` | v33's block device abstraction, the same "second implementation, not an imagined future one" reasoning one layer down: a `blockdev_ops` table of `read_sector`/`write_sector` so `fat.c` no longer calls `ata_*` directly. `ata.c` and `ramdisk.c` each register a backend at boot |
| `drivers/ramfs.c` + `drivers/ramdisk.c` | The real second implementations `vfs.c` and `blockdev.c` needed to prove each abstraction actually abstracts something. `ramfs.c`: 8 files, 4KB each, a flat namespace on purpose (`chdir`/`mkdir` honestly return failure, no subdirectory story exists). `ramdisk.c`: 256 512-byte sectors (128KB) of plain zeroed RAM, deliberately small, this proves the interface, it isn't meant to hold real data |
| `drivers/trash.c` | v39's real, recoverable delete: before this, `rm` went straight to FAT's own `0xE5`-marks-the-entry removal, which doesn't even free the clusters (see `fat.c`'s own note), bytes gone with no way back. `trash_put` copies a file's bytes into an 8-item, 4KB-each RAM table before the caller deletes it; `trash_restore` writes it back through `vfs_write_file` and only drops the RAM copy if that write actually succeeds, so a failed restore never loses the file twice. RAM-only and gone on reboot, an honest tradeoff against a disk-backed trash's own reserved-directory and collision-naming problems, neither of which this kernel has an answer for yet |
| `drivers/pci.c` | Legacy 0xCF8/0xCFC PCI config-space enumeration: an exhaustive bus/slot/function scan (`pci_find_device`, QEMU's device tree is tiny enough that this costs microseconds) plus the bus-master/IO/memory enable bits (`pci_enable_device`). Shared foundation under two later subsystems: v6's graphics (finds the VBE framebuffer's BAR0) and v7's networking (finds the RTL8139's I/O BAR) |
| `drivers/rtl8139.c` | The real NIC driver behind v7's networking: register-level reset/init, one RX ring buffer (polled on the Command Register's BUFE bit since v71 / 0.65.0, the OSDev/Linux `RxBufEmpty` recipe; the old ISR-ROK gate ran one packet behind after any two-frame burst, hidden for sixty versions because every caller reset the NIC before its single connection, exposed the day `weather_fetch` made two), and 4 separate TX descriptor buffers cycled in order (reusing one descriptor for every send works exactly once, then stalls the card forever, found via a real pcap capture, not reasoning). Needed v2's higher-half fix (`KVIRT_TO_PHYS`) since the card DMAs to physical addresses with no concept of the CPU's page tables at all, a bug MMIO register reads (the MAC readout) never exposed, only an independent `tcpdump` capture of all-zero frames did |
| `drivers/net.c` | Ethernet/ARP/IPv4/UDP/TCP built up from raw frames over `rtl8139.c`. Deliberately narrow: no routing table (assumes every destination is on the same /24 link, true for QEMU's SLIRP), no fragmentation, `tcp_get`/`tcp_serve_once` are one connection at a time with no retransmission or out-of-order reassembly. Extended post-v9 to chunk a response across multiple 536-byte segments stop-and-wait (the original single-segment cap silently truncated real pages); v10 replaced every wait's loop-iteration guess with a real `ticks()`-based deadline after proving under host load that a spin count isn't a real timeout; v30 added `tcp_probe_port`, a short-timeout SYN probe behind the `netscan` hacking-tools command. v75: `tcp_get` takes a fresh local port per connection (44000..59999, wrapping) and varies its ISN by it; a fixed port 44000 was fine while every caller spoke to a different host once per boot, but the map wallpaper's twelve back-to-back connections to one server hit a 4-tuple SLIRP still held half-closed (this side FINs and never waits for the last ACK, by design), reproducibly failing on the fourth |
| `drivers/http.c` + `drivers/html.c` + `drivers/json.c` | v8's browser layer, mostly glue over v6+v7, the HTML parser deliberately tiny. `http.c`: `http_get`/`http_post` build a request over `net.c`'s `tcp_get` and strip the reply down to the body past the first blank line. v75: `http_get`'s reply buffer is kmalloc'd at `body_maxlen + 2048`, replacing a fixed 2KB stack buffer that silently capped every reply (headers included) at 2047 bytes, fine for JSON one-liners, hopeless for a 40KB map tile. `html.c`: `html_to_text` strips tags (skipping `<script>`/`<style>` element *content* instead of printing it as page text, a real bug found live against example.com's own stylesheet) and unescapes the five basic entities; bytes above 0x80 are dropped rather than mis-rendered, a real bug (multi-byte UTF-8 landing on CP437's line-drawing glyphs, reading as a screen-wide stripe pattern) caught the same way. `json.c`: `json_extract_string` is a key-value grabber, not a parser, string values only, no arrays/nesting, enough to read one field out of an LLM API's JSON reply; `json_extract_number_text` (v71 / 0.65.0) is its numeric sibling, copying a bare number's exact text (`"lat":49.0983` -> `49.0983`) so `weather_fetch` can splice ip-api.com's coordinates straight into its Open-Meteo URL with no float round trip |
| `drivers/vbe.c` | The Bochs VBE register interface (ports 0x1CE/0x1CF), the only path QEMU's std VGA device actually exposes to a 32-bit kernel with no real-mode BIOS monitor. `vbe_set_mode`/`vbe_disable` switch graphics on and off at runtime, saving and restoring the real VGA Sequencer/CRTC/GC/Attribute-Controller register state around the switch (a real v6/v8 bug: `vbe_disable` alone left those registers in a graphics-mode state, breaking the shell it was supposed to return to). `vga_text_mode_init` (v16) programs mode 3 and the 16-color DAC palette from first principles, since v86 (no BIOS) never sets either up the way real hardware/QEMU do for free |
| `drivers/font.c` | Dumps the real IBM CP437 8x16 font out of VGA hardware plane 2 (the OSDev-wiki technique), not hand-authored glyph bitmaps. Falls back to an embedded table (`vgafont.h`) when the dump comes back all zero (v38: v86 has no BIOS to have loaded a font for it to steal). `font_set_aa`'s hook (v44) lets the GUI swap this bitmap path for a real antialiased DejaVu Sans/Serif/Mono render at physical resolution wherever a scaled framebuffer exists. Since v77 (0.67.1) the AA path lays strings out with a real pen advanced by each glyph's own table metric (`font_set_aa` takes a second advance hook), the fix for the photographed "Cl oudy" menu-bar gap that the old fixed 16px-per-glyph cell caused; `font_string_width` reports the real width and every alignment site uses it instead of `strlen*8`. Single-glyph `font_draw_char` callers (terminal grid) keep the fixed cell. Regression: `texttest` / `tools/textspacing-check.sh` |
| `drivers/window.c` | The one render-target abstraction every draw call in this kernel goes through: `window_pixel`/`window_rect`/`window_clear`, a viewport, an optional offscreen "screen band" for partial repaints, and a single redirectable supersample target (icons render into it at several times real size, then box-downsample for real anti-aliasing). v41's `window_open_scaled` opens a physical mode `s` times the logical one so `window_pixel` quietly fans out into an sxs block, the real fix for "the pixels are too big," not more vector polish on top of an 800x600 ceiling |
| `kernel/console.h` | Three-function VGA text-console prototypes (`putc`/`puts`/`puthex`), the header `kernel.c`'s own console implementation exposes to the rest of the kernel before the GUI exists |
| `lib/libc.c` | `memcpy`/`memset` and the other handful of freestanding primitives this kernel can't pull from a real libc (there isn't one), used everywhere byte-level copying is needed |
| `drivers/png.c` | v74 (0.66.0): minimal PNG decoder, the "prerequisite bridge" the satellite-wallpaper goal was blocked on. Scope is deliberately narrow and enforced (8-bit RGB/RGBA, and since v75 8-bit indexed with a PLTE lookup on output, because every real plain-HTTP map tile server found serves paletted PNGs; non-interlaced; anything else returns `PNG_E_UNSUPPORTED` rather than misdecoding, and an indexed image with no PLTE or an index past the palette is `PNG_E_FORMAT`), but underneath it is a real RFC 1950/1951 zlib inflater (stored, fixed-Huffman and dynamic-Huffman blocks, canonical-Huffman decode in the zlib "puff" shape, no recursion, ~300 bytes of stack tables) plus all five RFC 2083 scanline filters (None/Sub/Up/Average/Paeth). Chunk CRC-32s and the zlib Adler-32 trailer are both verified, so a corrupted byte fails loudly (`PNG_E_CRC` / `PNG_E_ZLIB`) instead of returning garbage pixels. Output is one kmalloc'd `w*h*channels` buffer in exactly `wallpaper_rgb`'s byte layout. Tested two ways: `tools/png-host-check.sh` compiles the same file natively against `tools/png-host/` shims (seconds), and the in-kernel `pngtest` decodes four real PNGs cut from the wallpaper itself (three truecolor, one 40-color indexed since v75, compared by host hash since its pixels are palette lookups of a quantized crop) (`drivers/png_testdata.h`, generated by `tools/gen_png_testdata.py` with every filter type cycled per row and each DEFLATE block kind asserted from the stream bytes), compares pixel-for-pixel against `wallpaper_rgb`, and prints an FNV-1a hash that `tools/png-check.sh` matches against the host's PIL decode of the identical bytes |
| `drivers/serial.c` | Minimal polling COM1 (0x3F8) output, debug-only, not wired to any shell command: a live boot trace readable with `-serial stdio`, immune to whatever state a crash leaves the VGA framebuffer in. `klog()` mirrors every entry here too. The one channel that made v54/v56/v58/v59/v62/v63/v64's headless verification passes possible, a boot-time direct-call trick can dump real values to it without ever opening a display |
| `drivers/mouse.c` + `drivers/vmmouse.c` | Pointer input. `mouse.c` is the PS/2 mouse on the 8042's second port (IRQ12, 3-byte relative packets). `vmmouse.c` (v62) probes the VMware absolute-pointer backdoor on I/O port 0x5658 at boot; where a host answers (QEMU's default pc machine, v86 in the browser) it switches the host to absolute mode and the GUI takes positions from it, PS/2 bytes still drained for phase but ignored. No backdoor (bare hardware, `-machine vmport=off`): PS/2 relative stays the only mouse |
| `kernel/kernel.c` | VGA text console, PS/2 scancode table, RTC clock, the shell, and a mouse-driven GUI desktop (`gui`) built on v6's graphics/font/mouse primitives. The GUI runs at 960x540 logical, scaled 2x to 1920x1080 physical; dock geometry and app layout live here too |

## Apps

Two different shapes of app live in this kernel, both dock-mounted, both
counted in `GUI_APP_COUNT` (20 real apps, plus the Apps-folder tile and
Trash, `kernel/kernel.c`).

**Five built-in apps, each its own header, each VFS-backed.** Same
persistence pattern every time: a fixed-size static array in RAM, one plain
text file on the real FAT disk (`vfs_replace_file`/`vfs_read_file`), manual
line parsing (no `sscanf`, no libc), write-through on every mutation, no
separate Save step.

| App | File | Format |
|---|---|---|
| Notes | `kernel/editor.h` | `NOTES.TXT`, a single 4095-byte buffer. The one app with real typography: `editor_fonts.h`'s embedded DejaVu Sans/Serif/Mono glyph table, F1/F2/F3 pickers for family/size(4)/weight, a real caret with vertical up/down that tracks column across lines |
| Reminders | `kernel/reminders.h` (v52 / 0.52.0) | `REMINDERS.TXT`, one line per item: `0`/`1` done flag, a space, the text. Up/down/select, `a` adds, `d` deletes, space toggles done |
| Calendar | `kernel/calendar.h` (v54 / 0.54.0 grid, v55 / 0.55.0 events) | No file for the grid itself, today's date comes straight from the RTC's CMOS BCD registers each time; day-of-week is Zeller's congruence, swept against libc's own `timegm` for every day 1900-2099 (`tools/check-calendar.sh`, 0 mismatches) before shipping. Events: `EVENTS.TXT`, one `YYYY-MM-DD\|text` line per day, one event per date (a second add on the same day overwrites, doesn't append) |
| Mail | `kernel/mail.h` (v59 / 0.58.0) | `MAIL.TXT`, `\|`-delimited `from\|subject\|body\|read`. Two starter messages compiled in so the inbox isn't blank before a real `MAIL.TXT` exists, which always wins once it does. No SMTP/IMAP client, deliberately: a local mail-shaped app, the same relationship Reminders has to a real to-do sync service |
| Contacts | `kernel/contacts.h` (v70 / 0.64.0) | `CONTACTS.TXT`, same VFS write-through shape as the other three: a fixed-size static array (name/phone/email), view/add/delete. No CardDAV/sync backend, deliberately, same offline-only call Mail already made |
| Chat | `kernel/chat.h` (v85 / 0.70.0) | `CHAT.TXT`, `role\|content` per line, an 8-message bounded ring (oldest drops once full). Real scrollback GUI (`n` sends, `c` clears, esc closes) plus the shell `chat <message>` command, both calling the same `chat_send` against Ollama's `/api/chat` (real conversation history, not v10's one-shot `/api/generate`). Model/host/port read from Settings-persisted `llm_model`/`llm_host`/`llm_port` (`kernel.c`'s `settings_load`/`settings_save`), not hardcoded; the Settings model row cycles only real installed models (`llama3.1:8b`, `qwen3:8b`), never free text |

**Calculator** (`kernel/calculator.h`, v70 / 0.64.0) doesn't fit the VFS-backed table above, it has no persistence at all on purpose: a recursive-descent parser over `+ - * / ()` and numbers, one-line input evaluated on enter. Ported from numen's real calculator parser (the v6 roadmap note that first flagged it as portable, pure logic, no network dependency).

**Stocks** (`kernel/stocks.h`, v71+ / 0.67.0) is a static demo-data app with no network backend: a fixed list of five real tickers (AAPL, MSFT, GOOGL, AMZN, TSLA) with plausible baked-in prices and daily changes, clearly labeled as demo data. No live market data or API calls (roadmap.md's real curl tests proved all plain-HTTP stock quote sources force HTTPS). List view with up/down selection, enter for details, esc closes—the same app-shape pattern Weather/Mail/Calendar established.

**Eleven apps ported natively from the fleet, thin ports on purpose.**
`gen_app.sh` turns a sibling repo's real single-file static build
(`~/Documents/Code/<app>/web/index.html`, this user's documented
sibling-checkout convention) into a plain C byte array,
`drivers/app_<name>.h` (`app_<name>_html`/`app_<name>_len`), no bundler, no
multi-file apps, real content only. Every array is reachable two ways:
`gui_launch_html` runs it through `html.c`'s `html_to_text` and renders the
real extracted copy read-only in the desktop; `serve_app` (the `serveapp
<name>` shell command) serves the exact original bytes over `net.c`'s
`tcp_serve_once`, verified for real over the actual network per v9/v35, not
assumed from a file check: a genuine `curl` through QEMU's `hostfwd` against
`serveapp keyrate`/`serveapp bookrank` came back the real `<title>`/meta
description, matching the actual repo content. v35 screened every candidate
by that same `html_to_text` transform before porting it, correctly skipping
apps whose real content only exists after client-side JS rendering this
kernel can't run (epiphany, healstack, blockframe, numen, curvely, roost,
monocode).

- **Curbfind**: Craigslist browser
- **Keyrate**: the one exception to "thin port": `gui_launch_html` originally just rendered the ported site's own marketing copy read-only, which meant the first keystroke anyone made to actually type closed the app (`gui_wait_close`'s "any key closes" contract). Fixed with a real native typing test in `kernel.c` itself: word list, a tiny LCG seeded from `irq.c`'s real `ticks()` (no `rand()`/no libc), a live input loop. `app_keyrate.h`'s ported HTML still exists, served only, not rendered in the GUI
- **Bookrank**: book summaries
- **Quotestreak**: quote-guessing game, no backend to begin with
- **Plan**: a planning app, ported for its real static copy at v35
- **Lexly**: gamified language learning
- **Toroid**: Conway's Game of Life on a toroidal grid (ships under the Toroid name; the source repo is `conway`)
- **Sparkjar**: idea forum
- **Homeqi**: feng shui home-assessment tool
- **Fieldbook**: every field of science and math explained plainly
- **Weather** (served copy only): the dock's own Weather app is native `kernel.c` logic against a live Open-Meteo fetch (temperature + WMO condition code, `weather_fetch`) for this machine's real location (v71 / 0.65.0: `geo_fetch` asks ip-api.com over plain HTTP once per boot, replacing a hardcoded downtown-Vancouver lat/lon; `tools/geo-check.sh` proves it headlessly against the host's own answer), not this file; v75 / 0.67.0: the same lat/lon drives the map wallpaper (`wall_fetch`, twelve OpenTopoMap tiles centered on the town, `tools/wallpaper-check.py`); `app_weather.h` is the ported static site, reachable only via `serveapp weather`

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
