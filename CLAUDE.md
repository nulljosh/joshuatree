# Joshua Tree

A from-scratch OS: a freestanding i386 kernel (renamed from `os` in Sep 2026) plus an ARM64 build that runs on a real Raspberry Pi 4. No libc, no dependencies beyond clang, ld.lld and qemu.

- `make` links `kernel.elf`. `make run` boots it. `./check.sh` is the only automated boot test.
- Builds with stock Apple clang (`-target i386-unknown-none`). No cross-toolchain; do not add one.
- Subsystem map: `docs/ARCHITECTURE.md`. Plan and verification notes: `docs/roadmap.md`. Design rules (icons, type, colour, dock, motion): `docs/DESIGN.md`, kept honest by `tools/checks/design-doc-check.py`.

## Verification

- `check.sh` reads raw VGA memory through the QEMU monitor (a headless `screendump` is black even when the kernel is fine). It proves "boots without crashing" and nothing more. Paging, disk I/O and context switches need checking against real artifacts.
- **Headless only.** Never open a QEMU window or a browser to verify anything. A pixel screenshot is possible only from a session with this Mac's display: launch `-display cocoa`, bring it frontmost, use `screencapture`.
- The monitor's `sendkey ret` does not reliably reach the kernel; letters do. To check a command's output, call it from `kmain` before `clear()`, dump VGA memory, then revert.
- Every shipped feature gets a permanent test that fails when the fix is reverted and passes when restored: a shell command in `kernel.c` (the `heaptest`/`tasktest`/`ring3test` pattern) or a `tools/checks/*-check.{sh,py,mjs}` script.
- After any rename, move or delete, run `tools/checks/check-refs.sh`: it fails when a backticked path in the docs points at nothing. `make hooks` (once per clone) installs `tools/hooks/pre-push` (check-refs plus a compile; skip once with `git push --no-verify`).
- `.github/workflows/check.yml` runs the whole regression suite on every push. Keep it current.
- Before a MINOR release: rerun the full suite, look at any security boundary the new code touches (user input, network data, a privilege edge), and take one screenshot to confirm the chrome matches the theme. Skip for PATCH.
- `gh release create`/`edit` gets real notes (what shipped, the root cause, how it was verified). `docs/roadmap.md`'s entry is usually the source.

## Docs

- Docs stay at 100%: every source file the progress graph counts has a row in `docs/ARCHITECTURE.md`, added in the same PR as the file. The landing page shows the number, so a dip is public.
- Write for people first: plain words, short sentences, one idea per sentence. A roadmap entry says what changed and why it matters in two or three lines, then names the check that proves it. Long stories (root cause, what was tried) go in the commit or PR. Name a file or flag only when the reader has to go there.

## Versioning

`VERSION` is `MAJOR.MINOR.PATCH`, semver since v31 (`v1`-`v30` keep their original tags). MINOR for a new capability, PATCH for a fix, MAJOR only for a real break. A code PR must bump it (the push hook checks), and the merge cuts a release through `.github/workflows/release.yml` (bare `X.Y.Z` tag, no letters). Each MAJOR gets a codename; none exists for 1.0.0.

## Theme

Keep the Satellite wallpaper in colour. The landing page is off-white `#faf8f4`, near-black ink, one terracotta accent `#b5502c`, simple rules and rounded cards; no teal, purple or gradients. Dark mode is warm near-black `#141311`. The logo is a crayon scribble of one Joshua tree, drawn by `tools/gen/logo.py` (`landing/logo.svg` with a light grain, `landing/mark-bold.svg` clean for 16 to 48 px, `landing/mark.png` and `landing/badge.png` for other sizes). The rest of the OS look is in `docs/DESIGN.md`.

## Landing page and demo

`landing/index.html` (deployed to joshuatree.heyitsmejosh.com) embeds a live v86 emulator booting this `kernel.elf` (`landing/v86/embed.js`). Input is captured only after a click or tap, because v86 listens on `window`; an 8 s idle timer starts a scripted tour and hands control back on the first click or key. `landing/v86/kernel.elf` is a build artifact, produced by `make kernel.elf` and built fresh by `.github/workflows/deploy.yml`, which runs `wrangler deploy` on every push to `main` that touches `landing/**`, `wrangler.toml` or the kernel sources (this repo's exception to the fleet-wide "a push deploys nothing" rule).

`tools/gen/progress.sh` redraws `progress.svg` from `git log`; rerun it after commits that should move the line. `python3 tools/gen/landing-roadmap.py` refreshes the landing page's "Where it's going" card from the Session task queue in `docs/roadmap.md` (`--check` verifies it; deploy runs it).

## Task routing

The main session directs (Sonnet or Opus; Fable for privilege, security or exact-layout work such as wire protocols, register frames and memory-model changes). Mechanical, well-scoped work with a known shape (tagged `[Haiku]` in `docs/roadmap.md`) goes to a Haiku subagent with full instructions: root cause, fix, required test, verification, commit and push.

## The loop

Resume state is in `docs/LOOP-HANDOFF.md`; its PR approval requirement stays in force. Each pass:

1. Take a direct request first; otherwise the open queue in `docs/roadmap.md`, reread fresh from disk (other agents commit to it in parallel).
2. Absent a request, the standing focus is typeface and icon sharpening, checked against real screenshots.
3. Borrow technique from prior art (OSDev wiki, xv6, ToaruOS, Linux/BSD) for anything with a known-solved shape.
4. Verify against an artifact (`check.sh`, a screenshot, a disk image), then run `tools/checks/check-refs.sh` after any rename.
5. Ship: commit, open a draft PR (`gh pr create --draft`, so pushes do not trigger CI), run `tools/ci-local.sh` (one run at a time), and only when it passes `gh pr ready <N>`. Bump `VERSION` for kernel work and give one clear TLDR.

## ARM64 and the Pi

`arch/arm64/` is a separate build for the Raspberry Pi (plan in `docs/ARM64.md`). `make -C arch/arm64 run` boots it under `qemu-system-aarch64`; `tools/checks/arm64-m0-check.py` proves it. The i386 Makefile is untouched.

- Flash with `tools/flash-pi.sh`: only that dev card carries the Wi-Fi key and the relay token. Release builds carry neither; the user brings their own key. Keys live outside the repo (`~/.config/joshuatree/`, `~/.claude-relay-token`); never print or commit one.
- The relay (`tools/claude-relay/relay.py`, `--api-key-file`) answers the Pi's `ask>` question through the Messages API on the Claude Platform credit. It picks Haiku, Sonnet or Opus by the question and gives Samantha two read-only file tools for `~/pi-files`. The Pi sends its live status (IP, clock, Wi-Fi bars) with each question.
- Samantha can act on the Pi by ending an answer with `[[note TEXT]]` or `[[led blink]]` on their own lines; `arch/arm64/ask.c` strips and runs them. Add an action in both places, and in the pi-actions check that ships with the Pi actions PR.
- The green light is GPIO 42 on the chip (Pi 4), driven through the GPIO registers and never at boot. Never wait on the `ticks` variable in `arch/arm64/main.c`: it stops counting early, and a wait on it hangs the boot loop and kills the keyboard.
- The SD card drops off the Mac for a second or two. Flash with a loop that waits for it, mounts it and writes in one step.

## Naming

"Joshua Tree" is the project's name; the earlier "Leopard Gecko" idea is dropped. Not to be confused with gato (`~/Documents/Code/gato`), a separate macOS voice app.

## Debug QEMU runs always get a timeout

Never start QEMU with `-d int` (or any `-d` trace) and `-D <file>` without `timeout` in front. On 2026-09-29 one ran for two days, grew `/tmp/fbq.log` to 5 GB and took the Mac down twice. Use `timeout 60 qemu-system-i386 ... -d int -D /tmp/x.log`, and delete the log afterwards.
