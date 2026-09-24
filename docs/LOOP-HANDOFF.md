# Joshua Tree loop handoff (2026-09-24, early morning)

## What the loop is

Continue scoped Joshua Tree work headlessly from a cloud session (no Mac display, no QEMU window, never the JT app). Work on a `claude/*` branch per item, open a PR, and merge it as soon as its CI is green: Joshua gave standing authorization on 2026-09-24 ("keep merging PRs as we go, make sure they pass CI"). One subagent at a time (Sonnet; Fable for kernel internals). Every PR ships a discriminating check, a `docs/ARCHITECTURE.md` row per new file, a `VERSION` bump.

## Where things stand

Merged today: #145 (1.0.7: Calendar date, Location, clipboard, checks in CI, plus five review fixes) and #146 (1.0.8: icons).

Open, all with CI running as of 04:45 UTC, merge in this order because each carries the next `VERSION`:
1. #148 (1.0.9, typography). Its own `tools/checks/baseline-check.py` was time-of-day dependent (the night wallpaper tint hid the Apps panel edge); fixed by pinning QEMU's RTC to a daytime hour in the check.
2. #155 (1.0.10, `tools/checks/filerobust-check.py`: empty, oversized, corrupt-FAT, full-disk cases). No kernel bug found; the corrupt-file case now requires exactly `n=4095`.
3. #157 (1.0.11, every app window drags live by its title bar: `window_move_rect` in `drivers/window.c`, drag state in `gui_app_mouse_tick`, multi-window drags move live; `tools/checks/windowdrag-check.py`).

Every merge moves `VERSION` on `main`, which re-conflicts the remaining PRs on that one line and makes GitHub skip their workflow until re-merged. After each merge: merge `origin/main` into the next branch, keep the branch's own version, `git checkout HEAD -- landing/index.html`, run `python3 tools/gen/inject-landing-facts.py`, verify (`version-bump-check.sh origin/main`, `landing-facts-check.py`, `suite-coverage-check.sh`, `check.sh`), push, wait for green, merge (squash).

In flight: a Sonnet subagent on `claude/chat-samantha` (1.0.12) wiring Chat to Samantha, the Turing project's Ollama-compatible `POST /api/chat` at turing.heyitsmejosh.com (defaults `llm_host`/`llm_port`/`llm_model`, a POST-forwarding exception in `worker.js`'s proxy for that one host and path, the demo tour's Chat step pressing Enter, a fake-server kernel check, a live step in the CI `network` job). The sandbox proxy blocks turing.heyitsmejosh.com and joshuatree.heyitsmejosh.com, so live proof only comes from CI or Joshua's own browser.

Direct requests from Joshua still open, in order: Chat working (above); windows draggable (#157); the landing demo tour with real in-app interaction (Files, Notes with paste, Terminal commands, Calculator, Contacts, Reminders, a window drag, Chat sending to Samantha) plus a headless-browser check, in the style of `tools/checks/lazy-boot-check.mjs`, that the Chat reply actually renders in the v86 demo (locally behind a fake Samantha via the proxy shim).

Follow-ups noted, not done: `loctest` (kernel.c) should switch to ramfs and restore the full location state (CodeRabbit on #145); the typography check's edge detection could tolerate the night tint instead of pinning the clock.

Environment notes for a fresh cloud session: `apt-get install -y qemu-system-x86 librsvg2-bin` works; the apt `python3-pil` is broken against the container's python, use `pip install pillow`. Worktrees live under the session scratchpad, one per branch.

## Next, in order

1. Merge #148, #155, #157 as each goes green, re-merging `main` into the rest after every merge.
2. Review and merge the Chat PR (`claude/chat-samantha`) when the subagent opens it; watch the CI `network` job's live step for whether turing.heyitsmejosh.com answers over plain HTTP (the kernel has no TLS).
3. Spawn one subagent for the demo tour interactivity plus the landing Chat render check; merge on green; `deploy.yml` publishes the landing page on merge.
4. Then the roadmap's "Biggest gaps" list top down, stability, reliability, compatibility, accessibility and security first.

## Restart prompt

```
/loop Resume Joshua Tree from docs/LOOP-HANDOFF.md. Merge #148, #155, #157 as each passes CI (re-merge main into the rest after every merge, VERSION conflicts only). Then the Chat-to-Samantha PR, then the demo tour interactivity with a headless-browser check that the Chat reply renders on the landing page. Headless only, one subagent at a time, every PR ships a check and an ARCHITECTURE row, merge on green.
```
