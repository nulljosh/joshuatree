# Joshua Tree loop handoff (2026-10-07, night)

## What the loop is

The Pi loop. Joshua Tree runs on a real Raspberry Pi 4 with a USB keyboard. The loop merges green PRs one at a time, rebuilds the SD card image, and works the roadmap toward a Pi that boots, gets online, and lets Joshua use it without a keyboard-only detour.

## The goal

Joshua's /goal: a mini Joshua Tree with Claude Code inside it, running on the Pi. Samantha now answers on the real Pi through the Messages API, on Joshua's own API credit. A relay on the Mac picks a cheap or strong model per question, so nobody picks models by hand. She can read files from a shared folder on the Mac, read-only, and the Pi sends its live status (IP, clock, Wi-Fi) with each question. That is the first real Pi tool.

## Standing rules

- Never ask Joshua a question you can answer with a sensible default; write the default down here.
- Claude opens pull requests as drafts and never merges by hand. `gh pr merge --auto --squash` only once a PR is ready and its local run is green. Never force-push main. Never use `--admin`.
- One Haiku agent at a time, and only when asked. Verify every claim yourself. Kill by PID only.
- One `tools/ci-local.sh` at a time (it takes a lock). Disk above 6 GB free.
- Joshua swaps the SD card by hand. Batch board tests and ask for one swap per round. Never pull the card while `tools/flash-pi.sh` is running; a cut-off flash froze the Mac's disk until a restart.
- At most six open PRs, zero issues. The loop does not mirror the roadmap into GitHub issues. Delete a branch once its PR merges.
- Ship visible fixes as their own tiny PR; the big stack stays one draft.
- Never print or commit the Wi-Fi password, tokens or keys. Wi-Fi name and key go only into dev cards (`tools/flash-pi.sh` with `JT_WIFI_DEV=1`); release and CI builds carry no network. Nothing destructive on his files or the LaCie.
- The relay's API key lives outside the repo. Never copy it into a file, a log or a PR.

## Where things stand

- The Pi 4 joins Wi-Fi on the real board. The chip has no login helper, so the WPA2 handshake is ours (arch/arm64/wpa.h). The stall was the router wanting its exact security element repeated in our reply. PR 453 (the Pi stack) is merged.
- A later stretch of Wi-Fi trouble (joins refused, then no address) cleared after a router reboot. Nothing in the code is known to be at fault.
- Clock: a lost DNS lookup used to wait 20 seconds per try. Each try now waits 2 seconds and retries six times.
- The console stays quiet on a good boot. Lines appear only when something breaks. The boot screen draws the tree while the photo decodes, and a small apple sits beside the Steve Jobs tribute line.
- The green-light blink broke the keyboard. PR 459 (the keyboard message, light off) turns it off for now. The keyboard works again on the board. PR 459 is open and one CI shard is rerunning.
- PR 460 (Samantha through the Messages API, plus the first Pi tools) is a draft.
- PR 458 (the HTTPS plan in the roadmap) is open.
- The graph refresh is merged; main is 2.29.1.
- Benchmark: DEFERRED. Run `/jt-bench` only when nothing else is running on the Mac.
- Naming: the file manager rename from Burrow to Drawer is decided, not done; it goes in its own PR. An OS rename is being considered; no pick yet.

## Next, in order

1. Finish PR 459 once its rerun is green, then merge it on GitHub green.
2. Finish PR 460: run `tools/ci-local.sh` once, flip it ready, merge on green.
3. After the Saturday reset (weekly usage was 93 percent at this checkpoint): Pi actions (the light, SD card files, opening an app), hide the Console on boot, a separate Claude app, and the model and effort chosen from the prompt.
4. Burrow to Drawer rename, in its own PR.
5. Split kernel/kernel.c, which is far too big to work in.
6. HTTPS on the Pi using the BearSSL already in third_party, following the plan in PR 458.
7. Benchmark when the Mac is quiet.

## Restart prompt

```
/loop Joshua Tree loop. State in ~/Documents/Code/joshuatree/docs/LOOP-HANDOFF.md on origin/main. First: finish PR 459 (keyboard message, light off) once its rerun is green, then PR 460 (Samantha through the Messages API and Pi tools, draft): one tools/ci-local.sh run, ready, merge on GitHub green. After the Saturday reset: Pi actions (light, SD card files, open an app), hide the Console on boot, a separate Claude app, model and effort in the prompt. Then the Burrow to Drawer rename in its own PR, split kernel/kernel.c, and HTTPS on the Pi from the plan in PR 458. Benchmark stays deferred until nothing else runs. Ship visible fixes as their own tiny PR. One Haiku agent at a time, verify every claim yourself, kill by PID only. Joshua swaps the SD card by hand, so batch board tests and ask for one swap per round. Never pull the card mid-flash. Never print Wi-Fi secrets or the relay key. Stop and checkpoint at 95 percent usage.
```
