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
17. **Deep idle.** Leave it alone five minutes: the screen goes dark. Touch a key: it is back at once, and the Pi runs cooler while dark (`sched`, run just after, still under 60 ms).
18. **Trackpad, if one is here.** Plug a USB precision touchpad in: the UART says `usb trackpad`. One finger moves the pointer, two scroll a page in Blackcomb, a tap clicks and a two-finger tap right-clicks. If it says `input mode refused, left a mouse`, send the pad's name: it still points, as a mouse.
19. **Battery, if a gauge board is here.** A UPS or laptop board with a MAX17048 at I2C 0x36 on the HAT pins (Geekworm style): the menu bar shows its percent and a battery where the power symbol was; `battery: N percent` on the UART at boot. No board: the power symbol, as before.
20. **Sleep.** Click the tree, then Sleep: the screen goes dark a second later. A key brings it back.
21. **Spaces.** Open Calendar, then Ctrl+Right: an empty desktop, and the menu bar says Desktop 2. Open Notes there, Ctrl+Left: Calendar is back alone. Notes' dock tile takes you to Desktop 2.
22. **Mission Control.** With two apps open, Ctrl+Up: the desktops along the top and both windows side by side. Enter or a click brings one up; a desktop chip goes there; Esc closes it.
23. **Glass.** Settings, Display, Tab to Glass, Right twice: the menu bar turns milkier (Tinted). Left four times: Clear, the wallpaper shows through. It stays after a restart.
24. **Shortcuts.** In the Terminal, `shortcut loud = vol 30; vol`, then `shortcut loud`: each command shows, then runs. `sam make a shortcut called night that sets volume 10`, then `shortcut night`.
25. **Tab groups.** In the Terminal, `browse en.wikipedia.org`, `tab new`, `browse duckduckgo.com`, then `tab group save work`. `tab close`, then `tab group work`: both tabs come back. Restart and `tab group work` again.
26. **Photos.** Put two `.JPG` files in a `PHOTOS` folder on the card. Spotlight, Photos: both show in the grid. Enter, then r, b and +: it turns, goes black and white and lighter. Restart: the edits are still there, and 0 puts it back.
27. **YouTube.** With `tools/claude-relay/yt.py --lan ...` running on the Mac, Blackcomb, `youtube.com/watch?v=QW29Ly55Kvs`, Enter, Tab, Enter: Drake's Plot Twist plays in the page, the whole 3:16, picture and sound. Space pauses and plays on. Stop `yt.py` on the Mac afterwards.
28. **Parental controls.** As an admin, `adduser kid kidpass`, then `sudo limits kid apps notes,calculator` and `sudo limits kid web off`. Log out and sign in as kid: Notes opens, Mines says it is not allowed in the menu bar, Blackcomb does not open.
29. **Bluetooth headphones and keyboards.** Put headphones in pairing mode. In the Terminal, `bt find`: they show by name as headphones. `bt connect 1`: `ready to play`. Open Music and play a song: it comes out of the headphones, with no stutter (if it stutters, note it: the UART's speed is the suspect). Then a Bluetooth keyboard in pairing mode, `bt find`, `bt connect N`: `ready`, and it types in the Terminal.
30. **The GPU.** In the Terminal, `gpu`: it says the reset is lifted, the V3D clock in MHz, that it is reading the ident, then `gpu: V3D 4.2, 1 core, MMU, ...` and `gpu: ready`. Any other line (refused, no bridge, did not answer, did not identify), send it word for word. If it freezes, send the last line on the screen: it names the step. Then drag a window: the desktop still works.
31. **Heat and the fan.** Fan's red wire on pin 4, black on pin 6 (the outer row, 2nd and 3rd pins from the end away from the USB ports): it spins at power on. In the Terminal, `temp`: around 40 to 60 C with the fan, and it says the chip slows down at 85.0 C.
32. **Energy Saver.** Settings, Display, Screen saver after: set 1 minute. Leave it: the time appears small on black and jumps every 10 s; five minutes later the screen goes dark. Move the mouse: the desktop comes back as it was. Restart: the 1 minute is still there.

Steps 1 to 3 are 10.0, 4 is 9.0's touch screen; 5 to 9 are last night's fixes.
