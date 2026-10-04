# Joshua Tree loop handoff (2026-10-03, night)

## What the loop is

Keep merging green PRs, then build the next item: the landing demo, the ARM64 port for the Raspberry Pi, and drivers that can be tested in QEMU. No stray branches or PRs left behind. The full list of what is left, with a model tag on each item, is the Pickup section of `docs/roadmap.md`.

## Where things stand (2026-10-04)

- Main is 2.6.21 once PR 393 merges (ARM screen: ramfb on QEMU virt). Stacked on it, not pushed yet: 2.6.22 on branch `arm64-m2-kbd` (worktree `../jt-arm-m2`): the Pi build draws through the VideoCore mailbox (proven on QEMU's raspi4b), a virtio input driver (keyboard and mouse), virtio-net (a real ARP answer) and virtio-blk (a sector read back), plus a `voicetime:` serial line in Samantha. Push it as one PR the moment 393 merges.
- Joshua buys the Pi 4B this weekend. First boot is `docs/RASPBERRY-PI.md`: serial text, and now a picture on the monitor. 3.0.0 ships only when the desktop boots on a real Pi.
- Voice chat (checked 2026-10-04): both sides are typed text in, her voice out, no speech-to-text, three server round trips per message, nothing streamed. Plan in the roadmap's Voice chat section.
- Notebook items are filed in the roadmap ("From the notebook"). Neo is the new name for the Strata enclosure.
- The pre-push hook now runs per worktree (`core.hooksPath` is the relative `tools/hooks`).
- Disk: keep 6 GB free before `tools/ci-local.sh`. The Kaggle model weights moved to `/Volumes/LaCie/models/kaggle-hands-2026-10-03`.

## Next, in order

1. Merge 393, then push `arm64-m2-kbd` as one PR (draft, `tools/ci-local.sh`, ready, merge).
2. ARM: input on the GIC interrupt instead of polling, then M1d (the real font and window drawing on ARM), then IP, DHCP and TCP over virtio-net, then a FAT reader on virtio-blk.
3. Voice: read `voicetime` off ten live messages, drop the `/api/pick` round trip, then streaming, then speech-to-text.
4. Neo rename across hardware docs and CAD. Icons in one design system.
5. If the Pi is in hand: first serial boot, photograph the console, fix whatever the real chip does differently.

## How to work

One small PR at a time, draft until `tools/ci-local.sh` passes (it takes about 27 minutes), bump VERSION for code changes, merge the moment CI is green. Fold PRs together before CI starts, because each rebase restarts it. Docs stay at 100 percent: every counted source file needs a row in `docs/ARCHITECTURE.md`.

## Restart prompt

```
/loop until we can't build out anymore: keep merging green PRs, then build the next item (landing demo, ARM64 M1, drivers testable in QEMU), no stray branches or PRs left behind. State is in docs/LOOP-HANDOFF.md and the Pickup section of docs/roadmap.md.
```
