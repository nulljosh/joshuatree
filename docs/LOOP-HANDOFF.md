# Joshua Tree loop handoff (2026-10-12, late night)

## What the loop is

A `/loop` that works Joshua Tree one tick at a time, all day and night until year end. Each tick reads `~/.claude/loops/joshuatree.md` and does the every-tick steps: the merge chain, one agent at a time, Nimbus grading, and the rules below. Goal order: 3.9.0 first, then 4.0 (many windows at once, mobile first), then 5.0 (talk to it).

## Where things stand

Main is 3.8.4: a grey boot splash with a crisp logo, a graphite grey ARM wallpaper, and a picker that leaves questions about its own topics alone.

PR 594 (draft, branch feat/pi-stack, "3.9.0: the Pi gets every app") folds the ten Pi-stack PRs into one. Its local CI run (tools/ci-local.sh) is live from the jt-stack worktree. It is not merged.

On the real Pi card tested tonight, the desktop boots and the Bluetooth mouse reconnects by itself. Multi-window is still broken, the boot bar is choppy, and the wallpaper still shows terracotta.

PR 596 (draft, branch v10-picker-topics, 3.8.4) carries the small picker: parity 297 of 297, 81.2 percent right on pooled dev, and 20 false tool calls in 434 chat questions. Those numbers are tuned to dev. The sealed sets are still unread, so no done claim yet.

PR 589 (draft, Nimbus, 3.11.0) reached round 7, graded A. PR 595 (draft) is the system design ideas doc, and PR 593 (draft) is the changelog and graphs.

Other drafts from 568 to 591 cover the Pi apps (Finder, Mail, Weather, Stocks, Reminders, Settings, games), window resize, clipboard, sharp apps at 2x, the tree menu, sam as a Terminal chat, and Nimble on the Pi. Three PRs are open and not drafts: 574 (clay wallpaper), 569 (sample song) and 560 (daily driver gaps). Check their CI before the next merge chain.

Bench is DEFERRED while ci-local runs for PR 594.

## Next, in order

1. Let the ci-local run for PR 594 finish. If it is green, take it through the merge chain, flash the dev card only, and board-test multi-window, the boot bar and the wallpaper.
2. Fix what the board finds on the 3.9 line: multi-window, the choppy boot bar, the terracotta wallpaper.
3. Merge the open drafts one at a time, rebased on main, each with its own green CI.
4. Run PR 596 through a full ci-local and a fresh sealed set before calling the picker done.
5. Keep grading Nimbus rounds on PR 589 until it reaches A+.
6. Run the bench once nothing else is running, then open a draft PR with clean numbers.
7. Then 4.0: many windows at once, mobile first.

## Restart prompt

```
/loop Joshua Tree, all day and night until year end. Read /Users/joshua/.claude/loops/joshuatree.md and do one tick exactly as it says (every-tick steps, merge chain, agents, Nimbus grading, rules), then update its State and Log and re-arm with this same prompt.
```

## Rules kept

Never push to a branch while its CI is running, because the workflow cancels in-flight runs. PRs stay draft until tools/ci-local.sh passes. No bare `git stash`, since it is shared across worktrees. Generated files (VERSION, landing, docs/TESTING.md) take main's side in a merge and are regenerated. No keys on the public card. Flash only the dev card. Every X.0.0 ships a refreshed logo and UI.
