# Testing Joshua Tree on a real Pi 4

This is the v3 board pass. Ten minutes, one card, one mouse. Every step says what you should see and what to send back if you don't.

You need: the flashed card (`tools/flash-pi.sh`), a Pi 4 with HDMI and a USB keyboard, your Bluetooth mouse (charged, and unpaired from any other device), and the Mac on the same Wi-Fi with the relay running (`python3 tools/claude-relay/relay.py --lan --turing ...`; see [RELAY-TLS.md](RELAY-TLS.md)).

Power the Pi from a clean state and work down the list. Stop at the first failure and send me that step's line.

## 1. Boot

The tree and a progress bar, then the desktop at 1080p with the menu bar and dock. The console stays quiet.

If it stays black or on the splash: send a photo of the screen and the time it took.

## 2. Keyboard

Open Spotlight with Cmd+Space (or F1). Type `term` and press Enter: the Terminal opens. Hold a letter: it repeats after about half a second. Press Caps Lock: capitals.

If keys do nothing: unplug and replug the keyboard, then tell me what it is.

## 3. Sound

In the Terminal: `beep`. A short tone from the 3.5 mm jack (headphones in). Then `vol` (shows 70), `vol 20` (quieter), `vol up`, `vol down`. F11 and F12 step the volume too.

If silence: send the line starting with `audio:` if one printed.

## 4. Bluetooth mouse

In the Terminal: `bt`. It prints where the radio is and its version. Put the mouse in pairing mode (hold its pairing button until it blinks), then `bt scan`: a 5 second scan that lists what it hears, your mouse by name.

Then `bt pair` and the mouse's number from the list. The arrow should move with the mouse, and a click should focus a window.

If the scan lists nothing: send the lines `bt` and `bt scan` print. If it lists the mouse but pairing fails: send the one plain line that `bt pair` ends with. This is the step that has only run against a fake radio so far, so a failure here is the most likely thing to find.

If Bluetooth can't be saved today, plug in a USB mouse: that path already works.

## 5. Apps in user mode

Click the dock's Calendar: it opens in its own window and shows the right month. Click a window to focus it, type, and the keys go only to that window. Close it from the Terminal or the window and the desktop carries on.

If an app crashes the desktop, send what the screen says: the whole point of user mode is that it shouldn't.

## 6. The card keeps your files

Open Notes, type a few words, wait three seconds (Notes saves itself two seconds after the last key). In the Terminal: `ls` lists the card's files and `cat NAME` prints one. Now pull the power, boot again, and open Notes: your words are still there.

If the note is gone: send me `ls` before and after.

## 7. Music

Put an MP3 you own in the card's `JT/MUSIC/` folder (Music makes the folder the first time it opens, so open it once, power off, copy the file, boot again). Open Music from Spotlight (type `music`): the song shows its real title and artist, its cover if the file has one (or a `COVER.JPG` or `FOLDER.JPG` beside it), and its length. Play it: it comes out of the jack. Drag the volume knob and the seek knob. Apple Music downloads won't play (their files are locked).

If it hangs on a file: tell me which file. If the title reads like `NEVERG~1` or the cover is missing, tell me the file name and what tool tagged it.

## 8. Samantha

Open her and ask something simple ("what is ten plus ten", "what time is it"). She answers in one line. The Pi finds the Mac's relay by itself, so there's nothing to type.

If she says she can't reach the host: check the relay is running on the Mac, then send the line she prints.

## What to send back

Each failed step's line, and a photo for anything on screen. Say which numbers worked. When it all passes, that is version 3.
