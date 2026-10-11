# Joshua Tree handoff (2026-10-10)

## Released

2.40.0 merged in PR #502 as 264c01d9. Full local gate: 223 checks plus four demos, all passed. Final GitHub run 38116180960 passed; release 38117167441 and deploy 38117167431 passed. Live Chromium/WebKit phone/desktop light/dark QA, signup, browser boot and actual wc counts passed. Stable ISO download matches SHA-256 4cabd69fdb9c55b2c28dcbcb9da244b328cf129a8ca47183df1aee158ee6f137. All superseded PRs closed; only main remained on the remote. No open GitHub issues.

## Current task

Joshua requested one more roadmap feature before flashing tomorrow. Branch feat/pi-screen-sleep, 2.41.0: HDMI blank after five idle minutes, keyboard/mouse wake, active Brick and assistant requests stay awake. USB, apps and networking continue. Focused host timing, wrap, mailbox refusal, broken-wake control and raspi4b mailbox/input checks pass. QEMU acknowledges blanking but does not hide pixels; physical HDMI sleep/wake still needs the board. Run the required full local gate before readying the draft PR; review final GitHub checks before merge. Prior authorization covers safely merging and releasing finished work.

## Limits and next board pass

The card has not been flashed since 2.34.0. Tomorrow check the new Calendar date, Clock, Samantha, Brick, notices, power controls and screen sleep, plus wired mouse and Wi-Fi/HTTPS. The ISO is for BIOS PCs, not the Pi card. Physical PC/UEFI boot remains unverified.

Clock bootstrap still uses build time. Conversations reset on reboot. SD saving, local weights from card and fallback updates remain open. Card backups and dev images contain credentials; never publish them. Verify the private TLS relay using docs/RELAY-TLS.md before a board Samantha test.

Codex relay backend does not exist; automatic review previously rejected repository egress without specific approval. No later answer approved that access. Preserve unrelated local worktrees and untracked plans.

Check usage between tasks and stop at 10% five-hour remaining. No paid model calls, reset credits or hardware flashing. Last snapshot: 41% remaining, before screen-sleep work.
