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

# The player release is built with link-time optimization and the recorded profile (about 10% more FPS).
if [ -f pgo/default.profdata ]; then export BB_PGO=use BB_LTO=ON; fi
bash build.sh
mkdir -p out/launcher
windres -I launcher launcher/launcher.rc -O coff -o out/launcher/launcher.res.o
clang++ -O2 -std=c++20 -municode -mwindows -static -Igpu/third_party/miniz -Iout/gpu/third_party/miniz -DMINIZ_STATIC_DEFINE -o out/launcher/DriveClub.exe   launcher/driveclub_launcher.cpp launcher/prepare.cpp launcher/gpu_check.cpp launcher/datx.cpp launcher/unlock.cpp launcher/graphics.cpp launcher/carswitch.cpp launcher/updater.cpp out/launcher/launcher.res.o out/gpu/third_party/miniz/libminiz.a   -lgdiplus -lgdi32 -luser32 -lshell32 -lole32 -ldwmapi -lwinhttp -lbcrypt -lcomctl32 -loleaut32 -lwbemuuid -liphlpapi
# The steering wheel tools, SDL linked in: WheelSetup is a window; FfbTest a console program (-mconsole after the SDL
# flags, which bring -mwindows).
sdl_static="$(pkg-config --static --libs sdl3)"
clang -O2 -o out/WheelSetup.exe tools/wheel_setup.c -static $sdl_static -lgdi32 -mwindows
clang -O2 -o out/FfbTest.exe tools/ffb_test.c -static $sdl_static -mconsole

rm -rf "$OUT"
mkdir -p "$OUT/runtime/patches"
cp out/launcher/DriveClub.exe out/WheelSetup.exe out/FfbTest.exe "$OUT/"
# The WPF launcher: its bootstrapper replaces DriveClub.exe; the Win32 launcher stays as DriveClub.Native.exe for
# Wine and for PCs without the .NET 8 desktop runtime. Needs the .NET 8 SDK (dotnet) on PATH.
WPF_OUT="${WPF_DIR:-out/wpf}"   # a folder built earlier with launcher/build-wpf.ps1, or built here
if [ -z "${WPF_DIR:-}" ]; then
  rm -rf out/wpf
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$(cygpath -w launcher/build-wpf.ps1)"   -OutputDirectory "$(cygpath -w "$PWD/out/wpf")" -Toolchain "$(cygpath -w "$(dirname "$(command -v clang++)")")"
fi
[ -f "$WPF_OUT/DriveClub.Launcher.exe" ] && [ -f "$WPF_OUT/DriveClub.Native.exe" ] || { echo "The WPF launcher was not built." >&2; exit 1; }
for f in "$WPF_OUT"/*; do
  case "$(basename "$f")" in native-build|*.pdb) ;; *) cp -r "$f" "$OUT/" ;; esac
done
cp "tools/FFB Tuner.ps1" "tools/FFB Tuner.bat" "$OUT/"   # the force feedback sliders, next to DriveClub.exe
cp out/DriveClubGame.exe "$OUT/runtime/"
cp -r gpu/third_party/fsr412/assets "$OUT/runtime/fsr4_412"
cp gpu/third_party/fsr412/bin/*.dll "$OUT/runtime/"
strip --strip-all "$OUT/runtime/DriveClubGame.exe" || true
cp patches/DriveClub.xml "$OUT/runtime/patches/"
cp packaging/release/trophies.tsv "$OUT/runtime/"   # trophy banner texts: names, descriptions, grades
cp packaging/release/README.txt packaging/release/THIRD_PARTY_NOTICES.txt LICENSE "$OUT/"
echo "$VERSION" > "$OUT/version.txt"

# Online play: the community server (a built folder: driveclubsrv.exe, its DLLs and sqldrivers/), its settings, and
# the game settings file the launcher copies into place when Online play is on.
SERVER_BIN="${DC_SERVER_DIR:-packaging/server_bin}"
if [ ! -f "$SERVER_BIN/driveclubsrv.exe" ]; then
  echo "The server build is missing: $SERVER_BIN/driveclubsrv.exe (set DC_SERVER_DIR)." >&2
  exit 1
fi
mkdir -p "$OUT/server" "$OUT/online"
cp -r "$SERVER_BIN"/. "$OUT/server/"
cp docs/online/server-config.json "$OUT/server/config.json"
cp docs/online/override-0.tss "$OUT/online/"
cp docs/online/play/Allow-Firewall.bat docs/online/play/Allow-Firewall.ps1 "$OUT/"   # next to DriveClub.exe: it finds runtime\ and server\ from there
cp docs/online/README.md docs/online/QUICKSTART.md "$OUT/online/"

# Every non-system DLL the game program loads, so nothing has to be installed.
ldd out/DriveClubGame.exe | awk '{print $3}' | grep -i -E '/msys64/|^/(clang64|mingw64|ucrt64)/' | sort -u | while read -r dll; do
  cp "$dll" "$OUT/runtime/"
done
# And the DLLs those DLLs load (two more rounds reach everything MinGW libraries pull in).
for round in 1 2; do
  for staged in "$OUT"/runtime/*.dll; do
    ldd "$staged" 2>/dev/null | awk '{print $3}' | { grep -i -E '/msys64/|^/(clang64|mingw64|ucrt64)/' || true; } | sort -u | while read -r dll; do
      [ -f "$OUT/runtime/$(basename "$dll")" ] || cp "$dll" "$OUT/runtime/"
    done
  done
done
# A package without its libraries starts nothing: stop when the collection found none.
if [ ! -f "$OUT/runtime/SDL3.dll" ] || [ "$(find "$OUT/runtime" -name '*.dll' | wc -l)" -lt 40 ]; then
  echo "The program libraries were not collected (ldd found no MSYS2 paths); the package would not start." >&2
  exit 1
fi
# DLSS frame generation: the bridge built from gpu/dlss_bridge and NVIDIA's DLSS libraries.
DLSS_BIN="${DLSS_BIN:-packaging/dlss_bin}"
cp "$DLSS_BIN/bbport_dlss.dll" "$DLSS_BIN/nvngx_dlss.dll" "$DLSS_BIN/nvngx_dlssg.dll" \
  "$DLSS_BIN/bbport_nr_nvngx.dll" "$OUT/runtime/"
if [ -f "$DLSS_BIN/nvngx_dlssnr.dll" ]; then
  cp "$DLSS_BIN/nvngx_dlssnr.dll" "$OUT/runtime/"
fi
mkdir -p "$OUT/licenses"
cp gpu/third_party/fsr412/LICENSE.txt "$OUT/licenses/AMD-FSR-SDK-LICENSE.txt"
cp gpu/third_party/fsr412/MSVC-LICENSE.txt "$OUT/licenses/Microsoft-Visual-C-Runtime-LICENSE.txt"
cp gpu/third_party/fsr412/NOTICE.txt "$OUT/licenses/FSR-4.1.2-NOTICE.txt"
cp packaging/dlss/NVIDIA-DLSS-LICENSE.txt packaging/dlss/dlss_bridge-LICENSE.txt "$OUT/licenses/"
# The community server is GPL-2.0 software: its licence and where its source is.
[ -f "$SERVER_BIN/LICENSE" ] && cp "$SERVER_BIN/LICENSE" "$OUT/licenses/driveclub-server-LICENSE.txt"
printf 'The program in the server folder is the DriveClub community server, GPL-2.0.
Source code: https://github.com/shad-net/driveclub-server and https://github.com/Judaswick/driveclub-server (branch coop).
' > "$OUT/licenses/driveclub-server-SOURCE.txt"
# DLLs the game program loads by name at run time.
for extra in SDL3.dll; do
  [ -f "out/$extra" ] && cp "out/$extra" "$OUT/runtime/" || true
done
# The program files of the release, so the launcher can tell when some are missing.
(cd "$OUT/runtime" && find . -type f ! -name files.txt | sed 's|^\./||' | sort > files.txt)
# Nothing of the game, the console firmware or the player may end up in the package.
leaks="$(find "$OUT" -type f \( -iname 'eboot.bin' -o -iname '*.sprx' -o -iname '*.prx' -o -iname '*.pkg' -o -iname '*.sfo' \
  -o -iname '*.psarc' -o -iname 'game*.dat' -o -iname 'game.ndx' -o -iname '*.log' -o \( -iname '*.spv' ! -path "$OUT/runtime/fsr4_412/*" \) -o -iname '*.dmp' \
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
