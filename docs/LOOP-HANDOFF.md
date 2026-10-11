# Joshua Tree handoff (2026-10-11)

## Current task

The 2.40.0 combined release (#502) is merged and every branch it carried is deleted. The loop's goal, set by Joshua on 2026-10-11: all PRs safely merged, zero GitHub issues, no stale branches, and a clean repo and folder layout with no loose files or folders. Joshua authorised merging PRs that pass their gates for this loop.

Open now: the boot animation on `feat/boot-draw-animation` (2.41.0, not yet pushed: the splash draws the tree on over 0.8 s, `tools/checks/bootanim-check.py` proves it). Its full `tools/ci-local.sh` run was still going. When it passes, commit, open a draft PR, mark it ready, and merge once GitHub is green. PR #503 (`feat/pi-screen-sleep`) belongs to another session; leave it unless Joshua says otherwise.

## Loop checklist

1. Push the boot animation, open the PR as a draft, run `tools/ci-local.sh`, then `gh pr ready` and merge on green.
2. Fast-forward local `main` to `origin/main`.
3. Branches: keep `main`, `feat/pi-screen-sleep` (open PR) and `fix/ci-preflight-affinity` (its worktree in `~/Documents/Codex/.../pi-security` has one unsaved change; ask before touching it). Delete any other branch once its PR is merged on GitHub.
4. Issues: zero open now. Keep it that way; fix a real bug instead of mirroring it into an issue.
5. Layout: audit the repo root for loose files and the folder above it for stray `jt-*` checkouts. Move or delete only what is untracked or ignored build output. Run `bash tools/checks/check-refs.sh` after any move.

## Usage

Check `~/.claude/scripts/usage.sh` between tasks. At 5 to 10 percent remaining, run `/checkpoint` and stop. Last snapshot: session 3 percent, weekly 1 percent used.

## Physical and release limits

Only 2.34.0 was flashed, hash-verified and ejected. Joshua confirmed boot and Spotlight opening Terminal. Clock/icon, power controls, Brick, notices, mouse, RNG and Samantha still need the board pass. ISO is for BIOS PCs, not the Pi card; UEFI and real PC boot remain unverified.

Clock bootstrap still uses build time. Conversations reset on reboot. Pi SD saving, local weights from card and fallback updates remain open. Card backups and dev images contain credentials; never commit or copy them to outputs.

The Mac TLS relay must be checked using docs/RELAY-TLS.md before a board Samantha test. Credentials stay in their existing private files.
