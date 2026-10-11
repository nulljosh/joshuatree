# Joshua Tree handoff (2026-10-10)

## Current task

The combined release is PR #502, feat/shell-wc, version 2.40.0. It contains every remote work branch: #478 and #491–501, plus the fresh-boot wc utility and supported integer formatting. The strict ARM dock-art check now derives Apps and Trash slots from APPS[] rather than stale numbers.

Joshua explicitly authorized safe merge, release, deployment, X announcement and avatar update. Merge only after the full gates pass. Close superseded PRs and delete their remote branches only after their contents are confirmed in main. Preserve unrelated local worktrees and untracked work.

## Validation

The repaired ARM desktop check passes on virt, Pi 1080p and Pi 4K. The wc shell regression proves fresh-boot availability, a missing-file error, exact 1-line/2-word/12-byte counts, and existing hello/note/unknown-command behavior. Unsupported %ld output failed before the fix. Build, boot, references and pre-push checks pass. A fresh desktop screenshot was inspected.

Full local gate: work/ci-final-2.40.0.log. GitHub run: 38114771523. Both verify code head 02d39cd; this subsequent update changes only markdown. Their final results belong in PR #502, not a claim made before they finish. The older local and GitHub CI runs were deliberately cancelled when this combined candidate superseded them.

## Goal and usage

Finish the CI repair and one additional small roadmap task, then safely merge and release the finished batch, remove superseded branches and leave GitHub green with zero open issues. Check usage between tasks and stop at 10% five-hour remaining. Last snapshot: 86% remaining. No paid model calls, reset credits or hardware flashing.

## PRs

#478 is the older Mines app. #491 is RNG/Terminal/Calendar (2.34.0, physically booted). #492 is readable Calendar/Clock/Samantha launch (2.35.0). #493 is CI preflight/affinity (2.35.1). #494 is power controls (2.36.0). #495 is Brick (2.37.0). #496 is app text and captures (2.37.1, head 14bca641). #497 is notifications (2.38.0, head 5df7be78). #498 is ISO links/packaging (2.38.1, head d75ab5c). The candidate contains all their code; do not close them until an approved candidate merge actually supersedes them.

## Physical and release limits

Only 2.34.0 was flashed, hash-verified and ejected. Joshua confirmed boot and Spotlight opening Terminal. Clock/icon, power controls, Brick, notices, mouse, RNG and Samantha still need the board pass. ISO is for BIOS PCs, not the Pi card; UEFI and real PC boot remain unverified. Latest published 2.33.5 has a versioned ISO only; the stable alias becomes available when the new release workflow runs.

Clock bootstrap still uses build time. Conversations reset on reboot. Pi SD saving, local weights from card and fallback updates remain open. Card backups and dev images contain credentials; never commit or copy them to outputs.

The Mac TLS relay must be checked using docs/RELAY-TLS.md before a board Samantha test. Credentials stay in their existing private files. Codex relay implementation is still blocked on specific repository-egress approval after automatic review rejected it; no backend exists. No user answer granted that approval.
