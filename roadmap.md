# Joshua Tree roadmap

Freestanding i386 kernel, no libc. v0 boots in QEMU with VGA text, PS/2
keyboard, RTC clock, and a shell that only knows `help clear echo time
reboot`. Everything below is the standard bare-metal-to-usable-OS path
(same order every OSDev-wiki "Bare Bones" -> "Meaty Skeleton" walkthrough
takes, and roughly what xv6/ToaruOS/Linux 0.01 did in their first months).

## Shipped (full detail in git log / commit messages, not duplicated here)

- Done
- v2, memory (from "one flat blob" to real address space)
- v3, multitasking (more than one thing running)
- v5, the "file explorer", done (Sep 2026)
- v7, networking (the "browser" part needs a network stack), done (Sep 2026)
- v8, the actual browser (the point of all of this), done (Sep 2026)
- v9, running real apps (the browser can now load something), done (Sep 2026)
- v10, talking to it like gato does (this is the actual point), done (Sep 2026)
- v11, a real desktop (this kernel gets a face), done (Sep 2026)
- v12, voice mode: an LLM actually controlling this kernel, done (Sep 2026)
- v13, real file writing (this kernel can create a file, not just a directory), done (Sep 2026)
- v14, the GUI dock grows to fit the rest of the codebase's apps, done (Sep 2026)
- v15, a real Dock, draggable icons, and a real rendering bug fixed, done (Sep 2026)
- v16, the kernel sets up its own VGA text mode instead of inheriting it, done (Sep 2026)
- v17, the kernel enables its own keyboard scanning, done (Sep 2026)
- v18, the kernel gets a real DAC palette and a real live demo replaces the recorded video, done (Sep 2026)
- v19, real Dock icons instead of letters, done (Sep 2026)
- v19, killing the visible CLI flash on the live demo (Sep 2026)
- v20, a real dmesg log, richer icons, and a desktop wallpaper (Sep 2026)
- v21, a real close button for touch-only visitors, a Mac-not-Ubuntu palette, a real menu bar mark, and smoother icon edges (Sep 2026)
- v22, the kernel boots straight to its own desktop, and the click-coordinate bug behind three separate reports (Sep 2026)
- v23, the real cursor-drift bug behind "the mouse is inoperable" (Sep 2026)
- v26, a real keyboard-only app test, and a real bug it immediately caught (Sep 2026)
- v27, real gradient icons, a real menu bar clock timezone bug (Sep 2026)
- v29, a real VFS (done Sep 2026)
- v30, basic hacking tools (Kali/Mr. Robot flavored, done Sep 2026)
- v31 / 0.31.0, real per-task memory isolation (done Sep 2026)
- v32 / 0.32.0, real signals: SIGKILL-equivalent (done Sep 2026)
- v33 / 0.33.0, a real block device abstraction (done Sep 2026)
- v34 / 0.34.0, real heap growth past the old 4MB wall (done Sep 2026)
- v35 / 0.35.0, six more real apps ported, and a real dock overflow bug (done Sep 2026)
- 0.35.1, VFS migration call sites v29 missed, and a real 64KB leak (Sep 2026)
- v36 / 0.36.0, a real terminal inside the desktop (Sep 2026)
- v37 / 0.37.0, the dock becomes a choice, and two real rendering bugs behind it (Sep 2026)
- 0.37.1, Terminal and Apps folder were unusable on a phone (Sep 2026)
- v38 / 0.38.0, the browser demo had no font at all (Sep 2026)
- v39 / 0.39.0, a real Trash, and the dock in a deliberate order (Sep 2026)
- v40 / 0.40.0, the dock stopped flashing on hover (Sep 2026)
- v41 / 0.41.0, 1600x1200: icons that are actually sharp (Sep 2026)
- v42 / 0.42.0, 16:9 at native 1920x1080, a real wallpaper, bilinear, notifications, an arrow pointer (Sep 2026)
- 0.42.1, the dock under a macro lens (Sep 2026)
- v44 / 0.44.0, the typeface pass, and a demo that shows itself off (Sep 2026)
- 0.44.1, the crunch (Sep 2026)
- v45 / 0.45.0, wind (Sep 2026)
- 0.45.1, the flashing cursor (Sep 2026)
- v47 / 0.47.0, real settings, persisted (Sep 2026)

## v4, storage (data survives reboot), done (Sep 2026)

- [ ] [Haiku] VFS layer so the shell's `open`/`read` don't care which fs backs them, lower priority now: FAT is the only filesystem that exists, so there's nothing to abstract over yet

## v6, graphics (text mode won't carry a browser), mostly done (Sep 2026), font renderer deferred

A real fork discovered mid-implementation: requesting a video mode via the multiboot header (the obvious first approach) makes QEMU boot straight into graphics mode with no way back to VGA text, breaking the working shell until the font renderer exists too, since text and framebuffer output can't coexist that way. The better path is switching graphics on and off at runtime via QEMU's Bochs VBE register interface (ports 0x1CE/0x1CF), which needs the framebuffer's real physical address first, only PCI config space knows that, not a fixed constant.

  Real, honest complication found along the way and worth recording, not smoothed over: a `screendump`/`screencapture` of the screen right after closing a graphics window still showed a garbled vertical-stripe pattern even once the register restore was proven byte-for-byte correct. Chased that as a second possible bug (tried resetting Bochs's own leftover bank/virtual-width/virtual-height/offset registers too, on the theory a stale wide pitch was the cause) before the keystroke-and-VGA-memory-dump test above showed the kernel's actual guest-visible text buffer was already completely correct the whole time. Conclusion: the visual artifact is a capture-tool-side rendering glitch around the text-to-graphics-to-text transition, not a guest bug, and it did not reappear in this final build's own verification. Recorded here rather than claimed silently fixed, in case a real display someday reproduces it and this note saves the next person from re-diagnosing the register path that was already proven correct.
- [ ] [Haiku] Memory efficiency pass: this is a real standing constraint from here on, not a one-time task. Watch static allocations (paging.c's extra page tables, kheap's growth), avoid needless copies, keep the kernel's own footprint small before it starts hosting real app logic in v9. Revisit whenever a subsystem's memory use looks bigger than it needs to be, not just once

  First real instance of this: `kheap.c` never coalesced adjacent free blocks, a real documented gap, not a hypothetical one. Freeing two neighboring small buffers left two separate free blocks even though they sat right next to each other in memory, so a later request that would have fit their combined space, exactly the shape `chat`/`build`'s repeated allocate-a-buffer-then-free-it pattern produces, pulled in a whole new physical frame instead of reusing what was already free. Fixed by merging adjacent free blocks on every `kfree`, using a property already true of the allocator's own list order (each new block is carved at a higher address and pushed to the head, so consecutive list nodes are always memory-adjacent) rather than adding a separate address-sorted structure. Verified by proving the test discriminates, not just that it prints "ok": ran `heaptest`'s new coalescing check against the actual fix (`coalesced adjacent free blocks: ok`, and confirmed via `pmm_free_frames()` before/after that no new physical frame was pulled in), then temporarily reverted `kfree` to the old no-coalescing version and reran the identical test, which correctly failed (`coalesce failed`), before restoring the fix.
- [ ] [Haiku] Product design pass: also a standing concern, not one task. The warm palette/copy discipline that landed on the landing page belongs on the kernel side too once there's a UI worth looking at, consistent colors, deliberate layout, not just "does the pixel show up." Revisit once the font renderer and windowing surface have real content to arrange, not before there's anything to design. First small touch already in: a two-note PC-speaker boot chime (`beep`/`boot_chime` in `kernel.c`, PIT channel 2) instead of silence, a square wave has no timbre to make genuinely soft, but two consonant notes beats one flat tone. Honest caveat: verified structurally (correct PIT/speaker register sequence, boots without hanging, real timing budget from `sleep_ticks`), not audibly, there's no way to capture real sound out of this headless setup, a human needs to actually listen once

Once graphics exist, a lighter early win becomes possible without waiting for v7/v8's full network stack and browser: pure-logic apps from the codebase (no DOM, no network dependency, e.g. numen's calculator parser, keyrate's typing-test scoring, weather's forecast math minus the live fetch) can be ported natively in C and rendered straight to the framebuffer, no HTTP client or HTML parser needed. This is real groundwork for v9, not a replacement for it: the full vision (real web apps served and rendered by the real browser) still needs v7 and v8. Worth a small side-track once the framebuffer and font renderer above are solid, picking one simple app's core logic (not its whole UI) as the first native port.

## v24, real window chrome and cleaning up redundant branding (Sep 2026)

Direct follow-up requests after the mouse fix made the demo actually usable enough to notice these.
- [ ] [Joshua] Honest, not-yet-solved gap, raised multiple times in two different forms that turn out to be the same root cause: the landing page still letterboxes (black bars) on most real viewports, and the real local QEMU window can't be resized at all. Confirmed the second one directly, not assumed: setting the window's size via System Events while it was running was silently ignored, the window snapped right back to exactly 800x632, since this kernel's GUI is a fixed, hardcoded 800x600 with no mechanism for the guest to negotiate a different resolution with whatever's actually displaying it, browser canvas or native QEMU window alike. A real fix exists (`vbe_set_mode` already accepts any width/height; the missing piece is threading the actual display size into the kernel at boot, most likely via the multiboot command line, which isn't currently parsed for anything custom), but it's a real, multi-file change (embed.js computing and passing the size, kmain parsing it, `gui_run`'s hardcoded `window_open(800,600,32)` and every layout constant built around it going dynamic instead), not a CSS tweak or a QEMU flag, deliberately not rushed into the same pass as the mouse fix. Separately, real macOS native fullscreen (the green button, a Spaces transition) on the QEMU window is a known QEMU cocoa-backend limitation with custom Bochs VBE modes, not something this kernel can fix or something that needs a custom QEMU build; the practical workaround for now is not using it.

## v25, a real menu bar flicker fix, and a real, unresolved gap in test infrastructure (Sep 2026)

Direct bug report from actually running the OS locally for the first time this session, not the browser demo: "menu bar redraws and rerenders and flashes when we hover over any icon."
- [ ] [Fable] Re-diagnosed (Sep 2026), gap still real but the cause isn't what was written here before. The USB-tablet theory was wrong, disproved directly: `info mice` lists only the real PS/2 mouse, and a temporary serial log at `mouse_get_delta`'s real call site proved IRQ12 packets do arrive and do decode, one clean callback per `mouse_move`. What's actually broken: the decoded dx/dy don't correspond to what was sent, at all, not scaled, not offset, just different every run including movement reported on an axis given 0. That's QEMU's HMP `mouse_move` and/or this headless config's IRQ timing, not this kernel's PS2 driver or `guitest.sh`'s dock-geometry math, both of which are correct. See `guitest.sh`'s own header for the full trace. Not wired into any regression flow, not claimed to work.

## v28, real process exit and reap (task.c's half of the gap, done Sep 2026)

`task.c` shipped preemptive round-robin but never closed the gap it flagged honestly at the time (see the file's own old header comment): a task couldn't safely `return` from its entry function, so every demo task (`task_a`/`task_b`/`preempt_task_a`/`preempt_task_b`) had to park in `hlt` forever once done, permanently burning a slot out of `MAX_TASKS=6`. Running `tasktest` then `preempttest` used to leave 4 of 6 slots dead until reboot, exactly the constraint `reaptest`'s own predecessor tests warned about ("no free task slots, run fewer other task tests first").
- [ ] [Fable] Not done yet, deliberately scoped out of this pass: the real `int 0x80` syscall gate and giving `ring3_test` an actual exit path instead of halting the whole kernel on its ring-3 fault. `ring3_test` isn't a task in the scheduler at all today (it's a one-shot ring-0 function that manually enters ring 3), so this is a separate, larger change (a real syscall ISR, ring3 code that calls it instead of running off the end, and integrating ring-3 execution into the same task/task_exit machinery this pass just built) not a trivial follow-on.

## Explicitly parked / non-goals

- SMP (multi-core), one CPU is plenty until everything above works
- A real filesystem journal / crash-consistency, FAT read support is enough for v4-v5
- Wi-Fi, wired NIC only, QEMU doesn't emulate Wi-Fi hardware anyway
- A real GUI toolkit (retained widgets, layout engine, a scene graph). v11's desktop is real and mouse-driven, but it's one hand-drawn screen with hardcoded hit-test rectangles, not a toolkit anything else could be built on top of. Build one if a second, different screen ever needs the same primitives enough times to justify it.
- gato (the macOS voice kiosk, `~/Documents/Code/gato`), different project, different repo, on purpose. Voice control of this kernel is a real future idea but nowhere near the front of the queue.

## Later product ideas (real, not scheduled, revisit once v8's browser actually renders to the framebuffer)

- Replace the landing page's recorded boot GIF with the real thing once there's something worth interacting with: an actual in-browser demo of this kernel, not a video of one.
- User accounts on the landing page, each with their own space.
- A downloadable, installable image so someone can put this on real hardware, not just QEMU, and actually boot into it (explicit north star, Joshua's own words). Real hardware brings its own driver-compatibility questions (this rtl8139 driver, the PCI enumeration, the ATA driver) that QEMU's emulated devices don't raise, worth its own pass when it's actually time, not assumed to just work.
- Real support for the entire codebase's fleet of apps, not just the handful hand-picked so far (explicit north star, Joshua's own words). v9/v11's `gen_app.sh` pipeline only handles the single-static-file case; most of the fleet is React/Vite (needs a bundler this kernel doesn't have) or split across multiple files `serve_app`'s one-shot single-connection server can't serve together. Getting from "5 apps" to "the whole codebase" is a real, larger project on its own, not a rename of the existing pipeline.
- A real text editor app for the GUI (Joshua's own request), with a font/typeface picker. `font.c` currently extracts exactly one embedded bitmap font (the real IBM CP437 dumped from VGA hardware); a picker needs at least a second real font to switch to, not a dropdown with one option, so this waits on that groundwork rather than shipping a fake selector.
