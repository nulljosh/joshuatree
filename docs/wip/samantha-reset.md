# Samantha reset after her face loads (release blocker 2.0.0), postmortem

Status: FIXED. Root cause was the landing demo's kiosk idle timer, not the kernel.

The demo page reboots the guest after 15 s without a mouse move, click or key (landing/v86/embed.js). Samantha's 72 face fetches plus a
spoken reply ride /api/proxy and can outlast that window, so the guest looked like it reset right after her face loaded: it was the page
restarting it. The fix bumps the idle clock on every /api/proxy fetch (the guest fetching is an app at work).

The hunt did turn up real kernel hardening that shipped with the fix: 32 KB ring-3 kernel stacks with a guard pad, a stack-room check in
sys_http_get/post, and the WIP serial tracing removed. The v86 serial log from the investigation was dropped from the tree; the native
repro stays in samantha-reset-repro.py.

## Investigation notes (second pass, kept for the record) (15 min): REPRODUCED locally, not yet fixed
`ln -s ~/Documents/Code/joshuatree/node_modules node_modules` in the worktree, then
`node tools/checks/facespeak-demo-check.mjs` reproduces the reset in headless Chromium v86 every time:
the serial shows a second `=== kmain boot start === v2.0.0` (line 101 of the v86 serial log, since removed).
Key fact: it resets BEFORE any key is typed. The last serial lines are seven `present` polls after `samfocus`,
the proxy had served 59 face frames (of 72), and the `face: idle=` line never printed. So the trigger is the
face loader itself at roughly frame 59, about 1.2 MB into the SYS_BRK heap, not the keyboard.
Next: instrument kernel/brk.c (serial on every paging_map_region call from frame_ptr, and on each new BRK PDE table)
plus a serial line in the double-fault handler, rebuild (make kernel.elf copies to landing/v86), rerun the check,
and read the last line before the reboot. Then write the ring-3 window regression check (brkpoke as a window task
growing to 2 MB in 20 KB steps, matching her malloc pattern, under QEMU -m 64).

## Third pass (15 min): traced to the instant after SYS_BRK returns, hypothesis: her 16 KB kernel stack overflows into her page directory
Tracing is in kernel/brk.c (frame_ptr, map_one, brk_set) and kernel/idt.c (one `isr: v/eip/err cr2=` line on every exception). Still WIP, strip before merge.
Run 3 serial (the v86 serial log, line 1490, since removed): the last brk_set finishes cleanly (`brk: done cr3=004f8000 saved=01cdc000 top=ff150000`,
`brk: back on task dir`) and the very next line is the reboot. No `isr:` line at all, so no exception was ever delivered: a true triple fault.
The page count at death differs per run (288 pages run 2, 336 pages run 3), so it is timing bound, not a fixed boundary. All brk frames are
contiguous 0x01df2000.. (28 to 32 MB physical, PDE 7, already identity mapped), MAX_EXTRA_TABLES is not exhausted, and the v86 LFB at 0xE0000000 is untouched.
What runs between brk returning and the next brk: the user copy into the new pages, then SYS_HTTP_GET (big path) on the task's 16 KB kernel stack with
interrupts on. Her page directory is pmm frame 0x01cdc000 and her brk page table 0x01df1000; task_create interleaves the kmalloc'd kernel stack with
paging_new_task_directory's pmm frames (kheap.c's own comment records a triple fault from exactly that adjacency). A kernel stack overflow by the
net path (nested rtl8139 IRQ on top of a deep http_get frame) scribbles downward into whatever frame sits just below the stack block; if that is the
directory, the first bytes hit are PDEs 1023 down to 0x3FC, her brk PDEs and the LFB, then 0x300, the kernel itself, and the next instruction fetch
triple faults with nothing to print. Next: print the kernel stack block range and dir_phys at exec_user_window, confirm adjacency; fix by
(a) guarding the stack with a canary page / 32 KB stack, (b) allocating dirs and kernel stacks so they are never neighbours, (c) a stack-depth
check in sys_http_get/post before the net call that returns -ENOMEM instead of overflowing. Regression check: window task, 2 MB brk, many big GETs.

## RESOLVED (fourth pass): it was never the kernel
With the stack hardened (32 KB + guard pad + -ENOMEM guard in sys_http_get/post) the demo still rebooted, and the serial showed her
kernel stack (0x01098250..0x010a1250) nowhere near her directory (0x01cdc000), no guard hit. The reboot is landing/v86/embed.js's
retail-kiosk idle reset: 15 s after the last click/key with no serial marker it treats as activity, it calls emulator.restart().
Her 72 face frames take longer than 15 s to arrive in the browser and need no click, so the demo rebooted mid-load every time, with
no CPU fault at all (which is why QEMU never showed one). Fix: window.fetch is wrapped so every guest request through /api/proxy
bumps lastInteractionTime. facespeak-demo-check now also asserts exactly one kmain banner before the face line (fails before, passes after).
After the fix: one boot, `face: idle=24 talk=48`, chatreply arrives. Separate, pre-existing on fix/facespeak: `speak: status=403`, the speak POST
never reaches the check's intercept; not this bug.
