#!/bin/bash
# v0.78.x: the dock magnify animation must finish in the time it was
# written to take, at whatever frame rate the machine happens to manage.
#
# Real report, right after v0.78.0's back buffer shipped: "the dock hover
# animation is now choppy and slow, not smooth." Measured before changing
# anything, and the cause was not the back buffer's copy. The animation
# advanced dock_hover_extra by exactly one fixed hop of 3 every time its
# 3-tick gate opened, and then did dock_anim_last_tick = ticks(), throwing
# the overshoot away. So a magnify always took 3 polls, and its real
# duration was 3 frames rather than the 9 ticks the constants describe.
# Anything that cost frame rate stretched the animation directly:
# measured 110ms per magnify at the frame rate v0.78.0 produced, against
# the ~90ms the same constants give at the rate they were tuned for.
# Three visible jumps spread over an extra tenth of a second is exactly
# what "choppy and slow" describes.
#
# The fix advances by however many 3-tick steps really elapsed and carries
# the remainder instead of resetting to now, so a slow frame means fewer,
# larger steps and never a longer animation.
#
# Measured, median of six magnifies, same harness both ways:
#   fixed hop per poll (v0.78.0): 11 ticks  (110 ms)
#   time-based (this fix)       :  6 ticks  ( 60 ms)
#
# v0.79.x: duration alone turned out not to be enough, and the same
# report came back ("dock hover on icons still janky, choppy, smooth zoom
# animation should be there") while this check was passing at a 50 ms
# median. Measured with an rdtsc probe calibrated against the PIT rather
# than guessed at: a wind sway frame costs 65 ms and fires every 5 ticks,
# so the whole gui_run loop was turning over at about 15 fps, and a
# magnify written to take six ticks got exactly one frame. It snapped from
# resting to fully lifted in a single jump while still finishing "on
# time", which is precisely the failure a duration assertion cannot see.
#
# So the kernel also reports dockstep=, the number of distinct sizes a
# magnify really passed through, and this asserts on that too. Real
# numbers, 18 magnifies over 3 boots at each stage:
#
#   before                          dockmag 5, dockstep 2   [1..3]
#   sway suppressed during a hover  dockmag 6, dockstep 3   [2..3]
#   + tray cached out of the frame  dockmag 6, dockstep 3   (frame 11ms -> 3.4ms)
#   + sub-pixel time ramp           dockmag 5, dockstep 6   [3..6]
#
# Two assertions, because a raw step count is a property of the machine as
# much as of the code. A box that only turns over three frames in sixty
# milliseconds cannot show six sizes however the animation is written, and
# a first attempt at "median distinct sizes >= 4" duly went red on this
# very machine the moment it was busy, which is a flake, not a finding.
#
#   1. the median magnify shows at least 3 distinct sizes. That is the
#      floor the report was below: measured 2, with hovers collapsing to a
#      single snap, because the sway had the frame rate down at 15 fps.
#   2. the median magnify shows a fresh size on all but at most one of the
#      frames it was given (dockstep vs dockframe). This is the part that
#      is purely the code's doing and does not move with machine speed,
#      and it is exactly what a quantum coarser than the frame rate
#      breaks: the old 3-tick gate advanced on one frame in three and left
#      the other two drawing nothing new. Measured side by side on the
#      same busy machine, same six hovers: the ramp shows 3 sizes in 2
#      frames and 6 in 5, the old quantum shows 3 in 5.
#
# Proven discriminating by actually reverting the ramp to the whole-pixel
# quantum of 3, with everything else left in place: real FAIL, while the
# duration assertion below went on passing at a 60 ms median.
#
# This asserts the median stays at or under 10 ticks. 8 was the original
# threshold, picked from this machine's own local numbers (6 vs 11), but
# CI's shared runner is slower and a genuinely time-based animation lands
# closer to its real 90ms target on a slower machine, not further from it:
# CI measured a clean 9-tick median on the real fix, which is CLOSER to the
# intended 9-tick (90ms) constant than the local 6, not a regression. 8 sat
# between 6 and 9 and CI's own correct number tripped it. 10 keeps real
# headroom below the true broken symptom (11, and worse under any further
# frame-rate cost) while not flagging the fix's own correct behavior on a
# slower box. Proven discriminating by actually reverting the animation
# block to the fixed-hop version: real FAIL at median 11.
set -e
cd "$(dirname "$0")/../.."
make -s kernel.elf

NAME=jt-dockanim
cleanup() { pkill -9 -f "qemu-system-i386.*$NAME" >/dev/null 2>&1 || true; }
trap cleanup EXIT

PORT=4497
LOG=/tmp/jt-dockanim-check.log
rm -f "$LOG"
qemu-system-i386 -kernel kernel.elf -display none -vga std \
    -qmp "tcp:127.0.0.1:$PORT,server,nowait" -serial "file:$LOG" -name "$NAME" &

set +e
python3 - "$PORT" "$LOG" <<'PYEOF'
import json, socket, sys, time
port, log_path = int(sys.argv[1]), sys.argv[2]

s = None
for _ in range(60):
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
f.readline(); cmd({"execute": "qmp_capabilities"})
time.sleep(3.0)

LOGICAL_W, LOGICAL_H = 960, 540
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247   # same dock constants as mwkeyflash-check.sh
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487

def move(x, y):
    cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
        {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})

def read_from(mark):
    with open(log_path) as fh:
        fh.seek(mark); return fh.read()
def size():
    try:
        with open(log_path) as fh: return len(fh.read())
    except FileNotFoundError: return 0

durations = []
steps = []
frames = []
for slot in (2, 4, 6, 8, 3, 5):
    move(480, 200); time.sleep(1.2)          # off the dock, let every slot decay to 0
    mark = size()
    move(SLOT0_X + slot * PITCH + DOCK_ICON // 2, ICON_ROW_Y)
    time.sleep(1.5)
    block = read_from(mark).splitlines()
    got = [int(l.split("=")[1]) for l in block if l.startswith("dockmag=")]
    sizes = [int(l.split("=")[1]) for l in block if l.startswith("dockstep=")]
    got_frames = [int(l.split("=")[1]) for l in block if l.startswith("dockframe=")]
    if got:
        durations.append(got[0])
    if sizes and got_frames:
        steps.append(sizes[0])
        frames.append(got_frames[0])

cmd({"execute": "quit"})

if len(durations) < 4:
    print("FAIL: only %d of 6 hovers produced a dockmag= marker, the hover path did not run"
          % len(durations)); sys.exit(1)
d = sorted(durations)
median = d[len(d) // 2]
print("magnify durations (PIT ticks, 10ms each): %s  median=%d (%d ms)"
      % (d, median, median * 10))
if median > 10:
    print("FAIL: the dock magnify takes %d ticks (%d ms), back to advancing by a fixed hop per "
          "poll instead of by elapsed time" % (median, median * 10)); sys.exit(1)
if len(steps) < 4:
    print("FAIL: only %d of 6 hovers produced a dockstep=/dockframe= pair, the smoothness markers "
          "are missing" % len(steps)); sys.exit(1)
st = sorted(steps)
st_median = st[len(st) // 2]
fr = sorted(frames)
fr_median = fr[len(fr) // 2]
print("distinct sizes per magnify: %s  median=%d" % (st, st_median))
print("frames per magnify:         %s  median=%d" % (fr, fr_median))
if st_median < 3:
    print("FAIL: the dock magnify shows only %d distinct sizes, so it reads as a jump rather than a "
          "zoom however fast it finishes" % st_median); sys.exit(1)
if st_median + 1 < fr_median:
    print("FAIL: the dock magnify showed %d distinct sizes across the %d frames it was given, so the "
          "animation's own quantum is coarser than the frame rate and frames go by redrawing nothing "
          "new" % (st_median, fr_median)); sys.exit(1)
print("PASS: the dock magnify finishes in %d ticks (%d ms) and shows %d distinct sizes across the "
      "%d frames it gets, time-based and as fine as the frame rate allows"
      % (median, median * 10, st_median, fr_median))
PYEOF
STATUS=$?
set -e
cleanup
exit $STATUS
