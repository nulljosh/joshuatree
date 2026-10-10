# Joshua Tree handoff (2026-10-10)

## Current work

Branch fix/ci-preflight-affinity (2.35.1) builds on feat/pi-clock-calendar. Two CI fixes are implemented: fixed-path validation runs before builds in local CI and pre-push, and the balancer preserves reserved shard 6 and includes its load before packing other checks. The host regression fixture passes, including argument variants, dry-run byte preservation, too-few-shards refusal and preflight rejection before builds. No kernel or relay code changed. Full release validation remains pending.

Branch feat/pi-clock-calendar, version 2.35.0, builds on PR #491 (fix/pi-rng-terminal-calendar, 2.34.0). Joshua asked for a readable Pi Calendar icon, a Clock app and Samantha QA in QEMU. Calendar now shows the month and large day number, with a zero-based month encoding that also handles December. Clock opens from Spotlight or Samantha's open action, displays Vancouver time and date, refreshes each minute and waits for network time when unset. Escape and the close button restore the previous pane. Samantha's dock tile now opens the existing assistant Terminal instead of the placeholder Console. Vancouver remains UTC-7 after March 8, 2026, per the B.C. time change.

## Validation

The prior session inspected a real QEMU screenshot and passed the focused Calendar/Clock/launch check after correcting Vancouver's winter offset. Temporary logs disappeared overnight. PR #492 is saved as draft. The first full process ended without a summary after 68 checks. The completed rerun (work/clock-ci-retry.log) passed 217 of 218 suite checks and all four demos; window snapping needed its one retry. The only failure was a fixed screenshot export path in arm64-calicon-check.py. That unnecessary export is removed and tmp-paths-check.py now passes. Calendar/Clock/Samantha checks passed after the removal. Ten additional host checks covered Vancouver transitions, New Year and leap day. The fresh two-job run in work/clock-ci-final.log stopped without a summary after at least 71 passed checks and zero observed failures. Its PID is no longer running; it is not a green gate. A full run must finish green before either PR is marked ready. No app source changed for this test correction. The suite includes the Calendar/Clock check, Samantha TLS/session/token checks and the bounded agent loop against fake responses. No paid Claude request is needed. Do not bypass a failing check. PR #491 was fully green and still open at resume; Joshua must explicitly approve PRs before merge.

## Physical state

The 2.34.0 development card was flashed, hash-verified and safely ejected at Joshua's request. Joshua confirmed it boots and Spotlight opens Terminal. He noticed the tiny calendar grid and has not yet tested Samantha. The new 2.35.0 changes have not been flashed. Physical Clock, icon and Samantha verification remain; no claim of API expiry is confirmed.

The previous Mac TLS relay stopped overnight. It was restored as a foreground process from approved main commit ab90b912, with the existing credentials. Strict TLS, GET rejection and wrong-token refusal passed without a model call. Its CA and server certificate live under ~/.config/joshuatree/relay-tls, with private keys on the Mac. Before another Pi test, recheck the Mac LAN address and start the reviewed relay with the existing token, API key file and TLS certificate flags as documented in docs/RELAY-TLS.md. Do not silently rotate credentials. Development card images and their backups contain credentials: keep them private.

## Limits

Clock is read-only: no alarms, timers or world clocks. Calendar is still an icon on ARM, not a full Calendar app. Clock bootstrap still uses build time. Terminal sessions reset on reboot and share browser navigation. The prior card boot proves boot only; RNG output and a real Samantha answer remain unverified on hardware.

## Next pickup

Joshua authorized more roadmap work and asked to keep README, loop pickup and roadmap current. Next is Codex access from the Pi. Official docs support device-code login in the official CLI and non-interactive read-only exec. A custom bare-metal OAuth client is not established by those docs; prefer the official CLI on the Mac, keep credentials off the card and establish tool isolation before wiring a relay. No Codex backend has been implemented or deployed yet. Do not change the current tested application source while the suite runs; use a separate branch/worktree for the next task.


## Work while Joshua is away

Boot and desktop/Clock/Terminal chapters are now in docs/GUIDE.md.
The ARM milestone list no longer repeats M1/M2 or describes confirmed board
boot and Wi-Fi joining as future work. Terminal
notes now match the model prompt, Samantha dock launch and Clock search.
Codex implementation is pending a specific approval: automatic approval
review rejected a proposed opt-in relay because it could send repository
contents through the logged-in Mac CLI. The question is pending in chat;
no relay or Pi source was changed. A sandbox probe read VERSION and refused
access to the private Codex config. That checks one boundary only; it does
not prove the whole CLI integration safe. Branch feat/pi-codex-relay is an
empty isolated worktree at work/pi-codex, based on the Clock branch.
