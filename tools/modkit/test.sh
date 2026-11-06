#!/usr/bin/env bash
# Builds and runs the mod kit's self-test (made-up data, no game files needed).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
[[ -f msys2-env.sh ]] && source ./msys2-env.sh
mkdir -p out
bash tools/modkit/lua51.sh
bash tools/modkit/atrac9.sh
cxx=${CXX:-clang++}
command -v "$cxx" >/dev/null || cxx=g++
src=$(ls tools/modkit/src/*.cpp | grep -v '/main.cpp$' | grep -v '/cmd_')
"$cxx" -std=c++17 -O1 -Wall -Wextra -Ithird_party/LibAtrac9/C/src -o out/modkit-selftest tools/modkit/tests/selftest.cpp $src out/lua51/*.o out/atrac9/*.o
./out/modkit-selftest
"$cxx" -std=c++17 -O2 -Wall -Wextra -Ithird_party/LibAtrac9/C/src -o out/at9test tools/modkit/tests/at9test.cpp tools/modkit/src/atrac9enc.cpp out/atrac9/*.o
./out/at9test
