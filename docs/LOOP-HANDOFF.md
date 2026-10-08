# Joshua Tree loop handoff (2026-10-07, 21:10)

## What the loop is

Keep merging green PRs, then take the Pi track toward 3.0.0 (the desktop boots on a real Raspberry Pi 4, and you can type and click). After that comes the goal: Claude Code running inside Joshua Tree on the Pi, which needs HTTPS first. One agent at a time, verify every claim myself, run the full local suite before a PR goes ready, merge on GitHub green, check the live site. No stray branches, PRs, issues or stale background agents left behind.

## Where things stand (2026-10-07, 21:10)

- Main is 2.29.0. Two PRs are open. PR 459 is a draft: one slow green light blink at boot, replacing the three quick blinks. Its local CI run is going right now (log at /tmp/jt-ci-led3.log). Do not start another run and do not stop this one. PR 458 is the HTTPS plan, roadmap only, and it now also says the full BearSSL TLS client has to be vendored.
- Done on the real Pi: Wi-Fi joins and holds (the WPA2 handshake is ours), DHCP and the network clock, the 12-hour clock and three-bar icon in the menu bar, the quiet console, and the boot screen. The clock read 3:14 PM on the real screen.
- The repo's BearSSL is only five crypto files. Until the handshake, certificate and record code is vendored from upstream, no secure page loads on the Pi.
- Bench: deferred, because CI is running. Rerun /jt-bench once the Mac is quiet.
- Weekly usage is at 93 percent, so big work waits for the Saturday reset.
- The main checkout at ~/Documents/Code/joshuatree sits on branch pi-flash-script. Work from a worktree under /tmp, never a path containing "samantha", and never switch branches in a worktree while CI runs from it.
- Joshua swaps the SD card by hand. Batch board tests and ask for one swap per round. Never pull the card mid-flash. Never print Wi-Fi secrets.

## Next, in order

1. Local CI on PR 459 finishes. If it is green, ready it and merge on GitHub green. Then merge 458.
2. HTTPS on the Pi: vendor the rest of BearSSL's TLS client (handshake, certificates, records) from upstream, then a test that fetches one secure page. Only then can a browser load secure pages.
3. Claude in the Console: type a question at the Pi's console and the answer prints, through the relay. Wi-Fi is in place, so this needs only the relay setup.
4. Track A on the Pi: the real desktop and dock in five slices, a wired USB mouse, sound from the 3.5 mm jack with a boot chime, then the ring-3 apps for ARM, Notes and Clock first.
5. Rename Burrow to Drawer, in its own change, after the open PRs are in.
6. Admin and sudo, SD card writes, then the Claude app on ARM.

## Restart prompt

```
/loop Joshua Tree loop. State in ~/Documents/Code/joshuatree/docs/LOOP-HANDOFF.md on origin/roadmap-https-plan (PR 458) and draft PR 459. First: check the local CI log at /tmp/jt-ci-led3.log for PR 459. If it passed, ready it and merge on GitHub green, then merge 458. If it is still running, wait for it and do not start another. Then vendor the full BearSSL TLS client and add a test that fetches one secure page on the Pi, then Claude in the Console through the relay. Ship visible fixes as their own tiny PR. One Haiku agent at a time, verify every claim yourself, kill by PID only. Joshua swaps the SD card by hand, so batch board tests and ask for one swap per round. Never pull the card mid-flash. Never print Wi-Fi secrets. Stop and checkpoint at 95 percent usage.
```
