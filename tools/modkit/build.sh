#!/usr/bin/env bash
# Builds the mod kit program, out/dcmod (out/dcmod.exe on Windows). On Windows it uses the MSYS2 CLANG64 toolchain
# like the rest of the project; on Linux any C++17 compiler will do.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
[[ -f msys2-env.sh ]] && source ./msys2-env.sh
mkdir -p out
bash tools/modkit/lua51.sh
bash tools/modkit/atrac9.sh
cxx=${CXX:-clang++}
command -v "$cxx" >/dev/null || cxx=g++
if [[ ${OS:-} == Windows_NT ]]; then
    exe=out/dcmod.exe
    link=(-static -lws2_32)
else
    exe=out/dcmod
    link=(-static-libstdc++ -static-libgcc)
fi
"$cxx" -std=c++17 -O2 -Wall -Wextra -o "$exe" -Ithird_party/LibAtrac9/C/src tools/modkit/src/*.cpp out/lua51/*.o out/atrac9/*.o "${link[@]}"
echo "Built $PWD/$exe"
