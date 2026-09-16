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
