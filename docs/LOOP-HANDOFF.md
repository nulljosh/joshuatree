# Joshua Tree loop handoff (2026-09-25, afternoon)

## What the loop is

Continue scoped Joshua Tree work headlessly. Work on a branch and open a PR; merge or deploy only after Joshua explicitly approves the PR. No subagents unless he asks. Weekly Claude usage was at 93 percent on 2026-09-25 and resets Saturday at 22:00, so heavy work waits for the reset.

## Where things stand

PR #203, branch `fix/portfolio-inapp-boot`, version 1.5.6, open and not merged. It does two things. Portfolio mode (`?full`, what heyitsmejosh.com frames) boots right away instead of waiting on an IntersectionObserver, the fix for the page not loading in X's in-app browser. Epiphany prices its holdings and first eight watchlist symbols off Stocks' live `/api/stocks` quotes, refreshed on open, once a minute and on r; crypto, commodities and macro stay sample. The kernel builds, `lazy-boot-check.mjs` passes, the pre-push suite passed. Not yet booted headlessly into Epiphany, and not checked on a real phone in X.

PRs #201 (libjt) and #202 (boot splash) are open from other sessions. The main checkout sits on the old `fix/stocks-live-data` branch with an uncommitted edit to `kernel/kernel.c` that drops the uptime line from About; it is not from this session, leave it alone until Joshua says.

## Next, in order

1. Check PR #203 CI. Boot it headlessly and open Epiphany to confirm live prices draw. Recheck VERSION against main before merge, other PRs are in flight.
2. After Joshua approves, merge and deploy, then ask him to open heyitsmejosh.com from a post in the X app.
3. Roadmap Session task queue item 1: real app demos in portfolio mode. Picking an app should play a short walkthrough, starting with the five homepage picks.

## Restart prompt

```
/loop Resume Joshua Tree from docs/LOOP-HANDOFF.md. First check PR #203's CI, boot it headlessly and confirm Epiphany shows live prices. Do not merge or deploy without Joshua's explicit approval. Then build real app demos for portfolio mode (roadmap Session task queue item 1), on a branch, one PR. No subagents. Watch Claude usage and stop heavy work above 95 percent weekly.
```
