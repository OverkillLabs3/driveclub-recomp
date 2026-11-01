#!/usr/bin/env bash
# Copies the shared-library closure of the AppImage's programs into usr/lib.
#
# The package must not depend on the build machine's SDL3, FFmpeg, protobuf or fmt, but it must
# use the host's glibc, Vulkan ICD/driver and OpenGL/DRM stack: those are tied to the kernel and
# the installed GPU driver, and bundling them breaks more systems than it fixes.
#
#   bash packaging/appimage/bundle-libs.sh APPDIR
#
# Run it with the dependency prefix on LD_LIBRARY_PATH (build-appimage.sh does), so ldd resolves
# the SDL3/FFmpeg the build used rather than the build host's. Extra exclusions:
# APPIMAGE_LIB_SKIP="pattern ..." (shell patterns).
set -euo pipefail
appdir=${1:?usage: bundle-libs.sh APPDIR}
libdir=$appdir/usr/lib
probe=$appdir/usr/bin/driveclub-probe
prepare=$appdir/usr/bin/driveclub-prepare
launcher=$appdir/usr/bin/driveclub-launcher
gpu=$appdir/usr/lib/driveclub-recomp/gpu/libbbgpu.so
docs=$appdir/usr/share/doc/driveclub-recomp
mkdir -p "$libdir" "$docs"
declare -A seen=()
missing=()
copied=0

is_elf() {
    [ -f "$1" ] && [ "$(head -c 4 -- "$1" | od -An -tx1 | tr -d ' \n')" = 7f454c46 ]
}

is_excluded() {
    local name=$1 pattern
    case "$name" in
        # glibc and the dynamic loader are the host's, always.
        linux-vdso.so.*|ld-linux*.so.*|libc.so.*|libm.so.*|libmvec.so.*|libdl.so.*|libpthread.so.*|\
        librt.so.*|libresolv.so.*|libutil.so.*|libanl.so.*|libnss_*.so.*|libBrokenLocale.so.*)
            return 0 ;;
        # Kernel/GPU/driver stack: the host's driver must match its kernel and display server.
        libGL.so.*|libEGL.so.*|libGLX.so.*|libOpenGL.so.*|libGLdispatch.so.*|\
        libdrm.so.*|libgbm.so.*|libLLVM*.so*|libvulkan.so.*|libvulkan_*.so*|\
        libnvidia-*.so*|libcuda.so*|libcudart.so*|libamdgpu_*.so*|libvdpau*.so*|libva*.so*)
            return 0 ;;
        # System services and audio: SDL loads the host's backends at run time.
        libudev.so.*|libsystemd.so.*|libdbus-1.so.*|libasound.so.*|libpulse.so.*|\
        libpipewire-*.so.*|libjack.so.*)
            return 0 ;;
        # Windowing/font stack: the Mesa/NVIDIA Vulkan driver is loaded by the host loader and
        # links the host's xcb. Older bundled X11/Wayland/font libraries break the driver and
        # the desktop.
        libX11*.so*|libxcb*.so*|libXau*.so*|libXdmcp*.so*|libXext*.so*|libXrender*.so*|\
        libXft*.so*|libXfixes*.so*|libXrandr*.so*|libXcursor*.so*|libXi*.so*|libXss*.so*|\
        libXtst*.so*|libxkbcommon*.so*|libxshmfence*.so*|libwayland-*.so*|libdecor-*.so*|\
        libfontconfig*.so*|libfreetype*.so*|libharfbuzz*.so*|libexpat*.so*|\
        libglib-2.0.so.*|libgobject-2.0.so.*|libgio-2.0.so.*|libgmodule-2.0.so.*|\
        libgthread-2.0.so.*|libusb-1.0.so.*)
            return 0 ;;
        # Base OS/desktop libraries. Host libraries (GIO, the Mesa driver) load these too; an
        # older bundled copy breaks them with missing symbol versions.
        libmount.so.*|libblkid.so.*|libuuid.so.*|libselinux.so.*|libcap.so.*|libattr.so.*|\
        libacl.so.*|libpcre2-*.so.*|libgcrypt.so.*|libgpg-error.so.*|liblz4.so.*|libzstd.so.*|\
        liblzma.so.*|libbz2.so.*|libxml2.so.*|libicu*.so.*|libsqlite3.so.*|libffi.so.*|\
        libnettle.so.*|libhogweed.so.*|libgnutls.so.*|libtasn1.so.*|libp11-kit.so.*|\
        libidn2.so.*|libunistring.so.*|libgmp.so.*|libpsl.so.*|libnghttp2.so.*|libssh*.so.*|\
        libldap*.so.*|libsasl2.so.*|libkrb5*.so.*|libgssapi*.so.*|libk5crypto*.so.*|\
        libcom_err.so.*|libkeyutils.so.*|libbrotli*.so.*|libsharpyuv.so.*|libwebp*.so.*|\
        libtiff*.so.*|libjpeg*.so.*|libopenjp2*.so.*|libdeflate*.so.*|\
        libjbig*.so.*|libLerc*.so.*|libnuma.so.*|libcrypt.so.*|libunwind*.so.*)
            return 0 ;;
    esac
    for pattern in ${APPIMAGE_LIB_SKIP:-}; do
        # shellcheck disable=SC2254
        case "$name" in $pattern) return 0 ;; esac
    done
    return 1
}

in_package() {
    case "$1" in
        "$appdir"/*) return 0 ;;
        *) return 1 ;;
    esac
}

collect() {
    local elf=$1 line name path real base link
    is_elf "$elf" || return 0
    while IFS= read -r line; do
        # ldd indents its output; trim it so the SONAME matching below sees the bare name.
        line=${line#"${line%%[![:space:]]*}"}
        name=${line%% *}
        case "$line" in
            *"=> not found"*)
                missing+=("$name ($elf)")
                continue ;;
        esac
        path=$(sed -n 's/.*=> \(\/[^ ]*\).*/\1/p' <<<"$line")
        [ -n "$path" ] || continue
        real=$(readlink -f -- "$path" 2>/dev/null || true)
        [ -n "$real" ] && [ -f "$real" ] || continue
        if in_package "$real"; then
            base=$(basename -- "$real")
            [[ -n ${seen[$base]:-} ]] || { seen[$base]=2; collect "$real"; }
            continue
        fi
        base=$(basename -- "$real")
        if is_excluded "$name" || is_excluded "$base"; then
            continue
        fi
        if [[ -z ${seen[$base]:-} ]]; then
            seen[$base]=1
            cp -L -- "$real" "$libdir/$base"
            copied=$((copied + 1))
        fi
        # Both the name the ELF asks for (SONAME) and the name ldd resolved point at the file.
        for link in "$name" "$(basename -- "$path")"; do
            if [[ "$link" != "$base" && ! -e "$libdir/$link" ]]; then
                ln -s -- "$base" "$libdir/$link"
            fi
        done
        # A copied library has its own dependencies (possibly outside the system set).
        if [[ ${seen[$base]:-0} == 1 ]]; then
            seen[$base]=2
            collect "$real"
        fi
    done < <(ldd "$elf" 2>/dev/null || true)
}

collect "$probe"
collect "$prepare"
collect "$launcher"
collect "$gpu"

if ((${#missing[@]})); then
    printf 'bundle-libs: missing at build time:\n' >&2
    printf '  %s\n' "${missing[@]}" >&2
    exit 1
fi

{
    echo "Shared libraries bundled in usr/lib by packaging/appimage/bundle-libs.sh."
    echo "glibc, the dynamic loader, the Vulkan loader/ICDs, OpenGL/EGL/DRM and the audio"
    echo "backends are provided by the host on purpose."
    (cd "$libdir" && ls -1 | sort)
} > "$docs/BUNDLED-LIBS.txt"
echo "bundle-libs: $copied libraries, $(du -sh "$libdir" | cut -f1) in usr/lib"
