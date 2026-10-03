# Joshua Tree loop handoff (2026-10-03, morning)

## What the loop is

Build Joshua Tree up to v2.5, one small PR at a time. 2.0 is the protected-apps release, 2.2 adds Music and Movies, then 2.3 to 2.5 add the cheap everyday apps. `docs/roadmap.md` has the ladder ("Apps after 2.2"). One draft PR per version, stacked on the one before. Agents are Haiku or Sonnet, up to three at once, never QEMU-heavy ones while `tools/ci-local.sh` runs (parallel boots made checks flake). Stop spawning agents above 90 percent weekly usage.

## Where things stand

- `release/2.0.0` is PR #331 and is still open. `release/2.2.0` is draft PR #356, stacked on it, worktree `/tmp/jt-loop/r220`. VERSION is 2.2.0.
- 2.2.0 holds: Music (WAV and MP3, seek, shuffle, repeat), Movies (motion-JPEG AVI with sound, picture follows the audio clock), three decoders with ASan and UBSan host tests, per-task x87 float state, `SYS_READFILE` (401), demo songs on a FAT disk, landing and docs.
- Full `ci-local.sh`: 7 of 8 shards clean. Shard 7 lost two static checks (kernel.c line ceiling, landing app count). Both are fixed and pass alone. The full run has not been repeated, so the PR is still draft.
- Known limits: `SYS_READFILE` reads with interrupts off (loading mid-song can glitch); Music and Movies are Apps folder only, not pinned to the dock; silent clips play at about 2x on this QEMU build.

## Next, in order

1. Run `tools/ci-local.sh` once more as a direct background command (not nested in a subshell, the first attempt died silently). If green: `gh pr ready 356` and put the release notes in the PR body.
2. 2.3: Photos, Minesweeper, Solitaire, Voice Memos, Samantha media tools. Draft PR on `release/2.2.0`.
3. 2.4: movie trim, Preview, step sequencer, stereo and 16-bit audio. 2.5: Docs, Sheets, Slides.
4. Hardware (3.0, the Strata Kit board) is the other critical path and needs the physical board.

## Restart prompt

```
/loop until v2.5 (Joshua 2026-10-03). State is in docs/LOOP-HANDOFF.md and the roadmap section "Apps after 2.2". Worktree /tmp/jt-loop/r220 (recreate with git worktree add if gone), PR #356 draft. First rerun tools/ci-local.sh as a direct background command; if green gh pr ready 356. Then 2.3, 2.4, 2.5, one draft PR each, ci-local green before ready, release notes, tag and landing/docs per CLAUDE.md. Haiku/Sonnet agents, max 3 at once, none QEMU-heavy while ci-local runs, stop spawning above 90 percent weekly usage and say so.
```
