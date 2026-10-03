# Raspberry Pi

Joshua Tree can't boot a real Pi yet. This page says what works today, what to buy, and exactly what to do the day the board arrives.

The plan and the milestones live in [ARM64.md](ARM64.md). The case is in [hardware/PI-CASE.md](hardware/PI-CASE.md).

## What works today

| | Status |
|---|---|
| Boots under QEMU's generic ARM machine and prints over the UART | Works. `make -C arch/arm64 run` |
| Boots as a Pi image on QEMU's Pi 4B model, enters at EL2, drops to EL1, prints | Works. `make -C arch/arm64 run-pi` |
| Boots on a real Pi 4 | Built, never tried. This is the first thing to test. |
| Anything on the screen | Not yet. That is milestone M1. |
| Keyboard, mouse, disk, network on the Pi | Not yet. M2 to M4. |

So on day one you will see text in a serial terminal, not a picture on a monitor. That is on purpose. It is the smallest thing that proves the kernel runs on the real chip.

## What to buy

- **Raspberry Pi 4 Model B, 4 GB.** The port targets the 4B first. The Pi 5 comes after.
- **A USB-C power supply, 5 V 3 A.** The official one is the safe pick.
- **A microSD card, 16 GB or more.**
- **A USB to serial cable, 3.3 V.** CP2102, FTDI or CH340 all work. This is how you see the output. Make sure it says 3.3 V, not 5 V.
- **Three female to female jumper wires.**
- A micro-HDMI to HDMI cable, a monitor, a USB keyboard and a USB mouse. You need these from milestone M1, not before.

A local store may have the Pi in stock. Check before you drive. Canada Computers and Memory Express are the usual ones in Vancouver, and PiShop.ca ships from Canada.

## Wire the serial cable

Turn the Pi off. Match the three wires. Never connect the adapter's 5 V or VCC pin to the Pi.

| Pi pin | What it is | Adapter |
|---|---|---|
| 6 | Ground | GND |
| 8 | GPIO14, the Pi's TX | RX |
| 10 | GPIO15, the Pi's RX | TX |

Transmit goes to receive. If you see nothing later, swapping the two data wires is the first fix.

## Build and boot

You need `clang`, `ld.lld` and `qemu-system-aarch64`. The Mac already has them.

```
make -C arch/arm64 pi          # makes arch/arm64/kernel8.img
make -C arch/arm64 run-pi      # try it on QEMU's Pi 4B first
```

The SD card needs the Pi's own boot files. The easy way:

1. Flash **Raspberry Pi OS Lite (64-bit)** to the card with Raspberry Pi Imager.
2. Open the card's boot partition on your computer.
3. Copy the old `kernel8.img` somewhere safe, then copy ours over it.
4. Add these four lines to the end of `config.txt`:

```
arm_64bit=1
kernel=kernel8.img
enable_uart=1
dtoverlay=disable-bt
```

`disable-bt` matters. It gives the good UART (the PL011) to the pins on the header.

5. Eject the card, put it in the Pi, plug the serial cable into your Mac.

Open the terminal. On a Mac the adapter shows up as `/dev/cu.usbserial-something`:

```
ls /dev/cu.usb*
screen /dev/cu.usbserial-0001 115200
```

Power the Pi. You should see:

```
Joshua Tree on ARM64
booted at EL2
EL1
M0 ok
```

That is milestone M0 on real hardware. To leave `screen`, press Ctrl-A then K.

## If nothing prints

1. Swap the two data wires.
2. Check the speed is 115200.
3. Check the adapter is 3.3 V and the ground wire is on pin 6.
4. Check all four lines are in `config.txt` and the card is fully ejected.
5. Add `uart_2ndstage=1` to `config.txt`. The Pi's own firmware then prints before ours. If you see that and not ours, the kernel is not starting. If you see neither, it is the wiring.
6. Look at the Pi's LEDs. A steady red light is power. A flickering green light is the card being read.

If it still fails, send the exact lines you see, even if they look like garbage. Garbage means the speed or the clock is off. Nothing at all means wiring or boot files.

## What is next

| Milestone | What you will see |
|---|---|
| M0 | Text over the serial cable. Built, waiting for a real board. |
| M1 | The Joshua Tree desktop on a monitor over HDMI. |
| M2 | Keyboard, mouse, network and disk, working in QEMU first. |
| M3 | Every app running on ARM. |
| M4 | The same on the real Pi: SD card, USB, Ethernet. Sound last. |
| M5 | The Pi 5. |

Joshua Tree 3.0 ships when it boots the desktop on a real Pi.
