#!/bin/bash
# Headless QA dogfood run, recorded to video. Boots the real kernel with no
# window, drives it like a user over QMP (absolute pointer clicks and real
# keystrokes, the same mechanism tools/checks/mwkeyflash-check.sh proved
# works), seeds real demo content, and records the whole session as frames
# that become an mp4 plus a gif.
#
# Nothing here needs a display or a human, so it is the QA ritual to run
# before every MINOR release (0.80.0, 0.90.0, 1.0.0) per the owner's own
# standing request, alongside CLAUDE.md's rule 4f bundling requirement.
#
# Usage: tools/qa-demo.sh          writes /tmp/jt-qa/jt-qa.mp4 and .gif
set -u
cd "$(dirname "$0")/.."
make -s kernel.elf

OUT=/tmp/jt-qa
rm -rf "$OUT"; mkdir -p "$OUT/frames"
PORT=4496
cleanup() { pkill -9 -f "qemu-system-i386.*jt-qademo" >/dev/null 2>&1 || true; }
trap cleanup EXIT

qemu-system-i386 -kernel kernel.elf -display none -vga std \
    -rtc base=localtime -net nic,model=rtl8139 -net user \
    -drive file=dotfiles.img,format=raw,if=ide,index=0 \
    -qmp "tcp:127.0.0.1:$PORT,server,nowait" -serial "file:$OUT/serial.log" \
    -name jt-qademo &

python3 tools/qa_demo_drive.py "$PORT" "$OUT"
STATUS=$?

# Frames are PPM at whatever the kernel's real mode is; ffmpeg reads them
# directly. 6fps keeps the file small and still shows every real transition.
if ls "$OUT"/frames/*.png >/dev/null 2>&1; then
    ffmpeg -y -loglevel error -framerate 6 -pattern_type glob -i "$OUT/frames/*.png" \
        -vf "scale=960:-2:flags=lanczos" -pix_fmt yuv420p "$OUT/jt-qa.mp4"
    ffmpeg -y -loglevel error -framerate 6 -pattern_type glob -i "$OUT/frames/*.png" \
        -vf "fps=6,scale=720:-2:flags=lanczos,split[a][b];[a]palettegen[p];[b][p]paletteuse" \
        "$OUT/jt-qa.gif"
    echo "video: $OUT/jt-qa.mp4"
    echo "gif:   $OUT/jt-qa.gif"
fi
cleanup
exit $STATUS
