# Board testing

What to try on the real Pi, most important first. The card carries 6.89.0. Stop at the first failure and send the step number with a photo. The long version of each step, with the lines to expect, is [7.0 board day](GUIDE.md#70-board-day).

1. **Wi-Fi.** In the Terminal, `wifi` says it joined and lists networks. Then Settings, Wi-Fi: press R (it says "Looked again."), pick your phone's hotspot, type its password, Enter. "Joining..." then "Joined." in about 15 seconds, no restart. Switch back to home the same way.
2. **Web.** `browse en.wikipedia.org/wiki/Raspberry_Pi`, then the same page in Blackcomb (Spotlight, `blackcomb`). Text first, then the pictures one by one while the window still scrolls. Try a news site and a page with a form.
3. **Mail.** `mail login imap.mail.me.com YOUR-APPLE-ID` with an app password (appleid.apple.com, App-Specific Passwords). `mail`, `mail 1`, the Mail tile, then reply to yourself with two lines (Shift+Enter between them).
4. **Calendar.** Make a "JT test" calendar on the Mac first. `calendar caldav https://caldav.icloud.com/ YOUR-APPLE-ID`, `calendar use N`, add a day and `calendar sync`: it shows on the Mac. Delete it on the Mac, open Calendar on the Pi: gone. Then close Calendar, add a day on the Mac, leave the Pi alone for a minute: it is there when you open Calendar.
5. **Bluetooth keyboard.** Keep the wired keyboard plugged in for this one. Put the Bluetooth keyboard in pairing mode, `bt scan`, then `bt pair NAME`. Type the six digits it shows on the Bluetooth keyboard, then Enter: "bt: keyboard ready", and it types in the Terminal. It must be a Bluetooth LE keyboard (Logitech's are); Apple's Magic Keyboard is not supported. Pairing it replaces a paired Bluetooth mouse.
6. **Sign in.** Time from Enter at the login screen to the desktop, in seconds.
7. **Tabs and history.** In Blackcomb, Cmd+T shows a tab strip; two letters of a visited page in the address bar offer it; Top Sites shows a site after two visits.

When all seven pass, 7.0.0 is cut.
