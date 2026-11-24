#!/usr/bin/env bash
# Fetches and builds the macOS build dependencies as x86_64 libraries. The game's own code is
# x86-64, so on Apple silicon the whole runtime is an x86_64 process under Rosetta 2, and every
# library it links has to be x86_64 too (Homebrew on Apple silicon only has arm64 ones).
# Header-only packages (Boost, nlohmann-json) are taken from Homebrew; Vulkan is the Khronos
# loader over MoltenVK. Everything is installed under out/deps-macos (gitignored).
#
#   bash packaging/macos/fetch-deps.sh
#
# build-macos.sh sources out/deps-macos/deps.env.
#
# Host tools (arm64 Homebrew is fine for these): cmake ninja pkg-config nasm glslang
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
deps=${BB_DEPS_DIR:-$PWD/out/deps-macos}
src=$deps/src
cache=$deps/cache
jobs=$(sysctl -n hw.ncpu 2>/dev/null || echo 8)
arch=x86_64
min_macos=${MACOSX_DEPLOYMENT_TARGET:-13.0}
export MACOSX_DEPLOYMENT_TARGET=$min_macos

vulkan_version=${VULKAN_VERSION:-vulkan-sdk-1.4.363.0}
moltenvk_version=${MOLTENVK_VERSION:-v1.4.2}
sdl3_version=${SDL3_VERSION:-3.4.18}
ffmpeg_version=${FFMPEG_VERSION:-6.1.2}
fmt_version=${FMT_VERSION:-11.1.4}
xxhash_version=${XXHASH_VERSION:-v0.8.3}
zydis_version=${ZYDIS_VERSION:-v4.1.1}
protobuf_version=${PROTOBUF_VERSION:-v21.12}
openssl_version=${OPENSSL_VERSION:-openssl-3.5.4}
libpng_version=${LIBPNG_VERSION:-v1.6.50}
miniupnpc_version=${MINIUPNPC_VERSION:-2.3.5}
robin_version=${ROBIN_MAP_VERSION:-v1.4.1}
vma_version=${VMA_VERSION:-v3.5.0}
epoll_shim_version=${EPOLL_SHIM_VERSION:-v0.0.20240608}

for tool in cmake ninja pkg-config nasm curl git; do
    command -v "$tool" >/dev/null || { echo "Need $tool (brew install $tool)." >&2; exit 1; }
done
command -v glslang >/dev/null || command -v glslangValidator >/dev/null \
    || { echo 'Need glslang (brew install glslang).' >&2; exit 1; }
arch -x86_64 /usr/bin/true 2>/dev/null \
    || { echo 'Rosetta 2 is not installed: softwareupdate --install-rosetta' >&2; exit 1; }
mkdir -p "$src" "$cache" "$deps/include" "$deps/lib"

fetch() { # fetch URL DEST
    [ -s "$2" ] && return 0
    echo "Fetching $(basename -- "$2") ..."
    curl -fL --retry 3 --retry-delay 2 -o "$2.part" "$1"
    mv -- "$2.part" "$2"
}
unpack() { # unpack ARCHIVE DIR
    rm -rf "$2" && mkdir -p "$2"
    tar -xf "$1" --no-same-owner -C "$2" --strip-components=1
}
github() { # github OWNER/REPO TAG NAME -> $src/NAME
    local archive=$cache/$3-$2.tar.gz
    fetch "https://github.com/$1/archive/refs/tags/$2.tar.gz" "$archive"
    [ -f "$src/$3/.unpacked-$2" ] || { unpack "$archive" "$src/$3"; touch "$src/$3/.unpacked-$2"; }
}
cmake_x86() { # cmake_x86 SRC BUILD [ARGS...]
    local s=$1 b=$2; shift 2
    cmake -S "$s" -B "$b" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES=$arch -DCMAKE_OSX_DEPLOYMENT_TARGET="$min_macos" \
        -DCMAKE_INSTALL_PREFIX="$deps" -DCMAKE_PREFIX_PATH="$deps" -DCMAKE_FIND_ROOT_PATH="$deps" \
        -DCMAKE_IGNORE_PREFIX_PATH="/opt/homebrew" -DCMAKE_POLICY_VERSION_MINIMUM=3.5 "$@" >/dev/null
    cmake --build "$b" --parallel "$jobs" >/dev/null
    cmake --install "$b" >/dev/null
}
done_mark() { [ -f "$deps/.done-$1" ]; }
mark() { touch "$deps/.done-$1"; echo "$1 installed"; }

# --- Vulkan: headers, loader, MoltenVK --------------------------------------------------------
if ! done_mark "vulkan-headers-$vulkan_version"; then
    github KhronosGroup/Vulkan-Headers "$vulkan_version" Vulkan-Headers
    cmake_x86 "$src/Vulkan-Headers" "$src/Vulkan-Headers-build"
    mark "vulkan-headers-$vulkan_version"
fi
if ! done_mark "vulkan-loader-$vulkan_version"; then
    github KhronosGroup/Vulkan-Loader "$vulkan_version" Vulkan-Loader
    cmake_x86 "$src/Vulkan-Loader" "$src/Vulkan-Loader-build" -DBUILD_TESTS=OFF \
        -DVULKAN_HEADERS_INSTALL_DIR="$deps"
    mark "vulkan-loader-$vulkan_version"
fi
# MoltenVK is built from source with the patches in patches/moltenvk (Apple-silicon performance; the first build
# takes a while: its dependencies are built too). MOLTENVK_PREBUILT=1 installs the unpatched release instead.
moltenvk_patches=$(cat patches/moltenvk/*.patch 2>/dev/null | shasum | cut -c1-12)
moltenvk_mark=moltenvk-$moltenvk_version-$([ "${MOLTENVK_PREBUILT:-0}" = 1 ] && echo prebuilt || echo "src-$moltenvk_patches")
if ! done_mark "$moltenvk_mark"; then
    if [ "${MOLTENVK_PREBUILT:-0}" = 1 ]; then
        archive=$cache/MoltenVK-macos-$moltenvk_version.tar
        fetch "https://github.com/KhronosGroup/MoltenVK/releases/download/$moltenvk_version/MoltenVK-macos.tar" "$archive"
        rm -rf "$src/MoltenVK" && mkdir -p "$src/MoltenVK"
        tar -xf "$archive" --no-same-owner -C "$src/MoltenVK"
        dylib=$(find "$src/MoltenVK" -name libMoltenVK.dylib -path '*dynamic*' | head -1)
        [ -n "$dylib" ] || dylib=$(find "$src/MoltenVK" -name libMoltenVK.dylib | head -1)
    else
        command -v xcodebuild >/dev/null || { echo 'Building MoltenVK needs Xcode (or MOLTENVK_PREBUILT=1).' >&2; exit 1; }
        mvk=$src/MoltenVK-source
        if [ ! -d "$mvk/.git" ]; then
            git clone --depth 1 --branch "$moltenvk_version" https://github.com/KhronosGroup/MoltenVK.git "$mvk"
        fi
        git -C "$mvk" checkout -q -- . && git -C "$mvk" clean -qfd -- MoltenVK
        for patch in patches/moltenvk/*.patch; do
            [ -e "$patch" ] && git -C "$mvk" apply "$PWD/$patch"
        done
        echo "Building MoltenVK $moltenvk_version from source (first time: about half an hour) ..."
        [ -d "$mvk/External/build" ] || (cd "$mvk" && ./fetchDependencies --macos >/dev/null)
        (cd "$mvk" && make macos >/dev/null)
        dylib=$mvk/Package/Release/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib
    fi
    lipo -info "$dylib" | grep -q x86_64 || { echo "MoltenVK has no x86_64 slice: $dylib" >&2; exit 1; }
    cp "$dylib" "$deps/lib/libMoltenVK.dylib"
    mkdir -p "$deps/share/vulkan/icd.d"
    # Listed as an ordinary driver: the video core (shadPS4's) creates its instance without
    # VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR, which the loader requires to show a
    # portability driver. shadPS4's own macOS package does the same.
    cat > "$deps/share/vulkan/icd.d/MoltenVK_icd.json" <<'EOF'
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "../../../lib/libMoltenVK.dylib",
        "api_version": "1.4.0",
        "is_portability_driver": false
    }
}
EOF
    mark "$moltenvk_mark"
fi

# --- header-only ------------------------------------------------------------------------------
if ! done_mark "robin-map-$robin_version"; then
    github Tessil/robin-map "$robin_version" robin-map
    cmake_x86 "$src/robin-map" "$src/robin-map-build"
    mark "robin-map-$robin_version"
fi
if ! done_mark "vma-$vma_version"; then
    mkdir -p "$deps/include/vma"
    fetch "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/$vma_version/include/vk_mem_alloc.h" \
        "$deps/include/vma/vk_mem_alloc.h"
    mark "vma-$vma_version"
fi
brew_prefix=$(brew --prefix 2>/dev/null || echo /opt/homebrew)
for header in boost/version.hpp nlohmann/json.hpp; do
    [ -f "$brew_prefix/include/$header" ] || { echo "Need $header (brew install boost nlohmann-json)." >&2; exit 1; }
done
# Copied, not linked to Homebrew's prefix, so CMake never sees arm64 libraries beside them.
if ! done_mark headers-from-brew; then
    rm -rf "$deps/include/boost" "$deps/include/nlohmann"
    # -L: Homebrew's include/ entries are symlinks into its Cellar.
    cp -RL "$brew_prefix/include/boost" "$brew_prefix/include/nlohmann" "$deps/include/"
    nl_cmake=$(dirname "$(find "$brew_prefix/share/cmake" "$brew_prefix/lib/cmake" -name nlohmann_jsonConfig.cmake 2>/dev/null | head -1)")
    mkdir -p "$deps/lib/cmake/nlohmann_json"
    cat > "$deps/lib/cmake/nlohmann_json/nlohmann_jsonConfig.cmake" <<EOF
if(NOT TARGET nlohmann_json::nlohmann_json)
    add_library(nlohmann_json::nlohmann_json INTERFACE IMPORTED)
    set_target_properties(nlohmann_json::nlohmann_json PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "$deps/include")
endif()
set(nlohmann_json_FOUND TRUE)
EOF
    : "$nl_cmake"
    mark headers-from-brew
fi

# --- libraries --------------------------------------------------------------------------------
if ! done_mark "fmt-$fmt_version"; then
    github fmtlib/fmt "$fmt_version" fmt
    cmake_x86 "$src/fmt" "$src/fmt-build" -DFMT_TEST=OFF -DFMT_DOC=OFF -DBUILD_SHARED_LIBS=ON
    mark "fmt-$fmt_version"
fi
if ! done_mark "xxhash-$xxhash_version"; then
    github Cyan4973/xxHash "$xxhash_version" xxHash
    cmake_x86 "$src/xxHash/cmake_unofficial" "$src/xxHash-build" -DXXHASH_BUILD_XXHSUM=OFF -DBUILD_SHARED_LIBS=ON
    mark "xxhash-$xxhash_version"
fi
if ! done_mark "zydis-$zydis_version"; then
    rm -rf "$src/zydis"
    git clone -q --depth 1 --branch "$zydis_version" --recurse-submodules --shallow-submodules \
        https://github.com/zyantific/zydis.git "$src/zydis"
    # Zycore is installed on its own so that zydis-config.cmake finds it.
    cmake_x86 "$src/zydis/dependencies/zycore" "$src/zycore-build" -DZYCORE_BUILD_SHARED_LIB=ON
    cmake_x86 "$src/zydis" "$src/zydis-build" -DZYDIS_BUILD_TOOLS=OFF -DZYDIS_BUILD_EXAMPLES=OFF \
        -DZYDIS_BUILD_DOXYGEN=OFF -DZYDIS_BUILD_SHARED_LIB=ON
    mark "zydis-$zydis_version"
fi
if ! done_mark "protobuf-$protobuf_version"; then
    # 21.x needs no Abseil. Its protoc is built for x86_64 too and runs under Rosetta, so the
    # generated code always matches the library.
    github protocolbuffers/protobuf "$protobuf_version" protobuf
    cmake_x86 "$src/protobuf" "$src/protobuf-build" -Dprotobuf_BUILD_TESTS=OFF \
        -Dprotobuf_BUILD_SHARED_LIBS=ON -Dprotobuf_WITH_ZLIB=OFF
    mark "protobuf-$protobuf_version"
fi
if ! done_mark "openssl-$openssl_version"; then
    github openssl/openssl "$openssl_version" openssl
    (cd "$src/openssl" && ./Configure darwin64-x86_64-cc shared no-tests no-docs \
        --prefix="$deps" --libdir=lib -mmacosx-version-min="$min_macos" >/dev/null \
        && make -j"$jobs" >/dev/null && make install_sw >/dev/null)
    mark "openssl-$openssl_version"
fi
if ! done_mark "libpng-$libpng_version"; then
    github pnggroup/libpng "$libpng_version" libpng
    cmake_x86 "$src/libpng" "$src/libpng-build" -DPNG_TESTS=OFF -DPNG_TOOLS=OFF -DPNG_STATIC=OFF \
        -DPNG_FRAMEWORK=OFF
    mark "libpng-$libpng_version"
fi
if ! done_mark "miniupnpc-$miniupnpc_version"; then
    archive=$cache/miniupnpc-$miniupnpc_version.tar.gz
    fetch "https://github.com/miniupnp/miniupnp/releases/download/miniupnpc_${miniupnpc_version//./_}/miniupnpc-$miniupnpc_version.tar.gz" "$archive"
    unpack "$archive" "$src/miniupnpc"
    cmake_x86 "$src/miniupnpc" "$src/miniupnpc-build" -DUPNPC_BUILD_STATIC=OFF -DUPNPC_BUILD_SHARED=ON \
        -DUPNPC_BUILD_TESTS=OFF -DUPNPC_BUILD_SAMPLE=OFF
    mark "miniupnpc-$miniupnpc_version"
fi
if ! done_mark "epoll-shim-$epoll_shim_version"; then
    # The PS4 network library's epoll, over kqueue (shadPS4 uses it on macOS too).
    github jiixyj/epoll-shim "$epoll_shim_version" epoll-shim
    cmake_x86 "$src/epoll-shim" "$src/epoll-shim-build" -DBUILD_TESTING=OFF -DBUILD_SHARED_LIBS=ON
    mark "epoll-shim-$epoll_shim_version"
fi
if ! done_mark "sdl3-$sdl3_version"; then
    archive=$cache/SDL3-$sdl3_version.tar.gz
    fetch "https://github.com/libsdl-org/SDL/releases/download/release-$sdl3_version/SDL3-$sdl3_version.tar.gz" "$archive"
    unpack "$archive" "$src/SDL3"
    cmake_x86 "$src/SDL3" "$src/SDL3-build" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF \
        -DSDL_EXAMPLES=OFF -DSDL_INSTALL_TESTS=OFF
    mark "sdl3-$sdl3_version"
fi
if ! done_mark "ffmpeg-$ffmpeg_version"; then
    # The same minimal FFmpeg as the AppImage: the game's video player only.
    archive=$cache/ffmpeg-$ffmpeg_version.tar.xz
    fetch "https://ffmpeg.org/releases/ffmpeg-$ffmpeg_version.tar.xz" "$archive"
    unpack "$archive" "$src/ffmpeg"
    (cd "$src/ffmpeg" && ./configure \
        --enable-cross-compile --arch=x86_64 --target-os=darwin \
        --cc="clang -arch x86_64" --cxx="clang++ -arch x86_64" \
        --extra-cflags="-mmacosx-version-min=$min_macos" --extra-ldflags="-arch x86_64 -mmacosx-version-min=$min_macos" \
        --prefix="$deps" --enable-shared --disable-static \
        --disable-programs --disable-doc --disable-debug --disable-network --disable-autodetect \
        --disable-avdevice --disable-avfilter --disable-postproc --disable-everything \
        --enable-avcodec --enable-avformat --enable-avutil --enable-swscale --enable-swresample \
        --enable-demuxer=mov,matroska,mpegts,mp3,wav \
        --enable-decoder=h264,hevc,aac,mp3,pcm_s16le,pcm_s24le,pcm_f32le \
        --enable-parser=h264,hevc,aac,mpegaudio \
        --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc \
        --enable-protocol=file,pipe --enable-pthreads --enable-pic --enable-x86asm >/dev/null \
        && make -j"$jobs" >/dev/null && make install >/dev/null)
    mark "ffmpeg-$ffmpeg_version"
fi

cat > "$deps/deps.env" <<EOF
# Written by packaging/macos/fetch-deps.sh. Sourced by build-macos.sh.
export BB_DEPS_DIR="$deps"
export PKG_CONFIG_LIBDIR="$deps/lib/pkgconfig:$deps/share/pkgconfig"
unset PKG_CONFIG_PATH
export CMAKE_PREFIX_PATH="$deps"
export VK_DRIVER_FILES="$deps/share/vulkan/icd.d/MoltenVK_icd.json"
EOF
echo "Dependencies are in $deps"
