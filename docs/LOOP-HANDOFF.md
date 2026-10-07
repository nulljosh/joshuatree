# Joshua Tree loop handoff (2026-10-07, evening)

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

- The Pi 4 joins Wi-Fi on the real board. The chip has no login helper, so the WPA2 handshake is ours (arch/arm64/wpa.h). The stall was the router wanting its exact security element repeated in our reply.
- The Pi gets an address from the router and fetches the time over the internet. The menu bar shows a Wi-Fi fan icon and a 12-hour clock top right, verified on the real screen at 3:14 PM.
- On a good boot the console shows only the title and typefaces. Lines appear only when something breaks. A small apple sits beside the Steve Jobs tribute line.
- Everything is in one draft PR, 453 (pi-stack). Ready flips only after a full local CI run.
- Naming: the file manager rename from Burrow to Drawer is decided, not done; it goes in its own PR after 453. An OS rename is being considered; no pick yet.

## Next, in order

1. Full local CI on PR 453 (one tools/ci-local.sh at a time), then flip it ready and merge.
2. Burrow to Drawer rename, in its own PR after 453.
3. Split kernel/kernel.c, which is far too big to work in.
4. HTTPS on the Pi using the BearSSL already in third_party.

## Restart prompt

```
/loop Joshua Tree loop. State in ~/Documents/Code/joshuatree/docs/LOOP-HANDOFF.md on origin/pi-stack (draft PR 453, 2.28.0). First: run tools/ci-local.sh on PR 453 against the latest commits, then ready and merge on GitHub green. Then board-test the DHCP and clock code from c14ac464 (Wi-Fi joins; bring-up was at step 9 of 10), then per-app menu bars, speaker, browser with HTTPS, Bluetooth mouse, phone, Yeti mic. Ship visible fixes as their own tiny PR. One Haiku agent at a time, verify every claim yourself, kill by PID only. Joshua swaps the SD card by hand, so batch board tests and ask for one swap per round. Never pull the card mid-flash. Never print Wi-Fi secrets. Stop and checkpoint at 95 percent usage.
```
