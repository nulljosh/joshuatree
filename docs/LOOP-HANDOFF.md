# Joshua Tree loop handoff (2026-10-08, evening)

## What the loop is

The Pi loop. Joshua Tree runs on a real Raspberry Pi 4 with a USB keyboard. The loop merges green PRs one at a time, rebuilds the SD card image, and works the roadmap toward a Pi that boots, gets online, and lets Joshua use it without a keyboard-only detour.

## The goal

Joshua's /goal: a mini Joshua Tree with Claude Code inside it, running on the Pi. Samantha answers on the real Pi through the Messages API, on Joshua's own API credit. A relay on the Mac picks a cheap or strong model per question, so nobody picks models by hand. She can read files from a shared folder on the Mac, read-only, and the Pi sends its live status (IP, clock, Wi-Fi) with each question. The next step is Samantha as an agent: the Terminal hosts her, the Console becomes logs, and she gets actions, multi-step answers and a kill switch.

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

- Main is 2.32.1. Merged today, in order: the Pi LED and Samantha actions (456, 459, 460), the HTTPS plan (458), graphs and docs (461, 463, 465 to 472), the clean benchmark numbers (473), the Codex handoff (464), the Calculator on ARM (475), the Pi browser over BearSSL TLS 1.2 (476), the local model (477), and the fail-fast browse with the DNS stub (479).
- On the board, confirmed by Joshua on 2026-10-08: `browse https://heyitsmejosh.com` printed `dns ok`, `tcp ok`, `tls ok`, `http 200`. Wi-Fi, the clock, the green light and Samantha's two actions were confirmed earlier. The Calculator, the local model and the mouse are proven only under QEMU.
- Open PRs: none known to be ready. `gh` was not reachable from this session; check `gh pr list` first.
- Finished but unpushed or unmerged branches: `token-gate` (2.32.2, the relay token only in dev builds; needs a PR), `minesweeper` (Mines icon art), `docs-batch` (a docs merge), and the two `checkpoint/*-1008` branches (state snapshots, delete once this file is on main). The prompt naming the model it used is not written yet.
- Running agents: none. This handoff came from a docs-only agent on a worktree branch, committed and not pushed.
- Spend: today's merges ran through the API credit agent and the relay. No exact figure is in the repo; read it from the Claude Platform usage page before the next round. Working estimate: a few dollars of credit left per loop pass, so keep passes to one PR each.
- Benchmark: DEFERRED. Run `/jt-bench` only when nothing else is running on the Mac.
- Naming: the file manager becomes Folio and the OS may become Mirage. Both decided in chat, neither done, trademark check still needed; each goes in its own PR.

## Next, in order

The ordered list with the why and the check for each is the Next list at the top of `docs/roadmap.md`. In short:

1. The Terminal hosts the agent, the Console becomes logs.
2. Samantha as an agent on the Pi: actions, multi-step, a kill switch.
3. Browser v2: back, forward, find, entities, links.
4. Codex login from the Pi by device code, only if the terms allow; else Codex through the relay on the Mac, read-only.
5. Security findings 2 and 3: the relay token over HTTPS, a trusted clock for the certificate check.
6. SD card writes and the self-update path.
7. The local model with real weights from the SD card.
8. The mouse on the board (needs a wired mouse).
9. `kernel/kernel.c` split in slices.
10. Trademark check before any rename.

## Restart prompt

```
/loop Joshua Tree loop. State in ~/Documents/Code/joshuatree/docs/LOOP-HANDOFF.md on origin/main. First: gh pr list, then open a PR for the token-gate branch (2.32.2) and merge it on green. Then the Next list at the top of docs/roadmap.md, in order: the Terminal hosts the agent and the Console becomes logs; Samantha as an agent on the Pi (actions, multi-step, kill switch); browser v2 (back, forward, find, entities, links); Codex by device code only if the terms allow, else through the relay read-only; the relay token over HTTPS and a trusted clock for certificates; SD card writes and self-update; real weights from the SD card; the mouse on the board; kernel.c split slices; a trademark check before any rename. One PR per pass, draft first, one tools/ci-local.sh run, ready, merge on GitHub green. Benchmark stays deferred until nothing else runs. One Haiku agent at a time, verify every claim yourself, kill by PID only. Joshua swaps the SD card by hand, so batch board tests and ask for one swap per round. Never pull the card mid-flash. Never print Wi-Fi secrets or the relay key. Stop and checkpoint at 95 percent usage.
```
