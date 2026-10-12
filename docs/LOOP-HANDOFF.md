# Joshua Tree handoff (2026-10-12, morning)

## The goal

Joshua, 2026-10-12: "/loop all day and night until v10." The loop ships one slice at a time on the Pi build, merges on green CI, and keeps this file, the roadmap and the graphs current. 3.0.0 "Hold it" is out (the Pi release). The next major is 4.0 "Many windows"; the gate for every major is in [DECISIONS.md](DECISIONS.md).

## Where it stands

Main is 3.5.0, released. Shipped since 3.0.0: Samantha lists and reads the card (3.1.0), Books (3.2.0), graphs restyled (3.2.1), a zsh-style Terminal with `sam` as a non-blocking command (3.3.0), `reboot` and `shutdown` (3.4.0), a Bluetooth mouse that saves its pairing to `JT/BTPAIR.BIN` (3.5.0).

In flight, merge in this order (each rebases onto the one before; take main's side on generated files and regenerate):
1. PR 557, window drag by the title bar, 3.6.0. In CI.
2. PR 559, Browser flicker: draw into an off-screen buffer, copy once. 3.6.1, stacked on 557. Joshua saw the flicker on the board, so this is the first fix to reflash.
3. PR 561, `reboot` refused after a Samantha reply, 3.5.1 on main, so it needs a new patch number after 557 lands.
4. PR 558, the mascot on empty Music and Books, 3.7.0.
5. PR 560, the daily driver gap list (docs only).

## What Joshua saw on the board (2026-10-12)

Works: boot, the desktop, the Bluetooth mouse (after a scan and pair on a fresh card), the zsh prompt, `sam` answers over the relay, Music and Books open, the Browser opens with the mascot.

Broken: the Browser flickers (fix in 559), `reboot` was refused after a `sam` answer (561), `sam "what is 5+5"` took about a minute (queued in the roadmap with its suspects: a TLS handshake per question, a dead baked relay host costing three probes, several agent steps on a small model; it needs `ticks()` timing on the board), and `sam` file questions failed when the Mac's Ollama was off (start it with `ollama serve`).

Not yet proven on the board: the Bluetooth reconnect after a reboot (3.5.0), window drag, the off-screen Browser draw, sound, SD writes.

## Next, in order

The full list is `docs/roadmap.md`, "Daily driver gaps". The order: the board bugs above, Bluetooth reconnect when the mouse sleeps, JavaScript in the Browser, drag for the Calculator and apps, clipboard and Cmd shortcuts, boot splash and login, Finder and USB storage, ssh, then the Kali-style safe network tools and a package manager. Each PR ticks its own line in the roadmap.

## How the loop runs

One Sonnet agent at a time, in a worktree, opening a draft PR. I read the diff, rerun its check and `inject-landing-facts.py` (must print `documented=100`), then mark it ready so CI runs. CI is about 25 minutes and a push restarts it. Merge only on green: `gh pr merge N --squash --delete-branch`. A flaky shard (runner setup, a Cloudflare reply, the phone-boot hint row) gets `gh run rerun ID --failed`; a real failure gets a real fix. A merge that changes `VERSION` cuts the release. Joshua flashes the card himself and says "card in"; the `flash-card` skill then runs `tools/flash-pi.sh "/Volumes/NO NAME"` from current main or the branch he names.

Every release PR also refreshes: this file, the roadmap ticks, `docs/TESTING.md` (`python3 tools/gen/testing-doc.py`), landing facts and headline, and the graphs (`bash tools/gen/progress.sh`, `python3 tools/gen/memory-map.py`, `python3 tools/gen/map.py`). The Pi row in `tools/gen/map.py` is typed by hand: update it when the board proves something new.

## Rules kept

No bare `git stash` (shared across worktrees). Generated files (VERSION, landing/*, docs/TESTING.md) take main's side in a merge and are regenerated. No keys on the public card; only the dev card carries the Wi-Fi key and relay token. Never touch the running relay. No crontab, watchdog or auto-commit daemon. Headless QEMU only. Every X.0.0 ships a refreshed logo and UI.
