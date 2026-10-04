# Joshua Tree loop handoff (2026-10-03, night)

## What the loop is

Keep merging green PRs, then build the next item: the landing demo, the ARM64 port for the Raspberry Pi, and drivers that can be tested in QEMU. No stray branches or PRs left behind. `docs/roadmap.md` has the ladder.

## Where things stand

- Main is at 2.6.13. No open PRs, no stray branches. The landing is live at that version.
- Shipped today: every app as its own protected program, Joshua's web portfolio (his face, voice, intro video, speaker button, phone tour), Music and Movies, the Raspberry Pi build, guide and case, and ARM64 up to M1b (exceptions, timer, MMU, caches, heap) under QEMU.
- CI is balanced by measured time: a run takes about 10 minutes. After adding or slowing checks, run `python3 tools/gen/ci-balance.py <run-id>` on a green run to deal them out again.
- Joshua buys the Pi tomorrow. The first real boot needs a 3.3 V USB serial cable, a microSD reader and a USB-A to USB-C adapter. Steps are in `docs/RASPBERRY-PI.md`. 3.0.0 ships only when the desktop boots on a real Pi.
- Known gaps: the phone shows two input bars (the page needs a real input for the keyboard); the tour is silent apart from the intro.

## Next, in order

1. If the Pi is in hand, follow `docs/RASPBERRY-PI.md` for the first serial boot and fix whatever the real chip does differently.
2. ARM64 M1c: a framebuffer and the desktop under QEMU.
3. M2: virtio drivers testable in QEMU (disk, network, input).
4. Tour narration in his cloned voice; keep QA on the live site, phone and desktop.

## Restart prompt

```
/loop until we can't build out anymore: keep merging green PRs, then build the next item (landing demo, ARM64 M1, drivers testable in QEMU), no stray branches or PRs left behind. State is in docs/LOOP-HANDOFF.md.
```
