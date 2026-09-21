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
