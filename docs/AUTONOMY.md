# Autonomy: the gap list

The honest distance between the Pi on the desk today and a computer that fixes itself. Written 2026-10-08. Every "missing" line below is a task; the ordered list at the end sizes them.

## The goal

You talk to the Raspberry Pi. You tell the assistant what to fix in the OS. It fixes it: the change is written, built, tested, merged, the Pi fetches the new kernel, flashes it, reboots into it and tells you it worked. Over time the model doing the thinking runs on the board itself, so the whole loop closes inside one small computer with no Linux and no cloud underneath.

## The pipeline that would close it

Nine steps. Each one says what exists and what is missing.

### 1. The Pi takes the prompt

- Exists: the Claude prompt row in the ARM Console (`arch/arm64/ask.c`). A USB keyboard works on the real Pi. Wi-Fi joins and holds, DHCP leases an address, the clock is set from the network. The Pi sends its live status (IP, clock, Wi-Fi) with each question.
- Missing: voice (no USB audio driver yet). No mouse on the real board (works in QEMU only). The Console is still the only way in; there is no separate Claude app on ARM.

### 2. The relay

- Exists: `tools/claude-relay/relay.py` on the Mac. Shared token, one request at a time, hard timeout, size cap. Two modes: headless Claude Code with read-only tools, or the Messages API on Joshua's credit with a cheap-or-strong model picked per question. `tools/checks/claude-relay-check.py` proves the rules.
- Missing: the Pi talks to it over plain HTTP on the LAN, so the token can be sniffed. No HTTPS on the Pi (the repo holds five BearSSL crypto files, not the TLS client). The relay cannot start a build; it answers questions and runs two read-only file tools.

### 3. Samantha acts

- Exists: `[[note TEXT]]` prints a note, `[[led blink]]` blinks the green light, and a benchmark runs. That is the whole set of Pi actions.
- Missing: an action that means "change the OS". There is no `[[fix TEXT]]` that hands a task to the repo agent, and no way for the Pi to learn the result later.

### 4. An agent builds a branch

- Exists: a repo agent on the API credit (`~/.config/joshuatree/agent.py`, outside the repo). Own branch, hard cost cap, no push. It can edit, build and run checks on the Mac.
- Missing: a trigger from the Pi. A way to open the draft PR by itself. A result file the Pi can poll (branch, PR number, pass or fail, one line of why).

### 5. CI

- Exists: `.github/workflows/check.yml` runs the suite on every push. `tools/ci-local.sh` runs it on the Mac first. `tools/checks/arm64-*.py` boot the Pi image under QEMU.
- Missing: nothing that blocks the loop. CI never runs on a real board, so a change that passes QEMU can still fail on the Pi.

### 6. A person merges

- Exists: the rule. Claude opens drafts, a person merges, `--admin` is never used (`docs/LOOP-HANDOFF.md`). `release.yml` tags and publishes on merge.
- Missing: nothing. This step stays human on purpose. The release should also attach `kernel8.img` so step 7 has something to fetch; today releases carry the i386 `kernel.elf` only.

### 7. The Pi fetches and flashes

- Exists: plain HTTP GET and POST over Wi-Fi (`drivers/http.c` on ARM). The SD card is read through EMMC2 under QEMU; the real board still boots from the card but the kernel reads nothing off it.
- Missing: SD card writes (MBR and FAT32, the card is a FAT32 volume with `kernel8.img` at the root). A download of several hundred KB into RAM and onto the card with a checksum. HTTPS, or a plain-HTTP mirror the Pi can trust by checksum alone.

### 8. Reboot into the new kernel, and survive a bad one

- Exists: nothing. The Pi has no software reboot call and no watchdog use.
- Missing: a reboot (the PM watchdog register on BCM2711). A/B boot: keep `kernel8.old`, boot the new image once, fall back if it never reaches the desktop. The Pi firmware's `tryboot` flag is the likely mechanism and must be checked on the board.

### 9. Report back

- Exists: the Pi can POST to a host at boot (`arch/arm64/ip.c`, built with `NETPORT=`).
- Missing: a boot message that says which version came up and whether the last update worked, sent to the relay and shown in Samantha's next answer.

## The local model question

Today there is no model on the board. The relay on the Mac does all the thinking.

What a Pi 4 with 8 GB can hold, in plain terms. A model is mostly a table of numbers, one per parameter. Stored at 4 bits each, a billion parameters take about half a gigabyte. So a 1 billion parameter model takes about half a gigabyte of RAM, a 3 billion one about one and a half, and a 7 billion one about three and a half, before the working memory the model needs while it runs. The kernel, the desktop and the apps need theirs on top. On this board, 3 billion parameters at 4 bits is a comfortable fit, 7 billion is tight, and anything bigger does not fit at all.

How fast it would answer is not known and must not be guessed. The Pi 4 has four cores and no graphics unit our code can use, so the speed is set by how fast the CPU can read the whole table from RAM once per word of output. Expect it to feel slow for a 3 billion model, and the first job is to measure it, not to promise it.

What we would have to write, since there is no Linux, no libc and no llama.cpp here:

- A reader for a model file (the GGUF layout is documented and simple).
- Integer matrix multiply with NEON for 4-bit and 8-bit weights. The FPU is already on at EL1.
- The transformer forward pass: attention, the feed-forward layers, the normalisation, and a cache for past tokens.
- A tokenizer (byte-pair) and a sampler.
- The SD card read, since a model file is far bigger than anything the Pi has loaded so far, and must not be baked into `kernel8.img`.
- The other three cores. The kernel parks them at boot; a model wants all four.

A 1 billion parameter model is the right first target: it fits with room to spare, it exercises every piece above, and a slow but correct answer proves the path. Quality at that size is low, so for a long while the local model is a fallback and a demo, and the relay stays the brain. The board can be changed later; the software path is the same.

## The gaps, in order

Size: small is a day, medium is a week, large is more.

1. A `[[fix TEXT]]` action that writes a task file for the repo agent, and a `[[status]]` action that reads its result. Small.
2. The repo agent opens a draft PR itself and writes a one-line result the Pi can poll. Small.
3. The release workflow attaches `kernel8.img`. Small.
4. SD card writes: MBR partition walk and FAT32 file write, with a check under QEMU. Large.
5. Fetch a release kernel over HTTP into RAM, verify a checksum, write it to the card. Medium, after 4.
6. Software reboot on BCM2711. Small.
7. A/B boot with `tryboot` and a fallback kernel, checked on the real board. Medium.
8. A boot report: version and last-update result sent to the relay and shown in the next answer. Small.
9. HTTPS on the Pi: vendor the rest of BearSSL's TLS client, one secure fetch as the test. Large.
10. A separate Claude app on ARM, and a mouse on the real board. Medium.
11. Start the other three cores. Medium.
12. Local inference, 1 billion parameters at 4 bits: file reader, NEON matmul, forward pass, tokenizer, measured on the board. Large.
13. Voice in: USB audio class driver for the Yeti, then speech to text through the relay. Large.

Steps 1 to 3 and 6 are small and independent. Steps 4, 5 and 7 are the hard middle: until the Pi can write its card, nothing it builds can reach it.

## The first three tasks

Each one is a full brief an agent can start from. Headless only. Draft PR, no merge, no `VERSION` edit unless the brief says so.

### Task 1: the `[[fix]]` and `[[status]]` actions

Goal: Samantha on the Pi can hand a task to the repo agent and read back its result.

Where: `tools/claude-relay/relay.py` (the action parser and `SAMANTHA` system prompt), `arch/arm64/ask.c` only if the answer format changes.

Do: add `[[fix TEXT]]`. The relay appends one JSON line `{"ts", "text", "pi"}` to `~/.config/joshuatree/tasks.jsonl` (path from a new `--tasks-file` flag, off by default) and answers "Filed." Add `[[status]]`: the relay reads the last line of `~/.config/joshuatree/results.jsonl` and returns it as plain text, or "No result yet." Both are bounded: text capped at 500 ASCII bytes, no shell, no path from the prompt.

Test: extend `tools/checks/claude-relay-check.py` with a stub answer carrying each action and assert the file contents and the reply text. Prove it fails with the parser reverted.

Done when: the check passes, `docs/CLAUDE-APP.md` lists the two actions, `docs/ARCHITECTURE.md`'s relay row mentions `--tasks-file`.

### Task 2: the release carries `kernel8.img`

Goal: a tagged release has the Pi kernel as an asset, so a future fetch has a fixed URL.

Where: `.github/workflows/release.yml`, `arch/arm64/Makefile`.

Do: in the release job, run `make -C arch/arm64 pi` (no Wi-Fi firmware, no network config; the build must still link and print `wifi no firmware`), compute `sha256sum kernel8.img > kernel8.img.sha256`, and attach both with `gh release upload`. Nothing in the i386 Makefile changes.

Test: `tools/checks/release-assets-check.sh`: parse the workflow and assert the two upload lines exist, and that a local `make -C arch/arm64 pi` without `build/wifi-fw/` produces a `kernel8.img`. Prove it fails when the upload lines are removed.

Done when: the check passes and is in `tools/checks/ci-suite.sh`, `docs/ARCHITECTURE.md` has a row for the check, and `docs/RASPBERRY-PI.md` says where to download a kernel.

### Task 3: software reboot on BCM2711

Goal: the Pi can restart itself. Step 8 of the pipeline needs it; so does any update.

Where: `arch/arm64/main.c` (or a new `arch/arm64/reboot.c` with a `docs/ARCHITECTURE.md` row), the ARM Console's command set.

Do: write the PM watchdog registers (the `PM_RSTC`, `PM_WDOG` pair with the password byte, the same sequence Linux's `bcm2835-wdt` driver uses, read from its source) to request a full reset. Add a Console line `reboot` that prints `reboot: now` then calls it. Under QEMU's `raspi4b` model the write may not reset; the function must print the line first so the check can see it. On QEMU `virt`, use PSCI `SYSTEM_RESET` instead (an `hvc` or `smc` with the documented function id), so the check can prove a real restart there.

Test: `tools/checks/arm64-reboot-check.py`: boot `virt`, type `reboot` over QMP, assert `reboot: now` then a second `M0` boot line in the serial log. Prove it fails with the PSCI call removed.

Done when: the check passes and is in CI, the row is in `docs/ARCHITECTURE.md`, and `docs/ARM64.md` lists `reboot` among the Console lines. A real-board test waits for the next card swap.
