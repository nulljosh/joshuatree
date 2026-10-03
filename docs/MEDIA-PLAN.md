# 2.2: Music and Movies

One promise: you can play your own songs and your own movies on Joshua Tree, from two real apps, and they do not crash the desktop.

VERSIONS.md splits this into 2.1 (Music) and 2.2 (Video). They ship together here as 2.2.0, built in the order below. This branch stacks on `release/2.0.0` (PR #331) and retargets to `main` once 2.0 merges.

## What exists already

- Sound: the SB16 driver plays 8-bit unsigned mono PCM, 4000 to 44100 Hz, through `SYS_AUDIO` (393) with a 32KB ring and a `played` counter we can sync video to.
- Pictures: `libjt` has its own JPEG decoder for ring-3 apps.
- Files: `SYS_OPEN`, `SYS_READ`, `SYS_READDIR` (388) already work from ring 3.
- Apps: ring-3 app pattern (`user/*.c`, `kernel/ring3app.c` table, dock slot, crash isolation check).

## Slices, in order

1. Decoders as plain C in `user/libjt/media/`, tested on the Mac with clang before the kernel ever boots: WAV (8/16-bit, mono/stereo, downmix to 8-bit mono), MP3 (public-domain minimp3 class decoder), MJPEG in an AVI container (frame index, audio track).
2. Music app: library from Files, play, pause, seek, next, volume, now-playing bar, shuffle and repeat.
3. Movie app: open a clip from Files, play, pause, seek, audio-led sync (frames follow `played`, never the reverse), letterbox, fullscreen.
4. Samantha tools: "play something", "pause", "what's playing".
5. Landing and docs sync, release notes, screenshots.

## Test plan

- Host unit tests (`tools/checks/media-unit.sh`): golden WAV and MP3 to PCM hashes, truncated and corrupt inputs, zero-length, huge header fields, odd sample rates. Must never read out of bounds (build with `-fsanitize=address,undefined`).
- Fuzz pass: mutate each fixture a few thousand times, the decoders must return an error, never crash or hang.
- Boot checks (headless QEMU, same pattern as `sb16-check.py`): Music plays a fixture and `played` advances, pause holds it, seek lands where asked. Movie plays a 2 second clip and frame count matches the clock within one frame.
- Crash isolation: both apps join the "crash on purpose, desktop survives" check.
- Sync check: audio and video drift under 100 ms over a 30 second clip.
- QA pass before ready: full `tools/ci-local.sh`, security look at every new parser (these read untrusted files), one screenshot to match the house theme.

## Suggestions beyond the ask

- Photos viewer: the JPEG and PNG decoders are already there, so it is a small app and it completes the media set.
- Stereo and 16-bit output: SB16 can do it, the 393 syscall cannot yet. Worth a second audio op once music sounds flat.
- Captions: an `.srt` next to the movie, drawn over the picture. Tiny.
- Resume where you left off, stored per file.
- Samantha reads a movie or song title off the file name and answers questions about what is playing.
- Sample media pack on the landing demo (public-domain clips) so the live demo has something to play.

## Open decisions

- MP3 decoder licence and size: the kernel image is near its wall (see LOOP-HANDOFF), so decoder code should live in the ring-3 app, not the kernel.
- A real video codec (H.264) is out of reach for this CPU class. MJPEG first, as VERSIONS.md says.
