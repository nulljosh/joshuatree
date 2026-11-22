# Joshua Tree

A from-scratch OS: an i386 kernel plus an ARM64 build for the Raspberry Pi 4. No libc. Needs clang, ld.lld and qemu. "Joshua Tree" is the name; "Leopard Gecko" is dropped, and it is not to be confused with gato, a separate macOS voice app.

Docs: `docs/GUIDE.md` (use, Pi, hardware), `docs/ARCHITECTURE.md` (design, syscalls, tests), `docs/ROADMAP.md` (plan), `docs/TESTING.md` (board list), `docs/LOOP-HANDOFF.md` (loop state). Add to these. No new .md files.
## Build and test
- `make` links `kernel.elf`. `make run` boots it. `./check.sh` proves only that the kernel boots.
- Use stock Apple clang (`-target i386-unknown-none`). No cross-toolchain.
- Check paging, disk and task switches against real artifacts.
- `.github/workflows/check.yml` is the merge gate. Keep it current.
- Run only targeted checks locally, one at a time. `tools/ci-local.sh` is the full run, only when nothing else is running.
- After any landing tour change, QA the whole tour in real Chrome before merging.
- Push a PR once, with all its commits. Each extra push cancels the running workflow.
- Batch small fixes: one PR per session, not one per fix. Every merge cuts a release the Pi pulls (13.21.4 to 13.22.0 made five releases on 2026-11-21).
- Every shipped feature gets a permanent test that fails if the fix is reverted: a command in `kernel/kernel.c` (like `heaptest`) or a `tools/checks/` script.
- After any rename, move or delete, run `bash tools/checks/check-refs.sh`. `make hooks` installs the pre-push hook once per clone.
- Never open a QEMU window or a browser to verify. A real screenshot needs the Mac display: `-display cocoa`, frontmost, then `screencapture`.
- The monitor's `sendkey ret` is unreliable; letters work. To see a command's output, call it from `kmain` before `clear()`, dump VGA memory, then revert.
- A QEMU run with `-d` and `-D <file>` gets `timeout` in front (`timeout 60 qemu-system-i386 ... -d int -D /tmp/x.log`). Delete the log after.
## Docs
- Docs stay at 100 percent: every source file the progress graph counts has a row in the Architecture section of `docs/ARCHITECTURE.md`, in the same PR.
- Plain words, short sentences. A roadmap entry says what changed and why in two or three lines, then names its check. Long stories go in the commit or PR.
## Versioning
- `VERSION` is semver. A code PR must bump it. The merge cuts a release (bare `X.Y.Z` tag).
- MINOR: a new user-visible capability. Its first slice is the MINOR; follow-ups are PATCHes until it is complete.
- PATCH: a fix, docs, tests, refactors, or polish of something shipped.
- MAJOR: a break (syscall or app ABI, on-disk format, removed feature) or a milestone Joshua names. Joshua decides, and it waits on the roadmap gate (8.0: someone other than Joshua follows the SDK page and runs their own app). Never per PR or per loop.
- Do not renumber or delete shipped tags. Release notes say what shipped, the root cause and how it was checked.
- Before a MINOR: rerun the full suite, review any security edge the code touches, take one screenshot.
## Theme
- Pi wallpaper: the park picture (`arch/arm64/park_wall.h`). QEMU starts on flat blue `#3B6EA5`. Settings offers Blue, Terracotta, Clay, Ink and Park. Brand accent: terracotta `#b5502c`. UI face: Tree Sans.
- Mark: a painted Joshua tree (`docs/brand/tree.png`), a flat traced glyph at 32 px and under, every copy from `tools/gen/gen_brand_art.py`. Boot splashes and menu bars show the glyph in one ink (`tools/gen/gen_boot_mark.py`).
- Landing: paper `#f4f3ec`, bark ink `#261a13`, one terracotta accent, rounded cards. No teal, purple or gradients. Dark mode `#15110e`.
- More in `docs/ARCHITECTURE.md` (Design rules).
## ARM64 and the Pi
Read this section before touching `arch/arm64/`.
- `arch/arm64/` is a separate build. `make -C arch/arm64 run` boots it; `tools/checks/arm64-m0-check.py` proves it. The i386 Makefile stays untouched.
- Flash with `tools/flash-pi.sh`. Only the dev card carries the Wi-Fi key and relay token. Keys live in `~/.config/joshuatree/` and `~/.claude-relay-token`. Never print or commit one.
- The relay (`tools/claude-relay/relay.py`) answers `sam QUESTION`. Run it with `--turing`. `--api-key-file` spends Claude Platform credit and starts only with `--allow-api-spend`.
- Samantha's `[[note TEXT]]` and `[[led blink]]` actions run in `arch/arm64/ask.c`. A new action goes in the relay, `ask.c` and `tools/checks/pi-actions-check.py`.
- The green light is GPIO 42, never driven at boot. Never wait on `ticks` in `arch/arm64/main.c`: it stops counting and hangs the boot loop.
- The SD card drops off the Mac for a second or two. Flash with a loop that waits for the card, then mounts and writes in one step.
- New arm64 checks must be wired into `tools/checks/ci-suite.sh`.
## Loop
- Where the loop stands and its restart prompt: `docs/LOOP-HANDOFF.md`. Resume state: the Loop handoff section of `docs/ROADMAP.md`. What keeps going wrong: the Lessons section of `docs/ARCHITECTURE.md`.
- PRs open as drafts (`gh pr create --draft`). Run targeted checks, `gh pr ready` before the first push, then merge on green CI, one PR at a time (the repo requires up-to-date branches).
- On a conflict in a generated file (`VERSION`, `landing/index.html`, `landing/version.txt`, generated blocks in `docs/ARCHITECTURE.md`), take main's side, then regenerate.
- Never resolve a Makefile conflict by taking one side blindly. Never push conflict markers.
## Other agents
`AGENTS.md` points Codex here. Codex starts with the Loop handoff in `docs/ROADMAP.md`, then its Top 10, one task, one branch, one PR. Good fits: reviews against checks, `tools/ci-local.sh` (one run at a time), small tested fixes, relay and script work, docs, regenerating `tools/gen/` graphs. Poor fits: long unattended loops, the big `kernel/kernel.c` split, anything needing the real Pi board.
