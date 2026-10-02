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
