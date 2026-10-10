# Joshua Tree handoff (2026-10-10)

## Current task

Branch fix/iso-download-links, version 2.38.1, builds on notifications #497. The release workflow uploads stable joshuatree.iso plus the versioned image, with both in SHA256SUMS. README and site now have direct ISO/checksum links. These become live after the workflow runs following an approved merge; latest published release 2.33.5 only has its versioned image. The ISO is for BIOS PCs, not the Pi card.

## Checks and review

ISO CD-ROM, raw-USB and non-Bochs framebuffer boot checks pass. Executing the actual workflow packaging block produces identical stable/versioned images and correct checksums for both. Pre-push passes. Notifications #497 passed sanitized bounds/replacement/wrap tests, QEMU real pixels/idle expiry/input/status preservation, Brick regression, normal Pi build and i386 build/boot. No full gate has run for the latest feature/release stack yet.

The user authorized a roadmap loop until five-hour usage reaches 90%, with A+ QA before main, CI/CD, release versions/tags, zero open issues/PRs/stale branches, fresh markdown and ISO links. At last check: zero open issues; nine remote branches, all main or attached to an open PR, so none are stale. Existing release and deploy runs on main are green. Draft PRs have not run GitHub CI and must not be described as green.

Merge/deploy still needs explicit PR approval. The release workflow has configured X keys and automatically posts release announcements; obtain authorization for that side effect or remove it from the intended release path before publishing. No paid calls, resets or flashes. Do not close real work merely to make counts zero.

## Parent branches

PR #491: RNG, Terminal and Calendar fixes, version 2.34.0. The development card booted on the physical Pi; hardware RNG and a real Samantha answer still need verification.

PR #492: readable Calendar icon, Clock and Samantha dock launch, version 2.35.0. Focused QEMU checks pass. Clock is read-only, Calendar remains an icon.

PR #493: fixed-path preflight and reserved ARM shard affinity, version 2.35.1. Its exact head 5d2e99f passed the full local gate: 219 suite checks and four demos, 2,391 seconds, zero failures. This validates the Clock code in that stack, not the later power/game additions.

PR #494: confirmed keyboard shutdown/restart, version 2.36.0. Host safety and actual HID-to-watchdog raspi4b shutdown/reboot pass. Physical verification and release-wide validation remain.

At the last GitHub read, #492, #493 and #494 were open drafts. Merge or deploy only after Joshua explicitly approves the PR. Never bypass a failing gate or raise the two-job local cap.

PR #495: native Brick; focused physics/input/pixel checks pass, full gate and board play remain.

PR #496: app text contrast and refreshed landing captures, version 2.37.1. Build, boot, sharpness, Terminal regressions and pre-push pass. Head 14bca641. Based on #495, independent of this notification branch.

## Physical state and limits

The 2.34.0 development card was flashed, hash-verified and safely ejected. Joshua confirmed boot and Spotlight opening Terminal. Later Clock, power and Brick changes have not been flashed. The next board checks are the new icon/Clock, power controls, Brick, mouse, RNG and Samantha.

Clock bootstrap still uses build time. Conversations reset on reboot. Pi SD saving, local model weights from card and fallback updates remain unimplemented. Development card images and backups contain credentials; keep them private.

The Mac TLS relay must be checked/restarted before a board Samantha test using the reviewed main script and existing credentials, as described in docs/RELAY-TLS.md. Do not rotate credentials silently. No paid model request was made for these tasks.

Codex relay implementation remains pending specific repository-access approval after automatic review rejected its potential repository egress. No backend exists. The empty feat/pi-codex-relay branch remains; work/pi-codex is now used by feat/pi-brick.
