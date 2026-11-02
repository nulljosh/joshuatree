# Board testing

What to try on the real Pi, most important first. Flash the card first (card in the Mac, `tools/flash-pi.sh`): it carries main, 8.9.0 or later. Stop at the first failure and send the step number with a photo. The long version of each step, with the lines to expect, is [7.0 board day](GUIDE.md#70-board-day).

Done on the board on 2026-11-01 and 11-02 (8.0.0 to 8.9.0): the flat boot with the tree glyph; pairing the mouse from Settings, and it comes back after a restart with a wiggle; the restart dialog; Notes kept through a restart; `cores` says 4 of 4; the blue wallpaper and the Picasso line. A pink speckle on light colours was a loose HDMI plug. The crackle on restart is the Pi's own boot stage: muted, with no chime, it still crackles (Joshua's video), so the fix is a restart that skips the firmware, not the sound driver.

1. **The park wallpaper.** Boot: Joshua Tree at dusk, dithered, with fine scanlines, sharp at 1080p. Drag a window across it: what it leaves behind is the picture again, no blocks of flat colour.
2. **The lag.** Drag a window with another one open, and sweep the pointer along the dock. Compare with last night: cores 1 to 3 now sleep, so it should feel lighter. `cores` run twice a minute apart: each count goes up by hundreds, not millions.
3. **Dock zoom is off.** Sweep the pointer along the dock: no tile grows. Settings, Dock, Magnify: Small turns it on; put it back to Off.
4. **Settings fits its window.** Every section at 1080p: nothing past the right edge or under the buttons; Display lists Park as a wallpaper.
5. **USB stick.** A FAT32 stick in a USB port: `stick ls` lists it, `stick cat NAME` shows a text file, `stick copy NAME` puts it in JT/ and `cat jt/NAME` reads it back.
6. **The Berkeley list.** `sched` says the longest turn is under 60 ms after a minute of normal use. `userrun hello | userrun hello` runs without a hang.
7. **Wi-Fi.** Settings, Wi-Fi: R looks again, pick the phone's hotspot, type its password: "Joined." in about 15 seconds. Switch back to home.
8. **Web.** `browse en.wikipedia.org/wiki/Raspberry_Pi`, then the same page in Blackcomb. Text first, then the pictures one by one.
9. **Mail and Calendar.** `mail login imap.mail.me.com YOUR-APPLE-ID` with an app password, `mail`, `mail 1`. `calendar caldav https://caldav.icloud.com/ YOUR-APPLE-ID`, add a day, `calendar sync`: it shows on the Mac.
10. **Write a program on the Pi.** `sudo userrun plankc build CODE.PK`, then `sudo userrun code HI.PK`; type a `main` that prints hi, Esc to save, build it, `userrun hi` prints hi.
11. **Bluetooth keyboard.** Settings, Bluetooth, S, its number, type the six digits it shows: it types in the Terminal.

Steps 1 to 5 are tonight's fixes.
