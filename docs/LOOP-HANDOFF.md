# Joshua Tree loop handoff (2026-10-05, afternoon)

## What the loop is

Keep merging green PRs, then build the next item toward the 6.0 capabilities (compositor, real multitasking, Samantha voice, accounts, backups) as 2.x steps. Big versions are cut only when their gates are true, so 3.0.0 waits for a real Raspberry Pi 4 boot. One agent at a time, verify every claim myself, run the full local suite before a PR goes ready, merge on GitHub green, check the release cut and the live site. No stray branches, PRs, issues or stale background agents left behind.

## Where things stand (2026-10-05)

- Main is 2.11.1 and live. Shipped today: 2.7.2 icons and the README mark, 2.8.0 Windgate, 2.9.0 Samantha full screen, 2.9.1 her mouth and blink, 2.10.0 a much richer Weather, 2.10.1 the Samantha check made condition-based, 2.11.0 Panes (a second, cmux-style terminal app; Terminal is unchanged), 2.11.1 the landing hero opening on Samantha's face with a poster before the click. The Launchpad has 29 apps.
- Open PR 421, the 2.12.0 mark refresh (rounded forks, engraved grooves, a ground line). Joshua has seen it and likes it. It is ready and waiting on GitHub CI. Its first run had seven jobs cancelled at exactly 15 minutes with empty logs while GitHub's status page said the service was degraded; rerun the failed jobs once the page is green, and do not change code for that.
- In progress: 2.12.1, Samantha's framing on wide screens. Joshua asked to give her face a little room. The approved mock scales her to 86 percent of the window width, puts her eyes about 40 percent down, and fills the sides with her wall colour through a soft feather. Branch `samantha-room`, worktree `/tmp/jt-room`. Joshua wants the final result graded honestly.
- Local testing: on 2026-10-05 afternoon a full-screen game put the terminal's whole process tree at background priority, the Mac ran about four times slower, the OS took about 15 seconds to boot under QEMU, and checks with fixed sleeps failed on clean main too. Before any local suite run `ps -o pri= -p $$`: 31 is normal, 4 is throttled. Do not run the full suite while throttled.
- Policy: helpers open draft PRs and pass `CI_LOCAL_JOBS=4 tools/ci-local.sh` before ready. Run `npx playwright install chromium webkit` first if the demo checks fail in under a second. Close out each agent with TaskStop as soon as its work merges. Agents twice killed processes that were not theirs; tell them to kill by PID only.
- The live site can be checked headless with Playwright (desktop and phone): poster before the click, a real reply and voice after it, Esc to the desktop.
- Known wart: on phones the landing page shows two "Message Samantha" bars, one drawn by the OS inside the demo and one from the page below it.
- Still carrying the 2.0 tree after the mark refresh: `landing/ad.mp4`, `ad-poster.jpg`, `neo-hero.jpg` and the hardware CAD outputs.
- App Store: Hikko iOS 3.0 is the only rejected app in the fleet. Reading why needs an Apple web login (`! asc-login`, 2FA code from Joshua's phone). Apple throttled sign-ins twice on 2026-10-05, so make one attempt only, when Joshua is at his phone. Hamurapi iOS and macOS are waiting for review.
- Samantha's portrait is Joshua's own Higgsfield generation. That question is settled.
- The Pi: Joshua went to buy a Raspberry Pi 4 (4 GB) and the first-boot kit on 2026-10-05. Ask whether he has it before planning around it.

## Next, in order

1. Land PR 421 (the mark) and the 2.12.1 framing change, then grade both for Joshua.
2. If the Pi is in hand: first serial boot per `docs/RASPBERRY-PI.md`. This is the gate for 3.0.0.
3. A design system for Joshua Tree (research the standards, write it down, check the OS against it).
4. Landing refresh with real browser QA at desktop and phone widths, starting with the double message bar on phones.
5. Web voice-in: browser mic to `/api/listen` (Cloudflare Whisper is already in the Worker). The OS mic needs USB audio first.
6. Mobile QA of Samantha and the landing page.
7. Fleet apps, one PR each: Curvely and Numen, Nimble, Sidewise, Wordroot, Brick on its other data source.
8. CI: rebalance the shards, add a hard timeout per check, replace fixed sleeps with waits on a serial or pixel marker.

## How to work

One agent at a time, always a draft PR first, a git worktree outside the main checkout (the main checkout may hold a local commit). Bump VERSION for code changes. Keep docs at 100 percent: every counted source file needs a row in `docs/ARCHITECTURE.md`. When adding an app, copy what the Windgate and Panes PRs touched. Never weaken a threshold; change what is measured and say why in a comment.

## Restart prompt

```
/loop until Joshua Tree reaches the 6.0 capabilities and keep going after (the long road is in docs/roadmap.md). State is in docs/LOOP-HANDOFF.md. Big versions are cut only when their gates are true, so 3.0.0 waits for Joshua's real Raspberry Pi 4 boot; build the capabilities of 4.0, 5.0 and 6.0 as 2.x steps. One agent at a time, full rules in every brief, verify every claim myself, full local suite before ready (check ps -o pri= -p $$ first, 4 means the Mac is throttled), merge on GitHub green, verify the release cuts and the live site, TaskStop agents when their work merges. Keep open PRs and issues at zero between rounds. PushNotification after each landed PR. Check Hamurapi review each round (asc status --app 6819131590); on approval schedule 0.99 for approval day plus 7. Checkpoint at 90 percent usage.
```
