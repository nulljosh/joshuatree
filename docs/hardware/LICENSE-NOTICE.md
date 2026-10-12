# Hardware license

SPDX-License-Identifier: CC-BY-NC-SA-4.0

Everything in `docs/hardware/` is © 2026 Joshua Trommel and licensed CC BY-NC-SA 4.0. That covers the CAD source (`neo_cad.py`), the STLs, the STEP file, the build sheet, the guides and the images. The operating system itself stays Apache 2.0, see the root `LICENSE`.

**You may** build and print the Neo Kit for yourself. Change it, remix it, share your changes. Give credit, say what you changed, and share your version under the same license.

**You may not** sell kits or prints, or sell it as a service, without written permission. Print shops like JLC3DP may print your copy for your own personal use, that is fine.

Build one for yourself. If you want to sell them, ask first: trommatic@icloud.com.

The license in full is in `LICENSE` next to this file, and the plain-English summary is at <https://creativecommons.org/licenses/by-nc-sa/4.0/>. This is a notice, not legal advice. "Free to build for yourself" is the plain version. It is not "open source hardware", and it does not grant any rights to the names or the mark, see `TRADEMARKS.md`.

## Radio firmware

The Pi 4's radio needs Cypress (now Infineon) firmware that is not ours and is not in this repo: the Wi-Fi files from RPi-Distro/firmware-nonfree (`tools/wifi-fw.sh`) and the Bluetooth patch `BCM4345C0.hcd` from RPi-Distro/bluez-firmware (`tools/bt-fw.sh`). Both are fetched at build time into `build/` (gitignored), each next to its licence (`copyright`, `LICENSE.cypress`), and embedded in `kernel8.img`. `LICENSE.cypress` lets anyone reproduce and distribute the binary in object code form, solely for use with Cypress chips, which the Pi's CYW43455 is. So a card image may carry them; the repo never does.
