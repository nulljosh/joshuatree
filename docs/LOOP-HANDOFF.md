# Joshua Tree loop handoff (2026-10-06, late night)

## What the loop is

The Pi loop. Joshua Tree runs on a real Raspberry Pi 4 with a USB keyboard. The loop merges green PRs, flashes each test build onto the SD card, and works down the roadmap's top 10 toward 3.0 (type and click on the real Pi).

## The goal

Joshua's /goal: we can build a mini Joshua Tree with Claude Code inside Joshua Tree on the Pi. The Claude app (merged, 2.14.0) works in QEMU through a relay on the Mac. It needs the Pi to have a network stack, so Wi-Fi comes first.

## Overnight rules (Joshua asleep, 2026-10-06 to 07)

Joshua is asleep and said: build as much as you can overnight, keep refreshing the loop, strengthen the harness. So:
- Never ask him a question; pick the sensible default and write it down here.
- Claude opens pull requests and never merges by hand; use `gh pr merge --auto --squash` only once a PR is ready and its local run was green. Never force-push main. Never use `--admin`.
- Max three helpers, two if any is Opus or Fable. Stop starting new work at 90 percent of either weekly limit, then only merge, flash-prep and write this file.
- One `tools/ci-local.sh` at a time (a lock lands with the harness PR). Disk must stay above 6 GB free.
- Keep at most four open PRs and zero issues. Delete a branch once its PR merges.
- The SD card needs Joshua: build the newest test image into a worktree and leave it ready; do not wait for the card.
- Never print or commit the Wi-Fi password, tokens or keys. Never run destructive commands on his files or the LaCie.
- Each round: merge what is green, rebase what conflicts, start the next item from the list below, refresh this file's "Where things stand", and send a one-line PushNotification if something needs him.

## Where things stand

- Main is 2.14.0. On the real Pi: 1080p desktop, readable console, the Steve Jobs tribute, USB keyboard with hot-plug. The Wi-Fi code is on the card and runs, but its lines scroll off the Console (the screen is the only debug channel; no serial cable yet).
- Open PRs: #435 Wi-Fi stage 1 (ready, in GitHub's checks), #436 tribute line, shopping list, decisions, roadmap (draft), #437 crash screen, FP state, OOM, boot health check (draft), #438 console scrollback with Page Up and Down and a pinned status line (draft). A helper is porting the IP stack to ARM (branch arm64-net-stack).
- Issues: none, on purpose. Branches: only those for open PRs.
- Test suites collide on fixed /tmp paths: run one `tools/ci-local.sh` at a time. A roadmap item fixes it.
- Standing answers are in `docs/DECISIONS.md`; the parts list is `docs/SHOPPING.md`; the top 10 is at the top of `docs/roadmap.md`.

## Next, in order

1. Flash the scrollback build once #438 is written, photograph the Console with Page Up, read the Wi-Fi lines.
2. Merge the open PRs as they go green, one at a time. Keep the PR list short.
3. Wi-Fi stage 2: join the network from the ignored config file, then the IP stack on top.
4. Admin and sudo, then the real desktop and dock on the Pi, then SD card writes.
5. Plain README and CLAUDE.md, the self-update loop, the fallback kernel, Doom.

## Restart prompt

```
/loop until it's done! Joshua Tree on the Pi: read docs/LOOP-HANDOFF.md, merge open PRs one at a time when green, flash the card with the newest build for Joshua to photograph, then work the Next list in order. Draft PRs, one ci-local run at a time, max three helpers (two if Opus), stop new work at 90 percent weekly usage. Keep the repo, branches and PR list clean. TLDR after each round.
```
