# Joshua Tree loop handoff (2026-09-23, afternoon)

## What the loop is

Continue scoped Joshua Tree work headlessly. No loop was started in this Codex session. Work on a branch and open a PR; merge or deploy only after Joshua explicitly approves the PR. Do not start subagents without a request.

## Where things stand

PR #149, branch `fix/stocks-live-data`, replaces the fixed Stocks demo prices with Yahoo Finance quotes and real history. The existing eight symbols remain. Native HTTP and the v86 portfolio embed share `/api/stocks`; the portfolio repo needs no separate code change. Prices refresh every minute while open, R retries, UTC quote times and stale/unavailable states are shown. Provider delays apply. Epiphany's sample holdings remain separate.

Build, Worker route tests, actual kernel parser under ASan/UBSan, all eight upstream quotes in all five ranges, headless boot, offline Stocks dock launch, and keyboard open/close across all 25 apps passed. The regression test fails with the Worker fix reverted. Fixed an offline crash by initializing the NIC before fetching. CI is still running; not merged or deployed. Patch version is 1.0.4; recheck VERSION against main before merge because other PRs are in flight.

The untracked `menubar/` directory predates this work and was left untouched. Earlier Chat, typography, clipboard and icon work is outside this session; inspect current PR state before resuming it.

## Next, in order

1. Check PR #149 CI and review. Fix relevant failures; do not merge or deploy without Joshua's explicit approval.
2. After approval, merge and verify both the native Stocks app and the portfolio embed against the deployed quote endpoint.
3. Resume the requested task or consult `docs/BLUEPRINT.md` and `docs/roadmap.md`. Do not infer approval from old handoff text.

## Restart prompt

```
/loop Resume Joshua Tree from docs/LOOP-HANDOFF.md. First check PR #149's CI and review; finish the Stocks live-price fix for native OS and portfolio embed. Keep checks headless. Work on a branch, no subagents unless requested, and do not merge or deploy without Joshua's explicit PR approval. After that, follow the next direct request or docs/roadmap.md.
```
