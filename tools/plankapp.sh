#!/bin/bash
# Plank programs as Joshua Tree apps (6.57.0): tools/plankapp.sh FILE.pk [OUT.APP]
# Plank (github.com/nulljosh/plank) compiles FILE.pk to LLVM IR (`plank emit`); clang builds that IR and Plank's own runtime for
# the Pi with the flags the built-in apps use; user/pklibc is the C library under them, libjt the malloc and strings; ld.lld
# links it all with user_c.ld into an image the card takes (JT/APPS/NAME.APP, then `userrun NAME`). PLANK names plank.py
# (default: ~/Documents/Code/plank/plank.py, else the one beside `plank` on PATH); it needs uv, as plank.py does.
set -e
src=$1
[ -f "$src" ] || { echo "usage: tools/plankapp.sh FILE.pk [OUT.APP]"; exit 1; }
out=${2:-$(basename "$src" .pk | tr a-z A-Z).APP}
name=$(basename "$out" .APP)
case "$name" in ''|?????????*|*[!A-Z0-9_-]*) echo "plankapp: $name.APP: the name must be 1 to 8 of A-Z, 0-9, - and _"; exit 1;; esac
root=$(cd "$(dirname "$0")/.." && pwd)
plank=${PLANK:-$HOME/Documents/Code/plank/plank.py}
[ -f "$plank" ] || { echo "plankapp: no plank.py at $plank (set PLANK=)"; exit 1; }
dir=$(mktemp -d); trap 'rm -rf "$dir"' EXIT
"$plank" emit "$src" > "$dir/prog.ll"
uv run --quiet --script "$plank" --version >/dev/null 2>&1 || true
uv run --quiet --with llvmlite python3 -c 'import importlib.util, sys
s = importlib.util.spec_from_file_location("plank", sys.argv[1]); m = importlib.util.module_from_spec(s); s.loader.exec_module(m)
sys.stdout.write(m.RUNTIME)' "$plank" > "$dir/runtime.c"
CC=(clang -target aarch64-none-elf -ffreestanding -nostdlibinc -fno-stack-protector -mstrict-align -fPIE -fdirect-access-external-data
    -fno-builtin -fno-asynchronous-unwind-tables -Os -Wno-unused-command-line-argument -I"$root/user" -I"$root/user/pklibc/include")
"${CC[@]}" -Wno-override-module -c "$dir/prog.ll" -o "$dir/prog.o"
"${CC[@]}" -w -c "$dir/runtime.c" -o "$dir/runtime.o"
"${CC[@]}" -Wall -c "$root/user/pklibc/pklibc.c" -o "$dir/pklibc.o"
"${CC[@]}" -c "$root/user/pklibc/setjmp.S" -o "$dir/setjmp.o"
"${CC[@]}" -mgeneral-regs-only -c "$root/user/pklibc/jtw.c" -o "$dir/jtw.o"   # 6.58.0: a window, linked only when the program calls jtw_*
for f in string stdlib text; do "${CC[@]}" -mgeneral-regs-only -I"$root/user/libjt" -c "$root/user/libjt/$f.c" -o "$dir/jt_$f.o"; done
ld.lld -pie --no-dynamic-linker -z norelro -T "$root/arch/arm64/user_c.ld" --oformat binary -o "$out" \
    "$dir/pklibc.o" "$dir/prog.o" "$dir/runtime.o" "$dir/setjmp.o" --start-lib "$dir/jtw.o" "$dir/jt_string.o" "$dir/jt_stdlib.o" "$dir/jt_text.o" --end-lib
[ "$(tail -c 16 "$out" | head -c 4)" = JTU1 ] || { rm -f "$out"; echo "plankapp: the JTU1 trailer is not last"; exit 1; }
echo "plankapp: made $out, $(wc -c < "$out" | tr -d ' ') bytes; copy it to JT/APPS/ on the card, then: userrun $(echo "$name" | tr A-Z a-z)"
