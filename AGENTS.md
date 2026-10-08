# Codex project instructions

Shared project guidance is in `CLAUDE.md`. Read it before working in this
repository. Keep project rules there so Codex and Claude use the same source.

Work on a branch and open a pull request for review. Merge, enable auto-merge,
or deploy only after Joshua explicitly approves the PR. This takes precedence
over the automatic ship-and-repeat loop in `CLAUDE.md`.

## Picking this up with Codex (the $20 plan)

Start with `docs/LOOP-HANDOFF.md`, then `docs/roadmap.md`'s Top 10. The plan has far less room than the Claude Max plan, so keep each task short: one task, one branch, one PR.

Good fits for Codex: reviewing a PR against its checks, running `tools/ci-local.sh` (one run at a time) and reading the failure, small fixes with a test, relay and script work in Python, docs and roadmap tidying, and regenerating the graphs (`tools/gen/`). Poor fits: long unattended loops, the big `kernel/kernel.c` split, or anything that needs the real Pi board (flashing and the photos stay with Joshua).

Use the shared notes in `CLAUDE.md` ("Pi, Samantha and the relay") before touching `arch/arm64/`.

