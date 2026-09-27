# Joshua Tree loop handoff (2026-09-27, evening)

## What the loop is

Build toward 2.0, one PR at a time, watching usage. Merge on green CI, never ask. Haiku subagents 15 min max for code, max two at once; CI waits go to subagents not background. Fix CI, never mute. Main session merges, picks next. A+ bar always. No LOC metrics public.

## Where things stand

Main is 1.6.6: kernel downloads once gzipped (landing loads twice as fast). Four-arm Joshua tree logo. Sound Blaster 16 driver. Chat window titled Samantha, speaking her replies aloud. PR #226 had Samantha's face filling Chat and animating while she talks, but crashed Linux CI only (Files, Mail, Weather all failed to open; PNG decode or heap sizing bug in kernel/chat_face.h not reproducible on macOS). Face feature reverted, but sound driver and Chat audio shipping. Auto-merge is armed. 44 stale branches cleaned; pre-push hook checks the pushed tree.

## Next, in order

1. Debug the Linux-only crash in kernel/chat_face.h (PNG frame fetch/decode path), then reattempt Samantha's face in Chat.
2. Sharper face frames (240px source, 16 talk frames).
3. Inline chat bar instead of prompt popup.
4. Terminal moved to far right of dock.
5. Samantha's idle loop on landing page.
6. Bluetooth (parked, needs real hardware).

## Restart prompt

```
/loop debug the Linux-only crash in kernel/chat_face.h (PR 226's face feature was reverted after crashing Files/Mail/Weather in CI), then reattempt Samantha's face in Chat, one PR at a time, watching Claude usage; hard stop at 90% session usage.
```
