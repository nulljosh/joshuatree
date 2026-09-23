#!/bin/bash
# MANUAL: needs a real Ollama server reachable at the host's SLIRP gateway
# address, which no CI runner has. Passes on this machine because Ollama
# is running locally; run by hand, not in ci-suite.sh.
# v0.85.4: headless, real-network proof that the shell `chat` command really
# reaches the host's Ollama server and gets a real reply back, the same
# "real fetch, not just a built URL" bar geo-check.sh already holds weather
# to. chattest (chat-check.sh) is deliberately network-free (see its own
# comment); this is the live counterpart, same "-net nic,model=rtl8139 -net
# user" SLIRP setup geo-check.sh already uses to reach 10.0.2.2 (the host,
# from QEMU's own gateway address). Reads the `chatreply=` line chat_send
# (kernel/chat.h) mirrors to serial for exactly this purpose, and fails if
# the model's own <think> block leaked into the answer (qwen3:8b is a
# reasoning model; chat_send sends "think":false to Ollama, this is the
# proof that option actually suppressed it end to end, not just that the
# field was present in the request).
set -e
cd "$(dirname "$0")/../.."
make -s kernel.elf

WORKDIR=$(mktemp -d /tmp/jt-chatlive-XXXX)
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
    send "chat say the word banana and nothing else"
    sleep 90
    echo quit
) | qemu-system-i386 -kernel kernel.elf -display none -vga std \
    -net nic,model=rtl8139 -net user -monitor stdio -serial "file:$LOG" >/dev/null 2>&1

reply=$(grep '^chatreply=' "$LOG" | head -1 | tr -d '\r' | cut -d= -f2-)

if [ -z "$reply" ]; then
    echo "FAIL: no chatreply= line on serial (no reply from the host's Ollama server)"
    tail -c 1200 "$LOG" 2>/dev/null || true
    exit 1
fi

case "$reply" in
    *"<think>"*) echo "FAIL: raw <think> block leaked into the chat reply: $reply"; exit 1 ;;
esac

echo "PASS: real reply from the host's Ollama server arrived, no <think> leakage:"
echo "  $reply"
