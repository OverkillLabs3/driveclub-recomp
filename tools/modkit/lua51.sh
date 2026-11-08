#!/usr/bin/env bash
# Compiles the Lua 5.1.5 compiler and base libraries in third_party/lua51 into out/lua51/*.o (used by build.sh and
# test.sh). Lua is MIT licensed; see third_party/lua51/COPYRIGHT.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
[[ -f msys2-env.sh ]] && source ./msys2-env.sh
cc=${CC:-clang}
command -v "$cc" >/dev/null || cc=gcc
mkdir -p out/lua51
for f in lapi lcode ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate lstring ltable ltm lundump lvm lzio lauxlib lbaselib lstrlib ltablib lmathlib; do
    if [[ ! -f out/lua51/$f.o || third_party/lua51/$f.c -nt out/lua51/$f.o ]]; then
        "$cc" -O2 -w -c -o "out/lua51/$f.o" "third_party/lua51/$f.c"
    fi
done
