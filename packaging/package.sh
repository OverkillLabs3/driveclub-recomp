#!/usr/bin/env bash
# Builds the player release: release/driveclub-recomp-<version>-win64/ and a zip next to it.
# Needs the MSYS2 CLANG64 toolchain used for the build (no other downloads).
# usage: packaging/package.sh [version]
set -euo pipefail
cd "$(dirname "$0")/.."
export PATH="/c/msys64/clang64/bin:$PATH"
VERSION="${1:-dev}"
NAME="driveclub-recomp-$VERSION-win64"
OUT="release/$NAME"

bash build.sh
mkdir -p out/launcher
windres -I launcher launcher/launcher.rc -O coff -o out/launcher/launcher.res.o
clang++ -O2 -std=c++20 -municode -mwindows -static -o out/launcher/DriveClub.exe   launcher/driveclub_launcher.cpp launcher/prepare.cpp launcher/gpu_check.cpp launcher/datx.cpp launcher/unlock.cpp launcher/graphics.cpp launcher/carswitch.cpp out/launcher/launcher.res.o   -lgdiplus -lgdi32 -luser32 -lshell32 -lole32 -ldwmapi
# The steering wheel tools, SDL linked in: WheelSetup is a window; FfbTest a console program (-mconsole after the SDL
# flags, which bring -mwindows).
sdl_static="$(pkg-config --static --libs sdl3)"
clang -O2 -o out/WheelSetup.exe tools/wheel_setup.c -static $sdl_static -lgdi32 -mwindows
clang -O2 -o out/FfbTest.exe tools/ffb_test.c -static $sdl_static -mconsole

rm -rf "$OUT"
mkdir -p "$OUT/runtime/patches"
cp out/launcher/DriveClub.exe out/WheelSetup.exe out/FfbTest.exe "$OUT/"
cp "tools/FFB Tuner.ps1" "tools/FFB Tuner.bat" "$OUT/"   # the force feedback sliders, next to DriveClub.exe
cp out/DriveClubGame.exe "$OUT/runtime/"
strip --strip-all "$OUT/runtime/DriveClubGame.exe" || true
cp patches/DriveClub.xml "$OUT/runtime/patches/"
cp packaging/release/README.txt packaging/release/THIRD_PARTY_NOTICES.txt LICENSE "$OUT/"
echo "$VERSION" > "$OUT/version.txt"

# Every non-system DLL the game program loads, so nothing has to be installed.
ldd out/DriveClubGame.exe | awk '{print $3}' | grep -i '/msys64/' | sort -u | while read -r dll; do
  cp "$dll" "$OUT/runtime/"
done
# And the DLLs those DLLs load (two more rounds reach everything MinGW libraries pull in).
for round in 1 2; do
  for staged in "$OUT"/runtime/*.dll; do
    ldd "$staged" 2>/dev/null | awk '{print $3}' | { grep -i '/msys64/' || true; } | sort -u | while read -r dll; do
      [ -f "$OUT/runtime/$(basename "$dll")" ] || cp "$dll" "$OUT/runtime/"
    done
  done
done
# DLSS frame generation: the bridge built from gpu/dlss_bridge and NVIDIA's DLSS libraries.
cp packaging/dlss_bin/bbport_dlss.dll packaging/dlss_bin/nvngx_dlss.dll packaging/dlss_bin/nvngx_dlssg.dll "$OUT/runtime/"
mkdir -p "$OUT/licenses"
cp packaging/dlss/NVIDIA-DLSS-LICENSE.txt packaging/dlss/dlss_bridge-LICENSE.txt "$OUT/licenses/"
# DLLs the game program loads by name at run time.
for extra in SDL3.dll; do
  [ -f "out/$extra" ] && cp "out/$extra" "$OUT/runtime/" || true
done
# The program files of the release, so the launcher can tell when some are missing.
(cd "$OUT/runtime" && find . -type f ! -name files.txt | sed 's|^\./||' | sort > files.txt)
# Nothing of the game, the console firmware or the player may end up in the package.
leaks="$(find "$OUT" -type f \( -iname 'eboot.bin' -o -iname '*.sprx' -o -iname '*.prx' -o -iname '*.pkg' -o -iname '*.sfo' \
  -o -iname '*.psarc' -o -iname 'game*.dat' -o -iname 'game.ndx' -o -iname '*.log' -o -iname '*.spv' -o -iname '*.dmp' \
  -o -iname 'launcher.ini' -o -path '*/data/*' -o -path '*/logs/*' -o -path '*/sce_sys/*' \) -print)"
if [ -n "$leaks" ]; then
  echo "Package leak check failed, these files must not ship:" >&2
  echo "$leaks" >&2
  exit 1
fi
echo "files: $(find "$OUT" -type f | wc -l), size: $(du -sh "$OUT" | cut -f1)"
python - "$OUT" "release/$NAME.zip" <<'PYEOF'
import os, sys, zipfile
src, dest = sys.argv[1], sys.argv[2]
top = os.path.basename(src.rstrip("/"))
if os.path.exists(dest):
    os.remove(dest)
with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for base, _, files in os.walk(src):
        for name in sorted(files):
            path = os.path.join(base, name)
            z.write(path, top + "/" + os.path.relpath(path, src).replace(os.sep, "/"))
PYEOF
echo "wrote release/$NAME.zip"
