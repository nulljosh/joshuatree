# Joshua Tree loop handoff (2026-10-07, midday)

## What the loop is

The Pi loop. Joshua Tree runs on a real Raspberry Pi 4 with a USB keyboard. The loop merges green PRs one at a time, keeps the newest SD card image built in worktree `jt-card3`, and works the roadmap's Top 10 toward 3.0 (type and click on the real Pi).

## The goal

Joshua's /goal: we can build a mini Joshua Tree with Claude Code inside Joshua Tree on the Pi. The Claude app works in QEMU through a relay on the Mac. The ARM build has the shared IP stack and the Console's ask> row talks to the relay. Wi-Fi has to join a network and fill in its network calls, then Claude can run on the Pi.

## Standing rules

- Never ask Joshua a question you can answer with a sensible default; write the default down here.
- Claude opens pull requests and never merges by hand; `gh pr merge --auto --squash` only once a PR is ready and its local run was green. Never force-push main. Never use `--admin`.
- Max three helpers, two if any is Opus or Fable. One Haiku agent at a time. Verify every claim yourself. Kill by PID only.
- One `tools/ci-local.sh` at a time (it takes a lock). Run it with `CI_LOCAL_JOBS=4`. Disk above 6 GB free.
- Joshua swaps the SD card by hand. Batch board tests and ask for one swap per round. Never ask for a swap while `tools/flash-pi.sh` is still running; check with `ps -Ao stat,args | grep flash-pi` first.
- Never pull the card mid-flash. A cut-off flash left `cp` stuck in kernel I/O, and that hung disk arbitration so every disk command froze. Only a Mac restart clears it.
- At most six open PRs, zero issues. Delete a branch once its PR merges.
- Never print or commit the Wi-Fi password, tokens or keys. Nothing destructive on his files or the LaCie.

## Where things stand

- Joshua has the Raspberry Pi 4. The first real boot draws the full desktop. Yesterday and last night went to Pi Wi-Fi, swapping the SD card by hand over and over. Wi-Fi is still unconfirmed on the real board.
- Main is 2.23.0 (desktop slice 3 merged as #445).
- Open draft PRs: #446 README Pi section, #447 2.22.1 Wi-Fi CMD5 reply flags (the real-board fix, branch `wifi-cmd5`), #448 2.24.0 ARM desktop slice 4 (mouse), #449 2.25.0 crayon scribble logo, #450 2.26.0 Pi dock icon parity, #451 2.27.0 font library slice 1. Zero issues.
- Wi-Fi status: local branch `wifi-finish` has seven commits that #447 does not contain, including the power fix "actually power the chip (set, not get), and flush the mailbox buffer". Its top commit is also pushed as `origin/wifi-stage1`, so that work is not local only, but it is not in #447. Finishing Wi-Fi means bringing these onto the PR branch.
- Disk is tight: about 4.5 GB free, minimum 6 GB. Eight stale agent worktrees under `.claude/worktrees` were removed today with their branches kept. Five locked ones remain: console-scrollback, wifi-finish, arm64-errors-tests, arm64-claude-console, arm64-desktop-slice4.
- The Mac disk layer hung this morning, so the Mac needs a restart before any board work. The Step 6c bench is deferred until then.
- Usage: weekly at 90 percent, resets Saturday 22:00. The loop stops and checkpoints at 95 percent.
- Standing answers are in `docs/DECISIONS.md`; the parts list is `docs/SHOPPING.md`; the Top 10 is at the top of `docs/roadmap.md`.

## Next, in order

1. Restart the Mac.
2. Reflash the card and let `tools/flash-pi.sh` finish. Then one card swap per round to test Wi-Fi on the board. Page Up shows the Wi-Fi lines; the pinned row shows the latest `wifi` line. If it says `WL_ON reads 1` and then a `sdio card rca` line, the chip is alive.
3. Finish Wi-Fi: move the `wifi-finish` commits onto #447, then join the network from the ignored config file, fill the four network calls, and make TCP survive loss.
4. Land #446 through #451 one at a time: rebase each above main's VERSION, check the test variants in `arch/arm64/Makefile` still link the shared network and ask objects, `CI_LOCAL_JOBS=4 tools/ci-local.sh` green, then ready, then merge on GitHub green, then `gh pr update-branch` on the rest.
5. The rest of the old Next list: desktop slice 4 is #448, then admin and sudo, SD card writes with MBR and FAT32, the self-update loop and the fallback kernel, and Doom.

## Restart prompt

```
/loop Joshua Tree loop. State in ~/Documents/Code/joshuatree/docs/LOOP-HANDOFF.md on origin/main. Top item: finish real-board Pi Wi-Fi (draft PR #447 CMD5 reply flags, local branch wifi-finish), then land the open draft PRs #446-#451 one at a time (CI_LOCAL_JOBS=4 tools/ci-local.sh green before ready, merge on GitHub green, gh pr update-branch after merges). One Haiku agent at a time, verify every claim myself, kill by PID only. Joshua swaps the SD card by hand, so batch board tests, ask for one swap per round, and never ask for a swap while tools/flash-pi.sh is still running. USAGE: check ~/.claude/scripts/usage.sh each round, stop and checkpoint at 95% weekly. Replies to Joshua: one line, his voice.
```
