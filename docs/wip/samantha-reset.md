# Samantha reset after her face loads (release blocker 2.0.0), diagnosis in progress

Status: NOT fixed, NOT reproduced natively. 12 minute budget ran out. Nothing in the kernel changed.

## What was tried
`samantha-reset-repro.py` boots `samantha llmhost= llmport= facehost=` under QEMU (64 MB, rtl8139, same RAM as the v86 demo)
with `-d int,cpu_reset,guest_errors -D qemu.log -no-reboot`, serves the real landing/face frames from a local server,
waits for the `face:` line, types a question and presses Enter. Run it from the worktree root: `python3 docs/wip/samantha-reset-repro.py 64`.

Result: no reboot. But the face loader stops early under QEMU slirp (`face: idle=15..20 talk=0`, three runs, a different count each time,
the server never sees the next request), so the heap never reached the ~1.5 MB (72 frames) of the demo. The repro therefore did not
exercise the failing state. First thing to do next: make the loader reach `idle=24 talk=48` natively (the jt_http_get failure at frame 15 to 20 is itself
suspicious: a failed fetch ends the whole clip in face_load_step), then type. The QEMU log already shows only v=20/21/2c/80 vectors for the shorter run.

## Suspects, from reading the code (unconfirmed)
- kernel/brk.c frame_ptr calls paging_map_region for every frame above 8 MB. Each new 4 MB region takes one of MAX_EXTRA_TABLES (16) page
  tables and a PDE write-through in paging_sync_task_dirs. With a 1.5 MB heap on a 64 MB guest this crosses several 4 MB regions.
  If paging_map_region returns 0 (tables exhausted) kheap grow_heap returns 0 and an unchecked kmalloc user would write through NULL.
- brk_set swaps CR3 to the kernel directory and back with interrupts on. An IRQ taken between the two on the wrong directory uses
  the task kernel stack from a directory that may not map it (checked: only the PDE write-through covers new regions).
- Samantha stops fetching while typed text is pending (face_step: inlen > 0), so the first keys land right when the window poll and decode
  path change from loading to idle animation: the first jpeg_decode_scaled of a held frame on the first poll after the load is the other new state.

## Next steps
1. Make the native repro load all 72 frames (retry the HTTP get, or serve over a path that does not drop), then watch qemu.log for the first
   `check_exception` chain before `reset` and map EIP with `nm kernel.elf`.
2. Add the regression check: window task with a ~2 MB brk heap then a POST-sized syscall (extend tools/checks/ring3brk-check.py), register in
   tools/checks/ci-suite.sh and tools/gen/testing-doc.py.

## 2026-10-01 second pass (15 min): REPRODUCED locally, not yet fixed
`ln -s ~/Documents/Code/joshuatree/node_modules node_modules` in the worktree, then
`node tools/checks/facespeak-demo-check.mjs` reproduces the reset in headless Chromium v86 every time:
the serial shows a second `=== kmain boot start === v2.0.0` (line 101 of samantha-reset-v86-serial.log, saved beside this file).
Key fact: it resets BEFORE any key is typed. The last serial lines are seven `present` polls after `samfocus`,
the proxy had served 59 face frames (of 72), and the `face: idle=` line never printed. So the trigger is the
face loader itself at roughly frame 59, about 1.2 MB into the SYS_BRK heap, not the keyboard.
Next: instrument kernel/brk.c (serial on every paging_map_region call from frame_ptr, and on each new BRK PDE table)
plus a serial line in the double-fault handler, rebuild (make kernel.elf copies to landing/v86), rerun the check,
and read the last line before the reboot. Then write the ring-3 window regression check (brkpoke as a window task
growing to 2 MB in 20 KB steps, matching her malloc pattern, under QEMU -m 64).

## Third pass (15 min): traced to the instant after SYS_BRK returns, hypothesis: her 16 KB kernel stack overflows into her page directory
Tracing is in kernel/brk.c (frame_ptr, map_one, brk_set) and kernel/idt.c (one `isr: v/eip/err cr2=` line on every exception). Still WIP, strip before merge.
Run 3 serial (samantha-reset-v86-serial.log, line 1490): the last brk_set finishes cleanly (`brk: done cr3=004f8000 saved=01cdc000 top=ff150000`,
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
