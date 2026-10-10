# Joshua Tree handoff (2026-10-10)

## Current task

The current worktree work/landing-footer is now on fix/ci-pi-runner (2.39.2), stacked on footer PR #500. Candidate GitHub run 38093213365 failed only the Pi power check because Ubuntu 24.04's QEMU has no raspi4b machine. Seven suite shards, demo and network passed. The ARM-reserved shard now uses Ubuntu 26.04 and fails early unless QEMU lists raspi4b; other shards stay on 24.04. Runner/cache versions are separated. Linux CI validation of this repair is still pending. The footer suite also now installs WebKit, matching its new browser check.

The footer branch fix/landing-footer (PR #500) adds a wide original desert illustration using the existing vector tree mark, a larger closing statement and accessible 44px directory links. It is based on release/2.39.0 but does not change that candidate or its running full gate. Footer version is 2.39.1; full gate and release approval remain required before it can reach main.

Release candidate branch release/2.39.0 in work/pi-notifications combines #478 and #491–498. It includes Mines, Pi RNG/Terminal/Calendar repairs, Clock, keyboard power controls, Brick, menu notices, app text contrast, fresh landing captures, the CI preflight/ARM affinity repair and direct ISO downloads. No merge or deployment has happened.

Review reproduced Mines closing when its help panel was clicked. Off-board content clicks now do nothing; the new regression failed before the fix and passes with it. Escape closes, a scripted game wins and Mail opens afterwards. The unused old app-title painter was removed after verifying it had no callers, clearing the last normal-build warning.

## Validation

Focused checks passed for the individual drafts: sanitizer and real QEMU input/pixel checks for games/power/notices, text sharpness and Terminal spacing, normal Pi image build, i386 build/boot and pre-push. Notes captures now open a new editor before typing; all 14 app tiles were retaken and inspected. ISO CD-ROM, raw-USB and non-Bochs framebuffer boot pass; the actual release packaging block creates identical stable/versioned ISOs and verified checksums.

The combined 2.39.0 full local gate and GitHub validation have not completed yet. Keep this candidate draft until they pass. #493 alone passed 219 checks plus four demos on exact head 5d2e99f; that does not validate the later additions. Latest main release/deploy runs were green. Draft/skipped checks are not green checks.

## Goal and approval

Continue implementable roadmap tasks until five-hour usage has 15% or less remaining, then stop. The latest snapshot was 17% remaining; stop at 15%. The user added a reference-inspired landing footer; it is isolated from the running release candidate. Keep markdown fresh. Target working CI/CD, version/tag/release updates, direct ISO links, zero unresolved PRs/issues/stale branches, and A+ QA before main. Never close real work just to make counts zero.

Main has zero open issues. Every remote branch was main or belonged to an open PR. The unused local feat/pi-codex-relay branch was deleted after verifying it was an ancestor; no work was lost. Active worktrees and their local branches remain untouched.

Explicit PR approval is still required to merge or deploy. The release workflow has configured X keys and posts release announcements, so publishing also needs authorization for that side effect or an approved change to the release path. No paid model requests, reset credits or hardware flashes are authorized by this loop.

## PRs

#478 is the older Mines app. #491 is RNG/Terminal/Calendar (2.34.0, physically booted). #492 is readable Calendar/Clock/Samantha launch (2.35.0). #493 is CI preflight/affinity (2.35.1). #494 is power controls (2.36.0). #495 is Brick (2.37.0). #496 is app text and captures (2.37.1, head 14bca641). #497 is notifications (2.38.0, head 5df7be78). #498 is ISO links/packaging (2.38.1, head d75ab5c). The candidate contains all their code; do not close them until an approved candidate merge actually supersedes them.

## Physical and release limits

Only 2.34.0 was flashed, hash-verified and ejected. Joshua confirmed boot and Spotlight opening Terminal. Clock/icon, power controls, Brick, notices, mouse, RNG and Samantha still need the board pass. ISO is for BIOS PCs, not the Pi card; UEFI and real PC boot remain unverified. Latest published 2.33.5 has a versioned ISO only; the stable alias becomes available when the new release workflow runs.

Clock bootstrap still uses build time. Conversations reset on reboot. Pi SD saving, local weights from card and fallback updates remain open. Card backups and dev images contain credentials; never commit or copy them to outputs.

The Mac TLS relay must be checked using docs/RELAY-TLS.md before a board Samantha test. Credentials stay in their existing private files. Codex relay implementation is still blocked on specific repository-egress approval after automatic review rejected it; no backend exists. No user answer granted that approval.
