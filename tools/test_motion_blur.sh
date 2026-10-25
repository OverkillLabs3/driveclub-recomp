#!/usr/bin/env bash
# Run in MSYS2 CLANG64 from the repository root.
set -euo pipefail
cd -- "${BASH_SOURCE[0]%/*}/.."
source ./msys2-env.sh
mkdir -p out/tests
clang++ -O2 -std=c++20 tools/motion_blur_test.cpp -o out/tests/motion_blur_test.exe
./out/tests/motion_blur_test.exe
clang -O2 tools/motion_blur_clock_test.c -o out/tests/motion_blur_clock_test.exe
./out/tests/motion_blur_clock_test.exe
clang++ -include cstdlib -O2 -std=c++20 -static tools/motion_blur_launcher_test.cpp launcher/prepare.cpp launcher/gpu_check.cpp launcher/datx.cpp launcher/unlock.cpp launcher/graphics.cpp launcher/carswitch.cpp launcher/updater.cpp -o out/tests/motion_blur_launcher_test.exe -lgdiplus -lgdi32 -luser32 -lshell32 -lole32 -ldwmapi -lwinhttp -lbcrypt -lcomctl32
./out/tests/motion_blur_launcher_test.exe
