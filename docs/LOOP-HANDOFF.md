# Joshua Tree loop handoff (2026-09-26, evening)

## What the loop is

Keep building Joshua Tree toward 2.0, watching usage until next major version release. Every 90% session usage or weekly reset, pause and /checkpoint. Merge on green CI, never ask. Haiku subagents 15 min max for code, max two at once; CI waits go to subagents not background tasks. Fix CI, never mute. Main session merges, picks next item. A+ bar always: spotless visuals, every line Joshua's voice mixed with Apple/Jobs. No LOC metrics public.

## Where things stand

Main is 1.6.0: released built-in benchmarks with numbers on README and landing. Boot to shell 250 ms, alloc+free 67 ns, memcpy 715 MB/s, context switch 3 us, disk 7 MB/s. PR #218 (portfolio demo: scripted tour, 40 live tickers, Curbfind local deals) auto-merge armed, CI green. Clock app (PR #219) draft, check passing. ARCHITECTURE.md rewritten as human-readable map (PR #216 merged). PR #202 (boot splash) conflicts with main, needs rebase.

## Next, in order

1. Merge PR #217 (benchmarks 1.6.0) on green, bump to 1.6.1.
2. Rebase PR #202 (boot splash) on main, merge when green.
3. Ready and merge PR #219 (clock with timer/alarm, 1.7.0).
4. Continue roadmap queue (max two Haiku subagents, 15 min each).
5. Checkpoint at 90% usage or weekly.

## Restart prompt

```
/loop Joshua Tree loop: merge 217 when green, rebase and merge 202, ready and merge 219, then roadmap queue with max 2 Haiku agents at 15 min each. Until next major release, watch usage. Merge on green, fix CI never mute, every check before push, A+ bar, headless only, /checkpoint at 90% or weekly.
```
