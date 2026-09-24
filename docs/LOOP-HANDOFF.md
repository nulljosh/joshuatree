# Joshua Tree loop handoff (2026-09-24, morning)

## What the loop is

Continue scoped Joshua Tree work headlessly from a cloud session (no Mac display, no QEMU window, never the JT app). Work on a `claude/*` branch per item, open a PR, and merge it as soon as its CI is green: Joshua gave standing authorization on 2026-09-24 ("keep merging PRs as we go, make sure they pass CI"). One subagent at a time (Sonnet; Fable for kernel internals). Every PR ships a discriminating check, a `docs/ARCHITECTURE.md` row per new file, a `VERSION` bump.

## Where things stand

Merged today, each auto-released by `release.yml` and the landing page redeployed by `deploy.yml`: #145 (1.0.7), #146 (1.0.8), #148 (1.0.9), #157 (1.0.11: every app window drags live by its title bar, `window_move_rect` in `drivers/window.c`, drag state in `gui_app_mouse_tick`; folds #155's `tools/checks/filerobust-check.py`), #159 (1.0.12: Chat talks to Samantha).

Chat is confirmed working end to end. `kernel/chat.h` defaults to model `samantha` at turing.heyitsmejosh.com:80 (the Turing project's Ollama-compatible `POST /api/chat`). CI's `network` job ran `tools/checks/chat-samantha-check.py --live` on a real runner and the real host answered over plain HTTP with a real reply, so the "host forces HTTPS" worry was unfounded; the 3xx handling stays as a clear status for any host that does redirect. `tools/checks/chatapp-check.py` drives the real GUI Chat app over QMP against a fake Samantha and proves the reply's ink lands in the window. `worker.js` forwards only `POST /api/chat` to that one host (JSON, 8 KB cap). The demo tour's Chat scene now presses Enter and sends a real question through the proxy.

In flight: a Sonnet subagent on `claude/demo-tour-chat` (1.0.13): a live window-drag scene in the idle tour (Notes, drag by the title bar and back), `tools/checks/demochat-check.mjs` (Playwright, headless Chromium, hermetic: `page.route` fakes the proxied Samantha reply, then counts CHAT_INK pixels on the v86 canvas to prove the reply renders in the landing demo) wired as a `demo` CI job the `check` gate needs, and a regenerated `progress.svg`.

Branch protection requires `check`, `check-refs` and `network` and "up to date with main", so every merge invalidates every other open PR; merge serially and fold small PRs together when possible. After each merge: merge `origin/main` into the next branch, keep the branch's own version, `git checkout HEAD -- landing/index.html`, run `python3 tools/gen/inject-landing-facts.py`, verify (`version-bump-check.sh origin/main`, `landing-facts-check.py`, `suite-coverage-check.sh`, `check-refs.sh`, `check.sh`), push, wait for green, merge (squash). The Claude app's auto-merge is unavailable for this repo, so merge by hand.

Follow-ups noted, not done: CodeRabbit on #157 (merged before its review landed): after a drag the window's rounded-corner pixels carry wallpaper sampled at the old position (`window_move_rect` copies the full rectangle; recompose the four corners at the destination), `tools/checks/windowdrag-check.py` should compare the exposed strip against a clean wallpaper baseline captured before the app opened, `tools/checks/filerobust-check.py`'s two `subprocess.run` calls need finite timeouts, and 1.0.11 should have been a MINOR bump (new capability) per CLAUDE.md. The `network` job's three wallpaper steps (`wallpaper-check.py`, `wallfx-check.py rain`, `satellite-wallpaper-check.py`) fail on every run with an fnv mismatch between the kernel's composed tiles and the host's, hidden by `|| true`; worth a real root cause. `loctest` (kernel.c) should switch to ramfs and restore the full location state (CodeRabbit on #145); the typography check's edge detection could tolerate the night tint instead of pinning the clock.

Environment notes for a fresh cloud session: `apt-get install -y qemu-system-x86 librsvg2-bin` works; the apt `python3-pil` is broken against the container's python, use `pip install pillow`; Playwright's Chromium is preinstalled at `/opt/pw-browsers`, `npm install` in the worktree gives the `playwright` package. The sandbox proxy blocks turing.heyitsmejosh.com and joshuatree.heyitsmejosh.com, so live proof only comes from CI or Joshua's own browser. Worktrees live under the session scratchpad, one per branch.

Also in flight: a second Sonnet subagent on `claude/chat-tools` (1.1.0): Chat asks Turing's `POST /api/pick` (`{"q":...}` -> `{"tool","arg"}`) before `/api/chat` and runs the picked tool in the kernel (new_reminder, new_note, open_app, weather from the cached reading, calendar_today, say), with `tools/checks/chattools-check.py` plus a `--live` step in the `network` job, and `worker.js` forwarding `/api/pick` like `/api/chat`. Joshua asked for this directly ("confirm we can use tools from the chat app"). Related: the Turing repo (nulljosh/turing, attached with push access, clone at /home/user/turing) has an uncommitted one-line edit to `worker.js`'s "what can you do" small-talk reply so it no longer says her hands are only on a Mac; push it to Turing's main as a patch release (its CLAUDE.md: work on main, bump VERSION, run its test set) only after 1.1.0 is on Joshua Tree's main. Turing's parity, laws and chat tests pass with the edit.

Queued for the next code PR: `tools/checks/ci-suite.sh` on main carries leftover conflict markers (lines 96-100) that make the runner skip `iconinset-check.py` and `calicon-check.py`; both pass locally, so just delete the three marker lines. Version names with no letters (direct request): `.github/workflows/release.yml` tag `jt-v$v` -> `$v`, release title `v$v:` -> `$v:`, ISO `joshuatree-v$v.iso` -> `joshuatree-$v.iso`, and CLAUDE.md's Versioning section to match.

Usage: Joshua reported 80% of his Claude usage at 05:40 UTC. Conserve mode from here: no new subagents, finish and merge the two branches above, then stop at 90%.

## Next, in order

1. Merge the demo tour PR (`claude/demo-tour-chat`, 1.0.13) and the Chat tools PR (`claude/chat-tools`, 1.1.0) on green, serially, re-merging main between them; fold the queued ci-suite/release.yml fixes into whichever goes last; then push the Turing reply edit.
2. Root-cause the `network` job's wallpaper fnv mismatch (above) so that job stops hiding failures behind `|| true`.
3. Then the roadmap's "Biggest gaps" list top down, stability, reliability, compatibility, accessibility and security first.

## Restart prompt

```
/loop Resume Joshua Tree from docs/LOOP-HANDOFF.md. Merge the demo tour PR (claude/demo-tour-chat) and the Chat tools PR (claude/chat-tools) when CI is green, fold in the queued ci-suite and release.yml fixes, push the Turing reply edit, then root-cause the network job's wallpaper fnv mismatch, then work docs/roadmap.md's "Biggest gaps" list top down. Headless only, one subagent at a time, every PR ships a check and an ARCHITECTURE row, merge on green.
```
