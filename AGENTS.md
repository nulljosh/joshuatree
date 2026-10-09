# Joshua Tree

A from-scratch OS: a freestanding i386 kernel plus an ARM64 build that runs on a real Raspberry Pi 4. No libc. Needs only clang, ld.lld and qemu.

- Subsystems: `docs/ARCHITECTURE.md`. Plan and queue: `docs/roadmap.md`. Look and feel: `docs/DESIGN.md`.

## Build and verify

- `make` links `kernel.elf`. `make run` boots it. `./check.sh` is the boot test.
- Build with stock Apple clang (`-target i386-unknown-none`). Do not add a cross-toolchain.
- `check.sh` reads VGA memory through the QEMU monitor. It proves the kernel boots, nothing more. Check paging, disk and task switches against real artifacts.
- `tools/ci-local.sh` runs the full suite locally (one run at a time). `.github/workflows/check.yml` runs it on push; keep it current.
- Every shipped feature gets a permanent test that fails when the fix is reverted: a shell command in `kernel/kernel.c` (like `heaptest`) or a `tools/checks/` script.
- After any rename, move or delete, run `bash tools/checks/check-refs.sh`. `make hooks` installs the pre-push hook once per clone.

## Headless only

- Never open a QEMU window or a browser to verify. A real screenshot needs this Mac's display: `-display cocoa`, bring it frontmost, `screencapture`.
- The monitor's `sendkey ret` is unreliable; letters work. To see a command's output, call it from `kmain` before `clear()`, dump VGA memory, then revert.

## QEMU debug timeout

Any QEMU run with a `-d` trace and `-D <file>` gets `timeout` in front: `timeout 60 qemu-system-i386 ... -d int -D /tmp/x.log`. Delete the log afterwards. An untimed run once filled 5 GB and took the Mac down.

## Docs

- Docs stay at 100 percent: every source file the progress graph counts has a row in `docs/ARCHITECTURE.md`, added in the same PR.
- Plain words, short sentences. A roadmap entry says what changed and why in two or three lines, then names its check. Long stories go in the commit or PR.

## Versioning

`VERSION` is semver `MAJOR.MINOR.PATCH`. MINOR for a new capability, PATCH for a fix, MAJOR for a real break. A code PR must bump it; the merge cuts a release (bare `X.Y.Z` tag). Release notes say what shipped, the root cause and how it was checked. Before a MINOR, rerun the full suite, review any security edge the code touches, and take one screenshot.

## Theme

Keep the Satellite wallpaper in colour. Landing page: off-white `#faf8f4`, near-black ink, one terracotta accent `#b5502c`, rounded cards, no teal, purple or gradients. Dark mode is `#141311`. The logo comes from `tools/gen/logo.py`. The rest is in `docs/DESIGN.md`.

## ARM64 and the Pi

`arch/arm64/` is a separate build (plan in `docs/ARM64.md`). `make -C arch/arm64 run` boots it in QEMU; `tools/checks/arm64-m0-check.py` proves it. The i386 Makefile is untouched.

- Flash with `tools/flash-pi.sh`. Only that dev card carries the Wi-Fi key and relay token. Keys live in `~/.config/joshuatree/` and `~/.claude-relay-token`; never print or commit one.
- The relay (`tools/claude-relay/relay.py`) answers the Pi's prompt (`Claude Haiku 5.5 $ `, named for the model that last answered) through the Messages API.
- Samantha acts on the Pi with `[[note TEXT]]` or `[[led blink]]` lines, run by `arch/arm64/ask.c`. A new action goes in the relay, in `ask.c` and in `tools/checks/pi-actions-check.py`.
- The green light is GPIO 42, never driven at boot. Never wait on `ticks` in `arch/arm64/main.c`: it stops counting and hangs the boot loop.
- The SD card drops off the Mac for a second or two. Flash with a loop that waits for it, mounts and writes in one step.

## Loop rules

- PRs open as drafts (`gh pr create --draft`). Run `tools/ci-local.sh`; only when it passes, `gh pr ready`.
- On a conflict in a generated file (`VERSION`, `landing/index.html`, `landing/version.txt`, `docs/TESTING.md`), take main's side, then regenerate.
- Never resolve a Makefile conflict by taking one side blindly. Merge both by hand.
- Never push with conflict markers in any file.
- New arm64 checks must be wired into `tools/checks/ci-suite.sh`.
- Resume state is in `docs/LOOP-HANDOFF.md`. What keeps going wrong is in docs/LESSONS.md.

## Naming

"Joshua Tree" is the name; "Leopard Gecko" is dropped. Not to be confused with gato, a separate macOS voice app.

## Shared agent guidance

`AGENTS.md` and this file contain identical project guidance. Update both
together; this repo does not allow tracked symlinks.

Shared project guidance is in `CLAUDE.md`. Read it before working in this
repository. Keep project rules there so Codex and Claude use the same source.

Work on a branch and open a pull request for review. Merge, enable auto-merge,
or deploy only after Joshua explicitly approves the PR. This takes precedence
over the automatic ship-and-repeat loop in `CLAUDE.md`.

## Picking this up with Codex (the $20 plan)

Start with `docs/LOOP-HANDOFF.md`, then `docs/roadmap.md`'s Top 10. The plan has far less room than the Claude Max plan, so keep each task short: one task, one branch, one PR.

Good fits for Codex: reviewing a PR against its checks, running `tools/ci-local.sh` (one run at a time) and reading the failure, small fixes with a test, relay and script work in Python, docs and roadmap tidying, and regenerating the graphs (`tools/gen/`). Poor fits: long unattended loops, the big `kernel/kernel.c` split, or anything that needs the real Pi board (flashing and the photos stay with Joshua).

Use the shared notes in `CLAUDE.md` ("Pi, Samantha and the relay") before touching `arch/arm64/`.

## The loop

Read `docs/LOOP-HANDOFF.md` for current state and the restart prompt.
