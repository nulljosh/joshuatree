Gate: check the sidebar for a project-post day or flair before posting. Text post with one photo of the Pi running it if Joshua has one.

Title: I wrote an operating system from scratch and it now runs bare metal on my Pi 4

Body:
No Linux underneath: the Pi boots straight into my own kernel (ARM64, written in C) from a FAT card. It has a desktop with a dock, a terminal, Notes, Calendar, Weather and a few games, and as of this week user accounts, a login screen and sudo.

Flashing is one script, tools/flash-pi.sh, onto any SD card, then boot. A USB mic for talking to the built-in assistant is the thing I'm testing on the board right now.

It also runs in a browser tab if you want to look before flashing: https://joshuatree.jaybulb.com. Source: https://github.com/nulljosh/joshuatree.

What would you want a tiny Pi OS like this to do that Raspberry Pi OS doesn't?
