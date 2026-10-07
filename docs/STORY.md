# Our story

## Who we are

One person in Langley, BC, and Claude.

Joshua decides what gets built, what it should feel like, and when it's good enough. Claude writes the code, runs the tests and keeps the notes. Neither of us could have built this alone.

## What we built

Joshua Tree is an operating system, written from nothing.

No Linux underneath. No libc. Every line that runs the machine is ours: the kernel, the drivers, the windows, the dock, the fonts, the apps. It's free, under Apache 2.0, and anyone can read every line.

You can try it without installing anything. The landing page boots the real thing in your browser.

## How it went

The first commit was August 31, 2026. A kernel that printed text and read a keyboard.

Five weeks and over 800 commits later, it has a desktop, a dock, a file system, sound, a network stack, and its own versions of Joshua's apps, each running in its own protected space so one crash can't take the machine down. Samantha, the assistant from Turing, talks through it with her own face and voice.

On October 6, 2026, it booted on a real computer for the first time. A Raspberry Pi 4 on a desk, plugged into a Samsung monitor, drew the Joshua Tree desktop. The first try showed a black screen, because the SD card wasn't seated. The second try worked.

## How we work

Small steps. Each change gets a test that fails without it and passes with it. Nothing ships without the checks going green. When something breaks, we measure before we guess: a photo of the screen, a log line, a pixel count.

The roadmap is public. So are the mistakes. The log in [RASPBERRY-PI.md](RASPBERRY-PI.md) says exactly what went wrong on the real board and what we did about it.

## Where it's going

The OS stays free. The money is in hardware: a small kit with Joshua Tree already on it, starting with the Pi. Further out, a box you talk to, running nothing but Joshua Tree and Samantha.

Next up: a keyboard and mouse on the real Pi, then the full desktop, then the apps. The plan is in [roadmap.md](roadmap.md). The money is in [MONEY.md](../MONEY.md).
