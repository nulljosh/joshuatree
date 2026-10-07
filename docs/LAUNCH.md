# Launch drafts

Not posted. Drafts for Joshua to edit and send. Every claim here was true on 2026-10-07; check the "hold until" lines before posting.

## Show HN

**Title:** Show HN: I built an operating system from scratch, and it runs in your browser

**Body:**

I'm one person in Langley, BC, working with Claude. Five weeks ago I wrote a kernel that printed text and read a keyboard. Now it has a desktop, a dock, a file system, sound, a network stack and 26 apps, each in its own protected space. No Linux underneath, no libc. Every line is ours and it's Apache 2.0.

You can boot the real thing in your browser, no install: joshuatree.heyitsmejosh.com

On October 6 it booted on a real Raspberry Pi 4 for the first time. The first try was a black screen because the SD card wasn't seated. The log of what went wrong on the real board is public, mistakes included.

The OS stays free. The plan is a small kit with it already on a Pi, for people who like building computers. There's a waitlist on the page.

Happy to answer questions about the hard parts: the USB stack, the framebuffer cache bug that made the screen half size, and how the test harness works.

**Hold until:** the mouse works on the real Pi, so the post can say you can type and click on it. Today it has a keyboard and no mouse, and Wi-Fi is not yet confirmed on the board.

## 30 second video

1. (0 to 5 s) The Pi on the desk, the screen black, power goes in. The Joshua Tree logo.
2. (5 to 12 s) The desktop appears: wallpaper, menu bar, dock. Type a line on the keyboard.
3. (12 to 20 s) Move the mouse, hover a dock tile, open an app. **Needs the mouse.**
4. (20 to 26 s) Samantha answers a spoken question. Use the browser demo if the Pi has no sound yet.
5. (26 to 30 s) "Written from nothing. Free. joshuatree.heyitsmejosh.com"

Voice: Joshua's own, plain. No music from anyone else. No claims beyond what is on screen.

## Where to post, in order

1. Hacker News (Show HN), a weekday morning Pacific time.
2. r/osdev and r/programming, with the video.
3. The osdev forums, with a write-up of the USB and framebuffer bugs. These are the first kit buyers.
4. One short thread on X with the video.
