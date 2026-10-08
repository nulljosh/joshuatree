# Joshua Tree Money

How Joshua Tree would make money. It does not make any yet. This page is
a plan, written so a stranger can follow it. Real numbers are marked
real. Everything else is an estimate or an assumption, and says so. The
fleet-wide ledger is GTM.md in the Code root, outside this repo.

## In one paragraph

The OS and its apps are free, Apache 2.0, forever. The money is the
ready-made kit: a Raspberry Pi 4 board, an SD card with Joshua Tree
already on it, a printed Neo case, built and tested, with support.
Joshua set the kit price at $500 USD on 2026-10-07. The kit is not built,
nothing is for sale, nobody has paid, and the waitlist has no count
yet. The first dollar is still ahead of us.

## What we sell and what stays free

| Thing | Price | State |
|---|---|---|
| The OS (kernel, desktop, drivers) | Free, Apache 2.0 | Shipped. Runs in the browser and on a real Pi 4 |
| The apps (Notes, Clock, Samantha, the rest) | Free, Apache 2.0 | Shipped on x86 and in QEMU. Being rebuilt for the Pi |
| The case designs (Neo, Neo Pi) | Free to build for yourself, CC BY-NC-SA 4.0 | Drawn, not printed |
| The kit: board, SD card with the OS, case, built, supported | $500 USD | Not built. No waitlist count yet. No date promised |

The hardware licence is non-commercial on purpose. Anyone can print a
case for themselves. Nobody can sell copies of the kit. That protects
kit sales while every line of software stays free.

No accounts, no payments and no tracking in the kernel. That is on
purpose, and it stays that way.

## The bet

An operating system you talk to. Ask it to open an app, take a note or
blink the light and it does, out loud. The assistant is Samantha, from
Turing, a separate project. Joshua Tree is the OS she runs the machine
through. No Linux underneath. No libc. Every line is ours, so every line
can be checked.

The software is the demo. The money is in the thing it runs on.

## What it is worth today

Revenue: $0. On purpose, the OS is free.

The real value right now is proof. One person built a whole computer
from nothing, got it onto a real board, and anyone can click and run it.
That gets a founder funded or an engineer hired senior. Treat it as the
reputation that sells the kit later.

## Who the kit is for

Nobody has bought one. These are the people we think would, and what
each would get.

| Who | What they get |
|---|---|
| People who build computers for fun | A box with nothing underneath. No Linux, no vendor OS. Every layer is readable and theirs to change |
| Makers | A board with the 40-pin header open through the case, real drivers for an LED, a button and a speaker, and an assistant that can act on the pins |
| Students | A whole OS small enough to read end to end, with a plain-words guide (planned, `docs/roadmap.md`) that explains every file |
| Small teams that want a private assistant on a box they own | Samantha on their desk, their own AI key, nothing stored on our side |

## The price: $500 USD, set by Joshua on 2026-10-07

One kit, built and tested, $500 USD. Target: about two months from the
decision, so early December 2026. Nothing here is a promise to anyone.

Why $500. It has to cover three things: the parts, the hours to build
and test each box, and the upkeep of the hardware and the software after
the sale. A hobby price covers parts only. A product price covers the
other two. The earlier $199 Kit and $349 Complete prices for the x86 Neo
box are replaced by this one number for the Pi kit. The x86 Neo plan
stays as a later box.

### Unit economics, from the BOM

The parts list for the Pi kit is in `docs/SHOPPING.md` and the case in
`docs/hardware/PI-CASE.md`. Neither has a kit total yet. The only line
in the repo with a real listing behind it is the x86 board in
`docs/HARDWARE.md` (about $120 USD), and that board is not in this kit.
So every Pi line below is an estimate. Read the table as a shape, not a
quote.

| Line | Amount (USD) | Real or estimate | Where it comes from |
|---|---|---|---|
| Raspberry Pi 4B board | no number yet | estimate needed | not priced in any doc yet; a retail listing goes here |
| Power supply, USB-C | no number yet | estimate needed | not in `docs/SHOPPING.md` yet |
| microSD card, 32 GB, with the OS on it | about $10 | estimate (CAD, rough) | `docs/SHOPPING.md` |
| Neo Pi case, printed | about $9 plastic at home; $40 to $75 from a print service | estimate | `docs/HARDWARE.md`, Prototype cost (sized for the bigger x86 case, so the Pi case should be less) |
| Bolts and nuts | about $7 | estimate | `docs/HARDWARE.md`, hardware bag |
| Card fees, about 3% of $500 | about $15 | estimate, standard processor rate | no processor picked |
| Shipping, small box | about $15 to $25 | estimate, no carrier quote | same guess as the x86 kit |
| Warranty reserve | about $10, too low | placeholder, no return data | same guess as the x86 kit. Too low for a hardware kit that can arrive damaged. One dead board or one return shipped both ways costs more than this. Needs a real number from a shipping and return quote |
| Build and test time | no number yet | estimate needed | depends on who builds it |
| **Price** | **$500** | **real, Joshua's call** | this page |

What the table says: the known lines add to roughly $65 to $140 before
the board, the power supply and labour. The board and power supply are
the biggest missing numbers. Until they are written down, the margin is
"probably wide, not known."

What fills it in, in order: the Pi 4B and power supply retail price, the
first real print quote for the Neo Pi STLs (built by
`docs/hardware/neo_pi_cad.py`), and a carrier quote
for one boxed kit to a US address.

**Retail check.** Before anyone defends the $500, price a Pi 4 kit at
retail: the board, a microSD card, a USB-C power supply and a stock
case, from one ordinary shop. Retail kit price: no number yet. Write it
here when it is found. The buyer will do this check. So the
$500 has to name what it buys beyond the parts: a box that is built,
tested and supported, the OS already on it, and a person to write to
when it breaks.

### Scenarios (assumptions, not facts)

**Made up for planning. Replace with real numbers as they arrive.**

No kit has sold. These three rows are a way to think about the first
year, not a forecast. Each assumption is written beside its number.

| Case | Kits sold in year one | Revenue at $500 | Estimated cost per kit | Gross margin | The assumption behind it |
|---|---|---|---|---|---|
| Low (assumption) | 20 | $10,000 | $250 | $5,000 (50%) | Friends, meetups and the osdev crowd. Parts at retail, case from a print service, Joshua builds each one |
| Middle (assumption) | 150 | $75,000 | $200 | $45,000 (60%) | One Show HN that lands plus a video that gets shared. Parts in small batches, case printed at home |
| High (assumption) | 1,000 | $500,000 | $170 | $330,000 (66%) | A second launch wave and press. Parts at small-volume pricing, a contract builder. Support load at this size needs a second person |

Margins fall if the board and power supply are priced high, and this
table is not a forecast.

The cost-per-kit column is a guess built on the estimate table above
plus a guess at labour. It is not from a quote. The margin is before
income tax, tools, the printer, returns above the reserve, certification
(see Risks), and any time spent on support.

## Before any money changes hands

A demand gate comes first. No parts get bought for a batch, and no
parts money is taken from anyone, until enough people have asked.

- **The gate: 50 people on the waitlist.** This number is a guess, a
  planning threshold, not a measured one. Move it if real numbers say
  so.
- **The waitlist** is the "Notify me" form on the landing page.
- **The count today:** no count exists yet. The form is live. Write the
  number here when one is taken.

Until the gate is met, the kit stays a plan. Nobody is charged, not even
a deposit.

## The first dollar, and the order we would try things in

What has to be true before anyone is charged:

- The demand gate above is met.
- A Pi that boots to the desktop, takes a keyboard and a mouse, and
  keeps a file across a reboot, on camera, on the real board.
- A real print of the Neo Pi case that fits the board (the fit test in
  `docs/hardware/PI-CASE.md` first).
- A real parts total, the retail check, and a real shipping quote.
- An answer on certification (see Risks).
- A real fulfilment path: who builds, who ships, who answers mail.

Then, in this order. Each step is paid for by the one before it.

| Step | What it is | Built? |
|---|---|---|
| 1. The kit | Board, card with the OS, case, built and supported, $500 | The OS runs on the Pi. The kit itself is not built |
| 2. Paid support | Help by mail for people who built their own from the free designs, or who want the OS on hardware we did not sell | Not built. No price |
| 3. A hosted or premium pack | Extra apps, voices or a hosted relay for Samantha, for people who do not want to bring a key | Not built. No price. Would carry cloud cost, see Risks |
| 4. Private model training for teams | Samantha tuned on a team's own documents, on a box they own | Not built. Not designed. The farthest out |

Only the OS on the Pi exists today. Everything else on this list is a
plan.

Channel: the Joshua Tree landing page, the same place the OS demo runs.
No third-party storefront planned for the first kits. Fulfilment at low
volume is Joshua building and shipping each box. At higher volume it is
a contract builder. No decision made, no quote gathered.

Precedent from the rest of the fleet (GTM.md): every app is free or
$1, no subscriptions, no tips, and the Stripe account has zero charges
ever. The kit is the one thing in the fleet priced above a dollar,
because it is the one thing with real parts and real hours in it.

## Bring your own AI key

Samantha answers through an AI model over the network. Release builds of
the OS carry no key and no network credentials. The card Joshua develops
on carries our Wi-Fi and our key, flashed with `tools/flash-pi.sh` and
the `JT_WIFI_DEV=1` flag, and that card never ships. The relay's key
lives outside the repo.

A customer types their own key into the box once, and every answer is
billed to their account, not ours. That keeps our cost per user near
zero. There is no server of ours in the middle to pay for, scale or
secure. It also keeps the promise that nothing of theirs is stored on
our side.

The catch: typing a key is a step some buyers will not want. That is
what the hosted pack in step 3 above would solve, at a cloud cost we do
not have today.

## Getting it seen

In order. Each one feeds the next.

1. Show HN: "I built an operating system from scratch, and it runs in your browser." The live demo is the pitch. One click, no install. Drafted in `docs/LAUNCH.md`, waits for the mouse working on the Pi.
2. A 30 second video: boot, open apps, ask for something, hear the answer. X, Reddit (r/osdev, r/programming), YouTube Shorts. Drafted in the same file.
3. The story: one person in Langley, Claude as the hands. Pitched to AI and developer newsletters.
4. The osdev forums: source, a write-up of the hard parts. These are the first kit buyers.
5. In person: a live Pi on a table at Vancouver maker meetups and the UBC and SFU computing clubs.
6. The waitlist on the landing page. It is live and verified by a test email. Its count is the demand gate (see "Before any money changes hands") before any hardware is ordered. No count has been taken yet.

## Risks, plainly

- **Parts supply and price swings.** The Pi 4 has had long shortages before. Board, card and power supply prices move. A $500 price with no parts quote behind it can lose its margin without anyone changing a line here.
- **Certification.** Selling a fully built electronic device usually needs FCC compliance in the US and ISED compliance in Canada. That is a real cost and a real wait, and neither is in this plan. To check before the first charge; no quote yet.
- **The warranty reserve is a placeholder.** About $10 a kit will not cover a board that arrives dead or a return shipped both ways. It needs a real number from a shipping and return quote.
- **Support on one person.** Every kit is a person who can mail Joshua. At the low case that is fine. At the high case it is a job. There is no second person today.
- **The names are not cleared.** The file manager may become Folio and the OS may become Mirage. Both were decided in chat, neither is done, and the trademark check is still pending (`docs/LOOP-HANDOFF.md`). "Joshua Tree" and "Neo Kit" are claimed as trademarks on the hardware pages, not registered.
- **Cloud cost of anything hosted.** Step 3 above puts a server of ours between the customer and the model. That is a bill that grows with use, and the fleet's rule so far is no subscriptions. It is not built, and should not be until the bill is understood.
- **The board is someone else's product.** The Raspberry Pi is made by a company we do not control. They can change it, raise the price or end it. The long-term answer in `docs/roadmap.md` is a carrier board of our own for the compute module. That is years away, not months.
- **The kit price is ahead of the kit.** $500 was set before a parts list, a print or a build existed. The honest state is "a price and a plan," not "a product."
- **Nothing to show for input yet.** On the real Pi there is no mouse, no browser and no HTTPS. A box you cannot click in is not a box people pay for.

## What would make us stop or change course

Stop or rethink the kit if any of these turns out true:

- The real parts total plus labour comes in above about $350. Then $500 is a hobby price again and the number has to move or the box has to change.
- The waitlist stays short of the demand gate after the Show HN post and the video have both run.
- The first ten buyers each take more than a few hours of support. Then the kit needs a guide and a second person before the eleventh.
- The Pi 4 goes out of stock for months with no drop-in board. Then the kit waits for the Pi 5 port, which does not exist.
- A trademark search finds a conflict on a name already on the box. Then the box waits for the rename.

Change course, not stop, if the kit sells but the hosted pack is what
people ask for. That would mean the price was for the convenience, not
the hardware, and the plan should follow the customer.

## The million dollar dream

A dream, not a plan. A small kit, sold to the people who build their own
computers for fun. At $500 a kit, a million dollars is 2,000 kits. It
needs the kit to exist, a landing page that sells it, one launch on
Hacker News and the osdev crowd, and someone besides Joshua building
boxes. Proof it is working: the first 100 paid orders.

## The billion dollar dream

A dream, not a plan. The Joshua Tree device. A box on the counter with a
screen and a face. You talk, she does it. Mail, calendar, music, the
lights. No cloud account needed for the basics. It needs a
manufacturing partner, an assistant model that runs inside the kernel,
sign-in with Google and Apple for mail, and a phone version of the OS.
Proof it is working: people keep talking to her after the first week.

## The trillion dollar dream

A dream, not a plan. The computer after the phone. Every device runs the
same small, auditable OS, and talking to it is the only interface most
people need. A trillion means a billion people. That is Apple and
Google territory. It needs an OS small enough to prove secure, an
assistant good enough to replace the home screen, and partners who put
it on hardware we do not build. Proof it is working: another company
ships Joshua Tree on their device.

## Where we are

- 2026-10-08: the OS runs on the real Pi 4. The kit is not built. The money plan now has a demand gate (50 on the waitlist, a guess), a retail check, and certification and the warranty reserve named as open costs. No waitlist count, no retail kit price, no certification quote yet.
- 2026-10-07: on the real Pi 4, Wi-Fi joins and holds, DHCP and the network clock work, the menu bar shows the 12-hour clock and the three-bar Wi-Fi icon, and Samantha can act on the Pi (print a note, blink the green light). The desktop, dock, window frame and a USB keyboard work. No mouse, no browser, no HTTPS yet. The landing waitlist is live. The kit is not built and has no waitlist number. Joshua is buying a wired USB mouse and a 3.3V serial cable.
- 2026-10-06: the first boot on real hardware, a Raspberry Pi 4, flashed in one command (`tools/flash-pi.sh`). Decided the same day: the first kit is Pi-based, because it is the board that boots, the one people already own, and its case is already drawn (`docs/hardware/PI-CASE.md`).
- 2026-09-28 to 2026-09-30: eighteen apps moved out of the kernel to ring 3, so one app crashing cannot take the machine down. A 36 second ad exists (Neo renders, the real OS booting, Samantha narrating). Touch and an on-screen keyboard work on a phone screen. The $199 x86 problem was solved on paper with two boxes; that plan is now the later x86 box.
- 2026-09-27: Samantha talks with a face and a real voice through the OS's own sound driver. The landing page runs the whole OS live in the browser.
- On the x86 side, `rtl8139.c`, `ata.c` and `pci.c` target real chips. `vmmouse.c` only works under VMware or QEMU. The x86 board has never been bought.

## Next

The demand gate and the retail check. Take a count from the "Notify me"
form and write it here. Price a Pi 4 kit at a normal shop and write that
number here too. Nothing gets bought for a batch until the gate is met.

Alongside, on the build side: a wired USB mouse working on the real Pi,
because a board you can click in is the first thing anyone would pay
for. After that: the Pi parts total and power supply price, the Neo Pi
fit test printed, a shipping and return quote, and an answer on
certification. Then the price has numbers under it.

*Set 2026-10-08: the demand gate and the retail check. Was: a wired USB
mouse on the Pi, set 2026-10-08. Was: a USB keyboard on the Pi, set
2026-10-06. Was: real drivers for one real board, set 2026-09-28.*
