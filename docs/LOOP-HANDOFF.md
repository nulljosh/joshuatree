# Loop state, 2026-09-23 06:10 (read this first)

**1.0.0 Hidden Valley shipped** (jt-v1.0.0, ISO and SHA256SUMS attached, CI green). 1.0.1 is PR #134: keyboard-only navigation (25/25), Lock Screen in the Apple menu (proven: locks, Esc and a wrong password cannot bypass, password unlocks), the pre-push hook that runs every fast check, CodeRabbit config that keeps only actionable comments.

In flight: `lag-profile` (Sonnet, issue #14: instrument frame time, find the top costs, fix, measure, 1.0.2) and `readme-short` (Haiku, README to 45 lines, details moved to the other docs).

Rules that now hold by machine, not memory: every code PR must bump VERSION (version-bump-check.sh in CI and in the pre-push hook) so every merge cuts a release with its ISO; the landing page's numbers are generated from the source at deploy and guarded by landing-facts-check.py; the README badge is the live release badge. Keep "require branches up to date" ON; land PRs one at a time, or fold several into one integration PR when the queue is long (that is how 1.0.0 went out: one CI run instead of five).

Every worker report is verified from real frames before merging. Tonight's false claims caught on review: a "no clipping" over a ":." rank, every "yes" scored as good, a teal bar, a lock-screen PASS with the account half never run, a panic check that exited to text mode first, a keyboard check reporting 0/25 because of a close-dot coordinate. Sonnet workers were reliable; Haiku ones need a frame to prove each claim.

Next: land 1.0.1, review the lag PR's before/after numbers by eye, then docs/roadmap.md's After 1.0 list top down (Bluetooth and languages are 1.1 design items for Joshua).

# Loop state, 2026-09-23 about 00:00 (read this first)

Stopped at 100% session usage. 1.0 is a Snow Leopard release (docs/roadmap.md): stability, security, speed, no features.

Open PRs, all auto-merge armed, land in this order with `gh pr update-branch` as each goes BEHIND: #116 Curbfind (0.99.0), #117 Shift everywhere + Notes word wrap + gallery and feature drive in CI (0.99.1), #118 parser fuzz + FAT mount DoS fix, #119 roadmap scope, #120 ISO and SHA256SUMS on every release, #121 security QA (masked Settings passwords, json.c bound, 1 s login delay, input-bounds-check.sh).

Pushed branches, no PR yet, need a look first:
- `soak-test`: soak-check.py (3 passes in CI, 20 by hand) plus pmm_free_frames_last/task_used_last in drivers/window.c. The 20-pass run was started in the background at /tmp/jt-loop/soak20.log; read its last lines before trusting it.
- `keyboard-a11y`: Enter on the bare desktop opens the Apps folder (kernel change) and keyboard-only-check.py, but the check FAILS for apps 19 to 24 (the fifth grid row never opens). Likely the script's row navigation, not the kernel. Fix, rerun, then PR.

Verified tonight by eye from headless frames: every dock and Apps-folder app is native (26/26 open, feature drive 16 actions change the screen), typography in Notes at 3 faces x 4 sizes x 2 weights is clean, all 95 printable characters draw in Notes and Terminal, Notes wraps at word boundaries.

Still open for the 1.0 gate: release notes (Haiku), e1000 driver stretch, lag issue #14, `gui-prompt-reminders-check.sh` is an orphan that fails on main. Joshua's decisions for 1.0 are in the roadmap: no Wi-Fi, no Bluetooth, English only.

Rules that held tonight: up to 4 Haiku workers or 2 Sonnet at once, every worker report verified from real frames before merging (workers claimed clean layouts over real bugs four times), never push a speculative CI run, keep "require branches up to date" ON (turning it off tripped the safety classifier).

# Loop state, 2026-09-22 evening (read this first)

Main is 0.89.0, released as `jt-v0.89.0`. Merged tonight: #95 Calendar views, #83 Esc, #87 Terminal mono, #96 icon redraw, #86 decoder fuzzing, #65 Chat default, #62 Activity, #97 landing copy. #88 and #92 were closed because #93 carries them.

Open, in landing order: #99 (typing checks wait for each key), #93 (Toroid and Quotes native, real fleet icons; its only CI failure was the flake #99 fixes), #98 (dock shadow follows the photo), #100 (CI in four parallel shards, auto-release on VERSION bump), then this ledger PR.

How the loop runs now:
- One Monitor on the open PRs is the wake signal, a 30 minute ScheduleWakeup is the fallback. Land one PR at a time; after each merge, `git pull` the local main and merge the new main into the next PR's worktree under `/tmp/jt-loop`.
- Conflicts in `kernel/icon_art.h`: never re-rasterize on a Mac to resolve them (rsvg bytes differ from the cloud session's). Keep main's bytes and only re-index the tables, then diff the table layout against a local `gen_icon_art.py` run.
- A check that fails only in CI is usually a timing flake. Reproduce with the slow QEMU shim (`taskpolicy -c background` in front of `qemu-system-i386` on PATH) before touching kernel code, and fix it by waiting on a real signal, never a longer sleep.
- Never push a speculative CI run: every failure emails Joshua.
- After #100: releases cut themselves on a VERSION bump. Pending Joshua: turn off "require branches up to date" so green PRs merge without queueing.

Next after the queue: QA gallery (1.0 list), then the Beta list top down (landing hero, dock polish, boot splash mark).

Restart prompt (current): `/loop Drive joshuatree to a defensible 1.0.0 following docs/LOOP-HANDOFF.md: land the open PRs one at a time, then work the Beta list in docs/roadmap.md top down. One Haiku worker at a time, review output yourself from zoomed headless frames, never a visible QEMU window, every fix a PR, short pings.`

# Handoff, 2026-09-23 about 00:55 UTC (cloud session, moving to desktop)

## Landed on main
- #95 (0.88.3): Calendar Day/Week/Month/Year views, title-bar gap fixed in 9 windowed apps, Apps-folder apps named in their frame, dock hover label capsule, QA tour driver fixed, three dead checks revived.
- #83: Esc closes the focused window instead of quitting the desktop.

## In flight, auto-merge armed (lands by itself when CI is green)
- #96 (0.89.0): Big Sur dock icons (`tools/gen/restyle_icons.py`, `tools/checks/iconlight-check.py`), text stem darkening (`text_ink`, `tools/checks/textsharp-check.py` covering Mail, Notes and Weather), Calendar fits a snapped half window. Full `tools/checks/ci-suite.sh` 49/49 locally.
- #87 (Terminal mono face) and #86 (decoder fuzz + auth.h pass): both merge cleanly on main + #96 and pass their checks locally (19,760 fuzz cases). Main requires up-to-date branches, so after #96 lands run "Update branch" on #87, then on #86 after #87 lands.

## Saved, not yet a PR (patches from two stopped workers)
- `docs/wip/calendar-date-icon.patch`: Calendar dock tile draws today's date from the RTC instead of a fixed "SEP 17", plus `tools/checks/calicon-check.py` (proven fail-before/pass-after). Apply with `git apply`, then regenerate the icon art: `python3 tools/gen/restyle_icons.py` and `python3 tools/gen/gen_icon_art.py` (needs `rsvg-convert`). Run its check and the icon checks before shipping.
- `docs/wip/qmp-harness.patch`: shared `tools/checks/jtqmp.py` (boot, move/click/key with the 0.35 s gap, dump, wait_until, dock geometry from kernel.c) with apptop, calviews and dockhover migrated. Unfinished: the suite-coverage guard (fail when a check isn't in `tools/checks/ci-suite.sh`) and the ARCHITECTURE rows. Verify each migrated check still fails on reverted code.
- Delete `docs/wip/` once both are applied.

## Next, in order
1. Land #96, #87, #86 (update branch, let auto-merge run).
2. Follow-up PR: apply the two patches; `.coderabbit.yaml` (drop docstring coverage, skip `kernel/icon_art.h` and generated files, review on PR open only); run `text_ink` through `gui_aa_char_mono` too, so Terminal text gets the same sharpening, with a Terminal row in textsharp-check.
3. Remaining old PRs, one at a time: #65 Chat qwen default, #62 Activity Monitor, the #88 -> #92 -> #93 stack (Toroid, Quotes, real fleet icons; #93 conflicts on `kernel/icon_art.h`: regenerate it with `gen_icon_art.py`, never hand-merge), then the #89/#90 handoff docs (probably superseded by this one; close if so).
4. Joshua's priorities: icons more macOS (the 15 Apps-folder fleet icons are next, same restyle table), sharper text everywhere, then QA and a headless test for every app, then roadmap bugs.

## Rules learned this session
- Never edit a tree while `ci-suite.sh` runs in it; use a worktree.
- Apps-folder keyboard checks need 0.35 s between keys and must wait for the app on screen, not sleep: the CI runner is slower (reproduce by loading every core).
- Joshua's standing rule: merge any PR once CI is green. Arm auto-merge (squash).
- CodeRabbit: red and Major findings are bugs to fix; Minor ones get a one-line reply and ride the next push.

## Restart prompt (historical, use the current one at the top)
Continue joshuatree from docs/LOOP-HANDOFF.md's top section: land #96/#87/#86, open the follow-up PR from docs/wip/, then work the old PR queue and Joshua's priorities. Main session manages, workers build in worktrees, review every screenshot and check yourself, auto-merge when CI is green, one-line TLDR per pass.

---

Everything below is history, kept for context. The current state and restart prompt are at the top.

# Tonight's plan to 1.0.0 (written 2026-09-21, about 01:20)

One worker at a time, each run is a batch, main session reviews every result by eye before it merges. Stop spawning at 85% weekly usage; tag 1.0.0 only if the gate below is met, otherwise tag 0.9.0 and say why.

1. Apps window cleanup. In flight.
2. Text batch: Terminal draws the Mono face at its real advance (today "m" is crushed and "i" floats); a typography sweep over every app at full resolution; a system typeface picker in Settings using the faces already embedded. Fonts loaded from disk wait for 1.1.
3. Layout batch: Calendar's clipped last week, the blank strip under the title bar in five apps, dock hover label backing, and `tools/qa_demo_drive.py` fixed so the tour covers Weather, Trash and every Apps-folder app.
4. Security batch: fuzz the PNG and JPEG decoders on the host (`tools/png-host`, `tools/jpeg-host`) and the HTTP, DNS and FAT parsers with hostile input; bounds audit of every text field; audit `kernel/auth.h` (salted iterated SHA-256 and the boot login already exist) for lockout, empty passwords and timing; add a lock screen from the menu.
5. Stability batch: every one of the 25 apps opened, used and closed in one headless gallery with panic detection; bad input, long lines, empty files, no disk, no network.
6. Main session watches the full gallery at full resolution, files what is ugly, one fix batch.
7. Docs refresh and honest release notes (Haiku). Full suite green. Tag `jt-v1.0.0`, release, landing updated.

Moved to 1.1: Photos app, fonts from disk, Settings consolidation beyond the typeface picker, snap keyboard shortcuts, icon redraws unless the gallery shows a bad one.

Gate for the 1.0.0 tag: full suite green, gallery reviewed with no open visual bug, fuzzers clean, no panic in any stability case.

Machine etiquette learned tonight: checks must never run a broad `pkill -f qemu-system-i386`; it kills other sessions' runs. Kill by PID or unique `-name`. Run QEMU checks one at a time.

# Joshua Tree loop handoff (2026-09-20, late)

Paste this to restart: `/loop` followed by the "Restart prompt" at the bottom.

## What the loop is

One main session directs, workers do the building. Each round:

1. Check usage (`~/.claude/scripts/usage.sh`) and taper:
   under 60% full speed, 60 to 75% no new Sonnet workers, 75 to 90% only land PRs, about 90% checkpoint and stop.
2. Fold in what landed, keep every open PR current with main, keep all three required GitHub checks green (`check`, `check-refs`, `network`).
3. Launch the next workers: 2 to 3 at once, 1 to 2 if Opus tier. Haiku for grep-and-fix and assets, Sonnet for a scoped feature, Opus or Fable only for architecture.
4. Review every worker's output yourself (look at the screenshot, read the test) before trusting it. Twice today a Haiku shipped something wrong: a save test that rewrote the file it was meant to read back, and an icon with no crowns.
5. Short progress ping to Joshua, in his voice.

Rules: hand-made worktrees under `/tmp/jt-loop/<branch>` (the session cwd is not a git repo, so `isolation: worktree` fails). Headless only, never a visible QEMU window. Workers never edit `docs/roadmap.md` and never sit polling CI. No em dashes. Joshua approved merging PRs on 2026-09-20 ("merge the pull requests, please handle it"); before that the permission guard blocked `gh pr merge --auto`.

## Where things stand (2026-09-20, late)

Merged: roadmap dump (#70), repo tidy with the demo kernel untracked and a pre-push hook (#72), black and white landing (#73), boot logo seams (#74), demo plays its tour before the page scrolls and the hero is edge to edge (#77, checked on the live site).

Open, auto-merge armed, none failing: #78 logo and pointer drawn at full resolution, #76 USB-bootable ISO plus drawing on the bootloader's framebuffer (desktop proven on `-vga vmware` and `-vga virtio`), #75 menu bar app out and this roadmap, #64 run by name, #65 Chat default, #62 Activity Monitor (fixed the icon table that made Apps draw a trash can).

Main requires branches to be current, so they land one at a time: each round run `gh pr update-branch <n>` on the next one that is BEHIND. A kernel PR that conflicts after a squash merge: merge `origin/main`, keep the superset side, rebuild, `./check.sh`, push.

The menu bar app lives in `~/Documents/Code/joshuatree-monitor` (private repo `nulljosh/joshuatree-monitor`); the Dock pin and the login launcher point there.

Verdict given to Joshua: this is not 1.0 yet. The plan is 0.9.0 beta, then 1.0.0. Both gates are in `docs/roadmap.md`.

Rules that changed today: one subagent at a time, Haiku, sequential, two only when asked. `wrangler deploy` by hand is blocked, deploys happen on merge. PR bodies are 3 to 5 punchy lines. Status pings are one or two lines, and any /yo or /hoe goes last in the reply.

Tools: `python3 /tmp/jt-dump.py 0.3` from a worktree with `kernel.elf` dumps the splash and the desktop to `/tmp/jt-q-*.png` (recreate it from `tools/checks/dockhover-check.py`'s QMP pattern if `/tmp` was wiped). Look at 3x nearest-neighbour crops before trusting any visual fix.

## Next, in order

1. Land the queue.
2. Dock polish PR (first Beta item in the roadmap). Branch from main after #62 and #78 are in. Dock changes also need the geometry constants in `tools/checks` and the tour's click targets in `landing/v86/embed.js`.
3. Work down the Beta list, then the 1.0.0 list. Lag (issue #14) and a real PC boot are the two that make it defensible.
4. Reconcile GitHub issues with the rewritten roadmap.

## Naming

JoshuaTree.com is taken. Wanted: simple, two syllables at most, rolls off the tongue, Bonsai or Koi flavoured, a bare .com, never a `-os.com` suffix. Bonsai collides with PrismML's Bonsai LLM, Koi with Koi Computers. Rejected: Yucca, Agave, Rowan, Niwakoi. Okay: Matsu, Tupelo. Free-looking and unverified: Pinekoi, Barkoi, Plumkoi, Koipine, Figpine, also `joshuatreecomputer.com`. Tool: `~/.claude/skills/asc-name-creator/domains.sh`. Rename nothing until he picks.

## Restart prompt

Drive joshuatree to a defensible 1.0.0 following docs/LOOP-HANDOFF.md: read it first, check usage and taper, land the open PRs one at a time, then work the Beta list in docs/roadmap.md top down. One Haiku worker at a time at most, review output yourself from zoomed headless frames, all GitHub checks green, every fix a PR at once, one-line pings in Joshua's voice. Stop at about 90% usage, on cancel, or at 1.0.0.

## Portfolio dock loop (paused 2026-09-21)

A separate loop from the main Joshua Tree roadmap loop. Fills the dock with real native apps instead of one-line HTML cards.

Done: Portfolio (v0.25), Epiphany (finance dashboard), Keyrate (typing test), Toroid (Game of Life).

Next in order: Curbfind, Bookrank, Lexly, Sparkjar, Quotes, then rest of Apps folder.

Pattern: one native app per iteration as a small kernel header (copy `kernel/toroid.h` loop shape). Add ARCHITECTURE row, bump VERSION and landing/version.txt in the same PR. Test headless by clicking the dock tile against static landing with ?full&portfolio. QA pass every few apps.

Restart prompt:

```
/loop until our main apps are filled in the docks, then remainder apps in the launchpad
```
