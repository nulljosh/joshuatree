# Joshua Tree loop handoff (2026-10-04)

## What the loop is

Keep merging green PRs, then build the next item: apps on their real backends, the ARM64 port for the Raspberry Pi, and drivers that can be tested in QEMU. No stray branches or PRs left behind. The full list of what is left, with a model tag on each item, is the Pickup section of `docs/roadmap.md`.

## Where things stand (2026-10-04)

- Main is 2.6.28 (2.6.29 is the Hikko PR: Sparkjar renamed, real forum ideas through `/api/hikko`; 2.6.28 was the landing tiles PR: Bookrank, Lexly, Curbfind and Epiphany tiles, a check that fails on a stale tile, and the built `user/*.bin` and `drivers/user_*.h` untracked).
- ARM64: M0 to M1d part one are done (serial, exceptions, MMU, a drawn desktop with smooth text). M2 has its first drivers (virtio keyboard, mouse, network, disk). M3a is done: an EL0 program runs, prints and exits through `svc`, a load from a kernel-only page faults and the kernel survives, and kernel RAM is not executable from EL0 (`tools/checks/arm64-m3-check.py`).
- Joshua buys the Pi 4B this weekend. First boot is `docs/RASPBERRY-PI.md`: serial text and a picture on the monitor. 3.0.0 ships only when the desktop boots on a real Pi.
- Voice chat: both sides are typed text in, her voice out, no speech-to-text, three server round trips per message, nothing streamed. Plan in the roadmap's Voice chat section.
- Neo is the new name for the Strata enclosure.
- The pre-push hook runs per worktree and now also checks the generated files (`docs/TESTING.md`, dock slots, the logo, icon tiles), so a stale one fails on the Mac, not in CI.
- Disk: keep 6 GB free before `tools/ci-local.sh`. The Kaggle model weights moved to `/Volumes/LaCie/models/kaggle-hands-2026-10-03`.

## Next, in order

1. If the Pi is in hand: first serial boot per `docs/RASPBERRY-PI.md`. Photograph the console and fix whatever the real chip does differently.
2. Fleet apps onto their real backends through `SYS_HTTP_GET`, one PR each, with the offline samples as the fallback: Curbfind, Epiphany, Stocks, Bookrank (2.6.26), Lexly (2.6.27) and Hikko (2.6.29) already pull live data. The real apps are native Swift and cannot run here, so the OS versions stay C rewrites.
3. CI slow-runner timing flakes: eight QEMUs share one runner.
4. ARM64 M3 rest (more programs, per-program address spaces), then M2 rest (IP, DHCP, TCP, FAT).
5. Voice chat items: read `voicetime` off ten live messages, drop the `/api/pick` round trip, stream, then speech-to-text.
6. Icons in one design system.

## How to work

One small PR at a time, draft until `tools/ci-local.sh` passes (it takes about 27 minutes), bump VERSION for code changes, merge the moment CI is green. Fold PRs together before CI starts, because each rebase restarts it. Docs stay at 100 percent: every counted source file needs a row in `docs/ARCHITECTURE.md`.

## Restart prompt

```
/loop until we can't build out anymore: keep merging green PRs, then build the next item (the Pi's first boot if the board is here, then the fleet apps on their real backends, CI flakes, ARM64 M3 and M2), no stray branches or PRs left behind. State is in docs/LOOP-HANDOFF.md and the Pickup section of docs/roadmap.md.
```
