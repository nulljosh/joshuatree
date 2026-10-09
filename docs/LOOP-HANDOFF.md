# Joshua Tree loop handoff (2026-10-09, afternoon)

## What the loop is

Finish Pi relay TLS and HTTPS clock validation in PR #490, then merge on green. Joshua explicitly approved merging these changes when safe. Keep usage low: no new features, agents or background automation.

## Where things stand

Main is 2.33.4 at 755e3f73. PR #488 shipped landing signup/mobile fixes and the redesigned welcome email, with actual delivery confirmed in Apple Mail. PR #489 shipped relay shared-file race protection, CI balancing and the Wi-Fi guide; full local checks and GitHub CI passed before merge.

Draft PR #490 is on fix/pi-relay-tls-clock, rebased onto main. It requires TLS for Pi relay traffic without an HTTP fallback, requires certificate/key configuration for a LAN relay, and accepts clock dates only over verified HTTPS with strict parsing and rollback protection. Focused host and QEMU checks pass, including wrong-host, expired, future and untrusted certificates, session resume and proof that a plaintext endpoint receives no bearer.

The first full local run exposed ARM checks cleaning the same build directory from different shards. ARM build checks now share shard 6. Final local validation is running in /tmp/jt-pi-security-final-ci.log, with shard logs /tmp/jt-ci-local-92797-shard*.log. The head was rebased without changing its source tree. Mark ready only after a full local pass, then require actual GitHub CI, not draft skips. This checkpoint only updates documentation.

No new card was flashed and no live relay restarted. TLS migration needs the relay certificate, matching host name or IP and development card updated together; read docs/RELAY-TLS.md. Initial certificate validation uses build time, so a certificate expired since the build can pass the first clock handshake. Persistent or signed fresh time and hardware entropy remain open. Physical Pi verification remains required.

Minesweeper #478 is draft with conflicts and outside the current security scope. There are zero open issues and no orphaned remote branches. Benchmarks are deferred while CI or QEMU runs. Checkpoint issue sync cannot locate this repo's docs/roadmap.md because the current script only accepts a root roadmap.

## Next, in order

1. Read the final local log and all shard results. Investigate any failure; do not bypass checks or run another suite in parallel.
2. Update #490 validation, mark ready on local green and wait for actual GitHub CI, including ARM tests. Merge safely using the verified head once green; Joshua has approved this.
3. Verify the normal release and landing deployment. Coordinate the relay certificate and card setup before a flash; verify keyboard, Wi-Fi, clock, relay and USB mouse on the board.
4. Run clean benchmarks only after all CI and QEMU work is quiet. Keep clock bootstrap and hardware randomness gaps explicit.

## Restart prompt

```text
Continue Joshua Tree PR #490 on fix/pi-relay-tls-clock. Read docs/LOOP-HANDOFF.md and /tmp/jt-pi-security-final-ci.log first. Complete the full local gate, then actual GitHub checks, then merge safely as Joshua approved. No bypasses, new features, subagents or background automation. No card flash or live relay restart without coordinated certificate/card setup. Keep replies and usage lean. Defer benchmarks until the Mac is quiet.
```
