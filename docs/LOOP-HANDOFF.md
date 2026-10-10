# Joshua Tree handoff (2026-10-10)

## Current task

Branch fix/app-text-contrast, version 2.37.1, builds on Brick #495. Ring-3 app text now uses the exact same ink curve as the kernel, including Terminal's mono face. The existing curve moved to lib/text_ink.h; glyph positions, sizes and widths are unchanged.

## Checks and review

Before/after real QEMU pixels show Mail and Notes full-ink coverage rising from 23–33% to 44–52%, with six intermediate edge levels. The stricter sharpness check requires 40% and rejects the old captures. Bold headings retain fewer mid-coverage pixels as stems darken; their separate floor preserves the six-level antialiasing requirement. Kernel build and boot pass. Terminal cell spacing and antialiasing/pitch regressions pass. All 14 landing tiles were refreshed and the pre-push gate passes.

No full suite was started for this fix. It stays draft until release-wide validation and Joshua's PR approval. No merge, deployment or card flash was done.

The user explicitly authorized continuing through implementable roadmap tasks until the five-hour quota has 10% remaining. Check usage between tasks; never use reset credits without explicit confirmation. Skip paid API calls, blocked Codex repository access and physical-only tasks. Do not spawn agents unless Joshua asks.

## Parent branches

PR #491: RNG, Terminal and Calendar fixes, version 2.34.0. The development card booted on the physical Pi; hardware RNG and a real Samantha answer still need verification.

PR #492: readable Calendar icon, Clock and Samantha dock launch, version 2.35.0. Focused QEMU checks pass. Clock is read-only, Calendar remains an icon.

PR #493: fixed-path preflight and reserved ARM shard affinity, version 2.35.1. Its exact head 5d2e99f passed the full local gate: 219 suite checks and four demos, 2,391 seconds, zero failures. This validates the Clock code in that stack, not the later power/game additions.

PR #494: confirmed keyboard shutdown/restart, version 2.36.0. Host safety and actual HID-to-watchdog raspi4b shutdown/reboot pass. Physical verification and release-wide validation remain.

At the last GitHub read, #492, #493 and #494 were open drafts. Merge or deploy only after Joshua explicitly approves the PR. Never bypass a failing gate or raise the two-job local cap.

PR #495: native Brick, version 2.37.0. Sanitized physics, real QEMU keyboard/mouse/timer/pixel checks, M2 input and Pi power regressions pass. Silent, scores not persisted; full gate and physical play remain.

## Physical state and limits

The 2.34.0 development card was flashed, hash-verified and safely ejected. Joshua confirmed boot and Spotlight opening Terminal. Later Clock, power and Brick changes have not been flashed. The next board checks are the new icon/Clock, power controls, Brick, mouse, RNG and Samantha.

Clock bootstrap still uses build time. Conversations reset on reboot. Pi SD saving, local model weights from card and fallback updates remain unimplemented. Development card images and backups contain credentials; keep them private.

The Mac TLS relay must be checked/restarted before a board Samantha test using the reviewed main script and existing credentials, as described in docs/RELAY-TLS.md. Do not rotate credentials silently. No paid model request was made for these tasks.

Codex relay implementation remains pending specific repository-access approval after automatic review rejected its potential repository egress. No backend exists. The empty feat/pi-codex-relay branch remains; work/pi-codex is now used by feat/pi-brick.
