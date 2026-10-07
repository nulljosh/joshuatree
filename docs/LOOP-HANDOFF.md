# Joshua Tree loop handoff (2026-10-07, afternoon)

## What the loop is

The Pi loop. Joshua Tree runs on a real Raspberry Pi 4 with a USB keyboard. The loop merges green PRs one at a time, rebuilds the SD card image, and works the roadmap toward a Pi that boots, gets online, and lets Joshua use it without a keyboard-only detour.

## The goal

Joshua's /goal: a mini Joshua Tree with Claude Code inside it, running on the Pi. The Claude relay works in QEMU through the Mac. On the Pi, Wi-Fi has joined a network; it still needs an address (DHCP) before the relay can reach anything.

## Standing rules

- Never ask Joshua a question you can answer with a sensible default; write the default down here.
- Claude opens pull requests as drafts and never merges by hand. `gh pr merge --auto --squash` only once a PR is ready and its local run is green. Never force-push main. Never use `--admin`.
- One Haiku agent at a time, and only when asked. Verify every claim yourself. Kill by PID only.
- One `tools/ci-local.sh` at a time (it takes a lock). Disk above 6 GB free.
- Joshua swaps the SD card by hand. Batch board tests and ask for one swap per round. Never pull the card while `tools/flash-pi.sh` is running; a cut-off flash froze the Mac's disk until a restart.
- At most six open PRs, zero issues. Delete a branch once its PR merges.
- Ship visible fixes as their own tiny PR; the big stack stays one draft.
- Never print or commit the Wi-Fi password, tokens or keys. Wi-Fi name and key go only into dev cards (`tools/flash-pi.sh` with `JT_WIFI_DEV=1`); release and CI builds carry no network. Nothing destructive on his files or the LaCie.

## Where things stand

- All Pi work is on branch `pi-stack` (2.28.0) in one draft PR, 453. The old PRs (446 to 452) were folded into it and closed.
- Wi-Fi joins on the real board. The chip has no supplicant, so the WPA2 4-way handshake is ours (`arch/arm64/wpa.h`: SHA-1, HMAC, key expansion, AES key unwrap), checked against the RFC and a recorded capture. Keys are installed in the chip. The stall was message 2 not repeating the association RSN element byte for byte (router wanted capabilities 0x000c; assoc info needs a 512-byte buffer).
- Bring-up stops at step 9 of 10, stuck on getting an address. DHCP is next.
- The console prints one line per result with a summary last. Wi-Fi status shows in the menu bar.
- The menu bar clock shows --:-- until network time exists.
- Apps are not on ARM yet, so the dock icons do nothing on the board. The boot demo plays one lap of the dock labels with no mouse.
- The latest commits are not covered by the last CI run on PR 453. `tools/ci-local.sh` is not running now; it has to run before the PR goes ready.
- Benchmark: DEFERRED. Nothing was running at checkpoint time, but the bench waits for the CI run on PR 453 and runs only when the Mac is quiet.
- `architecture.svg` is one combined graph drawn by `tools/gen/map.py`. `docs/VISION.md` holds the north star. `docs/hardware/BREADBOARD.md` has the wiring. The README logo merged as its own PR.
- Weekly usage is at 92 to 94 percent, so big new work waits for the Saturday 22:00 reset. The loop stops and checkpoints at 95 percent.
- Standing answers are in `docs/DECISIONS.md`; the parts list is `docs/SHOPPING.md`.

## Next, in order

1. Run `tools/ci-local.sh` on PR 453 against the latest commits. If green, `gh pr ready 453`, merge on GitHub green. If red, fix the cause, do not mute it.
2. Joshua's asks, in order: DHCP and an address on screen over the Wi-Fi data path, clock via network time (SNTP), per-app menu bars, speaker (3.5mm), browser with HTTPS, Bluetooth mouse, phone, Yeti mic.
3. Once the Mac is quiet, run the benchmark (`/jt-bench`) and open it as its own draft PR.
4. Small, separate PR: image and diagram-only changes skip the QEMU suite in CI. Not started.

## Restart prompt

```
/loop Joshua Tree loop. State in ~/Documents/Code/joshuatree/docs/LOOP-HANDOFF.md on origin/pi-stack (draft PR 453, 2.28.0). First: run tools/ci-local.sh on PR 453 against the latest commits, then ready and merge on GitHub green. Then DHCP on the Pi 4 (Wi-Fi joins, bring-up stops at step 9 of 10), then clock via network time, per-app menu bars, speaker, browser with HTTPS, Bluetooth mouse, phone, Yeti mic. Ship visible fixes as their own tiny PR. One Haiku agent at a time, verify every claim yourself, kill by PID only. Joshua swaps the SD card by hand, so batch board tests and ask for one swap per round. Never pull the card mid-flash. Never print Wi-Fi secrets. Stop and checkpoint at 95 percent usage.
```
