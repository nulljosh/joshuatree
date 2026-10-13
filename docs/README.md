# Joshua Tree docs

- [Current pickup](LOOP-HANDOFF.md): what is being tested now
- [Our story](STORY.md): who we are, how it went, where it is going, and the launch drafts (Show HN post, 30 second video script, not yet posted)
- [The book](BOOK.md): how it got here, in our words
- [Soul](SOUL.md): the pro and the rebel, and Seduce. Vilify. Ignore.: the ideas under every decision
- [Decisions](DECISIONS.md): the standing answers (who can push, who it is for, when 3.0 is done)
- [Whitepaper](WHITEPAPER.md): why and how
- [Architecture](ARCHITECTURE.md): how it fits together, every file
- [Design](DESIGN.md): the rules for icons, type, colour, the dock and motion, checked against the source, and the mark history
- [Hardware](HARDWARE.md): the real board and the 3.0 plan
- [Portfolio mode](PORTFOLIO.md): how heyitsmejosh.com is a mode of the OS, with Joshua in place of Samantha
- [Raspberry Pi guide](RASPBERRY-PI.md): what to buy (with the shopping list), the serial cable, and how to boot it on a real Pi 4. [Printable Pi case](hardware/PI-CASE.md). [Samantha box](hardware/SAMANTHA-BOX.md), a desk unit with a 7 inch screen. [Neo build and assembly](hardware/BUILD.md). [Breadboard prototype](hardware/BREADBOARD.md)
- [ARM64 and the Raspberry Pi](ARM64.md): the second CPU target. It boots under QEMU today with a drawn desktop, a keyboard, mouse, disk and network, and its first program in user mode (`make -C arch/arm64 run`); the Pi 4B is the first real board
- [Autonomy](AUTONOMY.md): the gap list between today's Pi and a computer that fixes itself, with the first three tasks
- [Encrypted Pi relay](RELAY-TLS.md): certificates, the trusted-time bootstrap and remaining security limits
- [The Claude app](CLAUDE-APP.md): Claude Code from inside Joshua Tree, through a relay on your Mac, and its security model
- [Research](RESEARCH.md): how comparable projects do it, and the reference series mapped to what is built
- [CI and deploy](CI.md): how a change gets from a branch to the live site
- [Roadmap](roadmap.md): what is next

## Reference

[User guide](GUIDE.md), [Terminal](TERMINAL.md), [System call ABI](SYSCALL-ABI.md), [Testing](TESTING.md), [Benchmarks](BENCHMARKS.md), [Board test](BOARD-TEST.md), [Lessons](LESSONS.md), [Vision](VISION.md), [Versions](VERSIONS.md), [Done](roadmap-done.md), [Current pickup](LOOP-HANDOFF.md), [Type](TYPE.md), [Font sources](fonts-sources.md), [Blueprint](BLUEPRINT.md), [Agent](AGENT.md), [Local LLM](LOCAL-LLM.md), [Security on the Pi](SECURITY-PI.md), [Threat model](THREAT-MODEL.md).

## Licenses

Software: [Apache License 2.0](../LICENSE), © 2026 Joshua Trommel.

Hardware designs: CC BY-NC-SA 4.0, free to build for yourself. See [hardware/LICENSE-NOTICE.md](hardware/LICENSE-NOTICE.md).

Joshua Tree™ and Neo Kit™ are trademarks. See [../TRADEMARKS.md](../TRADEMARKS.md).

[Back to the project](../README.md)
