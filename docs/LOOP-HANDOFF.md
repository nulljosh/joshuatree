# Joshua Tree handoff (2026-10-09, evening)

## Current scope

Joshua asked for hardware randomness, independent Terminal AI settings and readable Calendar Year dates while he makes dinner. He will flash the SD card himself. No card writes. The Mac relay was migrated to TLS using the already merged relay implementation.

Main is 2.33.5 at ab90b912. PR #490 passed all GitHub checks and merged. The new branch is fix/pi-rng-terminal-calendar, version 2.34.0. It replaces Pi timer entropy with the RNG200 hardware FIFO, refuses TLS on health faults or a one-second timeout, and wipes seed buffers. Virt keeps its existing timer provider for QEMU tests only. Two Terminal sessions switch with F3, keep separate typing, scrollback, conversation, model, effort and status, and cannot switch during a running request. Calendar Year view halves the existing antialiased face instead of drawing dots at the normal size.

## Validation

Focused checks passed: sanitized RNG register harness; real QEMU TLS with forced entropy failure, trusted and rejected certificates; Terminal session isolation and busy-switch refusal; all four Calendar views with distinct dates in twelve months. Calendar screenshot inspected at /tmp/jt-calviews-year.png. All 218 suite commands passed without retries, logged in /tmp/jt-evening-ci.log. All four demo checks also passed; the full local run finished in 2337 seconds. Mark PR #491 ready and verify actual GitHub checks. Joshua must explicitly approve the PR before merge. Do not bypass failures.

## Physical limits and flash setup

Hardware RNG and the new Terminal sessions need a real Pi test. Clock bootstrap still uses build time and has the documented stale-certificate window. Browser navigation is shared across Terminal sessions, and neither persists through reboot. Side-by-side layout remains open. Calendar is the i386 ring-3 app, not a new Pi Calendar app.

The card was backed up privately under the chat's work/card-before-tls directory. It has not been flashed. TLS certificate files exist under ~/.config/joshuatree/relay-tls; private keys stay on the Mac. The Mac relay on port 8765 now serves TLS. Strict CA verification, GET rejection and wrong-token refusal passed without a paid Claude call. Follow docs/RELAY-TLS.md. The Mac address was 10.0.0.116; recheck before using its matching certificate. Flash from the reviewed branch with CLAUDE_RELAY_HOST set to that address and TLS_TA pointing to ca.pem, never from the stale primary checkout. Joshua handles the card.
