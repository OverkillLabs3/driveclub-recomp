#!/usr/bin/env bash
# Fetches and builds the Linux build dependencies the AppImage needs but Ubuntu 24.04 does not
# package: a recent SDL3 with audio backends, a minimal FFmpeg, newer Vulkan headers, robin-map
# and the Vulkan Memory Allocator header. Everything is installed under out/deps (gitignored);
# nothing outside the checkout is touched.
#
#   bash packaging/appimage/fetch-deps.sh
#
# build-linux.sh and build-appimage.sh source out/deps/deps.env, so the same scripts also work
# without this step when the distribution provides the libraries.
#
# Environment:
#   BB_DEPS_DIR       install prefix (default: out/deps)
#   SDL3_VERSION      default 3.4.18
#   FFMPEG_VERSION    default 6.1.2
#   VULKAN_HEADERS_VERSION, ROBIN_MAP_VERSION, VMA_VERSION
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
deps=${BB_DEPS_DIR:-$PWD/out/deps}
sdl3_version=${SDL3_VERSION:-3.4.18}
ffmpeg_version=${FFMPEG_VERSION:-6.1.2}
vulkan_version=${VULKAN_HEADERS_VERSION:-vulkan-sdk-1.4.363.0}
robin_version=${ROBIN_MAP_VERSION:-v1.4.1}
vma_version=${VMA_VERSION:-v3.5.0}
jobs=$(nproc 2>/dev/null || echo 4)
mkdir -p "$deps/src" "$deps/cache"

fetch() { # fetch URL DEST
    local url=$1 dest=$2
    if [ -s "$dest" ]; then return 0; fi
    echo "Fetching $(basename -- "$dest") ..."
    if command -v curl >/dev/null; then
        curl -fL --retry 3 --retry-delay 2 -o "$dest.part" "$url"
    elif command -v wget >/dev/null; then
        wget -O "$dest.part" "$url"
    else
        echo 'Need curl or wget.' >&2
        exit 1
    fi
    mv -- "$dest.part" "$dest"
}

# --- Vulkan headers ---------------------------------------------------------------------------
# Ubuntu 24.04's headers predate VK_KHR_maintenance8 / VK_EXT_robustness2, which the video core
# uses. The headers only; the loader stays the host's.
if [ ! -f "$deps/include/vulkan/vulkan.h" ]; then
    archive=$deps/cache/Vulkan-Headers.tar.gz
    fetch "https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/$vulkan_version.tar.gz" "$archive"
    rm -rf "$deps/src/Vulkan-Headers"
    mkdir -p "$deps/src/Vulkan-Headers"
    tar -xzf "$archive" --no-same-owner -C "$deps/src/Vulkan-Headers" --strip-components=1
    rm -rf "$deps/include/vulkan" "$deps/include/vk_video"
    cp -r "$deps/src/Vulkan-Headers/include/." "$deps/include/"
    echo "Vulkan headers $vulkan_version installed"
fi

# --- robin-map (header-only) ------------------------------------------------------------------
if [ ! -f "$deps/include/tsl/robin_map.h" ]; then
    archive=$deps/cache/robin-map.tar.gz
    fetch "https://github.com/Tessil/robin-map/archive/refs/tags/$robin_version.tar.gz" "$archive"
    rm -rf "$deps/src/robin-map"
    mkdir -p "$deps/src/robin-map"
    tar -xzf "$archive" --no-same-owner -C "$deps/src/robin-map" --strip-components=1
    mkdir -p "$deps/include/tsl"
    cp "$deps/src/robin-map/include/tsl/"*.h "$deps/include/tsl/"
    echo "robin-map $robin_version installed"
fi
robin_config=$deps/lib/cmake/tsl-robin-map/tsl-robin-mapConfig.cmake
if [ ! -f "$robin_config" ]; then
    mkdir -p "$(dirname -- "$robin_config")"
    cat > "$robin_config" <<EOF
# Written by packaging/appimage/fetch-deps.sh: robin-map is header-only.
if(NOT TARGET tsl::robin_map)
    add_library(tsl::robin_map INTERFACE IMPORTED)
    set_target_properties(tsl::robin_map PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "$deps/include")
endif()
EOF
fi

# --- Vulkan Memory Allocator (single header) --------------------------------------------------
if [ ! -f "$deps/include/vma/vk_mem_alloc.h" ]; then
    mkdir -p "$deps/include/vma"
    fetch "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/$vma_version/include/vk_mem_alloc.h" \
        "$deps/include/vma/vk_mem_alloc.h"
    echo "Vulkan Memory Allocator $vma_version installed"
fi

# --- SDL3 with the audio backends the game needs ----------------------------------------------
# A static or audio-less SDL3 leaves the game silent; the AppImage bundles this build.
if [ ! -f "$deps/sdl3/lib/libSDL3.so.0" ]; then
    for header in alsa/asoundlib.h pulse/pulseaudio.h pipewire-0.3/pipewire/pipewire.h; do
        [ -f "/usr/include/$header" ] || echo "Note: /usr/include/$header is missing; SDL3 will be built without that audio backend." >&2
    done
    src=$deps/src/SDL3-$sdl3_version
    if [ ! -f "$src/CMakeLists.txt" ]; then
        archive=$deps/cache/SDL3-$sdl3_version.tar.gz
        fetch "https://github.com/libsdl-org/SDL/releases/download/release-$sdl3_version/SDL3-$sdl3_version.tar.gz" "$archive"
        rm -rf "$src"
        tar -xzf "$archive" --no-same-owner -C "$deps/src"
    fi
    cmake -S "$src" -B "$deps/src/sdl3-build" -G Ninja \
        -DCMAKE_INSTALL_PREFIX="$deps/sdl3" -DCMAKE_BUILD_TYPE=Release \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF \
        -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_INSTALL_TESTS=OFF \
        -DSDL_ALSA=ON -DSDL_PULSEAUDIO=ON -DSDL_PIPEWIRE=ON -DSDL_JACK=OFF -DSDL_SNDIO=OFF \
        -DSDL_WAYLAND=OFF -DSDL_WAYLAND_LIBDECOR=OFF >/dev/null
    cmake --build "$deps/src/sdl3-build" --parallel "$jobs"
    cmake --install "$deps/src/sdl3-build"
    echo "SDL3 $sdl3_version installed"
fi

# --- minimal FFmpeg ---------------------------------------------------------------------------
# Ubuntu's libavformat links most of the desktop (GTK, X11, GnuTLS, libxml2...). Bundling that
# closure into an AppImage conflicts with the host's glib/xcb; this build keeps the demuxers and
# decoders the game's video player uses.
if [ ! -f "$deps/ffmpeg/lib/libavformat.so.60" ]; then
    src=$deps/src/ffmpeg-$ffmpeg_version
    if [ ! -f "$src/configure" ]; then
        archive=$deps/cache/ffmpeg-$ffmpeg_version.tar.xz
        fetch "https://ffmpeg.org/releases/ffmpeg-$ffmpeg_version.tar.xz" "$archive"
        rm -rf "$src"
        tar -xJf "$archive" --no-same-owner -C "$deps/src"
    fi
    x86asm=--enable-x86asm
    command -v nasm >/dev/null || x86asm=--disable-x86asm
    (cd "$src" && ./configure \
        --cc="${CC:-cc}" --cxx="${CXX:-c++}" \
        --prefix="$deps/ffmpeg" --libdir="$deps/ffmpeg/lib" --incdir="$deps/ffmpeg/include" \
        --enable-shared --disable-static \
        --disable-programs --disable-doc --disable-debug \
        --disable-network --disable-autodetect \
        --disable-avdevice --disable-avfilter --disable-postproc \
        --disable-everything \
        --enable-avcodec --enable-avformat --enable-avutil --enable-swscale --enable-swresample \
        --enable-demuxer=mov,matroska,mpegts,mp3,wav \
        --enable-decoder=h264,hevc,aac,mp3,pcm_s16le,pcm_s24le,pcm_f32le \
        --enable-parser=h264,hevc,aac,mpegaudio \
        --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb,aac_adtstoasc \
        --enable-protocol=file,pipe \
        --enable-pthreads --enable-pic "$x86asm" >/dev/null)
    make -C "$src" -j"$jobs" >/dev/null
    make -C "$src" install >/dev/null
    echo "FFmpeg $ffmpeg_version installed (minimal)"
fi

cat > "$deps/deps.env" <<EOF
# Written by packaging/appimage/fetch-deps.sh. Sourced by build-linux.sh and build-appimage.sh.
export PKG_CONFIG_PATH="$deps/sdl3/lib/pkgconfig:$deps/ffmpeg/lib/pkgconfig\${PKG_CONFIG_PATH:+:\$PKG_CONFIG_PATH}"
export CMAKE_PREFIX_PATH="$deps:$deps/sdl3:$deps/ffmpeg\${CMAKE_PREFIX_PATH:+:\$CMAKE_PREFIX_PATH}"
export LD_LIBRARY_PATH="$deps/sdl3/lib:$deps/ffmpeg/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}"
export BB_VULKAN_HEADERS="$deps/include"
EOF
echo "Dependencies ready in $deps (source $deps/deps.env)"
