# Board testing

What to try on the real Pi, most important first. Flash the card first (`tools/flash-pi.sh`). Stop at the first failure and send the step number with a photo.

Done on the board, 2026-11-01 and 11-02 (8.0 to 8.9): the flat boot, mouse pairing from Settings, Notes kept through a restart, `cores` says 4 of 4, the blue wallpaper. A pink speckle was a loose HDMI plug. The crackle on restart is the Pi's own boot stage.

Bench first. On the Mac, run the relay with `--bench-dir ~/.cache/joshuatree/bench`. On the Pi, type `bench` (it says `bench: on`). From then on the Mac can run Terminal steps (`tools/pi-bench.py gpu`), press keys and click (`tools/pi-bench.py click 400 300`) and take a screenshot (`tools/pi-bench.py shot`). `bench` again turns it off.

1. **10.0 on the monitor.** Boot to the tree, then the desktop. `mem` shows heap pages; `cores` lists cores 1 to 3. Settings, About says 10.0.0.
2. **Screen reader.** Cmd+F5 or `reader on`. Left and Right along the dock are captioned and spoken.
3. **Language.** Settings, Language, Español, restart. It stays Spanish.
4. **Touch screen, if here.** Plug the panel's HDMI and USB in. The desktop fills it, and a tap opens a dock tile.
5. **Park wallpaper.** Joshua Tree at dusk, sharp at 1080p. Drag a window across it: no flat blocks are left behind.
6. **Lag.** Drag a window with another open, sweep the dock. `cores` run twice a minute apart should grow by hundreds, not millions.
7. **Dock zoom is off.** Settings, Dock, Magnify: Small turns it on. Put it back to Off.
8. **Settings fits its window** at 1080p. Display lists Park.
9. **USB stick.** `stick ls`, `stick cat NAME`, `stick copy NAME`, then `cat jt/NAME`.
10. **Berkeley list.** `sched` says the longest turn is under 60 ms. `userrun hello | userrun hello` does not hang.
11. **Wi-Fi.** Settings, Wi-Fi: R, pick the phone hotspot, type its password. "Joined." in about 15 seconds. Switch back.
12. **Web.** `browse en.wikipedia.org/wiki/Raspberry_Pi`, then the same page in Blackcomb.
13. **Mail and Calendar.** `mail login imap.mail.me.com YOUR-APPLE-ID`, `mail`, `mail 1`. `calendar caldav https://caldav.icloud.com/ YOUR-APPLE-ID`, add a day, `calendar sync`.
14. **Write a program on the Pi.** `sudo userrun plankc build CODE.PK`, then `sudo userrun code HI.PK`. Save, build, `userrun hi` prints hi.
15. **Bluetooth keyboard.** Settings, Bluetooth, pair, type the six digits.
16. **Files and screens.** The second dock tile says Files. Login, sign-out and shutdown show the park picture, not black.
17. **Deep idle.** Leave it five minutes: the screen goes dark. A key brings it back at once.
18. **Trackpad, if here.** One finger moves the pointer, two scroll in Blackcomb, a tap clicks. `input mode refused, left a mouse` means it still points.
19. **Battery, if a gauge board is here.** The menu bar shows its percent. `battery: N percent` on the UART at boot.
20. **Sleep.** Tree menu, Sleep. A key wakes it.
21. **Spaces.** Ctrl+Right gives an empty Desktop 2. Ctrl+Left returns.
22. **Mission Control.** Ctrl+Up shows the desktops and windows. Esc closes it.
23. **Glass.** Settings, Display, Glass. It stays after a restart.
24. **Shortcuts.** `shortcut loud = vol 30; vol`, then `shortcut loud`.
25. **Tab groups.** `tab new`, then `tab group save work`. Close the tab, then `tab group work` brings both back.
26. **Photos.** Two `.JPG` files in a `PHOTOS` folder show in the grid. r, b and + edit. Edits survive a restart. 0 undoes them.
27. **YouTube.** With `yt.py --lan` running on the Mac, Blackcomb plays `youtube.com/watch?v=QW29Ly55Kvs` in full, with sound.
28. **Parental controls.** `adduser kid kidpass`, `sudo limits kid apps notes,calculator`, `sudo limits kid web off`. Signed in as kid, Mines is refused.
29. **Bluetooth headphones and keyboard.** `bt find`, `bt connect 1`, then play a song. Then a keyboard: `bt connect N`.
30. **GPU.** `gpu` ends with `gpu: ready`. `gpu clear` paints a 64x64 tile. `gpu fill` (13.20.0) paints a terracotta panel in the middle of the screen. `gpu copy` copies the top-left third of the screen one third to the right. Any other line, send it word for word.
31. **Install from the web.** Blackcomb, `joshuatree.jaybulb.com/apps/fortune.app`, then `userrun fortune`.
32. **Heat and fan.** Fan spins at power on. `temp` reads about 40 to 60 C.
33. **Screen saver.** Settings, Display, Screen saver after 1 minute. The saver starts, a key brings the screen back.
34. **Print.** `print NOTE.TXT ADDRESS` with your printer's address, then `printers`. In Notes, Cmd+P prints.
35. **Scan.** `scan` (or `scan ADDRESS`) saves `PHOTOS/SCAN0001.JPG`, and Photos shows it.
34. **Clock after a stale card (13.9.3).** The menu bar clock sets itself within a minute. `google.com` opens in Blackcomb. `update` reaches the release.
35. **Paddle and dragging (13.9.3).** Paddle stays smooth while a window is dragged.
36. **Menu bar icons (13.9.3).** The power symbol shows Sleep, Restart and Shut Down. The clock opens Date and Time.

Steps 1 to 3 are 10.0. Step 4 is 9.0's touch screen. Steps 5 to 9 are the 8.x fixes.
38. **Update over the air (13.19.0).** On a card flashed with 13.19.0 or later: `update`, then `update install`. It restarts into the newest release; Wi-Fi, `sam` and `bench` still work.
39. **Blue by default (13.21.0).** Selections, buttons and the caret are blue; Weather is a blue sky. Settings, Display, Accent: step to Green, and Settings, Spotlight and the next app you open turn green. Restart: still green.
