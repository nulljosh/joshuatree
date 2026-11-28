#!/bin/sh
# One time, on Joshua's Mac (13.27.4): CI's release key, so every release a Pi sees is signed by release.yml and installs
# with no step on the Mac. It makes a P-256 key (mode 600, never replaced; ~/.config/joshuatree/release-signing.pem by
# default), stores it as the repo's JT_RELEASE_KEY secret with `gh secret set` (needs gh signed in), and writes its public
# half to arch/arm64/release_key.h. The kernel trusts that key for the OS image only ("os.image", appsig_os_ok), never for an
# app, so CI can ship OS updates but cannot sign store apps; tools/signapp.sh's app key stays on the Mac.
# The private key is never printed. Commit arch/arm64/release_key.h (or paste the RELEASE_KEY line it prints to Claude).
#   tools/release-key.sh [KEY.pem]
set -e
root=$(cd "$(dirname "$0")/.." && pwd)
key=${1:-$HOME/.config/joshuatree/release-signing.pem}
hdr=$root/arch/arm64/release_key.h
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
command -v gh >/dev/null && command -v openssl >/dev/null || { echo "release-key: needs gh and openssl"; exit 1; }
if [ ! -f "$key" ]; then
    mkdir -p "$(dirname "$key")"
    (umask 077; openssl ecparam -name prime256v1 -genkey -noout -out "$key")
fi
chmod 600 "$key"
openssl ec -in "$key" -pubout -outform DER 2>/dev/null | tail -c 65 > "$tmp/pub"   # an SPKI's last 65 bytes: 0x04, X, Y
hex=$(od -An -v -tx1 "$tmp/pub" | tr -d ' \n')
[ "${#hex}" = 130 ] && [ "${hex%"${hex#04}"}" = 04 ] || { echo "release-key: could not read the public half of $key"; exit 1; }
python3 - "$hex" "$hdr" <<'PY'
import sys
h, out = sys.argv[1], sys.argv[2]
b = [h[i:i + 2] for i in range(0, 130, 2)]
rows = ",\n    ".join(", ".join("0x" + x for x in b[i:i + 13]) for i in range(0, 65, 13))
open(out, "w").write("/* CI's release key, public half (13.27.4): release.yml signs each release's kernel8.img with it (the JT_RELEASE_KEY secret), so a\n"
                     "   Pi updates straight from GitHub with no step on Joshua's Mac. It is trusted for the OS image only, never for an app\n"
                     "   (appsig.c's appsig_os_ok). Written by tools/release-key.sh; all zero means no CI key yet, and only the app key signs. */\n"
                     "static const unsigned char release_key[65] = {\n    " + rows + "\n};\n")
PY
gh secret set JT_RELEASE_KEY -R nulljosh/joshuatree < "$key"
echo "release-key: JT_RELEASE_KEY is set and arch/arm64/release_key.h is written."
echo "Commit that file, or paste this line to Claude:"
echo "RELEASE_KEY $hex"
