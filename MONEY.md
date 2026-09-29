# Joshua Tree Money

How Joshua Tree makes money. The fleet-wide ledger is `GTM.md` in the Code root.

## Price

Free. The OS and its apps stay free, forever.

## Rail

None yet. No accounts, no payments in the kernel. That's on purpose.

## The bet

An operating system you talk to. Ask it to open an app or take a note and it does, out loud. The assistant is Samantha, from Turing, a separate project; Joshua Tree is the OS she runs the machine through. No Linux underneath. No libc. Every line is ours, so every line can be checked.

The software is the demo. The money is in the thing it runs on.

## What it's worth today

Revenue: $0. On purpose, the OS is free.

Real value right now is proof. One person built a whole computer from nothing, and anyone can click and run it. That gets a founder funded or an engineer hired senior. Treat it as the reputation that sells the dev kit later.

## Getting it seen

In order. Each one feeds the next.

1. Show HN: "I built an operating system from scratch, and it runs in your browser." The live demo is the pitch. One click, no install.
2. A 30 second video: boot, open apps, ask for something, hear the answer. X, Reddit (r/osdev, r/programming), YouTube Shorts.
3. The story: one person in Langley, Claude as the hands. Pitched to AI and developer newsletters.
4. The osdev forums: source, a write-up of the hard parts. These are the first dev kit buyers.
5. A waitlist on the landing page for the dev kit. The count is the demand test before any hardware gets ordered.

## The million dollar plan

A dev kit. A small board with Joshua Tree flashed on it, sold to the people who build their own computers for fun. The board pick and driver work are in `docs/HARDWARE.md`.

- 5,000 boards at $199 is $1M.
- Needs: real drivers for one real board (network, sound, USB keyboard and mouse), a landing page that sells it, one launch on Hacker News and the osdev crowd.
- Proof it's working: the first 100 preorders.

### Unit economics (v0, from the HARDWARE.md BOM)

Everything below is either sourced (a real listing) or marked estimate. As of this writing only the board price has a real listing behind it - the rest of the BOM is estimate. Don't read this as a quote.

| Line | Amount | Note |
|---|---|---|
| Parts (board, RAM, storage, case, PSU, USB stick, cables) | ~$250 | docs/HARDWARE.md BOM; mostly estimate, board price sourced |
| Payment fees (~3%, card processing) | ~$6 at $199 | standard processor rate, estimate |
| Shipping (domestic, boxed mini-ITX build) | ~$15-25 | estimate, no carrier quote yet |
| Warranty reserve | ~$10/unit | estimate, no return-rate data exists yet |
| **Cost per unit (parts + fees + shipping + warranty)** | **~$281-291** | **exceeds $199** |
| Price | $199 | current price, unchanged here |
| **Margin at $199** | **negative, roughly -$85 to -$95/unit** | the BOM does not support $199 today |

$199 doesn't work for a whole computer. So we don't sell a whole
computer at $199.

### The fix: two boxes (decided 2026-09-28)

**Strata Kit, $199.** The case, a USB stick with the OS on it, the screws,
and a printed parts list. The buyer brings the board, RAM, SSD and power
supply. Developers already own half of that, and the board is a $120
listing they can buy anywhere.

| Line | Amount | Note |
|---|---|---|
| Strata case (6 rings, cap, tray, rods) | unknown | no print quote yet; $40 is the stock-case estimate above |
| USB stick, pre-flashed | ~$8 | estimate |
| Screws, spacers, rods | ~$7 | estimate |
| Payment fees | ~$6 | 3% of $199 |
| Shipping, small box | ~$15 | estimate, no carrier quote |
| **Cost without the case** | **~$36** | |
| **Breakeven case cost** | **~$163** | $199 minus $36 |

The kit makes money as long as the Strata case costs under about $160 to
make. The first real print quote says by how much.

**Strata Complete, $349.** Everything built and tested, plug it in. Parts
are ~$250 with a stock case, ~$281-291 with fees, shipping and warranty,
so $349 leaves roughly $58-68 before the Strata case costs more than a
stock one. $299 would leave $8-18, too thin to survive one return.

**Later: a cheaper board.** The ODROID-H4 lists at $99
(<https://liliputing.com/odroid-h4-is-a-mini-pc-board-with-intel-alder-lake-n-that-sells-for-99-and-up/>)
against ~$120 for the current pick, but it has no PS/2 ports, so it only
works once the kernel has a USB driver for keyboards. That's the xHCI
item on the roadmap. Not before.

**Next real step:** get an instant quote for the Strata parts from a print
service using `docs/hardware/strata_cad.py`'s STLs. That one number decides
the kit margin.

### The first dollar

What has to be true before anyone is charged:
- A board that passes Phase 0-2 in `docs/HARDWARE.md` (boots, shows a
  screen, takes keyboard input) on real hardware, not QEMU.
- A real print quote for the Strata case under the ~$163 kit breakeven.
- A real fulfillment path: who assembles the kit and who ships it.

Path, in order:
1. **Waitlist first, no charge.** The landing page's existing waitlist
   ("Getting it seen," step 5 above) is the demand signal. No fixed
   threshold is set here - that's Joshua's call once Phase 2 is real and
   the board price isn't mostly estimate. The Kit ships first.
2. **Channel:** the Joshua Tree landing page (joshuatree.heyitsmejosh.com),
   same place the OS demo runs. No third-party storefront planned for v0.
3. **Fulfillment:** unresolved. Self-assembly-and-ship by Joshua at low
   volume, or a contract assembler at higher volume - no decision made,
   no quote gathered.
4. **First charge only after:** a board that passes Phase 0-3 (boots,
   desktop, input, a file survives a reboot) on camera, and a BOM that
   doesn't lose money: Kit at $199, Complete at $349.

### Milestones (targets, not promises)

Tied to the bring-up phases in `docs/HARDWARE.md`. No revenue, user
count, or date below is a fact.

| Milestone | Tied to | What it proves |
|---|---|---|
| Board boots, shows a picture | Phase 0-1 | GRUB/multiboot works on real silicon, not just QEMU |
| Keyboard and mouse work | Phase 2 | the PS/2 pick was right, or the fallback plan kicks in |
| Files survive a reboot | Phase 3 | "a real disk" in the 3.0 definition is true |
| The board gets online | Phase 4 | Samantha's tools work off QEMU |
| The board makes sound | Phase 5 | all five 3.0 pieces are real |
| First 100 preorders | after unit economics resolve | demand exists at a price that doesn't lose money |

## The billion dollar plan

The Joshua Tree device. A box on the counter with a screen and a face. You talk, she does it. Mail, calendar, music, the lights. No cloud account needed for the basics.

- 3 million devices at $299, plus a $5 a month plan for her bigger brain in the cloud. Hardware pays for itself, the plan is the business.
- Needs: a manufacturing partner, an assistant model running inside the kernel, sign-in with Google and Apple for mail, a phone version of the OS.
- Proof it's working: people keep talking to her after the first week.

## The trillion dollar plan

The computer after the phone. Joshua Tree becomes what people talk to instead of tapping: phones, cars, glasses, the kitchen. Every device runs the same small, auditable OS, and talking to it is the only interface most people ever need.

- A trillion means a billion people. That's Apple and Google territory.
- Needs: an OS small enough to trust and prove secure, an assistant good enough to replace the home screen, and partners who put it on hardware we don't build.
- Proof it's working: another company ships Joshua Tree on their device.

## Where we are

- 2026-09-28, afternoon: a 36 second ad exists. Strata renders, the real OS booting, Samantha narrating in her own voice, original music. It's in the README. The landing page doesn't show the hardware yet; that's the next conversion fix before any waitlist push.
- 2026-09-28: the $199 problem is fixed on paper. Two boxes: a $199 Strata Kit (case plus OS stick, bring your own board) that makes money if the case prints under about $160, and a $349 Strata Complete. The Strata case now has a real CAD file, a drawing and a build guide.
- 2026-09-28: 1.7.4 merged. The OS now gets its own internet address on its own, has the start of voice input, and on phones Samantha's face no longer covers her title bar. A broken deploy setting kept the live site on 1.7.3; the fix is in review. Work started on moving apps out of the core so one crashing app can't take the machine down.
- 2026-09-27: the Samantha app (Turing's assistant, reached through Joshua Tree) talks with a face and a real voice through the OS's own sound driver. The landing page runs the whole OS live in the browser.

## Next

Real drivers for one real board. That's the first dollar.

*Set 2026-09-28.*

*Landing synced 2026-09-28: the 36 second ad and the Strata case concept sit under "Want one?". Strata is a concept, not for sale, no price set.*

Homeqi now runs on its own at ring 3, so one more app can crash without taking the machine with it. Nothing to sell yet, but every app that leaves the kernel makes the real board pitch a bit more believable. *2026-09-28.*

Notes now has folders and lets you keep more than one note, so it starts to feel like a real app on a real board. Still free, nothing to sell. *2026-09-28.*

Touch works now and Notes gets its own on-screen keyboard on a phone screen, so you can tap a note and type it with no keys attached. That is the first thing a phone buyer tries, so it earns its place in the pitch. Still free, nothing to sell yet. *2026-09-28.*
