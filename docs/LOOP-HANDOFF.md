# Joshua Tree loop handoff (2026-10-03, night)

## What the loop is

Keep merging green PRs, then build the next item: the landing demo, the ARM64 port for the Raspberry Pi, and drivers that can be tested in QEMU. No stray branches or PRs left behind. The full list of what is left, with a model tag on each item, is the Pickup section of `docs/roadmap.md`.

## Where things stand

- Main is 2.6.13 and the landing is live at that version. One PR may be open from another session: #387 (the portfolio demo starts at once and tours the launchpad). It is not part of this loop. Merge it when green or leave it. If the main checkout sits on `joshua-intro-sync` with an uncommitted `VERSION`, that is its work, not ours.
- Shipped on 2026-10-03: every app as its own protected program, Joshua's web portfolio (his face, cloned voice, intro video, speaker button, phone tour), Music and Movies, the Raspberry Pi build, guide and case, and ARM64 through M1b (exceptions, timer, MMU, caches, heap) under QEMU. CI is balanced by measured time: a run takes about 10 minutes.
- Open gaps in the demo: the phone shows two input bars (the OS bar and the page composer), and the tour is silent after the intro. Both are the first Pickup items.
- Joshua buys the Pi on 2026-10-04: Pi 4B 4 GB, 3.3 V USB serial cable, a USB-C microSD reader (the Mac mini has no SD slot) and a USB-A to USB-C adapter. Steps are in `docs/RASPBERRY-PI.md`. 3.0.0 ships only when the desktop boots on a real Pi.
- Housekeeping that is not code: about 7.7 GB of model files sit in `/private/tmp/claude-loop` on the Mac mini. Joshua decides whether they stay.

## Next, in order

1. If the Pi is in hand: first serial boot per `docs/RASPBERRY-PI.md`, photograph the console, fix whatever the real chip does differently.
2. Landing and demo: one input bar on the phone, then the spoken tour, then a real-Chrome pass of the whole tour on desktop and phone.
3. CI: find out why slow runners flake (eight QEMUs share one runner), re-run `python3 tools/gen/ci-balance.py <run-id>` after adding checks, push once per PR.
4. ARM64 M1c (framebuffer and desktop in QEMU), then M2 (virtio drivers testable in QEMU).
5. Icons in one design system, then the app tiles retaken with a pinned clock.

## How to work

One small PR at a time, draft until `tools/ci-local.sh` passes (it takes about 27 minutes), bump VERSION for code changes, merge the moment CI is green. Fold PRs together before CI starts, because each rebase restarts it. Docs stay at 100 percent: every counted source file needs a row in `docs/ARCHITECTURE.md`.

## Restart prompt

```
/loop until we can't build out anymore: keep merging green PRs, then build the next item (landing demo, ARM64 M1, drivers testable in QEMU), no stray branches or PRs left behind. State is in docs/LOOP-HANDOFF.md and the Pickup section of docs/roadmap.md.
```
