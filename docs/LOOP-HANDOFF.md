# Joshua Tree loop handoff (2026-10-04, evening)

## What the loop is

Keep merging green PRs, then build the next item: the QA list from Joshua's photos, CI hardening, fleet apps on real backends, and the ARM64 port for the Raspberry Pi. No stray branches, PRs or issues left behind. The open work, with a model tag on each item, is the Pickup section of `docs/roadmap.md`. The QA list lives in the memory note `project_joshuatree_qa_backlog.md`.

## Where things stand (2026-10-04)

- Main is 2.6.35 (this PR) and live. Shipped today: the app sandbox on the Pi version (EL0, svc write and exit, kernel RAM not executable from EL0), Neo, Bookrank, Tonchi (the OS name for Lexly, the App Store rename is not applied yet), Hikko, fleet names in the Portfolio, Stocks, Burrow, Mail and Weather file reads, and the menu bar fix. Main CI stayed green all day. The reds were on PR branches.
- No PRs open once this one merges. 2.6.35 is the Launchpad: centered, icons 24 percent smaller, arrow keys now move the folder selection, apps opened from it size like dock apps.
- Policy: helper PRs open as draft and pass `CI_LOCAL_JOBS=4 tools/ci-local.sh` before they go ready. Only one helper runs the full suite at a time. Never iterate on GitHub CI.
- CI is slow because shard 5 holds 23 checks and shard 6 holds 42. Rebalance with `python3 tools/gen/ci-balance.py <green run id>` first. Never pick a shard by hand for a new check.
- The Pi: Joshua buys and picks it up today. First boot per `docs/RASPBERRY-PI.md`. 3.0.0 ships only when the desktop boots on a real Pi.
- Disk is tight (about 1 GB free) because other sessions run Xcode builds. Check `df -h /System/Volumes/Data` before heavy runs.

## Next, in order

1. CI: rebalance the shards, add a hard timeout per check, make key-driven checks wait on a serial or pixel marker instead of a fixed sleep.
2. Samantha: full screen, the first typed letter is dropped, the degree sign draws as "?".
3. Typography: sharpen text (the standing focus).
4. Hikko, Brick and Hagaki feature parity on real backends.
5. If the Pi is in hand: first serial boot, photograph the console, fix what the real chip does differently.

## How to work

One small PR at a time, up to two helpers on different files, always draft first. Bump VERSION for code changes. Docs stay at 100 percent: every counted source file needs a row in `docs/ARCHITECTURE.md`. Merge the moment CI is green, then remove the worktree and branch.

## Restart prompt

```
/loop until 3.0.0 or just before: Joshua Tree. State is in docs/LOOP-HANDOFF.md, the Pickup section of docs/roadmap.md and the memory note project_joshuatree_qa_backlog.md. Keep merging green PRs, then build the next item. Helper PRs stay draft until CI_LOCAL_JOBS=4 tools/ci-local.sh passes, at most two helpers at once, verify each PR yourself before merging, no stray branches or PRs or issues left behind. 3.0.0 waits on the real Pi 4 booting the desktop.
```
