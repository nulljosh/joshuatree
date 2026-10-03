# Joshua Tree loop handoff (2026-10-03, afternoon)

## What the loop is

Ship PR #358 (Joshua's face demo), then resume toward 2.0.0. One shippable slice per agent, about 10 minutes, headless QEMU only, one VM at a time. PRs stay draft until `tools/ci-local.sh` passes, then mark ready and merge on GitHub CI green. While a big PR waits, hold other merges. Keep README, landing, docs/TESTING.md and ARCHITECTURE.md current in the same PR. Zero issues always. Stop new work at 90% usage.

## Where things stand

Checkpoint 2026-10-03. PR #358 (Joshua's face fills the screen, light glass chat bar, 24fps blink and breathing, lip sync follows audio, "Tap to hear Joshua" button, demo MP4 recorded) ready for ci-local and merge. After that, remain on PR #331/2.0.0 gate work or pare back to smaller fixes.

PR #358 complete: Joshua face, light glass chat bar, 24fps idle breathing and smooth blinks, lip sync from audio position, emulated sound card playback even when browser locks audio, tap-to-hear button unlocks without stopping intro. Demo MP4 recorded and sent.

## Next, in order

1. Run `tools/ci-local.sh` on PR #358 (keep under 15 minutes), mark ready when green, merge.
2. Deploy to heyitsmejosh.com, verify live Joshua face and audio.
3. Joshua decides: ten apps still listed Free in buy.html; decide which stay free or move to $0.99.
4. Loop: remain on 2.0.0 gate, or pare back to smaller fixes as usage allows.

## Outstanding from 2.0.0 gate

- Every app in APPS[] runs as ring 3 (19 of 26 done).
- All ring-3 apps crash-tested headless.
- Phone: touch, on-screen keyboard, every app readable at phone size.
- Input by focus only, not global key pull.
- Tour scenes for Activity and Apps folder.

## Restart prompt

```
/loop ship Joshua Tree PR #358 (Joshua's face, light glass chat bar, 24fps blink, lip sync, tap-to-hear button, MP4 demo recorded) then stay on 2.0.0. Run ci-local.sh (under 15 minutes), mark ready, merge when GitHub CI green. Deploy to heyitsmejosh.com and verify live. Joshua decides: ten apps still free in buy.html. One ~10 minute slice per agent, headless only, draft until ci-local green, merge on green, zero issues, docs current. Stop at 90% usage.
```
