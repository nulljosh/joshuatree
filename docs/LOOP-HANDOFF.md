# Joshua Tree loop handoff (2026-10-07, late)

## What the loop is

The Pi loop. Joshua Tree runs on a real Raspberry Pi 4 with a USB keyboard. The loop merges green PRs one at a time, rebuilds the SD card image, and works the roadmap toward a Pi that boots, joins Wi-Fi, and lets Joshua use it without a keyboard-only detour.

## The goal

Joshua's /goal: a mini Joshua Tree with Claude Code inside it, running on the Pi. The Claude relay works in QEMU through the Mac. On the Pi, Wi-Fi has to join a network and get an address before the relay can reach anything.

## Standing rules

- Never ask Joshua a question you can answer with a sensible default; write the default down here.
- Claude opens pull requests as drafts and never merges by hand. `gh pr merge --auto --squash` only once a PR is ready and its local run is green. Never force-push main. Never use `--admin`.
- One Haiku agent at a time, and only when asked. Verify every claim yourself. Kill by PID only.
- One `tools/ci-local.sh` at a time (it takes a lock). Disk above 6 GB free.
- Joshua swaps the SD card by hand. Batch board tests and ask for one swap per round. Never pull the card while `tools/flash-pi.sh` is running; a cut-off flash froze the Mac's disk until a restart.
- At most six open PRs, zero issues. Delete a branch once its PR merges.
- Never print or commit the Wi-Fi password, tokens or keys. Nothing destructive on his files or the LaCie.

## Where things stand

- All open PRs (446 to 452) were folded into one draft, PR 453, on branch `pi-stack` (2.28.0). The old PRs and branches are closed.
- `tools/ci-local.sh` is running on PR 453. Benchmark is DEFERRED until it finishes; nothing else may run beside it.
- On the real board the Wi-Fi scan finds 11 networks. Joining is not written yet, so bring-up stops at step 8 of 10. Data path and network time come after that.
- The menu bar clock shows --:-- until a time source exists.
- Apps are not on ARM yet, so the dock icons do nothing on the board. The boot demo plays one lap of the dock labels with no mouse.
- `architecture.svg` is one combined graph drawn by `tools/gen/map.py`. The landing says it boots on a Pi 4.
- Weekly usage is at 90 to 93 percent, so big new features wait for the Saturday 22:00 reset. The loop stops and checkpoints at 95 percent.
- Standing answers are in `docs/DECISIONS.md`; the parts list is `docs/SHOPPING.md`.

## Next, in order

1. Let `ci-local` finish on PR 453. If green, `gh pr ready 453`, merge on GitHub green. If red, fix the cause, do not mute it.
2. Joshua's asks, in refreshed order: Wi-Fi join and DHCP, clock via network time, per-app menu bars, speaker (3.5mm), browser, Bluetooth mouse, phone, Yeti mic.
3. Once the Mac is quiet, run the benchmark (`/jt-bench`) and open it as its own draft PR.

## Restart prompt

```
/loop Joshua Tree loop. State in ~/Documents/Code/joshuatree/docs/LOOP-HANDOFF.md on origin/pi-stack (draft PR 453, 2.28.0). First: let tools/ci-local.sh finish on PR 453, then ready and merge on GitHub green. Then Wi-Fi join and DHCP on the Pi 4 (scan finds 11 networks, bring-up stops at step 8 of 10), then clock via network time, per-app menu bars, speaker, browser, Bluetooth mouse, phone, Yeti mic. One Haiku agent at a time, verify every claim yourself, kill by PID only. Joshua swaps the SD card by hand, so batch board tests and ask for one swap per round. Never pull the card mid-flash. Never print Wi-Fi secrets. Stop and checkpoint at 95 percent usage.
```
