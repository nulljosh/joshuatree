# Joshua Tree loop handoff (2026-10-05, morning)

## What the loop is

Keep merging green PRs, then build the next item toward the 6.0 capabilities (compositor, real multitasking, Samantha voice, accounts, backups) as 2.x steps. Big versions are cut only when their gates are true, so 3.0.0 waits for a real Raspberry Pi 4 boot. One agent at a time, verify every claim myself, run the full local suite before a PR goes ready, merge on GitHub green, check the release cut and the live site. No stray branches, PRs, issues or stale background agents left behind.

## Where things stand (2026-10-05)

- Main is 2.11.0 and live. Shipped since the last handoff: 2.7.2 icons and the README mark, 2.8.0 Windgate, 2.9.0 Samantha full screen with fading captions and a mouth driven by her voice, 2.9.1 her mouth parting along the real lip seam plus a blink, 2.10.0 a much richer Weather, 2.10.1 the Samantha check made condition-based, 2.11.0 Panes, a cmux-style tabs and splits terminal that shares Terminal's shell code. Terminal itself is unchanged.
- The Launchpad has 29 apps. No PRs or issues are open.
- Policy: helpers open draft PRs and pass `CI_LOCAL_JOBS=4 tools/ci-local.sh` before ready. Run `npx playwright install chromium webkit` first if the demo checks fail in under a second. Close out each agent with TaskStop as soon as its work merges.
- Two GitHub-only timing flakes were seen and passed on one rerun: the Hikko forum check (shard 3) and the Samantha full-screen check (shard 4, since hardened). Rerun a failed job once before digging.
- The landing hero demo is a blank box until clicked and opens on the desktop. It should show something first and open straight on Samantha's face.
- The landing badge was not refreshed in 2.7 to 2.11. Joshua said to refresh the artwork. Follow the logo-refresh skill and `docs/BADGE.md`, and send the preview to Joshua.
- App Store: Hikko iOS 3.0 is the only rejected app in the fleet. Reading why needs an Apple web login (`! asc-login`, 2FA code from Joshua's phone). Apple throttled sign-ins at 08:58 on 2026-10-05, so wait until after 10:30 and make one attempt.
- Samantha's portrait: Joshua thinks it resembles a real actress. Check where the portrait came from before the site goes further public.

## Next, in order

1. Landing hero: show something before the click and open on Samantha's face. Then the logo and badge refresh.
2. A design system for Joshua Tree (research the standards, write it down, check the OS against it).
3. Landing refresh with real Chrome QA at desktop and phone widths.
4. Web voice-in: browser mic to `/api/listen` (Cloudflare Whisper is already in the Worker). The OS mic needs hardware.
5. Mobile QA of Samantha and the landing page.
6. Fleet apps, one PR each: Curvely and Numen, Nimble, Sidewise, Wordroot, Brick on its other data source.
7. CI: rebalance the shards, add a hard timeout per check.
8. If the Pi is in hand: first serial boot per `docs/RASPBERRY-PI.md`.

## How to work

One agent at a time, always a draft PR first, a git worktree outside the main checkout (the main checkout may hold a local commit). Bump VERSION for code changes. Keep docs at 100 percent: every counted source file needs a row in `docs/ARCHITECTURE.md`. When adding an app, copy what the Windgate and Panes PRs touched. Never weaken a threshold; change what is measured and say why in a comment.

## Restart prompt

```
/loop until Joshua Tree reaches the 6.0 capabilities and keep going after (the long road is in docs/roadmap.md). State is in docs/LOOP-HANDOFF.md. Big versions are cut only when their gates are true, so 3.0.0 waits for Joshua's real Raspberry Pi 4 boot; build the capabilities of 4.0, 5.0 and 6.0 as 2.x steps. One agent at a time, full rules in every brief, verify every claim myself, full local suite before ready, merge on GitHub green, verify the release cuts and the live site, TaskStop agents when their work merges. Keep open PRs and issues at zero between rounds. PushNotification after each landed PR. Check Hamurapi review each round (asc status --app 6819131590); on approval schedule 0.99 for approval day plus 7. Checkpoint at 90 percent usage.
```
