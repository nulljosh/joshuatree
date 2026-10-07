# Joshua Tree loop handoff (2026-10-06, late night)

## What the loop is

The Pi loop. Joshua Tree runs on a real Raspberry Pi 4 with a USB keyboard. The loop merges green PRs, flashes each test build onto the SD card, and works down the roadmap's top 10 toward 3.0 (type and click on the real Pi).

## The goal

Joshua's /goal: we can build a mini Joshua Tree with Claude Code inside Joshua Tree on the Pi. The Claude app (merged, 2.14.0) works in QEMU through a relay on the Mac. It needs the Pi to have a network stack, so Wi-Fi comes first.

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
