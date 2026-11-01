#!/usr/bin/env bash
# Builds the native x86_64 Linux runtime: out/driveclub-probe, out/gpu/libbbgpu.so, the
# out/driveclub-prepare helper and out/driveclub-launcher. See BUILDING.md.
#
# The Linux port follows the upstream ARM Linux branch (seregasmith/ARM_linux_driveclub-recomp,
# branch arm_linux). On x86_64 the game's own code runs on the host CPU; the guest CPU layer
# (src/guest_cpu.h) is a set of plain calls there, so no FEX is involved.
#
# Environment:
#   CC, CXX           compilers (default: cc and c++)
#   BB_LTO            ON/OFF link-time optimization of libbbgpu (default ON)
#   BB_PGO            off/generate/use (default off)
#   BB_PGO_DIR        profile directory (default pgo)
#   BB_VULKAN_HEADERS include directory with a recent vulkan/vulkan.h (see packaging/appimage/fetch-deps.sh)
#   BB_DEPS_DIR       prefix with the AppImage build dependencies (default out/deps)
set -euo pipefail
cd -- "$(dirname -- "$0")"
CC=${CC:-$(command -v cc || command -v gcc || true)}
CXX=${CXX:-$(command -v c++ || command -v g++ || true)}
[ -n "$CC" ] && [ -n "$CXX" ] || { echo 'Need a C and a C++ compiler (set CC and CXX).' >&2; exit 1; }
deps=${BB_DEPS_DIR:-$PWD/out/deps}
# Dependencies fetched by packaging/appimage/fetch-deps.sh (SDL3, a minimal FFmpeg, headers).
if [ -f "$deps/deps.env" ]; then
    # shellcheck disable=SC1090
    . "$deps/deps.env"
fi
vulkan_headers=${BB_VULKAN_HEADERS:-}
if [ -z "$vulkan_headers" ]; then
    for candidate in "$deps/include" "$deps/vulkan/include" "$PWD/gpu/third_party/Vulkan-Headers/include"; do
        if [ -f "$candidate/vulkan/vulkan.h" ]; then vulkan_headers=$candidate; break; fi
    done
fi

for tool in cmake ninja pkg-config ar; do
    command -v "$tool" >/dev/null || { echo "Need $tool (see BUILDING.md)." >&2; exit 1; }
done
missing=
for module in vulkan sdl3 libavformat libavcodec libavutil libswscale libswresample; do
    pkg-config --exists "$module" 2>/dev/null || missing="$missing $module"
done
if [ -n "$missing" ]; then
    echo "Missing pkg-config modules:$missing (see BUILDING.md; on a fresh tree run packaging/appimage/fetch-deps.sh)." >&2
    exit 1
fi
read -r -a includes <<< "$(pkg-config --cflags vulkan sdl3)"
read -r -a libraries <<< "$(pkg-config --libs vulkan sdl3)"
read -r -a sdl_cflags <<< "$(pkg-config --cflags sdl3)"
read -r -a sdl_libs <<< "$(pkg-config --libs sdl3)"

mkdir -p out pgo
pgo=${BB_PGO:-off}
pgo_dir=${BB_PGO_DIR:-$PWD/pgo}
cmake_args=(
    -S gpu -B out/gpu -G Ninja
    -DCMAKE_BUILD_TYPE=RelWithDebInfo
    -DBB_PGO="$pgo"
    -DBB_PGO_DIR="$pgo_dir"
    -DBB_LTO="${BB_LTO:-ON}"
    -DBB_DIAGNOSTICS=OFF
    -DCMAKE_C_COMPILER="$CC"
    -DCMAKE_CXX_COMPILER="$CXX"
    -DCMAKE_C_FLAGS="-ffile-prefix-map=$PWD=."
    -DCMAKE_CXX_FLAGS="-ffile-prefix-map=$PWD=."
)
if [ -n "$vulkan_headers" ]; then
    cmake_args+=(-DVulkan_INCLUDE_DIR="$vulkan_headers" -DCMAKE_DISABLE_FIND_PACKAGE_VulkanHeaders=ON)
fi
if [ -n "${CMAKE_PREFIX_PATH:-}" ]; then cmake_args+=(-DCMAKE_PREFIX_PATH="$CMAKE_PREFIX_PATH"); fi
cmake "${cmake_args[@]}" >/dev/null
echo "GPU library: $("$CXX" --version | head -1), PGO $pgo, LTO ${BB_LTO:-ON}"
# A failed GPU build must stop here: an older libbbgpu.so would otherwise be used silently.
if ! ninja -C out/gpu bbgpu > out/gpu-build.log 2>&1; then
    grep -v '^[[]' out/gpu-build.log | tail -40 >&2
    echo 'GPU library build failed (full log: out/gpu-build.log)' >&2
    exit 1
fi

# ATRAC9: the game's audio codec, compiled once without this project's warning policy.
if [ ! -f out/libatrac9.a ] || [ -n "$(find third_party/LibAtrac9/C/src -newer out/libatrac9.a -name '*.c' -print -quit)" ]; then
    rm -rf out/atrac9 && mkdir -p out/atrac9
    for source in third_party/LibAtrac9/C/src/*.c; do
        "$CC" -std=c99 -O2 -g -w -fPIC -c "$source" -o "out/atrac9/$(basename "${source%.c}").o"
    done
    ar rcs out/libatrac9.a out/atrac9/*.o
fi

# The loader is not position independent: the game packs host pointers (TLS, thread handles in the brk
# heap) into 40 bits, so the executable and its heap stay low.
cflags=(-std=c11 -O2 -g -Wall -Wextra -Wno-format-truncation -Wno-unknown-pragmas
        -Wno-unknown-warning-option -Werror=implicit-function-declaration
        -Werror=incompatible-pointer-types -pthread -fno-pie -ffile-prefix-map="$PWD"=.)
runtime=(src/runtime*.c)
"$CC" "${cflags[@]}" -no-pie "${includes[@]}" -I. -Isrc src/probe.c "${runtime[@]}" src/vulkan_smoke.c \
    out/libatrac9.a -Lout/gpu -lbbgpu \
    -Wl,-rpath,'$ORIGIN/../lib/driveclub-recomp/gpu' -Wl,-rpath,"$PWD/out/gpu" -rdynamic \
    "${libraries[@]}" -lm -ldl -o out/driveclub-probe

# First-run preparation of the user's game dumps: launcher/prepare.cpp is portable C++ and is driven by
# this small CLI instead of the Windows launcher.
"$CXX" -std=c++20 -O2 -g -Wall -Wextra -Ilauncher tools/prepare_cli.cpp launcher/prepare.cpp \
    -o out/driveclub-prepare

# The native Linux launcher. It uses the ImGui that ships with the GPU sources and the same
# SDL3 the runtime uses; the AppImage's AppRun starts it when no argument is given.
imgui=gpu/third_party/imgui
"$CXX" -std=c++20 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
    -Ilauncher -I"$imgui" -I"$imgui/backends" \
    tools/launcher_linux.cpp launcher/prepare.cpp \
    "$imgui/imgui.cpp" "$imgui/imgui_draw.cpp" "$imgui/imgui_tables.cpp" "$imgui/imgui_widgets.cpp" \
    "$imgui/backends/imgui_impl_sdl3.cpp" "$imgui/backends/imgui_impl_sdlrenderer3.cpp" \
    "${sdl_cflags[@]}" "${sdl_libs[@]}" -pthread -o out/driveclub-launcher

echo "Built $PWD/out/driveclub-probe"
echo "Built $PWD/out/driveclub-prepare"
echo "Built $PWD/out/driveclub-launcher"
