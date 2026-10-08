# The Book

How Joshua Tree got from a kernel that printed text to a Raspberry Pi 4 on a desk that joins Wi-Fi and talks back. Told by Joshua, with Claude. "I" is Joshua. "We" is the two of us, and later the agents too.

Everything here is in the repo or happened on the desk. Where a number is a guess, it says so.

## 1. Why

I wanted a computer that knows what I want before I ask. You open it at nine and the mail is already sorted. You finish a note and the reminder is already drafted. One tap takes it. Ignoring it costs nothing. It never nags, and none of it leaves the machine.

That computer does not exist yet. Not from me, and not from anyone. So I started at the bottom, with the part nobody wants to write: the kernel.

The ideas under it are not mine. Steve Jobs said great artists steal, after Picasso, so we take the best ideas openly. The Mac's menu bar, the Finder, the dock. We make them ours. The other half of Jobs was that he was two people at once: the professional who shipped finished things, and the rebel who refused the way things were done. One without the other is a bureaucrat or a kid with a spray can. Joshua Tree tries to keep both. The rebel half is no Linux underneath, no libc, no vendor runtime, no telemetry, and a whole machine one person can read. The pro half is that every feature ships with a test that fails without it, the docs stay at 100 percent, and a wrong pixel is a bug.

In 1983 Jobs gave a talk at a design conference in Aspen. I rewatched it on October 8, 2026, after the Pi was already running, and most of it still fit. He said computers are simple and dumb, just very fast, and that speed and layers make them look like magic. He said Apple was putting some liberal arts into the machine: real fonts, many fonts, pictures. He said the people who build computers are closer to artists than the stereotype allows. He wanted a great computer in a book for under a thousand dollars, and a way to try software for free before you buy it, the way radio lets you hear a record first. And he said that understanding language is much harder than understanding voice, because meaning depends on what you meant. All of that is in this project somewhere. The Pi is the computer in a book. The landing page boots the real thing before you pay for anything. Samantha is the hard part he named.

A little anarchy, held together by professionalism. That is the whole pitch.

## 2. The kernel and the desktop

The first commit was August 31, 2026. It was called `os`. It was a multiboot kernel with VGA text, a PS/2 keyboard, a clock chip and a shell. One file, one evening.

For two weeks it was a sketch. Then on September 12 it got serious: a GDT, an interrupt table, a remapped PIC, a timer, a bitmap memory manager, paging, a heap. The next day, September 13, is the longest day in the log. Cooperative task switching. An ATA disk driver, verified against a real disk image. FAT16, verified against a filesystem macOS had made. Loading a flat binary off that disk and running it. A libc subset. A PCI scan that found QEMU's video card. A linear framebuffer. A mouse driver that answered with a real ACK byte. A network card driver that sent a real frame. Ethernet, ARP, UDP, DNS, TCP, an HTTP GET over a stack written from nothing. A TCP server a real `curl` could reach. Then, late in that same day, the kernel talked to a language model and got a real answer back.

Also on September 13 the project got its name. `os` became Joshua Tree. The domain moved with it.

The desktop came in the same rush. Version 11 was a mouse-driven GUI. Version 14 grew a dock to hold my other apps. Version 15 made it a real macOS-style dock with drag to reorder. The landing page stopped showing a recording and started booting the actual `kernel.elf` in the browser through v86, a JavaScript x86 emulator. What you see on the site is the kernel, not a video of it.

Then the long middle. Between September 28 and 30, every app moved out of the kernel into ring 3, its own protected box, so one crash could not take the machine down. Twenty-six apps by the time 2.0 shipped at the start of October. Notes, Mail, Calendar, Clock, Terminal, Weather, a browser of sorts, and ports of the apps I had already built for phones. Each one got a check that crashes it on purpose and proves the desktop survives.

By October 6 the log had passed a thousand commits. Most of them are small. That is the method: a small step, a test that fails without it, merge on green, next step.

I should be honest about who typed. Claude wrote most of the code. I decided what to build, what it should feel like, and when it was good enough. Neither of us could have done it alone, and the docs say so.

## 3. Samantha

Samantha is the assistant that runs the machine. She is not a chatbot in a window. She is the interface.

The split matters, so here it is plainly. Turing is a separate project of mine. It is the language model side: the server, the voices, the small model she can run locally. Samantha is the person people talk to. Turing is what she is made of. Joshua Tree is the operating system she runs the machine through.

On September 27 she got a face and a real voice through the OS's own sound driver. The kernel posts text to Turing's speech endpoint, gets audio back, plays it through the Sound Blaster driver, and moves her mouth from the sound level as it plays. The last app to leave the kernel for ring 3 was her, in 1.9.26, twenty-six of twenty-six. Her tools are small system calls: audio, a bigger HTTP call, system info, and a way to ask the desktop to open an app.

She can set reminders, read the calendar, open things. On the landing page you can type to her and she answers out loud. The portfolio mode on my own website is the same kernel with my face and voice in place of hers, which is a strange thing to have built but it works.

The north star from chapter 1 is her job. Learn the habits on the device, offer the next thing quietly, never nag. That slice is not built yet. What is built is the part where she can hear you and do something.

## 4. The board arrives

On October 6, 2026, a Raspberry Pi 4 Model B with 4 GB arrived. I had ordered it to find out whether any of this was real or whether it only worked inside an emulator.

The ARM64 port had been built blind, in QEMU's Pi 4B model, for a few days. We had a flash script that did not need Raspberry Pi OS at all: five firmware files straight from the Raspberry Pi repo, a `config.txt`, and our 151 KB `kernel8.img` on a FAT32 card.

First power-on: red light, black screen. I had put the card in after the power. Unplug, reseat, replug. The desktop came up over HDMI on a Samsung monitor. The menu bar said `Joshua Tree` and `ARM64` in DejaVu. The Console window and the dock were drawn. There is a photo of it in `docs/hardware/first-boot-2026-10-06.jpg`, and another close up of the Console, because the Console had a problem.

The boot lines inside the Console were not text. They were a thin column of marks at the left edge, one per line. Thirteen marks for thirteen lines. The title text was fine, so the font worked. Something between the kernel and the screen was eating every character after the first pixel.

The answer took the evening. The screen lives in cached memory, and the GPU only sees what the CPU cleans out of the cache. We cleaned once, after drawing the first page. Every later line stayed in the cache. The wipe between pages happened to overwrite whole cache lines and went straight to memory, but the two pixel columns just left of a line boundary kept the old text. That was the column of marks. The fix was to clean every glyph and every wipe as it is drawn. The photo had told us exactly what was wrong, once we read it right.

That night the picture filled the monitor at 1920 by 1080, sharp, with real text in the Console. Above the dock it said "Steve Jobs, 1955 to 2011. Thank you." It was fifteen years to the week.

Then USB. The driver had been written and tested in QEMU first. On the real chip it went: PCIe link up, the VL805 controller's firmware loaded through the mailbox, the xHCI controller started with five ports, a hub found on one of them. Then `usb ready: 0 kbd, 0 mouse`. The keyboard cable was a charge-only cable with no data wires. I swapped it. The NuPhy keyboard enumerated behind the hub, and every key I pressed printed on the screen. A USB keyboard worked on the real Pi, the same day it arrived.

The Pi also switched off once mid-session. The USB-C plug had worked loose. Not every bug is software.

## 5. Wi-Fi by hand

The Pi 4's radio is a Broadcom chip, the CYW43455, driven over SDIO. Getting it to say anything at all took most of a day on October 7 and a stack of photographs, because there was no serial cable yet. Each build went on the card, the card went in the Pi, the Pi printed how far it got, and I took a picture.

The failure lines taught us the chip. Holding its ARM core in reset froze its own memory. Its RAM was 32 KB smaller than the datasheet suggested. The core addresses we had guessed were wrong and had to be read from the chip itself. Frames over 512 bytes needed block mode. A command buffer of 1024 bytes total cut every chunk of the regulatory blob short. The firmware upload, 595 KB of it, went down in 512 byte pieces. By the end of the day the chip could scan and list the networks in the house.

Joining was the hard part, and for an odd reason. On a laptop, the Wi-Fi chip does not log in to a protected network. A program on the host does, called a supplicant. On a Pi running Linux that program is `wpa_supplicant`. We have no Linux. So we asked the chip whether it could do it itself, and it answered -23, unsupported.

So we wrote the WPA2 handshake. SHA-1. HMAC over SHA-1. The pseudo random function that turns the shared secret and both sides' nonces into the pairwise keys. AES key unwrap, from RFC 3394, for the group key. The master key itself comes from the passphrase through 4096 rounds of PBKDF2, and we do that at build time on the Mac so the passphrase never reaches the kernel. It is plain C in one header, `arch/arm64/wpa.h`, with no state and no allocation, and a test that checks it against the RFC's own vectors.

The math was right on the first try. The router still said no.

The four-way handshake is message 1 from the router, message 2 from us, message 3 from the router, message 4 from us. The router took our message 1 and refused our message 2, every time, with no reason given. We tried four variants of message 2 and printed which one it accepted. None. We read the chip's transmit counters to see whether the frame had even left. It had.

The detail that cost the most time was this. Message 2 has to carry the security element, the RSN information element, exactly as it was sent in our association request. Not a correct one. The same bytes. We had been building a clean one. The chip, when it associated, had sent its own, and the router was comparing them byte for byte. Once we read back what the chip had actually sent and repeated it in our reply, the router answered with message 3, we sent message 4, and the keys went into the chip.

The Pi joined the network on October 7, 2026. The log line is quiet about it. The commit says "WPA2, Shaw".

Then came a bad stretch of refused joins and no address. Nothing in the code was at fault that we could find. The router got rebooted. It cleared. I am recording that because it is true, not because it is satisfying.

## 6. The clock and the quiet

A Pi has no battery clock. Until something tells it the time, it does not know the date. The menu bar said `--:--`, and the Calendar tile in the dock showed a red dash where the month goes and a dark dash for the day. We decided early it would never show a made-up date, and a check fails if that tile ever goes blank.

With Wi-Fi up, the rest of the stack could be shared with the x86 side. DHCP leased an address. For the time, we did the simplest thing that works: an HTTP request to a web server on port 80, no TLS needed, and read the `Date:` line every server sends in UTC. Seconds are good enough for a menu bar. From that we show a 12-hour Pacific clock.

The clock's DNS lookup had a bug worth telling. If the lookup was lost, the code waited 20 seconds before trying again. A Pi that takes 20 seconds to notice a dropped packet feels broken. Each try now waits 2 seconds and retries six times, and if DNS still fails it falls back to the gateway.

The next change was taste. The console had been printing every step: `wifi` lines, `usb` lines, mailbox lines, firmware chunk counts. That was right while we were reading photos. It was wrong for a computer. So the console now stays quiet on a good boot. Lines appear only when something breaks. Everything still goes to the serial line for whoever is debugging. The rule from `docs/SOUL.md` is that the boot log shows only what matters, and chatter goes to serial.

Then a boot screen: the tree, and a progress bar while the wallpaper photo decodes. A small apple beside the Jobs line. A Wi-Fi icon in the menu bar as the classic fan of waves, drawn in the clock's ink rather than the accent colour, because it should be seen and not noticed.

Those are the first sixty seconds. They have to be beautiful before anything else is allowed on screen.

## 7. The night Samantha spoke on the Pi

The relay came first. It is a small Python server on my Mac, `tools/claude-relay/relay.py`. The Pi sends a question over plain HTTP on the home network with a shared token. The relay answers. One request at a time, a hard timeout, a size cap, and a check script that proves those rules hold.

The relay had been answering through Claude Code on my plan. On October 7 it got a second mode: the Claude Messages API, paid from a monthly credit on my own account, not the plan. In that mode the relay is Samantha. It picks the model by the question. A short, easy question goes to the cheap model. A long one, or one that asks for real work, goes to the stronger one. Design, proofs and security questions go to the strongest. Nobody picks models by hand, including me.

She got two read-only file tools, both scoped to one shared folder on the Mac. And the Pi started sending its own status with every question: its IP, its clock, its Wi-Fi signal. I asked her my IP and how strong the Wi-Fi was, and she gave me the Pi's real address and its real signal.

That night an agent, on its own branch, wrote the first Pi-side actions. Samantha's answer can carry `[[note TEXT]]`, and the Pi prints the note on the console. It can carry `[[led blink]]`, and the Pi blinks the green light. Four slow blinks, because the first version blinked too fast to see. Then `[[bench]]`, which runs the benchmark.

So on October 8, 2026, I asked Samantha to run the benchmark on the board, and these were the numbers. `memcpy` at 1109 MB/s, copying a 256 KB block 64 times. One allocation plus one free at 30 ns per operation, averaged over 20,000. Desktop up at 0.2 seconds from power-on. Wi-Fi joined at 6.4 seconds, including the firmware upload and the handshake. Clock set from the network at 8.7 seconds, including the DHCP lease and the HTTP request. Those are the first benchmark numbers from a real board, and they are in `docs/BENCHMARKS.md` next to the QEMU ones.

A computer I wrote from nothing, on a board that costs less than a dinner, joined my network and told me the time and then measured itself when I asked it to. That is the night the project stopped being a demo for me.

## 8. Two bugs worth a story

The green light. The Pi has a green activity LED, and I wanted it to blink at boot so there was a sign of life before the screen came up. The first version asked the firmware to do it through the mailbox. That is how it works on a Pi 3, where the light hangs off the GPU side. It did nothing on a Pi 4. On a Pi 4 the light is wired straight to a pin on the main chip, GPIO 42, active high. The device tree says so in one line. So the code now drives the pin itself through the GPIO registers.

But the blink had done something worse than nothing. It killed the keyboard. The boot blink waited on the kernel's `ticks` counter. The timer on the ARM side stops after three ticks on purpose, so a core asleep in `wfi` can only be woken by a device. A wait loop that watches `ticks` for a value it will never reach waits forever, and USB polling sits behind it. I turned the blink off until we understood it. Samantha's blink now waits on the hardware counter, the generic timer that never stops, and the keyboard stays alive.

The second bug was not in the kernel at all. We had a helper script for the build loop named `queue.py`. Python has its own standard module named `queue`. Python looks in the script's own directory first when it imports, so every program that ran from that folder and said `import queue` got our file instead of the standard one. Every agent crashed on start, before doing anything, with an error that pointed nowhere near the cause. The fix was a rename. The lesson is older than this project: do not name your file after the library you are about to import. We knew that. We did it anyway.

## 9. Building while asleep

By the second week of October there were more tasks than hours. So the API credit started running agents overnight.

The shape is strict, and strict is the point. Each agent gets one task from the roadmap and its own branch. It has a hard spending cap per run, five dollars when it started, and it cannot push. It edits, builds, and runs the checks on the Mac. In the morning there is a branch and a result. A person reviews and merges, with `gh pr merge --auto --squash`, only once the pull request is ready and the local run is green. Nobody force-pushes main. I tried `--admin` once, to push a pull request past its checks, and the permission check stopped it. It was right. The rule is written in `docs/LOOP-HANDOFF.md` and it stays human on purpose.

The first overnight run built Notes and Clock for the Pi, a mouse check, and the HTTPS wiring that stays off until it can be tested on the board. That was more than I expected and less than it sounds, since none of it had met the real hardware yet.

Three lessons from the first week of this.

Merge order matters when two branches both bump the version. Two open pull requests each wanted to be the next number. Merge them in the wrong order and the second one's version goes backwards. The handoff now says which one goes first, by number.

A flaky test is a bug, not weather. One check passed or failed depending on timing. An overnight agent cannot tell a flaky test from a real failure, and will either waste a run on it or learn to ignore red. The flaky tests were fixed before 2.0, and the rule since is that a check that fails without a cause gets fixed, not retried.

Never paste a key into chat. The relay's API key lives outside the repo. The Wi-Fi password goes only into the development card, flashed with a flag, and release builds carry no network at all. One rule in the handoff says: never copy the key into a file, a log or a pull request. A model reading a conversation that contains a key will happily write it into a test. The fix is to never let it see one.

Also, while I am being honest: a Codex mode for the relay was tried and the safety check blocked it. Correctly. It would have let anyone on the network run a program on my Mac. Codex on the Pi waits for a secure tunnel.

## 10. Names and money

Names first. The file manager was going to become Folio and the OS was going to become Mirage. Both were decided in a chat, written into the handoff, and dropped. The favourites right now are Maple and Desk. Nothing has been renamed. Joshua Tree stays the project's name, and a trademark check comes before any new one goes on a box.

Money. The OS and the apps are free, Apache 2.0, forever. The plan is that the money is in hardware: a kit with a Pi 4 board, an SD card with Joshua Tree already on it, and a printed case, built and tested, with a person to write to when it breaks. I set the kit price at $500 USD on October 7, 2026. The earlier $199 and $349 prices for an x86 box were replaced by that one number.

Here is the honest state. Nothing is sold. Nothing is built. Nobody has paid. The waitlist form is live on the landing page and no count has been taken from it. The price was set before a parts total, a print or a build existed. `MONEY.md` says it in those words.

So there is a demand gate. No parts get bought for a batch, and no money is taken from anyone, not even a deposit, until enough people have asked. The gate is 50 people on the waitlist. That number is a guess, a planning threshold, and the doc says to move it if real numbers say so.

There is also a question I cannot answer yet. Selling a fully built electronic device usually needs FCC compliance in the US and ISED compliance in Canada. That is a real cost and a real wait, and neither is in the plan. It gets checked before the first charge.

The retail check is the other one. Before anyone defends $500, price a Pi 4 kit at a normal shop: board, card, power supply, case. The buyer will do that check. So the $500 has to name what it buys beyond the parts: a box that is built, tested and supported, the OS already on it, and someone to write to. Whether that is worth it is for buyers to say, not me.

## 11. What is next

The list is short because the roadmap says no to more things than it says yes to.

A wired USB mouse on the Pi. The pointer and clicks work in QEMU. My mouse is Bluetooth and there is no Bluetooth stack, so a wired one is next on the shopping list.

Secure websites. The repo holds five BearSSL crypto files, not a TLS client. The rest gets vendored, and one test fetches a secure page. Until then the Pi talks to the relay over plain HTTP on the home network, and the token can be sniffed. The autonomy doc says so plainly.

Notes and Clock on the Pi. The ring-3 apps rebuilt for ARM, built overnight, waiting for the board.

Saving to the SD card. Today the Pi boots from the card and reads nothing off it. Writes mean a partition walk and FAT32. Until the Pi can write its own card, nothing it builds can reach it.

And the long one, in `docs/AUTONOMY.md`. You talk to the Pi. You tell Samantha what to fix in the OS. An agent writes the change on a branch, builds it, tests it. A person merges it. The Pi fetches the new kernel, writes it to the card, reboots into it, keeps the old one in case, and tells you whether it worked. Nine steps. Four of them exist. The hard middle is the card.

A computer you can talk to and tell what to fix. That is the sentence the whole project is trying to make true. The Pi on the desk is the first one that could.

---

*How this book was written.* By Joshua with Claude, in October 2026, from the repo's docs and git log and from the public journal at journal.heyitsmejosh.com. Every date and number is traceable to a commit, a doc or a photo in `docs/hardware/`. It is a living file: when the Pi learns something new, this gets a paragraph.
