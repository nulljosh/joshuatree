# Joshua Tree loop handoff (2026-10-08, afternoon)

## What the loop is

The Pi loop. Joshua Tree runs on a real Raspberry Pi 4 with a USB keyboard. The loop merges green PRs one at a time, rebuilds the SD card image, and works the roadmap toward a Pi that boots, gets online, and lets Joshua use it without a keyboard-only detour.

## The goal

Joshua's /goal: a mini Joshua Tree with Claude Code inside it, running on the Pi. Samantha now answers on the real Pi through the Messages API, on Joshua's own API credit. A relay on the Mac picks a cheap or strong model per question, so nobody picks models by hand. She can read files from a shared folder on the Mac, read-only, and the Pi sends its live status (IP, clock, Wi-Fi) with each question. That is the first real Pi tool.

## Standing rules

- Never ask Joshua a question you can answer with a sensible default; write the default down here.
- Claude opens pull requests as drafts and never merges by hand. `gh pr merge --auto --squash` only once a PR is ready and its local run is green. Never force-push main. Never use `--admin`.
- One Haiku agent at a time, and only when asked. Verify every claim yourself. Kill by PID only.
- One `tools/ci-local.sh` at a time (it takes a lock). Disk above 6 GB free.
- Joshua swaps the SD card by hand. Batch board tests and ask for one swap per round. Never pull the card while `tools/flash-pi.sh` is running; a cut-off flash froze the Mac's disk until a restart.
- At most six open PRs, zero issues. The loop does not mirror the roadmap into GitHub issues. Delete a branch once its PR merges.
- Ship visible fixes as their own tiny PR; the big stack stays one draft.
- Never print or commit the Wi-Fi password, tokens or keys. Wi-Fi name and key go only into dev cards (`tools/flash-pi.sh` with `JT_WIFI_DEV=1`); release and CI builds carry no network. Nothing destructive on his files or the LaCie.
- The relay's API key lives outside the repo. Never copy it into a file, a log or a PR.

## Where things stand

- Main is 2.32.1. Merged today: the Pi Calculator with scientific mode (#475), a text browser on the Pi with HTTPS (#476), a tiny local LLM on the Pi (#477), the clean bench numbers (#473), a Bookrank deflake (#474), the Codex notes (#464), and the browse DNS fix (#479). The DNS fix: the Pi's DNS went to QEMU's 10.0.2.3 instead of the DHCP server. Each browse step now times out fast and prints dns, tcp, tls and http lines.
- The SD card holds the 2.32.1 dev build. The browser is not yet confirmed on the real board. The Mac lost sight of the card for a long stretch (reader or port), and the flash itself takes seconds.
- Security, open: every Pi image built on the Mac embedded the relay token, not only the dev card. The fix is draft PR 481 (token and relay host only with JT_WIFI_DEV=1). It is not merged. Two more findings are open: the token goes in clear over Wi-Fi, and the certificate clock is set from an HTTP Date header nobody authenticates.
- Drafts: 478 (Minesweeper, icon rule failures), 480 (docs batch: LESSONS, RESEARCH, LOCAL-LLM plan, SECURITY-PI review, CLAUDE.md trimmed with loop rules), 481 (token gate).
- Finished branches with no PR yet: self-update path from the SD card, SD card writes, loop safety tools, landing and docs refresh, CI doc, the kernel.c usertest slice, the bench.sh fix, QMP ports, the bench-docs check, the Hikko deflake.
- Benchmark: DEFERRED. A repo agent (`agent.py`) was running at 17:00. Run `/jt-bench` only when nothing else is running on the Mac.
- Credit: the repo agents run on the API credit. About $60 spent this afternoon, roughly $80 left, expires Nov 4. The main session only reviews and merges.
- Weekly Claude usage is 98 percent and resets Saturday 22:00. Loop is live until then.
- Process lessons: open drafts first (drafts never run CI, so no failure emails). Run the suite locally through credit agents before ready. Never resolve a Makefile conflict by taking one side; the calc.o FPCC rule was lost once that way. Generated files take main's side on conflicts.

## Next, in order

1. Merge 481 (token gate) before any card goes to anyone else. Then fix the two open findings: token over Wi-Fi in clear, and the clock from an unauthenticated Date header.
2. Confirm the browser on the real board with the 2.32.1 card, once the SD reader is back.
3. Move 478 out of draft once its icon rules pass; review 480.
4. Open PRs for the finished branches, one at a time, starting with the bench.sh fix (it wipes the real-Pi section of docs/BENCHMARKS.md).
5. Benchmark when the Mac is quiet.
6. Split kernel/kernel.c, which is too big to work in.

## Restart prompt

```
/loop Joshua Tree loop, live until Saturday 22:00. State in docs/LOOP-HANDOFF.md on origin/main. First: merge 481 (relay token only in dev builds) once its CI is green, then fix the token-over-Wi-Fi and Date-header clock findings. Then confirm the 2.32.1 browser on the real board when the SD reader is back, move 478 out of draft and review 480, and open PRs for the finished branches one at a time. Drafts first, one tools/ci-local.sh at a time, merge on GitHub green, never resolve a Makefile conflict by taking one side. Benchmark only when no agent.py, ci-local, ci-suite or qemu-system is running. Credit is about $80 until Nov 4; reviews and merges only near the Saturday reset (98 percent weekly).
```
