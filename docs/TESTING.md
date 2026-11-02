# Board testing

What to try on the real Pi, most important first. Flash the card first (card in the Mac, `tools/flash-pi.sh`): it carries main, 8.0.0 or later. Stop at the first failure and send the step number with a photo. The long version of each step, with the lines to expect, is [7.0 board day](GUIDE.md#70-board-day).

Done on the board on 2026-11-01 (8.0.0): the flat grey boot with the tree glyph, sharp; pairing the mouse from Settings, Bluetooth (S looks, its number pairs); the restart dialog. A pink speckle all over light colours was a loose HDMI plug, not the OS.

1. **Bluetooth after a restart.** With the mouse paired from Settings, restart: it moves the pointer again with no pairing. Then Settings, Bluetooth, F forgets it; restart, and it does not come back until you pair it again.
2. **The crackle.** Plug headphones or a speaker into the jack. Note when it crackles: at boot, at the restart chime, while Music plays, or all the time with nothing playing. Then Settings, Sound, Mute on and off. Say which; it decides where the fix goes (the PWM's idle level or the chime's start).
3. **Settings fits its window.** Every section, at 1080p: nothing runs past the right edge or under the buttons, and the Bluetooth rows show their numbers (from 8.9.0).
4. **Dock zoom.** From 8.9.0 the zoom is off. Settings, Dock, Magnify: Small, then sweep the pointer fast along the dock. The pointer and a playing song keep going; it never stalls. Then put it back to Off.
5. **Wi-Fi.** In the Terminal, `wifi` says it joined and lists networks. Then Settings, Wi-Fi: press R (it says "Looked again."), pick your phone's hotspot, type its password, Enter. "Joining..." then "Joined." in about 15 seconds, no restart. Switch back to home the same way. Pair the mouse once more: Wi-Fi comes back on its own afterwards.
6. **Web.** `browse en.wikipedia.org/wiki/Raspberry_Pi`, then the same page in Blackcomb (Spotlight, `blackcomb`). Text first, then the pictures one by one while the window still scrolls.
7. **Mail and Calendar.** `mail login imap.mail.me.com YOUR-APPLE-ID` with an app password, `mail`, `mail 1`. `calendar caldav https://caldav.icloud.com/ YOUR-APPLE-ID`, add a day, `calendar sync`: it shows on the Mac.
8. **Write a program on the Pi.** In the Terminal, with `sudo` in front of each `userrun` since the Pi has accounts: `userrun plankc build CODE.PK`, then `userrun code HI.PK`. Type `fn main() {`, Enter, `  print("hi")`, Enter, `}`, then Esc to save. `userrun plankc build HI.PK`, wait for `userrun plankc: exit 0`, then `userrun hi` prints hi.
9. **The Berkeley list (8.1 to 8.6, once it is on the card).** `cores` says 4 of 4 running, and run twice, each core's count goes up. `sched` says the longest turn is under 60 ms after a minute of normal use. `userrun hello | userrun hello` runs without a hang (the pipe).
10. **USB stick (8.9.0, once it is on the card).** A FAT32 stick in a USB port: the boot log says `usb disk`. `stick ls` lists it, `stick cat NAME` shows a text file, `stick copy NAME` puts it in JT/ and `cat jt/NAME` reads it back.
11. **Bluetooth keyboard.** Keep the wired keyboard in. Put a Bluetooth LE keyboard in pairing mode, Settings, Bluetooth, S, its number, type the six digits it shows, Enter: it types in the Terminal. Watch for a key that repeats on its own.

Steps 1 to 4 are tonight's fixes. Step 9 and 10 wait for those releases to be on the card.
