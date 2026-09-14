#!/bin/sh
# v35 (0.35.0): keeps every embedded app's C byte array in sync with its
# real source repo. Real limit, not glossed over: this kernel has no
# network access to the host machine at boot and no package manager, a
# freestanding kernel genuinely cannot pull a "live update" the way a
# real OS with a running userspace could. What's real and achievable is
# build-time sync: run this before `make` (or via `make sync-apps`) and
# every embedded app is regenerated from whatever its source repo's
# index.html currently says, so a stale year-old copy never silently
# ships. Not wired into every `make` invocation automatically, on
# purpose, regenerating on every single incremental build would make
# `make` depend on every source repo existing and being readable, a real
# cost this kernel's own CI/local-dev loop shouldn't pay by default.
set -e
cd "$(dirname "$0")"
CODE="$HOME/Documents/Code"

# name|source repo path relative to $CODE|source file|output header|C array name
APPS="
weather|weather|web/index.html|drivers/app_weather.h|app_weather
curbfind|curbfind|web/index.html|drivers/app_curbfind.h|app_curbfind
keyrate|keyrate|index.html|drivers/app_keyrate.h|app_keyrate
bookrank|bookrank|index.html|drivers/app_bookrank.h|app_bookrank
quotestreak|quotestreak|index.html|drivers/app_quotestreak.h|app_quotestreak
plan|plan|index.html|drivers/app_plan.h|app_plan
lexly|lexly|index.html|drivers/app_lexly.h|app_lexly
toroid|conway|index.html|drivers/app_toroid.h|app_toroid
sparkjar|sparkjar|index.html|drivers/app_sparkjar.h|app_sparkjar
homeqi|homeqi|index.html|drivers/app_homeqi.h|app_homeqi
fieldbook|fieldbook|index.html|drivers/app_fieldbook.h|app_fieldbook
"

echo "$APPS" | while IFS='|' read -r name repo relpath out arrayname; do
    [ -z "$name" ] && continue
    src="$CODE/$repo/$relpath"
    if [ ! -f "$src" ]; then
        echo "skip $name: $src not found"
        continue
    fi
    ./gen_app.sh "$src" "$out" "$arrayname" | sed "s/^/$name: /"
done

echo "done. rebuild with: make clean && make"
