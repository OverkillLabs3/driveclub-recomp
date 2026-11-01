#!/usr/bin/env bash
# Builds the Linux AppImage: release/driveclub-recomp-<version>-x86_64.AppImage.
#
#   bash packaging/appimage/build-appimage.sh [version]
#
# It builds the native runtime first (build-linux.sh) unless SKIP_BUILD=1, stages an AppDir
# under out/appimage, bundles the non-system shared libraries and packs a type-2 AppImage with
# mksquashfs. glibc, the Vulkan loader/ICD/driver and the display/audio stack stay the host's.
#
# Environment:
#   APPIMAGE_VERSION   version when no argument is given (default: dev)
#   SKIP_BUILD=1       reuse out/driveclub-probe and out/gpu/libbbgpu.so
#   APPIMAGE_COMP      squashfs compressor (default zstd)
#   APPIMAGE_ZSTD_LEVEL, APPIMAGE_BLOCK
#   APPIMAGE_RUNTIME   a local type-2 runtime instead of downloading it
#   MKSQUASHFS         path to mksquashfs
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
cd -- "$here"
version=${1:-${APPIMAGE_VERSION:-dev}}
deps=${BB_DEPS_DIR:-$PWD/out/deps}
if [ -f "$deps/deps.env" ]; then
    # shellcheck disable=SC1090
    . "$deps/deps.env"
fi
arch=$(uname -m)
[ "$arch" = x86_64 ] || { echo "The AppImage is built for x86_64 (this is $arch)." >&2; exit 1; }

if [ "${SKIP_BUILD:-0}" != 1 ]; then
    bash build-linux.sh
fi
probe=$here/out/driveclub-probe
prepare=$here/out/driveclub-prepare
launcher=$here/out/driveclub-launcher
gpu=$here/out/gpu/libbbgpu.so
[ -f "$probe" ] || { echo 'Build first: bash build-linux.sh' >&2; exit 1; }
[ -f "$prepare" ] || { echo 'Missing out/driveclub-prepare (bash build-linux.sh).' >&2; exit 1; }
[ -f "$launcher" ] || { echo 'Missing out/driveclub-launcher (bash build-linux.sh).' >&2; exit 1; }
[ -f "$gpu" ] || { echo 'Missing out/gpu/libbbgpu.so (bash build-linux.sh).' >&2; exit 1; }

build=$here/out/appimage
appdir=$build/DriveClub.AppDir
port=$appdir/usr/share/driveclub-recomp
docs=$appdir/usr/share/doc/driveclub-recomp
mkdir -p "$build" "$here/release"
rm -rf -- "$appdir"
mkdir -p "$appdir/usr/bin" "$appdir/usr/lib/driveclub-recomp/gpu" \
    "$port/patches" "$port/fonts" "$docs" "$appdir/usr/share/applications" \
    "$appdir/usr/share/icons/hicolor/256x256/apps"

install -m755 "$probe" "$appdir/usr/bin/driveclub-probe"
install -m755 "$prepare" "$appdir/usr/bin/driveclub-prepare"
install -m755 "$launcher" "$appdir/usr/bin/driveclub-launcher"
install -m755 "$gpu" "$appdir/usr/lib/driveclub-recomp/gpu/libbbgpu.so"
install -m644 launcher/assets/fonts/001futura-webfont.ttf "$port/fonts/001futura-webfont.ttf"
# RelWithDebInfo builds carry debug info; the players do not need it. Only debug sections are removed:
# the runtime_*/guest_cpu_* exports libbbgpu resolves stay in the dynamic symbol table.
if command -v strip >/dev/null; then
    strip --strip-debug "$appdir/usr/bin/driveclub-probe" "$appdir/usr/bin/driveclub-prepare" \
        "$appdir/usr/bin/driveclub-launcher" "$appdir/usr/lib/driveclub-recomp/gpu/libbbgpu.so"
fi
printf '%s\n' "$version" > "$port/version.txt"
install -m644 packaging/release/trophies.tsv "$port/trophies.tsv"
install -m644 patches/DriveClub.xml "$port/patches/DriveClub.xml"
install -m644 packaging/release/THIRD_PARTY_NOTICES.txt "$docs/THIRD_PARTY_NOTICES.txt"
install -m644 LICENSE "$docs/LICENSE"
install -m644 README.md "$docs/README.md"
install -m644 BUILDING.md "$docs/BUILDING.md"
install -m644 packaging/appimage/README.txt "$docs/README-AppImage.txt"

install -Dm755 packaging/appimage/AppRun "$appdir/AppRun"
install -Dm644 packaging/appimage/driveclub-recomp.desktop "$appdir/driveclub-recomp.desktop"
install -Dm644 packaging/appimage/driveclub-recomp.desktop \
    "$appdir/usr/share/applications/driveclub-recomp.desktop"
install -Dm644 packaging/appimage/driveclub-recomp.png \
    "$appdir/usr/share/icons/hicolor/256x256/apps/driveclub-recomp.png"
install -Dm644 packaging/appimage/driveclub-recomp.png "$appdir/driveclub-recomp.png"
install -Dm644 packaging/appimage/driveclub-recomp.png "$appdir/.DirIcon"
ln -s AppRun "$appdir/driveclub-recomp"

# The bundled libraries. The staged GPU library comes first, so ldd resolves libbbgpu from the
# AppDir without copying a second copy into usr/lib.
echo 'Bundling shared libraries ...'
LD_LIBRARY_PATH="$appdir/usr/lib/driveclub-recomp/gpu:${LD_LIBRARY_PATH:-}" \
    bash packaging/appimage/bundle-libs.sh "$appdir"

mksquashfs=${MKSQUASHFS:-$(command -v mksquashfs || true)}
[ -n "$mksquashfs" ] || { echo 'Need mksquashfs (install squashfs-tools or set MKSQUASHFS).' >&2; exit 1; }
if [ -n "${APPIMAGE_RUNTIME:-}" ]; then
    runtime=$APPIMAGE_RUNTIME
    [ -x "$runtime" ] || { echo "APPIMAGE_RUNTIME is not executable: $runtime" >&2; exit 1; }
else
    runtime=$build/runtime-x86_64
fi
if [ ! -x "$runtime" ]; then
    echo "Fetching the AppImage type-2 runtime ..."
    if command -v curl >/dev/null; then
        curl -fL --retry 3 -o "$runtime" \
            https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64
    elif command -v wget >/dev/null; then
        wget -O "$runtime" https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-x86_64
    else
        echo 'Need curl or wget for the AppImage runtime, or set APPIMAGE_RUNTIME.' >&2
        exit 1
    fi
    chmod +x "$runtime"
fi

comp=${APPIMAGE_COMP:-zstd}
zstd_level=${APPIMAGE_ZSTD_LEVEL:-15}
block=${APPIMAGE_BLOCK:-128K}
comp_args=()
[ "$comp" = zstd ] && comp_args=(-Xcompression-level "$zstd_level")
squashfs=$build/driveclub.squashfs
rm -f -- "$squashfs"
echo "Packing the squashfs ($comp, block $block) ..."
"$mksquashfs" "$appdir" "$squashfs" -comp "$comp" "${comp_args[@]}" -b "$block" \
    -noappend -all-root -quiet

out=$here/release/driveclub-recomp-$version-x86_64.AppImage
cat "$runtime" "$squashfs" > "$out"
chmod +x "$out"
sha256sum "$out" > "$out.sha256"
echo "AppImage: $out ($(du -h "$out" | cut -f1))"
echo "SHA-256: $(cut -d' ' -f1 "$out.sha256")"
