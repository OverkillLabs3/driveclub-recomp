#!/usr/bin/env bash
# usage: pgo_build.sh generate|use|lto   (local test helper, not part of the repository)
set -euo pipefail
cd -- "$(dirname -- "$0")"
source ./msys2-env.sh
mode="$1"
root="$(cygpath -m "$PWD")"
pgo=off; [ "$mode" = generate ] && pgo=generate; [ "$mode" = use ] && pgo=use
build="out/gpu-pgo-$mode"
cmake -S gpu -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBB_PGO="$pgo" -DBB_PGO_DIR="$root/pgo" \
    -DBB_DIAGNOSTICS=OFF -DBB_LTO=ON -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_C_FLAGS="-ffile-prefix-map=$root=." -DCMAKE_CXX_FLAGS="-ffile-prefix-map=$root=." >/dev/null
ninja -C "$build" DriveClubGame > "$build-build.log" 2>&1 || { tail -30 "$build-build.log" >&2; exit 1; }
cp "$build/DriveClubGame.exe" "out/DriveClubGame.pgo-$mode.exe"
echo "Built out/DriveClubGame.pgo-$mode.exe"
