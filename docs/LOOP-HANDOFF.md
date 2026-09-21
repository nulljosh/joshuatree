# Joshua Tree loop handoff (2026-09-20, evening)

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

## Where things stand

Merged today: new spiky-crown icon (#60), Weather window (#53), save round-trip check (#61), landing headline no longer vanishes on click (#63), roadmap prune (#59). #51 closed as superseded. Releases `jt-v0.85.2` and `jt-v0.85.3` published.

Open, auto-merge armed:

| PR | What | Worktree |
|---|---|---|
| #67 | Colour wallpaper + seamless boot logo (this missed #52's merge, main still ships B&W) | `wallpaper-colour` |
| #62 | Activity Monitor app | `activity-monitor-app` |
| #64 | Shell runs programs by bare name (1.0 gap one) | `shell-launch-by-name` |
| #65 | Chat defaults to qwen3:8b | `chat-qwen-default` |
| #66 | Repo and docs cleanup, human-readable roadmap | `repo-docs-cleanup` |
| #68 | Landing page colour (clrs.cc palette) | `landing-colour` |

A Bonsai worker (`chat-bonsai`, stacked on #65) was still running at close: Chat moves to the OpenAI-style API for every backend, Bonsai 2 on `10.0.2.2:8080` as default with fallback to Qwen. Check whether it opened a PR; review before arming.

The conflict tax: every kernel PR commits `landing/v86/kernel.elf`, so each merge leaves the others conflicted. Land them one at a time with `tools/resync-pr.sh <worktree-dir>` (merges the remote branch and main, rebuilds the demo kernel, runs `check.sh`, pushes; stops and lists files if anything other than the binary conflicts, most likely the shell help line in `kernel/kernel.c`). If `/tmp/jt-loop` is gone after a reboot, recreate a worktree with `git worktree add /tmp/jt-loop/<name> origin/<branch>`.

## Next, in order

1. Land the six PRs, then the Bonsai PR.
2. One PR that stops tracking `landing/v86/kernel.elf` in git and builds it in the deploy workflow. Ends the conflict tax.
3. Apply the landing-colour worker's Theme rule wording to `CLAUDE.md` (it is in PR #68's description or the worker report).
4. Release 0.86.0: full regression suite first, real notes, `jt-v0.86.0` tag plus `gh release create`, landing changelog updated (colour wallpaper, Activity app, launch by name, Qwen and Bonsai chat).
5. 1.0 gap two: ring-3 programs in `user/` (cat, wc, grep, calc), stacked on `shell-launch-by-name`. Sonnet.
6. Reconcile GitHub issues with the rewritten roadmap (titles changed; close stale, re-sync, no duplicates).
7. Roadmap items to add: multi-provider Chat (ChatGPT, Claude, Gemini, DeepSeek, MiniMax, Qwen, Kimi, GLM, Grok, Mistral; needs keys and TLS), night tint colour check, Apps-folder window title bug.
8. 1.0 also needs Joshua's name and codename call.

Blocked on Joshua: deleting merged remote branches is permission-blocked. Run
`git -C ~/Documents/Code/joshuatree push origin --delete chat-llm-console engraving-os roadmap-prune-0920 stocks-epiphany`.

## Naming

JoshuaTree.com is taken. Wanted: simple, two syllables at most, rolls off the tongue, Bonsai or Koi flavoured, a bare .com, never a `-os.com` suffix. Bonsai collides with PrismML's Bonsai LLM, Koi with Koi Computers. Rejected: Yucca, Agave, Rowan, Niwakoi. Okay: Matsu, Tupelo. Free-looking and unverified: Pinekoi, Barkoi, Plumkoi, Koipine, Figpine, also `joshuatreecomputer.com`. Tool: `~/.claude/skills/asc-name-creator/domains.sh`. Rename nothing until he picks.

## Restart prompt

Drive joshuatree to 1.0.0 following docs/LOOP-HANDOFF.md: read it first, check usage and taper, land the open PRs one at a time with tools/resync-pr.sh, review the Bonsai worker's PR, then work the "Next, in order" list. Max 2 to 3 workers, review their output yourself, headless only, all GitHub checks green, short pings in Joshua's voice. Stop at about 90% usage, on cancel, or at 1.0.
