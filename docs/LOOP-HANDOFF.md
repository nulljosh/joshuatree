# Joshua Tree loop handoff (2026-10-05, evening)

## What the loop is

Keep merging green PRs, then build the next item toward the 6.0 capabilities (compositor, real multitasking, Samantha voice, accounts, backups) as 2.x steps. Big versions are cut only when their gates are true, so 3.0.0 waits for a real Raspberry Pi 4 boot. One agent at a time, verify every claim myself, run the full local suite before a PR goes ready, merge on GitHub green, check the release cut and the live site. No stray branches, PRs, issues or stale background agents left behind.

## Where things stand (2026-10-05, 18:10)

- Main is 2.12.1 and live, zero open PRs and issues. Shipped today: 2.7.2 icons and the README mark, 2.8.0 Windgate, 2.9.0 Samantha full screen, 2.9.1 her mouth and blink, 2.10.0 a much richer Weather, 2.10.1 the Samantha check made condition-based, 2.11.0 Panes (a second, cmux-style terminal app; Terminal is unchanged), 2.11.1 the landing hero opening on Samantha's face with a poster before the click, 2.12.0 the mark refresh (rounded forks, engraved grooves, a ground line), 2.12.1 her framing on wide screens (86 percent of the window width, eyes about 40 percent down, wall-colour side bands) plus captions that rise from the bottom, stack to three and fade a few seconds after she goes quiet. Graded for Joshua: framing A-, captions B+, logo A. The Launchpad has 29 apps.
- The main checkout at `~/Documents/Code/joshuatree` holds a stale local commit on `main` (a 2026-10-04 handoff edit). Leave it; work from a worktree of `origin/main` under `/tmp` (never a path containing "samantha").
- Local testing: when Joshua's game (driven by the Conveyer session) is frontmost, macOS drops this terminal's process tree to background priority and the Mac runs about four times slower; fixed-sleep checks then fail on clean main. Before any local suite run `ps -o pri= -p $$`: 31 is normal, 4 is throttled. Do not run the full suite while throttled; tell Conveyer by SendMessage.
- Policy: helpers open draft PRs and pass `CI_LOCAL_JOBS=4 tools/ci-local.sh` before ready. Run `npx playwright install chromium webkit` first if the demo checks fail in under a second. Close out each agent with TaskStop as soon as its work merges. Never message a running workflow agent to add requirements; brief fully up front. Agents twice killed processes that were not theirs; tell them to kill by PID only.
- Branch protection wants the PR up to date with main; after another merge run `gh pr update-branch`. A GitHub job that dies with an empty log while the status page shows an incident is rerun once, never fixed in code.
- The live site can be checked headless with Playwright (desktop and phone): poster before the click, a real reply and voice after it, Esc to the desktop. `tools/checks/hero-poster-check.mjs` waits up to 70 seconds for the poster to lift because the page's own deadline is 60.
- Known wart: on phones the landing page shows two "Message Samantha" bars, one drawn by the OS inside the demo and one from the page below it.
- Still carrying the 2.0 tree after the mark refresh: `landing/ad.mp4`, `ad-poster.jpg`, `neo-hero.jpg` and the hardware CAD outputs.
- App Store (handled 2026-10-05 evening, nothing left to do here): the 4.3 Spam wave got truthful nine-question replies on Hamurapi, Mailbag, Curbfind and Windgate; Hikko iOS was resubmitted with real screenshots; Madobe, Plaintxt iOS and Sidewise iOS were skipped on purpose until they have real features. Check `asc status --app 6819131590` each round; on Hamurapi approval schedule 0.99 for approval day plus 7 (command in its MONEY.md).
- Samantha's portrait is Joshua's own Higgsfield generation. That question is settled.
- The Pi: Joshua found a store that stocks the Raspberry Pi 4 and buys it 2026-10-06. Ask whether he has it before planning around it.

## Next, in order

1. If the Pi is in hand: first serial boot per `docs/RASPBERRY-PI.md`. This is the gate for 3.0.0.
2. A design system for Joshua Tree (research the standards, write it down, check the OS against it).
3. Landing refresh with real browser QA at desktop and phone widths, starting with the double message bar on phones, then the graphics quality of the page.
4. Web voice-in: browser mic to `/api/listen` (Cloudflare Whisper is already in the Worker). The OS mic needs USB audio first.
5. Mobile QA of Samantha and the landing page.
6. Fleet apps, one PR each: Curvely and Numen, Nimble, Sidewise, Wordroot, Brick on its other data source.
7. CI: rebalance the shards, add a hard timeout per check, replace fixed sleeps with waits on a serial or pixel marker.

## How to work

One agent at a time, always a draft PR first, a git worktree outside the main checkout. Bump VERSION for code changes. Keep docs at 100 percent: every counted source file needs a row in `docs/ARCHITECTURE.md`. When adding an app, copy what the Windgate and Panes PRs touched. New Samantha code goes in headers (`user/samcaps.h`, `user/shellcore.h`), not `samantha.c`, which sits at its 2000-line ceiling. Never weaken a threshold; change what is measured and say why in a comment.

## Restart prompt

```
/loop until Joshua Tree reaches the 6.0 capabilities and keep going after (the long road is in docs/roadmap.md). State is in docs/LOOP-HANDOFF.md on origin/main. Big versions are cut only when their gates are true, so 3.0.0 waits for Joshua's real Raspberry Pi 4 boot (he buys it 2026-10-06; first serial boot per docs/RASPBERRY-PI.md is then the top item); build the capabilities of 4.0, 5.0 and 6.0 as 2.x steps. One agent at a time, full rules in every brief, verify every claim myself, full local suite before ready (CI_LOCAL_JOBS=4 tools/ci-local.sh via nohup, Monitor that only reads the log, never pgrep; check ps -o pri= -p $$ first, 4 means the Mac is throttled by Conveyer's game, tell Conveyer), merge on GitHub green, gh pr update-branch after other merges, verify the release cuts and the live version.txt, remove worktrees, TaskStop agents when their work merges, never message a running workflow agent to add requirements. Keep open PRs and issues at zero between rounds. PushNotification after each landed PR. Check Hamurapi review each round (asc status --app 6819131590); on approval schedule 0.99 for approval day plus 7. Checkpoint at 90 percent usage; stay under 95 percent weekly.
```
