#!/bin/bash
# The small tool picker (kernel/picker.c) boots headless with the network off and agrees with its Python reference.
# `pickcheck` runs 297 golden sentences compiled into the kernel (answers from turing/training/smallpicker/ref.py) and must
# print "pickcheck: 297/297 ok". Four sentences are also typed through the shell as `pick ...`, so the command path is proved too.
# Reverting kernel/picker.c (or the weights) makes either line fail. QEMU gets -nic none: the picker needs no network.
set -e
cd "$(dirname "$0")/../.."
make -s kernel.elf

WORKDIR=$(mktemp -d)
trap 'rm -rf "$WORKDIR"' EXIT
VGA="$WORKDIR/vga"; LOG="$WORKDIR/log"

send() {
    local s="$1" i c
    for (( i=0; i<${#s}; i++ )); do
        c="${s:$i:1}"
        case "$c" in
            " ") echo "sendkey spc" ;;
            *)   echo "sendkey $c" ;;
        esac
    done
    echo "sendkey ret"
}

(
    sleep 3
    echo 'sendkey ctrl-alt-backspace'; sleep 1    # leave the GUI for the text shell
    send "pickcheck"; sleep 2
    send "pick open prs on turing"; sleep 2
    send "pick check my inbox"; sleep 2
    send "pick read the current tab"; sleep 2
    send "pick tell me a joke"; sleep 2
    echo 'xp /4000xb 0x000b8000'
    sleep 1
    echo quit
) | qemu-system-i386 -kernel kernel.elf -display none -monitor stdio -nic none > "$LOG" 2>&1

python3 - "$LOG" "$VGA" <<'PYEOF'
import re, sys
bytes_ = []
for l in open(sys.argv[1]).readlines():
    m = re.match(r'^[0-9a-f]+:\s+(.*)', l.strip())
    if m: bytes_.extend(int(x, 16) for x in m.group(1).split())
chars = bytes_[0::2]
text = ''.join(chr(c) if 32 <= c < 127 else '.' for c in chars)
with open(sys.argv[2], 'w') as f:
    for i in range(0, len(text), 80):
        row = text[i:i+80].rstrip('. ')
        if row: f.write(row + '\n')
PYEOF

fail=0
for want in "pickcheck: 297/297 ok" "pick: open_prs" "pick: unread_mail" "pick: read_tab" "pick: null"; do
    if grep -q "$want" "$VGA"; then echo "ok: $want"; else echo "FAIL: missing '$want'"; fail=1; fi
done
[ $fail = 0 ] || { cat "$VGA"; exit 1; }
echo "PASS: the C picker agrees with its Python reference"
