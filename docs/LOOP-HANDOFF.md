# Joshua Tree loop handoff (2026-09-24, 11:45 UTC)

## What the loop is

Keep building Joshua Tree headlessly from a cloud session: no Mac display, no QEMU window, never the JT app. One claude/* branch per piece, a PR per piece, merge the moment CI is green (standing authorization from Joshua, 2026-09-24). Small pieces: one feature or fix per branch, one push per branch. Every PR ships a check and a `docs/ARCHITECTURE.md` row per new file, plus a short plain-words roadmap line (see CLAUDE.md, "Writing docs"). Report usage each pass; checkpoint this file before context hits 90%.

Target: 1.2.0, then keep going. Sound (AC97) is pencilled as 1.3.0. Compositor, TLS and real hardware are weeks each, not loop items.

## What shipped today

1.0.7 through 1.1.2 are on main, released with bare tags (`1.0.13` style), landing page redeployed on each merge. Highlights: windows drag live by the title bar (1.0.11); Chat talks to Samantha, the Turing project's model, over the network (1.0.12, proven against the real host from CI); Samantha acts from Chat: reminders, notes, open an app, weather, today's calendar (1.1.0); the landing tour drags a window and a Playwright check proves the demo's Chat renders a reply, gating merges (1.0.13); CI emails stopped (superseded runs end cancelled, 1.1.1; no duplicate push-to-main run, 1.1.2); the wallpaper network checks compare the theme they boot in (1.1.2). Turing v4.2.1 says Samantha has hands in Joshua Tree.

## Open PRs (merge on green, in this order, re-merging main between)

- #165 `claude/docs-sync` (1.1.3): README, landing copy, roadmap catch-up, plain-words rule, CI badge reads pull-request runs.
- #166 `claude/cursor-glide` (1.1.4): the tour's pointer glides instead of teleporting; `tools/checks/cursorglide-check.mjs` in the demo job.
- Coming from subagents: `claude/drag-corners` (Chat reply timeout bounded, clipboard check hardened, filerobust subprocess timeouts; the rounded-corner repaint after a drag moves to `claude/reliability-2`), `claude/desktop-basics` (text selection with copy and cut in Notes plus a tour scene; right-click desktop menu and app switcher follow on `claude/desktop-basics-2` and `-3`, 1.2.0).
- #151 `claude/e1000` is another session's draft. Leave it alone.

Merge recipe: `git merge --no-commit --no-ff origin/main`; conflicts are only VERSION, `landing/version.txt`, `landing/index.html`, and added lines in `docs/roadmap.md` / `docs/ARCHITECTURE.md`. Keep the branch's version (bump if main passed it), `git checkout HEAD -- landing/index.html`, `python3 tools/gen/inject-landing-facts.py`, keep both sides' added doc lines, `git commit` the merge, then `version-bump-check.sh origin/main`, `landing-facts-check.py`, `suite-coverage-check.sh`, `check-refs.sh`, `landing-roadmap.py --check`, `check.sh`, and push. Merges are serial because branch protection requires "up to date".

## Next, in order

1. Merge the open PRs above as they go green.
2. Samantha acts inside apps, not just opens them: new picker tools on the Turing side (`worker.js` PICKABLE, PICK_SYSTEM, `S.sound`, plus `web/samantha.js` parity; Turing works on main, runs its check set, bumps VERSION) and kernel handlers in `kernel/chat.h`: add a calendar event, mark a reminder done, send mail, search files, set the wallpaper, read a note back. Extend `tools/checks/chattools-check.py`. MINOR bump.
3. Docs readability pass: older roadmap entries, `docs/ARCHITECTURE.md` rows to one or two plain sentences, this file. Prose-only.
4. Network job leftovers (non-blocking, warnings only): `wallpaper-check.py`'s tint model is 23.7 off the kernel's warm-map tint; `wallfx-check.py rain` captures a black band after re-entering the GUI; `satellite-wallpaper-check.py` compares an exact hash against PIL's JPEG decode, impossible with `drivers/jpeg.c`'s documented tolerance (use `tools/jpeg-host` or a diff bound).
5. Sound: AC97 driver, a tone test, 1.3.0.
6. Samantha on-device (roadmap, unscheduled, [Fable]).

## Environment notes

Worktrees live under the session scratchpad, one per branch. `apt-get install -y qemu-system-x86 librsvg2-bin`; use `pip install pillow` (the apt python3-pil is broken here). Playwright's Chromium is preinstalled at `/opt/pw-browsers/chromium` (a different revision than playwright-core expects, so checks pass `executablePath` when that path exists); `npm install` in a worktree, or symlink `node_modules` from another. `landing/v86/kernel.elf` is a build artifact: `make kernel.elf` before any browser check. The sandbox cannot reach turing.heyitsmejosh.com, ip-api.com or the tile hosts from the guest; live proof comes only from CI's `network` job. Turing is cloned with push access at /home/user/turing.

Usage at this checkpoint: rate limit allowed, no overage, context 71%, session cost about $577.

## Restart prompt

```
/loop Resume Joshua Tree from docs/LOOP-HANDOFF.md and aim for 1.2.0 and beyond: merge my open PRs as they go green (re-merge main between them), then work "Next, in order". Small pieces: one feature or fix per branch and PR. One subagent per piece (Sonnet; Fable for kernel internals), worktrees under the session scratchpad, headless only, never touch codex/* or claude/e1000. Every PR ships a check and an ARCHITECTURE row, plain-words docs, merge on green, report usage each pass, checkpoint before 90%.
```
