#!/usr/bin/env bash
# Builds the macOS runtime: out/driveclub-probe, out/gpu/libbbgpu.dylib, out/driveclub-prepare and
# out/driveclub-launcher. Everything is x86_64: the game's own code is x86-64 and runs on the
# host CPU, which on Apple silicon is Rosetta 2. Vulkan is the Khronos loader over MoltenVK.
#
#   bash packaging/macos/fetch-deps.sh   # once
#   bash build-macos.sh
#
# Environment:
#   BB_LTO            ON/OFF link-time optimization of libbbgpu (default OFF on macOS)
#   BB_DEPS_DIR       prefix with the macOS build dependencies (default out/deps-macos)
set -euo pipefail
cd -- "$(dirname -- "$0")"
deps=${BB_DEPS_DIR:-$PWD/out/deps-macos}
[ -f "$deps/deps.env" ] || { echo "Run packaging/macos/fetch-deps.sh first." >&2; exit 1; }
# shellcheck disable=SC1090
. "$deps/deps.env"
arch=x86_64
export MACOSX_DEPLOYMENT_TARGET=${MACOSX_DEPLOYMENT_TARGET:-13.0}
CC=${CC:-clang}
CXX=${CXX:-clang++}
for tool in cmake ninja pkg-config; do
    command -v "$tool" >/dev/null || { echo "Need $tool (brew install $tool)." >&2; exit 1; }
done
read -r -a includes <<< "$(pkg-config --cflags vulkan sdl3)"
read -r -a libraries <<< "$(pkg-config --libs vulkan sdl3)"
read -r -a sdl_cflags <<< "$(pkg-config --cflags sdl3)"
read -r -a sdl_libs <<< "$(pkg-config --libs sdl3)"

mkdir -p out
cmake -S gpu -B out/gpu -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_OSX_ARCHITECTURES=$arch \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$MACOSX_DEPLOYMENT_TARGET" \
    -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
    -DCMAKE_PREFIX_PATH="$deps" -DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew \
    -DCMAKE_FIND_FRAMEWORK=LAST \
    -DProtobuf_PROTOC_EXECUTABLE="$deps/bin/protoc" -DBoost_INCLUDE_DIR="$deps/include" \
    -DBB_LTO="${BB_LTO:-OFF}" -DBB_PGO=off -DBB_DIAGNOSTICS=OFF \
    -DCMAKE_C_FLAGS="-ffile-prefix-map=$PWD=." \
    -DCMAKE_CXX_FLAGS="-ffile-prefix-map=$PWD=." >/dev/null
echo "GPU library: $("$CXX" --version | head -1), $arch"
if ! ninja -C out/gpu bbgpu > out/gpu-build.log 2>&1; then
    grep -v '^[[]' out/gpu-build.log | grep -E 'error|Error' -A3 | head -60 >&2
    echo 'GPU library build failed (full log: out/gpu-build.log)' >&2
    exit 1
fi

if [ ! -f out/libatrac9.a ] || [ -n "$(find third_party/LibAtrac9/C/src -newer out/libatrac9.a -name '*.c' -print -quit)" ]; then
    rm -rf out/atrac9 && mkdir -p out/atrac9
    for source in third_party/LibAtrac9/C/src/*.c; do
        "$CC" -arch $arch -std=c99 -O2 -g -w -c "$source" -o "out/atrac9/$(basename "${source%.c}").o"
    done
    ar rcs out/libatrac9.a out/atrac9/*.o
fi

# compat_macos.h maps the Linux runtime's few Linux-only calls to Darwin and keeps its heap low.
cflags=(-arch $arch -std=c11 -O2 -g -Wall -Wextra -Wno-unknown-pragmas -Wno-unknown-warning-option
        -Werror=implicit-function-declaration -Werror=incompatible-pointer-types -pthread
        -include src/compat_macos.h -ffile-prefix-map="$PWD"=.)
runtime=(src/runtime*.c)
"$CC" "${cflags[@]}" "${includes[@]}" -I. -Isrc src/probe.c "${runtime[@]}" src/vulkan_smoke.c src/compat_macos.c \
    out/libatrac9.a -Lout/gpu -lbbgpu \
    -Wl,-rpath,@executable_path/gpu -Wl,-rpath,@executable_path/../Frameworks -Wl,-rpath,"$deps/lib" \
    -Wl,-export_dynamic "${libraries[@]}" -lm -o out/driveclub-probe

"$CXX" -arch $arch -std=c++20 -O2 -g -Wall -Wextra -Ilauncher tools/prepare_cli.cpp launcher/prepare.cpp \
    -o out/driveclub-prepare

imgui=gpu/third_party/imgui
"$CXX" -arch $arch -std=c++20 -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
    -Ilauncher -I"$imgui" -I"$imgui/backends" \
    tools/launcher_linux.cpp tools/unlock_linux.cpp launcher/prepare.cpp \
    "$imgui/imgui.cpp" "$imgui/imgui_draw.cpp" "$imgui/imgui_tables.cpp" "$imgui/imgui_widgets.cpp" \
    "$imgui/backends/imgui_impl_sdl3.cpp" "$imgui/backends/imgui_impl_sdlrenderer3.cpp" \
    "${sdl_cflags[@]}" "${sdl_libs[@]}" -Wl,-rpath,"$deps/lib" -pthread -o out/driveclub-launcher

# The video core points the Vulkan loader at the executable's folder on macOS (vk_platform.cpp).
# Replaced only when it changed, and by rename: copying over the file a running game has mapped
# corrupts that game's code and Rosetta's translation of it ("code signature supplement failed").
if ! cmp -s "$deps/lib/libMoltenVK.dylib" out/libMoltenVK.dylib; then
    cp "$deps/lib/libMoltenVK.dylib" out/libMoltenVK.dylib.new && mv -f out/libMoltenVK.dylib.new out/libMoltenVK.dylib
fi
sed 's|"library_path": *"[^"]*"|"library_path": "./libMoltenVK.dylib"|' \
    "$deps/share/vulkan/icd.d/MoltenVK_icd.json" > out/MoltenVK_icd.json

echo "Built $PWD/out/driveclub-probe"
echo "Built $PWD/out/driveclub-prepare"
echo "Built $PWD/out/driveclub-launcher"
