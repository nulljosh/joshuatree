# Joshua Tree handoff (2026-10-10)

## Current task

Branch feat/pi-brick, version 2.37.0, builds on power controls #494. Brick is an original native ARM brick-breaker. Open it from Spotlight. Arrows/A-D or mouse move the paddle; Space/click launches and pauses, R restarts, Escape/red dot closes. It has 32 bricks, three lives, win and game-over states. Scores are not saved and there is no sound.

The fixed-size integer rules have no allocator. A 30 Hz hardware-counter tick animates independently of input. Spotlight suspends painting; switching apps closes the game. Outside the game, the virtio desktop keeps its interrupt-driven wait.

## Checks and review

Brick's host physics harness passes under ASan/UBSan, including collisions, bounds, pause, win, lost lives, restart and 10,000 steps. The real QEMU keyboard/mouse/pixel test passes: held arrows, mouse movement/click, timer-driven ball motion, pause, restart, Spotlight overlay, app switching, red close and Escape. The existing M2 disk/network/input check and Pi shutdown/restart checks pass. The normal Pi build, pre-push, references, registration and generated docs pass. The framebuffer screenshot was inspected.

No full suite was started for Brick. It stays draft until release-wide validation and Joshua's PR approval. No merge, deployment or card flash was done. Physical Pi play remains.

## Parent branches

PR #491: RNG, Terminal and Calendar fixes, version 2.34.0. The development card booted on the physical Pi; hardware RNG and a real Samantha answer still need verification.

PR #492: readable Calendar icon, Clock and Samantha dock launch, version 2.35.0. Focused QEMU checks pass. Clock is read-only, Calendar remains an icon.

PR #493: fixed-path preflight and reserved ARM shard affinity, version 2.35.1. Its exact head 5d2e99f passed the full local gate: 219 suite checks and four demos, 2,391 seconds, zero failures. This validates the Clock code in that stack, not the later power/game additions.

PR #494: confirmed keyboard shutdown/restart, version 2.36.0. Host safety and actual HID-to-watchdog raspi4b shutdown/reboot pass. Physical verification and release-wide validation remain.

At the last GitHub read, #492, #493 and #494 were open drafts. Merge or deploy only after Joshua explicitly approves the PR. Never bypass a failing gate or raise the two-job local cap.

## Physical state and limits

The 2.34.0 development card was flashed, hash-verified and safely ejected. Joshua confirmed boot and Spotlight opening Terminal. Later Clock, power and Brick changes have not been flashed. The next board checks are the new icon/Clock, power controls, Brick, mouse, RNG and Samantha.

Clock bootstrap still uses build time. Conversations reset on reboot. Pi SD saving, local model weights from card and fallback updates remain unimplemented. Development card images and backups contain credentials; keep them private.

The Mac TLS relay must be checked/restarted before a board Samantha test using the reviewed main script and existing credentials, as described in docs/RELAY-TLS.md. Do not rotate credentials silently. No paid model request was made for these tasks.

Codex relay implementation remains pending specific repository-access approval after automatic review rejected its potential repository egress. No backend exists. The empty feat/pi-codex-relay branch remains; work/pi-codex is now used by feat/pi-brick.
