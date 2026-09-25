# Joshua Tree loop handoff (2026-09-25, early morning)

## What the loop is

Keep building Joshua Tree headlessly toward 2.0. Stability, usability and performance come before feature count. Merge on green CI, never ask. Sonnet subagents, max 4 at once (2 if any is Fable), 15 minutes each. Subagents push a PR and hand back; they never wait on CI or start watchers. The main session watches CI, bundles PRs into one run when several queue (branch protection reruns CI per merge), QA's every result from real frames or screenshots at 4x, and merges. Stop at 90% session or weekly usage with /checkpoint.

A+ is the bar: everything spotless, every line in Joshua's voice mixed with Apple and Steve Jobs. No lines of code or check counts as public metrics.

## Where things stand

Main moved 1.1.5 to 1.4.x tonight. Shipped: Notes text selection, Chat that reads like a product with an empty state full of things to try, every Samantha tool shown in the landing demo, smooth antialiased window corners, Calendar padding, menu bar weather color, uptime gone, CI shards balanced by real timings (about 12 minutes down to 8), a new one-color engraved tree mark, a landing page on one design system with Meet Samantha and app sections, a privacy page, and the demo's weather freeze fixed at the relay (8 second timeout).

In flight: PR #184 (1.4.1 bundle) in CI; branch `claude/int-1.4.3` holds the next bundle (landing QA round 3, copy pass, the stb_truetype font engine). An agent is wiring the font engine into Notes on `claude/notes-ttf`: sharp text at physical resolution, sizes 12 to 200, four families, and it must pass textsharp-check.py (a bilinear shortcut failed it and was reverted).

Kernel memory ceiling: about 227KB headroom before .bss hits the ring-3 window. Fonts must be subset (17KB each for Latin).

## Next, in order

1. Land #184, then push `claude/int-1.4.3` as one PR.
2. Land Notes on the font engine; then Terminal and the UI font on the same path.
3. Landing to A+: grade from full-page screenshots (scroll, force reveal opacity, serve over http so CSS masks load).
4. Roadmap stability items top down: nothing crashes, keyboard floor for every app, Files view buttons.
5. The 2.0 spine: a libc, loading programs from disk, apps outside the kernel, then a tiny C compiler.
6. Sound (AC97) and a Music app.

## Restart prompt

```
/loop Joshua Tree loop toward 2.0: read docs/LOOP-HANDOFF.md first. Merge PRs on green CI, bundle queued PRs into one CI run, Sonnet subagents max 4 at 15 minutes each with no CI watchers, QA every result from real frames at 4x, headless only, landing to A+ in Joshua's voice, stop at 90% session or weekly usage with /checkpoint.
```
