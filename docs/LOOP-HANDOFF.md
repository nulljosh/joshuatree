# Joshua Tree loop handoff (2026-10-08, late evening)

## Pickup update (2026-10-09)

The older snapshot below is historical. Cached main is 2.33.1. Keyboard work through `ec114bf9` is on `fix/pi-input-wifi-20261009`, in `~/Documents/Code/jt-pi-repair`, with a 2.33.2 patch prepared for review.

Spotlight shortcuts now toggle search closed. F2 and Ctrl+T close search and open Terminal. Search debug lines stay on serial so they do not repaint through the overlay. The keyboard-only and USB key-debug checks pass, including exact pixel restoration. A fresh Pi dev image builds. The full suite and the real-board test are still pending; no SD card was flashed. Wi-Fi was left alone because Joshua reports it works about nine times out of ten.

Next: review the draft PR, then run the full local suite before ready. Merge or deploy only after Joshua approves the PR. Batch the board check into one card swap: letter opens search, type `term`, Enter opens Terminal, Esc returns, search shortcuts toggle, and F2/Ctrl+T work while search is open. Do not restart the broad roadmap loop for this fix.

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

- Evening: Joshua flashed 2.32.1 and confirmed on the real Pi 4: `browse https://heyitsmejosh.com` gives dns ok, tcp ok, tls ok, http 200, the page title and a link. First HTTPS page fetched by his own OS on the board. Wi-Fi icon and a network clock show in the menu bar. JavaScript-heavy pages show little text.
- Main is 2.32.1. Merged today: the Pi Calculator with scientific mode (#475), a text browser on the Pi with HTTPS (#476), a tiny local LLM on the Pi (#477), the clean bench numbers (#473), a Bookrank deflake (#474), the Codex notes (#464), and the browse DNS fix (#479). The DNS fix: the Pi's DNS went to QEMU's 10.0.2.3 instead of the DHCP server. Each browse step now times out fast and prints dns, tcp, tls and http lines.
- The SD card holds the 2.32.1 dev build. The browser is not yet confirmed on the real board. The Mac lost sight of the card for a long stretch (reader or port), and the flash itself takes seconds.
- Security, open: every Pi image built on the Mac embedded the relay token, not only the dev card. The fix is draft PR 481 (token and relay host only with JT_WIFI_DEV=1). It is not merged. Two more findings are open: the token goes in clear over Wi-Fi, and the certificate clock is set from an HTTP Date header nobody authenticates.
- Drafts: 478 (Minesweeper, icon rule failures), 480 (docs batch: LESSONS, RESEARCH, LOCAL-LLM plan, SECURITY-PI review, CLAUDE.md trimmed with loop rules), 481 (token gate).
- Finished branches with no PR yet: self-update path from the SD card, SD card writes, loop safety tools, landing and docs refresh, CI doc, the kernel.c usertest slice, the bench.sh fix, QMP ports, the bench-docs check, the Hikko deflake.
- Benchmark: DEFERRED. A repo agent (`agent.py`) was running at 17:00. Run `/jt-bench` only when nothing else is running on the Mac.
- Credit: about $133 of $200 spent, about $66 left (expires Nov 4). Earlier: about $124 of $200 spent (logs, 70 finished agents), roughly $76 left, expires Nov 4. Finished on branches: landing refresh v2 and QA page, loop hardening scripts, CI doc, security review, local-LLM plan, token gate fix (draft 481), a prompt that names the model (hit its turn limit, WIP, not merged). Running or queued: browser v2, Codex groundwork, Codex capture harness (local mocks only), Samantha agent, roadmap sync, relay over TLS (the token no longer crosses Wi-Fi in clear), on-device Wi-Fi setup for cards with no key, local landing browser checks (11 fail locally in 0 s, being diagnosed), Calendar icon fix, /model and /effort per Terminal pane after the Samantha agent. The main session only reviews and merges.
- Late evening: rendered mode and a QuickJS port are queued (the Mac relay renders JS pages in headless Chromium for `browse -r`; QuickJS on the Pi plus a tiny page model for simple scripts). Real video needs a JPEG/MJPEG path and an audio driver, neither built. Joshua was asked whether he listens through HDMI or the 3.5mm jack.
- Codex on the Pi is gray. Decision waits on Joshua testing `browse https://chatgpt.com` on the Pi. Then either Pi-direct device-code login (token on the dev card only) or official codex on the Mac via the relay, read-only. Nothing for the login is built.
- Batches: about 25 branches wait and CI takes 20 to 25 minutes per PR, so the loop merges in about six themed batches (docs, Pi security, Pi OS core, browser, tooling, SD and self-update and Wi-Fi), each after its local suite passes. A 15-minute ping loop to Joshua counts down to the core being flashable (about 23:00 Thursday).
- Goals set: Samantha as an agent in Joshua Tree (see roadmap, decided 2026-10-08 evening); a working browser controllable from Console and Terminal.
- Process: new PRs open as drafts so CI does not send failure emails. A credit agent runs the full local suite first. 11 landing, demo and portfolio browser checks cannot run locally (no Chromium), so CI answers those.
- Weekly Claude usage is 98 percent and resets Saturday 22:00. Loop is live until then.
- Process lessons: open drafts first (drafts never run CI, so no failure emails). Run the suite locally through credit agents before ready. Never resolve a Makefile conflict by taking one side; the calc.o FPCC rule was lost once that way. Generated files take main's side on conflicts.

## Next, in order

1. Joshua is flashing another card: the 481 token gate must be merged before it goes to anyone else. Then fix the two open findings: token over Wi-Fi in clear, and the clock from an unauthenticated Date header.
2. Browser confirmed on the board (evening). Next: browser v2 and the Terminal and Console split.
3. Move 478 out of draft once its icon rules pass; review 480.
4. Open PRs for the finished branches, one at a time, starting with the bench.sh fix (it wipes the real-Pi section of docs/BENCHMARKS.md).
5. Benchmark when the Mac is quiet.
6. Split kernel/kernel.c, which is too big to work in.

## Restart prompt

```
/loop Joshua Tree loop, live until Saturday 22:00. State in docs/LOOP-HANDOFF.md on origin/main. First: merge 481 (relay token only in dev builds) once its CI is green, then fix the token-over-Wi-Fi and Date-header clock findings. Then confirm the 2.32.1 browser on the real board when the SD reader is back, move 478 out of draft and review 480, and open PRs for the finished branches one at a time. Drafts first, one tools/ci-local.sh at a time, merge on GitHub green, never resolve a Makefile conflict by taking one side. Benchmark only when no agent.py, ci-local, ci-suite or qemu-system is running. Credit is about $80 until Nov 4; reviews and merges only near the Saturday reset (98 percent weekly).
```
