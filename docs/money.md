# Joshua Tree monetization

OS stays free, forever. No paid license, no subscription for the kernel/GUI/apps.

Money comes from hardware:
- Dev-kit board, JT pre-flashed, sold to the osdev/retrocomputing crowd
- Voice-control reference device, tied to the v100 roadmap goal
- Support/consulting for embedded teams who want a tiny, fully auditable stack, no Linux, no libc

Real chips already supported: rtl8139 NIC, ATA, PCI. Gap: mouse driver is QEMU-only, needs real PS/2/USB HID before "hardware-ready" is true.

Long-term bet: port Joshua's own apps (epiphany, curvely, etc) to run natively on JT, zero runtime between app and bare metal. Wire in Turing (local LLM) so the OS eventually builds and extends itself from voice/chat, not hand-written C. That's the real end state of the v100 roadmap item.

## Revenue projections (guesses, not commitments, no data behind these yet)

Rough guess, small-hobbyist-hardware scale, not a startup pitch:

- **Year 1** (dev-kit board only, osdev/retrocomputing niche): 100-500 boards/year at $80-150 each → $10k-$60k/year. Comparable to a modest Kickstarter-class niche board (Pi Pico clones, badge kits).
- **Year 2-3** (voice-device SKU added, LLM-on-JT story matures, some press from "an OS that builds itself by voice"): 500-2,000 units/year across both SKUs at $100-250 avg → $75k-$300k/year, plus a few consulting contracts ($10k-$50k each, 2-4/year).
- **Year 3+, if the native-app-porting bet lands** (Bloomberg-style zero-runtime trading terminal running on JT hardware, a real differentiated pitch): this is the actual upside case, not the hobbyist-board case, and has no comparable to size against. Could be flat, could be the whole business. Genuinely unknown.

One real person building this part-time, not funded, not staffed: expect Year 1 to be closer to $0-$10k than $60k unless the board actually ships and gets real distribution. Treat all of the above as an optimistic ceiling, not a forecast.

## Update, 2026-09-16

A full session went into CI reliability and the public landing page, not new hardware capability, but it's the same credibility bet: a dev-kit pitch to the osdev crowd lives or dies on "does the demo actually work when a stranger clicks it," and tonight closed a real gap there. CI was flaky for hours on a genuine kernel-state race (fixed with a generic poll helper, not a timeout hack), and the landing page's live in-browser demo had a real, hard-to-spot layout bug (a device-frame experiment that collapsed the demo on both mobile and desktop, reverted same night). Neither moves revenue directly, but a broken first impression is a real cost against the Year 1 numbers above, this was cheap insurance against that.
