#!/bin/bash
# v85: headless in-kernel check for the Chat rework (real VFS-backed
# history, /api/chat with full history in the request, buffer growth past
# the old 512-byte input cap). Same shape as tools/checks/png-check.sh: boots
# kernel.elf under `-display none`, drops out of the GUI with esc, types
# `chattest` through the monitor's sendkey path, and reads the pass/fail
# marker off the serial port (chattest has no screen output of its own,
# and QEMU's `sendkey ret` doesn't reliably reach real interactive text
# entry per CLAUDE.md, but this precedent already works for pngtest's own
# no-argument command the same way). No network round-trip here on
# purpose, same line pngtest already draws for image decoding vs a real
# host fetch: chat_send's own http_post/tcp_get call is exercised for
# real by the shell `chat <msg>` command and the GUI Chat app against a
# live Ollama host, not by this fast, network-independent regression
# check.
set -e
cd "$(dirname "$0")/.."
make -s kernel.elf

WORKDIR=$(mktemp -d /tmp/jt-chat-XXXX)
trap 'rm -rf "$WORKDIR"' EXIT
LOG="$WORKDIR/serial"

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
    echo 'sendkey esc'; sleep 1
    send "chattest"; sleep 4
    echo quit
) | qemu-system-i386 -kernel kernel.elf -display none -monitor stdio -serial "file:$LOG" >/dev/null 2>&1

if grep -q "chattest PASS" "$LOG"; then
    echo "PASS: chat history round-trips through CHAT.TXT, 600-char message survives past the old 512-byte cap, /api/chat request carries both turns, ring bound holds at CHAT_MAX"
    exit 0
fi
echo "FAIL: chattest did not report PASS on serial"
tail -c 1200 "$LOG" 2>/dev/null || true
exit 1
