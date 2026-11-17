# Hardware license

SPDX-License-Identifier: CC-BY-NC-SA-4.0

Everything in `docs/hardware/` is © 2026 Joshua Trommel, licensed CC BY-NC-SA 4.0. That covers the CAD source, the STL and STEP files, the print pack, the guides and the images. The operating system stays Apache 2.0 (see the root `LICENSE`).

- **You may** build and print the Nimbus case for yourself, change it, remix it and share your changes. Give credit, say what you changed, and share under the same license.
- **You may not** sell kits or prints, or sell it as a service, without written permission. A print shop may print your copy for your own use.
- Build one for yourself. To sell them, ask first: trommatic@icloud.com.

The full license is in `LICENSE` next to this file. This notice is not legal advice. It is not "open source hardware", and it grants no rights to the names or the mark (see `TRADEMARKS.md`).

## Radio firmware

The Pi 4's radio needs Cypress (now Infineon) firmware that is not in this repo. The Wi-Fi files come from RPi-Distro/firmware-nonfree (`tools/wifi-fw.sh`) and the Bluetooth patch `BCM4345C0.hcd` from RPi-Distro/bluez-firmware (`tools/bt-fw.sh`). Both are fetched into `build/` (gitignored) with their licences beside them. `LICENSE.cypress` lets anyone distribute the binary for Cypress chips, so a card image may carry them. The repo never does.
