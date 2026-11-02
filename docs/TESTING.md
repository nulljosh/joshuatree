# Board testing

What to try on the real Pi, most important first. Flash the card first (card in the Mac, `tools/flash-pi.sh`): it carries main, 10.0.0 or later. Stop at the first failure and send the step number with a photo. The long version of each step, with the lines to expect, is [7.0 board day](GUIDE.md#70-board-day).

Done on the board on 2026-11-01 and 11-02 (8.0.0 to 8.9.0): the flat boot with the tree glyph; pairing the mouse from Settings, and it comes back after a restart with a wiggle; the restart dialog; Notes kept through a restart; `cores` says 4 of 4; the blue wallpaper and the Picasso line. A pink speckle on light colours was a loose HDMI plug. The crackle on restart is the Pi's own boot stage: muted, with no chime, it still crackles (Joshua's video), so the fix is a restart that skips the firmware, not the sound driver.

1. **10.0 on the monitor.** Boot: the tree, then the desktop. `mem` says how many heap pages are in RAM (0 sent to the card is normal); `cores` lists cores 1 to 3 with their app slices. Settings, About says 10.0.0.
2. **The screen reader.** Cmd+F5, or `reader on` in the Terminal, turns it on: the menu bar says Screen reader on. Press Left and Right along the dock. Each tile is captioned over the dock, and once you stop for 600 ms Samantha says it, if the relay is reachable.
3. **Language.** Settings, Up to Language, Right to Español, then restart. It is still Spanish after the boot. Apps and the Terminal stay English.
4. **A touch screen, if one is here.** Plug the panel's HDMI and its USB into the Pi: the desktop fills it at its own size, and a tap on a dock tile opens that app.
5. **The park wallpaper.** Boot: Joshua Tree at dusk, dithered, with fine scanlines, sharp at 1080p. Drag a window across it: what it leaves behind is the picture again, no blocks of flat colour.
6. **The lag.** Drag a window with another one open, and sweep the pointer along the dock. Compare with last night: cores 1 to 3 now sleep, so it should feel lighter. `cores` run twice a minute apart: each count goes up by hundreds, not millions.
7. **Dock zoom is off.** Sweep the pointer along the dock: no tile grows. Settings, Dock, Magnify: Small turns it on; put it back to Off.
8. **Settings fits its window.** Every section at 1080p: nothing past the right edge or under the buttons; Display lists Park as a wallpaper.
9. **USB stick.** A FAT32 stick in a USB port: `stick ls` lists it, `stick cat NAME` shows a text file, `stick copy NAME` puts it in JT/ and `cat jt/NAME` reads it back.
10. **The Berkeley list.** `sched` says the longest turn is under 60 ms after a minute of normal use. `userrun hello | userrun hello` runs without a hang.
11. **Wi-Fi.** Settings, Wi-Fi: R looks again, pick the phone's hotspot, type its password: "Joined." in about 15 seconds. Switch back to home.
12. **Web.** `browse en.wikipedia.org/wiki/Raspberry_Pi`, then the same page in Blackcomb. Text first, then the pictures one by one.
13. **Mail and Calendar.** `mail login imap.mail.me.com YOUR-APPLE-ID` with an app password, `mail`, `mail 1`. `calendar caldav https://caldav.icloud.com/ YOUR-APPLE-ID`, add a day, `calendar sync`: it shows on the Mac.
14. **Write a program on the Pi.** `sudo userrun plankc build CODE.PK`, then `sudo userrun code HI.PK`; type a `main` that prints hi, Esc to save, build it, `userrun hi` prints hi.
15. **Bluetooth keyboard.** Settings, Bluetooth, S, its number, type the six digits it shows: it types in the Terminal.
16. **Files.** The dock's second tile says Files, and the login, sign-out and shutdown screens show the park picture, not black.

Steps 1 to 3 are 10.0, 4 is 9.0's touch screen; 5 to 9 are last night's fixes.
