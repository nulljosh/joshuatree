# Joshua Tree monetization

OS stays free, forever. No paid license, no subscription for the kernel/GUI/apps.

Money comes from hardware:
- Dev-kit board, JT pre-flashed, sold to the osdev/retrocomputing crowd
- Voice-control reference device, tied to the v100 roadmap goal
- Support/consulting for embedded teams who want a tiny, fully auditable stack, no Linux, no libc

Real chips already supported: rtl8139 NIC, ATA, PCI. Gap: mouse driver is QEMU-only, needs real PS/2/USB HID before "hardware-ready" is true.

Long-term bet: port Joshua's own apps (epiphany, curvely, etc) to run natively on JT, zero runtime between app and bare metal. Wire in Turing (local LLM) so the OS eventually builds and extends itself from voice/chat, not hand-written C. That's the real end state of the v100 roadmap item.

No revenue numbers exist yet. Don't invent one.
