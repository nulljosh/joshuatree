# Joshua Tree loop handoff (2026-09-23, evening)

## What the loop is

One Haiku subagent at a time (sequential, never parallel batch). Main session plans on Sonnet; two workers max (Sonnet for scoped features, Haiku for mechanical, Fable only for ring-3/compositor work). Verify every worker result yourself from 4x headless crops before merging. Never a visible QEMU window.

## Where things stand

Merged to main: 1.0.1 (#134) keyboard-only navigation and Lock Screen, 1.0.3 (#139) lag halved (window open 500ms→100ms, wallpaper cached), docs/BLUEPRINT.md (#143) SerenityOS/ToaruOS research and six-phase roadmap toward ring-3 apps and a window server.

Open with auto-merge armed: 1.0.7 (#145) Calendar dock tile shows today's date, Location field in Settings used by weather and map, system clipboard Ctrl+C/X/V, suite-coverage guard; fixed CI by demoting 17 never-run checks to MANUAL, adding librsvg2-bin to the runner, making clipboard check wait on serial. 1.0.8 (#146) icons at retina quality, Apps-folder icons on the same squircle and top light as the dock, iconinset-check passing.

In flight (workers still running): branch typography (1.0.9 typography pass) in /tmp/jt-loop/typography, and turing branch jt-chat (Ollama-shaped /api/chat on the Samantha worker plus a Joshua Tree knowledge pack so the demo Chat answers; silent today because it posts to 10.0.2.2 which does not exist in a browser).

Lesson of the day: CI on this repo is 80 emulator-driven checks on shared runners. Most failures were timing bets. Fix: fold PRs into one run, wait on real signals (not sleeps or timing checks), do not wire 17 new checks at once.

## Next, in order

1. **JT side of Chat:** Samantha default model in settings, proxy allow-list for turing.heyitsmejosh.com only, fake-server check so queries work locally
2. **CI hardening:** Replace timing sleeps with real signals (file existence, process readiness), move timing measurements to main-only section, keep CI checks green before promoting MANUAL checks to the suite one at a time after three green main runs
3. **Typography and icon polish:** Standing items, no blockers
4. **BLUEPRINT phase 1:** Extract apps from kernel.c into separate files (Stocks, Epiphany, Calendar, etc.) as ring-3 libraries ready for network and database work

## Restart prompt

```
/loop Keep the joshuatree loop running: read docs/LOOP-HANDOFF.md and docs/BLUEPRINT.md, land the open PRs one at a time (fold into one integration PR when more than two are queued), verify every worker result yourself from 4x headless crops before merging, never a visible QEMU window, two workers max (Sonnet for scoped features, Haiku for mechanical, Fable only for ring-3/compositor correctness), then: JT side of Chat (Samantha default model, proxy allow-list for turing.heyitsmejosh.com, fake-server check), CI hardening (signals not sleeps, timing measurements main-only, promote MANUAL checks one at a time after three green main runs), typography and icon polish as standing items, then BLUEPRINT phase 1. Short pings in Joshua's voice, taper at 90% session usage.
```
