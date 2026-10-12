# SPDX-License-Identifier: CC-BY-NC-SA-4.0
# Copyright (c) 2026 Joshua Trommel. Hardware design, see docs/hardware/LICENSE-NOTICE.md
# Raspberry Pi 4 Model B numbers, from its mechanical drawing. Constants only: neo_pi_cad.py and samantha_cad.py
# both import them, so a caliper correction lands in both cases at once. mm throughout.
# The windows are deliberately bigger than the connectors, so half a millimetre of error in a port still fits.

BOARD_L, BOARD_S = 85.0, 56.0          # the board outline
PCB_T = 1.4                            # board thickness
STANDOFF = 4.0                         # the gap under the board, for the solder joints on its back
TOP_PORT = 16.5                        # the USB and Ethernet stack, the tallest thing on the board

# Mounting holes: 58 x 49 mm, 3.5 mm in from each corner, relative to the board centre
HOLES = [(-39.0, -24.5), (19.0, -24.5), (-39.0, 24.5), (19.0, 24.5)]

# Port windows: (centre along the wall, width, z from, z to). z is measured from the top of the board, so each
# case adds its own floor and standoff height. Along the wall is board x (front, back) or board y (right).
FRONT_WIN = (-11.0, 52.0, -1.0, 8.0)          # USB-C power, two micro-HDMI, 3.5 mm jack: board x 5.5 to 57.5
RIGHT_WIN = (0.25, 53.5, -1.0, TOP_PORT)      # Ethernet and the four USB ports: board y 1.5 to 55
BACK_WIN = (-9.5, 51.5, 0.0, 9.5)             # the 40-pin GPIO header, for a serial cable or a HAT
