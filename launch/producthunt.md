Post: https://www.producthunt.com/products/joshua-tree

# Product Hunt: Joshua Tree (update, 4.10)

Name: Joshua Tree

Tagline: A whole computer built from scratch, now on a Raspberry Pi

Description: An operating system written from nothing in C: its own kernel, desktop, network and 32 apps. Boot it in a browser tab or flash it onto a Raspberry Pi 4, sign in with your own account, and talk to Samantha, the assistant built in. Free and open source.

Topics: Open Source, Developer Tools, Raspberry Pi

Pricing: Free. Apache 2.0, nothing to buy.

Links: joshuatree.jaybulb.com, github.com/nulljosh/joshuatree

Gallery (kept off the repo, on the Mac): ~/Documents/Code/joshuatree-launch-gallery/01-desktop.jpg first, then 02-weather, 03-terminal, 04-hamurapi.

## First comment (update)

When I launched Joshua Tree here it lived in a browser tab: a small operating system I wrote from the first line of the bootloader up, with its own windows, dock, sound, internet and apps, no Linux underneath and no libc.

Since then it moved onto real hardware. You can flash it onto a Raspberry Pi 4 with one script and it boots straight to the desktop. This week the Pi got what a computer you share needs: accounts with salted, slow password hashes, a login screen, admins and sudo. A home folder for each person, snapshots of it and a card that survives a pulled plug are next.

Samantha, the assistant from my Turing project, runs on it too. Hold F2, ask her something, and she answers out loud. Talking to her through a USB mic on the Pi itself is being tested on the real board right now.

I built it to learn how a computer actually works, all the way down, and to see how far one person can take that. It is free and Apache 2.0. Every file is on GitHub, and every change runs a test suite in an emulator before it lands. Try it in your browser, or flash a card and tell me what breaks.
