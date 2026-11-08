#!/usr/bin/env bash
# Builds the game runtime (out/DriveClubGame.exe) with the MSYS2 CLANG64 toolchain. See BUILDING.md.
set -euo pipefail
cd -- "$(dirname -- "$0")"
mkdir -p out
# shellcheck source=msys2-env.sh
source ./msys2-env.sh
for tool in clang cmake ninja pkg-config; do
    command -v "$tool" >/dev/null || { echo "Need $tool from MSYS2 CLANG64 (see BUILDING.md)." >&2; exit 1; }
done
# Source paths in the binary (assert and log messages) are written relative to the repository.
root="$(cygpath -m "$PWD")"
case "${BB_DIAGNOSTICS:-OFF}" in
    ON|on|1|TRUE|true) diagnostic=ON; build=out/gpu-diagnostics; output=out/DriveClubGame.diagnostics.exe ;;
    OFF|off|0|FALSE|false) diagnostic=OFF; build=out/gpu; output=out/DriveClubGame.exe ;;
    *) echo 'BB_DIAGNOSTICS must be ON or OFF.' >&2; exit 1 ;;
esac

# BB_PGO=use with BB_LTO=ON builds with the recorded profile (pgo/default.profdata); the default build is the fast one.
cmake -S gpu -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBB_PGO="${BB_PGO:-off}" \
    -DBB_DIAGNOSTICS="$diagnostic" -DBB_LTO="${BB_LTO:-OFF}" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_C_FLAGS="-ffile-prefix-map=$root=." -DCMAKE_CXX_FLAGS="-ffile-prefix-map=$root=." >/dev/null
if ! ninja -C "$build" DriveClubGame > "$build-build.log" 2>&1; then
    grep -v '^[[]' "$build-build.log" | tail -40 >&2
    echo "Build failed (full log: $build-build.log)" >&2
    exit 1
fi
cp "$build/DriveClubGame.exe" "$output"
echo "Built $PWD/$output"
