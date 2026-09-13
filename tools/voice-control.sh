#!/bin/bash
# Voice control for Joshua Tree, kept a separate host-side layer on purpose
# (see roadmap.md's v10 note: no audio driver, no STT/TTS on bare metal,
# that stays gato's job, not this kernel's). Records a short mic clip,
# transcribes it with a local whisper.cpp model, asks a local Ollama model
# to map the transcript onto one of this kernel's real shell commands, and
# types that command into a running QEMU instance through its monitor
# socket, the same keystroke-injection technique this whole project's own
# testing has used all along, not a new interface built just for this.
#
# Requires QEMU already running with a monitor socket:
#   qemu-system-i386 -kernel kernel.elf -display none \
#     -monitor unix:/tmp/jt-monitor.sock,server,nowait
set -e

MODEL_STT=~/.cache/whisper-cpp/ggml-base.en.bin
MODEL_LLM=llama3.1:8b
CLIP=/tmp/jt-voice-clip.wav
MONSOCK=/tmp/jt-monitor.sock
RECORD_SECONDS=${1:-4}

if [ ! -S "$MONSOCK" ]; then
    echo "no QEMU monitor socket at $MONSOCK -- start the kernel first"
    exit 1
fi

echo "listening... (speak now, ${RECORD_SECONDS}s)"
rec -q -r 16000 -c 1 "$CLIP" trim 0 "$RECORD_SECONDS" 2>/dev/null

echo "transcribing..."
TEXT=$(whisper-cli -m "$MODEL_STT" -f "$CLIP" -nt -np 2>/dev/null | tr -d '\n' | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
echo "heard: $TEXT"

if [ -z "$TEXT" ] || [ "$TEXT" = "[BLANK_AUDIO]" ]; then
    echo "(nothing heard)"
    exit 0
fi

read -r -d '' PROMPT_TEMPLATE <<'EOF' || true
You control a minimal OS kernel shell called Joshua Tree. Its real commands
are exactly: help clear echo time uptime mem reboot ls cat exec rm cd mkdir
browse lspci gfxtest fonttest mousetest nettest web serve serveapp chat
build gui. Given a spoken request, reply with ONLY the exact command line
to type, nothing else: no explanation, no quotes, no markdown. If the
request needs an argument (a filename, a chat message), include it plainly
after the command. Any open-ended question, or anything asking to ask/tell
something to the AI, maps to "chat <the question or message>", not to
"echo not supported": that fallback is only for requests with no reasonable
mapping at all (e.g. "turn off the lights").

Examples:
Request: what's the capital of france
chat what is the capital of france
Request: how much memory is free
mem
Request: open the desktop
gui
Request: turn off the lights
echo not supported

Request:
EOF

FULL_PROMPT="${PROMPT_TEMPLATE} ${TEXT}"

CMD=$(python3 - "$MODEL_LLM" "$FULL_PROMPT" <<'PYEOF'
import json, sys, urllib.request
model, prompt = sys.argv[1], sys.argv[2]
body = json.dumps({"model": model, "stream": False, "prompt": prompt}).encode()
req = urllib.request.Request("http://localhost:11434/api/generate", data=body,
                              headers={"Content-Type": "application/json"})
with urllib.request.urlopen(req, timeout=60) as r:
    resp = json.load(r)["response"].strip()
# strip a thinking model's <think>...</think> preamble if present
if "</think>" in resp:
    resp = resp.split("</think>", 1)[1].strip()
lines = [l.strip() for l in resp.splitlines() if l.strip()]
print(lines[-1] if lines else "")
PYEOF
)

echo "command: $CMD"

if [ -z "$CMD" ]; then
    echo "(LLM gave no command)"
    exit 0
fi

# QEMU's monitor sendkey only takes plain lowercase key names: an uppercase
# letter needs an explicit shift-<key> combo and most punctuation has no
# single-character key name at all. Real bug this exact gap produced the
# first time this ran: "chat What is the capital of France?" silently
# dropped every capital and the "?", landing as "chat hat is the capital of
# rance". None of this kernel's commands need capitalization or punctuation
# to work, so lowercase everything and drop anything that isn't a letter,
# digit, or space, rather than mistype the LLM's answer into the kernel.
CMD=$(printf '%s' "$CMD" | tr '[:upper:]' '[:lower:]' | tr -cd 'a-z0-9 ')

echo "typing: $CMD"

# Type the command into the real, running kernel shell one character at a
# time via QEMU's monitor sendkey, exactly like this project's own test
# harness has done all session, then press Enter.
{
    for (( i=0; i<${#CMD}; i++ )); do
        c="${CMD:$i:1}"
        case "$c" in
            " ") echo "sendkey spc" ;;
            *)   echo "sendkey $c" ;;
        esac
        sleep 0.02
    done
    echo "sendkey ret"
    sleep 0.3
} | nc -U -w2 "$MONSOCK" > /dev/null

echo "sent to the kernel."
