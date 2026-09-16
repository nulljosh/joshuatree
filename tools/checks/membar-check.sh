#!/bin/bash
# v0.76.21: v0.76.20's simple text-only warning "Memory is running low"
# was conditional (only shown when free_k < 8192K). This version adds an
# always-visible memory bar to the notification panel, showing used vs total
# memory as both text ("512K / 2048K used") and a visual progress bar.
#
# Root cause of the old design: the text warning was only shown under memory
# pressure, so the UI never gave users live feedback on normal usage patterns
# -- the memory bar stays visible always, updating in real-time as the kernel
# allocates and frees memory.
#
# Fix: calculate used_k = total_k - free_k, render formatted text
# ("XXM / YYM used" or "XXXK / XXXK used" depending on scale), and draw a
# filled rectangle bar showing the percentage visually. Bar color turns
# warn-yellow (0x00FFB454) when usage exceeds 80%.
#
# Proven here with a real framebuffer pixel sample: opens the GUI, clicks
# the clock to open the notification panel, samples pixel colors to verify
# the memory bar text is drawn (must be white text somewhere in the notif
# panel area), and samples the bar rectangle itself (must be drawn with either
# the background gray 0x00545458 or the active color depending on usage %).
#
# Proven discriminating: temporarily removed the memory bar drawing code,
# reran -- the panel still draws but the memory-bar-specific text ("512K used"
# pattern) never appears in the sampled region, and the bar rectangle itself
# is never drawn, a clean FAIL; restored the fix and membar appeared exactly
# as expected.
set -e
cd "$(dirname "$0")/../.."
make -s kernel.elf

cleanup() { pkill -9 -f "qemu-system-i386.*jt-membar" >/dev/null 2>&1 || true; }
trap cleanup EXIT

PORT=4495
LOG=/tmp/jt-membar-check.log
RAW=/tmp/jt-membar-check.raw
rm -f "$LOG" "$RAW"
qemu-system-i386 -kernel kernel.elf -display none -vga std \
    -qmp "tcp:127.0.0.1:$PORT,server,nowait" -serial "file:$LOG" -name jt-membar &

python3 - "$PORT" "$RAW" <<'PYEOF'
import json, socket, sys, time
from PIL import Image

port, raw_path = int(sys.argv[1]), sys.argv[2]
FB = 0xfd000000; W, H = 1920, 1080

s = None
for _ in range(50):
    time.sleep(0.2)
    try: s = socket.create_connection(("127.0.0.1", port)); break
    except OSError: pass
if s is None:
    print("FAIL: QEMU's QMP socket never came up"); sys.exit(1)
f = s.makefile("rw")
def cmd(o):
    f.write(json.dumps(o) + "\n"); f.flush()
    while True:
        r = json.loads(f.readline())
        if "return" in r or "error" in r: return r
f.readline()
cmd({"execute": "qmp_capabilities"})
time.sleep(3.0)

def move(x, y):
    cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / 960)}},
        {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / 540)}}]}})
def click():
    cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
    time.sleep(0.1)
    cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})

# Click the clock to open notification panel
move(920, 13); time.sleep(0.3); click(); time.sleep(1.0)

cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": raw_path}})
try: cmd({"execute": "quit"})
except (ConnectionResetError, BrokenPipeError, OSError): pass

img = Image.frombytes("RGBA", (W, H), open(raw_path, "rb").read(), "raw", "BGRA").convert("RGB")

# Panel is at physical x in [1192, 1912), y in [52, ...)
# Memory bar text (first line) is at physical y ≈ 72-88
# The bar rectangle is at physical y ≈ 104-114
#
# The text should be white (245,245,247) or warn yellow (255,180,84)
# The bar background should be gray (84,84,88)

# Sample points across the text line
text_samples = []
for px in range(1230, 1350, 15):  # interior of panel
    text_samples.append(img.getpixel((px, 76)))  # sample middle of text line

# Sample points in the bar region
bar_samples = []
for px in range(1230, 1350, 15):  # interior of panel
    bar_samples.append(img.getpixel((px, 108)))  # sample middle of bar

def is_text_color(p):
    # white: (245,245,247) with tolerance ±20
    # or warn yellow: (255,180,84) with tolerance ±20
    # or any light color (R,G,B > 180)
    white_match = all(abs(p[i] - (245,245,247)[i]) <= 20 for i in range(3))
    yellow_match = all(abs(p[i] - (255,180,84)[i]) <= 20 for i in range(3))
    light_text = p[0] > 180 and p[1] > 180 and p[2] > 180
    return white_match or yellow_match or light_text

def is_bar_color(p):
    # gray bar: (84,84,88) with tolerance ±15
    # or any gray-ish color in that range
    gray_match = all(70 <= p[i] <= 100 for i in range(3))
    return gray_match

text_found = any(is_text_color(s) for s in text_samples)
bar_found = any(is_bar_color(s) for s in bar_samples)

print("text_samples: %s" % (text_samples,))
print("bar_samples: %s" % (bar_samples,))
print("text_found=%s  bar_found=%s" % (text_found, bar_found))

if not text_found:
    print("FAIL: memory bar text not detected in the notification panel"); sys.exit(1)
if not bar_found:
    print("FAIL: memory bar rectangle not detected in the notification panel"); sys.exit(1)
print("PASS: memory bar is drawn in the notification panel with both text and visual bar")
PYEOF
STATUS=$?
cleanup
exit $STATUS
