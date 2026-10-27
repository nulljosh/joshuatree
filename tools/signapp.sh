#!/bin/sh
# App signing (6.54.0). A signed app runs for anyone; an unsigned one only for an admin (see docs/GUIDE.md, Write an app).
#   tools/signapp.sh NAME.APP [KEY.pem]   writes NAME.SIG beside it, signed with KEY (default ~/.config/joshuatree/app-signing.pem)
#   tools/signapp.sh --new-key [KEY.pem] [HEADER.h]   makes a P-256 key (mode 600, never replaced) and writes its public half as
#                                                    the kernel's header (default arch/arm64/app_key.h); the private half never leaves KEY
# What is signed is SHA-256 of "JTAPP1\0" NAME "\0" IMAGE, NAME the file's name in lower case (arch/arm64/appsig.h).
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
def=$HOME/.config/joshuatree/app-signing.pem
if [ "$1" = --new-key ]; then
    key=${2:-$def}; hdr=${3:-$root/arch/arm64/app_key.h}
    [ -e "$key" ] && { echo "signapp: $key is there already; a key is never replaced"; exit 1; }
    (umask 077; openssl ecparam -name prime256v1 -genkey -noout -out "$key")
    openssl ec -in "$key" -pubout -outform DER 2>/dev/null | tail -c 65 | python3 -c '
import sys
b = sys.stdin.buffer.read(); assert len(b) == 65 and b[0] == 4, "not a P-256 point"
rows = ",\n    ".join(", ".join("0x%02x" % x for x in b[i:i + 13]) for i in range(0, 65, 13))
print("/* The app signing key'"'"'s public half (tools/signapp.sh --new-key): an uncompressed P-256 point. */\nstatic const unsigned char app_key[65] = {\n    " + rows + "\n};")' > "$hdr"
    echo "signapp: new key in $key, its public half in $hdr"
    exit 0
fi
app=$1; key=${2:-$def}
[ -f "$app" ] || { echo "usage: tools/signapp.sh NAME.APP [KEY.pem] | --new-key [KEY.pem] [HEADER.h]"; exit 1; }
[ -f "$key" ] || { echo "signapp: no key at $key (tools/signapp.sh --new-key makes one)"; exit 1; }
name=$(basename "$app" | sed 's/\.[Aa][Pp][Pp]$//' | tr A-Z a-z)
case "$name" in ''|?????????*|*[!a-z0-9_-]*) echo "signapp: $name: the name must be 1 to 8 of A-Z, 0-9, - and _"; exit 1;; esac
dir=$(mktemp -d); trap 'rm -rf "$dir"' EXIT
{ printf 'JTAPP1\000%s\000' "$name"; cat "$app"; } > "$dir/msg"
sig="${app%.*}.SIG"
openssl dgst -sha256 -sign "$key" -out "$sig" "$dir/msg"
echo "signapp: signed $app as $name: $sig"
