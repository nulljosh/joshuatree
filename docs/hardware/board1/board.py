from PIL import Image, ImageDraw, ImageFont
import sys
K = 10   # px per mm
W, H = 200 * K, 130 * K
PAPER, INK, DIM, ACC = (38, 26, 19), (244, 243, 236), (184, 170, 150), (224, 120, 80)
im = Image.new("RGB", (W, H), PAPER); d = ImageDraw.Draw(im)
f = lambda s: ImageFont.truetype("/System/Library/Fonts/SFNS.ttf", s)
ox, oy, bw, bh = 20 * K, 20 * K, 160 * K, 90 * K   # the board, 160 x 90 mm
def R(x, y, w, h, label=None, col=INK, fill=None, sz=22, lw=3):
    x0, y0 = ox + x * K, oy + y * K
    d.rounded_rectangle([x0, y0, x0 + w * K, y0 + h * K], radius=6, outline=col, width=lw, fill=fill)
    if label:
        d.multiline_text((x0 + w * K / 2, y0 + h * K / 2), label, fill=col, font=f(sz), anchor="mm", align="center")
d.rounded_rectangle([ox, oy, ox + bw, oy + bh], radius=4 * K, outline=INK, width=5)
for hx, hy in ((4, 4), (156, 4), (4, 86), (156, 86)):   # M2.5 mounting holes
    d.ellipse([ox + (hx - 1.4) * K, oy + (hy - 1.4) * K, ox + (hx + 1.4) * K, oy + (hy + 1.4) * K], outline=INK, width=3)
R(52, 24, 55, 40, None, ACC, lw=5)   # the Compute Module 5
d.text((ox + 58 * K, oy + 28 * K), "Raspberry Pi Compute Module 5", fill=ACC, font=f(26))
d.text((ox + 58 * K, oy + 32 * K), "BCM2712, 4 x Cortex-A76, up to 16 GB", fill=DIM, font=f(20))
R(56, 40, 47, 4, "100-pin", ACC, sz=16, lw=2); R(56, 52, 47, 4, "100-pin", ACC, sz=16, lw=2)
# the back edge: power, display, network, USB
R(0, 70, 9, 12, "USB-C\npower", sz=15)
R(12, 76, 15, 8, "HDMI 0", sz=17); R(30, 76, 15, 8, "HDMI 1", sz=17)
R(48, 72, 16, 14, "Ethernet", sz=17)
R(68, 74, 15, 12, "USB-A 3", sz=16); R(86, 74, 15, 12, "USB-A 3", sz=16)
R(104, 78, 9, 7, "USB-C", sz=14)
R(116, 79, 7, 7, "jack", sz=14)
# the top: storage, fan, GPIO
R(112, 26, 42, 24, "M.2 M-key NVMe 2242 / 2280", sz=18)
R(8, 6, 51, 5, "40-pin GPIO", sz=18)
R(64, 6, 10, 5, "fan", sz=16); R(78, 6, 10, 5, "RTC", sz=16)
R(0, 30, 8, 12, "micro\nSD", sz=15)
R(10, 40, 18, 12, "PMIC +\n5 V / 5 A", DIM, sz=15)
R(30, 14, 14, 8, "Wi-Fi\nantenna", DIM, sz=14)
R(140, 6, 14, 6, "power", sz=16)
for i, c in enumerate(((90, 220, 120), (230, 80, 60))):   # status LEDs
    x0, y0 = ox + (128 + i * 4) * K, oy + 8 * K; d.ellipse([x0, y0, x0 + 18, y0 + 18], fill=c)
# the mark on the silkscreen
mark = Image.open(sys.argv[2]).convert("RGBA").resize((14 * K, 14 * K)); im.paste(mark, (ox + 120 * K, oy + 51 * K), mark)
d.text((ox + 112 * K, oy + 66 * K), "JOSHUA TREE  ·  board 1", fill=INK, font=f(24))
# dimensions and title
d.line([ox, oy + bh + 6 * K, ox + bw, oy + bh + 6 * K], fill=DIM, width=2); d.text((ox + bw / 2 - 40, oy + bh + 7 * K), "160 mm", fill=DIM, font=f(22))
d.line([ox + bw + 6 * K, oy, ox + bw + 6 * K, oy + bh], fill=DIM, width=2); d.text((ox + bw + 7 * K, oy + bh / 2), "90 mm", fill=DIM, font=f(22))
d.text((ox, 6 * K), "Joshua Tree board 1: a Compute Module 5 carrier, top view", fill=INK, font=f(40))
d.text((ox, 11 * K), "Concept only. Parts and positions to be laid out in KiCad.", fill=DIM, font=f(24))
im.save(sys.argv[1])
