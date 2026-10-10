# Joshua Tree handoff (2026-10-10)

## Current task

Branch feat/pi-notifications, version 2.38.0, based on Brick #495. The Pi menu bar shows bounded, four-second messages for Wi-Fi joining and Brick win/game over. A newer message replaces the previous one. The clock and Wi-Fi retain their space; input stays with the app. The hardware counter drives expiry, including the virt desktop while no keys arrive.

## Checks and review

ASan/UBSan checks cover truncation, nonprintable bytes, replacement, empty messages and timer wrap. QEMU checks real visible/cleared pixels, idle expiry, Spotlight input and unchanged clock/Wi-Fi pixels. The first pixel test caught an oversized font; fixed by using the normal menu font and clipping to the free gap. Build and regression results are in the PR. Full release-wide validation and physical Pi checks remain; no merge, deployment or flash.

The user authorized a roadmap loop until five-hour usage reaches 90%, and added CI/CD, release versions/tags, zero open issues/PRs/stale branches and ISO download links. GitHub currently has zero open issues. Do not close real work just to make the counts zero. Merge/deploy still requires explicit PR approval. No paid calls, resets or flashes; skip blocked work.

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
