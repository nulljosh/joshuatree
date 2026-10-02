# Joshua Tree loop handoff (2026-10-01, morning)

## What the loop is

Build Joshua Tree to 2.0.0, one small PR at a time. docs/VERSIONS.md is the map: 1.8, then 1.9, then 2.0. Each agent gets one shippable slice, about 10 minutes, headless QEMU only, one VM at a time. Two agents at once if either is Fable or Opus, otherwise three. PRs stay draft until `tools/ci-local.sh` is green, then merge on green without asking. While a big PR waits, hold every other merge (main requires up to date branches), and fold small docs changes into a PR that is already open. Keep README, landing, docs/TESTING.md and ARCHITECTURE.md current in the same PR as the change. Zero open issues, always. At 90% session usage, checkpoint and stop starting new work.

## The 2.0.0 gate

2.0.0 ships only when all of these are true and each has a headless check in ci-suite.sh:

1. 1.8 done: a phone home screen, an app grid, one app full screen at a time, a back button.
2. 1.9 done: touch works, an on-screen keyboard, every app readable at phone size.
3. Every app in the APPS[] table runs as its own ring 3 program. No app code left in kernel.c. Done (26 apps: Samantha was last to ship).
4. A check crashes each app on purpose and proves the desktop is still alive after every one. Done for every ring-3 app.
5. Input goes to the focused window only, not a global key pull. Done for ring-3 window apps. All 26 apps now open as real compositor windows with private memory and per-window input.

## Where things stand

Checkpoint 2026-10-01 morning. The loop shipped on branches (not yet merged; a big combined 2.0 PR coming):

- 26 of 26 apps now run as protected ring-3 programs sharing the screen as compositor windows.
- Apps are real windows: crash one, the desktop and all 25 others stay alive. Input goes to the focused window only.
- Kernel fixes landed: heap/VFS race condition, stale page directories in task copies, BRK fault handling.
- Per-task growable heaps (SYS_BRK) and malloc() now work.
- Kernel user window moved to make room.
- Antialiased DejaVu type in every app.
- System clipboard across all apps.
- Window resize working.
- Notes with folders and a phone keyboard.
- Terminal with an allowlisted shell syscall.
- Mail compose inline with real outgoing mail through the Worker (Resend, token + rate limit, secret not yet set).
- Weather and Stocks refetch in place.
- New 2.0 logo (Apple-simple, picked by Joshua) and 2.0 UI pass complete.
- Landing page synced to 26 of 26 and QA'd.
- Demo tour update in flight.

Still open: multiwindow check, deleting the old blocking launcher, merging everything into one 2.0 PR, full CI.

Also queued after 3.0: 3D-print plan for Mesa case, ad refresh, Blender model refresh per Joshua's ideas.

## Next, in order

1. Multiwindow check and old launcher cleanup.
2. Merge everything into release/2.0-int as one combined PR.
3. Full CI pass.
4. Tag 2.0.0 on main.
5. 2.1 Music, 2.2 Video.
6. 3.0 on the ASRock J4125B-ITX, the Strata Kit at 3.1.

## Restart prompt

```
/loop build Joshua Tree as far as possible until 3.0 hardware (2.0, then 2.x per docs/VERSIONS.md), zero issues and PRs; then stop and refresh the ad and Blender model with Joshua's ideas.
```
